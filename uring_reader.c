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

#define _DEFAULT_SOURCE // for syscall()
#define _GNU_SOURCE // for more O_ flags
#include "uring_reader.h"
#include "utils.h"

#include <unistd.h> // syscall()
#include <sys/syscall.h> // syscall numbers
#include <sys/mman.h> // mmap()
#include <sys/fcntl.h> // O_ flags
#include <sys/types.h> // more O_ flags
#include <sys/stat.h>

#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h> // memset()
#include <assert.h>
#include <sysexits.h>

/* System call wrappers provided since glibc does not yet
 * provide wrappers for io_uring system calls.
 */
static int io_uring_setup(unsigned entries, struct io_uring_params *p) {
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

static int io_uring_register(int ring_fd, unsigned int opcode, void* arg, unsigned int nr_args) {
    return (int)syscall(__NR_io_uring_register, ring_fd, opcode, arg, nr_args);
}

static int io_uring_enter(int ring_fd, unsigned int to_submit, unsigned int min_complete, unsigned int flags) {
    return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, NULL, 0);
}

/* Macros for barriers needed by io_uring */
#define io_uring_smp_store_release(p, v) atomic_store_explicit((p), (v), memory_order_release)
#define io_uring_smp_load_acquire(p) atomic_load_explicit((p), memory_order_acquire)

static bool create_ring(struct uring_reader* r) {
    // create inactive ring
    struct io_uring_params setup_params = {0};
    int capacity = r->files;
    // need one extra to fit 2*the bigger half when odd
    if ((capacity & 1) != 0) {
        capacity++;
    }
    setup_params.sq_entries = setup_params.cq_entries = capacity;
    //setup_params.flags |= IORING_SETUP_IOPOLL; // busy-wait, requires O_DIRECT
    setup_params.flags |= IORING_SETUP_CQSIZE; // use .cq_entries instead of the separate argument
    setup_params.flags |= IORING_SETUP_R_DISABLED; // I want to limit to open, read and write
#ifdef IORING_SETUP_SUBMIT_ALL // quite recent (5.18) and not required
    setup_params.flags |= IORING_SETUP_SUBMIT_ALL; // don't skip remaining of one fails
#endif
#ifdef IORING_SETUP_COOP_TASKRUN // I don't have 5.19 yet, and not required
    setup_params.flags |= IORING_SETUP_COOP_TASKRUN; // don't signal on completion
#endif
    int ring_fd = io_uring_setup(capacity, &setup_params);
    if (ring_fd < 0 && errno == ENOSYS)
    {
        fprintf(stderr, "io_uring is not available, falling back to blocking IO.\n");
        return false;
    }
    checkerr(ring_fd, EX_OSERR, "create ring");
    fprintf(stderr,
            "Got uring with %d sqes and %d cqes (wanted %d).\n",
            setup_params.sq_entries, setup_params.cq_entries, capacity
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
    void *sq_ptr = mmap((void*)0, sring_sz, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_POPULATE,
            ring_fd, IORING_OFF_SQ_RING
    );
    if (sq_ptr == MAP_FAILED) {
        checkerr(-1, EX_UNAVAILABLE, "mmap()ing submission queue of %d bytes", sring_sz);
    }

    void *cq_ptr = sq_ptr;
    if ((setup_params.features & IORING_FEAT_SINGLE_MMAP) == 0) {
        /* Map in the completion queue ring buffer in older kernels separately */
        cq_ptr = mmap((void*)0, cring_sz, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_POPULATE,
                ring_fd, IORING_OFF_CQ_RING
        );
        if (cq_ptr == MAP_FAILED) {
            checkerr(-1, EX_UNAVAILABLE,
                    "mmap()ing completion queue of %d bytes", cring_sz
            );
        }
    }

    /* Map in the submission queue entries array */
    int sqes_sz = setup_params.sq_entries * sizeof(struct io_uring_sqe);
    struct io_uring_sqe* sqes = mmap(0, sqes_sz, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_POPULATE,
            ring_fd, IORING_OFF_SQES
    );
    if (sqes == MAP_FAILED) {
        checkerr(-1, EX_UNAVAILABLE,
                "mmap()ing submission queue entries array of %d bytes", sqes_sz
        );
    }
    // end of mostly copied code

    r->params = setup_params;
    r->ring_fd = ring_fd;
    r->sq_ptr = sq_ptr;
    r->cq_ptr = cq_ptr;
    r->sqes = sqes;
    /* Save useful fields for later easy reference */
    r->sring_array = sq_ptr + setup_params.sq_off.array;
    r->sring_tail = sq_ptr + setup_params.sq_off.tail;
    r->sring_mask = sq_ptr + setup_params.sq_off.ring_mask;
    r->cqes = cq_ptr + setup_params.cq_off.cqes;
    r->cring_head = cq_ptr + setup_params.cq_off.head;
    r->cring_tail = cq_ptr + setup_params.cq_off.tail;
    r->cring_mask = cq_ptr + setup_params.cq_off.ring_mask;

    return true;
}

static void register_to_ring(struct uring_reader* r, size_t register_bytes) {
    // restrict to open and read
    struct io_uring_restriction restrictions[3] = {{0}};
    restrictions[0].opcode = IORING_RESTRICTION_SQE_FLAGS_ALLOWED;
    restrictions[0].sqe_op = IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS | IOSQE_FIXED_FILE;
    restrictions[1].opcode = IORING_RESTRICTION_SQE_OP;
    restrictions[1].sqe_op = IORING_OP_OPENAT;
    restrictions[2].opcode = IORING_RESTRICTION_SQE_OP;
    restrictions[2].sqe_op = IORING_OP_READ_FIXED;
    checkerr(io_uring_register(r->ring_fd, IORING_REGISTER_RESTRICTIONS, &restrictions, 3),
            EX_SOFTWARE, "restrict IO operations"
    );

    // use registered file descriptors
    int* fds = (int*)r->cqes; // borrow it; it will be overwritten when used
    memset(fds, -1/*sparse*/, r->files*sizeof(int));
    checkerr(io_uring_register(r->ring_fd, IORING_REGISTER_FILES, fds, r->files),
            EX_UNAVAILABLE,
            "register %d fds", r->files
    );

    // use one registered buffer for all files
    // TODO use buffer un-registered if it fails.
    struct iovec buffer_vec = {.iov_base = r->registered_buffer, .iov_len = register_bytes};
    checkerr(io_uring_register(r->ring_fd, IORING_REGISTER_BUFFERS, &buffer_vec, 1),
            EX_SOFTWARE,
            "registor an already allocated buffer of %dKIB", register_bytes/1024
    );

    // finally, enable the ring
    checkerr(io_uring_register(r->ring_fd, IORING_REGISTER_ENABLE_RINGS, NULL, 0),
            EX_OSERR, "enable the ring"
    );
}

void* uring_create(struct uring_reader* r,
        unsigned int files, unsigned int per_file_buffer_sz,
        size_t extra_buffer_sz, size_t alloc_extra_other
) {
    memset(r, 0, sizeof(struct uring_reader));
    r->files = files;
    r->per_file_buffer_sz = per_file_buffer_sz;

    // allocate the buffers for all files at once
    size_t buffers_sz = r->files * r->per_file_buffer_sz;
#ifdef __linux__
    // enable one read to always be in prograss
    buffers_sz *= 2;
#endif
    buffers_sz += extra_buffer_sz;
    // also allocate the list of bytes read and line numbers at the end of it
    size_t alloc_sz = buffers_sz + alloc_extra_other;
#ifdef __linux__
    alloc_sz += r->files * sizeof(off_t);
#endif
    alloc_sz += r->files * sizeof(int);
    char* alloc = mmap((void*)0, alloc_sz, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_ANONYMOUS,
            -1/*fd*/, 0/*offset in file*/
    );
    if (alloc == MAP_FAILED)
    {
        checkerr(-1, EX_UNAVAILABLE, "mmap()ing %dKiB of buffers", (int)(alloc_sz/1024));
    }

    r->registered_buffer = alloc;
    void *alloc_ret = &alloc[buffers_sz-extra_buffer_sz];
    alloc += buffers_sz + alloc_extra_other;
#ifdef __linux__
    r->bytes_read = (off_t*)alloc;
    // allocated memory is zeroed - don't need to zero again
    alloc += r->files * sizeof(off_t);

    if (create_ring(r)) {
        register_to_ring(r, buffers_sz);
    } else {
        r->ring_fd = -1;
        // two buffers already allocated, so might as well use it.
        r->per_file_buffer_sz *= 2;
    }
#endif

    r->buffer_sizes = (int*)alloc;
    for (int i = 0; i < r->files; i++) {
        r->buffer_sizes[i] = r->per_file_buffer_sz;
    }

    return alloc_ret;
}

void uring_destroy(struct uring_reader *r) {
#ifdef __linux__
    checkerr(close(r->ring_fd), EX_SOFTWARE, "closing uring");
#endif
    void* alloc_end = &r->buffer_sizes[r->files];
    void* alloc_start = r->registered_buffer;
    checkerr(munmap(alloc_start, alloc_end-alloc_start), EX_SOFTWARE, "freeing memory");
}


#ifdef __linux__
union uring_data {
    uint64_t raw;
    struct {
        uint32_t file;
        enum {
            OPEN_FILE,
            READ_TO_BUFFER_A,
            READ_TO_BUFFER_B
        } operation;
    } info;
};
static_assert(sizeof(union uring_data) == sizeof(uint64_t), "uring_data.info is too big");

static void open_and_read(struct uring_reader* r, int file, unsigned int* local_tail) {
    // The number of operations an io_uring can have in progress is
    // not limited to the submission queue size.
    // Using registered file descriptors means we don't need to wait for the open
    // to complete to know the file descriptor to use for reads.
    // Therefore the first read can be submitted at the same time as the open.
    // When opening to a registererd fd succeeds, the completion event
    // provides no additional information. Therefore we can use SKIP_SUCCESS,
    // which means there will never be more than one completion per file.
    // This means a completion queue of size files is enough.

    union uring_data user_data;
    user_data.info.file = file;

    /* Add our submission queue entry to the tail of the SQE ring buffer */
    unsigned int index = *local_tail & *r->sring_mask;
    struct io_uring_sqe *sqe = &r->sqes[index];
    sqe->opcode = IORING_OP_OPENAT;
    sqe->fd = AT_FDCWD;
    sqe->addr = (size_t)r->filenames[file];
    sqe->off = S_IRUSR; // mode_t, doesn't matter siince we're opening readonly
    sqe->open_flags = O_RDONLY; //| O_DIRECT;
    user_data.info.operation = OPEN_FILE;
    sqe->user_data = user_data.raw;
    sqe->file_index = file+1;
    // IOSQE_FIXED_FILE is not supported, and only matters for ->fd which I don't want it anyway
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
    user_data.info.operation = READ_TO_BUFFER_A;
    sqe->user_data = user_data.raw;
    r->sring_array[index] = index;
    *local_tail = *local_tail+1;

    r->to_submit += 2;
}

static void uring_submit(struct uring_reader* r, unsigned int wait_for) {
    int flags = 0;
    if (wait_for != 0) {
        flags = IORING_ENTER_GETEVENTS;
    }

    // submit untill all have been received
    while (r->to_submit > 0)
    {
        int consumed_now = checkerr(io_uring_enter(r->ring_fd, r->to_submit, wait_for, flags),
                EX_SOFTWARE,
                "io_uring_enter()"
        );
        if (consumed_now != r->to_submit) {
            fprintf(stderr,
                    "uring_submit(%u): Only %d of %d submissions returned.",
                    wait_for, consumed_now, r->to_submit
            );
        }
        r->to_submit -= consumed_now;
    }
}

static struct iovec uring_handle_completion(struct uring_reader *r, int *file) {
    struct iovec read = {.iov_base = NULL, .iov_len = -1};
    unsigned int chead;
start:
    /* Read barrier */
    chead = io_uring_smp_load_acquire(r->cring_head);
    /* Remember, this is a ring buffer. If head == tail, it means that the  buffer is empty. */
    if (chead == *r->cring_tail || r->to_submit == r->files) {
        return read;
    }

    /* Get the entry */
    struct io_uring_cqe *cqe = &r->cqes[chead & (*r->cring_mask)];
    union uring_data user_data;
    user_data.raw = cqe->user_data;
    /* Write barrier so that update to the head are made visible */
    io_uring_smp_store_release(r->cring_head, chead+1);

    if (user_data.info.operation == OPEN_FILE) {
        checkerr_sys(cqe->res,
                EX_NOINPUT,
                "open %s through uring", r->filenames[user_data.info.file]
        );
        // in case link gets broken, assume CQE_SKIP_SUCCESS was ignored
        goto start;
    } else if (cqe->res == -ECANCELED) {
        fprintf(stderr, "A read for %s was canceled.\n", r->filenames[user_data.info.file]);
        goto start;
    }

    read.iov_len = checkerr_sys(cqe->res,
            EX_IOERR,
            "read up to %d bytes from %s through uring",
            r->buffer_sizes[user_data.info.file],
            r->filenames[user_data.info.file]
    );
    r->bytes_read[user_data.info.file] += (off_t)read.iov_len;
    int buffer_index = user_data.info.file;
    if (user_data.info.operation == READ_TO_BUFFER_B) {
        buffer_index += r->files;
    }

    *file = user_data.info.file;
    read.iov_base = &r->registered_buffer[buffer_index * r->per_file_buffer_sz];

    if (read.iov_len == 0) {
        r->open_files--;
        r->bytes_read[*file] = -1;
    } else {
        // prepare another
        int other_buffer_index;
        if (user_data.info.operation == READ_TO_BUFFER_A) {
            user_data.info.operation = READ_TO_BUFFER_B;
            other_buffer_index = user_data.info.file + r->files;
        } else {
            user_data.info.operation = READ_TO_BUFFER_A;
            other_buffer_index = user_data.info.file;
        }
        char* other_buffer = &r->registered_buffer[other_buffer_index * r->per_file_buffer_sz];
        unsigned int stail = *r->sring_tail;
        unsigned int index = stail & *r->sring_mask;
        struct io_uring_sqe *sqe = &r->sqes[index];
        sqe->opcode = IORING_OP_READ_FIXED;
        sqe->fd = *file;
        sqe->flags = IOSQE_FIXED_FILE;
        sqe->addr = (size_t)other_buffer;
        sqe->len = r->buffer_sizes[*file];
        sqe->off = r->bytes_read[*file];
        sqe->buf_index = 0;
        sqe->user_data = user_data.raw;
        r->sring_array[index] = index;
        /* Update the tail */
        io_uring_smp_store_release(r->sring_tail, stail+1);
        r->to_submit++;
    }

    return read;
}
#endif // defined(__linux__)

/// Create uring and submit open & initial read for all files.
/// r is filled in by this function and can be passed in uninitialized.
void uring_open_files(struct uring_reader* r, const char* const* filenames) {
    r->filenames = filenames;
    unsigned int tail = *r->sring_tail;
    for (int i = 0; i < r->files/2; i++) {
        open_and_read(r, i, &tail);
    }
    /* Update the tail */
    io_uring_smp_store_release(r->sring_tail, tail);
    // submit untill all have been received
    uring_submit(r, 0);

    /* Add our submission queue entry to the tail of the SQE ring buffer */
    tail = *r->sring_tail;
    for (int i = r->files/2; i < r->files; i++) {
        open_and_read(r, i, &tail);
    }
    /* Update the tail */
    io_uring_smp_store_release(r->sring_tail, tail);
    uring_submit(r, r->files);
}


struct iovec uring_get_any_unloaned(struct uring_reader *r, int *file) {
    struct iovec read = {.iov_base = NULL, .iov_len = -1};
    if (r->open_files == 0) {
        *file = -1;
        return read;
    }
    while (true) {
        read = uring_handle_completion(r, file);
        if (read.iov_base != NULL) {
            break;
        }
        uring_submit(r, 1);
    }
    return read;
}

void uring_return_loan(char* buffer) {
    // not needed yet
    (void)buffer; // silence unused warning (and unnamed parameter isn't standardized yet.)
}
