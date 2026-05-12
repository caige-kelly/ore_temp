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

# cJSON powers the LSP server's JSON-RPC layer (src/lsp/). Resolved
# via pkg-config so the nixpkgs-pinned version drives the include
# path on every host. Outside Nix, install libcjson-dev and the
# same vars resolve. If pkg-config is missing entirely these expand
# to nothing — the main `ore` binary builds because src/lsp/ is the
# only consumer and the linker only complains on `ore lsp`.
CJSON_CFLAGS ?= $(shell pkg-config --cflags libcjson 2>/dev/null)
CJSON_LIBS   ?= $(shell pkg-config --libs   libcjson 2>/dev/null)
CFLAGS  += $(CJSON_CFLAGS)
LDFLAGS += $(CJSON_LIBS)

TARGET = ore

# --- REWRITE MODE ---
# Only compile the foundational systems. Sema, Consumers, and Build System
# are intentionally excluded until the DB and Parser are stable.

CORE_DIRS := src/support src/db src/lexer src/parser src/consumers/driver
SRCS := $(shell find $(CORE_DIRS) -name '*.c' -print)

# actually run a test binary
SRCS += src/main.c

FORMAT = clang-format
FORMAT_FLAGS = -i -style=file --fallback-style=LLVM

.PHONY: all clean test test-determinism test-invalidation \
        test-invalidation-debug test-intern-pool format

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

# Unit tests for the unified intern pool (R4 Step 2). Standalone build
# — only depends on src/sema/intern_pool/intern_pool.c, no other sema
# code. Compiled with ASan to catch any memory bugs in the pool itself.
test-intern-pool:
	@$(TEST_CC) $(TEST_CFLAGS) tools/intern_pool_test.c \
	    src/sema/intern_pool/intern_pool.c -o ore-intern-pool-test
	@./ore-intern-pool-test

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