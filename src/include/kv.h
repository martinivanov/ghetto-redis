#ifndef KV_H
#define KV_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define ENTRY_INIT(k, klen, v, vlen) \
  &(Entry){                          \
      .key = k,                      \
      .keylen = klen,                \
      .val = v,                      \
      .vallen = vlen                 \
  };

typedef struct {
  const uint8_t *key;
  const size_t keylen;
  const uint8_t *val;
  const size_t vallen;
} Entry;

int entry_compare(const void *a, const void *b, void *udata);
uint64_t entry_hash_xxhash3(const void *a, uint64_t seed0, uint64_t seed1);
uint64_t entry_hash(const void *a, uint64_t seed0, uint64_t seed1);
void entry_free(void *a);

#endif