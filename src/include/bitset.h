#ifndef BITSET_H
#define BITSET_H

#include <stdint.h>

typedef uint64_t bitset64;

#define BITSET64_SIZE(bitset) (sizeof(bitset) * 8)
#define BITSET64_SET(bitset, bit) ((bitset) |= (1 << (bit)))
#define BITSET64_CLEAR(bitset, bit) ((bitset) &= ~(1 << (bit)))
#define BITSET64_GET(bitset, bit) ((bitset) & (1 << (bit)))
#define BITSET64_RESET(bitset) ((bitset) = 0)
#define BITSET64_EACH(bitset, bit) for (bit = 0; bit < BITSET_SIZE(bitset); bit++) if (BITSET64_GET(bitset, bit))

#endif // BITSET_H