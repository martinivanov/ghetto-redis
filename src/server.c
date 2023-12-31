#define _GNU_SOURCE

#include <asm-generic/errno-base.h>
#include <asm-generic/socket.h>
#include <assert.h>
#include <errno.h>
#include <sys/eventfd.h>
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
#include <pthread.h>
#include <sched.h>

#include "include/logging.h"
#include "include/protocol.h"
#include "include/vector.h"
#include "include/vector_types.h"

#include "include/hashmap.h"
#include "include/deque.h"
#include "include/state.h"
#include "include/commands.h"
#include "include/kv.h"
#include "include/spscq.h"

#define _GNU_SOURCE

#define unlikely(expr) __builtin_expect(!!(expr), 0)
#define likely(expr) __builtin_expect(!!(expr), 1)

static uint64_t get_monotonic_usec() {
  struct timespec tv = {0, 0};
  clock_gettime(CLOCK_MONOTONIC, &tv);
  return tv.tv_sec * 1000000 + tv.tv_nsec / 1000;
}

void init_shards(GRState *gr_state) {
  Shard *shards = gr_state->shards;
  for (size_t i = 0; i < gr_state->num_shards; i++) {
    Shard *shard = &shards[i];
    shard->shard_id = i;
    shard->conns = (vector_Conn_ptr *)malloc(sizeof(vector_Conn_ptr));
    init_vector_Conn_ptr(shard->conns, 128);
    for (size_t j = 0; j < capacity_vector_Conn_ptr(shard->conns); j++) {
      shard->conns->array[j] = NULL;
    }

    deque_init(&shard->idle_conn_queue);
    deque_init(&shard->pending_writes_queue);

    int queue_efd = eventfd(0, 0);
    if (queue_efd == -1) {
      perror("eventfd()");
      exit(1);
    }

    shard->queue_efd = queue_efd;
    shard->cb_queues = (struct spscq **)malloc(sizeof(struct spscq *) * gr_state->num_shards);
    for (size_t j = 0; j < gr_state->num_shards; j++) {
      shard->cb_queues[j] = spscq_create(NULL, 100000);
    }

    uint64_t seed = get_monotonic_usec(NULL);
    shard->dbs = (struct hashmap **)malloc(sizeof(struct hashmap *) * gr_state->num_dbs);
    for (size_t j = 0; j < gr_state->num_dbs; j++) {
      shard->dbs[j] = hashmap_new(sizeof(Entry), 1 << 20, seed, seed, entry_hash_xxhash3, entry_compare, entry_free, NULL);
    }

    shard->gr_state = gr_state;

    shard->stats = (ShardStats){0};
  }
}

void free_server_state(GRState *gr_state) {
  for (size_t i = 0; i < gr_state->num_shards; i++) {
    Shard *shard = &gr_state->shards[i];
    for (size_t j = 0; j < gr_state->num_dbs; j++) {
      hashmap_free(shard->dbs[j]);
    }
    free(shard->dbs);
    free_vector_Conn_ptr(shard->conns);
    free(shard->conns);
    for (size_t j = 0; j < gr_state->num_shards; j++) {
      spscq_destroy(shard->cb_queues[j]);
    }
    free(shard->cb_queues);
    close(shard->queue_efd);
  }
  hashmap_free(gr_state->commands);
}

static void fd_set_nb(int fd) {
  errno = 0;
  int flags = fcntl(fd, F_GETFL, 0);
  if (errno) {
    panic("fcntl error");
    return;
  }

  flags |= O_NONBLOCK;
  //TODO: extract this into a separate function
  flags |= O_NDELAY;

  errno = 0;
  (void)fcntl(fd, F_SETFL, flags);
  if (errno) {
    panic("fcntl error");
  }
}

void conn_put(vector_Conn_ptr *conns, Conn *conn, size_t shard_id) {
  size_t capacity = capacity_vector_Conn_ptr(conns);
  LOG_DEBUG("shard_id=%zu conn_put(%d)", shard_id, conn->fd);
  if (capacity <= (size_t)conn->fd) {
    resize_vector_Conn_ptr(conns, conn->fd + 1);
  }

  size_t new_capacity = capacity_vector_Conn_ptr(conns);
  for (size_t i = capacity; i < new_capacity; i++) {
    conns->array[i] = NULL;
  }

  conns->array[conn->fd] = conn;
  conns->used = conns->size;
}

int32_t accept_new_conn(Shard *shard, int fd_listener) {
  struct sockaddr_in client_addr = {};
  socklen_t socklen = sizeof(client_addr);
  int client_fd = accept(fd_listener, (struct sockaddr *)&client_addr, &socklen);
  if (client_fd < 0) {
    LOG_WARN("accept() error");
    return -1;
  }

  LOG_DEBUG("accepted fd=%d", client_fd);

  fd_set_nb(client_fd);
  Conn *client = (Conn *)malloc(sizeof(Conn));
  memset(client, 0, sizeof(*client));
  if (!client) {
    close(client_fd);
    return -1;
  }

  client->fd = client_fd;
  client->shard_id = shard->shard_id;
  client->db = 0;
  client->addr = client_addr;
  client->recv_buf_size = 0;
  client->recv_buf_read = 0;
  client->send_buf_size = 0;
  client->send_buf_sent = 0;

  client->idle_start = get_monotonic_usec();

  conn_put(shard->conns, client, shard->shard_id);
  deque_push_back_and_attach(shard->idle_conn_queue, client, Conn, idle_conn_queue_node);

  return client_fd;
}

void handle_command(Shard *shard, Conn *conn, CmdArgs *args) {
  const Command *cmd = lookup_command(args, shard->gr_state->commands);
  
  if (cmd == NULL) {
    uint8_t *p = args->buf;
    char *cmd_name = (char *)&p[args->offsets[0]];
    size_t cmd_name_len = args->lens[0];
    char *first_arg = args->argc > 1 ? (char *)&p[args->offsets[1]] : "";
    size_t first_arg_len = args->argc > 1 ? args->lens[1] : 0;
    char message[1024];
    snprintf(message, sizeof(message),
             "unknown command '%.*s', with args beginning with: '%.*s'",
             (int)cmd_name_len, cmd_name, (int)first_arg_len, first_arg);
    write_simple_generic_error(conn, message);
    return;
  }

  if (cmd->arity != args->argc - 1 && cmd->arity != VAR_ARGC) {
    char message[64];
    snprintf(message, sizeof(message), "wrong number of arguments for '%.*s' command", (int)cmd->name_len, cmd->name);
    write_simple_generic_error(conn, message);
    return;
  }

  cmd->func(shard, conn, args);
}

bool try_handle_request(Shard *shard, Conn *conn) {
  if (unlikely(conn->recv_buf_size < 1)) {
    return false;
  }

  uint8_t *buf = conn->recv_buf + conn->recv_buf_read;
  CmdArgs args;
  ParseError err;
  if (likely(buf[0] == '*')) {
    err = parse_resp_request(conn, &args);
  } else {
    err = parse_inline_request(conn, &args);
  }
  assert(conn->send_buf_sent <= conn->send_buf_size);

  LOG_DEBUG_WITH_CTX(shard->shard_id, "try_handle_request() err=%d buf='%.*s'", err, (int)conn->recv_buf_size, conn->recv_buf + conn->recv_buf_read);

  switch (err) {
    case PARSE_OK:
      break;
    case PARSE_INCOMPLETE:
      return false;
    case PARSE_ERROR:
      write_simple_generic_error(conn, "parse error");
      conn->state = END;
      return false;
    case PARSE_ERROR_INVALID_ARGC:
      write_simple_generic_error(conn, "invalid argc");
      conn->state = END;
      return false;
  }

#if LOG_LEVEL == DEBUG_LEVEL
  for (size_t i = 0; i < args.argc; i++) {
    LOG_DEBUG_WITH_CTX(shard->shard_id, "Arg %zu: %.*s", i, args.lens[i], &args.buf[args.offsets[i]]);
  }
#endif

  if (likely(args.argc > 0)) {
    handle_command(shard, conn, &args);
  }

  if (unlikely(conn->state == END)) {
    return false;
  }

  if (unlikely(conn->recv_buf_size < (args.len))) {
    return false;
  }

  conn->recv_buf_read += args.len;
  size_t remaining = conn->recv_buf_size - args.len;
  LOG_DEBUG_WITH_CTX(shard->shard_id, "[after command] conn->recv_buf_read=%zu conn->recv_buf_size=%zu remaining=%zu", conn->recv_buf_read, conn->recv_buf_size, remaining);

  conn->recv_buf_size = remaining;

  return conn->recv_buf_size > 0;
}

bool try_fill_buffer(Shard *shard, Conn *conn, bool io) {
  assert(conn->recv_buf_size < sizeof(conn->recv_buf));

  if (io) {
  //if (io && !(conn->state & PIPELINE)) {
    ssize_t rv = 0;
    do {
      size_t cap = sizeof(conn->recv_buf) - conn->recv_buf_size;
      if (conn->recv_buf_read > 0) {
        LOG_DEBUG_WITH_CTX(shard->shard_id, "compacting buffer by moving %zu byte from offset %zu to 0", conn->recv_buf_size, conn->recv_buf_read);
        memmove(conn->recv_buf, &conn->recv_buf[conn->recv_buf_read], conn->recv_buf_size);
        conn->recv_buf_read = 0;
      }
      rv = read(conn->fd, &conn->recv_buf[conn->recv_buf_size], cap);
      LOG_DEBUG_WITH_CTX(shard->shard_id, "read %zu bytes(up to %zu) errno=%d", rv, cap, errno);
    } while (rv < 0 && errno == EINTR);

    if (rv < 0 && errno == EAGAIN) {
      // got EAGAIN, stop.
      return false;
    }

    if (rv < 0) {
      LOG_WARN("read() error");
      conn->state = END;
      return false;
    }

    if (rv == 0) {
      if (conn->recv_buf_size > 0) {
        LOG_WARN("unexpected EOF");
      } else {
        LOG_INFO("EOF");
      }

      conn->state = END;
      return false;
    }

    conn->recv_buf_size += (size_t)rv;
    assert(conn->recv_buf_size <= sizeof(conn->recv_buf));
  }

  if (conn->state & DISPATCH_WAITING) {
    return false;
  }  

  while (try_handle_request(shard, conn)) {
  }

  return conn->state & (END | DISPATCH_WAITING);
}

void state_request_cb(Shard *shard, Conn *conn) {
  LOG_DEBUG_WITH_CTX(shard->shard_id, "state_request_cb");
  while (try_fill_buffer(shard, conn, false)) {
  }
}

void state_request_epoll(Shard *shard, Conn *conn) {
  LOG_DEBUG_WITH_CTX(shard->shard_id, "state_request_epoll");
  while (try_fill_buffer(shard, conn, true)) {
  }
}

void conn_done(Shard *shard, Conn *conn) {
  close(conn->fd);
  deque_detach(&shard->pending_writes_queue, conn->pending_writes_queue_node);
  free(conn->pending_writes_queue_node);
  deque_detach(&shard->idle_conn_queue, conn->idle_conn_queue_node);
  free(conn->idle_conn_queue_node);
  shard->conns->array[conn->fd] = NULL;
  free(conn);
  conn = NULL;
}

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

#define MAX_IDLE_MS 60000

uint64_t close_idle_connections(Shard *shard) {
  uint64_t now_us = get_monotonic_usec();
  Deque *queue = &shard->idle_conn_queue;
  while (!deque_is_empty(queue)) {
    Conn *conn = queue->head->data;
    if (conn == NULL) {
      continue;
    }

    uint64_t elapsed = (now_us - conn->idle_start) / 1000;
    if (elapsed > MAX_IDLE_MS) {
      conn_done(shard, conn);
    } else {
      return MAX_IDLE_MS - elapsed;
    }
  }

  return MAX_IDLE_MS; 
}

uint64_t execute_callbacks(Shard *shard) {
  size_t num_shards = shard->gr_state->num_shards;

  bool notify = false;
  uint64_t count = 0;
  for (size_t i = 0; i < num_shards; i++) {
    if (i == shard->shard_id) {
      continue;
    }

    struct spscq *cb_queue = shard->cb_queues[i];
    CBContext *ctx = spscq_dequeue(cb_queue);
    while (ctx != NULL) {
      count++;
      void *arg = ctx;
      ctx->cb(shard, arg);
      Conn *c = ctx->conn;

      if (c->shard_id == shard->shard_id) { // our connection, we can handle IO
          state_request_cb(shard, c);

        if (c->send_buf_size > c->send_buf_sent) {
          deque_push_back_and_attach(shard->pending_writes_queue, c, Conn, pending_writes_queue_node);
          notify = true;
        }
      }

      free(ctx);

      ctx = spscq_dequeue(cb_queue);
    } 
  }

  if (notify) {
    write(shard->queue_efd, &(uint64_t){1}, sizeof(uint64_t));
  }

  return count;
}

int pin_shard_to_cpu(Shard *shard) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(shard->shard_id + 1, &cpuset);
  int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (rc != 0) {
    LOG_WARN("pthread_setaffinity_np()");
    return -1;
  }

  return 0;
}

bool wakeup_pending_shards(GRState *gr_state) {
  bool notify = false;
  for (size_t i = 0; i < gr_state->num_shards; i++) {
    Shard *s = &gr_state->shards[i];
    if (atomic_exchange(&s->notify_cb, false)) {
      write(s->queue_efd, &(uint64_t){1}, sizeof(uint64_t));
      notify = true;
    }
  }

  return notify;
}

void run_loop(void *arg) {
  Shard *shard = (Shard*)arg;

  // set thread affinity
  int pin_err = pin_shard_to_cpu(shard);
  if (pin_err) {
    panic("pin_shard_to_cpu() failed");
  }

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

  fd_set_nb(fd_listener);

  LOG_DEBUG("fd_listener=%d", fd_listener);

  int fd_epoll = epoll_create(1);
  LOG_DEBUG("fd_epoll=%d", fd_epoll);
  epoll_register(fd_epoll, fd_listener, EPOLLIN);
  LOG_DEBUG("listening");

  epoll_register(fd_epoll, shard->queue_efd, EPOLLIN);

  uint64_t total_nfds = 0;
  uint64_t total_flushes = 0;
  uint64_t total_callbacks = 0;

  uint64_t total_eventfd_events = 0;
  uint64_t total_read_events = 0;
  uint64_t total_write_events = 0;

  int nfds = 0;
  struct epoll_event events[128];
  int timeout = -1;
  while (shard->gr_state->running) {
    nfds = epoll_wait(fd_epoll, events, 128, timeout);
    if (nfds == -1) {
      perror("epoll_wait()");
      exit(1);
    }

    total_nfds += nfds;

    for (int i = 0; i < nfds; i++) {
      if (events[i].data.fd == fd_listener) {
        int fd = accept_new_conn(shard, fd_listener);
        if (fd < 0) {
          panic("accept_new_conn()");
        }
        epoll_register(fd_epoll, fd, EPOLLIN | EPOLLERR | EPOLLRDHUP | EPOLLHUP);
      } else if (events[i].data.fd == shard->queue_efd) {
        total_eventfd_events++;
        uint64_t val = 0; 
        read(shard->queue_efd, &val, sizeof(val));
      } else {
        struct epoll_event ev = events[i];
        int fd = ev.data.fd;
        Conn *conn = shard->conns->array[fd];

        conn->idle_start = get_monotonic_usec();
        deque_move_to_back(&shard->idle_conn_queue, conn->idle_conn_queue_node);
        if (ev.events & EPOLLIN) {
          total_read_events++;
          state_request_epoll(shard, conn);
        } else if (ev.events & EPOLLOUT) {
          total_write_events++;
          LOG_DEBUG("EPOLLOUT");
          flush_response_buffer(conn);
          epoll_modify(fd_epoll, fd, EPOLLIN | EPOLLERR | EPOLLRDHUP | EPOLLHUP);
        }

        if (conn->state == END || ev.events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
          epoll_unregister(fd_epoll, fd);
          conn_done(shard, conn);
        } else {
          if (conn->state & BLOCKED) {
            conn->state &= ~BLOCKED;
            epoll_modify(fd_epoll, fd, EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLRDHUP | EPOLLHUP);
          }
        }
      }
    }

    total_callbacks += execute_callbacks(shard);

    timeout = close_idle_connections(shard);

    bool notify = wakeup_pending_shards(shard->gr_state);
    if (notify) {
      timeout = 0;
    }
  }

  shard->stats = (ShardStats){
    .total_nfds = total_nfds,
    .total_flushes = total_flushes,
    .total_callbacks = total_callbacks,
    .total_eventfd_events = total_eventfd_events,
    .total_read_events = total_read_events,
    .total_write_events = total_write_events,
  };

  close(fd_listener);

  for (size_t i = 0; i < capacity_vector_Conn_ptr(shard->conns); i++) {
    Conn *conn = shard->conns->array[i];
    if (conn) {
      conn_done(shard, conn);
    }
  }
}

const size_t NUM_THREADS = 1;

int main() {
  Shard shards[NUM_THREADS];
  // TODO: rename GRState to GRServer?
  GRState gr_state = {
    .running = true,
    .num_dbs = 16,
    .commands = init_commands(),
    .num_shards = NUM_THREADS,
    .shards = shards,
  };

  init_shards(&gr_state);

  pthread_t threads[NUM_THREADS];
  for (size_t i = 0; i < NUM_THREADS; i++) {
    Shard *s = &shards[i];
    pthread_create(&threads[i], NULL, (void *(*)(void *))run_loop, (void *)s);
  }


  ShardStats total_stats = {0};
  for (size_t i = 0; i < NUM_THREADS; i++) {
    pthread_join(threads[i], NULL);
  
    Shard *s = &shards[i];
    total_stats.total_nfds += s->stats.total_nfds;
    total_stats.total_flushes += s->stats.total_flushes;
    total_stats.total_callbacks += s->stats.total_callbacks;
    total_stats.total_eventfd_events += s->stats.total_eventfd_events;
    total_stats.total_read_events += s->stats.total_read_events;
    total_stats.total_write_events += s->stats.total_write_events;

    printf("shard_id=%zu total_nfds=%zu total_flushes=%zu total_callbacks=%zu total_eventfd_events=%zu total_read_events=%zu total_write_events=%zu\n", s->shard_id, s->stats.total_nfds, s->stats.total_flushes, s->stats.total_callbacks, s->stats.total_eventfd_events, s->stats.total_read_events, s->stats.total_write_events);
  }

  printf("total_nfds=%zu total_flushes=%zu total_callbacks=%zu total_eventfd_events=%zu total_read_events=%zu total_write_events=%zu\n", total_stats.total_nfds, total_stats.total_flushes, total_stats.total_callbacks, total_stats.total_eventfd_events, total_stats.total_read_events, total_stats.total_write_events);

  free_server_state(&gr_state);

  return 0;
}
