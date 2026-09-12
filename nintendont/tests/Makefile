# Host build of the runtime-sync engine's decisions, for testing without a Wii.
#
# SlotSyncLogic.c is compiled here by the host compiler and by devkitARM in the
# kernel -- the same file, so what these tests pass is what the console runs.
# The protocol core comes from wii/core for the same reason it does there:
# a fourth copy of the wire format is exactly what CLAUDE.md says not to add.

CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -Werror -O2 -g
CFLAGS  += -I../../wii/core
LDLIBS  :=

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

CORE    := ../../wii/core
SOURCES := test_runtime.c ../kernel/SlotSyncLogic.c \
	   $(CORE)/sha256.c $(CORE)/protocol.c $(CORE)/client.c $(CORE)/memcard.c \
	   $(CORE)/fingerprint.c ../../wii/tests/host_socket.c

test_runtime: $(SOURCES) ../kernel/SlotSyncLogic.h
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(LDLIBS)

# A card with a GALE01 save on it, so the maker-code lookup has something real
# to read. Synthetic, so treat what it proves as approximate (CLAUDE.md).
fixture.raw:
	python3 ../../server/scripts/make_fixture.py $@ --mbit 4 \
		--save 'GALE01:zelda:The Legend of Zelda:Quest Log 3:5'

.PHONY: check clean
check: test_runtime fixture.raw
	./test_runtime --card fixture.raw

clean:
	rm -f test_runtime test_runtime.exe fixture.raw
