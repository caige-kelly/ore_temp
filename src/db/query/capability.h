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
// Per-namespace member-list reads — depend on NAMESPACE_TYPE.
// ============================================================
//
// The raw getters `db_namespace_member_count` / `db_namespace_member_at`
// in db.h are convention-only: callers in query frames must call
// `db_query_namespace_type(s, ns)` first to record the dep. Forgetting
// that anchor is a silent cache-staling hazard. These wrappers fire
// the producing query internally, so the dep is recorded mechanically.
//
// Use the raw db.h getters ONLY from outside a query frame
// (`ide/completion.c` enumerating members for autocomplete is the
// canonical case).
uint32_t  db_read_namespace_member_count(db_query_ctx *ctx, NamespaceId n);
DeclEntry db_read_namespace_member_at(db_query_ctx *ctx, NamespaceId n,
                                      uint32_t i);

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

// Producer-side self-data read. Returns NULL if `def` is not a
// KIND_FUNCTION or its body_ast_id_map row hasn't been allocated.
// The slot is owned by the INFER_BODY / BODY_SCOPES compute frames;
// callers OUTSIDE those frames see the last computed snapshot.
const struct BodyAstIdMap *
db_get_fn_body_ast_id_map_untracked(struct db *s, DefId d);

// ============================================================
// Producer-write typed setters (follow-ups #10).
// ============================================================
//
// These wrap the raw-pointer-cast producer writes that used to live
// in type.c / scope.c / body_scopes.c behind a typed surface. Each
// one encodes a "safe co-product write" contract — same entity key
// on both sides of the slot, owned by the calling query's frame —
// in code, so a future reader can't accidentally turn the pattern
// into a toxic side-channel by editing the raw-pointer-cast away.
//
// Each setter MUST be called from inside its owning producer
// query's compute (the slot ID is the same as the query's key);
// the wrappers do not assert this because the producer-call shape
// guarantees it structurally. Driver-level / cross-query writes
// have no business calling these — use the producer query.

// NAMESPACE_TYPE producer — stamps the member-pool range columns
// (field_lo, field_len) for `nsid`.
void db_write_namespace_type_outputs(struct db *s, NamespaceId nsid,
                                     uint32_t field_lo, uint32_t field_len);

// DEF_IDENTITY producer — stamps the three identity columns for a
// freshly-minted `def` (name, parent_module, ast_id). Caller is
// responsible for ensuring `def` was just created via db_create_def
// in the same frame.
void db_write_def_identity(struct db *s, DefId def, StrId name,
                           NamespaceId parent_module, AstId ast_id);

// NAMESPACE_SCOPES producer — stamps the five scope-row columns
// (parent, meta, owning_module, decl_lo, decl_len) for `scope_id`.
void db_write_namespace_scope(struct db *s, ScopeId scope_id,
                              ScopeId parent, ScopeMeta meta,
                              NamespaceId owning_module,
                              uint32_t decl_lo, uint32_t decl_len);

// INFER_BODY / BODY_SCOPES producer — reset the body_ast_id_map
// row for `def` (free + init) and return the mut pointer so the
// caller can walk it. Returns NULL if `def` isn't a KIND_FUNCTION
// or the row hasn't been allocated (caller treats both as no-op).
struct BodyAstIdMap *
db_write_fn_body_ast_id_map_reset(struct db *s, DefId def);

#endif // ORE_DB_QUERY_CAPABILITY_H
