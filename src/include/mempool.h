#ifndef MEMPOOL_H
#define MEMPOOL_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "deque.h"

typedef struct {
    DequeNode free_list_node;
    uint8_t mem[];
} MemPoolItem;

typedef struct {
  void *pool;
  Deque free_list;
  size_t total_pool_size;
  size_t pool_item_size;
  size_t pool_size;
  size_t pool_idx;
  atomic_flag lock;
} MemPool;

MemPool *mem_pool_create(size_t pool_size, size_t elem_size);
void mem_pool_destroy(MemPool *pool);
void *mem_pool_rent(MemPool *pool);
void mem_pool_return(MemPool *pool, void *args);

#endif // MEMPOOL_H