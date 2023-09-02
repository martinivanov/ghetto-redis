#ifdef FALSE
#ifndef DEQUE_EMBEDDED_H
#define DEQUE_EMBEDDED_H

#include <stddef.h>
#include <stdbool.h>

#define container_of(ptr, type, member) ({                  \
    const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
    (type *)( (char *)__mptr - offsetof(type, member) );})

struct Deque;

typedef struct Deque {
    struct Deque *prev;
    struct Deque *next;
} Deque;

void deque_init(Deque *node);
bool deque_is_empty(Deque *node);
void deque_detach(Deque *node);
void deque_insert_after(Deque *node, Deque *new_node);
void deque_insert_before(Deque *node, Deque *new_node);

#define DEQUE_FOREACH(node, head) \
    for (Deque *node = (head)->next; node != (head); node = node->next)


#endif
#endif