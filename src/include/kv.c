#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

#include "hashmap.h"
#include "kv.h"

int entry_compare(const void *a, const void *b, void *udata) {
  (void)(udata);

  const Entry *ea = a;
  const Entry *eb = b;
  if (ea->keylen != eb->keylen) {
    return ea->keylen - eb->keylen;
  }
  return memcmp(ea->key, eb->key, ea->keylen);
}

uint64_t entry_hash_xxhash3(const void *a, uint64_t seed0, uint64_t seed1) {
  const Entry *ea = a;
  return hashmap_xxhash3(ea->key, ea->keylen, seed0, seed1);
}

uint64_t entry_hash(const void *a, uint64_t seed0, uint64_t seed1) {
  (void)(seed0);
  (void)(seed1);

  const Entry *ea = a;
  uint64_t hash = 0;
  for (size_t i = 0; i < ea->keylen; i++) {
    hash = hash * 31 + ea->key[i];
  }
  return hash;
}

void entry_free(void *a) {
  Entry *ea = a;
  if (ea->key) {
    free((void *)ea->key);
  }
  if (ea->val) {
    free((void *)ea->val);
  }
}