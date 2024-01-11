#define REACTOR_URING

#ifdef REACTOR_URING

// _GNU_SOURCE is needed for at* functions, see: https://stackoverflow.com/questions/5582211/what-does-define-gnu-source-imply
#define _GNU_SOURCE
#include <fcntl.h>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/eventfd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
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

// Based on:
// https://unixism.net/loti/what_is_io_uring.html
// https://github.com/axboe/liburing/wiki/io_uring-and-networking-in-2023#ring-messages
// https://nick-black.com/dankwiki/index.php/Io_uring

enum UringRequestType {
  ACCEPT,
  READ,
  WRITE,
  CLOSE,
  WAKEUP
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

typedef struct {
  enum UringRequestType type;
  Conn *conn;
} reactor_uring_write_request;

typedef struct {
  enum UringRequestType type;
  Conn *conn;
} reactor_uring_close_request;

typedef struct {
  enum UringRequestType type;
  uint64_t count;
} reactor_uring_wakeup_request;

void free_request(reactor_uring_request *req) {
  free(req);
}

void add_write_request(struct io_uring *ring, Conn *conn) {
  reactor_uring_write_request *req = malloc(sizeof(reactor_uring_write_request));
  req->type = WRITE;
  req->conn = conn;
  uint8_t *buf = &conn->send_buf[conn->send_buf_sent];
  size_t remaining = conn->send_buf_size - conn->send_buf_sent;
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_write(sqe, conn->fd, buf, remaining, 0);
  io_uring_sqe_set_data(sqe, req);
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
  io_uring_prep_multishot_accept(sqe, fd_listener, (struct sockaddr *)&req->addr, &req->addr_len, 0);
  io_uring_sqe_set_data(sqe, req);
}

void add_close_request(struct io_uring *ring, Conn *conn) {
  reactor_uring_close_request *req = malloc(sizeof(reactor_uring_close_request));
  req->type = CLOSE;
  req->conn = conn;
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_close(sqe, conn->fd);
  io_uring_sqe_set_data(sqe, req);
}

void add_wakeup_request(struct io_uring *ring, int fd, reactor_uring_wakeup_request *req) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_read(sqe, fd, &req->count, sizeof(req->count), 0);
  io_uring_sqe_set_data(sqe, req);
}

void reactor_uring_accepted(Reactor *reactor, struct io_uring_cqe *cqe) {
   struct io_uring *ring = (struct io_uring *)reactor->handle;
   reactor_uring_accept_request *req = (reactor_uring_accept_request *)cqe->user_data;
   int client_fd = cqe->res;
   if (client_fd < 0) {
     LOG_DEBUG("accept error: %d", client_fd);
     return;
   }

   LOG_DEBUG("accepted fd=%d", client_fd);

   Conn *conn = reactor->on_accept(reactor, req->addr, client_fd);

   if (conn != NULL) {
     reactor_conn_emplace(reactor, conn);
   }

   add_read_request(ring, conn);
}

void reactor_uring_data_read(Reactor *reactor, GRContext *context, struct io_uring_cqe *cqe) {
  struct io_uring *ring = (struct io_uring *)reactor->handle;
  reactor_uring_read_request *req = (reactor_uring_read_request *)cqe->user_data;
  Conn *conn = req->conn;

  if (cqe->res <= 0) {
    LOG_DEBUG("EOF");
    conn->flags |= END;
    free_request(req);
    add_close_request(ring, conn);
    return;
  }

  if (conn->recv_buf_read > 0) {
    memmove(conn->recv_buf, &conn->recv_buf[conn->recv_buf_read], conn->recv_buf_size);
    conn->recv_buf_read = 0;
  }

  conn->recv_buf_size += cqe->res;

  LOG_DEBUG("read %zu bytes into recv_buf_size=%zu, recv_buf_read=%zu", cqe->res, conn->recv_buf_size, conn->recv_buf_read);

  while (reactor->on_data_available(context, conn)) {
  }
  
  if (conn->flags & END) {
    free_request(req);
    add_close_request(ring, conn);
    return;
  }
  
  // size_t remaining_send = conn->send_buf_size - conn->send_buf_sent;
  // if (conn->send_buf_size > 0) {
  //   add_write_request(ring, conn);
  // }  

  add_read_request(ring, conn);

  free_request(req);
}

void reactor_uring_data_written(Reactor *reactor, GRContext *context, struct io_uring_cqe *cqe) {
  struct io_uring *ring = (struct io_uring *)reactor->handle;
  reactor_uring_write_request *req = (reactor_uring_write_request *)cqe->user_data;
  Conn *conn = req->conn;

  conn->send_buf_sent += cqe->res;

  size_t remaining = conn->send_buf_size - conn->send_buf_sent;

  if (remaining > 0) {
    LOG_DEBUG("more data available in the response buffer remaining=%zu", remaining);
    add_write_request(ring, conn);
  } else {
    LOG_DEBUG("response buffer fully flushed, resetting");
    conn->send_buf_size = 0;
    conn->send_buf_sent = 0;
  } 

  free_request(req);

  conn->flags &= ~BLOCKED;
}

void reactor_uring_woken_up(Reactor *reactor, GRContext *context, struct io_uring_cqe *cqe) {
  struct io_uring *ring = (struct io_uring *)reactor->handle;
  reactor_uring_wakeup_request *req = (reactor_uring_wakeup_request *)cqe->user_data;
  uint64_t count = reactor_poll_callbacks(reactor, context);


  atomic_store(&reactor->sleeping, true);

  add_wakeup_request(ring, reactor->wakeup_fd, req);
}

void reactor_uring_closed(Reactor *reactor, struct io_uring_cqe *cqe) {
  reactor_uring_close_request *req = (reactor_uring_close_request *)cqe->user_data;
  Conn *conn = req->conn;
  reactor->conns->array[conn->fd] = NULL;
  free(conn);
  free_request(req);
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

  reactor->listen_fd = fd_listener;
  reactor->handle = &ring;

  rv = io_uring_queue_init(128, &ring, 0);
  add_accept_request(&ring, fd_listener);

  reactor_uring_wakeup_request wakeup_req = {
    .type = WAKEUP
  };

  add_wakeup_request(&ring, reactor->wakeup_fd, &wakeup_req);

  io_uring_submit(&ring);

  struct io_uring_cqe *cqe;

  while (reactor->running) {
    int ret = io_uring_wait_cqe(&ring, &cqe);
    reactor_uring_request *req = (reactor_uring_request *)cqe->user_data;
    switch (req->type) {
      case ACCEPT: {
        LOG_DEBUG("ACCEPT");
        reactor_uring_accepted(reactor, cqe);
        break;
      }
      case READ: {
        LOG_DEBUG("READ");
        reactor_uring_data_read(reactor, context, cqe);
        break;
      }
      case WRITE: {
        LOG_DEBUG("WRITE");
        reactor_uring_data_written(reactor, context, cqe);
        break;
      }
      case CLOSE: {
        LOG_DEBUG("CLOSE");
        reactor_uring_closed(reactor, cqe);
        break;
      }
      case WAKEUP: {
        LOG_DEBUG("WAKEUP");
        reactor_uring_woken_up(reactor, context, cqe);
        break;
      }
    }

    io_uring_cqe_seen(&ring, cqe);

    for (size_t i = 0; i < size_vector_Conn_ptr(reactor->conns); i++) {
      Conn *conn = reactor->conns->array[i];
      if (conn) {
        size_t remaining = conn->send_buf_size - conn->send_buf_sent;
        if (remaining > 0 && !(conn->flags & BLOCKED)) {
          add_write_request(&ring, conn);
          conn->flags |= BLOCKED;
        }
      }
    }

    io_uring_submit(&ring);

    reactor_wakeup_pending(reactor, context);
  }

  size_t pending_close_requests = 0;
  for (size_t i = 0; i < capacity_vector_Conn_ptr(reactor->conns); i++) {
    Conn *conn = reactor->conns->array[i];
    if (conn) {
        add_close_request(&ring, conn);
        pending_close_requests++;
    }
  }

  io_uring_submit(&ring);

  do {
    int ret = io_uring_wait_cqe(&ring, &cqe);
    reactor_uring_request *req = (reactor_uring_request *)cqe->user_data;

    switch (req->type) {
      case CLOSE: {
        LOG_DEBUG("CLOSE");
        reactor_uring_closed(reactor, cqe);
        pending_close_requests--;
        break;
      }
    }
  
    io_uring_cqe_seen(&ring, cqe);
  } while (pending_close_requests > 0);
}

// bool reactor_uring_wakeup_pending(Reactor *reactor, GRContext *context) {
//   struct io_uring *ring = (struct io_uring *)reactor->handle;

//   ShardSet *shard_set = context->shard_set;
//   for (size_t i = 0; i < shard_set->size; i++) {
//     if (BITSET64_GET(reactor->soft_notify, i)) {
//       Shard *shard = &shard_set->shards[i];
//       Reactor *r = shard->reactor;
//       struct io_uring *target_ring = (struct io_uring *)r->handle;

//       int target_fd = target_ring->ring_fd;
//       if (atomic_exchange(&r->sleeping, false)) {
//         struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
//         reactor_uring_wakeup_request *req = malloc(sizeof(reactor_uring_wakeup_request));
//         req->type = WAKEUP;
//         io_uring_prep_msg_ring(sqe, target_fd, 0, 0, 0);
//         io_uring_sqe_set_data(sqe, req);
//       }
//     }
//   }

//   io_uring_submit(ring);

//   BITSET64_RESET(reactor->soft_notify);
// }

#endif // REACTOR_URING