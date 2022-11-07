/* tailmerge - A program to sort together files like tail -f
 * Copyright (C) 2022 Torbjørn Birch Moltu
 *
 * licenced under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "heap.h"

#include <stdio.h> //
#include <errno.h> // errno
#include <string.h> // strerror()
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h> // memcmp(), memcpy(), malloc(), realloc(), free()
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h> // close()
#include <sys/uio.h> // struct iovec, writev()
#include <sysexits.h> // EX_OK, EX_USAGE, EX_NOINPUT, EX_UNAVAILABLE, EX_IOERR

const char *HELP_MESSAGE = "\
Usage: tailmerge file1 [file2]...\n\
\n\
\"Sorts\" the files but prints the file name above each group of lines from a file, like `tail -f`.\n\
Files are merged by sorting the next unprinted line from each file,\n\
without reordering lines from the same file or keeping everything in RAM.\n\
(Memory usage is linear with the number of files, not with the file sizes.)\n\
";
const char *MARKER = "\n>>> ";

// print error messages and exit if `ret` is negative,
// otherwise pass it through to caller.
int checkerr(int ret, int status, const char *desc, ...) {
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
    exit(status);
}

void *check_malloc(size_t bytes) {
    void *allocation = malloc(bytes);
    if (allocation == NULL)
    {
        fputs("Not enough memory.\n", stderr);
        exit(EX_UNAVAILABLE);
    }
    return allocation;
}

void single_free(void **allocation) {
    if (allocation != NULL  &&  *allocation != NULL) {
        free(*allocation);
        *allocation = NULL;
    }
}


struct source {
    char *buffer; //< owned allocation that bytes are read into
    int capacity; //< size of buffer
    int length; //< how many bytes in buffer have been read
    int start; //< offset of the next line; bytes in buffer before this have already been written
    int end; //< offset of the following line
    const char *path; //< borrowed name of the file, NUL-terminated
    int fd; //< owned file descriptor
};

struct source source_create(const char *path, int default_buffer_size) {
    struct source s = {
        .buffer = check_malloc(default_buffer_size),
        .capacity = default_buffer_size,
        .end = s.start = s.length = 0,
        .path = path,
        .fd = checkerr(open(path, O_RDONLY), 2, "opening %s", path)
    };
    return s;
}

void source_destroy(struct source *source) {
    single_free((void**)&source->buffer);
    if (source->fd != -1) {
        if (close(source->fd) != 0) {
            fprintf(stderr, "Error closing %s: %s\n", source->path, strerror(errno));
            // but don't exit
        }
        source->fd = -1;
    }
}

struct iovec source_line(const struct source *source) {
    struct iovec slice = {
        .iov_base = &source->buffer[source->start],
        .iov_len = source->end - source->start
    };
    return slice;
}

/// find the next line.
/// returns true if there is another line
bool source_advance(struct source *source) {
    if (source->end == source->length) {
        source->start = source->end;
        return false;
    }
    char *next_end = memchr(
        &source->buffer[source->end],
        '\n',
        source->length - source->end
    );
    if (next_end != NULL) {
        source->start = source->end;
        source->end = next_end + 1 - source->buffer;
        return true;
    } else {
        return false;
    }
}

bool source_read(struct source *source) {
    // move start of unfinished line to front
    if (source->start != 0) {
        memmove(
            source->buffer,
            &source->buffer[source->start],
            source->end - source->start
        );
        source->end -= source->start;
        source->start = 0;
    }
    int more = checkerr(
        read(source->fd, &source->buffer[source->end], source->capacity - source->end),
        EX_IOERR,
        "reading from %s", source->path
    );
    source->length = source->end + more;
    if (source->length == 0) {
        // end of file and nothing left in buffer
        return false;
    }
    char *end = memchr(&source->buffer[source->end], '\n', more);
    if (end == NULL) {
        source->end = source->length;
    } else {
        source->end = end + 1 - source->buffer;
    }
    return true;
}


struct lines {
    struct iovec *to_write; //< owned allocation
    int length; //< number of unwritten slices
    int capacity; //< max number of slices
};

struct lines lines_create(int capacity) {
    struct lines lines = {
        .to_write = check_malloc(capacity * sizeof(struct iovec)),
        .length = 0,
        .capacity = capacity
    };
    return lines;
}

void lines_destroy(struct lines *lines) {
    single_free((void**)&lines->to_write);
}

void lines_flush(struct lines *lines) {
    int completely_written = 0;
    while (completely_written < lines->length) {
        ssize_t written = writev(
            0,
            &lines->to_write[completely_written],
            lines->length-completely_written
        );
        checkerr((int)written, EX_IOERR, "writing to stdout");
        while (written >= (ssize_t)lines->to_write[completely_written].iov_len) {
            written -= lines->to_write[completely_written].iov_len;
            completely_written++;
        }
        if (written != 0) {
            void *start = lines->to_write[completely_written].iov_base;
            lines->to_write[completely_written].iov_base = (char*)start + written;
            lines->to_write[completely_written].iov_len -= written;
        }
    }
    // TODO use regular write() if one?
    lines->length = 0;
}

void lines_add(struct lines *lines, struct iovec slice) {
    if (lines->length == lines->capacity) {
        lines_flush(lines);
    }
    lines->to_write[lines->length] = slice;
    lines->length++;
}

const struct iovec NEWLINE = { .iov_base = "\n", .iov_len = 1 };


int main(int argc, const char **argv) {
    if (argc < 2) {
        fputs(HELP_MESSAGE, stderr);
        exit(EX_USAGE);
    }
    int sources_length = argc - 1;

    struct source *sources = check_malloc(sources_length * sizeof(struct source));

    int last = -1;
    struct heap sorter = heap_create(SLICE_MIN, sources_length);
    heap_set_memory(&sorter, check_malloc(heap_get_needed_memory(&sorter)));
    for (int i=0; i<sources_length; i++) {
        sources[i] = source_create(argv[i+1], 0xffff);
        if (source_read(&sources[i])) {
            heap_push_slice(&sorter, source_line(&sources[i]), i);
        } else {
            source_destroy(&sources[i]);
        }
    }

    struct lines lines = lines_create(1024);
    struct iovec line;
    int next = -1;
    while ((next = heap_pop_slice_value(&sorter, &line)) != -1) {
        if (next != last) {
            // add header
            struct iovec separator = { .iov_base = "\n>>> ", .iov_len = 5 };
            if (last == -1) {
                // first line of output, skip newline
                separator.iov_base = (char*)separator.iov_base + 1;
                separator.iov_len--;
            }
            lines_add(&lines, separator);
            struct iovec name = {
                .iov_base = (void*)sources[next].path,
                .iov_len = strlen(sources[next].path)
            };
            lines_add(&lines, name);
            lines_add(&lines, NEWLINE);
            last = next;
        }

        lines_add(&lines, line);

        if (source_advance(&sources[next])) {
            line = source_line(&sources[next]);
            // have more lines in buffer
            heap_push_slice(&sorter, line, next);
        } else if (((char*)line.iov_base)[line.iov_len-1] != '\n') {
            // was truncated
            lines_flush(&lines);
            bool is_truncated = true;
            while (source_read(&sources[next])) {
                line = source_line(&sources[next]);
                lines_add(&lines, line);
                is_truncated = ((char*)line.iov_base)[line.iov_len-1] != '\n';
                if ((! is_truncated) && source_advance(&sources[next])) {
                    // that means there was a newline
                    heap_push_slice(&sorter, line, next);
                    break;
                } else {
                    lines_flush(&lines);
                }
            }
            if (is_truncated) {
                lines_add(&lines, NEWLINE);
            }
        } else {
            // need to read more
            lines_flush(&lines);
            if (source_read(&sources[next])) {
                line = source_line(&sources[next]);
                heap_push_slice(&sorter, line, next);
            }
        }
    }

    // TODO optimize last remaining file by reading into all buffers

    // optional cleanup
    lines_destroy(&lines);
    free(heap_get_memory(&sorter));
    for (int i=0; i<sources_length; i++) {
        source_destroy(&sources[i]);
    }
    free(sources);

    return EX_OK;
}
