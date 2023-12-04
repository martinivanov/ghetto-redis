#include <stdlib.h>

#include "mempool.h"
#include "deque.h"

MemPool *mem_pool_create(size_t pool_size, size_t item_size) {
    MemPool *mempool = malloc(sizeof(MemPool));
    size_t total_size = sizeof(MemPoolItem) + item_size;
    mempool->total_pool_size = total_size * pool_size;
    mempool->pool = malloc(mempool->total_pool_size);
    mempool->pool_size = pool_size;
    mempool->pool_item_size = item_size;
    deque_init(&mempool->free_list);

    atomic_flag_clear(&mempool->lock);

    for (size_t i = 0; i < pool_size; i++) {
        MemPoolItem *item = (MemPoolItem *)((uint8_t *)mempool->pool + i * total_size);
        deque_push_back_node(&mempool->free_list, &item->free_list_node);
    }

    return mempool;
}

void mem_pool_destroy(MemPool *pool) {
    free(pool->pool);
    free(pool);
}

void *mem_pool_rent(MemPool *pool) {
    // while(atomic_flag_test_and_set(&pool->lock) == true) {}
    DequeNode *node = deque_pop_front_node(&pool->free_list);
    // atomic_flag_clear(&pool->lock);

    if (node == NULL) {
        printf("malloc\n");
        void *item = malloc(pool->pool_item_size);
        return item;
    }

    MemPoolItem *item = (MemPoolItem *)((uint8_t *)node - offsetof(MemPoolItem, free_list_node));
    return item->mem;
}

void mem_pool_return(MemPool *mempool, void *mem) {    
    MemPoolItem *item = (MemPoolItem *)((uint8_t *)mem - offsetof(MemPoolItem, mem));
    if ((void *)item < mempool->pool || (void *)item >= (uint8_t *)mempool->pool + mempool->total_pool_size) {
        free(mem);
        return;
    }

    // while (atomic_flag_test_and_set(&mempool->lock) == true) {}
    deque_push_back_node(&mempool->free_list, &item->free_list_node);
    // atomic_flag_clear(&mempool->lock);
}
