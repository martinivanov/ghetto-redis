#ifndef STATE_H
#define STATE_H

#include <stdbool.h>
#include <stddef.h>

#include "deque.h"
#include "hashmap.h"
#include "vector_types.h"
#include "mpscq.h"

typedef struct GRState GRState;

typedef struct {
  size_t shard_id;
  GRState *gr_state; // back reference to the global state
  vector_Conn_ptr *conns;
  Deque idle_conn_queue;
  Deque pending_writes_queue;
  int queue_efd;
  struct mpscq *cb_queue;
  struct hashmap **dbs;
} Shard;

struct GRState {
  bool running;
  size_t num_dbs;
  struct hashmap *commands;
  size_t num_shards;
  Shard *shards;
};

typedef struct {
  void *arg;
  void (*cb)(void *);
} Callback;

#endif