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

typedef struct Reactor Reactor;

typedef struct {
  size_t shard_id;
  struct hashmap **dbs;
  Reactor *reactor;
} Shard;

typedef struct {
  Shard *shards;
  size_t size;
} ShardSet;

typedef struct {
  size_t num_dbs;
  struct hashmap *commands;
  Shard *shard;
  ShardSet shard_set;
} GRContext;

#endif