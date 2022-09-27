#ifndef _URING_READER_H_
#define _URING_READER_H_

#ifdef __linux__
#include <linux/io_uring.h>
#include <stdatomic.h>
#endif
#include <sys/uio.h> // struct iovec
#include <sys/types.h> // off_t

/// A struct for reading multiple files at the same time.
/// Will also support having one writer.
///
/// Allocates and manages buffers, and automatically queues reads when buffer become unused.
/// Does not deal with splitting into lines or preserving incomplete lines between reads.
/// (It wants to keep the read pointers aligned.)
struct uring_reader {
    // inputs
    int files;
    int per_file_buffer_sz;
    const char* const* filenames;

#ifdef __linux__
    // ring info && bookkeeping
    struct io_uring_params params;
    int ring_fd;
    int to_submit;
    void* sq_ptr;
    void* cq_ptr;
    struct io_uring_sqe* sqes;
    atomic_uint *sring_tail;
    unsigned int *sring_mask;
    unsigned int* sring_array;
    atomic_uint *cring_head;
    atomic_uint *cring_tail;
    unsigned int *ring_mask;
    struct io_uring_cqe* cqes;
#endif // defined(__linux__)

    // fields for the program logic not solely tied to using the rings
    char* registered_buffer;
    off_t* bytes_read;
    int* lines_read;
    int* incomplete_line_length;
    int open_files; // added files - completely read files
    /// since there is only one buffer per file, and our api is pull-based,
    /// we need to copy unfinished line to start of buffer when the function is called the next time
    /// -1 or file index for which buffer[copy_from..copy_from+line_length] should be copied to 0...
    int needs_copy;
    int copy_from;
};

/// Initialize struct, create uring and allocate memory
/// r is filled in by this function and can be passed in uninitialized.
///
/// To enable allocating all memory needed by the program in one go,
/// this function can allocate more than it itself needs and return that to the caller:
/// extra_buffer_sz is how many extra bytes to allocate & register with the kernel
/// alloc_extra_other is how many bytes (in addition to extra_buffer_sz) to allocate but not register.
/// Returns a pointer to the start of the extra buffer. The unregistered memory follows directly after it.
void* uring_create(struct uring_reader* r,
        int files, int per_file_buffer_sz,
        int extra_buffer_sz, int alloc_extra_other
);

/// Close the uring and all opened files, and free memory.
void uring_destroy(struct uring_reader *r);

/// Complete setup and submit open & initial read for all files.
void uring_open_files(struct uring_reader* r, const char* const* filenames, int out_fd);

/// Get the next finished read for a file, but don't wait for it to finish.
/// Returns {.iov_base=NULL, iov_len=0} if not yet available.
/// Returns {.iov_base=NULL, iov_len=-1} if end of file.
///
/// If successful, the returned buffer will be marked as "loaned out":
/// When finished with the read, uring_return_loan() or uring_write_and_return_loan() should be called.
/// (But they don't need to be called straight away.)
///
/// Intended for "fast forwarding" to a start key, where we want to go through the available bytes.
struct iovec uring_try_get_next_read(int file_index);

/// Signal that you're done with the read, and that the buffer can be used to read further.
///
/// Intended for when all lines in a read should be "fast-forwarded" past.
void uring_return_loan(char* buffer);

/// Set up a linked write-then-read.
/// When a write later completes, bytes_written will be incremented with how many was written.
/// If buffer is NULL, just a simle write is queued.
///
/// Intended for when all the remaining lines in a read are ready to be written,
/// or if the writer is out of iovecs.
void uring_write_and_return_loan(struct iovec* registered_slices, int slices,
        off_t *bytes_written,
        char* buffer
);

/// Wait until a previously queued write comletes.
/// If now is false, it will wait for this the next time it needs to wait for a read.
///
/// Intended for when the writer is out of iovecs.
void uring_wait_for_write(bool now);

/// Like uring_try_get_next_read(), but blocks until a read is available.
///
/// Intended to be called directly after uring_write_and_return_loan().
struct iovec uring_get_next_read(int file);

/// Read no further into this file, and redistribute its buffers to to other files.
///
/// Intended for when the "end point" has been passed in this file.
void uring_close_file(int file);

#endif // !defined(_URING_READER_H_)
