#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>

#include "deque.h"


void deque_init(Deque *deque) {
    deque->head = NULL;
    deque->tail = NULL;
}

bool deque_is_empty(Deque *deque) {
    return !deque->head;
}

void deque_push_front_node(Deque *deque, DequeNode *node) {
    node->next = deque->head;
    node->prev = NULL;

    if (deque->head) {
        deque->head->prev = node;
    } else {
        deque->tail = node;
    }

    deque->head = node;
}

void deque_push_front(Deque *deque, void *data) {
    DequeNode *node = malloc(sizeof(DequeNode));
    node->data = data;
    deque_push_front_node(deque, node);
}

void deque_push_back_node(Deque *deque, DequeNode *node) {
    node->next = NULL;
    node->prev = deque->tail;

    if (deque->tail) {
        deque->tail->next = node;
    } else {
        deque->head = node;
    }

    deque->tail = node;
}

void deque_push_back(Deque *deque, void *data) {
    DequeNode *node = malloc(sizeof(DequeNode));
    node->data = data;
    deque_push_back_node(deque, node);
}

DequeNode *deque_pop_front_node(Deque *deque) {
    if (deque == NULL) return NULL;
    if (deque_is_empty(deque)) return NULL;

    DequeNode *node = deque->head;
    deque->head = node->next;
    if (deque->head) {
        deque->head->prev = NULL;
    } else {
        deque->tail = NULL;
    }

    return node;
}

void* deque_pop_front(Deque *deque) {
    if (deque == NULL) return NULL;
    if (deque_is_empty(deque)) return NULL;

    DequeNode *node = deque->head;
    void *data = node->data;

    deque->head = node->next;
    if (deque->head) {
        deque->head->prev = NULL;
    } else {
        deque->tail = NULL;
    }

    free(node);
    node = NULL;
    return data;
}

void* deque_pop_back(Deque *deque) {
    if (deque == NULL) return NULL;
    if (deque_is_empty(deque)) return NULL;

    DequeNode *node = deque->tail;
    void *data = node->data;

    deque->tail = node->prev;
    if (deque->tail) {
        deque->tail->next = NULL;
    } else {
        deque->head = NULL;
    }

    free(node);
    node = NULL;
    return data;
}

void deque_detach(Deque *deque, DequeNode *node) {
    if (deque == NULL) return;
    if (deque_is_empty(deque)) return;
    if (node == NULL) return;

    if (node->prev) {
        node->prev->next = node->next;
    } else {
        deque->head = node->next;
    }

    if (node->next) {
        node->next->prev = node->prev;
    } else {
        deque->tail = node->prev;
    }
}

void deque_move_to_back(Deque *deque, DequeNode *node) {
    if (deque == NULL) return;
    if (deque_is_empty(deque)) return;
    if (node == NULL) return;

    if (node == deque->tail) {
        return;
    }

    deque_detach(deque, node);
    deque_push_back_node(deque, node);
}

void deque_move_to_front(Deque *deque, DequeNode *node) {
    if (deque == NULL) return;
    if (deque_is_empty(deque)) return;
    if (node == NULL) return;

    if (node == deque->head) {
        return;
    }

    deque_detach(deque, node);
    deque_push_front_node(deque, node);
}

void deque_destroy(Deque *deque) {
    while (!deque_is_empty(deque)) {
        deque_pop_front(deque);
    }
}