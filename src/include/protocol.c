#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "logging.h"
#include "protocol.h"

void write_simple_error(Conn *conn, const char *prefix, const char *msg) {
  uint8_t *buf = conn->send_buf + conn->send_buf_size;
  conn->send_buf_size += sprintf((char *)buf, "-%s %s\r\n", prefix, msg);
}

void write_simple_generic_error(Conn *conn, const char *msg) {
  write_simple_error(conn, "ERR", msg);
}

void write_simple_string(Conn *conn, const char *msg, size_t len) {
  uint8_t *buf = conn->send_buf + conn->send_buf_size;
  conn->send_buf_size += sprintf((char *)buf, "+%.*s\r\n", (int)len, msg);
}

void write_bulk_string(Conn *conn, const uint8_t *data, size_t len) {
  uint8_t *buf = conn->send_buf + conn->send_buf_size;
  size_t written = 0;
  written += (size_t)sprintf((char *)buf + written, "$%zu", len);
  memcpy(buf + written, CRLF, sizeof(CRLF));
  written += 2;
  memcpy(buf + written, data, len);
  written += len;
  memcpy(buf + written, CRLF, sizeof(CRLF));
  written += 2;
  conn->send_buf_size += written;
}

void write_null_bulk_string(Conn *conn) {
  uint8_t *buf = conn->send_buf + conn->send_buf_size;
  conn->send_buf_size += sprintf((char *)buf, "$-1\r\n");
}

void write_integer(Conn *conn, int64_t val) {
  uint8_t *buf = conn->send_buf + conn->send_buf_size;
  conn->send_buf_size += sprintf((char *)buf, ":%ld\r\n", val);
}

ParseError parse_number(uint8_t **cur, uint8_t *end, size_t *result) {
  size_t res = 0;
  uint8_t *p = *cur;
  bool complete = false;

  while (p <= end) {
    if (*p == '\r') {
      complete = true;
      break;
    }

    if (*p < (uint8_t)'0' || *p > (uint8_t)'9') {
      return PARSE_ERROR;
    }

    if (res > (SIZE_MAX - (*p - (uint8_t)'0')) / 10) {
      return PARSE_ERROR;
    }

    res = res * 10 + (*p - (uint8_t)'0');
    p++;
  }

  if (!complete) {
    return PARSE_INCOMPLETE;
  }

  if (p + sizeof(CRLF) > end) {
    return PARSE_INCOMPLETE;
  }

  // skip over CRLF
  p += sizeof(CRLF);
  *cur = p;

  *result = res;

  return PARSE_OK;
}

ParseError parse_resp_request(Conn *conn, CmdArgs *args) {
  uint8_t *cur = conn->recv_buf + conn->recv_buf_read;
  uint8_t *start = cur;
  uint8_t *end = cur + conn->recv_buf_size;

  if (start == end) {
    return PARSE_INCOMPLETE;
  }
  
  cur++;

  ParseError err = parse_number(&cur, end, &args->argc);
  if (err != PARSE_OK) {
    return err;
  }

  if (args->argc > MAX_ARGC) {
    return PARSE_ERROR;
  }

  if (cur + sizeof(CRLF) > end) {
    return PARSE_INCOMPLETE;
  }

  for (size_t i = 0; i < args->argc; i++) {
    if (cur == end) {
      return PARSE_INCOMPLETE;
    }

    if (*cur == '$') {
      cur++;
      size_t arglen = 0;
      ParseError err = parse_number(&cur, end, &arglen);
      if (err != PARSE_OK) {
        return err;
      }

      if (cur + arglen + sizeof(CRLF) > end) {
        return PARSE_INCOMPLETE;
      }

      args->lens[i] = arglen;
      args->offsets[i] = cur - start;
      cur += arglen + sizeof(CRLF);
    } else {
      return PARSE_ERROR;
    }
  }

  args->buf = start;
  args->len = cur - start;

  return PARSE_OK;
}

ParseError parse_inline_request(Conn *conn, CmdArgs *args) {
  args->argc = 0;
  args->len = 0;

  uint8_t *cur = conn->recv_buf + conn->recv_buf_read;
  args->buf = cur;
  size_t offset = 0;
  size_t len = 0;
  bool complete = false;
  bool in_arg = false;
  bool crlf = false;
  size_t i = 0;
  for (i = 0; i < conn->recv_buf_size; i++) {
    if (cur[i] == ' ') {
      if (in_arg) {
        args->offsets[args->argc] = offset;
        args->lens[args->argc] = len;
        args->argc++;
        in_arg = false;
      }
    } else if (cur[i] == '\r') {
      if (in_arg) {
        args->offsets[args->argc] = offset;
        args->lens[args->argc] = len;
        args->argc++;
        in_arg = false;
      }
      crlf = true;
    } else if (cur[i] == '\n') {
      if (!crlf) {
        return PARSE_ERROR;
      }
      complete = true;
      crlf = false;
      break;
    } else {
      if (!in_arg) {
        if (args->argc == MAX_ARGC) {
          return PARSE_ERROR;
        }
        offset = i;
        len = 0;
        in_arg = true;
      }
      len++;
    }
  }
  
  if (!complete) {
    return PARSE_INCOMPLETE;
  }

  if (in_arg) {
    args->offsets[args->argc] = offset;
    args->lens[args->argc] = len;
    args->argc++;
  }

  args->len = i + 1;

  return PARSE_OK;
}