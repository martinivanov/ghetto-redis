
#include <stdlib.h>

#include "mpmcq.h"
#include "reactor.h"
#include "state.h"

void reactor_init(
    Reactor *reactor, 
    size_t id, 
    reactor_on_cb on_cb,
    reactor_on_accept on_accept,
    reactor_on_data_available on_data_available
) {
    reactor->id = id;
    reactor->running = true;

    reactor->cb_queue = malloc(sizeof(mpmcq));
    mpmcq_init(reactor->cb_queue, 8192);

    reactor->sleeping = true;
    reactor->soft_notify = 0;

    reactor->conns = (vector_Conn_ptr *)malloc(sizeof(vector_Conn_ptr));
    init_vector_Conn_ptr(reactor->conns, 128);
    for (size_t i = 0; i < capacity_vector_Conn_ptr(reactor->conns); i++) {
      reactor->conns->array[i] = NULL;
    }

    reactor->on_cb = on_cb;
    reactor->on_accept = on_accept;
    reactor->on_data_available = on_data_available;

    reactor->wakeup_fd = eventfd(0, 0); // or eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK); //TODO: check what the options are
    if (reactor->wakeup_fd == -1) {
        perror("eventfd");
        exit(1);
    }
}

void reactor_destroy(Reactor *reactor) {
    mpmcq_destroy(reactor->cb_queue);
    free(reactor->cb_queue);
    free_vector_Conn_ptr(reactor->conns);
    free(reactor->conns);
}

uint64_t reactor_poll_callbacks(Reactor *reactor, GRContext *context) {
  uint64_t count = 0;

  while (true) {
    void *ctx = mpmcq_dequeue(reactor->cb_queue);
    if (ctx == NULL) {
      break;
    }

    count++;

    reactor->on_cb(context, ctx);

    free(ctx);
  }

  return count;
}

bool reactor_has_pending_messages(Reactor *reactor) {
  size_t count = mpmcq_count(reactor->cb_queue);

  return count > 0;
}

bool reactor_send_message(Reactor *reactor, Reactor *target, void *message) {
  (void)reactor;
  return mpmcq_enqueue(target->cb_queue, message);
}

void reactor_conn_emplace(Reactor *reactor, Conn *conn) {
  vector_Conn_ptr *conns = reactor->conns;
  size_t capacity = capacity_vector_Conn_ptr(conns);
  if (capacity <= (size_t)conn->fd) {
    resize_vector_Conn_ptr(conns, conn->fd + 1);
  }

  size_t new_capacity = capacity_vector_Conn_ptr(conns);
  for (size_t i = capacity; i < new_capacity; i++) {
    conns->array[i] = NULL;
  }

  conns->array[conn->fd] = conn;
  conns->used = conns->size;
}