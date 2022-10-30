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

#include "heap.h"
#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <heap size> string1,string2-,string3,... ...\n", argv[0]);
        fprintf(stderr, "',' pushes the preceeding characters, '-' pops one,");
        fprintf(stderr, "at the end of each argument, all entries are popped.\n");
        return EX_USAGE;
    }

    // parse heap_size without ignoring non-digits
    unsigned int size = 0;
    for (const char* size_str = argv[1]; *size_str != '\0'; size_str++) {
        if (*size_str < '0' || *size_str > '9') {
            fprintf(stderr, "heap size must be a positive whole number.\n");
            return EX_USAGE;
        }
        unsigned int new_size = size*10 + (unsigned int)(*size_str - '0');
        if (new_size < size) {
            fprintf(stderr, "heap size is too big.\n");
            return EX_USAGE;
        }
        size = new_size;
    }

    struct heap heap = heap_create(SLICE_MIN, size);
    heap_set_memory(&heap, malloc(heap_get_needed_memory(&heap)));
    int string_index = 0;

    for (int arg=2; arg<argc; arg++) {
        const char* start = argv[arg];
        const char* pos = start;
        while (*pos != '\0') {
            if (*pos == ',') {
                // push preceeding, also if empty
                size_t length = pos - start;
                string_index++;
                heap_push_bytes(&heap, start, length, string_index);
                start = pos + 1;
            } else if (*pos == '-') {
                // push preeceeding if any
                if (start != pos) {
                    size_t length = pos - start;
                    string_index++;
                    heap_push_bytes(&heap, start, length, string_index);
                }
                // then pop one
                struct iovec string;
                int value = heap_pop_slice_value(&heap, &string);
                printf("%02d: ", value);
                fwrite((char*)string.iov_base, string.iov_len, 1, stdout);
                putchar('\n');
                start = pos + 1;
            }
            pos++;
        }
        // push remainder if any
        if (start != pos) {
            size_t length = pos - start;
            string_index++;
            heap_push_bytes(&heap, start, length, string_index);
        }
        // pop all
        while (!heap_is_empty(&heap)) {
            struct iovec string;
            int value = heap_pop_slice_value(&heap, &string);
            printf("%02d: ", value);
            fwrite((char*)string.iov_base, string.iov_len, 1, stdout);
            putchar('\n');
        }
    }

    free(heap_get_memory(&heap));
    return EX_OK;
}
