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

CORE_DIRS := src/support src/syntax src/db src/sema src/lexer src/parser src/compiler src/ide src/consumers/driver
SRCS := $(shell find $(CORE_DIRS) -name '*.c' -print)

# LSP server. Builds into the main `ore` binary as the `ore lsp`
# subcommand. Compiles only when CJSON_LIBS resolved (we're in the
# Nix dev shell or libcjson-dev is installed); otherwise the LSP
# sources are skipped and `ore lsp` reports "lsp not built". This
# keeps `make all` working in cJSON-less environments without forcing
# a separate target for the common case.
ifneq ($(strip $(CJSON_LIBS)),)
  LSP_DIRS := src/consumers/lsp
  SRCS += $(shell find $(LSP_DIRS) -name '*.c' -print)
  CFLAGS += -DORE_HAS_LSP=1
endif

# actually run a test binary
SRCS += src/main.c

FORMAT = clang-format
FORMAT_FLAGS = -i -style=file --fallback-style=LLVM

.PHONY: all clean test test-determinism test-invalidation \
        test-invalidation-debug test-intern-pool test-stringpool \
        test-vec test-file-incremental test-decl-incremental test-durability \
        test-source-edit test-cross-module test-lsp test-syntax test-syntax-kind \
        test-layout-unified \
        check-syntax-contract format mac-leaks \
        profile-workload profile-compaction ore-lsp-workload

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

# Unit tests for the unified intern pool. Standalone build — depends on
# the pool's .c plus the chained arena's .c (its only transitive
# dependency), and NOTHING ELSE in the compiler tree. This is the
# property that lets us iterate on the pool while the rest of the
# compiler is mid-refactor. Compiled with ASan to catch memory bugs.
test-intern-pool:
	@$(TEST_CC) $(TEST_CFLAGS) tools/intern_pool_test.c \
	    src/db/intern_pool/intern_pool.c \
	    src/support/data_structure/arena.c \
	    -o ore-intern-pool-test
	@./ore-intern-pool-test

# Standalone red/green syntax library tests. Builds ONLY the syntax
# module + its support deps + each test driver — proves the extraction
# contract: zero Ore-specific code is needed to use the library. ASan
# verifies the manual refcount discipline (no leaks, no double-frees).
SYNTAX_LIB_SRCS := \
    src/syntax/green.c \
    src/syntax/node_cache.c \
    src/syntax/builder.c \
    src/syntax/red.c \
    src/syntax/ptr.c \
    src/syntax/iter.c \
    src/syntax/text.c \
    src/syntax/sll.c \
    src/support/data_structure/arena.c \
    src/support/data_structure/vec.c \
    src/support/data_structure/hashmap.c

test-syntax: check-syntax-contract
	@$(TEST_CC) $(TEST_CFLAGS) tools/syntax_test.c \
	    $(SYNTAX_LIB_SRCS) -o ore-syntax-test
	@./ore-syntax-test
	@$(TEST_CC) $(TEST_CFLAGS) tools/syntax_red_test.c \
	    $(SYNTAX_LIB_SRCS) -o ore-syntax-red-test
	@./ore-syntax-red-test
	@$(TEST_CC) $(TEST_CFLAGS) tools/syntax_ptr_test.c \
	    $(SYNTAX_LIB_SRCS) -o ore-syntax-ptr-test
	@./ore-syntax-ptr-test
	@$(TEST_CC) $(TEST_CFLAGS) tools/syntax_token_test.c \
	    $(SYNTAX_LIB_SRCS) -o ore-syntax-token-test
	@./ore-syntax-token-test
	@$(TEST_CC) $(TEST_CFLAGS) tools/syntax_iter_test.c \
	    $(SYNTAX_LIB_SRCS) -o ore-syntax-iter-test
	@./ore-syntax-iter-test
	@$(TEST_CC) $(TEST_CFLAGS) tools/syntax_text_test.c \
	    $(SYNTAX_LIB_SRCS) -o ore-syntax-text-test
	@./ore-syntax-text-test
	@$(TEST_CC) $(TEST_CFLAGS) tools/syntax_sll_test.c \
	    src/syntax/sll.c -o ore-syntax-sll-test
	@./ore-syntax-sll-test
	@$(TEST_CC) $(TEST_CFLAGS) tools/syntax_mutable_test.c \
	    $(SYNTAX_LIB_SRCS) -o ore-syntax-mutable-test
	@./ore-syntax-mutable-test
	@$(TEST_CC) $(TEST_CFLAGS) tools/syntax_green_mut_test.c \
	    $(SYNTAX_LIB_SRCS) -o ore-syntax-green-mut-test
	@./ore-syntax-green-mut-test
	@$(TEST_CC) $(TEST_CFLAGS) tools/syntax_mutation_test.c \
	    $(SYNTAX_LIB_SRCS) -o ore-syntax-mutation-test
	@./ore-syntax-mutation-test
	@$(TEST_CC) $(TEST_CFLAGS) tools/syntax_smoke_test.c \
	    $(SYNTAX_LIB_SRCS) -o ore-syntax-smoke-test
	@./ore-syntax-smoke-test
	@$(TEST_CC) $(TEST_CFLAGS) tools/syntax_extras_test.c \
	    $(SYNTAX_LIB_SRCS) -o ore-syntax-extras-test
	@./ore-syntax-extras-test

# OreSyntaxKind enum tests — exhaustiveness of the name table +
# classifier predicates. Standalone; only depends on syntax_kind.c
# and the src/syntax/ header for the SyntaxKind typedef.
test-syntax-kind:
	@$(TEST_CC) $(TEST_CFLAGS) tools/syntax_kind_test.c \
	    src/parser/syntax_kind.c -o ore-syntax-kind-test
	@./ore-syntax-kind-test

# Layout single-stream output verification (Phase A.0). Asserts source
# bytes round-trip, virtual tokens are zero-width, document order is
# monotonic, Token stays 16 bytes. Standalone — links lexer + layout +
# token + syntax_kind + support primitives. ASan-enabled.
test-layout-unified:
	@$(TEST_CC) $(TEST_CFLAGS) tools/layout_unified_test.c \
	    src/lexer/layout.c src/lexer/lexer.c src/lexer/token.c \
	    src/parser/syntax_kind.c \
	    src/support/data_structure/stringpool.c \
	    src/support/data_structure/arena.c \
	    src/support/data_structure/vec.c \
	    -o ore-layout-unified-test
	@./ore-layout-unified-test

# Extraction contract lint: src/syntax/ must NEVER include from
# src/db/, src/parser/, src/sema/, src/ide/, src/lexer/, src/compiler/,
# or src/consumers/. The library may only depend on
# src/support/data_structure/ and the C standard library.
check-syntax-contract:
	@bad=$$(grep -rEn 'include\s+"[^"]*\.\.(/\.\.)*/(db|parser|sema|ide|lexer|compiler|consumers)/' src/syntax/ 2>/dev/null); \
	if [ -n "$$bad" ]; then \
	    echo "extraction-contract VIOLATION in src/syntax/:"; \
	    echo "$$bad"; \
	    echo ""; \
	    echo "src/syntax/ may only include from src/support/data_structure/"; \
	    echo "and the C standard library. See src/syntax/syntax.h for details."; \
	    exit 1; \
	fi

# Unit tests for the StringPool. Same self-contained pattern: pool .c +
# arena .c + driver, nothing else. ASan-enabled.
test-stringpool:
	@$(TEST_CC) $(TEST_CFLAGS) tools/stringpool_test.c \
	    src/support/data_structure/stringpool.c \
	    src/support/data_structure/arena.c \
	    -o ore-stringpool-test
	@./ore-stringpool-test

# Unit tests for Vec (malloc-default + arena-fixed flavors). Self-
# contained: vec .c + arena .c + driver. ASan-enabled.
test-vec:
	@$(TEST_CC) $(TEST_CFLAGS) tools/vec_test.c \
	    src/support/data_structure/vec.c \
	    src/support/data_structure/arena.c \
	    -o ore-vec-test
	@./ore-vec-test

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

# Step 3 gate — per-file early-cutoff. Two files in one module; edit
# one, assert only its QUERY_FILE_AST recomputes while the sibling is
# verified-unchanged and skipped (the whole point of keying the parse
# query by FileId). Links the core sources minus src/main.c.
FILE_INCREMENTAL_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                              tools/file_incremental_test.c

test-file-incremental:
	@$(CC) $(CFLAGS) $(FILE_INCREMENTAL_TEST_SRCS) $(LDFLAGS) \
	    -o ore-file-incremental-test
	@./ore-file-incremental-test

# Gap A gate — per-decl typecheck cutoff. Two functions in one file;
# edit one's body, assert only that function re-typechecks while the
# sibling's sema slots stay frozen (QUERY_DECL_AST's structural
# fingerprint is position-independent).
DECL_INCREMENTAL_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                              tools/decl_incremental_test.c

test-decl-incremental:
	@$(CC) $(CFLAGS) $(DECL_INCREMENTAL_TEST_SRCS) $(LDFLAGS) \
	    -o ore-decl-incremental-test
	@./ore-decl-incremental-test

# Step 4 gate — durability fast-path. A LOW (workspace) edit must NOT
# walk a HIGH-only (library) slot's dependency graph.
DURABILITY_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                        tools/durability_test.c

test-durability:
	@$(CC) $(CFLAGS) $(DURABILITY_TEST_SRCS) $(LDFLAGS) \
	    -o ore-durability-test
	@./ore-durability-test

# Production gate for the db_source_set_text edit primitive: no-op on
# identical text, reparse + version++ on a real change, and a 64-edit
# loop that must stay leak-bounded (the arena->malloc storage change).
SOURCE_EDIT_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                         tools/source_edit_test.c

test-source-edit:
	@$(CC) $(CFLAGS) $(SOURCE_EDIT_TEST_SRCS) $(LDFLAGS) \
	    -o ore-source-edit-test
	@./ore-source-edit-test

# Gap B gate — multi-file modules + @import resolution. Two files in
# the same directory share a ModuleId; @import("./b.ore") resolves;
# cross-file incrementality holds (a-edit leaves b frozen; b-edit
# re-typechecks a's consumers).
CROSS_MODULE_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                          tools/cross_module_test.c

test-cross-module:
	@$(CC) $(CFLAGS) $(CROSS_MODULE_TEST_SRCS) $(LDFLAGS) \
	    -o ore-cross-module-test
	@./ore-cross-module-test

# LSP integration tests. Spawn `./ore lsp` as a subprocess and drive
# it via JSON-RPC over stdin/stdout. Covers regressions for cross-file
# invalidation, hover-no-crash, and didOpen-publishes-diags. Builds
# its own driver (tools/lsp_test.c) — does NOT link the compiler
# sources; it uses the real `ore` binary as the system under test, so
# `make all` must run first.
test-lsp: $(TARGET)
	@$(CC) $(CFLAGS) tools/lsp_test.c -o ore-lsp-test
	@./ore-lsp-test ./ore

# Profile workload — drives the compiler through standardized
# scenarios with built-in query stats + memory tracking. Always
# built with -DORE_DEBUG_QUERIES=1 to expose cache-hit counters.
# See docs/profiling.md for the Instruments attach workflow.
LSP_WORKLOAD_SRCS := $(filter-out src/main.c, $(SRCS)) \
                     tools/lsp_workload.c

ore-lsp-workload:
	@$(CC) $(CFLAGS) -DORE_DEBUG_QUERIES=1 $(LSP_WORKLOAD_SRCS) \
	    $(LDFLAGS) -o ore-lsp-workload

# Quick sanity sweep across all scenarios. For instrument runs
# (Instruments / leaks / perf), run the binary directly with the
# scenario you care about + --attach-pause.
profile-workload: ore-lsp-workload
	@./ore-lsp-workload all --iters 50

# Compaction-focused stress: writes per-iter CSV to /tmp so the
# sawtooth oscillation pattern can be plotted / spot-checked.
profile-compaction: ore-lsp-workload
	@./ore-lsp-workload compaction-stress --iters 50 --csv > /tmp/profile-compaction.csv
	@echo "wrote /tmp/profile-compaction.csv ($$(wc -l < /tmp/profile-compaction.csv) lines)"