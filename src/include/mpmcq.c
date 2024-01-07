#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h> 
#include <sys/types.h>

#include "mpmcq.h"

void mpmcq_init(mpmcq* queue, size_t size) {
    queue->buffer = (mpmcq_cell*)malloc(sizeof(mpmcq_cell) * size);
    queue->buffer_mask = size - 1;
    for (size_t i = 0; i < size; i++) {
        queue->buffer[i].seq = i;
    }
    queue->enqueue_pos = 0;
    queue->dequeue_pos = 0;
}

void mpmcq_destroy(mpmcq* queue) {
    free(queue->buffer);
}

bool mpmcq_enqueue(mpmcq* queue, void* data) {
    mpmcq_cell* cell;
    size_t pos = atomic_load_explicit(&queue->enqueue_pos, memory_order_acquire);
    while(true) {
        cell = &queue->buffer[pos & queue->buffer_mask];
        size_t seq = atomic_load_explicit(&cell->seq, memory_order_acquire);
        size_t diff = seq - pos;
        if (diff == 0) {
            if (atomic_compare_exchange_weak(&queue->enqueue_pos, &pos, pos + 1)) {
                break;
            } 
        } else if (diff < 0) {
            return false;
        } else {
            pos = atomic_load_explicit(&queue->enqueue_pos, memory_order_relaxed);
        }
    }

    cell->data = data;
    atomic_store_explicit(&cell->seq, pos + 1, memory_order_release);

    return true;
}

void *mpmcq_dequeue(mpmcq* queue) {
    mpmcq_cell* cell;
    size_t pos = atomic_load_explicit(&queue->dequeue_pos, memory_order_relaxed);
    while(true) {
        cell = &queue->buffer[pos & queue->buffer_mask];
        size_t seq = atomic_load_explicit(&cell->seq, memory_order_acquire);
        ssize_t diff = seq - (pos + 1);
        if (diff == 0) {
            if (atomic_compare_exchange_weak(&queue->dequeue_pos, &pos, pos + 1)) {
                break;
            } 
        } else if (diff < 0) {
            return NULL;
        } else {
            pos = atomic_load_explicit(&queue->dequeue_pos, memory_order_relaxed);
        }
    }

    void* data = cell->data;
    atomic_store_explicit(&cell->seq, pos + queue->buffer_mask + 1, memory_order_release);
    return data;
}

size_t mpmcq_count(mpmcq* queue) {
    size_t pos = atomic_load_explicit(&queue->enqueue_pos, memory_order_relaxed);
    size_t count = atomic_load_explicit(&queue->dequeue_pos, memory_order_relaxed);
    return pos - count;
}