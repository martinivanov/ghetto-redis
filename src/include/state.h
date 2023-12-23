#ifndef STATE_H
#define STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>

#include "deque.h"
#include "hashmap.h"
#include "vector_types.h"
#include "mpscq.h"

typedef struct GRState GRState;

typedef struct {
  uint64_t total_nfds;
  uint64_t total_flushes;
  uint64_t total_callbacks;
  uint64_t total_eventfd_events;
  uint64_t total_read_events;
  uint64_t total_write_events;
} ShardStats;

typedef struct {
  size_t shard_id;
  GRState *gr_state; // back reference to the global state
  vector_Conn_ptr *conns;
  Deque idle_conn_queue;
  Deque pending_writes_queue;
  atomic_bool notify_cb;
  int queue_efd;
  struct mpscq **cb_queues;
  struct hashmap **dbs;

  ShardStats stats;
} Shard;

struct GRState {
  bool running;
  size_t num_dbs;
  struct hashmap *commands;
  size_t num_shards;
  Shard *shards;
};


#endif