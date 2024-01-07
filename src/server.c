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
#include "include/reactor.h"
#include "include/utils.h"

static uint64_t get_monotonic_usec() {
  struct timespec tv = {0, 0};
  clock_gettime(CLOCK_MONOTONIC, &tv);
  return tv.tv_sec * 1000000 + tv.tv_nsec / 1000;
}

void on_cb(GRContext *context, void *arg) {
  Shard *shard = context->shard;
  Reactor *reactor = shard->reactor;

  CBContext *ctx = arg;
  ctx->cb(context, arg);

  Conn *c = ctx->conn;
  // We may have a have more requests in the pipeline and were previously blocked due to a dispatched request. 
  // We check if we are executing on the shard owning the connection and try to process more requests from the pipeline.
  // TODO: this can probably be done in a nicer way.
  if (c->shard_id == reactor->id) {
    while (reactor->on_data_available(context, c)) {
    }
  }
}

Conn *on_accept(Reactor *reactor, struct sockaddr_in client_addr, int client_fd) {
  LOG_DEBUG("on_accept()");

  set_fd_options(client_fd, O_NONBLOCK | O_NDELAY);

  Conn *client = (Conn *)malloc(sizeof(Conn));
  memset(client, 0, sizeof(*client));
  if (!client) {
    close(client_fd);
    return NULL;
  }

  client->fd = client_fd;
  client->shard_id = reactor->id;
  client->db = 0;
  client->addr = client_addr;
  client->recv_buf_size = 0;
  client->recv_buf_read = 0;
  client->send_buf_size = 0;
  client->send_buf_sent = 0;

  client->idle_start = get_monotonic_usec();

  return client;
  // deque_push_back_and_attach(shard->idle_conn_queue, client, Conn, idle_conn_queue_node);
}

void handle_command(GRContext *context, Conn *conn, CmdArgs *args) {
  const Command *cmd = lookup_command(args, context->commands);
  
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

  cmd->func(context, conn, args);
}

bool on_data_available(GRContext *context, Conn *conn) {
  [[maybe_unused]] Shard *shard = context->shard;

  if (conn->recv_buf_size < 1) {
    return false;
  }

  uint8_t *buf = conn->recv_buf + conn->recv_buf_read;
  CmdArgs args;
  ParseError err;
  if (buf[0] == '*') {
    err = parse_resp_request(conn, &args);
  } else {
    err = parse_inline_request(conn, &args);
  }
  assert(conn->send_buf_sent <= conn->send_buf_size);

  // LOG_DEBUG_WITH_CTX(shard->shard_id, "try_handle_request() err=%d buf='%.*s'", err, (int)conn->recv_buf_size, conn->recv_buf + conn->recv_buf_read);

  switch (err) {
    case PARSE_OK:
      break;
    case PARSE_INCOMPLETE:
      return false;
    case PARSE_ERROR:
      write_simple_generic_error(conn, "parse error");
      conn->flags |= END;
      return false;
    case PARSE_ERROR_INVALID_ARGC:
      write_simple_generic_error(conn, "invalid argc");
      conn->flags |= END;
      return false;
  }

  // dump buffer
  LOG_DEBUG("conn->recv_buf:%.*s", (int)MESSAGE_MAX_LENGTH, conn->recv_buf);

#if LOG_LEVEL == DEBUG_LEVEL
  for (size_t i = 0; i < args.argc; i++) {
    LOG_DEBUG_WITH_CTX(shard->shard_id, "Arg %zu: %.*s", i, args.lens[i], &args.buf[args.offsets[i]]);
  }
#endif

  if (args.argc > 0) {
    handle_command(context, conn, &args);
  }

  if (conn->flags & END) {
    return false;
  }

  if (conn->recv_buf_size < (args.len)) {
    return false;
  }

  conn->recv_buf_read += args.len;
  size_t remaining = conn->recv_buf_size - args.len;
  LOG_DEBUG_WITH_CTX(shard->shard_id, "[after command] conn->recv_buf_read=%zu conn->recv_buf_size=%zu remaining=%zu", conn->recv_buf_read, conn->recv_buf_size, remaining);

  conn->recv_buf_size = remaining;

  return conn->recv_buf_size > 0;
}

void init_shards(ShardSet *shard_set, Reactor *reactors, size_t num_dbs) {
  for (size_t i = 0; i < shard_set->size; i++) {
    Shard *shard = &shard_set->shards[i];
    shard->shard_id = i;

    reactor_init(&reactors[i], i, on_cb, on_accept, on_data_available);

    shard->reactor = &reactors[i];

    uint64_t seed = get_monotonic_usec();
    shard->dbs = (struct hashmap **)malloc(sizeof(struct hashmap *) * num_dbs);
    for (size_t j = 0; j < num_dbs; j++) {
      shard->dbs[j] = hashmap_new(sizeof(Entry), 1 << 20, seed, seed, entry_hash_xxhash3, entry_compare, entry_free, NULL);
    }
  }
}

void free_context(GRContext *context) {
  Shard *shard = context->shard;
  for (size_t j = 0; j < context->num_dbs; j++) {
    hashmap_free(shard->dbs[j]);
  }

  free(shard->dbs);

  reactor_destroy(shard->reactor);
}

#define MAX_IDLE_MS 60000

// uint64_t close_idle_connections(Shard *shard) {
//   uint64_t now_us = get_monotonic_usec();
//   Deque *queue = &shard->idle_conn_queue;
//   while (!deque_is_empty(queue)) {
//     Conn *conn = queue->head->data;
//     if (conn == NULL) {
//       continue;
//     }

//     uint64_t elapsed = (now_us - conn->idle_start) / 1000;
//     if (elapsed > MAX_IDLE_MS) {
//       conn_done(shard, conn);
//     } else {
//       return MAX_IDLE_MS - elapsed;
//     }
//   }

//   return MAX_IDLE_MS; 
// }

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

void run_loop(void *arg) {
  GRContext *context = (GRContext *)arg;
  Shard *shard = context->shard;
  Reactor *reactor = shard->reactor; 

  // set thread affinity
//  int pin_err = pin_shard_to_cpu(shard);
//  if (pin_err) {
//    panic("pin_shard_to_cpu() failed");
//  }

  LOG_DEBUG("shard id: %d run_loop()", shard->shard_id);

  reactor_run(reactor, context);
}

const size_t NUM_THREADS = 1;

int main() {
  Shard shards[NUM_THREADS];
  Reactor reactors[NUM_THREADS];

  ShardSet shard_set = (ShardSet) {
    .shards = shards,
    .size = NUM_THREADS,
  };

  init_shards(&shard_set, (Reactor*)&reactors, 16);

  struct hashmap *commands = init_commands();

  pthread_t threads[NUM_THREADS];
  GRContext contexts[NUM_THREADS];

  for (size_t i = 0; i < NUM_THREADS; i++) {
    contexts[i] = (GRContext) {
      .num_dbs = 16,
      .commands = commands,
      .shard = &shards[i],
      .shard_set = &shard_set,
    };
  }

  for (size_t i = 0; i < NUM_THREADS; i++) {
    // dump context
    LOG_DEBUG("context num_dbs %zu", contexts[i].num_dbs);

    pthread_create(&threads[i], NULL, (void *(*)(void *))run_loop, (void *)&contexts[i]);
  }

  for (size_t i = 0; i < NUM_THREADS; i++) {
    pthread_join(threads[i], NULL);
    free_context(&contexts[i]);
  }

  hashmap_free(commands);

  return 0;
}
