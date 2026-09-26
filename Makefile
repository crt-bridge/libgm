# SPDX-License-Identifier: GPL-3.0-or-later
# Without this line, `make`'s default goal (no target given) is the FIRST real
# target in the file -- and $(LIB_OBJS): $(LIB_HDRS) (a rule with no recipe,
# further down) became that in effect, ahead of `all:` itself. A bare `make`
# then built only a single object, never the library. A PRE-EXISTING bug
# (reproduced on the Makefile from before this feature, not introduced by the
# POSIX port); fixed here because it blocks this same task's Windows
# non-regression check.
.DEFAULT_GOAL := all

UNAME := $(shell uname -s)

CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -O2 -Iinclude -Isrc

ifeq ($(UNAME),Darwin)
  GM_POSIX := 1
  CC      ?= clang
  AR      ?= ar
  LDFLAGS ?= -lpthread
else
ifeq ($(UNAME),Linux)
  GM_POSIX := 1
  CC      ?= gcc
  AR      ?= ar
  LDFLAGS ?= -lpthread
  # glibc under strict -std=c11
  # declares neither clock_gettime/CLOCK_MONOTONIC nor select() without a
  # feature-test macro. MEASURED on the POSIX build box.
  CFLAGS  += -D_POSIX_C_SOURCE=200809L
else
  # MinGW (and any platform with no uname): historical values, unchanged to the byte.
  CC      ?= x86_64-w64-mingw32-gcc
  AR      ?= x86_64-w64-mingw32-ar
  LDFLAGS ?= -lws2_32 -lwinmm
endif
endif

LIB_SRCS = src/gm_socket.c src/gm.c src/gm_proto.c src/gm_heartbeat.c src/gm_input.c src/lz4.c
LIB_OBJS = $(LIB_SRCS:.c=.o)
LIB      = libgm.a
DLL      = libgm.dll

# lz4.c is third-party code, not written for -Wconversion/-Wpedantic. It
# compiles without error (no -Werror) but drowns the signal from the OTHER
# files under hundreds of lines. The relaxation targets this one unit only:
# the rest of libgm keeps the full discipline.
src/lz4.o: CFLAGS += -Wno-conversion -Wno-sign-conversion -Wno-pedantic

# Every object in LIB_OBJS shares gm_internal.h (struct gm_handle): a TU
# compiled in gm_proto.c that writes a new field of this struct, without
# recompiling gm.c (which ALLOCATES it via sizeof(struct gm_handle)), writes
# out of bounds of a handle that is too small -- SILENT heap corruption (no
# compile error, no warning), which only reveals itself on a later, random
# crash (here: gm_close(), STATUS_HEAP_CORRUPTION 0xc0000374, when
# last_status_from/has_status_from were added). The implicit %.o rule carried
# NO dependency on the shared headers: `make` only recompiled the modified .c
# file. Same lesson as BRIDGE_HDRS below, applied here to the library itself.
LIB_HDRS = include/gm.h src/gm_internal.h src/gm_socket.h src/gm_hb_compat.h src/lz4.h
$(LIB_OBJS): $(LIB_HDRS)

SMOKE_SRC = test/libgm_smoke.c
SMOKE_BIN = libgm_smoke.exe

# test_select_nfds — the defect this test guards against is SILENT: select(0, …)
# on POSIX returns 0 forever, the binary compiles, starts, and never receives
# anything. The test includes gm_socket.c directly to reach a static function
# without exporting it; it opens NO socket.
SELECT_TEST_SRC = test/test_select_nfds.c
ifeq ($(GM_POSIX),1)
SELECT_TEST_BIN = test_select_nfds
else
SELECT_TEST_BIN = test_select_nfds.exe
endif

# libgm_input_smoke.exe — host-side proof for the Groovy input channel
# (UDP/32101). Same pattern as SMOKE_BIN: same CFLAGS (hence -Isrc, which
# makes gm_internal.h accessible to the test), linked against $(LIB) and
# -lws2_32.
INPUT_SMOKE_SRC = test/libgm_input_smoke.c
INPUT_SMOKE_BIN = libgm_input_smoke.exe

# Two optional test tools need sources that live outside this repository.
# Each is built only when its source folder is found; otherwise its rule
# prints "skipped" and succeeds, so `make` works in a standalone checkout.
# Point these variables at the sources to build them.
#   EMITTER_SRC  -- the RetroArch driver's groovy_*.h headers (the same
#                   headers as record/drivers/ in the crt-bridge/RetroArch fork)
#   RECEIVER_SRC -- the receiver's protocol parser, proto.c and proto.h
EMITTER_SRC  ?= ../emitter/retroarch/src
RECEIVER_SRC ?= ../receiver

# groovy_test_bridge.dll — Python ctypes test harness for the RetroArch
# driver's logic, called by the emitter's test suite. Needs EMITTER_SRC.
BRIDGE_SRC = test/groovy_test_bridge.c
BRIDGE_DLL = groovy_test_bridge.dll
BRIDGE_CFLAGS = $(CFLAGS) -I$(EMITTER_SRC)
# The headers the bridge includes are full-fledged prerequisites: they carry
# all the tested logic. Without this line, changing a default in
# groovy_audio.h leaves the DLL untouched, and the tests judge the old code
# while reporting the new one as failing.
BRIDGE_DRIVER_HDRS = $(wildcard $(EMITTER_SRC)/groovy_*.h)
BRIDGE_HDRS = $(BRIDGE_DRIVER_HDRS) include/gm.h

# proto_probe.dll — the receiver's REAL parser, callable from the sender's
# tests. Compiles the receiver's parser source directly (RECEIVER_SRC): the
# verdict handed to the tests is the one from the code that runs on the
# receiver, never from a copy.
PROBE_SRC    = test/proto_probe.c
PROBE_DLL    = proto_probe.dll
# -Isrc: lz4.h lives in src/ (embedded) — the probe links against it via
# $(LIB) rather than duplicating the header or requiring an lz4 package.
PROBE_CFLAGS = $(CFLAGS) -I$(RECEIVER_SRC) -Isrc
PROBE_HDRS   = $(RECEIVER_SRC)/proto.h src/lz4.h
PROBE_PARSER = $(wildcard $(RECEIVER_SRC)/proto.c)

# test_uso_fallback -- FAILURE paths of the split send, via setsockopt/
# getsockopt/sendto stand-ins: the test includes gm_socket.c (pattern from
# test_select_nfds) and links against the objects from gm.c and gm_proto.c as
# is. Windows only (UDP_SEND_MSG_SIZE); also run by the emitter's Python
# test suite.
USO_TEST_SRC  = test/test_uso_fallback.c
USO_TEST_BIN  = test_uso_fallback.exe
USO_TEST_OBJS = src/gm.o src/gm_proto.o src/gm_heartbeat.o src/gm_input.o src/lz4.o

# test_gm_seq — clean-room rewrite: a direct unit test of
# gm_seq_is_newer() against its documented case table. No link against
# $(LIB): gm_seq.h is self-contained (static inline, only <stdint.h>).
SEQ_TEST_SRC = test/test_gm_seq.c
ifeq ($(GM_POSIX),1)
SEQ_TEST_BIN = test_gm_seq
else
SEQ_TEST_BIN = test_gm_seq.exe
endif

.PHONY: all test clean seq-test

# On POSIX, `all` is limited to the static library, and this is a deliberate
# choice:
#   - $(DLL), $(BRIDGE_DLL) and $(PROBE_DLL) link with -Wl,--out-implib, a
#     MinGW-specific flag;
#   - $(BRIDGE_DLL) and $(PROBE_DLL) are ONLY used by the sender fork's
#     ctypes tests, out of scope on POSIX;
#   - $(SMOKE_BIN) and $(INPUT_SMOKE_BIN) carry a .exe name.
# None of this is needed by gmsource, the only POSIX consumer of libgm.
ifeq ($(GM_POSIX),1)
all: $(LIB)
else
all: $(LIB) $(DLL) $(SMOKE_BIN) $(INPUT_SMOKE_BIN) $(BRIDGE_DLL) $(PROBE_DLL) $(USO_TEST_BIN)
endif

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

# Shared library variant — Python ctypes consumes this. The --out-implib
# side-product is the import lib for any future MinGW C/C++ caller.
$(DLL): $(LIB_OBJS)
	$(CC) -shared -o $@ $^ $(LDFLAGS) -Wl,--out-implib,libgm.dll.a

$(SMOKE_BIN): $(SMOKE_SRC) $(LIB)
	$(CC) $(CFLAGS) -o $@ $(SMOKE_SRC) $(LIB) $(LDFLAGS)

$(INPUT_SMOKE_BIN): $(INPUT_SMOKE_SRC) $(LIB)
	$(CC) $(CFLAGS) -o $@ $(INPUT_SMOKE_SRC) $(LIB) $(LDFLAGS)

# test_select_nfds — the defect this test guards against is SILENT: select(0, …)
# on POSIX returns 0 forever, the binary compiles, starts, and never receives
# anything. The test includes gm_socket.c directly to reach a static function
# without exporting it; it opens NO socket. Rule placed AFTER `all:` -- a rule
# defined earlier becomes `make`'s default goal with no argument (found while
# checking the Windows build).
$(SELECT_TEST_BIN): $(SELECT_TEST_SRC) src/gm_socket.c src/gm_socket.h
	$(CC) $(CFLAGS) -o $@ $(SELECT_TEST_SRC) $(LDFLAGS)

$(USO_TEST_BIN): $(USO_TEST_SRC) src/gm_socket.c $(LIB_HDRS) $(USO_TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $(USO_TEST_SRC) $(USO_TEST_OBJS) $(LDFLAGS)

$(SEQ_TEST_BIN): $(SEQ_TEST_SRC) src/gm_seq.h
	$(CC) $(CFLAGS) -o $@ $(SEQ_TEST_SRC)

seq-test: $(SEQ_TEST_BIN)
	./$(SEQ_TEST_BIN)

# groovy_test_bridge.dll links against libgm.a so it can call gm_init /
# gm_send_switchres / gm_close. The include path above makes the driver's
# headers available without copying them.
ifneq ($(BRIDGE_DRIVER_HDRS),)
$(BRIDGE_DLL): $(BRIDGE_SRC) $(BRIDGE_HDRS) $(LIB)
	$(CC) -shared $(BRIDGE_CFLAGS) -o $@ $(BRIDGE_SRC) $(LIB) $(LDFLAGS) \
	    -Wl,--out-implib,$(BRIDGE_DLL).a
else
.PHONY: $(BRIDGE_DLL)
$(BRIDGE_DLL):
	@echo "skipped $(BRIDGE_DLL): no groovy_*.h in EMITTER_SRC=$(EMITTER_SRC)"
endif

# Do NOT link $(LDFLAGS) (-lws2_32 -lwinmm): the receiver's parser and lz4.c
# (via $(LIB)) need nothing more.
ifneq ($(PROBE_PARSER),)
$(PROBE_DLL): $(PROBE_SRC) $(PROBE_HDRS) $(PROBE_PARSER) $(LIB)
	$(CC) -shared $(PROBE_CFLAGS) -o $@ $(PROBE_SRC) $(PROBE_PARSER) $(LIB) \
	    -Wl,--out-implib,$(PROBE_DLL).a
else
.PHONY: $(PROBE_DLL)
$(PROBE_DLL):
	@echo "skipped $(PROBE_DLL): no proto.c in RECEIVER_SRC=$(RECEIVER_SRC)"
endif

ifeq ($(GM_POSIX),1)
test: $(SELECT_TEST_BIN)
	./$(SELECT_TEST_BIN)
else
test: $(SMOKE_BIN) $(SELECT_TEST_BIN) $(USO_TEST_BIN)
	./$(SELECT_TEST_BIN)
	./$(USO_TEST_BIN)
	@echo "libgm_smoke.exe built — run it against a live receiver"
endif

clean:
	rm -f $(LIB) $(DLL) libgm.dll.a $(LIB_OBJS) $(SMOKE_BIN) $(INPUT_SMOKE_BIN) \
	    $(BRIDGE_DLL) $(BRIDGE_DLL).a $(PROBE_DLL) $(PROBE_DLL).a $(SELECT_TEST_BIN) \
	    $(SEQ_TEST_BIN) $(USO_TEST_BIN)
