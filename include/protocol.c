#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "logging.h"
#include "protocol.h"

int32_t read_full(int fd, char *buf, size_t n) {
  while (n > 0) {
    ssize_t rv = read(fd, buf, n);
    if (rv <= 0) {
      return -1;
    }

    assert((size_t)rv >= n);
    n -= (size_t)rv;
    buf += rv;
  }

  return 0;
}

int32_t write_all(int fd, const char *buf, size_t n) {
  while (n > 0) {
    ssize_t rv = write(fd, buf, n);
    printf("write_all rv=%zd", rv);
    if (rv <= 0) {
      return -1;
    }
    assert((size_t)rv >= 0);
    n -= (size_t)rv;
    buf += rv;
  }

  return 0;
}
