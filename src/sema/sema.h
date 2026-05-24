#ifndef ORE_SEMA_H
#define ORE_SEMA_H

#include "../db/db.h"
#include "../db/intern_pool/intern_pool.h"
#include "../parser/ast.h"

// Per-fn body-scope tree types live in db.h (data shapes only —
// ScopeRow, ScopedBind, FnBody, BODY_SCOPE_NONE). Operations on them —
// build, lookup — are sema's job and declared below.

// =============================================================================
// sema — language semantics layer
//
// Architecture: db/ is the salsa-style query engine + storage + basic
// infrastructure (parse, name resolution, def materialization, module
// scoping). sema/ holds the language-semantic logic (typechecking,
// coercion, const evaluation, layout) and the orchestration that drives
// it.
//
// The split:
//   - db/query/*.c — thin salsa wrappers. Each wrapper does DB_QUERY_GUARD,
//     calls the matching sema_* impl, stamps the result + fingerprint via
//     db_query_succeed. No semantic decisions live here.
//   - sema/*.c — the actual work. Sema functions take `struct db *` so they
//     can read/write the db's per-kind tables (db.fns, db.structs, …)
//     and call back into db queries (db_query_resolve_ref, …) to record
//     salsa deps for invalidation.
//
// Sema is a *consumer* of db queries, not a producer of cached results
// itself. All caching lives in db's slot machinery.
// =============================================================================

// === Type-query implementations =============================================
//
// One sema_* function per salsa query. Each is the body for the
// matching db_query_* wrapper. Order in this file mirrors the chunk
// progression (1 → 5b).

IpIndex sema_type_of_def(struct db *s, DefId def);
IpIndex sema_fn_signature(struct db *s, DefId def);
// Body type inference. Out-param `body_fp_out` (may be NULL) accumulates
// a fingerprint over the typed body — (visit_idx, types[i].v) folded for
// every AST node whose TinySpan is contained in the body's source range.
// Stable under sibling-decl edits; reflects any body-content type change.
// See plan Phase 7 for rationale.
IpIndex sema_infer_body(struct db *s, DefId def, Fingerprint *body_fp_out);
IpIndex sema_type_of_expr(struct db *s, ASTStore *ast, AstNodeId node,
                          NamespaceId nsid, DefId enclosing_fn,
                          FileId file_local);

// Bidirectional type check: types `node` (the synth side), then verifies
// the result coerces to `expected` (the check side). Returns true on
// success; emits a db_emit_error_t on mismatch via the current query
// frame's slot. `file_local` is needed to look up the node's span for
// the diag — pass through whatever the caller has.
//
// For AST_STMT_BLOCK, propagates `expected` to the LAST statement (Zig
// rule: block's value = tail expression). For AST_STMT_IF, propagates
// to both branches independently so a wrong branch is pinpointed in the
// diag rather than reported as "branches don't match." Other shapes
// fall through to synth-then-compare.
//
// Coercion rules (chunk 5i v1): equal types pass; comptime_int coerces
// to any concrete int/float; comptime_float coerces to f32/f64. Full
// Zig-variance (ptr/slice constness drops, optional-coercion, error-
// union wrapping) is the chunk-when-we-port-coerce.c follow-up.
bool sema_check_expr(struct db *s, ASTStore *ast, AstNodeId node,
                     IpIndex expected, NamespaceId nsid, DefId enclosing_fn,
                     FileId file_local);

// === Type-resolution helpers ================================================

// Resolve a type-position AST expression to an IpIndex. Handles
// primitives (i32, bool, …), keyword type forms (void, type),
// constructors (^T, []T, [N]T, ?T, [^]T, const T, Fn(…) → R), and
// user-defined identifiers via resolve_ref → type_of_def.
//
// `file_local` is the FileId backing `ast`; threaded through so every
// recursive visit can stamp the per-node type cache (sema_cache_node_type)
// for downstream IDE consumers. Pass the FileId you have in scope — the
// previous workaround of deriving it from `nsid` via db_get_namespace_files
// is gone.
IpIndex sema_resolve_type_expr(struct db *s, ASTStore *ast, AstNodeId id,
                               NamespaceId nsid, FileId file_local);

// Per-node type cache write — the single source of truth for stamping
// `FileNodeData.types[node.idx]`. Sema's wrapper paths call this so any
// node visited during typecheck has its computed type readable from the
// cache without re-running resolution. Safe on NONE-shaped inputs.
void sema_cache_node_type(struct db *s, FileId file_local, AstNodeId node,
                          IpIndex type);

// Post-typecheck walker that re-stamps FileNodeData.types[] for every
// AST node whose type is recoverable from salsa-cached query results.
// Called from sema_check_module after the per-decl loop, so it runs
// even when each individual decl's type_of_def early-cuts. This is the
// load-bearing path that survives reparse-zeroing of the per-file
// cache + sibling-decl edits that leave THIS decl's salsa fingerprint
// unchanged. See type_of_def.c for the implementation notes.
void sema_stamp_file_types(struct db *s, FileId fid);

// (sema_lookup_primitive_name removed 2026-05-24 — primitives are now
//  real DefIds in every namespace's parent scope, populated by
//  db_init_primitives. Name resolution goes through the standard
//  resolve_ref → type_of_def chain. See src/db/db.c for the seed list
//  and db_primitive_type_for for the DefId → IpIndex mapping.)

// (sema_decl_name_from_node helper removed 2026-05-21 — name extras for
//  PARAM/FIELD/VARIANT/INIT_FIELD now store StrId directly. Read with
//  `(StrId){.idx = ex[0]}` at the call site.)

// === Body scopes ============================================================

// Build the body scope tree for `fn_def` into the shared db pools
// (db.body_scope_rows / _binds / db.node_to_scope), publishing the
// per-fn ranges into db.fns.body[row]. Invoked from db_query_body_scopes;
// not normally called directly. Returns an all-zero FnBody if `fn_def`
// doesn't have a fn-shaped body.
FnBody sema_body_scopes(struct db *s, DefId fn_def);

// Look up `name` from the scope at `use_node` within `fn_def`'s body
// scope tree. Walks the parent chain from the innermost scope outward;
// latest bind in each scope wins for shadowing. Reads db.fns.body[row]
// + the pools raw — NO dep recorded — so the caller must already be
// inside a query frame that declared a dep on body_scopes(fn_def).
// Returns IP_NONE on miss (caller falls through to module scope).
IpIndex sema_body_scope_lookup(struct db *s, DefId fn_def,
                               AstNodeId use_node, StrId name);

// === Orchestration / dumps (called from the driver) =========================

// Run the full type-checking pipeline for `nsid`: module_exports first,
// then def_identity + type_of_def + infer_body for every top-level
// decl. Idempotent — repeated calls hit the salsa cache after the
// first.
void sema_check_module(struct db *s, NamespaceId nsid);

// Verification dump: top-level decl types + per-fn local scopes.
// The driver guards this on !ORE_NO_DUMP.
void sema_dump_module(struct db *s, NamespaceId nsid);

#endif // ORE_SEMA_H
