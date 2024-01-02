#ifndef STATE_H
#define STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>

#include "deque.h"
#include "hashmap.h"
#include "vector_types.h"
#include "spscq.h"
#include "bitset.h"

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
  atomic_bool sleeping;
  int queue_efd;
  struct spscq **cb_queues;
  struct mpscq *mpscq;
  struct hashmap **dbs;

  // max 64 shards allowed (64 threads)
  bitset64 soft_notify;

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