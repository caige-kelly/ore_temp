#ifndef ORE_DB_QUERY_CAPABILITY_H
#define ORE_DB_QUERY_CAPABILITY_H

// =====================================================================
// Capability layer — single, gated entry for cross-query result reads.
// =====================================================================
//
// Why this exists
// ---------------
// Queries inside src/db/query/ historically read result-column state
// directly: `*(StrId *)vec_get(&s->defs.names, d.idx)` etc. The
// salsa dep-tracking discipline is then convention-only — readers must
// remember to first call `db_query_*` so the dep gets recorded on
// their frame.
//
// When a reader forgets, the engine's early-cut path returns stale
// data on subsequent invalidations. Most visibly: the LSP cascade
// where commenting/uncommenting one fn poisons unrelated decls until
// the LSP restarts. CLI never sees it (always compiles from scratch).
//
// The capability layer makes the dep-recording side effect MECHANICAL:
// every `db_read_*(ctx, key)` wrapper fires the producing query
// underneath (records the dep on the caller's frame) THEN returns the
// column value. Forgetting the dep is no longer possible if you go
// through this layer.
//
// Enforcement
// -----------
// Two layers:
// 1. RUNTIME: every `db_read_*` asserts a query frame is active. Use
//    `db_get_*_untracked` from driver-level entry points (CHECK
//    driver, main, test bootstrap) that own the result computation
//    rather than consuming it.
// 2. LINT: `tools/lint_untracked_reads.sh` greps for raw `s->{table}.`
//    accesses and `&s->{table}` address-of escapes outside this file
//    and engine*.c. Legitimate raw reads carry a trailing
//    `// LINT_UNTRACKED_OK: <reason>` comment that the grep skips.
//
// Per-query boundary rules
// ------------------------
// - capability wrappers cross query boundaries (caller is one query,
//   the underlying `db_query_*` may be a different one).
// - A query MUST NOT call `db_read_<own_column>(ctx, own_key)` —
//   cycle detection in the engine will panic. Inside a query's
//   compute, use file-local INTERNAL accessors to write to its own
//   slot.
//
// Naming
// ------
// `db_read_*` — TRACKED read; requires active query frame; records
// dep. `db_get_*_untracked` — driver-level / content-addressed read;
// records nothing; safe at any time.

#include "../db.h"
#include "../diag/ast_id.h"
#include "../ids/ids.h"
#include "engine.h"

// ============================================================
// Per-def reads — depend on TYPE_OF_DECL(d).
// ============================================================
//
// Reading a per-def column requires the def's identity stamp to be
// alive at the current revision. We force TYPE_OF_DECL to run (or
// verify-valid) before the raw read; the resulting dep on
// (TYPE_OF_DECL, d.idx) is what catches "this DefId got reclaimed" or
// "the def's content changed".

StrId       db_read_def_name(db_query_ctx *ctx, DefId d);
DefKind     db_read_def_kind(db_query_ctx *ctx, DefId d);
NamespaceId db_read_def_parent_module(db_query_ctx *ctx, DefId d);
uint32_t    db_read_def_kind_row(db_query_ctx *ctx, DefId d);

// ============================================================
// Per-fn reads — depend on FN_SIGNATURE / INFER_BODY / BODY_SCOPES.
// ============================================================

const FnSignature  *db_read_fn_signature(db_query_ctx *ctx, DefId d);
const FnBody       *db_read_fn_body_scopes(db_query_ctx *ctx, DefId d);
NodeTypesRange      db_read_fn_body_node_types(db_query_ctx *ctx, DefId d);
const BodyAstIdMap *db_read_fn_body_ast_id_map(db_query_ctx *ctx, DefId d);

// ============================================================
// Per-file reads — depend on FILE_AST / LINE_INDEX / FILE_IMPORTS.
// ============================================================

struct GreenNode *db_read_file_ast(db_query_ctx *ctx, FileId fid);

// ============================================================
// Untracked variants — for the driver-level + content-addressed
// callsites. Naming convention makes audits visible.
// ============================================================
//
// Use these when you are the SOLE OWNER of a computation (CHECK
// driver, sweep passes), or when the data being read is content-
// addressed / static (intern pool, primitives, string pool). Never
// from inside a memoized query body unless you've thought hard about
// why a dep would be meaningless.

DefKind     db_get_def_kind_untracked(struct db *s, DefId d);
StrId       db_get_def_name_untracked(struct db *s, DefId d);
NamespaceId db_get_def_parent_module_untracked(struct db *s, DefId d);
uint32_t    db_get_def_kind_row_untracked(struct db *s, DefId d);
uint32_t    db_get_def_count_untracked(struct db *s);
bool        db_def_id_valid_untracked(struct db *s, DefId d);

#endif // ORE_DB_QUERY_CAPABILITY_H
