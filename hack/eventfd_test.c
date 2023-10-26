#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "../src/include/mpscq.h"

static struct mpscq *q;

typedef void (*request_cb)(void *);

typedef struct {
    int64_t i;
    request_cb cb;
} Request;

void test_callback(void *arg) {
    int64_t *i = arg;
    printf("i = %ld\n", *i);
}

void *producer_func(void *arg) {
    int fd = (int)(intptr_t)arg;
    char buf[128];
    char *b = buf;
    size_t bufsize = sizeof(buf);
    int64_t i = 0;
    while (true) {
        Request *req = malloc(sizeof(Request));
        req->i = i;
        req->cb = test_callback;

        if (mpscq_enqueue(q, req) && i % 500 == 0) {
            write(fd, &i, sizeof(int64_t));
        }
        i++;
    }
    return NULL;
}

int main() {
    q = mpscq_create(NULL, 1024);

    int efd = eventfd(0, 0);

    pthread_t tid;
    pthread_create(&tid, NULL, producer_func, (void *)(uintptr_t)efd);

    while (true) {
        int64_t i;
        int res = read(efd, &i, sizeof(int64_t));
        if (res == -1) {
            perror("read");
            exit(1);
        }

        while (true) {
            Request *p = mpscq_dequeue(q);
            if (p == NULL) {
                break;
            }

            if (p->i % 1000000 == 0) {
                void *arg = &p->i;
                p->cb(arg);
            }
            free(p);
        }
    }

    return 0;
}