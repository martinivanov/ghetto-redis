#ifndef COMMANDS_H
#define COMMANDS_H

#include "state.h"
#include "hashmap.h"
#include "kv.h"

#define VAR_ARGC (size_t)-1

typedef void (*command_func)(Shard *shard, Conn* conn, const CmdArgs* args);

typedef struct {
    size_t name_len;
    uint8_t *name;
    size_t arity;
    command_func func;
} Command;

typedef struct {
  void (*cb)(void *);
} Callback;

typedef struct {
  Callback base;
  Shard *original_shard;
  Shard *target_shard;
  Conn *conn;
  CmdArgs *args;
} GetShardReq;

typedef struct {
  Callback base;
  Shard *shard;
  Conn *conn;
  Entry *entry;
} GetShardResp;

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