#ifdef FALSE
#include <stddef.h>
#include <stdbool.h>
#include "deque_embedded.h"

void deque_init(Deque *node) {
    node->next = node;
    node->prev = node;
}

bool deque_is_empty(Deque *node) {
    return node->next == node;
}

void deque_detach(Deque *node) {
    if (node == NULL) {
        return;
    }

    if (deque_is_empty(node)) {
        return;
    }

    Deque *prev = node->prev;
    Deque *next = node->next;

    if (next != NULL) {
        next->prev = prev;
    }
    
    if (prev != NULL) {
        prev->next = next;
    }
}

void deque_insert_after(Deque *node, Deque *new_node) {
    Deque *next = node->next;

    new_node->next = next;
    new_node->prev = node;
    next->prev = new_node;
    node->next = new_node;
}

void deque_insert_before(Deque *node, Deque *new_node) {
    Deque *prev = node->prev;

    new_node->next = node;
    new_node->prev = prev;
    prev->next = new_node;
    node->prev = new_node;
}
#endif