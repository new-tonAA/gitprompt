# gitprompt -- a distributed version control system for prompts.
#
# One external dependency, zlib, vendored under third_party/zlib so that a
# checkout builds with nothing but a C compiler and make.  On Windows use
# mingw32-make; on Unix make.

# make predefines CC=cc, so ?= would never take effect; only override the
# builtin default, not a CC the user or the environment supplied.
ifeq ($(origin CC),default)
CC := gcc
endif

PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin

CPPFLAGS += -Ithird_party/zlib
CFLAGS   ?= -O2
CFLAGS   += -Wall -Wextra -Wno-unused-parameter -std=gnu99
LDLIBS   += -Lthird_party/zlib -lz

BIN := gitprompt

ifeq ($(OS),Windows_NT)
BIN := gitprompt.exe
# Link the runtime and zlib in, so the binary runs without shipping DLLs.
LDFLAGS += -static
# `serve` and `gp://` use winsock, which the sockets come from on Windows; on
# Unix they are in libc and nothing extra is needed.
LDLIBS += -lws2_32
endif

SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:.c=.o)

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

src/%.o: src/%.c src/gp.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

test: $(BIN)
	sh test/smoke.sh

clean:
	rm -f $(OBJS) $(BIN)
	rm -rf build

.PHONY: all install test clean
