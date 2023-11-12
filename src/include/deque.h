#ifndef DEQUE_H
#define DEQUE_H

#include <stdbool.h>

typedef struct DequeNode {
    void *data;  // Pointer to the actual data
    struct DequeNode *prev;
    struct DequeNode *next;
} DequeNode;

typedef struct {
    DequeNode *head;
    DequeNode *tail;
} Deque;

void deque_init(Deque *deque);
bool deque_is_empty(Deque *deque);
void deque_push_front(Deque *deque, void *data);
void deque_push_back_node(Deque *deque, DequeNode *node);
void deque_push_back(Deque *deque, void *data);
DequeNode *deque_pop_front_node(Deque *deque);
void* deque_pop_front(Deque *deque);
void* deque_pop_back(Deque *deque);
void deque_detach(Deque *deque, DequeNode *node);
void deque_move_to_back(Deque *deque, DequeNode *node);
void deque_move_to_front(Deque *deque, DequeNode *node);
void deque_destroy(Deque *deque);

#define deque_push_back_and_attach(deque, data, type, field) \
    deque_push_back(&deque, data); \
    ((type*)data)->field = deque.tail;

#define deque_push_front_and_attach(deque, data, type, field) \
    deque_push_front(&deque, data); \
    ((type*)data)->field = deque.head;

#endif