#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "logging.h"

void info(const char *message) { printf("[INFO] %s\n", message); }

void warn(const char *message) { printf("[WARN] %s\n", message); }

void error(const char *message) { printf("[ERROR] %s\n", message); }

void panic(const char *message) {
  printf("[PANIC] [%d] %s\n", errno, message);
  abort();
}
