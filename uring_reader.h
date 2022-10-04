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

#ifndef _URING_READER_H_
#define _URING_READER_H_

#ifdef __linux__
#include <linux/io_uring.h>
#include <stdatomic.h>
#endif
#include <sys/uio.h> // struct iovec
#include <sys/types.h> // off_t
#include <stdbool.h>

/// A struct for reading multiple files at the same time.
/// Will also support having one writer.
///
/// Allocates and manages buffers, and automatically queues reads when buffer become unused.
/// Does not deal with splitting into lines or preserving incomplete lines between reads.
/// (It wants to keep the read pointers aligned.)
struct uring_reader {
    // inputs:
    /// Number of files to open.
    int files;
    /// How big the buffer passed to each read should be.
    int per_file_buffer_sz;
    const char* const* filenames;

#ifdef __linux__
    /// Ring info && bookkeeping.
    struct io_uring_params params;
    /// The file descriptor used to perform syscalls related to the io_uring.
    int ring_fd;
    /// How many submission entries have been added since io_uring_enter() was last called.
    /// Increased when adding an entry, passed to io_uring_enter()
    /// and decremented with how many entries the io_uring_enter() call processed.
    int to_submit;
    /// The start of the Submission Queue area.
    /// It is mmap()ed from the ring fd.
    void* sq_ptr;
    /// The start of the Completion Queue area.
    /// It is mmap()ed from the ring fd.
    void* cq_ptr;
    /// The start of the Submission Queue Entries area.
    /// It is mmap()ed from the ring fd.
    /// (It is separate from the submission queue because a submission entry is quite big,
    ///  and some programs might reuse many of the fields.)
    struct io_uring_sqe* sqes;

    // Fields that are a function of the abouve pointers and offsets from .params:

    /// The actual ring buffer of submissions.
    /// The values are indexes into .sqes.
    unsigned int* sring_array;
    /// Incremented when a submission entry is added.
    /// It doesn't wrap around (until ~0 is reached.
    atomic_uint *sring_tail;
    /// To get an index into sring_array:
    /// .sring_tail & .sring_mask gets an index into .sring_array
    unsigned int *sring_mask;
    /// The actual ring buffer of completions.
    struct io_uring_cqe* cqes;
    /// Where to start reading completion evets.
    /// (after anding with .cring_mask)
    atomic_uint *cring_head;
    /// Where to stop reading completion evets.
    /// (after anding with .cring_mask)
    atomic_uint *cring_tail;
    unsigned int *cring_mask;

    // Fields for the program logic not solely tied to using the rings:

    /// The uring read operations don't advance the offset for the fd,
    /// so we have to pass it in and increase it when reads complete.
    off_t* bytes_read;
#endif // defined(__linux__)
    /// The start of the registered buffer,
    /// of which each file uses sub-slices.
    char* registered_buffer;
    /// For reusing buffers when another file is closed:
    /// This allows giving the slices to the preceeding open file.
    /// (This means that the slices of the first file can't be reused:
    ///  Moving the start of the slices is difficult because an in progress read
    ///  will fill in from the middle of the new slice.
    /// (Redistributing buffers fairly would be even more complicated,
    ///  and would require using vectored reads.)
    int* buffer_sizes;
    /// The numer of still open filees:
    /// .files - completely read files and files we're no longer interested in.
    /// This is used to detect when there are no more files.
    int open_files;
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
        unsigned int files, unsigned int per_file_buffer_sz,
        size_t extra_buffer_sz, size_t alloc_extra_other
);

/// Close the uring and all opened files, and free memory.
void uring_destroy(struct uring_reader *r);

/// Complete setup and submit open & initial read for all files.
void uring_open_files(struct uring_reader* r, const char* const* filenames);

/// Get any finished read, submitting queued reads and waiting only if none are ready.
/// file is set to the file index of the returned read.
/// Will not return any read for which another read has already been finished.
/// (This is to avoid callers needing to handle more than one read per file at any time.
///
/// Returns {.iov_base=NULL, iov_len=-1} if no files are open.
/// If successful, the returned buffer will be marked as "loaned out":
/// When finished with the read, uring_return_loan() or uring_write_and_return_loan() should be called.
/// (But they don't need to be called straight away.)
///
/// Intended for "fast forwarding" to a start key, where we want to go through the available bytes.
struct iovec uring_get_any_unloaned(struct uring_reader *r, int *file);

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
