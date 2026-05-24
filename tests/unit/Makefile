CC      = gcc
CFLAGS  = -Wall -Wextra -g \
          -Istubs \
          -I../../src \
          -I../../src/qemu
LDFLAGS = -lpthread

SRCS = test_dispatch.c \
       ../../src/qemu/nvkvm_objects.c \
       ../../src/qemu/nvkvm_dispatch.c \
       frontend_stubs.c

.PHONY: all clean run

all: test_dispatch

test_dispatch: $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

run: test_dispatch
	./test_dispatch

clean:
	rm -f test_dispatch
