
#include <stdlib.h>

#include "mpmcq.h"
#include "reactor.h"
#include "state.h"

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