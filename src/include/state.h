#ifndef STATE_H
#define STATE_H

#include <stdbool.h>
#include <stddef.h>

#include "deque.h"
#include "hashmap.h"
#include "vector_types.h"
#include "mpscq.h"

typedef struct {
  size_t shard_id;
  vector_Conn_ptr *conns;
  Deque idle_conn_queue;
  Deque pending_writes_queue;
  bool running;
  size_t num_dbs;
  struct hashmap **dbs;
  struct hashmap *commands;
} State;

typedef struct {
  bool running;
  size_t num_dbs;
  struct hashmap *commands;
  size_t num_shards;
  struct shard *shards;
} GlobalState;

typedef struct {
  size_t shard_id;
  vector_Conn_ptr *conns;
  Deque idle_conn_queue;
  Deque pending_writes_queue;
  int queue_efd;
  struct mpscq *cb_queue;
  struct hashmap **dbs;
} Shard;

typedef struct {
  void *arg;
  void (*cb)(void *);
} Callback;

#endif