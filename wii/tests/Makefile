# Host build of the portable core, for testing without a Wii.
#
# The core has no libogc dependency precisely so this is possible: the protocol
# code that ships to the console is the same code these tests exercise against
# a real server.

CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -Werror -O2 -g
CFLAGS  += -I../core -I../source
LDLIBS  :=

# Winsock needs an explicit link. $(OS) is not reliably exported into make's
# environment under MSYS2, so detect the platform from uname instead.
UNAME := $(shell uname -s 2>/dev/null)
ifneq (,$(findstring MINGW,$(UNAME)))
LDLIBS += -lws2_32
endif
ifneq (,$(findstring MSYS,$(UNAME)))
LDLIBS += -lws2_32
endif
ifneq (,$(findstring CYGWIN,$(UNAME)))
LDLIBS += -lws2_32
endif

SOURCES := test_core.c host_socket.c ../core/sha256.c ../core/protocol.c \
	   ../core/client.c ../core/fingerprint.c ../source/wii_fingerprint.c

test_core: $(SOURCES) ../core/sha256.h ../core/protocol.h ../core/client.h \
	   ../core/fingerprint.h host_socket.h
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(LDLIBS)

.PHONY: check clean
check: test_core
	./test_core

clean:
	rm -f test_core test_core.exe
