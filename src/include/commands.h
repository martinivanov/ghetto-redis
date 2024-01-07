#ifndef COMMANDS_H
#define COMMANDS_H

#include "state.h"
#include "hashmap.h"
#include "kv.h"
#include "bitset.h"
#include "reactor.h"

#define VAR_ARGC (size_t)-1

typedef void (*command_func)(GRContext *context, Conn* conn, const CmdArgs* args);
typedef void (*dispatch_cb)(GRContext *context, void *ctx);

#define ALLOW_INLINE_EXEC 1

#ifndef ALLOW_INLINE_EXEC
#define ALLOW_INLINE_EXEC 1
#endif

#define DECLARE_KEY_COPY                                                   \
  uint8_t *_key = malloc(keylen);                                          \
  memcpy(_key, key, keylen);

#define UNPACK_KEYED_CTX                                                                    \
  [[maybe_unused]] KeyedCBContext *keyed_ctx = (KeyedCBContext*)ctx;                        \
  [[maybe_unused]] uint8_t *key = keyed_ctx->key;                                           \
  [[maybe_unused]] size_t keylen = keyed_ctx->keylen;                                       \
  [[maybe_unused]] uint64_t hash = keyed_ctx->hash;

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
  typedef struct                                                                                                                                                                             \
  {                                                                                                                                                                                          \
    req_t                                                                                                                                                                                    \
  } __##name##_req_t;                                                                                                                                                                        \
                                                                                                                                                                                             \
  typedef struct                                                                                                                                                                             \
  {                                                                                                                                                                                          \
    resp_t                                                                                                                                                                                   \
  } __##name##_resp_t;                                                                                                                                                                       \
                                                                                                                                                                                             \
  void __cmd_##name##_resp(GRContext *context, __##name##_resp_t *ctx)                                                                                                                       \
  {                                                                                                                                                                                          \
    [[maybe_unused]] ShardSet *shard_set = context->shard_set;                                                                                                                               \
    [[maybe_unused]] Shard *shard = context->shard;                                                                                                                                          \
    [[maybe_unused]] Reactor *reactor = shard->reactor;                                                                                                                                      \
    CBContext *cb_ctx = (CBContext *)ctx;                                                                                                                                                    \
    Conn *conn = cb_ctx->conn;                                                                                                                                                               \
    cmd_pre_resp                                                                                                                                                                             \
    cmd_resp                                                                                                                                                                                 \
    cmd_post_resp                                                                                                                                                                            \
    conn->flags &= ~DISPATCH_WAITING;                                                                                                                                                        \
  }                                                                                                                                                                                          \
  void __cmd_##name##_req(GRContext *context, __##name##_req_t *ctx)                                                                                                                         \
  {                                                                                                                                                                                          \
    [[maybe_unused]] ShardSet *shard_set = context->shard_set;                                                                                                                               \
    [[maybe_unused]] Shard *shard = context->shard;                                                                                                                                          \
    [[maybe_unused]] Reactor *reactor = shard->reactor;                                                                                                                                      \
    CBContext *cb_ctx = (CBContext *)ctx;                                                                                                                                                    \
    Conn *conn = (Conn *)cb_ctx->conn;                                                                                                                                                       \
    [[maybe_unused]] struct hashmap *db = shard->dbs[conn->db];                                                                                                                              \
    UNPACK_KEYED_CTX                                                                                                                                                                         \
    cmd_pre_dispatch_exec                                                                                                                                                                    \
    cmd_exec                                                                                                                                                                                 \
    __##name##_resp_t *resp_ctx = malloc(sizeof(__##name##_resp_t));                                                                                                                         \
    fill_req_cb_ctx((CBContext *)resp_ctx, cb_ctx->dst, cb_ctx->src, cb_ctx->conn, (dispatch_cb)__cmd_##name##_resp);                                                                        \
    cmd_post_dispatch_exec                                                                                                                                                                   \
    Shard *target_shard = cb_ctx->src;                                                                                                                                                       \
    Reactor *target_reactor = target_shard->reactor;                                                                                                                                         \
    reactor_send_message(reactor, target_reactor, resp_ctx);                                                                                                                                 \
    BITSET64_SET(reactor->soft_notify, target_reactor->id);                                                                                                                                  \
  }                                                                                                                                                                                          \
  void cmd_##name(GRContext *context, Conn *conn, const CmdArgs *args)                                                                                                                       \
  {                                                                                                                                                                                          \
    [[maybe_unused]] ShardSet *shard_set = context->shard_set;                                                                                                                               \
    [[maybe_unused]] Shard *shard = context->shard;                                                                                                                                          \
    [[maybe_unused]] Reactor *reactor = shard->reactor;                                                                                                                                      \
    const uint8_t *key_buf_ptr = args->buf + args->offsets[1];                                                                                                                               \
    const size_t keylen = args->lens[1];                                                                                                                                                     \
    const uint64_t hash = hashmap_xxhash3(key_buf_ptr, keylen, 0, 0);                                                                                                                        \
    const size_t shard_id = hash % shard_set->size;                                                                                                                                          \
    LOG_DEBUG_WITH_CTX(shard->shard_id, "dispatching %s to shard %zu", #name, shard_id);                                                                                                     \
    cmd_vars                                                                                                                                                                                 \
    if (ALLOW_INLINE_EXEC && shard_id == shard->shard_id)                                                                                                                                     \
    {                                                                                                                                                                                        \
      [[maybe_unused]] struct hashmap *db = shard->dbs[conn->db];                                                                                                                            \
      cmd_pre_inline_exec                                                                                                                                                                    \
      cmd_exec                                                                                                                                                                               \
      cmd_resp                                                                                                                                                                               \
    }                                                                                                                                                                                        \
    else                                                                                                                                                                                     \
    {                                                                                                                                                                                        \
      Shard *target_shard = &shard_set->shards[shard_id];                                                                                                                                    \
      Reactor *target_reactor = target_shard->reactor;                                                                                                                                       \
      __##name##_req_t *ctx = malloc(sizeof(__##name##_req_t));                                                                                                                              \
      fill_req_cb_ctx((CBContext *)ctx, shard, target_shard, conn, (dispatch_cb)__cmd_##name##_req);                                                                                         \
      cmd_pre_dispatch if (reactor_send_message(reactor, target_reactor, ctx))                                                                                                               \
      {                                                                                                                                                                                      \
        conn->flags |= DISPATCH_WAITING;                                                                                                                                                     \
        BITSET64_SET(reactor->soft_notify, target_reactor->id);                                                                                                                              \
      }                                                                                                                                                                                      \
      else                                                                                                                                                                                   \
      {                                                                                                                                                                                      \
        write_simple_generic_error(conn, "shard dispatch queue full");                                                                                                                       \
      }                                                                                                                                                                                      \
    }                                                                                                                                                                                        \
  }

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

void cmd_ping(GRContext *context, Conn *conn, const CmdArgs *args);
void cmd_echo(GRContext *context, Conn *conn, const CmdArgs *args);
void cmd_quit(GRContext *context, Conn *conn, const CmdArgs *args);
void cmd_get(GRContext *context, Conn *conn, const CmdArgs *args);
void cmd_set(GRContext *context, Conn *conn, const CmdArgs *args);
void cmd_del(GRContext *context, Conn *conn, const CmdArgs *args);
void cmd_shutdown(GRContext *context, Conn *conn, const CmdArgs *args);
void cmd_flushall(GRContext *context, Conn *conn, const CmdArgs *args);
void cmd_select(GRContext *context, Conn *conn, const CmdArgs *args);
void cmd_incr(GRContext *context, Conn *conn, const CmdArgs *args);
void cmd_decr(GRContext *context, Conn *conn, const CmdArgs *args);
void cmd_incrby(GRContext *context, Conn *conn, const CmdArgs *args);
void cmd_decrby(GRContext *context, Conn *conn, const CmdArgs *args);
void cmd_clients(GRContext *context, Conn *conn, const CmdArgs *args);
void cmd_mget(GRContext *context, Conn *conn, const CmdArgs *args);
void cmd_mset(GRContext *context, Conn *conn, const CmdArgs *args);
void cmd_dispatch_ping(GRContext *context, Conn *conn, const CmdArgs *args);

#endif