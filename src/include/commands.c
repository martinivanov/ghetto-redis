#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <ctype.h>

#include "commands.h"
#include "hashmap.h"
#include "state.h"
#include "protocol.h"
#include "kv.h"

inline const Command* lookup_command(CmdArgs *CmdArgs, struct hashmap* commands) {
  Command *cmd = &(Command) {
    .name_len = CmdArgs->lens[0],
    .name = CmdArgs->buf + CmdArgs->offsets[0]
  };

  const Command *found = hashmap_get(commands, cmd);
  if (found) {
    return found;
  }

  return NULL;
}

int command_compare(const void *a, const void *b, void *udata) {
  (void)(udata);

  const Command *ca = a;
  const Command *cb = b;
  if (ca->name_len != cb->name_len) {
    return ca->name_len - cb->name_len;
  }
  return memcmp(ca->name, cb->name, ca->name_len);
}

uint64_t command_hash(const void *a, uint64_t seed0, uint64_t seed1) {
  const Command *c = a;
  return hashmap_xxhash3(c->name, c->name_len, seed0, seed1);
}

struct hashmap* init_commands() {
  struct hashmap *commands = hashmap_new(sizeof(Command), 1 << 16, 0, 0, command_hash, command_compare, NULL, NULL);

  register_command(commands, "PING", 0, cmd_ping);
  register_command(commands, "ECHO", 1, cmd_echo);
  register_command(commands, "QUIT", 0, cmd_quit);
  register_command(commands, "GET", 1, cmd_get);
  register_command(commands, "SET", 2, cmd_set);
  register_command(commands, "DEL", 1, cmd_del);
  register_command(commands, "SHUTDOWN", 0, cmd_shutdown);
  register_command(commands, "FLUSHALL", 0, cmd_flushall);
  register_command(commands, "SELECT", 1, cmd_select);
  register_command(commands, "INCR", 1, cmd_incr);
  register_command(commands, "DECR", 1, cmd_decr);
  register_command(commands, "INCRBY", 2, cmd_incrby);
  register_command(commands, "DECRBY", 2, cmd_decrby);
  register_command(commands, "CLIENTS", 0, cmd_clients);
  register_command(commands, "MGET", VAR_ARGC, cmd_mget);
  register_command(commands, "MSET", VAR_ARGC, cmd_mset);

  return commands;
}

void free_commands(struct hashmap* commands) {
  hashmap_free(commands);
}

void register_command(struct hashmap* commands, const char* name, size_t arity, command_func func) {
  Command *cmd = &(Command) {
    .name_len = strlen(name),
    .name = (uint8_t*)name,
    .arity = arity,
    .func = func
  };

  hashmap_set(commands, cmd);
}

void cmd_ping(Shard *shard, Conn *conn, const CmdArgs *args) {
  (void)shard;
  (void)args;
  write_simple_string(conn, "PONG", 4);
}

void cmd_echo(Shard *shard, Conn *conn, const CmdArgs *args) {
  (void)shard;
  const uint8_t *echo = args->buf + args->offsets[1];
  const size_t echolen = args->lens[1];
  write_bulk_string(conn, echo, echolen);
}

void cmd_quit(Shard *shard, Conn *conn, const CmdArgs *args) {
  (void)shard;
  (void)args;
    conn->state = END;
    write_simple_string(conn, "OK", 2);
}

void cmd_get(Shard *shard, Conn *conn, const CmdArgs *args) {
  const uint8_t *key = args->buf + args->offsets[1];
  const size_t keylen = args->lens[1];

  struct hashmap *db = shard->dbs[conn->db];
  Entry *entry = (Entry *)hashmap_get(db, &(Entry){.key = key, .keylen = keylen});
  if (entry) {
    write_bulk_string(conn, entry->val, entry->vallen);
  } else {
    write_null_bulk_string(conn);
  }
}

void cmd_set(Shard *shard, Conn *conn, const CmdArgs *args) {
    const size_t keylen = args->lens[1];
    const uint8_t *key = (uint8_t *)malloc(keylen);
    memcpy((void *)key, args->buf + args->offsets[1], keylen);

    const size_t vallen = args->lens[2];
    const uint8_t *val = (uint8_t *)malloc(vallen);
    memcpy((void *)val, args->buf + args->offsets[2], vallen);

    struct hashmap *db = shard->dbs[conn->db];

    Entry *entry = &(Entry){.key = key, .keylen = keylen, .val = val, .vallen = vallen};
    Entry *existing = (Entry *)hashmap_set(db, entry);
    if (existing) {
      entry_free((void*)existing);
    }
    write_simple_string(conn, "OK", 2);
}

void cmd_del(Shard *shard, Conn *conn, const CmdArgs *args) {
    const uint8_t *cmd = args->buf + args->offsets[0];
    const uint8_t *key = &cmd[args->offsets[1]];
    const size_t keylen = args->lens[1];

    struct hashmap *db = shard->dbs[conn->db];
    const void *entry = hashmap_delete(db, &(Entry){.key = (void *)key, .keylen = keylen});
    if (entry) {
      entry_free((void*)entry);
      write_integer(conn, 1);
    } else {
      write_integer(conn, 0);
    }
}

void cmd_shutdown(Shard *shard, Conn *conn, const CmdArgs *args) {
  (void)conn;
  (void)args;
  GRState *gr_state = shard->gr_state;
  gr_state->running = false;
}

void cmd_flushall(Shard *shard, Conn *conn, const CmdArgs *args) {
  (void)args;

  GRState *gr_state = shard->gr_state;
  for (size_t i = 0; i < gr_state->num_dbs; i++) {
    struct hashmap *db = shard->dbs[i];
    hashmap_clear(db, true);
  }

  write_simple_string(conn, "OK", 2);
}

void cmd_select(Shard *shard, Conn *conn, const CmdArgs *args) {
  (void)shard;

  const uint8_t *cmd = args->buf + args->offsets[0];
  const uint8_t *db = &cmd[args->offsets[1]];
  const size_t dblen = args->lens[1];

  char* tmp = (char*)malloc(dblen + 1);
  memcpy(tmp, db, dblen);
  tmp[dblen] = '\0';

  char *endptr;
  uint64_t dbnum = strtoull(tmp, &endptr, 10);
  free(tmp);

  if (errno) {
    write_simple_generic_error(conn, "invalid DB index");
    return;
  }

  GRState *gr_state = shard->gr_state;
  if (dbnum >= gr_state->num_dbs) {
    write_simple_generic_error(conn, "invalid DB index");
    return;
  }
  
  conn->db = dbnum;
  write_simple_string(conn, "OK", 2);
}

bool try_parse_signed_integer(const uint8_t *buf, size_t len, int64_t *result) {
  int64_t res = 0;
  bool is_negative = false;
  size_t pos = 0;
  
  if (buf[pos] == '-' || buf[pos] == '+') {
    if (buf[pos] == '-') {
      is_negative = true;
    }
    pos++;
  }

  while(pos < len && isdigit(buf[pos])) {
    res = res * 10 + (buf[pos] - '0');
    pos++;
  }

  if (pos != len) {
    return false;
  }

  if (is_negative) {
    res = -res;
  }
  
  *result = res;

  return true;
}

void modify_counter(Shard *shard, Conn *conn, const CmdArgs *args, int64_t delta) {
  const size_t keylen = args->lens[1];
  const uint8_t *key = (uint8_t *)malloc(keylen);
  memcpy((void *)key, args->buf + args->offsets[1], keylen);

  struct hashmap *db = shard->dbs[conn->db];
  int64_t val = 0;
  Entry *entry = (Entry *)hashmap_get(db, &(Entry){.key = key, .keylen = keylen});
  if (entry) {
    if (!try_parse_signed_integer(entry->val, entry->vallen, &val)) {
      write_simple_generic_error(conn, "value is not an integer or out of range");
      free((void *)key);
      return;
    }
  }

  val += delta;
  char *buf = (char*)malloc(20); // 20 bytes for int64_t
  size_t len = sprintf(buf, "%ld", val);
  Entry *existing = (Entry *)hashmap_set(db, &(Entry){.key = key, .keylen = keylen, .val = (uint8_t *)buf, .vallen = len});
  if (existing) {
    entry_free((void*)existing);
  }
  write_integer(conn, val);
}

void cmd_incr(Shard *shard, Conn *conn, const CmdArgs *args) {
  modify_counter(shard, conn, args, 1);
}

void cmd_decr(Shard *shard, Conn *conn, const CmdArgs *args) {
  modify_counter(shard, conn, args, -1);
}

void cmd_incrby(Shard *shard, Conn *conn, const CmdArgs *args) {
  int64_t delta = 0;
  if (!try_parse_signed_integer(args->buf + args->offsets[2], args->lens[2], &delta)) {
    write_simple_generic_error(conn, "value is not an integer or out of range");
    return;
  }

  if (delta < 0) {
    write_simple_generic_error(conn, "increment would produce negative integer");
    return;
  }

  modify_counter(shard, conn, args, delta);
}

void cmd_decrby(Shard *shard, Conn *conn, const CmdArgs *args) {
  int64_t delta = 0;
  if (!try_parse_signed_integer(args->buf + args->offsets[2], args->lens[2], &delta)) {
    write_simple_generic_error(conn, "value is not an integer or out of range");
    return;
  }

  if (delta < 0) {
    write_simple_generic_error(conn, "decrement would produce negative integer");
    return;
  }

  modify_counter(shard, conn, args, -delta);
}

void cmd_clients(Shard *shard, Conn *conn, const CmdArgs *args) {
  (void)args;

  vector_Conn_ptr *conns = shard->conns;

  size_t count = 0;
  for (size_t i = 0; i < conns->size; i++) {
    Conn *c = conns->array[i];
    if (c == NULL || c->state == END) {
      continue;
    }
    count++;
  }

  write_array_header(conn, count);

  for (size_t i = 0; i < conns->size; i++) {
    Conn *c = conns->array[i];
    if (c == NULL || c->state == END) {
      continue;
    }

    uint16_t port = ntohs(c->addr.sin_port);
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(c->addr.sin_addr), ip, INET_ADDRSTRLEN);

    char buf[256];
    size_t len = sprintf(buf, "fd=%d %s:%d", c->fd, ip, port); 
    
    write_bulk_string(conn, (uint8_t *)buf, len);
  }
}

void cmd_mget(Shard *shard, Conn *conn, const CmdArgs *args) {
  struct hashmap *db = shard->dbs[conn->db];
  write_array_header(conn, args->argc - 1);
  for (size_t i = 1; i < args->argc; i++) {
    uint8_t *key = args->buf + args->offsets[i];
    size_t keylen = args->lens[i];

    Entry *entry = (Entry *)hashmap_get(db, &(Entry){.key = key, .keylen = keylen});
    if (entry) {
      write_bulk_string(conn, entry->val, entry->vallen);
    } else {
      write_null_bulk_string(conn);
    }
  }
}

void cmd_mset(Shard *shard, Conn *conn, const CmdArgs *args) {
  if (args->argc % 2 != 1) {
    write_simple_generic_error(conn, "wrong number of arguments for MSET");
    return;
  }

  struct hashmap *db = shard->dbs[conn->db];
  for (size_t i = 1; i < args->argc; i += 2) {
    size_t keylen = args->lens[i];
    uint8_t *key = (uint8_t*)malloc(keylen);
    memcpy((void *)key, args->buf + args->offsets[i], keylen);

    size_t vallen = args->lens[i + 1];
    uint8_t *val = (uint8_t*)malloc(vallen);
    memcpy((void *)val, args->buf + args->offsets[i + 1], vallen);

    Entry *entry = &(Entry){.key = key, .keylen = keylen, .val = val, .vallen = vallen};
    Entry *existing = (Entry *)hashmap_set(db, entry);
    if (existing) {
      entry_free((void*)existing);
    }
  }

  write_simple_string(conn, "OK", 2);
}