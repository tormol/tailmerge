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
    // inputs
    int files;
    int per_file_buffer_sz;
    const char* const* filenames;

    struct io_uring_params params;
    int ring_fd;
    int to_submit;
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

    // fields for the program logic not solely tied to using the rings
    char* registered_buffer;
    int open_files; // added files - completely read files
};

void create_ring(struct uring_reader* r) {
    // create inactive ring
    struct io_uring_params setup_params = {0};
    setup_params.sq_entries = setup_params.cq_entries = r->files;
    //setup_params.flags |= IORING_SETUP_IOPOLL; // busy-wait, requires O_DIRECT
    setup_params.flags |= IORING_SETUP_CQSIZE; // use .cq_entries instead of the separate argument
    setup_params.flags |= IORING_SETUP_R_DISABLED; // I want to limit to open, read and write
    #ifdef IORING_SETUP_SUBMIT_ALL // quite recent (5.18) and not required
        setup_params.flags |= IORING_SETUP_SUBMIT_ALL; // don't skip remaining of one fails
    #endif
    #ifdef IORING_SETUP_COOP_TASKRUN // I don't have 5.19 yet, and not required
        setup_params.flags |= IORING_SETUP_COOP_TASKRUN; // don't signal on completion
    #endif
    int ring_fd = checkerr(io_uring_setup(r->files, &setup_params), "create ring");
    printf(
            "Got uring with %d sqes and %d cqes (wanted %d).\n",
            setup_params.sq_entries, setup_params.cq_entries, r->files
    );

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

    r->params = setup_params;
    r->ring_fd = ring_fd;
    r->sq_ptr = sq_ptr;
    r->cq_ptr = cq_ptr;
    r->sqes = sqes;
    /* Save useful fields for later easy reference */
    r->sring_tail = sq_ptr + setup_params.sq_off.tail;
    r->sring_mask = sq_ptr + setup_params.sq_off.ring_mask;
    r->sring_array = sq_ptr + setup_params.sq_off.array;
    r->cring_head = cq_ptr + setup_params.cq_off.head;
    r->cring_tail = cq_ptr + setup_params.cq_off.tail;
    r->cring_mask = cq_ptr + setup_params.cq_off.ring_mask;
    r->cqes = cq_ptr + setup_params.cq_off.cqes;
}

void register_to_ring(struct uring_reader* r) {
    // restrict to open and read
    struct io_uring_restriction restrictions[5] = {{0}};
    restrictions[0].opcode = IORING_RESTRICTION_SQE_FLAGS_ALLOWED;
    restrictions[0].sqe_op = IOSQE_IO_LINK;
    restrictions[1].opcode = IORING_RESTRICTION_SQE_FLAGS_ALLOWED;
    restrictions[1].sqe_op = IOSQE_CQE_SKIP_SUCCESS;
    restrictions[2].opcode = IORING_RESTRICTION_SQE_FLAGS_ALLOWED;
    restrictions[2].sqe_op = IOSQE_FIXED_FILE;
    restrictions[3].opcode = IORING_RESTRICTION_SQE_OP;
    restrictions[3].sqe_op = IORING_OP_OPENAT;
    restrictions[4].opcode = IORING_RESTRICTION_SQE_OP;
    restrictions[4].sqe_op = IORING_OP_READ_FIXED;
    // checkerr(
    //         io_uring_register(r->ring_fd, IORING_REGISTER_RESTRICTIONS, &restrictions, 5),
    //         "restrict IO operations"
    // );

    // use registered file descriptors
    int fds[r->files];
    for (int i=0; i<r->files; i++) {
        fds[i] = -1; // sparse
    }
    checkerr(
            io_uring_register(r->ring_fd, IORING_REGISTER_FILES, &fds, r->files),
            "register %d fds", r->files
    );

    // use one registered buffer for all files
    unsigned int buffers_sz = r->files * r->per_file_buffer_sz;
    char* buffers = mmap((void*)0, buffers_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_UNINITIALIZED, -1, 0);
    if (buffers == MAP_FAILED)
    {
        checkerr(-1, "mmap()ing %dKiB of buffers", buffers_sz/1024);
    }
    struct iovec buffer_vec = {.iov_base = buffers, .iov_len = buffers_sz};
    checkerr(
            io_uring_register(r->ring_fd, IORING_REGISTER_BUFFERS, &buffer_vec, 1),
            "registor an already allocated buffer of %dKIB", buffers_sz/1024
    );
    r->registered_buffer = buffers;

    // finally, enable the ring
    checkerr(io_uring_register(r->ring_fd, IORING_REGISTER_ENABLE_RINGS, NULL, 0), "enable the ring");
}

void open_and_read(struct uring_reader* r, int file, unsigned int* local_tail) {
    // The number of operations an io_uring can have in progress is
    // not limited to the submission queue size.
    // Using registered files descriptors means we don't need to wait for the open
    // to complete to know the file descriptor to use for reads.
    // Therefore the first read can be submitted at the same time as the open.
    // When opening to a registererd fd succeeds, the completion event
    // provides no additional information. Therefore we can use SKIP_SUCCESS,
    // which means there will never be more than one completion per files.
    // This means a completion queue of size files is enough.

    /* Add our submission queue entry to the tail of the SQE ring buffer */
    unsigned int index = *local_tail & *r->sring_mask;
    struct io_uring_sqe *sqe = &r->sqes[index];
    sqe->opcode = IORING_OP_OPENAT;
    sqe->fd = AT_FDCWD;
    sqe->addr = (size_t)r->filenames[file];
    sqe->off = S_IRUSR; // mode_t, doesn't matter siince we're opening readonly
    sqe->open_flags = O_RDONLY; //| O_DIRECT;
    sqe->user_data = !file; // negative to distinguish from reads
    sqe->file_index = file+1;
    // IOSQE_FIXED_FILE is not supported, and only matters for ->fd which I don't want anyway
    sqe->flags = IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS;
    r->sring_array[index] = index;
    *local_tail = *local_tail+1;
    r->open_files++;

    index = *local_tail & *r->sring_mask;
    sqe = &r->sqes[index];
    sqe->opcode = IORING_OP_READ_FIXED;
    sqe->fd = file;
    sqe->flags = IOSQE_FIXED_FILE;
    sqe->addr = (size_t)&r->registered_buffer[file*r->per_file_buffer_sz];
    sqe->len = r->per_file_buffer_sz;
    sqe->off = 0;
    sqe->buf_index = 0;
    sqe->user_data = file;
    r->sring_array[index] = index;
    *local_tail = *local_tail+1;
}

/// Create uring and submit open & initial read for all files.
/// r is filled in by this function and can be passed in uninitialized.
void uring_open_files
(struct uring_reader* r, int files, const char* const* filenames, int per_file_buffer_sz) {
    memset(r, 0, sizeof(struct uring_reader));
    r->files = files;
    r->filenames = filenames;
    r->per_file_buffer_sz = per_file_buffer_sz;

    create_ring(r);
    register_to_ring(r);
    r->filenames = filenames;

    unsigned int tail = *r->sring_tail;
    for (int i = 0; i < r->files/2; i++) {
        open_and_read(r, i, &tail);
    }
    /* Update the tail */
    io_uring_smp_store_release(r->sring_tail, tail);
    int to_consume = (r->files/2)*2;
    do
    {
        int consumed_now = checkerr(
                io_uring_enter(r->ring_fd, to_consume, 1, 0),
                "io_uring_enter()"
        );
        printf("io_uring_enter() consumed %d of %d OPENAT+READ_FIXED sqes\n", consumed_now, to_consume);
        to_consume -= consumed_now;
    } while (to_consume > 0);

    /* Add our submission queue entry to the tail of the SQE ring buffer */
    tail = *r->sring_tail;
    for (int i = r->files/2; i < r->files; i++) {
        open_and_read(r, i, &tail);
    }
    /* Update the tail */
    io_uring_smp_store_release(r->sring_tail, tail);
    r->to_submit = (r->files - r->files/2)*2;
}

void uring_read(struct uring_reader* r) {
    /*
    * Tell the kernel we have submitted events with the io_uring_enter()
    * system call. We also pass in the IOURING_ENTER_GETEVENTS flag which
    * causes the io_uring_enter() call to wait until min_complete
    * (the 3rd param) events complete.
    * */
    int consumed_now = checkerr(
            io_uring_enter(r->ring_fd, r->to_submit, 1, IORING_ENTER_GETEVENTS),
            "io_uring_enter()"
    );
    printf("io_uring_enter() consumed %d of %d sqes\n", consumed_now, r->to_submit);
    r->to_submit -= consumed_now;

    /* Read barrier */
    unsigned int chead = io_uring_smp_load_acquire(r->cring_head);
    unsigned int stail = *r->sring_tail;
    /* Remember, this is a ring buffer. If head == tail, it means that the  buffer is empty. */
    while (chead != *r->cring_tail) {
        /* Get the entry */
        struct io_uring_cqe *cqe = &r->cqes[chead & (*r->cring_mask)];
        if ((int)cqe->user_data < 0) {
            checkerr_sys(cqe->res, "open %s through uring", r->filenames[!cqe->user_data]);
        } else {
            checkerr_sys(
                    cqe->res,
                    "read up to %d bytes from %s through uring",
                    r->per_file_buffer_sz,
                    r->filenames[cqe->user_data]
            );
        }
        chead++;

        // we only get successful completion events from reads
        int file = cqe->user_data;
        int bytes_read = cqe->res;
        const char* filename = r->filenames[file];
        if (bytes_read == 0) {
            printf("%s finished\n", filename);
            r->open_files--;
        }

        char* buffer = &r->registered_buffer[file * r->per_file_buffer_sz];
        char* newline_at = memchr(buffer, '\n', bytes_read);
        if (newline_at == NULL) {
            memcpy(&buffer[16], " ...\0", 5);
        } else {
            *newline_at = '\0';
        }
        printf("%d bytes read from %s; first line: %s\n", cqe->res, filename, buffer);

        // and read more
        unsigned int index = stail & *r->sring_mask;
        struct io_uring_sqe *sqe = &r->sqes[index];
        sqe->opcode = IORING_OP_READ_FIXED;
        sqe->fd = file;
        sqe->flags = IOSQE_FIXED_FILE;
        sqe->addr = (size_t)&r->registered_buffer[file*r->per_file_buffer_sz];
        sqe->len = r->per_file_buffer_sz;
        sqe->off = 0;
        sqe->buf_index = 0;
        sqe->user_data = file;
        r->sring_array[index] = index;
        r->to_submit++;
        stail++;
    }
    /* Write barrier so that update to the head are made visible */
    io_uring_smp_store_release(r->cring_head, chead);
    /* Update the tail */
    io_uring_smp_store_release(r->sring_tail, stail);
}

int main(int argc, const char* const* argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s file...\n", argv[0]);
        return 1;
    }

    struct uring_reader r;
    uring_open_files(&r, argc-1, &argv[1], 16*1024);
    while (r.open_files > 0) {
        uring_read(&r);
    }

    close(r.ring_fd);
    return 0;
}
