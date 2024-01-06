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

#include <asm-generic/errno-base.h>
#include <asm-generic/socket.h>

#include "reactor.h"
#include "state.h"
#include "logging.h"
#include "protocol.h"
#include "vector_types.h"
#include "mpmcq.h"
#include "utils.h"

int32_t reactor_epoll_accept(Reactor *reactor, int fd_listener);
bool reactor_epoll_try_read(Reactor *reactor, Conn *conn);
void reactor_epoll_on_epollin(Reactor *reactor, Conn *conn, GRContext *context);
void reactor_epoll_flush(Conn *conn);
void reactor_epoll_close(Reactor *reactor, Conn *conn);

void epoll_register(int fd_epoll, int fd, uint32_t events) {
	struct epoll_event ev;
	ev.events = events;
	ev.data.fd = fd;
	if (epoll_ctl(fd_epoll, EPOLL_CTL_ADD, fd, &ev) == -1) {
		panic("epoll_ctl()");
	}
}

void epoll_unregister(int fd_epoll, int fd) {
  struct epoll_event ev;
  ev.events = 0;
  ev.data.fd = fd;
  if (epoll_ctl(fd_epoll, EPOLL_CTL_DEL, fd, &ev) == -1) {
		panic("epoll_ctl()");
  }
}

void epoll_modify(int fd_epoll, int fd, uint32_t events) {
  struct epoll_event ev;
  ev.events = events;
  ev.data.fd = fd;
  if (epoll_ctl(fd_epoll, EPOLL_CTL_MOD, fd, &ev) == -1) {
		panic("epoll_ctl()");
  }
}

void reactor_init(
    Reactor *reactor, 
    size_t id, 
    reactor_on_cb on_cb,
    reactor_on_accept on_accept,
    reactor_on_data_available on_data_available
) {
    reactor->id = id;
    reactor->running = true;

    reactor->cb_queue = malloc(sizeof(mpmcq));
    mpmcq_init(reactor->cb_queue, 8192);

    reactor->sleeping = false;
    reactor->soft_notify = 0;

    reactor->conns = (vector_Conn_ptr *)malloc(sizeof(vector_Conn_ptr));
    init_vector_Conn_ptr(reactor->conns, 1024);

    reactor->on_cb = on_cb;
    reactor->on_accept = on_accept;
    reactor->on_data_available = on_data_available;

    reactor->wakeup_fd = eventfd(0, 0); // or eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK); //TODO: check what the options are
    if (reactor->wakeup_fd == -1) {
        perror("eventfd");
        exit(1);
    }
}

void reactor_destroy(Reactor *reactor) {
    mpscq_destroy(reactor->cb_queue);
    free_vector_Conn_ptr(reactor->conns);
}

void reactor_flush_pending_writes(Reactor *reactor) {
  for (size_t i = 0; i < capacity_vector_Conn_ptr(reactor->conns); i++) {
    Conn *conn = reactor->conns->array[i];
    if (conn) {
      while (conn->send_buf_size > 0) {
        reactor_epoll_flush(conn);
      }
    }
  }
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

  LOG_DEBUG("fd_listener=%d", fd_listener);

  int fd_epoll = epoll_create(1);
  LOG_DEBUG("fd_epoll=%d", fd_epoll);
  epoll_register(fd_epoll, fd_listener, EPOLLIN);
  LOG_DEBUG("listening");

  epoll_register(fd_epoll, reactor->wakeup_fd, EPOLLIN);

  int nfds = 0;
  struct epoll_event events[128];
  int timeout = -1;
  while (reactor->running) {
    reactor_flush_pending_writes(reactor);

    reactor_wakeup_pending(reactor, context);

    size_t cb_count = reactor_poll_callbacks(reactor, context); 
    if (cb_count > 0) {
      nfds = epoll_wait(fd_epoll, events, 128, 0);
    } else {
      atomic_store(&reactor->sleeping, true);
      if (reactor_has_pending_messages(reactor)) {
        atomic_store(&reactor->sleeping, false);
        continue;
      }

      nfds = epoll_wait(fd_epoll, events, 128, timeout);
      atomic_store(&reactor->sleeping, false);
    }

    if (nfds == -1 && errno == EINTR) {
      continue;
    }

    if (nfds < 0) {
      perror("epoll_wait()");
      exit(1);
    }

    for (int i = 0; i < nfds; i++) {
      if (events[i].data.fd == fd_listener) {
        int fd = reactor_epoll_accept(reactor, fd_listener);
        if (fd < 0) {
          panic("accept_new_conn()");
        }
        epoll_register(fd_epoll, fd, EPOLLIN | EPOLLERR | EPOLLRDHUP | EPOLLHUP);
      } else if (events[i].data.fd == reactor->wakeup_fd) {
        uint64_t val = 0; 
        read(reactor->wakeup_fd, &val, sizeof(val));
      } else {
        struct epoll_event ev = events[i];
        int fd = ev.data.fd;
        Conn *conn = reactor->conns->array[fd];

        // TODO: add idle connection handling
        //conn->idle_start = get_monotonic_usec();
        // deque_move_to_back(&shard->idle_conn_queue, conn->idle_conn_queue_node);
        if (ev.events & EPOLLIN) {
          reactor_epoll_on_epollin(reactor, conn, context);
        } 
        
        if (ev.events & EPOLLOUT) {
          reactor_epoll_flush(conn);
          if (!(conn->flags & BLOCKED)) {
            epoll_modify(fd_epoll, fd, EPOLLIN | EPOLLERR | EPOLLRDHUP | EPOLLHUP);
          }
        }

        if ((conn->flags & END) || ev.events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
          epoll_unregister(fd_epoll, fd);
          reactor_epoll_close(reactor, conn);
        } else {
          if (conn->flags & BLOCKED) {
            LOG_WARN("conn->state & BLOCKED");
            conn->flags &= ~BLOCKED;
            epoll_modify(fd_epoll, fd, EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLRDHUP | EPOLLHUP);
          }
        }
      }
    }

    // timeout = close_idle_connections(shard);
    timeout = -1;
  }

  close(fd_listener);

  for (size_t i = 0; i < capacity_vector_Conn_ptr(reactor->conns); i++) {
    Conn *conn = reactor->conns->array[i];
    if (conn) {
      reactor_epoll_close(reactor, conn);
    }
  }
}

bool reactor_wakeup_pending(Reactor *reactor, GRContext *context) {
  bool notifed = false;
  size_t shard_count = context->shard_set->size;
  ShardSet *shard_set = context->shard_set;
  for (int i = 0; i < shard_count; i++) {
    if (BITSET64_GET(reactor->soft_notify, i)) {
      Shard *shard = &shard_set->shards[i];
      Reactor *r = shard->reactor;
      if (atomic_exchange(&r->sleeping, false)) {
        write(r->wakeup_fd, &(uint64_t){1}, sizeof(uint64_t));
        notifed = true;
      }
    }
  }

  // reset the mask
  BITSET64_RESET(reactor->soft_notify);

  return notifed;
}

int32_t reactor_epoll_accept(Reactor *reactor, int fd_listener) {
  struct sockaddr_in client_addr = {};
  socklen_t socklen = sizeof(client_addr);
  int client_fd = accept(fd_listener, (struct sockaddr *)&client_addr, &socklen);
  if (client_fd < 0) {
    LOG_WARN("accept() error");
    return -1;
  }

  LOG_DEBUG("accepted fd=%d", client_fd);

  Conn *conn = reactor->on_accept(reactor, client_addr, client_fd);
  
  if (conn != NULL) {
    reactor_conn_emplace(reactor, conn);
  }

  return client_fd;
}

void reactor_epoll_on_epollin(Reactor *reactor, Conn *conn, GRContext *context) {
  assert(conn->recv_buf_size < sizeof(conn->recv_buf));

  ssize_t rv = 0;
  do {
    size_t cap = sizeof(conn->recv_buf) - conn->recv_buf_size;
    if (conn->recv_buf_read > 0) {
      memmove(conn->recv_buf, &conn->recv_buf[conn->recv_buf_read], conn->recv_buf_size);
      conn->recv_buf_read = 0;
    }
    rv = read(conn->fd, &conn->recv_buf[conn->recv_buf_size], cap);
  } while (rv < 0 && errno == EINTR);

  if (rv < 0 && errno == EAGAIN) {
    // got EAGAIN, stop.
    return;
  }

  if (rv < 0) {
    LOG_WARN("read() error");
    conn->flags |= END;
    return;
  }

  if (rv == 0) {
    if (conn->recv_buf_size > 0) {
      LOG_WARN("unexpected EOF");
    } else {
      LOG_INFO("EOF");
    }

    conn->flags |= END;
    return;
  }

  conn->recv_buf_size += (size_t)rv;
  assert(conn->recv_buf_size <= sizeof(conn->recv_buf));

  if (conn->flags & DISPATCH_WAITING) {
    return;
  }  

  while (reactor->on_data_available(context, conn)) {
  }
}

void reactor_epoll_flush(Conn *conn) {
  size_t rv = 0;
  do {
    size_t remaining = conn->send_buf_size - conn->send_buf_sent;

    LOG_DEBUG_WITH_CTX(conn->shard_id, "conn->send_buf_size=%zu conn->send_buf_sent=%zu remaining=%zu", conn->send_buf_size, conn->send_buf_sent, remaining);
    LOG_DEBUG_WITH_CTX(conn->shard_id, "conn->send_buf:%.*s", (int)MESSAGE_MAX_LENGTH, conn->send_buf);

    rv = write(conn->fd, &conn->send_buf[conn->send_buf_sent], remaining);
  } while (rv < 0 && errno == EINTR);

  if (rv < 0 && errno == EAGAIN) {
    conn->flags |= BLOCKED;
    return;
  }

  if (rv < 0) {
    LOG_WARN("write() error");
    conn->flags = END;
    return;
  }

  conn->send_buf_sent += (size_t)rv;

  if (conn->send_buf_sent > conn->send_buf_size) {
    LOG_DEBUG("conn->send_buf_sent=%zu conn->send_buf_size=%zu", conn->send_buf_sent, conn->send_buf_size);
    assert(conn->send_buf_sent <= conn->send_buf_size);
    panic("send_buf_sent > send_buf_size");
  }

  if (conn->send_buf_sent == conn->send_buf_size) {
    LOG_DEBUG("Response sent fully --- conn->send_buf_sent=%zu conn->send_buf_size=%zu", conn->send_buf_sent, conn->send_buf_size);
    conn->send_buf_sent = 0;
    conn->send_buf_size = 0;
    return;
  }
}

void reactor_epoll_close(Reactor *reactor, Conn *conn) {
  close(conn->fd);
  //deque_detach(&shard->idle_conn_queue, conn->idle_conn_queue_node);
  //free(conn->idle_conn_queue_node);
  reactor->conns->array[conn->fd] = NULL;
  free(conn);
  conn = NULL;
}