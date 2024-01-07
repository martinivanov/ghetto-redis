#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>

void info(const char *message) { printf("[INFO] %s\n", message); }

void warn(const char *message) { printf("[WARN] %s\n", message); }

void error(const char *message) { printf("[ERROR] %s\n", message); }

void panic(const char *message) {
  printf("[PANIC] [%d] %s\n", errno, message);
  abort();
}

static struct io_uring ring;

static void fd_set_nb(int fd) {
  errno = 0;
  int flags = fcntl(fd, F_GETFL, 0);
  if (errno) {
    panic("fcntl error");
    return;
  }

  flags |= O_NONBLOCK;
  // TODO: extract this into a separate function
  flags |= O_NDELAY;

  errno = 0;
  (void)fcntl(fd, F_SETFL, flags);
  if (errno) {
    panic("fcntl error");
  }
}

enum request_type {
  ACCEPT,
  READ,
  WRITE,
  CLOSE,
};

typedef struct {
  int type;
  int socket;
  struct sockaddr_in *addr;
  socklen_t *addr_len;
  char *buf;
  int buf_len;
} request;

void add_read_request(int socket) {
  request *req = malloc(sizeof(request));
  req->type = READ;
  req->socket = socket;
  req->buf = malloc(1024);
  req->buf_len = 1024;
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  io_uring_prep_read(sqe, socket, req->buf, req->buf_len, 0);
  io_uring_sqe_set_data(sqe, req);
}

void free_request(request *req) {
  switch (req->type) {
  case READ:
    free(req->buf);
    break;
  case WRITE:
    free(req->buf);
    break;
  }

  free(req);
}

void add_accept_request(int fd_listener, struct sockaddr_in *client_addr, socklen_t *socklen) {
  request *req = malloc(sizeof(request));
  req->type = ACCEPT;
  req->addr = client_addr;
  req->addr_len = socklen;
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  io_uring_prep_accept(sqe, fd_listener, (struct sockaddr *)req->addr, req->addr_len, 0);
  io_uring_sqe_set_data(sqe, req);
}

void add_write_request(request *req) {
  req->type = WRITE;
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  io_uring_prep_write(sqe, req->socket, req->buf, req->buf_len * 2, 0);
  io_uring_sqe_set_data(sqe, req);
}

void add_close_request(request *req) {
  req->type = CLOSE;
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  io_uring_prep_close(sqe, req->socket);
  io_uring_sqe_set_data(sqe, req);
}

int main() {
  int fd_listener = socket(AF_INET, SOCK_STREAM, 0);
  int val = 1;
  setsockopt(fd_listener, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = ntohs(1337),
      .sin_addr.s_addr = ntohl(0),
  };

  int rv = bind(fd_listener, (struct sockaddr *)&addr, sizeof(addr));
  if (rv) {
    panic("bind()");
  }

  rv = listen(fd_listener, SOMAXCONN);
  if (rv) {
    panic("listen()");
  }

  fd_set_nb(fd_listener);

  io_uring_queue_init(128, &ring, 0);
  struct sockaddr_in client_addr = {};
  socklen_t socklen = sizeof(client_addr);
  add_accept_request(fd_listener, &client_addr, &socklen);
  io_uring_submit(&ring);
  struct io_uring_cqe *cqe;
  int count = 10;
  while (count-- > 0) {
    int ret = io_uring_wait_cqe(&ring, &cqe);

    request *req = (request *)cqe->user_data;
    switch (req->type) {
    case ACCEPT:
      add_accept_request(fd_listener, &client_addr, &socklen);
      add_read_request(cqe->res);
      io_uring_submit(&ring);
      free_request(req);
      break;
    case READ:
      if (cqe->res <= 0) {
        add_close_request(req);
      } else {
        add_write_request(req);
      }
      io_uring_submit(&ring);
      break;
    case WRITE:
      printf("Wrote %d bytes\n", cqe->res);
      add_read_request(req->socket);
      io_uring_submit(&ring);
      free_request(req);
      break;
    case CLOSE:
      free_request(req);
      break;
    default:
      fprintf(stderr, "Unexpected req type %d\n", req->type);
      break;
    }

    io_uring_cqe_seen(&ring, cqe);
  }
  return 0;
}
