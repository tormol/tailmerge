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
#include <errno.h>
#include <string.h> // strlen() and strerror()
#include <stdio.h> // fprintf() and stderr
#include <stdlib.h> // exit()
#include <stdarg.h> // used by checkerr()
#include <unistd.h> // syscall()
#include <sys/syscall.h> // syscall numbers
#include <sys/uio.h> // struct iovec
#include <linux/io_uring.h>
#include <sys/mman.h> // mmap()
#include <asm-generic/mman.h> // MAP_UNINITIALIZED

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


int setup_ring(int files) {
    // create inactive ring
    struct io_uring_params setup_params = {0};
    setup_params.sq_entries = setup_params.cq_entries = files;
    setup_params.flags |= IORING_SETUP_IOPOLL; // busy-wait, requires O_DIRECT
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
        fds[i] = i;
    }
    checkerr(io_uring_register(ring_fd, IORING_REGISTER_FILES, &fds, files), "register %d fd", files);

    // use registered buffer, 16KiB*files
    const unsigned int PER_FILE_BUFFER_SZ = 16*1024;
    unsigned int buffers_sz = files * PER_FILE_BUFFER_SZ;
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

    return ring_fd;
}

int main(int argc, const char* const* argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s file...\n", argv[0]);
        return 1;
    }
    int ring_fd = setup_ring(argc-1);
    close(ring_fd);
    return ring_fd == -1 ? 1 : 0;
}
