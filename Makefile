# Default to clang, but only if the user hasn't set CC themselves
# (via env var, `make CC=gcc`, etc.). The `origin` check is needed
# because Make has a built-in CC=cc that ?= won't override.
#
# clang is the single C toolchain across both `make all` and the
# sanitizer smoke tests in `make test`. Pre-this-change we had a
# split (CC=zig cc + TEST_CC=clang) to work around B22, but we
# never actually used Zig's cross-compile capability so the extra
# complexity wasn't earning its keep. The Nix devShell pins
# pkgs.clang_19 so the version is reproducible across platforms.
ifeq ($(origin CC),default)
  CC = clang
endif

CFLAGS  ?= -std=c23 -Wall -Isrc -g
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

.PHONY: all clean test test-determinism test-invalidation \
        test-invalidation-debug format

format:
	$(FORMAT) $(FORMAT_FLAGS) $(SRCS)

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $(TARGET)

# Debug build with untracked-read trace mode + per-QueryKind telemetry.
# See bug_of_bugs.md #16. The flag enables:
#   - SEMA_READ_UNTRACKED frame/slot bookkeeping (forces RECOMPUTE on
#     revalidate for any slot whose body called the macro)
#   - per-QueryKind counters surfaced via --dump-query-stats
#   - the def_origin precondition assert (B11)
# Production builds leave the flag off; release behavior is unchanged.
debug-queries:
	$(CC) $(CFLAGS) -DORE_DEBUG_QUERIES=1 $(SRCS) $(LDFLAGS) -o $(TARGET)

clean:
	rm -f $(TARGET)

# C smoke-test build. Same CC as the main build (clang); we keep
# TEST_CC as a separate var only to make it cheap to swap if we
# ever need a different compiler for sanitizer builds again. ASan
# works portably via clang's bundled libclang_rt (darwin + linux).
TEST_CC    ?= $(CC)
TEST_CFLAGS ?= $(CFLAGS) -fsanitize=address $(NIX_LDFLAGS)

test:
	@CC="$(CC)" TEST_CC="$(TEST_CC)" TEST_CFLAGS="$(TEST_CFLAGS)" sh tools/test.sh

# Bytewise-compare two runs of every fixture in examples/tests/.
# Catches non-determinism in dump output (HashMap iteration leaks,
# uninitialized reads, etc.) before it becomes a hard-to-debug
# regression downstream.
test-determinism: $(TARGET)
	@sh tools/determinism_test.sh

# In-process incremental / invalidation tests. Drives Sema across
# multiple revisions of the same input and asserts that the slot
# machinery re-derives correct types and preserves pointer identity
# where it should (interned compound types + slot-cached signatures).
#
# Compiled separately from the main binary because it links the same
# Sema sources but provides its own `main`. Keep the source list in
# sync with $(SRCS) minus src/main.c.
INVALIDATION_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                         tools/sema_invalidation_test.c

test-invalidation:
	@$(CC) $(CFLAGS) $(INVALIDATION_TEST_SRCS) $(LDFLAGS) -o ore-invalidation-test
	@./ore-invalidation-test

# Same harness, compiled with -DORE_DEBUG_QUERIES=1. Catches untracked
# reads + exercises the REVALIDATE_RECOMPUTE branch for slots flagged
# via SEMA_READ_UNTRACKED. All scenarios that pass under the default
# build must also pass here; #16-specific scenarios (T19+) light up
# only under this build.
test-invalidation-debug:
	@$(CC) $(CFLAGS) -DORE_DEBUG_QUERIES=1 $(INVALIDATION_TEST_SRCS) $(LDFLAGS) \
	    -o ore-invalidation-test
	@./ore-invalidation-test