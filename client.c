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

static int32_t query(int fd, const char *text) {
    uint32_t len = (uint32_t)strlen(text);
    if (len >= MESSAGE_MAX_LENGTH) {
        return -1;
    }

    char send_buf[MESSAGE_HEADER_LENGTH + MESSAGE_MAX_LENGTH];
    memcpy(send_buf, &len, MESSAGE_HEADER_LENGTH);
    memcpy(&send_buf[MESSAGE_HEADER_LENGTH], text, len);
    int32_t err = write_all(fd, send_buf, MESSAGE_HEADER_LENGTH + len);
    if (err) {
        return err;
    }

    char recv_buf[MESSAGE_HEADER_LENGTH + MESSAGE_MAX_LENGTH + 1];
    errno = 0;
    err = read_full(fd, recv_buf, MESSAGE_HEADER_LENGTH);
    if (err) {
        if (errno == 0) {
            info("EOF");
        } else {
            warn("read() error");
        }

        return err;
    }

    int32_t resp_len;
    memcpy(&resp_len, recv_buf, MESSAGE_HEADER_LENGTH);
    if (resp_len > MESSAGE_MAX_LENGTH) {
        warn("message too long");
        return -1;
    }

    err = read_full(fd, &recv_buf[MESSAGE_HEADER_LENGTH], resp_len);
    if (err) {
        info("read() error");
        return err;
    }

    recv_buf[MESSAGE_HEADER_LENGTH + resp_len] = '\0';
    char *message = &recv_buf[MESSAGE_HEADER_LENGTH];
    printf("Server said: %s\n", message);

    return 0;
}

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = ntohs(1337),
        .sin_addr.s_addr = ntohl(INADDR_LOOPBACK),
    };

    int rv = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rv) {
        panic("connect()");
    }


    int32_t err = query(fd, "hello 1");
    if (err) {
        goto done;
    }

    err = query(fd, "hello 1");
    if (err) {
        goto done;
    }

    err = query(fd, "hello 2");
    if (err) {
        goto done;
    }

    err = query(fd, "hello 3");
    if (err) {
        goto done;
    }

done:
    close(fd);
    return 0;
}
