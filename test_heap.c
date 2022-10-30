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
#include <string.h>
#include <sysexits.h>

typedef void(*pop_callback)(struct iovec str, int inserted);

static int perform_sequence(struct heap *heap, const char* input, pop_callback pop_callback) {
    // ensure it's empty
    while (!heap_is_empty(heap)) {
        heap_pop_slice_value(heap, NULL);
    }

    int insert_number = 0;
    const char* pos = input;
    while (*pos != '\0') {
        if (*pos == ',') {
            // push preceeding, also if empty
            size_t length = pos - input;
            insert_number++;
            heap_push_bytes(heap, input, length, insert_number);
            input = pos + 1;
        } else if (*pos == '-') {
            // push preeceeding if any
            if (input != pos) {
                size_t length = pos - input;
                insert_number++;
                heap_push_bytes(heap, input, length, insert_number);
            }
            // then pop one
            struct iovec string;
            int value = heap_pop_slice_value(heap, &string);
            pop_callback(string, value);
            input = pos + 1;
        }
        pos++;
    }

    // push remainder if any
    if (input != pos) {
        size_t length = pos - input;
        insert_number++;
        heap_push_bytes(heap, input, length, insert_number);
    }

    // pop all
    while (!heap_is_empty(heap)) {
        struct iovec string;
        int value = heap_pop_slice_value(heap, &string);
        pop_callback(string, value);
    }

    return insert_number;
}

static struct iovec string_output, value_output;
static size_t string_output_capacity, value_output_capacity;
static void pop_store(struct iovec string, int insert_number) {
    memcpy(string_output.iov_base+string_output.iov_len, string.iov_base, string.iov_len);
    string_output.iov_len += string.iov_len;
    ((char*)string_output.iov_base)[string_output.iov_len] = ',';
    string_output.iov_len++;

    // knowing the needed capacity would require iterating the input twice.
    char buf[8];
    int length = snprintf(buf, sizeof(buf), "%d,", insert_number);
    while (value_output.iov_len + length > value_output_capacity) {
        value_output_capacity *= 2;
        value_output.iov_base = realloc(value_output.iov_base, value_output_capacity);
    }
    memcpy(value_output.iov_base+value_output.iov_len, buf, length);
    value_output.iov_len += length;
}

static void assert_sequence(struct heap *heap, const char* input,
                            const char* expected_output, const char* expected_values, int expected_max_value) {
    if (input == NULL) {
        // total reset
        free(string_output.iov_base);
        free(value_output.iov_base);
        string_output.iov_base = value_output.iov_base = NULL;
        string_output.iov_len = value_output.iov_len = 0;
        string_output_capacity = value_output_capacity = 0;
        return;
    }

    printf("Testing %s ", input);
    fflush(stdout);

    // reset output strings length
    string_output.iov_len = value_output.iov_len = 0;
    // allocate or grow strings if necessary
    size_t max_output_length = strlen(input) + 1;
    if (max_output_length > string_output_capacity) {
        string_output.iov_base = realloc(string_output.iov_base, max_output_length);
        string_output_capacity = max_output_length;
    }
    if (value_output_capacity == 0) {
        value_output_capacity = 24;
        value_output.iov_base = malloc(value_output_capacity);
    }

    // run test
    int max_value = perform_sequence(heap, input, pop_store);
    // prepare results
    if (string_output.iov_len > 0) {
        string_output.iov_len--;
    }
    char* got_output = (char*)string_output.iov_base;
    got_output[string_output.iov_len] = '\0';
    if (value_output.iov_len > 0) {
        value_output.iov_len--;
    }
    char* got_values = (char*)value_output.iov_base;
    got_values[value_output.iov_len] = '\0';

    if (expected_output != NULL && strcmp(got_output, expected_output) != 0) {
        printf("FAILED\nExpected output %s (%d bytes)\n", expected_output, (int)strlen(expected_output));
        printf(" but got output %s (%d bytes)\n", got_output, (int)string_output.iov_len);
        printf("     and values %s\n (highest: %d)\n", got_values, max_value);
        exit(EXIT_FAILURE);
    } else if (expected_values != NULL && strcmp(got_values, expected_values) != 0) {
        printf("FAILED\nExpected values %s (and highest: %d)\n", expected_values, expected_max_value);
        printf(" but got values %s (highest: %d)\n", got_values, max_value);
        printf("     and output %s (%d bytes)\n", got_output, (int)string_output.iov_len);
        exit(EXIT_FAILURE);
    } else if (expected_max_value >= 0 && expected_max_value != max_value) {
        printf("FAILED\nExpected max value %d but got %d\n", expected_max_value, max_value);
        printf("from values %s\n", got_values);
        printf(" and output %s (%d bytes)\n", got_output, (int)string_output.iov_len);
        exit(EXIT_FAILURE);
    } else {
        printf("PASSED\n");
    }
}

static void pop_verbose(struct iovec string, int insert_number) {
    printf("%02d: ", insert_number);
    fwrite((char*)string.iov_base, string.iov_len, 1, stdout);
    putchar('\n');
}

static void usage(const char* argv0) {
    fprintf(stderr, "Usage: %s <heap value> string1,string2-,string3,... ...\n", argv0);
    fprintf(stderr, "       %s assert input expected_output [expected values [expected_max_value]]\n", argv0);
    fprintf(stderr, "',' pushes the preceeding characters, '-' pops one,");
    fprintf(stderr, "at the end of each argument, all entries are popped.\n");
    exit(EX_USAGE);
}

/// parse integer_without ignoring non-digits
static unsigned int parse_unsigned(const char* arg, const char* desc, unsigned int max) {
    if (arg == NULL || *arg == '\0') {
        fprintf(stderr, "%s must not be empty\n", desc);
        exit(EX_USAGE);
    }
    unsigned int value = 0;
    for (const char* value_str = arg; *value_str != '\0'; value_str++) {
        if (*value_str < '0' || *value_str > '9') {
            fprintf(stderr, "%s must be a positive whole number.\n", desc);
            exit(EX_USAGE);
        }
        unsigned int new_value = value*10 + (unsigned int)(*value_str - '0');
        if (new_value < value || new_value > max) {
            fprintf(stderr, "%s is too big.\n", desc);
            exit(EX_USAGE);
        }
        value = new_value;
    }
    return value;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
    }

    if (strcmp(argv[1], "assert") == 0) {
        if (argc < 3 || argc > 6) {
            usage(argv[0]);
        }
        const char* input = argv[2];
        const char* expected_output = argc<4 ? NULL : (*argv[3] == '\0' ? NULL : argv[3]);
        const char* expected_values = argc<5 ? NULL : (*argv[4] == '\0' ? NULL : argv[4]);
        int expected_max_value = argc<6 ? -1 : (int)parse_unsigned(argv[5], "max value", ~0>>1);

        int max_size = strlen(argv[2]);
        struct heap heap = heap_create(SLICE_MIN, max_size);
        heap_set_memory(&heap, malloc(heap_get_needed_memory(&heap)));
        assert_sequence(&heap, input, expected_output, expected_values, expected_max_value);
        return EX_OK;
    }

    struct heap heap = heap_create(SLICE_MIN, parse_unsigned(argv[1], "heap value", ~0));
    heap_set_memory(&heap, malloc(heap_get_needed_memory(&heap)));
    for (int arg=2; arg<argc; arg++) {
        perform_sequence(&heap, argv[arg], pop_verbose);
    }
    free(heap_get_memory(&heap));
    return EX_OK;
}
