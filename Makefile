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
# Build everything under src/ except `src/sema/type_legacy/` — the
# pruned typecheck files we keep around as a design reference while
# rebuilding the type system fresh in `src/sema/type/`. Drop the
# whole `type_legacy/` directory once the rebuild is fully replaced.
SRCS := $(shell find src -path 'src/sema/type_legacy' -prune -o -name '*.c' -print)

FORMAT = clang-format
FORMAT_FLAGS = -i -style=file --fallback-style=LLVM

.PHONY: all clean test test-determinism format

format:
	$(FORMAT) $(FORMAT_FLAGS) $(SRCS)

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $(TARGET)

clean:
	rm -f $(TARGET)

# asan's runtime library is platform-specific. macOS/clang ships it inside
# -fsanitize=address; Linux toolchains often need an explicit -lasan. Inject
# the linker flag from the env (e.g. NIX_LDFLAGS="-lasan") rather than
# hard-coding it here.
TEST_CFLAGS ?= $(CFLAGS) -fsanitize=address $(NIX_LDFLAGS)

test:
	@CC="$(CC)" TEST_CFLAGS="$(TEST_CFLAGS)" sh tools/test.sh

# Bytewise-compare two runs of every fixture in examples/tests/.
# Catches non-determinism in dump output (HashMap iteration leaks,
# uninitialized reads, etc.) before it becomes a hard-to-debug
# regression downstream.
test-determinism: $(TARGET)
	@sh tools/determinism_test.sh