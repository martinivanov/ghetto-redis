CC = clang

CFLAGS = -luring -ggdb -fsanitize=address
TARGET = "uring-echo-server"
SOURCES = "main.c"

BUILD_CMD = $(CC) $(CFLAGS) -o $(TARGET) $(SOURCES)

build:
	$(BUILD_CMD)