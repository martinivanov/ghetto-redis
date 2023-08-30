CC = clang

TARGET = "server"

BUILD = debug

CFLAGS.release = -O3 -march=native -flto
CFLAGS.debug = -O0 -ggdb -DDEBUG -fsanitize=address
CFLAGS.profile = -O0 -g -pg
CFLAGS=-Wall -Wextra -Wno-gnu -pthread -std=gnu2x ${CFLAGS.${BUILD}}

SOURCES=$(wildcard src/*.c) $(wildcard src/**/*.c) $(wildcard src/include/**/*.c)

BUILD_CMD = $(CC) $(CFLAGS) -o $(TARGET) $(SOURCES)

build:
	$(BUILD_CMD)

release:
	$(MAKE) BUILD=release build

debug:
	$(MAKE) BUILD=debug build

profile:
	$(MAKE) BUILD=profile build

run: build
	./$(TARGET)

run-release: release
	./$(TARGET)

run-release-numa: release
	numactl --all --physcpubind=9 --localalloc ./$(TARGET)

clean:
	rm -rf $(TARGET)
