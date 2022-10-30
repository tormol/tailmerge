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

//! A bounded min-heap where items have both a key and a value.

#ifndef _HEAP_H_
#define _HEAP_H_
#include <sys/uio.h> // struct iovec
#include <sys/time.h> // struct timeval
#include <stdbool.h>

struct heap_entry {
    union {
        struct iovec slice_key;
        struct timeval time_key;
    };
    int value;
};

enum heap_type {
    SLICE_MIN,
    //TIME_MIN
};

struct heap {
    struct heap_entry* entries;
    enum heap_type type;
    unsigned int length;
    unsigned int capacity;
};

/// Initializes the struct but does not allocate
struct heap heap_create(enum heap_type type, unsigned int size);
size_t heap_get_needed_memory(const struct heap *heap);
void heap_set_memory(struct heap *heap, void* alloc);
void* heap_get_memory(struct heap *heap);

bool heap_is_empty(const struct heap *heap);

bool heap_push_slice(struct heap *heap, struct iovec key, int value);
bool heap_push_bytes(struct heap *heap, const char* key, int key_length, int value);
//bool heap_push_timestamp(struct heap *heap, struct timeval key, int value);

struct iovec heap_peek_key_slice(const struct heap *heap);
//struct timeval heap_peek_key_timestamp(const struct heap *heap);

int heap_pop_slice_value(struct heap *heap, struct iovec *key);

void heap_debug_print(const struct heap *heap);

#endif // !defined(_HEAP_H_)
