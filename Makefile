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

CORE_DIRS := src/support src/syntax src/ast src/db src/sema src/lexer src/parser_new src/compiler src/ide src/consumers/driver
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

.PHONY: all clean test test-determinism test-intern-pool test-stringpool \
        test-vec test-hashmap test-file-incremental test-decl-incremental \
        test-durability test-source-edit test-lsp test-syntax test-syntax-kind \
        test-layout-unified test-ast-wrappers test-parser-green \
        test-cycle-struct test-reparse-churn test-import-resolution \
        test-scope-shadowing test-node-type-router test-diag-render \
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
	    src/syntax/syntax_kind.c -o ore-syntax-kind-test
	@./ore-syntax-kind-test

# Typed AST wrapper tests (Phase A.1.1). Builds hand-crafted green
# trees, casts to FnDef/BinExpr/etc., verifies accessors. Links the
# wrapper sources + the syntax library + syntax_kind. ASan-enabled.
test-ast-wrappers:
	@$(TEST_CC) $(TEST_CFLAGS) tools/ast_wrappers_test.c \
	    src/ast/ast.c src/ast/ast_decl.c src/ast/ast_expr.c \
	    src/ast/ast_stmt.c src/ast/ast_type.c \
	    src/syntax/syntax_kind.c \
	    $(SYNTAX_LIB_SRCS) -o ore-ast-wrappers-test
	@./ore-ast-wrappers-test

# Green-tree parser smoke test (Phase A.1.2). Runs parser_new against
# examples/*.ore and verifies basic shape invariants. Standalone — links
# only the new parser + lexer + layout + syntax library + support. Does
# NOT touch src/db (parser_new is decoupled from db.h). ASan-enabled.
test-parser-green:
	@$(TEST_CC) $(TEST_CFLAGS) tools/parser_green_test.c \
	    src/parser_new/parser.c src/parser_new/parse_decl.c \
	    src/parser_new/parse_stmt.c src/parser_new/parse_expr.c \
	    src/lexer/layout.c src/lexer/lexer.c src/lexer/token.c \
	    src/syntax/syntax_kind.c \
	    src/db/intern_pool/intern_pool.c \
	    src/support/data_structure/stringpool.c \
	    $(SYNTAX_LIB_SRCS) -o ore-parser-green-test
	@./ore-parser-green-test

# Layout single-stream output verification (Phase A.0). Asserts source
# bytes round-trip, virtual tokens are zero-width, document order is
# monotonic, Token stays 16 bytes. Standalone — links lexer + layout +
# token + syntax_kind + support primitives. ASan-enabled.
test-layout-unified:
	@$(TEST_CC) $(TEST_CFLAGS) tools/layout_unified_test.c \
	    src/lexer/layout.c src/lexer/lexer.c src/lexer/token.c \
	    src/syntax/syntax_kind.c \
	    src/support/data_structure/stringpool.c \
	    src/support/data_structure/arena.c \
	    src/support/data_structure/vec.c \
	    -o ore-layout-unified-test
	@./ore-layout-unified-test

# Extraction contract lint: src/syntax/ must NEVER include from
# src/db/, src/parser_new/, src/sema/, src/ide/, src/lexer/, src/compiler/,
# or src/consumers/. The library may only depend on
# src/support/data_structure/ and the C standard library.
check-syntax-contract:
	@bad=$$(grep -rEn 'include\s+"[^"]*\.\.(/\.\.)*/(db|parser_new|sema|ide|lexer|compiler|consumers)/' src/syntax/ 2>/dev/null); \
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

# Unit tests for HashMap<uint64_t, void*>. Self-contained: only
# hashmap + arena. ASan-enabled.
test-hashmap:
	@$(TEST_CC) $(TEST_CFLAGS) tools/hashmap_test.c \
	    src/support/data_structure/hashmap.c \
	    src/support/data_structure/arena.c \
	    -o ore-hashmap-test
	@./ore-hashmap-test

# Unit tests for Vec (malloc-default + arena-fixed flavors). Self-
# contained: vec .c + arena .c + driver. ASan-enabled.
test-vec:
	@$(TEST_CC) $(TEST_CFLAGS) tools/vec_test.c \
	    src/support/data_structure/vec.c \
	    src/support/data_structure/arena.c \
	    -o ore-vec-test
	@./ore-vec-test

# Unit tests for PagedVec — Salsa-style fixed-page segmented vector.
# The pointer-stability test is load-bearing: captures a pointer to an
# early element, pushes 100k more, dereferences the original pointer
# under ASan. Validates the entire premise of the storage primitive.
# Self-contained: just paged_vec.c + driver.
test-paged-vec:
	@$(TEST_CC) $(TEST_CFLAGS) tools/paged_vec_test.c \
	    src/support/data_structure/paged_vec.c \
	    -o ore-paged-vec-test
	@./ore-paged-vec-test

# Bytewise-compare two runs of every fixture in examples/tests/.
# Catches non-determinism in dump output (HashMap iteration leaks,
# uninitialized reads, etc.) before it becomes a hard-to-debug
# regression downstream.
test-determinism: $(TARGET)
	@sh tools/determinism_test.sh

# (sema_invalidation_test removed — targeted a deleted architecture.
#  The four DB-level incremental tests below are the real gate:
#  test-file-incremental, test-decl-incremental, test-source-edit,
#  test-durability.)

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

# Phase 0 P0 gate (G1) — cancellation semantics. db_query_begin returns
# CANCELED; request_end sweep resets RUNNING leftovers to EMPTY; next
# request computes the same query freshly. Load-bearing for LSP cancel.
CANCELLATION_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                          tools/cancellation_test.c

test-cancellation:
	@$(CC) $(CFLAGS) $(CANCELLATION_TEST_SRCS) $(LDFLAGS) \
	    -o ore-cancellation-test
	@./ore-cancellation-test

# Phase 0 P0 gate (G2) — cycle correctness for unions (mirrors
# cycle_struct_test). Confirms wip-publish covers KIND_UNION via the
# shared SK_UNION_DECL code path in build_struct_type.
CYCLE_UNION_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                         tools/cycle_union_test.c

test-cycle-union:
	@$(CC) $(CFLAGS) $(CYCLE_UNION_TEST_SRCS) $(LDFLAGS) \
	    -o ore-cycle-union-test
	@./ore-cycle-union-test

# Phase 0 P0 gate (G5) — failure-then-retry. db_query_fail caches ERROR;
# same-rev re-begin returns BEGIN_ERROR; slot reset → COMPUTE recovers.
FAILURE_RETRY_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                           tools/failure_retry_test.c

test-failure-retry:
	@$(CC) $(CFLAGS) $(FAILURE_RETRY_TEST_SRCS) $(LDFLAGS) \
	    -o ore-failure-retry-test
	@./ore-failure-retry-test

# Phase 0 P0 gate (G6) — scope shadowing LOOKUP correctness. Existing
# scope_shadowing test only verifies distinct scope_ids; this one
# actually calls sema_body_scope_lookup at inner vs outer use sites
# and asserts the correct binding is returned.
SCOPE_LOOKUP_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                         tools/scope_shadowing_lookup_test.c

test-scope-shadowing-lookup:
	@$(CC) $(CFLAGS) $(SCOPE_LOOKUP_TEST_SRCS) $(LDFLAGS) \
	    -o ore-scope-shadowing-lookup-test
	@./ore-scope-shadowing-lookup-test

# Phase 0 P0 gate (G7) — node_type_router probes for UNION + VARIABLE.
# Extends existing node_type_router_test which only covered FUNCTION,
# CONSTANT, STRUCT.
ROUTER_EXT_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                       tools/node_type_router_extended_test.c

test-node-type-router-extended:
	@$(CC) $(CFLAGS) $(ROUTER_EXT_TEST_SRCS) $(LDFLAGS) \
	    -o ore-node-type-router-extended-test
	@./ore-node-type-router-extended-test

# Phase 0 P0 gate (G9) — multi-level import cascade. A→B→C three-file
# chain; edit C; assert namespace_type(C) reflects the new pub-decl set
# and import resolution still works through both hops.
IMPORT_CASCADE_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                            tools/import_cascade_test.c

test-import-cascade:
	@$(CC) $(CFLAGS) $(IMPORT_CASCADE_TEST_SRCS) $(LDFLAGS) \
	    -o ore-import-cascade-test
	@./ore-import-cascade-test

# Phase 0 P0 gap G10 — KNOWN FAILING TEST. Documents the orphan-DefId
# diag leak: when an edit shifts a decl's byte range, its old DefId
# becomes orphan but its diag stays in db.diag_lists and is still
# collected. The rewrite (per-entry push-stamp + filtered collect)
# must fix this. NOT part of the green gate.
DIAG_LIFECYCLE_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                            tools/diag_lifecycle_test.c

test-diag-lifecycle-known-failing:
	@$(CC) $(CFLAGS) $(DIAG_LIFECYCLE_TEST_SRCS) $(LDFLAGS) \
	    -o ore-diag-lifecycle-test
	@./ore-diag-lifecycle-test || echo "(expected failure — see plan)"

# Step 4 gate — durability fast-path. A LOW (workspace) edit must NOT
# walk a HIGH-only (library) slot's dependency graph.
DURABILITY_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                        tools/durability_test.c

test-durability:
	@$(CC) $(CFLAGS) $(DURABILITY_TEST_SRCS) $(LDFLAGS) \
	    -o ore-durability-test
	@./ore-durability-test

# Tier 1 audit-close gate — type cycles via `^`. Mutually-referential
# structs must materialize without infinite recursion (wip-publish
# pattern in build_struct_type).
CYCLE_STRUCT_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                          tools/cycle_struct_test.c

test-cycle-struct:
	@$(CC) $(CFLAGS) $(CYCLE_STRUCT_TEST_SRCS) $(LDFLAGS) \
	    -o ore-cycle-struct-test
	@./ore-cycle-struct-test

# Tier 1 audit-close gate — reparse churn. NodeCache hash-cons must
# preserve sibling-decl GreenNode pointers across reparses; total
# memory must stay bounded across 100+ edits (LSan-verified).
REPARSE_CHURN_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                           tools/reparse_churn_test.c

test-reparse-churn:
	@$(TEST_CC) $(TEST_CFLAGS) $(REPARSE_CHURN_TEST_SRCS) \
	    -o ore-reparse-churn-test
	@./ore-reparse-churn-test

# Tier 1 audit-close gate — cross-file @import end-to-end under
# file-as-namespace. Replaces the deleted cross_module_test.c which
# targeted the old directory-as-module design.
IMPORT_RESOLUTION_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                                tools/import_resolution_test.c

test-import-resolution:
	@$(CC) $(CFLAGS) $(IMPORT_RESOLUTION_TEST_SRCS) $(LDFLAGS) \
	    -o ore-import-resolution-test
	@./ore-import-resolution-test

# Tier 2 audit-close gate — body_scopes shadowing. Nested let-binds
# of the same name must live in distinct scope_ids so the lookup walk
# resolves to the innermost binding.
SCOPE_SHADOWING_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                              tools/scope_shadowing_test.c

test-scope-shadowing:
	@$(CC) $(CFLAGS) $(SCOPE_SHADOWING_TEST_SRCS) $(LDFLAGS) \
	    -o ore-scope-shadowing-test
	@./ore-scope-shadowing-test

# Tier 2 audit-close gate — db_query_node_type router. Every IDE
# hover request flows through this dispatch; covers param body,
# body expression, struct field, and decl-name fallback paths.
NODE_TYPE_ROUTER_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                               tools/node_type_router_test.c

test-node-type-router:
	@$(CC) $(CFLAGS) $(NODE_TYPE_ROUTER_TEST_SRCS) $(LDFLAGS) \
	    -o ore-node-type-router-test
	@./ore-node-type-router-test

# Tier 2 audit-close gate — diag rendering + span resolution. Covers
# db_format_diag (template-arg interpolation) and db_resolve_span
# (byte-range → 1-indexed line/col).
DIAG_RENDER_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                         tools/diag_render_test.c

test-diag-render:
	@$(CC) $(CFLAGS) $(DIAG_RENDER_TEST_SRCS) $(LDFLAGS) \
	    -o ore-diag-render-test
	@./ore-diag-render-test

# Production gate for the db_source_set_text edit primitive: no-op on
# identical text, reparse + version++ on a real change, and a 64-edit
# loop that must stay leak-bounded (the arena->malloc storage change).
SOURCE_EDIT_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                         tools/source_edit_test.c

test-source-edit:
	@$(CC) $(CFLAGS) $(SOURCE_EDIT_TEST_SRCS) $(LDFLAGS) \
	    -o ore-source-edit-test
	@./ore-source-edit-test

# (Gap B / cross-module test removed — exercised directory-as-module
#  + db_query_module_for_path, both of which were deferred to the
#  "incremental module composition" follow-up. The file-as-namespace
#  model in the current codebase doesn't share modules across files;
#  testing @import across files needs a new fixture against
#  workspace_resolve_import instead.)

# LSP integration tests. Spawn `./ore lsp` as a subprocess and drive
# it via JSON-RPC over stdin/stdout. Covers regressions for cross-file
# invalidation, hover-no-crash, and didOpen-publishes-diags. Builds
# its own driver (tools/lsp_test.c) — does NOT link the compiler
# sources; it uses the real `ore` binary as the system under test, so
# `make all` must run first.
test-lsp: $(TARGET)
	@$(CC) $(CFLAGS) $(CJSON_LIBS) tools/lsp_test.c -o ore-lsp-test
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

# Parse-only throughput benchmark — times db_query_file_ast with a
# fresh db per iteration. Reports avg ms + MB/s for tracking parser
# perf across changes. Compiled with -O2; no sanitizers.
PARSE_BENCH_SRCS := $(filter-out src/main.c, $(SRCS)) \
                    tools/parse_bench.c

parse-bench:
	@$(CC) -O2 -std=c23 -Wall -Isrc $(CJSON_CFLAGS) $(PARSE_BENCH_SRCS) \
	    $(LDFLAGS) -o ore-parse-bench

# Compaction-focused stress: writes per-iter CSV to /tmp so the
# sawtooth oscillation pattern can be plotted / spot-checked.
profile-compaction: ore-lsp-workload
	@./ore-lsp-workload compaction-stress --iters 50 --csv > /tmp/profile-compaction.csv
	@echo "wrote /tmp/profile-compaction.csv ($$(wc -l < /tmp/profile-compaction.csv) lines)"