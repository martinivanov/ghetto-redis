
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
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "include/logging.h"
#include "include/protocol.h"
#include "include/vector.h"
#include "include/vector_types.h"

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
  if (size_vector_Conn_ptr(conns) <= (size_t)conn->fd) {
    resize_vector_Conn_ptr(conns, conn->fd + 1);
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

  return 0;
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

const char *strnstr(const char *haystack, const char *needle,
                    size_t haystack_len) {
  size_t needle_len = strlen(needle);

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

bool try_handle_request(Conn *conn) {
  // TODO: add a processed offset here instead of memmove?
  const char *crlf =
      strnstr((const char *)conn->recv_buf, "\r\n", conn->recv_buf_size);
  if (crlf == NULL) {
    return false;
  }

  size_t len = crlf - (char *)conn->recv_buf;

  if (strstarts(conn->recv_buf, CMD_ECHO, sizeof(CMD_ECHO))) {
    size_t echolen = len - sizeof(CMD_ECHO) + 1;
    memcpy(conn->send_buf, &conn->recv_buf[sizeof(CMD_ECHO) + 1], echolen);
    conn->send_buf_size = echolen;
  } else if (strstarts(conn->recv_buf, CMD_PING, sizeof(CMD_PING))) {
    static const char *PONG = "PONG\r\n";
    memcpy(conn->send_buf, PONG, sizeof(&PONG));
    conn->send_buf_size = sizeof(&PONG) - 1;
  } else if (strstarts(conn->recv_buf, CMD_GET, sizeof(CMD_GET))) {
    const char *args = (const char *)&conn->recv_buf[sizeof(CMD_GET) + 1];
    const size_t keylen = crlf - args;
    char *key = malloc(keylen + 1);
    memcpy(key, args, keylen);
    key[keylen] = '\0';
    printf("keylen=%zu key=%s\n", keylen, key);
  } else if (strstarts(conn->recv_buf, CMD_SET, sizeof(CMD_SET))) {
    const char *args = (const char *)&conn->recv_buf[sizeof(CMD_SET) + 1];
    const char *arg_delim = strnstr(args, " ", len - sizeof(CMD_SET) - 1);
    if (!arg_delim) {
      static const char *ERROR = "ERROR\r\n";
      memcpy(conn->send_buf, ERROR, sizeof(&ERROR));
      conn->send_buf_size = sizeof(&ERROR) - 1;
    } else {
      const size_t keylen = arg_delim - args;
      char *key = malloc(keylen + 1);
      memcpy(key, args, keylen);
      key[keylen] = '\0';
      const size_t vallen = crlf - arg_delim - 1;
      char *val = malloc(vallen + 1);
      memcpy(val, arg_delim + 1, vallen);
      val[vallen] = '\0';
      printf("keylen=%zu key=%s vallen=%zu val=%s\n", keylen, key, vallen, val);
    }
  } else if (strstarts(conn->recv_buf, CMD_DEL, sizeof(CMD_DEL))) {
    const char *args = (const char *)&conn->recv_buf[sizeof(CMD_DEL) + 1];
    const size_t keylen = crlf - args;
    char *key = malloc(keylen + 1);
    memcpy(key, args, keylen);
    key[keylen] = '\0';
    printf("keylen=%zu key=%s\n", keylen, key);
  } else if (strstarts(conn->recv_buf, CMD_QUIT, sizeof(CMD_QUIT))) {
    conn->state = END;
    return false;
  } else {
    // invalid command
  }

  // remove the request from the buffer.
  // note: frequent memmove is inefficient.
  // note: need better handling for production code.
  if (conn->recv_buf_size < (len + 2)) {
    return false;
  }

  size_t remaining = conn->recv_buf_size - len - 2;
  if (remaining) {
    memmove(conn->recv_buf, &conn->recv_buf[len + 2], remaining);
  }

  conn->recv_buf_size = remaining;

  conn->state = RESPONSE;
  state_response(conn);

  return (conn->state == REQUEST);
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

int main() {
  vector_Conn_ptr conns;
  init_vector_Conn_ptr(&conns, 128);

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  int val = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = ntohs(1337),
      .sin_addr.s_addr = ntohl(0),
  };

  int rv = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
  if (rv) {
    panic("bind()");
  }

  rv = listen(fd, SOMAXCONN);
  if (rv) {
    panic("listen()");
  }

  fd_set_nb(fd);

  vector_pollfd poll_args;
  init_vector_pollfd(&poll_args, 32);
  while (true) {
    // printf("Polling\n");
    clear_vector_pollfd(&poll_args);

    struct pollfd pfd = {fd, POLLIN, 0};
    insert_vector_pollfd(&poll_args, pfd);
    for (int i = 0; i < size_vector_Conn_ptr(&conns); i++) {
      Conn *conn = conns.array[i];
      if (!conn) {
        continue;
      }

      struct pollfd pfd = {
          .fd = conn->fd,
          .events = (conn->state == REQUEST) ? POLLIN : POLLOUT,
      };

      pfd.events |= POLLERR;
      insert_vector_pollfd(&poll_args, pfd);
    }

    int rv =
        poll(poll_args.array, (nfds_t)size_vector_pollfd(&poll_args), 1000);
    if (rv < 0) {
      panic("poll");
    }

    for (size_t i = 1; i < size_vector_pollfd(&poll_args); i++) {
      pollfd pfd = poll_args.array[i];
      if (pfd.revents) {
        Conn *conn = conns.array[pfd.fd];
        printf("handling connection %d\n", pfd.fd);
        handle_connection(conn);
        if (conn->state == END) {
          conn_done(&conns, conn);
        }
      }
    }

    uint64_t now_us = get_monotonic_usec();
    for (int i = 0; i < size_vector_Conn_ptr(&conns); i++) {
      Conn *conn = conns.array[i];
      if (!conn) {
        continue;
      }

      if (now_us - conn->idle_start > 5000000) {
        conn_done(&conns, conn);
      }
    }

    if (poll_args.array[0].revents) {
      accept_new_conn(&conns, fd);
    }
  }

  return 0;
}
