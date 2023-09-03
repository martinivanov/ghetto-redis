#ifndef STATE_H
#define STATE_H

#include <stdbool.h>
#include <stddef.h>

#include "deque.h"
#include "hashmap.h"
#include "vector_types.h"

typedef struct {
  vector_Conn_ptr *conns;
  Deque idle_conn_queue;
  Deque pending_writes_queue;
  bool running;
  size_t num_dbs;
  struct hashmap **dbs;
  struct hashmap *commands;
} State;

#endif