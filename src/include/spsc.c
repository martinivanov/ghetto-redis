#include <stdlib.h>

#include "spscq.h"

struct spscq *spscq_create(struct spscq *q, size_t size) {
    if(!q) {
        q = calloc(1, sizeof(*q));
    }
    q->head = 0;
    q->tail = 0;
    q->buffer = calloc(size, sizeof(void *));
    q->size = size;
    return q;
}

void spscq_destroy(struct spscq *q)
{
    free(q->buffer);
    free(q);
}

bool spscq_enqueue(struct spscq *q, void *obj)
{
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    size_t next_tail = tail + 1;
    if(next_tail == q->size) {
        next_tail = 0;
    }

    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    if(next_tail == head) {
        return false;
    }

    q->buffer[tail] = obj;

    atomic_store_explicit(&q->tail, next_tail, memory_order_release);

    return true;
}

void *spscq_dequeue(struct spscq *q) {
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    if(head == tail) {
        return NULL;
    }

    void *obj = q->buffer[head];

    size_t next_head = head + 1;
    if(next_head == q->size) {
        next_head = 0;
    }

    atomic_store_explicit(&q->head, next_head, memory_order_release);

    return obj;
}