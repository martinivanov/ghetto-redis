#ifndef COMMANDS_H
#define COMMANDS_H

#include "state.h"
#include "hashmap.h"
#include "kv.h"

#define VAR_ARGC (size_t)-1

typedef void (*command_func)(Shard *shard, Conn* conn, const CmdArgs* args);
typedef void (*dispatch_cb)(void *ctx);

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