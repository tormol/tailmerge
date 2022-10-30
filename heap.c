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
#include <string.h>

struct heap heap_create(enum heap_type type, unsigned int size) {
    struct heap heap = {
        .entries = NULL,
        .type = type,
        .length = 0,
        .capacity = size
    };
    return heap;
}
size_t heap_get_needed_memory(const struct heap *heap) {
    return heap->capacity * sizeof(struct heap_entry);
}
void heap_set_memory(struct heap *heap, void* alloc) {
    heap->entries = (struct heap_entry*)alloc;
}
void* heap_get_memory(struct heap *heap) {
    return heap->entries;
}

bool heap_is_empty(const struct heap *heap) {
    return heap->length == 0;
}

bool heap_push_slice(struct heap *heap, struct iovec key, int value) {
    if (heap->length == heap->capacity) {
        return false;
    }

    heap->entries[heap->length].slice_key = key;
    heap->entries[heap->length].value = value;
    heap->length++;

    // the algorithm is simplest if array starts at 1, so just subtract when indexing
    unsigned int inserted = heap->length;

    while (inserted > 1) {
        unsigned int half = inserted/2;
        struct heap_entry *inserted_entry = &heap->entries[inserted-1];
        struct heap_entry *half_entry = &heap->entries[half-1];

        size_t inserted_length = inserted_entry->slice_key.iov_len;
        size_t half_length = half_entry->slice_key.iov_len;
        size_t min_length = inserted_length < half_length ? inserted_length : half_length;

        int cmp = memcmp(inserted_entry->slice_key.iov_base, half_entry->slice_key.iov_base, min_length);
        // stop if half is less than inserted
        if (cmp > 0) {
            break;
        }
        // if equal up to the shortest, stop if lengths are equal
        // (meaning all bytes were compared and entries are completely equal)
        // or if half is shorter than inserted (get shortest first)
        if (cmp == 0 && inserted_length >= half_length) {
            break;
        }

        struct heap_entry tmp = *half_entry;
        *half_entry = *inserted_entry;
        *inserted_entry = tmp;
        inserted /= 2;
    }
    
    return true;
}
bool heap_push_bytes(struct heap *heap, const char* key, int key_length, int value) {
    struct iovec slice = {.iov_base = (void*)key, .iov_len = key_length};
    return heap_push_slice(heap, slice, value);
}

int heap_pop_slice_value(struct heap *heap, struct iovec *popped_key) {
    if (heap->length == 0) {
        if (popped_key != NULL) {
            popped_key->iov_base = NULL;
            popped_key->iov_len = 0;
        }
        return -1;
    }

    // get the min
    int top_value = heap->entries[0].value;
    if (popped_key != NULL) {
        *popped_key = heap->entries[0].slice_key;
    }

    // put the last item in front, which likely is greater than its now children
    heap->length--;
    heap->entries[0] = heap->entries[heap->length];

    // and then fix the heap
    unsigned int new_parent = 1; // use one-based indexing when calculating, to simplify the logic
    do {
        struct iovec parent_key = heap->entries[new_parent-1].slice_key;
        size_t parent_length = parent_key.iov_len;
        unsigned int left_child = new_parent*2;
        unsigned int right_child = left_child+1;
        if (right_child <= heap->length) {
            // if right child is less than left child and less than parent
            struct iovec right_child_key = heap->entries[right_child-1].slice_key;
            size_t right_child_length = right_child_key.iov_len;
            size_t min_length_right = parent_length < right_child_length ? parent_length : right_child_length;
            struct iovec left_child_key = heap->entries[left_child-1].slice_key;
            size_t left_child_length = left_child_key.iov_len;
            size_t min_length_child = right_child_length < left_child_length ? right_child_length : left_child_length;
            if (memcmp(right_child_key.iov_base, left_child_key.iov_base, min_length_child) < 0
                && memcmp(parent_key.iov_base, right_child_key.iov_base, min_length_right) > 0) {
                // swap right child with parent
                struct heap_entry tmp = heap->entries[new_parent-1];
                heap->entries[new_parent-1] = heap->entries[right_child-1];
                heap->entries[right_child-1] = tmp;
                new_parent = right_child;
                continue;
            }
        }
        if (left_child <= heap->length) {
            // if left child is less than parent
            struct iovec left_child_key = heap->entries[left_child-1].slice_key;
            size_t left_child_length = left_child_key.iov_len;
            size_t min_length_left = parent_length < left_child_length ? parent_length : left_child_length;
            if (memcmp(parent_key.iov_base, left_child_key.iov_base, min_length_left) > 0) {
                // swap right child with parent
                struct heap_entry tmp = heap->entries[new_parent-1];
                heap->entries[new_parent-1] = heap->entries[left_child-1];
                heap->entries[left_child-1] = tmp;
                new_parent = left_child;
                continue;
            }
        }
    } while (false);

    return top_value;
}
