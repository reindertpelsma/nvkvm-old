# Makefile — nvkvm isolate stub binary
#
# Produces a fully static, position-independent executable with no libc
# dependency. After build, nvkvm_stub_bin.h is generated for embedding
# into the QEMU backend.

CC      ?= gcc
STRIP   ?= strip
XXD     ?= xxd

CFLAGS  := -O2 -g -std=c11 \
           -nostdlib -static -fPIE \
           -Wall -Wextra -Wno-unused-parameter \
           -I../.. \
           -I../../src

LDFLAGS := -static -pie -Wl,-z,relro,-z,now

TARGET  := nvkvm_stub
BIN_H   := nvkvm_stub_bin.h

.PHONY: all clean

all: $(TARGET) $(BIN_H)

$(TARGET): nvkvm_stub.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -lgcc

$(BIN_H): $(TARGET)
	$(STRIP) --strip-all $(TARGET)
	$(XXD) -i $(TARGET) > $@

clean:
	rm -f $(TARGET) $(BIN_H)
