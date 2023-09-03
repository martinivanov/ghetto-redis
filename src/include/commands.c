#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

#include "commands.h"
#include "hashmap.h"
#include "state.h"
#include "protocol.h"
#include "kv.h"

typedef void (*command_func)(State *state, Conn* conn, CmdArgs* args);

int cmd_compare(const void *a, const void *b, void *udata) {
  (void)(udata);

  const Command *ca = a;
  const Command *cb = b;
  if (ca->name_len != cb->name_len) {
    return ca->name_len - cb->name_len;
  }
  return memcmp(ca->name, cb->name, ca->name_len);
}

uint64_t cmd_hash(const void *a, uint64_t seed0, uint64_t seed1) {
  const Command *c = a;
  return hashmap_xxhash3(c->name, c->name_len, seed0, seed1);
}

void cmd_free(void *a) {
  Command *c = a;
  if (c->name) {
    free((void *)c->name);
  }
}

struct hashmap* init_commands() {
  struct hashmap *commands = hashmap_new(sizeof(Command), 1 << 16, 0, 0, cmd_hash, cmd_compare, cmd_free, NULL);

  register_command(commands, "PING", 0, cmd_ping);
  register_command(commands, "ECHO", 1, cmd_echo);
  register_command(commands, "QUIT", 0, cmd_quit);
  register_command(commands, "GET", 1, cmd_get);
  register_command(commands, "SET", 2, cmd_set);
  register_command(commands, "DEL", 1, cmd_del);
  register_command(commands, "SHUTDOWN", 0, cmd_shutdown);
  register_command(commands, "FLUSHALL", 0, cmd_flushall);
  register_command(commands, "SELECT", 1, cmd_select);

  return commands;
}

void free_commands(struct hashmap* commands) {
  hashmap_free(commands);
}

void register_command(struct hashmap* commands, const char* name, size_t arity, command_func func) {
  Command* cmd = (Command*)malloc(sizeof(Command));
  cmd->name_len = strlen(name);
  cmd->name = (uint8_t*)name;
  cmd->arity = arity;
  cmd->func = func;
  hashmap_set(commands, cmd);
}

void cmd_ping(State *state, Conn *conn, CmdArgs *args) {
  (void)state;
  (void)args;
  write_simple_string(conn, "PONG", 4);
}

void cmd_echo(State *state, Conn *conn, CmdArgs *args) {
  (void)state;
  if (args->argc == 2) {
    const uint8_t *echo = args->buf + args->offsets[1];
    const size_t echolen = args->lens[1];
    write_bulk_string(conn, echo, echolen);
  }
  else {
    write_simple_generic_error(conn, "wrong number of arguments for 'echo' command");
  }
}
void cmd_quit(State *state, Conn *conn, CmdArgs *args) {
  (void)state;
  (void)args;
    conn->state = END;
    write_simple_string(conn, "OK", 2);
}

void cmd_get(State *state, Conn *conn, CmdArgs *args) {
  const uint8_t *key = args->buf + args->offsets[1];
  const size_t keylen = args->lens[1];

  struct hashmap *db = state->dbs[conn->db];
  Entry *entry = (Entry *)hashmap_get(db, &(Entry){.key = key, .keylen = keylen});
  if (entry) {
    write_bulk_string(conn, entry->val, entry->vallen);
  } else {
    write_null_bulk_string(conn);
  }
}

void cmd_set(State *state, Conn *conn, CmdArgs *args) {
    const size_t keylen = args->lens[1];
    const uint8_t *key = (uint8_t *)malloc(keylen);
    memcpy((void *)key, args->buf + args->offsets[1], keylen);

    const size_t vallen = args->lens[2];
    const uint8_t *val = (uint8_t *)malloc(vallen);
    memcpy((void *)val, args->buf + args->offsets[2], vallen);

    struct hashmap *db = state->dbs[conn->db];

    Entry *entry = &(Entry){.key = key, .keylen = keylen, .val = val, .vallen = vallen};
    hashmap_set(db, entry);
    write_simple_string(conn, "OK", 2);
}

void cmd_del(State *state, Conn *conn, CmdArgs *args) {
    const uint8_t *cmd = args->buf + args->offsets[0];
    const uint8_t *key = &cmd[args->offsets[1]];
    const size_t keylen = args->lens[1];

    struct hashmap *db = state->dbs[conn->db];
    const void *entry = hashmap_delete(db, &(Entry){.key = (void *)key, .keylen = keylen});
    if (entry) {
      entry_free((void*)entry);
      write_integer(conn, 1);
    } else {
      write_integer(conn, 0);
    }
}

void cmd_shutdown(State *state, Conn *conn, CmdArgs *args) {
  (void)conn;
  (void)args;
  state->running = false;
}

void cmd_flushall(State *state, Conn *conn, CmdArgs *args) {
  (void)args;

  for (size_t i = 0; i < state->num_dbs; i++) {
    struct hashmap *db = state->dbs[i];
    hashmap_clear(db, true);
  }

  write_simple_string(conn, "OK", 2);
}

void cmd_select(State *state, Conn *conn, CmdArgs *args) {
  (void)state;

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

  if (dbnum > state->num_dbs) {
    write_simple_generic_error(conn, "invalid DB index");
    return;
  }
  
  conn->db = dbnum;
  write_simple_string(conn, "OK", 2);
}
