#ifndef REACTOR_H
#define REACTOR_H

#ifndef REACTOR_EPOLL
#define REACTOR_EPOLL
#endif

#include <stddef.h>
#include <sys/eventfd.h>
#include <stdatomic.h>

#include "bitset.h"
#include "vector_types.h"
#include "state.h"
#include "vyukov_mpsc.h"

// typedef all callbacks
typedef void (*reactor_on_cb) (Reactor *reactor, void *arg);
typedef Conn *(*reactor_on_accept) (Reactor *reactor, struct sockaddr_in, int client_fd);
typedef bool (*reactor_on_data_available) (GRContext *context, Conn *conn);

void reactor_init(
    Reactor *reactor, 
    size_t id,
    reactor_on_cb on_cb, 
    reactor_on_accept on_accept, 
    reactor_on_data_available on_data_available);

void reactor_destroy(Reactor *reactor);

void reactor_run(Reactor *reactor, GRContext *context);
bool reactor_wakeup_pending(Reactor *reactor, GRContext *context);
uint64_t reactor_poll_callbacks(Reactor *reactor, GRContext *context);
bool reactor_send_message(Reactor *reactor, Reactor *target, void *message);
bool reactor_has_pending_messages(Reactor *reactor);

struct Reactor {
    size_t id;
    bool running;
    int wakeup_fd;
    mpscq_t *cb_queue;
    bitset64 soft_notify;
    atomic_bool sleeping;

    vector_Conn_ptr *conns;

    // callbacks
    reactor_on_cb on_cb;
    reactor_on_accept on_accept;
    reactor_on_data_available on_data_available;
};

#endif // REACTOR_H