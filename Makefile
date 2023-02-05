CC = clang
LDFLAGS = 
CFLAGS = -g -Wall -Iinclude

build-server:
	$(CC) $(CLFAGS) -o server server.c include/logging.c include/protocol.c $(LDFLAGS)

build-client:
	$(CC) $(CLFAGS) -o client client.c include/logging.c include/protocol.c $(LDFLAGS)

build: build-server build-client

all: clean build

clean:
	rm client server
