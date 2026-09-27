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

CFLAGS   ?= -O2
CFLAGS   += -Wall -Wextra -Wno-unused-parameter -std=gnu99

BIN := gitprompt

ifeq ($(OS),Windows_NT)
BIN := gitprompt.exe
# Windows ships no zlib to link, so the vendored build of it is used, and both
# the runtime and zlib are linked in so the binary runs without shipping DLLs.
CPPFLAGS += -Ithird_party/zlib
LDFLAGS  += -static
LDLIBS   += -Lthird_party/zlib -lz
# `serve` and `gp://` use winsock, which the sockets come from on Windows; on
# Unix they are in libc and nothing extra is needed.
LDLIBS   += -lws2_32
else
# Every system git runs on already has zlib, and git links that one rather than
# carrying its own; this does the same.  The vendored copy under third_party is
# a Windows build of the library and is not an archive for these platforms, so
# neither its directory nor its header is on the search path here.
LDLIBS   += -lz
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
