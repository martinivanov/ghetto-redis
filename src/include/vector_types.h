#ifndef VECTOR_TYPES_H
#define VECTOR_TYPES_H

#include <stdint.h>
#include <sys/poll.h>

#include "protocol.h"
#include "vector.h"

typedef struct pollfd pollfd;

VECTOR_TYPE_PTR(Conn);
VECTOR_TYPE(pollfd);

#endif