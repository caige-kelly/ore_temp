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

CORE_DIRS := src/support src/syntax src/ast src/db src/lexer src/parser_new src/compiler src/ide src/consumers/driver
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

# Keep-zone — the subset that compiles against the new query engine
# DURING the rewrite (Phases C/D in progress). Excludes sema / ide /
# compiler / consumers, which still reference deleted layer headers
# until their Phase D port. Engine-level tests link against this set.
# Folded back into SRCS once the downstream layers are ported.
KEEP_ZONE_DIRS := src/support src/syntax src/ast src/lexer src/parser_new src/db
KEEP_ZONE_SRCS := $(shell find $(KEEP_ZONE_DIRS) -name '*.c' -print)

# === Test object cache =========================================
# Every keep-zone test re-compiling all ~50 keep-zone sources from
# scratch is the bulk of the test-suite latency. Cache the .o files
# in $(BUILD_ASAN_DIR) mirroring the source tree, link tests against
# the cached set. Header-dep tracking via -MMD/-MP keeps the cache
# safe across .h edits. The main `ore` binary still compiles from
# $(SRCS) directly via the $(TARGET) rule below — no ASan there, no
# need for caching. Standard CMake/Meson/Bazel pattern hand-rolled
# in Make. CI target wipes the dir first; dev iterations keep it.
BUILD_DIR      := build
BUILD_ASAN_DIR := $(BUILD_DIR)/asan
BUILD_TESTS    := $(BUILD_DIR)/tests

KEEP_ZONE_OBJS := $(patsubst src/%.c,$(BUILD_ASAN_DIR)/%.o,$(KEEP_ZONE_SRCS))

$(BUILD_ASAN_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	@$(TEST_CC) $(TEST_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_TESTS):
	@mkdir -p $@

-include $(KEEP_ZONE_OBJS:.o=.d)

FORMAT = clang-format
FORMAT_FLAGS = -i -style=file --fallback-style=LLVM

.PHONY: all clean ci test test-determinism test-intern-pool test-stringpool \
        test-vec test-hashmap \
        test-durability test-source-edit test-lsp test-syntax test-syntax-kind \
        test-layout-unified test-ast-wrappers test-parser-green \
        test-reparse-churn \
        test-scope-shadowing \
        test-keep-zone $(ALL_KEEPZONE_TESTS) \
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
	rm -rf $(BUILD_DIR)

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

# test-file-incremental + test-decl-incremental drivers removed in D3.4:
# they #include'd D1-deleted per-query headers (db/query/ast.h). Their
# coverage is owned by the keep-zone test-input-incremental,
# test-parse-incremental, and test-trivia-stable (which assert
# per-source / per-decl cutoff via db_slot_fingerprint and the FILE_AST /
# top_level_entry fps respectively).

# Phase 0 P0 gate (G1) — cancellation semantics. db_query_begin returns
# CANCELED; request_end sweep resets RUNNING leftovers to EMPTY; next
# request computes the same query freshly. Load-bearing for LSP cancel.
CANCELLATION_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                          tools/cancellation_test.c

test-cancellation:
	@$(CC) $(CFLAGS) $(CANCELLATION_TEST_SRCS) $(LDFLAGS) \
	    -o ore-cancellation-test
	@./ore-cancellation-test


# Phase 0 P0 gate (G5) — failure-then-retry. db_query_fail caches ERROR;
# same-rev re-begin returns BEGIN_ERROR; slot reset → COMPUTE recovers.
FAILURE_RETRY_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                           tools/failure_retry_test.c

test-failure-retry:
	@$(CC) $(CFLAGS) $(FAILURE_RETRY_TEST_SRCS) $(LDFLAGS) \
	    -o ore-failure-retry-test
	@./ore-failure-retry-test

# Pre-Phase-C debt D-B2 (Gap G8) — import cycle safety. Links against
# the KEEP_ZONE (no sema), runs under a 30s wall-clock guard so a
# regression that reintroduces unbounded import recursion fails as a
# timeout rather than hanging CI. ASan-enabled.
# Pre-Phase-C debt D-HM — orphan reclamation removes routing-HashMap
# entries (two-pass) so the routing maps stay bounded across a long LSP
# session. Links against the KEEP_ZONE (no sema), ASan-enabled — gates the
# two-pass removal + deep-free path for use-after-free / leaks.
# C.0 gate — input→query dependency edge. Edit a source → its file_ast
# recomputes; an unrelated file's file_ast cache-hits (per-source, not
# per-tier); byte-identical edit is a no-op. KEEP_ZONE, ASan. Also
# exercises FILE_AST Vec-routing (the SoA slot-sentinel fix).
# W1 regression — dep-dedup collision safety. Two deps colliding in
# dep_index_key (differ only in bit 56) must both be recorded, not
# silently coalesced into one (which would drop a dep → missed
# invalidation). KEEP_ZONE, ASan.
# C.0b gate — line_index pure query. LF/CRLF offsets, position.c reads it,
# per-source recompute. KEEP_ZONE, ASan.
# C.1 gate — decl_ast content-hash firewall (C2/C20) + file_ast→decl_ast
# dep chain. Edit A → A's fp changes, B's fp position-independent. ASan.
# C.1 gate (C3/C19) — trivia stability. Comment/whitespace edit reparses
# but leaves decl_ast structural fingerprints unchanged. ASan.
# C.1.a gate — top_level_entry firewall + FILE_SET dep. Name lookup, the
# position-independent sibling-edit firewall, and the file-add correctness
# case (NOT_FOUND → resolves once a file defining the name joins the
# namespace) that a coarse tier bump alone would miss. KEEP_ZONE, ASan.
# C.2c gate — NAMESPACE_ITEMS per-namespace items index + AstId. Direct
# enumeration of every top-level name; the index fp is position-independent
# (trivia shift leaves it stable) while top_level_entry's ptr stays current
# and its AstId stable; content/rename edits flip the index fp. KEEP_ZONE,
# ASan (covers the malloc body lifecycle).
# D1 gate — name layer (scope.c). def_identity: stable DefId (interning) +
# rename→new; namespace_scopes: internal name→DefId scope parented to
# primitives; resolve_ref: scope-chain lookup + primitive fall-through + miss.
# KEEP_ZONE, ASan.
# D2.0 gate — semantic-kind classification (struct/fn/const) + struct→enum
# retype yields a new DefId classified KIND_ENUM. KEEP_ZONE, ASan.
# D2.1 gate — type_of_def + fn_signature + resolve_type_expr: struct/self-ref/
# fn/typed-const types; nominal stable across sibling edit; field edit flips fp.
# KEEP_ZONE, ASan.
# D2.2 gate — namespace_type: pub members (private excluded), IPK_NAMESPACE_TYPE,
# body-edit fp-stable, pub-toggle flips fp. KEEP_ZONE, ASan.
# D2.2 gate — aggregate/enum/namespace member-pool compaction reclaims
# recompute-stranded ranges; surviving ranges stay correct. KEEP_ZONE, ASan.
# D2.3 gate — body_scopes: structural scope tree + bind_site bindings (no types),
# bind_site lookup, position-independent structural fp. KEEP_ZONE, ASan.
# D2.4 gate — body inference: inferred binds, param/local ref typing via
# bind_site→node-map, content-firewalled infer_body fp. KEEP_ZONE, ASan.
# D2.5 gate — the check driver + unused-decl warnings. Type errors surface
# via db_collect_diags_for_file; unused = unreferenced-private (pub/main/
# referenced exempt) via the dep-graph-as-reference-graph plain pass;
# incremental + same-type-ref-swap correctness. KEEP_ZONE, ASan.
# D3.0 gate — the node-type router (db_query_node_type) + db_node_enclosing_def:
# type-at-node across the infer_body / fn_signature / type_of_def ranges + the
# top-level resolve_ref fallback. KEEP_ZONE, ASan.
# D2.6 gate — principled bidirectional check_expr for SK_PRODUCT_EXPR (typed
# construction): struct/array literals (named + positional + inferred-size),
# loud diags on shape mismatches, no silent fallbacks. KEEP_ZONE, ASan.
# S1 gate — per-namespace file reverse index behind db_get_namespace_files
# (O(files-in-namespace), not O(all files)). Membership, multi-file
# namespace, evicted exclusion, empty/sentinel → NULL. KEEP_ZONE, ASan
# (inner-Vec init/append/free lifecycle).
# Phase-8 gate — FILE_SET remove-on-evict. Evicting a file drops it from
# member_files and recomputes the FILE_SET fp from the survivors (fold,
# since combine can't subtract); empty namespace returns to the seed.
# KEEP_ZONE, ASan.
# C.1.b gate — file_imports @import extraction + fp firewall. One import
# yields path StrId (quotes stripped) + anchored site; an unrelated edit
# that shifts the import leaves the fp stable; changing the path changes
# the fp. Body is a standalone malloc, freed on recompute + evict (ASan
# confirms no leak/UAF). KEEP_ZONE, ASan.
# Pre-Phase-C foundation hygiene (A8) — db_format_type recursion bound.
# Builds a deeply-nested type and confirms the renderer caps at depth 16
# ("..." marker) instead of overflowing the stack. KEEP_ZONE, ASan.
# Pre-Phase-C foundation hygiene (A6) — virtual-source name collision.
# workspace_admit_virtual must reject a duplicate synthetic name via the
# virtual_by_name index (db_admit_virtual_source isn't in source_by_path).
# KEEP_ZONE, ASan.
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

# Phase P cutover — diag_lifecycle_test deleted. The orphan-DefId
# pathology it documented is structurally impossible now: per-query
# DiagBundles owned by their slot, reclaim_slot frees the bundle,
# collector liveness-gates each column. F4's
# test_sticky_squiggle_body_anchor (in lsp_test.c) is the live
# architectural-property test.

# Step 4 gate — durability fast-path. A LOW (workspace) edit must NOT
# walk a HIGH-only (library) slot's dependency graph.
DURABILITY_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                        tools/durability_test.c

test-durability:
	@$(CC) $(CFLAGS) $(DURABILITY_TEST_SRCS) $(LDFLAGS) \
	    -o ore-durability-test
	@./ore-durability-test


# Tier 1 audit-close gate — reparse churn. NodeCache hash-cons must
# preserve sibling-decl GreenNode pointers across reparses; total
# memory must stay bounded across 100+ edits (LSan-verified).
REPARSE_CHURN_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                           tools/reparse_churn_test.c

test-reparse-churn:
	@$(TEST_CC) $(TEST_CFLAGS) $(REPARSE_CHURN_TEST_SRCS) \
	    -o ore-reparse-churn-test
	@./ore-reparse-churn-test


# Tier 2 audit-close gate — body_scopes shadowing. Nested let-binds
# of the same name must live in distinct scope_ids so the lookup walk
# resolves to the innermost binding.
SCOPE_SHADOWING_TEST_SRCS := $(filter-out src/main.c, $(SRCS)) \
                              tools/scope_shadowing_test.c

test-scope-shadowing:
	@$(CC) $(CFLAGS) $(SCOPE_SHADOWING_TEST_SRCS) $(LDFLAGS) \
	    -o ore-scope-shadowing-test
	@./ore-scope-shadowing-test

# test-node-type-router + test-node-type-router-extended removed in D3.4:
# both drivers #include'd D1-deleted db/query/node_type.h. The D3.0
# keep-zone test-node-type is the principled replacement (covers body
# locals, params, struct fields, top-level refs, member-access receivers).

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

# === Keep-zone tests ============================================
# Every test that links the full $(KEEP_ZONE_OBJS) set. Adding one
# = one entry in KEEPZONE_TESTS (file convention:
# tools/<name_with_underscores>_test.c). Each test still gets its
# own isolated binary in $(BUILD_TESTS)/ — one crash doesn't take
# down others, ASan reports stay per-test. The win is shared
# compile cache: sources compile once instead of 26 times.
#
# The per-test inline comments (above the deleted rules in the
# pre-refactor file) lived next to the recipe; they now live next
# to the test source itself (tools/<name>_test.c top-of-file
# comments). The recipe IS the foreach below.
KEEPZONE_TESTS := \
    body-scopes check classify coerce def-identity dep-dedup \
    body-ast-id \
    diag-anchor-size \
    evict-membership evict-readmit file-imports format-type-depth import \
    infer-body init-list input-incremental line-index \
    namespace-files namespace-items namespace-scopes \
    namespace-type node-type orphan-reclaim parse-incremental \
    pool-compaction resolve-ref top-level-entry trivia-stable \
    type-of-def virtual-collision
# import-cycle is defined below: it gets a timeout wrapper because
# its happy path is "ensure no cycle deadlock" — a regression would
# hang the whole suite without the limit.

# Hyphens in target name → underscores in source filename.
uscore = $(subst -,_,$(1))

define keepzone_test_rule
$(BUILD_TESTS)/$(1): tools/$$(call uscore,$(1))_test.c $$(KEEP_ZONE_OBJS) | $$(BUILD_TESTS)
	@$$(TEST_CC) $$(TEST_CFLAGS) $$^ $$(LDFLAGS) -o $$@
test-$(1): $(BUILD_TESTS)/$(1)
	@$$<
endef

$(foreach t,$(KEEPZONE_TESTS),$(eval $(call keepzone_test_rule,$(t))))

$(BUILD_TESTS)/import-cycle: tools/import_cycle_test.c $(KEEP_ZONE_OBJS) | $(BUILD_TESTS)
	@$(TEST_CC) $(TEST_CFLAGS) $^ $(LDFLAGS) -o $@
test-import-cycle: $(BUILD_TESTS)/import-cycle
	@timeout 30 $<

ALL_KEEPZONE_TESTS := $(addprefix test-,$(KEEPZONE_TESTS) import-cycle)

# Meta — run every keep-zone test. The actual gate during Phase D
# (full `make test` requires the ore binary, which stays broken
# until D3 lands). Sources compile once into $(BUILD_ASAN_DIR), so
# the whole suite re-runs in seconds after the first build.
test-keep-zone: $(ALL_KEEPZONE_TESTS)
	@echo "[OK] keep-zone: $(words $(ALL_KEEPZONE_TESTS)) tests"

# `make ci` — clean-slate gate. Wipe the object cache first so a
# stale .o can't hide a real failure. Use this before committing.
# Recipe uses sub-makes so `clean` finishes before the rebuild
# starts even under `make -j` (sibling-prereq parallelism would
# otherwise race the wipe against in-flight test builds).
ci:
	@$(MAKE) clean
	@$(MAKE) test-keep-zone

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