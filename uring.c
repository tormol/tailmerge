/* This file is part of tailmerge.
 * Copyright (C) 2022 Torbjørn Birch Moltu
 *
 * Licenced under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * tailmerge is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with tailmerge. If not, see <https://www.gnu.org/licenses/>.
 */

#define _POSIX_C_SOURCE 200809L // yes I'm new
#define _DEFAULT_SOURCE // for syscall()
#define _GNU_SOURCE // for more O_ flags
#include <errno.h>
#include <string.h> // strlen() and strerror()
#include <stdio.h> // fprintf() and stderr
#include <stdlib.h> // exit()
#include <stdarg.h> // used by checkerr()
//#include <stdbool.h>
#include <unistd.h> // syscall()
#include <sys/syscall.h> // syscall numbers
#include <sys/uio.h> // struct iovec
#include <linux/io_uring.h>
#include <sys/mman.h> // mmap()
#include <asm-generic/mman.h> // MAP_UNINITIALIZED
#include <sys/types.h> // more O_ flags
#include <sys/stat.h>
#include <fcntl.h> // open()
#include <stdatomic.h>
#include <assert.h>

/* helper functions */

// print error messages and exit if `ret` is negative,
// otherwise pass it through to caller.
int checkerr(int ret, const char *desc, ...) {
    if (ret >= 0) {
        return ret;
    }
    int err = errno;
    fprintf(stderr, "Failed to ");
    va_list args;
    va_start(args, desc);
    vfprintf(stderr, desc, args);
    va_end(args);
    fprintf(stderr, ": %s\n", strerror(err));
    exit(1);
}

int checkerr_sys(int ret, const char *desc, ...) {
    if (ret >= 0) {
        return ret;
    }
    int err = -ret;
    fprintf(stderr, "Failed to ");
    va_list args;
    va_start(args, desc);
    vfprintf(stderr, desc, args);
    va_end(args);
    fprintf(stderr, ": %s\n", strerror(err));
    exit(1);
}

/* System call wrappers provided since glibc does not yet
 * provide wrappers for io_uring system calls.
 */
int io_uring_setup(unsigned entries, struct io_uring_params *p) {
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

int io_uring_register(int ring_fd, unsigned int opcode, void* arg, unsigned int nr_args) {
    return (int)syscall(__NR_io_uring_register, ring_fd, opcode, arg, nr_args);
}

int io_uring_enter(int ring_fd, unsigned int to_submit, unsigned int min_complete, unsigned int flags) {
    return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, NULL, 0);
}

/* Macros for barriers needed by io_uring */
#define io_uring_smp_store_release(p, v) atomic_store_explicit((p), (v), memory_order_release)
#define io_uring_smp_load_acquire(p) atomic_load_explicit((p), memory_order_acquire)

struct uring_reader {
    int files;
    int per_file_buffer_sz;
    struct io_uring_params params;
    int ring_fd;
    void* sq_ptr;
    void* cq_ptr;
    struct io_uring_sqe* sqes;
    atomic_uint* sring_tail;
    unsigned int* sring_mask;
    unsigned int* sring_array;
    atomic_uint* cring_head;
    atomic_uint* cring_tail;
    unsigned int* cring_mask;
    struct io_uring_cqe* cqes;
    char* registered_buffer;
    int opening_files; // added files - completed opens
};

struct uring_reader setup_ring(int files, int per_file_buffer_sz) {
    // create inactive ring
    struct io_uring_params setup_params = {0};
    setup_params.sq_entries = setup_params.cq_entries = files;
    //setup_params.flags |= IORING_SETUP_IOPOLL; // busy-wait, requires O_DIRECT
    setup_params.flags |= IORING_SETUP_CQSIZE; // use .cq_entries instead of the separate argument
    setup_params.flags |= IORING_SETUP_R_DISABLED; // I want to limit to open, read and write
    #ifdef IORING_SETUP_SUBMIT_ALL // quite recent (5.18) and not required
        setup_params.flags |= IORING_SETUP_SUBMIT_ALL; // don't skip remaining of one fails
    #endif
    #ifdef IORING_SETUP_COOP_TASKRUN // I don't have 5.19 yet, and not required
        setup_params.flags |= IORING_SETUP_COOP_TASKRUN; // don't signal on completion
    #endif
    int ring_fd = checkerr(io_uring_setup(files, &setup_params), "create ring");

    // create rings (copied from example in man io_uring)
    int sring_sz = setup_params.sq_off.array + setup_params.sq_entries * sizeof(unsigned);
    int cring_sz = setup_params.cq_off.cqes + setup_params.cq_entries * sizeof(struct io_uring_cqe);

    /* Rather than check for kernel version, the recommended way is to
     * check the features field of the io_uring_params structure, which is a 
     * bitmask. If IORING_FEAT_SINGLE_MMAP is set, we can do away with the
     * second mmap() call to map in the completion ring separately.
     */
    if ((setup_params.features & IORING_FEAT_SINGLE_MMAP) != 0) {
        sring_sz = cring_sz > sring_sz ? cring_sz : sring_sz;
        cring_sz = sring_sz > cring_sz ? sring_sz : cring_sz;
    }

    /* Map in the submission and completion queue ring buffers.
     *  Kernels < 5.4 only map in the submission queue, though.
     */
    void *sq_ptr = mmap((void*)0, sring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring_fd, IORING_OFF_SQ_RING);
    if (sq_ptr == MAP_FAILED) {
        checkerr(-1, "mmap()ing submission queue of %d bytes", sring_sz);
    }

    void *cq_ptr = sq_ptr;
    if ((setup_params.features & IORING_FEAT_SINGLE_MMAP) == 0) {
        /* Map in the completion queue ring buffer in older kernels separately */
        cq_ptr = mmap((void*)0, cring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring_fd, IORING_OFF_CQ_RING);
        if (cq_ptr == MAP_FAILED) {
            checkerr(-1, "mmap()ing completion queue of %d bytes", cring_sz);
        }
    }

    /* Map in the submission queue entries array */
    int sqes_sz = setup_params.sq_entries * sizeof(struct io_uring_sqe);
    struct io_uring_sqe* sqes = mmap(0, sqes_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring_fd, IORING_OFF_SQES);
    if (sqes == MAP_FAILED) {
        checkerr(-1, "mmap()ing submission queue entries array of %d bytes", sqes_sz);
    }
    // end of mostly copied code

    // restrict to open and read
    struct io_uring_restriction restrictions[2] = {{0}};
    restrictions[0].opcode = IORING_RESTRICTION_SQE_OP;
    restrictions[0].sqe_op = IORING_OP_OPENAT;
    restrictions[1].opcode = IORING_RESTRICTION_SQE_OP;
    restrictions[1].sqe_op = IORING_OP_READ_FIXED;
    checkerr(io_uring_register(ring_fd, IORING_REGISTER_RESTRICTIONS, &restrictions, 2), "restrict IO operations");

    // use registered file descriptors
    int fds[files];
    for (int i=0; i<files; i++) {
        fds[i] = -1; // sparse
    }
    checkerr(io_uring_register(ring_fd, IORING_REGISTER_FILES, &fds, files), "register %d fds", files);

    // use one registered buffer for all files
    unsigned int buffers_sz = files * per_file_buffer_sz;
    char* buffers = mmap((void*)0, buffers_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_UNINITIALIZED, -1, 0);
    if (buffers == MAP_FAILED)
    {
        checkerr(-1, "mmap()ing %dKiB of buffers", buffers_sz/1024);
    }
    struct iovec buffer_vec = {.iov_base = buffers, .iov_len = buffers_sz};
    checkerr(io_uring_register(ring_fd, IORING_REGISTER_BUFFERS, &buffer_vec, 1),
             "registor an already allocated buffer of %dKIB", buffers_sz/1024);

    // finally, enable the ring
    checkerr(io_uring_register(ring_fd, IORING_REGISTER_ENABLE_RINGS, NULL, 0), "enable the ring");

    struct uring_reader r = {
        .files = files,
        .per_file_buffer_sz = per_file_buffer_sz,
        .params = setup_params,
        .ring_fd = ring_fd,
        .sq_ptr = sq_ptr,
        .cq_ptr = cq_ptr,
        .sqes = sqes,
        /* Save useful fields for later easy reference */
        .sring_tail = sq_ptr + setup_params.sq_off.tail,
        .sring_mask = sq_ptr + setup_params.sq_off.ring_mask,
        .sring_array = sq_ptr + setup_params.sq_off.array,
        .cring_head = cq_ptr + setup_params.cq_off.head,
        .cring_tail = cq_ptr + setup_params.cq_off.tail,
        .cring_mask = cq_ptr + setup_params.cq_off.ring_mask,
        .cqes = cq_ptr + setup_params.cq_off.cqes,
        // fields for the program logic not solely tied to using the rings
        .registered_buffer = buffers,
        .opening_files = 0
    };
    return r;
}

void add_file_to_open(struct uring_reader* r, const char* file) {
    /* Add our submission queue entry to the tail of the SQE ring buffer */
    unsigned int tail = *r->sring_tail;
    unsigned int index = tail & *r->sring_mask;
    assert((int)index == r->opening_files);
    struct io_uring_sqe *sqe = &r->sqes[r->opening_files];
    /* Fill in the parameters required for the read or write operation */
    sqe->opcode = IORING_OP_OPENAT;
    sqe->fd = AT_FDCWD;
    sqe->addr = (size_t)file;
    sqe->off = S_IRUSR; // mode_t, doesn't matter siince we're opening readonly
    sqe->open_flags = O_RDONLY ; //| O_DIRECT;
    sqe->user_data = r->opening_files;
    sqe->file_index = r->opening_files+1;
    sqe->flags = 0; // IOSQE_FIXED_FILE; // not supported and only for ->fd which I don't want anyway

    r->sring_array[r->opening_files] = r->opening_files;
    tail++;
    /* Update the tail */
    io_uring_smp_store_release(r->sring_tail, tail);

    r->opening_files++;
}

void open_and_read_all(struct uring_reader* r) {
    // not sure if consumed mean submissions not rejected, or submissions accepted now, or submission completed.
    // implementing the first
    int to_consume = r->opening_files;
    while (r->opening_files > 0)
    {
        /*
        * Tell the kernel we have submitted events with the io_uring_enter()
        * system call. We also pass in the IOURING_ENTER_GETEVENTS flag which
        * causes the io_uring_enter() call to wait until min_complete
        * (the 3rd param) events complete.
        * */
        int consumed_now = checkerr(
                io_uring_enter(r->ring_fd, to_consume, r->opening_files, IORING_ENTER_GETEVENTS),
                "io_uring_enter()"
        );
        printf("io_uring_enter() consumed %d of %d OPENAT sqes\n", consumed_now, to_consume);
        to_consume -= consumed_now;

        /* Read barrier */
        unsigned int head = io_uring_smp_load_acquire(r->cring_head);
        /*
        * Remember, this is a ring buffer. If head == tail, it means that the
        * buffer is empty.
        * */
        while (head != *r->cring_tail) {
            /* Get the entry */
            struct io_uring_cqe *cqe = &r->cqes[head & (*r->cring_mask)];
            struct io_uring_sqe *sqe = &r->sqes[cqe->user_data];

            checkerr_sys(cqe->res, "open %s through uring", (const char*)sqe->addr);
            printf("%s opened (internal offset %d)\n", (const char*)sqe->addr, sqe->file_index-1);

            r->opening_files--;
            head++;
        }

        /* Write barrier so that update to the head are made visible */
        io_uring_smp_store_release(r->cring_head, head);
    }
}

int main(int argc, const char* const* argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s file...\n", argv[0]);
        return 1;
    }

    struct uring_reader r = setup_ring(argc-1, 16*1024);
    for (int i=1; i<argc; i++) {
        add_file_to_open(&r, argv[i]);
    }
    open_and_read_all(&r);

    close(r.ring_fd);
    return 0;
}
