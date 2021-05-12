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
Usage: log_merge file1 [file2]...\n\
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
    single_free(&source->buffer);
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


bool sources_less(int left_index, int right_index,
                  const struct source sources[], int sources_length,
                  int last) {
    struct iovec left_line = source_line(&sources[left_index]);
    struct iovec right_line = source_line(&sources[right_index]);
    int min_length = left_line.iov_len < right_line.iov_len
                   ? left_line.iov_len
                   : right_line.iov_len;
    int cmp = memcmp(left_line.iov_base, right_line.iov_base, min_length);
    if (cmp != 0)
    {
        return cmp < 0;
    }
    if (left_index == last) {
        return true;
    }
    if (right_index == last) {
        return false;
    }
    return left_index < right_index;
}


struct sorter {
    int *elements; //< owned allocation of natural numbers
    int heapified; //< numbor of sorted elements
    int unordered; //< number of unsorted elements after heapified
    int capacity; //< capacity of .elements
};

struct sorter sorter_create(int max_elements) {
    struct sorter sorter = {
        .elements = check_malloc(max_elements * sizeof(int)),
        .heapified = 0,
        .unordered = 0,
        .capacity = max_elements
    };
    memset(sorter.elements, -1, sorter.capacity * sizeof(int));
    return sorter;
}

void sorter_destroy(struct sorter *sorter) {
    single_free(&sorter->elements);
}

// returns -1 if empty
int sorter_pop(struct sorter *sorter,
               const struct source sources[], int sources_length,
               int last) {
    // push unsorted elements properly until we can do push-then-pop
    while (sorter->unordered > 1) {
        int index = sorter->heapified;
        // up-heap
        while (index > 0) {
            // zero-based; odd(1) means left child, even(2) means right
            int parent = ((index+1)/2)-1;
            if (! sources_less(
                    sorter->elements[index],
                    sorter->elements[parent],
                    sources, sources_length, last
            )) {
                break;
            }
            int tmp = sorter->elements[index];
            sorter->elements[index] = sorter->elements[parent];
            sorter->elements[parent] = tmp;
        }
        sorter->heapified++;
        sorter->unordered--;
    }
    // try push-then-pop (the common case)
    if (sorter->unordered == 1) {
        if (sorter->heapified == 0  ||
            sources_less(
                sorter->elements[sorter->heapified],
                sorter->elements[0],
                sources, sources_length, last
            )) {
            int next = sorter->elements[sorter->heapified];
            sorter->elements[sorter->heapified] = -1;
            sorter->unordered = 0;
            return next;
        } else {
            // fake a proper heap - works because down-heap puts the last in the root
            sorter->heapified++;
            sorter->unordered = 0;
        }
    } else if (sorter->heapified == 0) {
        return -1;
    }
    // have a proper heap
    int next = sorter->elements[0];
    sorter->heapified--;
    // down-heap
    sorter->elements[0] = sorter->elements[sorter->heapified];
    sorter->elements[sorter->heapified] = -1;
    int top = 0;
    while (true) {
        int left = (top+1)*2-1;
        int right = (top+1)*2;
        int after = top;
        if (left < sorter->heapified  &&
            sources_less(
                sorter->elements[left],
                sorter->elements[after],
                sources, sources_length, last
            )) {
            after = left;
        }
        if (right < sorter->heapified  &&
            sources_less(
                sorter->elements[right],
                sorter->elements[after],
                sources, sources_length, last
            )) {
            after = right;
        }
        if (after == top) {
            break;
        }
        after = top;
    }
    return next;
}

void sorter_push(struct sorter *sorter, int value) {
    int index = sorter->heapified + sorter->unordered;
    if (index >= sorter->capacity) {
        fprintf(stderr, "Error adding %d to sorter: already at capacity (%d)\n",
                        value,
                        sorter->capacity);
        exit(EX_SOFTWARE);
    }
    if (value < 0) {
        fprintf(stderr, "Error adding %d to sorter: should not be negative.\n", value);
        exit(EX_SOFTWARE);
    }
    if (value >= sorter->capacity) {
        fprintf(stderr, "Error adding %d to sorter; should be smaller than %d.\n",
                        value, sorter->capacity);
        exit(EX_SOFTWARE);
    }
    int *slot = &sorter->elements[index];
    if (*slot != -1) {
        fprintf(stderr, "Error adding %d to sorter: slot %d already occupied (%d).\n",
                        value, index, *slot);
        exit(EX_SOFTWARE);
    }
    *slot = value;
    sorter->unordered++;
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
    single_free(&lines->to_write);
}

void lines_flush(struct lines *lines) {
    int completely_written = 0;
    while (completely_written < lines->length) {
        size_t written = writev(
            0,
            &lines->to_write[completely_written],
            lines->length-completely_written
        );
        checkerr((int)written, EX_IOERR, "writing to stdout");
        while (written >= lines->to_write[completely_written].iov_len) {
            written -= lines->to_write[completely_written].iov_len;
            completely_written++;
        }
        if (written != 0) {
            lines->to_write[completely_written].iov_base += written;
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
    struct sorter sorter = sorter_create(sources_length);
    for (int i=0; i<sources_length; i++) {
        sources[i] = source_create(argv[i+1], 0xffff);
        if (source_read(&sources[i])) {
            sorter_push(&sorter, i);
        } else {
            source_destroy(&sources[i]);
        }
    }

    struct lines lines = lines_create(1024);
    int next = -1;
    while ((next = sorter_pop(&sorter, sources, sources_length, last)) != -1) {
        if (next != last) {
            // add header
            struct iovec separator = { .iov_base = "\n>>> ", .iov_len = 5 };
            if (last == -1) {
                // first line of output, skip newline
                separator.iov_base = &separator.iov_base[1];
                separator.iov_len--;
            }
            lines_add(&lines, separator);
            struct iovec name = {
                .iov_base = sources[next].path,
                .iov_len = strlen(sources[next].path)
            };
            lines_add(&lines, name);
            lines_add(&lines, NEWLINE);
            last = next;
        }

        struct iovec line = source_line(&sources[next]);
        lines_add(&lines, line);
        if (source_advance(&sources[next])) {
            // have more lines in buffer
            sorter_push(&sorter, next);
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
                    sorter_push(&sorter, next);
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
                sorter_push(&sorter, next);
            }
        }
    }

    // TODO optimize last remaining file by reading into all buffers

    // optional cleanup
    lines_destroy(&lines);
    sorter_destroy(&sorter);
    for (int i=0; i<sources_length; i++) {
        source_destroy(&sources[i]);
    }
    free(sources);

    return EX_OK;
}
