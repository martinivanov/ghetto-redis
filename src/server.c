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
#include <pthread.h>

#include "include/logging.h"
#include "include/protocol.h"
#include "include/vector.h"
#include "include/vector_types.h"

#include "include/hashmap.h"
#include "include/deque.h"
#include "include/state.h"
#include "include/commands.h"
#include "include/kv.h"

#define unlikely(expr) __builtin_expect(!!(expr), 0)
#define likely(expr) __builtin_expect(!!(expr), 1)

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

    int seed = time(NULL);
    shard->dbs = (struct hashmap **)malloc(sizeof(struct hashmap *) * gr_state->num_dbs);
    for (size_t j = 0; j < gr_state->num_dbs; j++) {
      shard->dbs[j] = hashmap_new(sizeof(Entry), 1 << 20, seed, seed, entry_hash_xxhash3, entry_compare, entry_free, NULL);
    }

    shard->gr_state = gr_state;
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

int32_t accept_new_conn(Shard *shard, int fd_listener) {
  struct sockaddr_in client_addr = {};
  socklen_t socklen = sizeof(client_addr);
  int client_fd = accept(fd_listener, (struct sockaddr *)&client_addr, &socklen);
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
  client->db = 0;
  client->addr = client_addr;
  client->recv_buf_size = 0;
  client->recv_buf_read = 0;
  client->send_buf_size = 0;
  client->send_buf_sent = 0;

  client->idle_start = get_monotonic_usec();

  conn_put(shard->conns, client);
  deque_push_back_and_attach(shard->idle_conn_queue, client, Conn, idle_conn_queue_node);

  return client_fd;
}

bool try_flush_buffer(Conn *conn) {
  ssize_t rv = 0;
  do {
    size_t remaining = conn->send_buf_size - conn->send_buf_sent;
    // print buffer contents
#ifdef DEBUG
    printf("conn->send_buf_size=%zu conn->send_buf_sent=%zu remaining=%zu\n", conn->send_buf_size, conn->send_buf_sent, remaining);
    printf("conn->send_buf:\n%.*s\n", (int)MESSAGE_MAX_LENGTH, conn->send_buf);
#endif
    rv = write(conn->fd, &conn->send_buf[conn->send_buf_sent], remaining);
  } while (rv < 0 && errno == EINTR);

  if (rv < 0 && errno == EAGAIN) {
    conn->state |= BLOCKED;
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
#ifdef DEBUG
    printf("Response sent fully --- conn->send_buf_sent=%zu conn->send_buf_size=%zu\n", conn->send_buf_sent, conn->send_buf_size);
#endif
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

#ifdef DEBUG
  for (size_t i = 0; i < args.argc; i++) {
    printf("Arg %zu: ", i);
    for (size_t j = 0; j < args.lens[i]; j++) {
      printf("%c", args.buf[args.offsets[i] + j]);
    }
    printf("\n");
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
#ifdef DEBUG
  printf("[after command] conn->recv_buf_read=%zu conn->recv_buf_size=%zu remaining=%zu\n", conn->recv_buf_read, conn->recv_buf_size, remaining);
#endif

  conn->recv_buf_size = remaining;

  return conn->recv_buf_size > 0;
}

bool try_fill_buffer(Shard *shard, Conn *conn) {
  assert(conn->recv_buf_size < sizeof(conn->recv_buf));

  ssize_t rv = 0;
  do {
    size_t cap = sizeof(conn->recv_buf) - conn->recv_buf_size;
    if (conn->recv_buf_read > 0) {
#ifdef DEBUG
      printf("compacting buffer by moving %zu byte from offset %zu to 0\n", conn->recv_buf_size, conn->recv_buf_read);
#endif
      memmove(conn->recv_buf, &conn->recv_buf[conn->recv_buf_read], conn->recv_buf_size);
      conn->recv_buf_read = 0;
    }
    rv = read(conn->fd, &conn->recv_buf[conn->recv_buf_size], cap);
#ifdef DEBUG
    printf("read %zu bytes(up to %zu) errno=%d \n", rv, cap, errno);
#endif
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

  while (try_handle_request(shard, conn)) {
  }

  bool test = (conn->state == END);
  return test;
}

void state_request(Shard *shard, Conn *conn) {
  while (try_fill_buffer(shard, conn)) {
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

void epoll_modify(int fd_epoll, int fd, uint32_t events) {
  struct epoll_event ev;
  ev.events = events;
  ev.data.fd = fd;
  if (epoll_ctl(fd_epoll, EPOLL_CTL_MOD, fd, &ev) == -1) {
    perror("epoll_ctl()\n");
    exit(1);
  }
}

void flush_pending_writes(Shard *shard) {
    while (!deque_is_empty(&shard->pending_writes_queue)) {
      Conn *conn = deque_pop_front(&shard->pending_writes_queue);
      if (conn == NULL) {
        continue;
      }
      conn->pending_writes_queue_node = NULL;
      state_response(conn);
    }
}

#define MAX_IDLE_MS 5000

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

void run_loop(void *arg) {
  Shard *shard = (Shard*)arg;

  printf("shard_id=%zu\n", shard->shard_id);

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
    printf("errno=%d\n", errno);
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
  int timeout = -1;
  while (shard->gr_state->running) {

    // execute callbacks

    flush_pending_writes(shard);

    nfds = epoll_wait(fd_epoll, events, 128, timeout);
    if (nfds == -1) {
      perror("epoll_wait()");
      exit(1);
    }

    for (int i = 0; i < nfds; i++) {
      if (events[i].data.fd == fd_listener) {
        int fd = accept_new_conn(shard, fd_listener);
        if (fd < 0) {
          panic("accept_new_conn()");
        }
        epoll_register(fd_epoll, fd, EPOLLIN | EPOLLERR | EPOLLRDHUP | EPOLLHUP);
      } else {
        struct epoll_event ev = events[i];
        int fd = ev.data.fd;
        Conn *conn = shard->conns->array[fd];

        conn->idle_start = get_monotonic_usec();
        deque_move_to_back(&shard->idle_conn_queue, conn->idle_conn_queue_node);
        if (ev.events & EPOLLIN) {
          state_request(shard, conn);
        } else if (ev.events & EPOLLOUT) {
          state_response(conn);
          epoll_modify(fd_epoll, fd, EPOLLIN | EPOLLERR | EPOLLRDHUP | EPOLLHUP);
        }

        if (conn->state == END || ev.events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
          epoll_unregister(fd_epoll, fd);
          conn_done(shard, conn);
        } else {
          if (conn->send_buf_size != conn->send_buf_sent) {
            deque_push_back_and_attach(shard->pending_writes_queue, conn, Conn, pending_writes_queue_node);
          }

          if (conn->state & BLOCKED) {
            conn->state &= ~BLOCKED;
            epoll_modify(fd_epoll, fd, EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLRDHUP | EPOLLHUP);
          }
        }
      }
    }

    timeout = close_idle_connections(shard);
  }

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

  for (size_t i = 0; i < NUM_THREADS; i++) {
    pthread_join(threads[i], NULL);
  }

  free_server_state(&gr_state);

  return 0;
}