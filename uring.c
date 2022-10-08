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

#include <string.h> // memcpy()
#include <stdio.h> // fprintf() and stderr
#include <stdbool.h>
#include "uring_reader.h"

#define PER_FILE_BUFFER_SZ (4*1024)
#define MAX_PRINT_CHARACTERS (32 - 1)

struct file_line_info {
    int lines_read;
    char incomplete_line[MAX_PRINT_CHARACTERS + 1];
    int incomplete_line_length;
    off_t line_start_offset;
};

struct line {
    int line_number;
    int line_length;
    char *line;
    off_t byte_offset;
};

/// returns number of newlines found
static int find_lines(char* buffer, int bytes, int *first_line_length, int *last_line_starts) {
    char* newline_at = memchr(buffer, '\n', bytes);
    if (newline_at == NULL) {
        *first_line_length = bytes;
        *last_line_starts = bytes;
        return 0;
    }

    *first_line_length = (int)(newline_at + 1 - buffer);
    int lines = 0;
    char* start_of_line = buffer;
    do {
        lines++;
        int line_length = (int)(newline_at - start_of_line);
#ifdef DEBUG
        start_of_line[line_length] = '\0';
        fprintf(stderr, "%3d: %s\n", lines, start_of_line);
        start_of_line[line_length] = '\n';
#endif
        bytes -= line_length;
        start_of_line = newline_at + 1;
        bytes--;
        newline_at = memchr(start_of_line, '\n', bytes);
    } while (newline_at != NULL);
    *last_line_starts = (int)(start_of_line - buffer);
    return lines;
}

struct line get_first_line_in_read(struct file_line_info *l, struct iovec read) {
    struct line line = {
        .byte_offset = l->line_start_offset,
        .line_number = l->lines_read,
        .line_length = 0,
        .line = NULL
    };

    if (read.iov_len == 0) {
        // Don't count a terminating newline.
        if (l->incomplete_line_length == 0) {
            line.line_number--;
        }
        return line;
    }

    int first_line_length, last_line_starts;
    int new_lines = find_lines(read.iov_base, read.iov_len, &first_line_length, &last_line_starts);
    if (l->incomplete_line_length == 0) {
        line.line = read.iov_base;
        if (first_line_length > MAX_PRINT_CHARACTERS) {
            first_line_length = MAX_PRINT_CHARACTERS;
        }
        line.line_length = first_line_length;
    } else if (l->incomplete_line_length >= MAX_PRINT_CHARACTERS) {
        line.line = l->incomplete_line;
        line.line_length = MAX_PRINT_CHARACTERS;
    } else {
        if (l->incomplete_line_length + first_line_length > MAX_PRINT_CHARACTERS) {
            first_line_length = MAX_PRINT_CHARACTERS - l->incomplete_line_length;
        }
        line.line = l->incomplete_line;
#ifdef DEBUG
        fprintf(stderr, "copy %d bytes to offset %d\n", first_line_length, l->incomplete_line_length);
#endif
        memcpy(&l->incomplete_line[l->incomplete_line_length], read.iov_base, first_line_length);
        line.line_length = l->incomplete_line_length + first_line_length;
    }
    line.line[line.line_length] = '\0';

    // update for next
    l->lines_read += new_lines;
    int next_incomplete_line_length = read.iov_len - last_line_starts;
#ifdef DEBUG
    fprintf(stderr, "prev incomplete: %d, new bytes: %d, next incomplete: %d\n", l->incomplete_line_length, (int)read.iov_len, next_incomplete_line_length);
#endif
    l->line_start_offset += l->incomplete_line_length + read.iov_len - next_incomplete_line_length;
    l->incomplete_line_length = next_incomplete_line_length;
#ifdef DEBUG
    fprintf(stderr, "prev offset: %d, next offset: %d\n", (int)line.byte_offset, (int)l->incomplete_line_length);
#endif

    return line;
}

static void finish_read(struct file_line_info *l, struct iovec read) {
#ifdef DEBUG
    fprintf(stderr, "copy %d last bytes of %d bytes.\n", l->incomplete_line_length, (int)read.iov_len);
#endif
    // copy incomplete line
    char* bytes = (char*)read.iov_base;
    int line_starts_at = read.iov_len - l->incomplete_line_length;
    int to_copy = l->incomplete_line_length;
    if (to_copy > MAX_PRINT_CHARACTERS) {
        to_copy = MAX_PRINT_CHARACTERS;
    }
    memcpy(l->incomplete_line, &bytes[line_starts_at], to_copy);
}

int main(int argc, const char* const* argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s file...\n", argv[0]);
        return 1;
    }

    struct uring_reader reader;
    struct file_line_info* lines_info = uring_create(&reader,
            argc-1,
            PER_FILE_BUFFER_SZ,
            0,
            (argc-1) * sizeof(struct file_line_info)
    );

    // line numbers start at 1
    for (int i = 0; i < reader.files; i++) {
        lines_info[i].lines_read = 1;
    }

    uring_open_files(&reader, &argv[1]);

    while (true) {
        int file = -1;
        struct iovec read = uring_get_any_unloaned(&reader, &file);
        if (file == -1) {
            break;
        }

        struct line line = get_first_line_in_read(&lines_info[file], read);
        if (line.line_length == 0) {
            printf("%s finished: %d lines %llu bytes\n",
                    reader.filenames[file], line.line_number,
                    (unsigned long long int)line.byte_offset
            );
        } else {
            if (line.line_length > 16) {
                line.line_length = 16;
            } else {
                line.line_length--;
            }
            line.line[line.line_length] = '\0';
            printf("%s:%03d (offset %05llu): %s ...\n",
                    reader.filenames[file], line.line_number,
                    (unsigned long long int)line.byte_offset,
                    line.line
            );
        }
        finish_read(&lines_info[file], read);

        uring_return_loan(read.iov_base);
    }

#ifdef DEBUG
    uring_destroy(&reader);
#endif

    return 0;
}
