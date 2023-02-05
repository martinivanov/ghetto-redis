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

const size_t MESSAGE_MAX_LENGTH = 4096;
const size_t MESSAGE_HEADER_LENGTH = 4;

void info(const char* message) {
    printf("[INFO] %s\n", message);
}

void warn(const char* message) {
    printf("[WARN] %s\n", message);
}

void error(const char* message) {
    printf("[ERROR] %s\n", message);
}

void panic(const char* message) {
    printf("[PANIC] [%d] %s\n", errno, message);
    abort();
}

static int32_t read_full(int fd, char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = read(fd, buf, n);
        if (rv <= 0) {
            return -1;
        }

        assert((size_t)rv >= n);
        n -= (size_t)rv;
        buf += rv;
    }

    return 0;
}

static int32_t write_all(int fd, const char *buf, size_t n) {
    while(n > 0) {
        ssize_t rv = write(fd, buf, n);
        printf("write_all rv=%zd", rv);
        if (rv <= 0) {
            return -1;
        }
        assert((size_t)rv >= 0);
        n -= (size_t)rv;
        buf += rv;
    }

    return 0;
}

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
