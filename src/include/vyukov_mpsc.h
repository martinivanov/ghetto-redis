#ifndef VYUKOV_MPSC_H
#define VYUKOV_MPSC_H

#include <stdatomic.h>
#include <stdlib.h>

// Define the node structure
typedef struct {
    _Atomic(struct mpscq_node_t*) volatile next;
    void* state;
} mpscq_node_t;

// Function to create the queue
typedef struct {
    _Atomic(struct mpscq_node_t*) head;
    _Atomic(struct mpscq_node_t*) tail;
    mpscq_node_t *stub;
} mpscq_t;

void vmpscq_init(mpscq_t* self);
void vmpscq_enqueue(mpscq_t* self, mpscq_node_t* n);
mpscq_node_t* vmpscq_dequeue(mpscq_t* self);
bool vmpscq_is_empty(mpscq_t* self);

#endif // VYUKOV_MPSC_H