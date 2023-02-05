CC = clang
CFLAGS = -g

build-server: server

build-client: client

build: build-server build-client

all: clean build

clean:
	rm client server
