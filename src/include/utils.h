#ifndef UTILS_H
#define UTILS_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

#include "logging.h"

static void set_fd_options(int fd, int flags) {
  errno = 0;
  int old_flags = fcntl(fd, F_GETFL, 0);
  if (errno) {
    panic("fcntl error");
    return;
  }

  old_flags |= flags;

  errno = 0;
  (void)fcntl(fd, F_SETFL, old_flags);
  if (errno) {
    panic("fcntl error");
  }
}

#endif // UTILS_H