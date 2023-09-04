#ifndef COMMANDS_H
#define COMMANDS_H

#include "state.h"
#include "hashmap.h"

typedef void (*command_func)(State *state, Conn* conn, CmdArgs* args);

typedef struct {
    size_t name_len;
    uint8_t *name;
    size_t arity;
    command_func func;
} Command;

struct hashmap* init_commands();
void free_commands(struct hashmap* commands);
void register_command(struct hashmap* commands, const char* name, size_t arity, command_func func);

command_func get_command(const uint8_t *cmd, size_t cmdlen);

void cmd_ping(State *state, Conn *conn, CmdArgs *args);
void cmd_echo(State *state, Conn *conn, CmdArgs *args);
void cmd_quit(State *state, Conn *conn, CmdArgs *args);
void cmd_get(State *state, Conn *conn, CmdArgs *args);
void cmd_set(State *state, Conn *conn, CmdArgs *args);
void cmd_del(State *state, Conn *conn, CmdArgs *args);
void cmd_shutdown(State *state, Conn *conn, CmdArgs *args);
void cmd_flushall(State *state, Conn *conn, CmdArgs *args);
void cmd_select(State *state, Conn *conn, CmdArgs *args);
void cmd_incr(State *state, Conn *conn, CmdArgs *args);
void cmd_decr(State *state, Conn *conn, CmdArgs *args);
void cmd_incrby(State *state, Conn *conn, CmdArgs *args);
void cmd_decrby(State *state, Conn *conn, CmdArgs *args);

#endif
