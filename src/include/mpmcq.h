#ifndef MPMCQ_H
#define MPMCQ_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#define CACHE_LINE_SIZE 64

typedef struct {
    _Atomic(size_t) seq;
    void *data;
} mpmcq_cell;

typedef struct {
    char pad0[CACHE_LINE_SIZE];
    mpmcq_cell *buffer;
    size_t buffer_mask;
    char pad1[CACHE_LINE_SIZE];
    _Atomic(size_t) enqueue_pos;
    char pad2[CACHE_LINE_SIZE];
    _Atomic(size_t) dequeue_pos;
} mpmcq;

void mpmcq_init(mpmcq *q, size_t size);
void mpmcq_destroy(mpmcq *q);
bool mpmcq_enqueue(mpmcq *q, void *data);
void *mpmcq_dequeue(mpmcq *q);
size_t mpmcq_count(mpmcq *q);

#endif // MPMCQ_H