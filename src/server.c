
#include <asm-generic/errno-base.h>
#include <asm-generic/socket.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "include/logging.h"
#include "include/protocol.h"
#include "include/vector.h"
#include "include/vector_types.h"

#include "include/hashmap.h"

#define unlikely(expr) __builtin_expect(!!(expr), 0)
#define likely(expr) __builtin_expect(!!(expr), 1)

static struct hashmap *state = NULL;
static bool running = true;

typedef struct {
  const uint8_t *key;
  const size_t keylen;
  const uint8_t *val;
  const size_t vallen;
} Entry;

#define MAX_ARGC 8

typedef struct {
  size_t argc;
  size_t len;
  size_t offsets[MAX_ARGC];
  size_t lens[MAX_ARGC];
} CmdArgs;

static void fd_set_nb(int fd) {
  errno = 0;
  int flags = fcntl(fd, F_GETFL, 0);
  if (errno) {
    panic("fcntl error");
    return;
  }

  flags |= O_NONBLOCK;

  errno = 0;
  (void)fcntl(fd, F_SETFL, flags);
  if (errno) {
    panic("fcntl error");
  }
}

void conn_put(vector_Conn_ptr *conns, Conn *conn) {
  size_t capacity = capacity_vector_Conn_ptr(conns);
  printf("conn_put(%d)\n", conn->fd);
  if (capacity <= (size_t)conn->fd) {
    resize_vector_Conn_ptr(conns, conn->fd + 1);
  }

  size_t new_capacity = capacity_vector_Conn_ptr(conns);
  for (size_t i = capacity; i < new_capacity; i++) {
    conns->array[i] = NULL;
  }

  conns->array[conn->fd] = conn;
  // TODO: fix this - move it inside the vector logic or replace the vector with
  // a proper map
  conns->used = conns->size;
}

static uint64_t get_monotonic_usec() {
  struct timespec tv = {0, 0};
  clock_gettime(CLOCK_MONOTONIC, &tv);
  return tv.tv_sec * 1000000 + tv.tv_nsec / 1000;
}

int32_t accept_new_conn(vector_Conn_ptr *conns, int fd) {
  struct sockaddr_in client_addr = {};
  socklen_t socklen = sizeof(client_addr);
  int client_fd = accept(fd, (struct sockaddr *)&client_addr, &socklen);
  if (client_fd < 0) {
    warn("accept() error");
    return -1;
  }

  printf("accepted fd=%d\n", client_fd);

  fd_set_nb(client_fd);
  Conn *client = (Conn *)malloc(sizeof(Conn));
  memset(client, 0, sizeof(*client));
  if (!client) {
    close(client_fd);
    return -1;
  }

  client->fd = client_fd;
  client->state = REQUEST;
  client->recv_buf_size = 0;
  client->send_buf_size = 0;
  client->send_buf_sent = 0;

  client->idle_start = get_monotonic_usec();

  conn_put(conns, client);

  return client_fd;
}

bool try_flush_buffer(Conn *conn) {
  ssize_t rv = 0;
  do {
    size_t remaining = conn->send_buf_size - conn->send_buf_sent;
    rv = write(conn->fd, &conn->send_buf[conn->send_buf_sent], remaining);
  } while (rv < 0 && errno == EINTR);

  if (rv < 0 && errno == EAGAIN) {
    return false;
  }

  if (rv < 0) {
    warn("write() error");
    conn->state = END;
    return false;
  }

  conn->send_buf_sent += (size_t)rv;
  assert(conn->send_buf_sent <= conn->send_buf_size);

  if (conn->send_buf_sent == conn->send_buf_size) {
    // response was sent fully, reset state to request
    conn->state = REQUEST;
    conn->send_buf_sent = 0;
    conn->send_buf_size = 0;
    return false;
  }

  // try to flush again as we have remaining data in the write buffer
  return true;
}

void state_response(Conn *conn) {
  while (try_flush_buffer(conn)) {
  }
}

const uint8_t *strnstr(const uint8_t *haystack, const uint8_t *needle,
                       size_t haystack_len, size_t needle_len) {
  if (needle_len == 0) {
    return haystack;
  }

  if (haystack_len < needle_len) {
    return NULL;
  }

  size_t search_limit = haystack_len - needle_len + 1;
  for (size_t i = 0; i < search_limit; i++) {
    bool match = true;
    for (size_t j = 0; j < needle_len; j++) {
      if (haystack[i + j] != needle[j]) {
        match = false;
        break;
      }
    }
    if (match) {
      return haystack + i;
    }
  }
  return NULL;
}

bool strstarts(const uint8_t *str, const uint8_t *prefix, size_t prefixlen) {
  return strncmp((const char *)str, (const char *)prefix, prefixlen) == 0;
}

static const uint8_t CMD_ECHO[] = {'E', 'C', 'H', 'O'};
static const uint8_t CMD_PING[] = {'P', 'I', 'N', 'G'};
static const uint8_t CMD_GET[] = {'G', 'E', 'T'};
static const uint8_t CMD_SET[] = {'S', 'E', 'T'};
static const uint8_t CMD_DEL[] = {'D', 'E', 'L'};
static const uint8_t CMD_QUIT[] = {'Q', 'U', 'I', 'T'};
static const uint8_t CMD_SHUTDOWN[] = {'S', 'H', 'U', 'T', 'D', 'O', 'W', 'N'};
static const uint8_t CMD_FLUSHALL[] = {'F', 'L', 'U', 'S', 'H', 'A', 'L', 'L'};

static const uint8_t CRLF[] = {'\r', '\n'};

int entry_compare(const void *a, const void *b, void *udata) {
  (void)(udata);

  const Entry *ea = a;
  const Entry *eb = b;
  if (ea->keylen != eb->keylen) {
    return ea->keylen - eb->keylen;
  }
  return memcmp(ea->key, eb->key, ea->keylen);
}

uint64_t entry_hash(const void *a, uint64_t seed0, uint64_t seed1) {
  (void)(seed0);
  (void)(seed1);

  const Entry *ea = a;
  uint64_t hash = 0;
  for (size_t i = 0; i < ea->keylen; i++) {
    hash = hash * 31 + ea->key[i];
  }
  return hash;
}

void entry_free(void *a) {
  Entry *ea = a;
  if (ea->key) {
    free((void *)ea->key);
  }
  if (ea->val) {
    free((void *)ea->val);
  }
}

size_t parse_number(uint8_t **buf) {
  size_t result = 0;
  uint8_t *p = *buf;
  while (*p != '\r') {
    if (unlikely(*p < (uint8_t)'0' || *p > (uint8_t)'9')) {
      return (size_t)-1;
    }

    result = result * 10 + (*p - (uint8_t)'0');
    p++;
  }

  // skip over CRLF
  p += sizeof(CRLF);
  *buf = p;

  return result;
}

CmdArgs *parse_resp_request(Conn *conn) {
  CmdArgs *args = malloc(sizeof(CmdArgs));

  uint8_t *buf = conn->recv_buf;
  buf++;
  args->argc = parse_number(&buf);

  for (size_t i = 0; i < args->argc; i++) {
    if (*buf == '$') {
      buf++;
      size_t arglen = parse_number(&buf);
      args->lens[i] = arglen;
      args->offsets[i] = buf - conn->recv_buf;
      buf += arglen + sizeof(CRLF);
    } else {
      goto bail;
    }
  }

  args->len = buf - conn->recv_buf;

  return args;

bail:
  free(args);
  return NULL;
}

CmdArgs *parse_inline_request(Conn *conn) {
  CmdArgs *args = malloc(sizeof(CmdArgs));
  args->argc = 0;
  args->len = 0;

  size_t offset = 0;
  size_t len = 0;
  bool in_arg = false;
  bool crlf = false;
  size_t i = 0;
  for (i = 0; i < conn->recv_buf_size; i++) {
    if (conn->recv_buf[i] == ' ') {
      if (in_arg) {
        args->offsets[args->argc] = offset;
        args->lens[args->argc] = len;
        args->argc++;
        in_arg = false;
      }
    } else if (conn->recv_buf[i] == '\r') {
      if (in_arg) {
        args->offsets[args->argc] = offset;
        args->lens[args->argc] = len;
        args->argc++;
        in_arg = false;
      }
      crlf = true;
    } else if (conn->recv_buf[i] == '\n') {
      if (!crlf) {
        goto bail;
      }
      crlf = false;
      break;
    } else {
      if (!in_arg) {
        if (args->argc == MAX_ARGC) {
          goto bail;
        }
        offset = i;
        len = 0;
        in_arg = true;
      }
      len++;
    }
  }

  if (in_arg) {
    args->offsets[args->argc] = offset;
    args->lens[args->argc] = len;
    args->argc++;
  }

  args->len = i + 1;

  return args;

bail:
  free(args);
  return NULL;
}

void write_simple_error(Conn *conn, const char *prefix, const char *msg) {
  conn->send_buf_size =
      sprintf((char *)&conn->send_buf, "-%s %s\r\n", prefix, msg);
}

void write_simple_generic_error(Conn *conn, const char *msg) {
  write_simple_error(conn, "ERR", msg);
}

void write_simple_string(Conn *conn, const char *msg, size_t len) {
  uint8_t *buf = conn->send_buf;
  buf[0] = '+';
  buf++;
  memcpy(buf, msg, len);
  buf += len;
  memcpy(buf, CRLF, sizeof(CRLF));
  buf += 2;
  conn->send_buf_size = buf - conn->send_buf;
}

void write_bulk_string(Conn *conn, const uint8_t *data, size_t len) {
  size_t written = 0;
  written += (size_t)sprintf((char *)&conn->send_buf[written], "$%zu", len);
  memcpy(&conn->send_buf[written], CRLF, sizeof(CRLF));
  written += 2;
  memcpy(&conn->send_buf[written], data, len);
  written += len;
  memcpy(&conn->send_buf[written], CRLF, sizeof(CRLF));
  written += 2;
  conn->send_buf_size = written;
}

void write_null_bulk_string(Conn *conn) {
  conn->send_buf_size = sprintf((char *)&conn->send_buf, "$-1\r\n");
}

void write_integer(Conn *conn, int64_t val) {
  conn->send_buf_size = sprintf((char *)&conn->send_buf, ":%ld\r\n", val);
}

void handle_command(Conn *conn, CmdArgs *args) {
  const uint8_t *cmd = &conn->recv_buf[args->offsets[0]];
  const size_t cmdlen = args->lens[0];

  if (strnstr(cmd, CMD_ECHO, cmdlen, sizeof(CMD_ECHO))) {
    if (args->argc == 2) {
      const uint8_t *echo = &conn->recv_buf[args->offsets[1]];
      const size_t echolen = args->lens[1];
      write_bulk_string(conn, echo, echolen);
    } else {
      write_simple_generic_error(
          conn, "wrong number of arguments for 'echo' command");
    }
  } else if (strnstr(cmd, CMD_PING, cmdlen, sizeof(CMD_PING))) {
    write_simple_string(conn, "PONG", 4);
  } else if (strnstr(cmd, CMD_QUIT, cmdlen, sizeof(CMD_QUIT))) {
    conn->state = END;
  } else if (strnstr(cmd, CMD_GET, cmdlen, sizeof(CMD_GET))) {
    const uint8_t *key = &conn->recv_buf[args->offsets[1]];
    const size_t keylen = args->lens[1];

    Entry *entry =
        (Entry *)hashmap_get(state, &(Entry){.key = key, .keylen = keylen});
    if (entry) {
      write_bulk_string(conn, entry->val, entry->vallen);
    } else {
      write_null_bulk_string(conn);
    }
  } else if (strnstr(cmd, CMD_SET, cmdlen, sizeof(CMD_SET))) {
    const size_t keylen = args->lens[1];
    const uint8_t *key = (uint8_t *)malloc(keylen);
    memcpy((void *)key, &conn->recv_buf[args->offsets[1]], keylen);

    const size_t vallen = args->lens[2];
    const uint8_t *val = (uint8_t *)malloc(vallen);
    memcpy((void *)val, &conn->recv_buf[args->offsets[2]], vallen);

    Entry *entry =
        &(Entry){.key = key, .keylen = keylen, .val = val, .vallen = vallen};
    hashmap_set(state, entry);
    write_simple_string(conn, "OK", 2);
  } else if (strnstr(cmd, CMD_DEL, cmdlen, sizeof(CMD_DEL))) {
    const uint8_t *key = &conn->recv_buf[args->offsets[1]];
    const size_t keylen = args->lens[1];

    const void *entry = hashmap_delete(state, &(Entry){.key = (void *)key, .keylen = keylen});
    if (entry) {
      entry_free((void*)entry);
      write_integer(conn, 1);
    } else {
      write_integer(conn, 0);
    }
  } else if (strnstr(cmd, CMD_SHUTDOWN, cmdlen, sizeof(CMD_SHUTDOWN))) {
    running = false;
  } else if (strnstr(cmd, CMD_FLUSHALL, cmdlen, sizeof(CMD_FLUSHALL))) {
    hashmap_clear(state, true);
    write_simple_string(conn, "OK", 2);
  } else {
    char message[64];
    char *first_arg =
        args->argc > 1 ? (char *)&conn->recv_buf[args->offsets[1]] : "";
    size_t first_arg_len = args->argc > 1 ? args->lens[1] : 0;
    snprintf(message, sizeof(message),
             "unknown command '%.*s', with args beginning with: '%.*s'",
             (int)cmdlen, cmd, (int)first_arg_len, first_arg);
    write_simple_generic_error(conn, message);
  }
}

bool try_handle_request(Conn *conn) {
  if (unlikely(conn->recv_buf_size < 1)) {
    return false;
  }

  bool result = false;
  CmdArgs *args = NULL;
  if (likely(conn->recv_buf[0] == '*')) {
    args = parse_resp_request(conn);
  } else {
    args = parse_inline_request(conn);
  }

  if (unlikely(!args)) {
    conn->state = END;
    goto bail;
  }

  // for (size_t i = 0; i < args->argc; i++) {
  //   printf("Arg %zu: ", i);
  //   for (size_t j = 0; j < args->lens[i]; j++) {
  //     printf("%c", conn->recv_buf[args->offsets[i] + j]);
  //   }
  //   printf("\n");
  // }

  if (likely(args->argc > 0)) {
    handle_command(conn, args);
  }

  if (unlikely(conn->state == END)) {
    goto bail;
  }

  if (unlikely(conn->recv_buf_size < (args->len))) {
    goto bail;
  }

  size_t remaining = conn->recv_buf_size - args->len;
  if (remaining) {
    memmove(conn->recv_buf, &conn->recv_buf[args->len], remaining);
  }

  conn->recv_buf_size = remaining;

  conn->state = RESPONSE;
  state_response(conn);

  result = (conn->state == REQUEST);

bail:
  if (args) {
    free(args);
  }

  return result;
}

bool try_fill_buffer(Conn *conn) {
  assert(conn->recv_buf_size < sizeof(conn->recv_buf));

  ssize_t rv = 0;
  do {
    size_t cap = sizeof(conn->recv_buf) - conn->recv_buf_size;
    rv = read(conn->fd, &conn->recv_buf[conn->recv_buf_size], cap);
  } while (rv < 0 && errno == EINTR);

  if (rv < 0 && errno == EAGAIN) {
    // got EAGAIN, stop.
    return false;
  }

  if (rv < 0) {
    warn("read() error");
    conn->state = END;
    return false;
  }

  if (rv == 0) {
    if (conn->recv_buf_size > 0) {
      warn("unexpected EOF");
    } else {
      info("EOF");
    }

    conn->state = END;
    return false;
  }

  conn->recv_buf_size += (size_t)rv;
  assert(conn->recv_buf_size <= sizeof(conn->recv_buf));

  while (try_handle_request(conn)) {
  }

  bool test = (conn->state == END);
  return test;
}

void state_request(Conn *conn) {
  while (try_fill_buffer(conn)) {
  }
}

void handle_connection(Conn *conn) {
  conn->idle_start = get_monotonic_usec();
  if (conn->state == REQUEST) {
    state_request(conn);
  } else if (conn->state == RESPONSE) {
    state_response(conn);
  } else {
    assert(0);
  }
}

void conn_done(vector_Conn_ptr *conns, Conn *conn) {
  conns->array[conn->fd] = NULL;
  close(conn->fd);
  free(conn);
}

void epoll_register(int fd_epoll, int fd, uint32_t events) {
	struct epoll_event ev;
	ev.events = events;
	ev.data.fd = fd;
	if (epoll_ctl(fd_epoll, EPOLL_CTL_ADD, fd, &ev) == -1) {
		perror("epoll_ctl()\n");
		exit(1);
	}
}

void epoll_unregister(int fd_epoll, int fd) {
  struct epoll_event ev;
  ev.events = 0;
  ev.data.fd = fd;
  if (epoll_ctl(fd_epoll, EPOLL_CTL_DEL, fd, &ev) == -1) {
    perror("epoll_ctl()\n");
    exit(1);
  }
}

int main() {
  state = hashmap_new(sizeof(Entry), 0, 0, 0, entry_hash, entry_compare, entry_free,
                      NULL);

  vector_Conn_ptr conns;
  init_vector_Conn_ptr(&conns, 128);
  for (size_t i = 0; i < capacity_vector_Conn_ptr(&conns); i++) {
    conns.array[i] = NULL;
  }

  int fd_listener = socket(AF_INET, SOCK_STREAM, 0);
  int val = 1;
  setsockopt(fd_listener, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = ntohs(1337),
      .sin_addr.s_addr = ntohl(0),
  };

  int rv = bind(fd_listener, (struct sockaddr *)&addr, sizeof(addr));
  if (rv) {
    panic("bind()");
  }

  rv = listen(fd_listener, SOMAXCONN);
  if (rv) {
    panic("listen()");
  }

  fd_set_nb(fd_listener);

  printf("fd_listener=%d\n", fd_listener);

  int fd_epoll = epoll_create(1);
  printf("fd_epoll=%d\n", fd_epoll);
  epoll_register(fd_epoll, fd_listener, EPOLLIN);
  printf("listening\n");

  int nfds = 0;
  struct epoll_event events[128];
  while (running) {
    nfds = epoll_wait(fd_epoll, events, 128, -1);
    if (nfds == -1) {
      perror("epoll_wait()");
      exit(1);
    }

    for (int i = 0; i < nfds; i++) {
      if (events[i].data.fd == fd_listener) {
        int fd = accept_new_conn(&conns, fd_listener);
        if (fd < 0) {
          panic("accept_new_conn()");
        }
        epoll_register(fd_epoll, fd, EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLHUP);
      } else {
        struct epoll_event ev = events[i];
        int fd = ev.data.fd;
        Conn *conn = conns.array[fd];
        handle_connection(conn);
        if (conn->state == END || ev.events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
          printf("closing fd=%d\n", conn->fd);
          epoll_unregister(fd_epoll, fd);
          conn_done(&conns, conn);
        }
      }
    }

    // uint64_t now_us = get_monotonic_usec();
    // for (size_t i = 0; i < size_vector_Conn_ptr(&conns); i++) {
    //   Conn *conn = conns.array[i];
    //   if (!conn) {
    //     continue;
    //   }

    //   if (now_us - conn->idle_start > 60000000) {
    //     conn_done(&conns, conn);
    //   }
    // }
  }

  close(fd_listener);

  return 0;
}
