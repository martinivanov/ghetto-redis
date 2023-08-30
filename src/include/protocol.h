#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdint.h>

#define MESSAGE_MAX_LENGTH 8192

enum State {
    REQUEST = 1 << 0,
    RESPONSE = 1 << 1,
    BLOCKED = 1 << 2,
    END = 1 << 31,
};

typedef struct {
    int fd;
    enum State state;

    size_t recv_buf_size;
    size_t recv_buf_read;
    uint8_t recv_buf[MESSAGE_MAX_LENGTH];

    size_t send_buf_size;
    size_t send_buf_sent;
    uint8_t send_buf[MESSAGE_MAX_LENGTH];

    uint64_t idle_start;
} Conn;

int32_t read_full(int fd, char *buf, size_t n);
int32_t write_all(int fd, const char *buf, size_t n);

#endif
