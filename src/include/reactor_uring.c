#define REACTOR_URING

#ifdef REACTOR_URING

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/eventfd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <assert.h>
#include <liburing.h>

#include <asm-generic/errno-base.h>
#include <asm-generic/socket.h>

#include "utils.h"
#include "reactor.h"

enum UringRequestType {
  ACCEPT,
  READ,
  WRITE,
  CLOSE,
};

// TODO: split struct into accept, read, write, close
typedef struct {
    enum UringRequestType type;
} reactor_uring_request;

typedef struct {
  enum UringRequestType type;
  struct sockaddr_in addr;
  socklen_t addr_len;
} reactor_uring_accept_request;

typedef struct {
  enum UringRequestType type;
  Conn *conn;
} reactor_uring_read_request;

void free_request(reactor_uring_request *req) {
  free(req);
}

void add_read_request(struct io_uring *ring, Conn *conn) {
  reactor_uring_read_request *req = malloc(sizeof(reactor_uring_read_request));
  req->type = READ;
  req->conn = conn;
  uint8_t *buf = conn->recv_buf + conn->recv_buf_size;
  size_t buf_len = sizeof(conn->recv_buf) - conn->recv_buf_size;
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_read(sqe, conn->fd, buf, buf_len, 0);
  io_uring_sqe_set_data(sqe, req);
}

void add_accept_request(struct io_uring *ring, int fd_listener) {
  reactor_uring_accept_request *req = malloc(sizeof(reactor_uring_accept_request));
  req->type = ACCEPT;
  req->addr_len = sizeof(req->addr);
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  // TODO: try to use multishot accept
  io_uring_prep_accept(sqe, fd_listener, (struct sockaddr *)&req->addr, &req->addr_len, 0);
  io_uring_sqe_set_data(sqe, req);
}

Conn *reactor_uring_accept(Reactor *reactor, struct io_uring_cqe *cqe) {
   reactor_uring_accept_request *req = (reactor_uring_accept_request *)cqe->user_data;
    int client_fd = cqe->res;
    if (client_fd < 0) {
      LOG_DEBUG("accept error: %d", client_fd);
      return NULL;
    }

    LOG_DEBUG("accepted fd=%d", client_fd);

    Conn *conn = reactor->on_accept(reactor, req->addr, client_fd);

    if (conn != NULL) {
      reactor_conn_emplace(reactor, conn);
    }

    free_request(req); 

    return conn;
}

void reactor_run(Reactor *reactor, GRContext *context) {
  LOG_DEBUG("starting reactor %zu", reactor->id);
  LOG_DEBUG("reactor->wakeup_fd=%d", reactor->wakeup_fd);
  int fd_listener = socket(AF_INET, SOCK_STREAM, 0);
  int val = 1;
  setsockopt(fd_listener, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val));
  
  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = ntohs(1337),
      .sin_addr.s_addr = ntohl(0),
  };

  int rv = bind(fd_listener, (struct sockaddr *)&addr, sizeof(addr));
  if (rv) {
    LOG_DEBUG("errno=%d", errno);
    panic("bind()");
  }

  rv = listen(fd_listener, SOMAXCONN);
  if (rv) {
    panic("listen()");
  }

  set_fd_options(fd_listener, O_NONBLOCK);

  struct io_uring ring;

  rv = io_uring_queue_init(128, &ring, 0);
  add_accept_request(&ring, fd_listener);
  io_uring_submit(&ring);
  struct io_uring_cqe *cqe;

  while (reactor->running) {
    int ret = io_uring_wait_cqe(&ring, &cqe);
    reactor_uring_request *req = (reactor_uring_request *)cqe->user_data;

    switch (req->type) {
      case ACCEPT: {
        LOG_DEBUG("ACCEPT");
        Conn *conn = reactor_uring_accept(reactor, cqe);
        add_accept_request(&ring, fd_listener);
        add_read_request(&ring, conn);
        io_uring_submit(&ring);
        break;
      }
      case READ: {
        LOG_DEBUG("READ");
        reactor_uring_read_request *read_req = (reactor_uring_read_request *)cqe->user_data;
        Conn *conn = read_req->conn;
        conn->recv_buf_size += cqe->res;

        printf("accumulated recv_buf %.*s\n", (int)conn->recv_buf_size, conn->recv_buf);

        add_read_request(&ring, conn);
        io_uring_submit(&ring);
        // free_request(req);
        break;
      }
      case WRITE: {
        LOG_DEBUG("WRITE");
        break;
      }
      case CLOSE: {
        LOG_DEBUG("CLOSE");
        break;
      }
    }
  
    io_uring_cqe_seen(&ring, cqe);
  }
}

bool reactor_wakeup_pending(Reactor *reactor, GRContext *context) {
    return true;
}

#endif // REACTOR_URING