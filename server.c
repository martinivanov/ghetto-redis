#include <arpa/inet.h>
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
#include <unistd.h>

#include "include/logging.h"
#include "include/protocol.h"
#include "include/vector.h"
#include "include/vector_types.h"

int32_t handle_request(int fd) {
  char recv_buf[MESSAGE_HEADER_LENGTH + MESSAGE_MAX_LENGTH + 1];
  errno = 0;
  int32_t err = read_full(fd, recv_buf, MESSAGE_HEADER_LENGTH);
  if (err) {
    if (errno == 0) {
      info("EOF");
    } else {
      warn("read() error");
    }
  }

  uint32_t len = 0;
  memcpy(&len, recv_buf, MESSAGE_HEADER_LENGTH);
  if (len > MESSAGE_MAX_LENGTH) {
    warn("message too long");
    return -1;
  }

  err = read_full(fd, &recv_buf[MESSAGE_HEADER_LENGTH], len); // skip the header
  if (err) {
    warn("read() error");
    return -1;
  }

  recv_buf[MESSAGE_HEADER_LENGTH + len] = '\0';
  printf("client says: '%s' with length %d\n", &recv_buf[MESSAGE_HEADER_LENGTH],
         len);

  uint32_t send_len = strlen(&recv_buf[MESSAGE_HEADER_LENGTH]);
  char send_buf[MESSAGE_HEADER_LENGTH + send_len];
  memcpy(send_buf, &send_len, MESSAGE_HEADER_LENGTH);
  strcpy(&send_buf[MESSAGE_HEADER_LENGTH], &recv_buf[MESSAGE_HEADER_LENGTH]);
  printf("server will respond with: '%s' with length %d\n",
         &send_buf[MESSAGE_HEADER_LENGTH], send_len);

  return write_all(fd, send_buf, MESSAGE_HEADER_LENGTH + send_len);
}

void handle(int fd) {
  while (true) {
    int32_t err = handle_request(fd);
    if (err) {
      break;
    }
  }
}

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
  // TODO: fix this - move it inside the vector logic or replace the vector with a proper map
  conns->used = conns->size;
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
  if (!client) {
    close(client_fd);
    return -1;
  }

  client->fd = client_fd;
  client->state = REQUEST;
  client->recv_buf_size = 0;
  client->send_buf_size = 0;
  client->send_buf_sent = 0;

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

bool try_handle_request(Conn *conn) {
  if (conn->recv_buf_size < MESSAGE_HEADER_LENGTH) {
    // not enough data in the buffer. Will retry in the next iteration
    return false;
  }

  uint32_t len = 0;
  memcpy(&len, &conn->recv_buf[0], MESSAGE_HEADER_LENGTH);
  if (len > MESSAGE_MAX_LENGTH) {
    warn("message too long");
    conn->state = END;
    return false;
  }

  if (MESSAGE_HEADER_LENGTH + len > conn->recv_buf_size) {
    return false;
  }

  // echo back
  memcpy(&conn->send_buf[0], &len, MESSAGE_HEADER_LENGTH);
  memcpy(&conn->send_buf[MESSAGE_HEADER_LENGTH],
         &conn->recv_buf[MESSAGE_HEADER_LENGTH], len);
  conn->send_buf_size = MESSAGE_HEADER_LENGTH + len;

  // remove the request from the buffer.
  // note: frequent memmove is inefficient.
  // note: need better handling for production code.
  size_t remaining = conn->recv_buf_size - MESSAGE_HEADER_LENGTH - len;
  if (remaining) {
    memmove(conn->recv_buf, &conn->recv_buf[MESSAGE_HEADER_LENGTH + len],
            remaining);
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
  assert(conn->recv_buf_size < sizeof(conn->recv_buf) - conn->recv_buf_size);

  while (try_handle_request(conn)) {
  }

  return (conn->state == END);
}

void state_request(Conn *conn) {
  while (try_fill_buffer(conn)) {
  }
}

void handle_connection(Conn *conn) {
  if (conn->state == REQUEST) {
    state_request(conn);
  } else if (conn->state == RESPONSE) {
    state_response(conn);
  } else {
    assert(0);
  }
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
    printf("Polling\n");
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
          conns.array[conn->fd] = NULL;
          close(conn->fd);
          free(conn);
        }
      }
    }

    if (poll_args.array[0].revents) {
      accept_new_conn(&conns, fd);
    }
  }

  return 0;
}
