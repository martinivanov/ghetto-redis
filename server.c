#include <asm-generic/socket.h>
#include <assert.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "include/logging.h"
#include "include/protocol.h"
#include "include/vector.h"
#include "include/vector_types.h"

int32_t handle_request(int fd) {
    char recv_buf[MESSAGE_HEADER_LENGTH + MESSAGE_MAX_LENGTH + 1];
    errno = 0;
    int32_t err = read_full(fd, recv_buf, MESSAGE_HEADER_LENGTH);
    if (err) {
        if (errno == 0) {
            info("EOF");
        } else {
            warn("read() error");
        }
    }

    uint32_t len = 0;
    memcpy(&len, recv_buf, MESSAGE_HEADER_LENGTH);
    if (len > MESSAGE_MAX_LENGTH) {
        warn("message too long");
        return -1;
    }

    err = read_full(fd, &recv_buf[MESSAGE_HEADER_LENGTH], len); // skip the header
    if (err) {
        warn("read() error");
        return -1;
    }

    recv_buf[MESSAGE_HEADER_LENGTH + len] = '\0';
    printf("client says: '%s' with length %d\n", &recv_buf[MESSAGE_HEADER_LENGTH], len);

    uint32_t send_len = strlen(&recv_buf[MESSAGE_HEADER_LENGTH]);
    char send_buf[MESSAGE_HEADER_LENGTH + send_len];
    memcpy(send_buf, &send_len, MESSAGE_HEADER_LENGTH);
    strcpy(&send_buf[MESSAGE_HEADER_LENGTH], &recv_buf[MESSAGE_HEADER_LENGTH]);
    printf("server will respond with: '%s' with length %d\n", &send_buf[MESSAGE_HEADER_LENGTH], send_len);

    return write_all(fd, send_buf, MESSAGE_HEADER_LENGTH + send_len);
}

void handle(int fd) {
    while (true) {
        int32_t err = handle_request(fd);
        if (err) {
            break;
        }
    }
}

int main() {
    vector_int32_t v;
    init_vector_int32_t(&v, 5);

    for (int i = 0; i < 100; i++) {
        insert_vector_int32_t(&v, i);
    }

    return 0;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = ntohs(1337),
        .sin_addr.s_addr = ntohl(0),
    };

    int rv = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rv) {
        panic("bind()");
    }

    rv = listen(fd, SOMAXCONN);
    if (rv) {
        panic("listen()");
    }
    
    while (true) {
        struct sockaddr_in client_addr = {};
        socklen_t socklen = sizeof(client_addr);
        int client_fd = accept(fd, (struct sockaddr *)&client_addr, &socklen);
        if (client_fd < 0) {
            continue;
        }

        handle(client_fd);
        close(client_fd);
    }

    return 0;
}
