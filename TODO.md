# General
1. hash table
1. io_uring
  * https://developers.redhat.com/articles/2023/04/12/why-you-should-use-iouring-network-io
1. allocators
  * http://ithare.com/testing-memory-allocators-ptmalloc2-tcmalloc-hoard-jemalloc-while-trying-to-simulate-real-world-loads/
  * https://github.com/jemalloc/jemalloc/
1. Better command parsing
  * https://github.com/dragonflydb/dragonfly/blob/9e36197742ce9baed19ecc750c9fe5a41e558e99/src/facade/redis_parser.cc#L21
1. Kernel bypass
  * https://github.com/Xilinx-CNS/onload
1. Reference counting for KVs in the hash table to avoid copying when calling back?
  * KV versioning for transactions - use a version number to know what value to use when executing a request?
  * something like RCU?