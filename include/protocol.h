#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

extern const size_t MESSAGE_MAX_LENGTH;
extern const size_t MESSAGE_HEADER_LENGTH;

int32_t read_full(int fd, char *buf, size_t n);
int32_t write_all(int fd, const char *buf, size_t n);

#endif
