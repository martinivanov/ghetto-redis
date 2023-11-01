#ifndef COMMANDS_H
#define COMMANDS_H

#include "state.h"
#include "hashmap.h"
#include "kv.h"

#define VAR_ARGC (size_t)-1

typedef void (*command_func)(Shard *shard, Conn* conn, const CmdArgs* args);
typedef void (*dispatch_cb)(Shard *shard, void *ctx);

// DEFINE_COMMAND(
//   template, 
//   TemplateReq, 
//   TemplateResp,
//   EMPTY_CMD_VARS,
//   EMPTY_CMD_VARS_INIT,
//   EMPTY_CMD_PRE_INLINE_EXEC,
//   EMPTY_CMD_PRE_DISPATCH,
//   EMPTY_CMD_PRE_EXEC,
//   EMPTY_CMD_EXEC,
//   EMPTY_CMD_POST_EXEC,
//   EMPTY_CMD_PRE_RESP,
//   EMPTY_CMD_RESP,
//   EMPTY_CMD_POST_RESP
// )

// void __cmd_template_resp(Shard *shard, TemplateResp *ctx)
// {
//   CBContext *cb_ctx = (CBContext *)ctx;
//   Conn *conn = cb_ctx->conn;
//   {
//   }
//   {
//   }
//   {
//   }
//   {
//   }
//   conn->state &= ~DISPATCH_WAITING;
//   write(shard->queue_efd, &(uint64_t){1}, sizeof(uint64_t));
// }
// void __cmd_template_req(Shard *shard, TemplateReq *ctx)
// {
//   CBContext *cb_ctx = (CBContext *)ctx;
//   Conn *conn = (Conn *)cb_ctx->conn;
//   struct hashmap *db = shard->dbs[conn->db];
//   {
//       // declare variables
//   }
//   KeyedCBContext *keyed_ctx = (KeyedCBContext *)ctx;
//   uint8_t *key = keyed_ctx->key;
//   size_t keylen = keyed_ctx->keylen;
//   uint64_t hash = keyed_ctx->hash;
//   {
//      // pre
//   }
//   {
//   }
//   TemplateResp *resp_ctx = malloc(sizeof(TemplateResp));
//   fill_req_cb_ctx((CBContext *)resp_ctx, cb_ctx->dst, cb_ctx->src, cb_ctx->conn, (dispatch_cb)__cmd_template_resp);
//   {
//   }
//   mpscq_enqueue(cb_ctx->src->cb_queue, resp_ctx);
//   write(cb_ctx->src->queue_efd, &(uint64_t){1}, sizeof(uint64_t));
// }
// void cmd_template(Shard *shard, Conn *conn, const CmdArgs *args)
// {
//   const GRState *gr_state = shard->gr_state;
//   const uint8_t *key = args->buf + args->offsets[1];
//   const size_t keylen = args->lens[1];
//   const uint64_t hash = hashmap_xxhash3(key, keylen, 0, 0);
//   const size_t shard_id = hash % gr_state->num_shards;
//   {
//      // declare variables
//   }
//   {
//      // initialize variables
//   }
//   if (shard_id == shard->shard_id)
//   {
//     struct hashmap *db = shard->dbs[conn->db];
//     {
//        // pre inline execution
//     }
//     {
//        // inline execution
//     }
//     {
//        // post inline execution
//     }
//   }
//   else
//   {
//     Shard *target_shard = &gr_state->shards[shard_id];
//     TemplateReq *ctx = malloc(sizeof(TemplateReq));
//     fill_req_cb_ctx((CBContext *)ctx, shard, target_shard, conn, (dispatch_cb)__cmd_template_req);
//     {
//        // pre dispatch
//     }
//     if (mpscq_enqueue(target_shard->cb_queue, ctx))
//     {
//       conn->state |= DISPATCH_WAITING;
//       write(target_shard->queue_efd, &(uint64_t){1}, sizeof(uint64_t));
//     }
//     else
//     {
//       write_simple_generic_error(conn, "shard dispatch queue full");
//     }
//   }
// }


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

#define DEFINE_COMMAND(name, req_t, resp_t, cmd_vars, cmd_pre_inline_exec, cmd_exec, cmd_resp, cmd_pre_dispatch, cmd_pre_dispatch_exec, cmd_post_dispatch_exec, cmd_pre_resp, cmd_post_resp) \
  void __cmd_##name##_resp(Shard *shard, resp_t *ctx)                    \
  {                                                                       \
    CBContext *cb_ctx = (CBContext *)ctx;                                \
    Conn *conn = cb_ctx->conn;                                            \
    cmd_pre_resp                                                           \
    cmd_resp                                                              \
    cmd_post_resp                                                         \
    conn->state &= ~DISPATCH_WAITING;                                     \
    write(shard->queue_efd, &(uint64_t){1}, sizeof(uint64_t));              \
  }\
  void __cmd_##name##_req(Shard *shard, req_t *ctx)                       \
  {                                                                       \
    CBContext *cb_ctx = (CBContext *)ctx;                                 \
    Conn *conn = (Conn *)cb_ctx->conn;                                    \
    struct hashmap *db = shard->dbs[conn->db];                        \
    UNPACK_KEYED_CTX                                                      \
    cmd_pre_dispatch_exec                                                           \
    cmd_exec                                                              \
    resp_t *resp_ctx = malloc(sizeof(resp_t));                           \
    fill_req_cb_ctx((CBContext *)resp_ctx, cb_ctx->dst, cb_ctx->src, cb_ctx->conn, (dispatch_cb)__cmd_##name##_resp); \
    cmd_post_dispatch_exec                                                       \
    mpscq_enqueue(cb_ctx->src->cb_queue, resp_ctx);\
    write(cb_ctx->src->queue_efd, &(uint64_t){1}, sizeof(uint64_t));\
  }                                                                       \
  void cmd_##name(Shard *shard, Conn *conn, const CmdArgs *args)          \
  {                                                                       \
    const GRState *gr_state = shard->gr_state;                            \
    const uint8_t *key_buf_ptr = args->buf + args->offsets[1];                    \
    const size_t keylen = args->lens[1];                                  \
    const uint64_t hash = hashmap_xxhash3(key_buf_ptr, keylen, 0, 0);             \
    const size_t shard_id = hash % gr_state->num_shards;                  \
    cmd_vars                                                              \
    if (shard_id == shard->shard_id) {                                    \
      struct hashmap *db = shard->dbs[conn->db];                        \
      cmd_pre_inline_exec                                                 \
      cmd_exec                                                           \
      cmd_resp                                                           \
    } else {                                                              \
      Shard *target_shard = &gr_state->shards[shard_id];                   \
      req_t *ctx = malloc(sizeof(req_t));                                 \
      fill_req_cb_ctx((CBContext *)ctx, shard, target_shard, conn, (dispatch_cb)__cmd_##name##_req); \
      cmd_pre_dispatch                                                            \
      if (mpscq_enqueue(target_shard->cb_queue, ctx)) {                   \
        conn->state |= DISPATCH_WAITING;\
        write(target_shard->queue_efd, &(uint64_t){1}, sizeof(uint64_t));\
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

typedef struct {
  KeyedCBContext ctx;
} GetShardReq;

typedef struct {
  CBContext ctx;
  Entry *entry;
} GetShardResp;

typedef struct {
  KeyedCBContext ctx;
  uint8_t *val;
  size_t vallen;
} SetShardReq;

typedef struct {
  CBContext ctx;
} SimpleOKResp;

typedef struct {
  KeyedCBContext ctx;
} DelShardReq;

typedef struct {
  CBContext ctx;
  int64_t val;
} IntegerResp;

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

#endif