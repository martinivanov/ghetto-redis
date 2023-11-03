#ifndef COMMANDS_H
#define COMMANDS_H

#include "state.h"
#include "hashmap.h"
#include "kv.h"

#define VAR_ARGC (size_t)-1

typedef void (*command_func)(Shard *shard, Conn* conn, const CmdArgs* args);
typedef void (*dispatch_cb)(Shard *shard, void *ctx);

#define DECLARE_KEY_COPY                                                   \
  uint8_t *_key = malloc(keylen);                                          \
  memcpy(_key, key, keylen);

#define UNPACK_KEYED_CTX                                                   \
  KeyedCBContext *keyed_ctx = (KeyedCBContext*)ctx;                        \
  uint8_t *key = keyed_ctx->key;                                           \
  size_t keylen = keyed_ctx->keylen;                                       \
  uint64_t hash = keyed_ctx->hash;

#define CMD_VARS(block) block
#define CMD_PRE_INLINE_EXEC(block) block
#define CMD_EXEC(block) block
#define CMD_RESP(block) block
#define CMD_PRE_DISPATCH(block) block
#define CMD_PRE_DISPATCH_EXEC(block) block
#define CMD_POST_DISPATCH_EXEC(block) block
#define CMD_PRE_RESP(block) block
#define CMD_POST_RESP(block) block

#define REQ_CTX(fields) \
  CBContext ctx; \
  fields

#define KEYED_REQ_CTX(fields) \
  KeyedCBContext ctx; \
  fields

#define RESP_CTX(fields) \
  CBContext ctx; \
  fields

#define DEFINE_COMMAND(name, req_t, resp_t, cmd_vars, cmd_pre_inline_exec, cmd_exec, cmd_resp, cmd_pre_dispatch, cmd_pre_dispatch_exec, cmd_post_dispatch_exec, cmd_pre_resp, cmd_post_resp) \
  typedef struct {                                                        \
    req_t                                                                 \
  } __##name##_req_t;                                                     \
\
  typedef struct {                                                            \
    resp_t                                                                \
  } __##name##_resp_t;                                                    \
  \
  void __cmd_##name##_resp(Shard *shard, __##name##_resp_t *ctx)                    \
  {                                                                       \
    CBContext *cb_ctx = (CBContext *)ctx;                                \
    Conn *conn = cb_ctx->conn;                                            \
    cmd_pre_resp                                                           \
    cmd_resp                                                              \
    cmd_post_resp                                                         \
    conn->state &= ~DISPATCH_WAITING;                                     \
    atomic_store(&shard->notify_cb, true);                                 \
  }\
  void __cmd_##name##_req(Shard *shard, __##name##_req_t *ctx)                       \
  {                                                                       \
    CBContext *cb_ctx = (CBContext *)ctx;                                 \
    Conn *conn = (Conn *)cb_ctx->conn;                                    \
    struct hashmap *db = shard->dbs[conn->db];                        \
    UNPACK_KEYED_CTX                                                      \
    cmd_pre_dispatch_exec                                                           \
    cmd_exec                                                              \
    __##name##_resp_t *resp_ctx = malloc(sizeof(__##name##_resp_t));                           \
    fill_req_cb_ctx((CBContext *)resp_ctx, cb_ctx->dst, cb_ctx->src, cb_ctx->conn, (dispatch_cb)__cmd_##name##_resp); \
    cmd_post_dispatch_exec                                                       \
    mpscq_enqueue(cb_ctx->src->cb_queue, resp_ctx);\
    atomic_store(&cb_ctx->src->notify_cb, true);                          \
  }                                                                       \
  void cmd_##name(Shard *shard, Conn *conn, const CmdArgs *args)          \
  {                                                                       \
    const GRState *gr_state = shard->gr_state;                            \
    const uint8_t *key_buf_ptr = args->buf + args->offsets[1];                    \
    const size_t keylen = args->lens[1];                                  \
    const uint64_t hash = hashmap_xxhash3(key_buf_ptr, keylen, 0, 0);             \
    const size_t shard_id = hash % gr_state->num_shards;                  \
    LOG_DEBUG_WITH_CTX(shard->shard_id, "dispatching %s to shard %zu", #name, shard_id); \
    cmd_vars                                                              \
    if (shard_id == shard->shard_id) {                                    \
      struct hashmap *db = shard->dbs[conn->db];                        \
      cmd_pre_inline_exec                                                 \
      cmd_exec                                                           \
      cmd_resp                                                           \
    } else {                                                              \
      Shard *target_shard = &gr_state->shards[shard_id];                   \
      __##name##_req_t *ctx = malloc(sizeof(__##name##_req_t));                                 \
      fill_req_cb_ctx((CBContext *)ctx, shard, target_shard, conn, (dispatch_cb)__cmd_##name##_req); \
      cmd_pre_dispatch                                                            \
      if (mpscq_enqueue(target_shard->cb_queue, ctx)) {                   \
        conn->state |= DISPATCH_WAITING;\
        atomic_store(&target_shard->notify_cb, true);                       \
      } else {\
        write_simple_generic_error(conn, "shard dispatch queue full");\
      }\
    }                                                                     \
  }                                                                       \

typedef struct {
    size_t name_len;
    uint8_t *name;
    size_t arity;
    command_func func;
} Command;

typedef struct {
  Shard *src;
  Shard *dst;
  Conn *conn;
  dispatch_cb cb;
} CBContext;

typedef struct {
    CBContext base;
    uint8_t *key;
    size_t keylen;
    uint64_t hash;
} KeyedCBContext;

struct hashmap* init_commands();
void free_commands(struct hashmap* commands);
void register_command(struct hashmap* commands, const char* name, size_t arity, command_func func);

const Command* lookup_command(CmdArgs *CmdArgs, struct hashmap* commands);

void cmd_ping(Shard *shard, Conn *conn, const CmdArgs *args);
void cmd_echo(Shard *shard, Conn *conn, const CmdArgs *args);
void cmd_quit(Shard *shard, Conn *conn, const CmdArgs *args);
void cmd_get(Shard *shard, Conn *conn, const CmdArgs *args);
void cmd_set(Shard *shard, Conn *conn, const CmdArgs *args);
void cmd_del(Shard *shard, Conn *conn, const CmdArgs *args);
void cmd_shutdown(Shard *shard, Conn *conn, const CmdArgs *args);
void cmd_flushall(Shard *shard, Conn *conn, const CmdArgs *args);
void cmd_select(Shard *shard, Conn *conn, const CmdArgs *args);
void cmd_incr(Shard *shard, Conn *conn, const CmdArgs *args);
void cmd_decr(Shard *shard, Conn *conn, const CmdArgs *args);
void cmd_incrby(Shard *shard, Conn *conn, const CmdArgs *args);
void cmd_decrby(Shard *shard, Conn *conn, const CmdArgs *args);
void cmd_clients(Shard *shard, Conn *conn, const CmdArgs *args);
void cmd_mget(Shard *shard, Conn *conn, const CmdArgs *args);
void cmd_mset(Shard *shard, Conn *conn, const CmdArgs *args);
void cmd_dispatch_ping(Shard *shard, Conn *conn, const CmdArgs *args);

#endif