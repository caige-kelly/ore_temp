# Default to zig cc, but only if the user hasn't set CC themselves
# (via env var, `make CC=gcc`, etc.). The `origin` check is needed
# because Make has a built-in CC=cc that ?= won't override.
ifeq ($(origin CC),default)
  CC = zig cc
endif

CFLAGS  ?= -std=c17 -Wall -Isrc -g
LDFLAGS ?=

# Pick up Nix-provided linker flags when present; harmless when empty.
LDFLAGS += $(NIX_LDFLAGS)

TARGET = ore
SRCS   = $(shell find src -name '*.c')

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $(TARGET)

clean:
	rm -f $(TARGET)

TEST_CFLAGS ?= $(CFLAGS) -fsanitize=address -lasan $(NIX_LDFLAGS)

test:
	@CC="$(CC)" TEST_CFLAGS="$(TEST_CFLAGS)" sh tools/test.sh