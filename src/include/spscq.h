#ifndef SPSCQ_H

#define SPSCQ_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

struct spscq {
    atomic_size_t head;
    atomic_size_t tail;
    size_t size;
	void * _Atomic *buffer;
};

struct spscq *spscq_create(struct spscq *q, size_t size);
void spscq_destroy(struct spscq *q);

bool spscq_enqueue(struct spscq *q, void *data);
void *spscq_dequeue(struct spscq *q);
bool spscq_is_empty(struct spscq *q);

#endif // SPSCQ_H