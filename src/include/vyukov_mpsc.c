#include <stdatomic.h>
#include <stdbool.h>

#include "vyukov_mpsc.h"

void vmpscq_init(mpscq_t* self) {
    self->stub = (mpscq_node_t*)malloc(sizeof(mpscq_node_t));
    self->head = self->stub;
    self->tail = self->stub;
    atomic_store(&self->stub->next, NULL);
}

void vmpscq_enqueue(mpscq_t* self, mpscq_node_t* n) {
    atomic_store(&n->next, NULL);
    mpscq_node_t* prev = atomic_exchange(&self->tail, n);
    atomic_store(&prev->next, n);
}

mpscq_node_t* vmpscq_dequeue(mpscq_t* self) {
  mpscq_node_t* head_copy = self->head;
  mpscq_node_t* next = head_copy->next;

  if (next != NULL) {
    self->head = next;
    head_copy->state = next->state;
    return head_copy;
  }
  return NULL;
}

bool vmpscq_is_empty(mpscq_t* self) {
    return self->head == self->tail;
}