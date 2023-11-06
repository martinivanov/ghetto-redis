#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

#include "deque.h"

#define MESSAGE_MAX_LENGTH 8192
#define MAX_ARGC 64
#define MAX_PIPELINE_REQUESTS 128

struct CBContext;

enum State {
    REQUEST = 1 << 0,
    RESPONSE = 1 << 1,
    BLOCKED = 1 << 2,
    DISPATCH_WAITING = 1 << 3,
    PIPELINE = 1 << 4,
    PIPELINE_INCOMPLETE = 1 << 5,
    END = 1 << 31,
};

typedef enum {
  PARSE_OK,
  PARSE_ERROR,
  PARSE_ERROR_INVALID_ARGC,
  PARSE_INCOMPLETE,
  PARSE_EOF,
} ParseError;

typedef struct {
  uint8_t *buf;
  size_t pipeline_idx;
  size_t argc;
  size_t len;
  size_t offsets[MAX_ARGC];
  size_t lens[MAX_ARGC];
} CmdArgs;

static const uint8_t CRLF[] = {'\r', '\n'};

typedef struct {
    int fd;
    enum State state;
    size_t shard_id;

    struct sockaddr_in addr;

    size_t db;

    size_t recv_buf_size;
    size_t recv_buf_read;
    uint8_t recv_buf[MESSAGE_MAX_LENGTH];

    size_t send_buf_size;
    size_t send_buf_sent;
    uint8_t send_buf[MESSAGE_MAX_LENGTH];

    uint64_t idle_start;

    DequeNode *pending_writes_queue_node;
    DequeNode *idle_conn_queue_node;

    size_t pipeline_req_count;
    CmdArgs *pipeline_reqs[MAX_PIPELINE_REQUESTS];
    
    size_t pipeline_resp_count;
    struct CBContext *pipeline_resps[MAX_PIPELINE_REQUESTS];
} Conn;

void write_simple_error(Conn *conn, const char *prefix, const char *msg);
void write_simple_generic_error(Conn *conn, const char *msg);
void write_simple_string(Conn *conn, const char *msg, size_t len);
void write_bulk_string(Conn *conn, const uint8_t *data, size_t len);
void write_null_bulk_string(Conn *conn);
void write_integer(Conn *conn, int64_t val);
void write_array_header(Conn *conn, size_t len);

ParseError parse_number(uint8_t **cur, uint8_t *end, size_t *result);
ParseError parse_resp_request(Conn *conn, CmdArgs *args);
ParseError parse_inline_request(Conn *conn, CmdArgs *args);

#endif
