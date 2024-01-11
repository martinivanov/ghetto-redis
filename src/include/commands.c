#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <ctype.h>
#include <unistd.h>

#include "commands.h"
#include "hashmap.h"
#include "state.h"
#include "protocol.h"
#include "kv.h"
#include "reactor.h"

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
  // register_command(commands, "CLIENTS", 0, cmd_clients);
  // register_command(commands, "MGET", VAR_ARGC, cmd_mget);
  // register_command(commands, "MSET", VAR_ARGC, cmd_mset);
  register_command(commands, "DPING", 1, cmd_dispatch_ping);

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

void fill_req_cb_ctx(CBContext *cb_ctx, Shard *src, Shard *dst, Conn *conn, dispatch_cb cb) {
  cb_ctx->src = src;
  cb_ctx->dst = dst;
  cb_ctx->conn = conn;
  cb_ctx->cb = cb;
}

void cmd_ping(GRContext *context, Conn *conn, const CmdArgs *args) {
  (void)context;
  (void)args;
  write_simple_string(conn, "PONG", 4);
}

void cmd_echo(GRContext *context, Conn *conn, const CmdArgs *args) {
  (void)context;
  const uint8_t *echo = args->buf + args->offsets[1];
  const size_t echolen = args->lens[1];
  write_bulk_string(conn, echo, echolen);
}

void cmd_quit(GRContext *context, Conn *conn, const CmdArgs *args) {
  (void)context;
  (void)args;
  conn->flags |= END;
  write_simple_string(conn, "OK", 2);
}

DEFINE_COMMAND(
  get, 
  KEYED_REQ_CTX(), 
  RESP_CTX(
    Entry *entry;
  ),
  CMD_VARS(
    const uint8_t *key = key_buf_ptr;
  ),
  CMD_PRE_INLINE_EXEC(),
  CMD_EXEC(
    Entry *entry = (Entry *)hashmap_get_with_hash(db, &(Entry){.key = key, .keylen = keylen}, hash);
  ),
  CMD_RESP(
    if (entry) {
      write_bulk_string(conn, entry->val, entry->vallen);
    } else {
      write_null_bulk_string(conn);
    }
  ),
  CMD_PRE_DISPATCH(
    ctx->ctx.key = malloc(keylen);
    memcpy(ctx->ctx.key, key, keylen);
    ctx->ctx.keylen = keylen;
    ctx->ctx.hash = hash; 
  ),
  CMD_PRE_DISPATCH_EXEC(),
  CMD_POST_DISPATCH_EXEC(
    free(keyed_ctx->key);
    if (entry) {
      // We need to copy the whole entry because overwriting entries during SET frees the old entry and
      // we may have a dispatched GET response that may try to dereference the freed old entry.
      // This slows down GETs but it's the simplest solution for now.
      // TODO: use RCU, reference counting or something else to avoid copying the whole entry
      uint8_t *key_copy = (uint8_t *)malloc(entry->keylen);
      memcpy(key_copy, entry->key, entry->keylen);

      uint8_t *val_copy = (uint8_t *)malloc(entry->vallen);
      memcpy(val_copy, entry->val, entry->vallen);

      Entry *copy = ENTRY_INIT(key_copy, entry->keylen, val_copy, entry->vallen);

      resp_ctx->entry = (Entry *)malloc(sizeof(Entry));
      memcpy(resp_ctx->entry, copy, sizeof(Entry));
    } else {
      resp_ctx->entry = NULL;
    }
  ),
  CMD_PRE_RESP(
   Entry *entry = ctx->entry; 
  ),
  CMD_POST_RESP(
    if (entry) {
      free((void *)entry->key);
      free((void *)entry->val);
      free(entry);
    }
  )
)

DEFINE_COMMAND(
  set, 
  KEYED_REQ_CTX(
    uint8_t *val;
    size_t vallen;
  ), 
  RESP_CTX(),
  CMD_VARS(
    uint8_t *key = malloc(keylen);
    memcpy(key, key_buf_ptr, keylen);
    
    size_t vallen = args->lens[2];
    uint8_t *val = (uint8_t *)malloc(vallen); 
    memcpy((void *)val, args->buf + args->offsets[2], vallen);
  ),
  CMD_PRE_INLINE_EXEC(),
  CMD_EXEC(
    Entry *entry = ENTRY_INIT(key, keylen, val, vallen);
    Entry *existing = (Entry *)hashmap_set_with_hash(db, entry, hash);
    if (existing) {
      entry_free((void*)existing);
    }
  ),
  CMD_RESP(
    write_simple_string(conn, "OK", 2);
  ),
  CMD_PRE_DISPATCH(
    ctx->ctx.key = (uint8_t *)key;
    ctx->ctx.keylen = (size_t)keylen;
    ctx->ctx.hash = (size_t)hash;
    ctx->val = (uint8_t *)val;
    ctx->vallen = (size_t)vallen;
  ),
  CMD_PRE_DISPATCH_EXEC(
    size_t vallen = ctx->vallen;
    uint8_t *val = ctx->val;
  ),
  CMD_POST_DISPATCH_EXEC(),
  CMD_PRE_RESP(),
  CMD_POST_RESP()
)

DEFINE_COMMAND(
  del, 
  KEYED_REQ_CTX(), 
  RESP_CTX(
    uint64_t res;
  ),
  CMD_VARS(
    const uint8_t *key = key_buf_ptr;
  ),
  CMD_PRE_INLINE_EXEC(),
  CMD_EXEC(
    const void *entry = hashmap_delete_with_hash(db, &(Entry){.key = key, .keylen = keylen}, hash);
    uint64_t res = 0;
    if (entry) {
      entry_free((void*)entry);
      res = 1;
    } 
  ),
  CMD_RESP(
    write_integer(conn, res);
  ),
  CMD_PRE_DISPATCH(
    ctx->ctx.key = malloc(keylen);
    memcpy(ctx->ctx.key, key, keylen);
    ctx->ctx.keylen = keylen;
    ctx->ctx.hash = hash;
  ),
  CMD_PRE_DISPATCH_EXEC(),
  CMD_POST_DISPATCH_EXEC(
    free(keyed_ctx->key);
    resp_ctx->res = res;
  ),
  CMD_PRE_RESP(
    uint64_t res = ctx->res;
  ),
  CMD_POST_RESP()
)

void cmd_shutdown(GRContext *context, Conn *conn, const CmdArgs *args) {
  (void)conn;
  (void)args;
  Reactor *reactor = context->shard->reactor;
  ShardSet *shard_set = context->shard_set;
  for (size_t i = 0; i < shard_set->size; i++) {
    Shard *shard = &shard_set->shards[i];
    Reactor *r = shard->reactor;
    r->running = false;
    BITSET64_SET(reactor->soft_notify, i);
  }

  reactor_wakeup_pending(reactor, context);
}

void cmd_flushall(GRContext *context, Conn *conn, const CmdArgs *args) {
  (void)args;
  ShardSet *shard_set = context->shard_set;

  for (size_t i = 0; i < shard_set->size; i++) {
    Shard *shard = &shard_set->shards[i];
    for (size_t j = 0; j < context->num_dbs; j++) {
      struct hashmap *db = shard->dbs[j];
      hashmap_clear(db, true);
    }
  }

  write_simple_string(conn, "OK", 2);
}

void cmd_select(GRContext *context, Conn *conn, const CmdArgs *args) {
  const uint8_t *db = args->buf + args->offsets[1];
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

  if (dbnum >= context->num_dbs) {
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

bool try_modify_counter(Shard *shard, Conn *conn, const uint8_t *key, const size_t keylen, const uint64_t hash, uint64_t *res, int64_t delta) {
  struct hashmap *db = shard->dbs[conn->db];
  int64_t val = 0;
  Entry *entry = (Entry *)hashmap_get_with_hash(db, &(Entry){.key = key, .keylen = keylen}, hash);
  if (entry) {
    if (!try_parse_signed_integer(entry->val, entry->vallen, &val)) {
      free((void *)key);
      return false;
    }
  }

  val += delta;
  char *buf = (char*)malloc(20); // 20 bytes for int64_t
  size_t len = sprintf(buf, "%ld", val);
  Entry *new = ENTRY_INIT(key, keylen, (uint8_t*)buf, len);
  Entry *existing = (Entry *)hashmap_set_with_hash(db, new, hash);
  if (existing) {
    entry_free((void*)existing);
  }

  *res = val;

  return true;
}

// void cmd_mget(Shard *shard, Conn *conn, const CmdArgs *args) {
//   struct hashmap *db = shard->dbs[conn->db];
//   write_array_header(conn, args->argc - 1);
//   for (size_t i = 1; i < args->argc; i++) {
//     uint8_t *key = args->buf + args->offsets[i];
//     size_t keylen = args->lens[i];

//     Entry *entry = (Entry *)hashmap_get(db, &(Entry){.key = key, .keylen = keylen});
//     if (entry) {
//       write_bulk_string(conn, entry->val, entry->vallen);
//     } else {
//       write_null_bulk_string(conn);
//     }
//   }
// }

// void cmd_mset(Shard *shard, Conn *conn, const CmdArgs *args) {
//   if (args->argc % 2 != 1) {
//     write_simple_generic_error(conn, "wrong number of arguments for MSET");
//     return;
//   }

//   struct hashmap *db = shard->dbs[conn->db];
//   for (size_t i = 1; i < args->argc; i += 2) {
//     size_t keylen = args->lens[i];
//     uint8_t *key = (uint8_t*)malloc(keylen);
//     memcpy((void *)key, args->buf + args->offsets[i], keylen);

//     size_t vallen = args->lens[i + 1];
//     uint8_t *val = (uint8_t*)malloc(vallen);
//     memcpy((void *)val, args->buf + args->offsets[i + 1], vallen);

//     Entry *entry = &(Entry){.key = key, .keylen = keylen, .val = val, .vallen = vallen};
//     Entry *existing = (Entry *)hashmap_set(db, entry);
//     if (existing) {
//       entry_free((void*)existing);
//     }
//   }

//   write_simple_string(conn, "OK", 2);
// }

DEFINE_COMMAND(
  dispatch_ping,
  KEYED_REQ_CTX(
    size_t shard_id;
  ),
  RESP_CTX(
    size_t shard_id;
  ),
  CMD_VARS(
    const uint8_t *key = key_buf_ptr;
  ),
  CMD_PRE_INLINE_EXEC(),
  CMD_EXEC(),
  CMD_RESP(
    write_integer(conn, shard_id);
  ),
  CMD_PRE_DISPATCH(
    ctx->ctx.key = malloc(keylen);
    memcpy(ctx->ctx.key, key, keylen);
    ctx->ctx.keylen = keylen;
    ctx->ctx.hash = hash;
    ctx->shard_id = shard_id;
  ),
  CMD_PRE_DISPATCH_EXEC(),
  CMD_POST_DISPATCH_EXEC(
    resp_ctx->shard_id = ctx->shard_id;
    free(keyed_ctx->key);
  ),
  CMD_PRE_RESP(
    size_t shard_id = ctx->shard_id;
  ),
  CMD_POST_RESP()
)

DEFINE_COMMAND(
  incr, 
  KEYED_REQ_CTX(), 
  RESP_CTX(
    uint64_t res;
  ),
  CMD_VARS(
    uint8_t *key = malloc(keylen);
    memcpy(key, key_buf_ptr, keylen);
  ),
  CMD_PRE_INLINE_EXEC(
  ),
  CMD_EXEC(
    uint64_t res = 0;
    try_modify_counter(shard, conn, key, keylen, hash, &res, 1);
  ),
  CMD_RESP(
    write_integer(conn, res);
  ),
  CMD_PRE_DISPATCH(
    ctx->ctx.key = key;
    ctx->ctx.keylen = keylen;
    ctx->ctx.hash = hash;
  ),
  CMD_PRE_DISPATCH_EXEC(
  ),
  CMD_POST_DISPATCH_EXEC(
    resp_ctx->res = res;
  ),
  CMD_PRE_RESP(
    uint64_t res = ctx->res;
  ),
  CMD_POST_RESP()
)

DEFINE_COMMAND(
  decr, 
  KEYED_REQ_CTX(), 
  RESP_CTX(
    uint64_t res;
  ),
  CMD_VARS(
    uint8_t *key = malloc(keylen);
    memcpy(key, key_buf_ptr, keylen);
  ),
  CMD_PRE_INLINE_EXEC(
  ),
  CMD_EXEC(
    uint64_t res = 0;
    try_modify_counter(shard, conn, key, keylen, hash, &res, -1);
  ),
  CMD_RESP(
    write_integer(conn, res);
  ),
  CMD_PRE_DISPATCH(
    ctx->ctx.key = key;
    ctx->ctx.keylen = keylen;
    ctx->ctx.hash = hash;
  ),
  CMD_PRE_DISPATCH_EXEC(
  ),
  CMD_POST_DISPATCH_EXEC(
    resp_ctx->res = res;
  ),
  CMD_PRE_RESP(
    uint64_t res = ctx->res;
  ),
  CMD_POST_RESP()
)

DEFINE_COMMAND(
  incrby,
  KEYED_REQ_CTX(
    int64_t delta;
  ),
  RESP_CTX(
    uint64_t res;
  ),
  CMD_VARS(
    uint8_t *key = malloc(keylen);
    memcpy(key, key_buf_ptr, keylen);

    int64_t delta = 0;
    if (!try_parse_signed_integer(args->buf + args->offsets[2], args->lens[2], &delta)) {
      write_simple_generic_error(conn, "value is not an integer or out of range");
      return;
    }

    if (delta < 0) {
      write_simple_generic_error(conn, "increment would produce negative integer");
      return;
    }
  ),
  CMD_PRE_INLINE_EXEC(),
  CMD_EXEC(
    uint64_t res = 0;
    try_modify_counter(shard, conn, key, keylen, hash, &res, delta);
  ),
  CMD_RESP(
    write_integer(conn, res);
  ),
  CMD_PRE_DISPATCH(
    ctx->ctx.key = key;
    ctx->ctx.keylen = keylen;
    ctx->ctx.hash = hash;
    ctx->delta = delta;
  ),
  CMD_PRE_DISPATCH_EXEC(
    uint64_t delta = ctx->delta;
  ),
  CMD_POST_DISPATCH_EXEC(
    resp_ctx->res = res;
  ),
  CMD_PRE_RESP(
    uint64_t res = ctx->res;
  ),
  CMD_POST_RESP()
)

DEFINE_COMMAND(
  decrby,
  KEYED_REQ_CTX(
    int64_t delta;
  ),
  RESP_CTX(
    uint64_t res;
  ),
  CMD_VARS(
    uint8_t *key = malloc(keylen);
    memcpy(key, key_buf_ptr, keylen);

    int64_t delta = 0;
    if (!try_parse_signed_integer(args->buf + args->offsets[2], args->lens[2], &delta)) {
      write_simple_generic_error(conn, "value is not an integer or out of range");
      return;
    }

    if (delta < 0) {
      write_simple_generic_error(conn, "increment would produce negative integer");
      return;
    }
  ),
  CMD_PRE_INLINE_EXEC(),
  CMD_EXEC(
    uint64_t res = 0;
    try_modify_counter(shard, conn, key, keylen, hash, &res, -delta);
  ),
  CMD_RESP(
    write_integer(conn, res);
  ),
  CMD_PRE_DISPATCH(
    ctx->key = key;
    ctx->ctx.keylen = keylen;
    ctx->ctx.hash = hash;
    ctx->delta = delta;
  ),
  CMD_PRE_DISPATCH_EXEC(
    uint64_t delta = ctx->delta;
  ),
  CMD_POST_DISPATCH_EXEC(
    resp_ctx->res = res;
  ),
  CMD_PRE_RESP(
    uint64_t res = ctx->res;
  ),
  CMD_POST_RESP()
)