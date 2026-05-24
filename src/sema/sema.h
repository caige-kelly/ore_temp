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

// === Per-decl resolved-types builder =======================================
//
// rust-analyzer's InferenceResult pattern, flattened SoA-style into the
// shared db.node_types_pool. Each per-decl query that types a sub-tree
// (infer_body, fn_signature, struct_field_types) constructs one of
// these builders at the top of its body, has every type-resolving sema
// call write into the pool range via the active builder, then
// finalizes into a NodeTypesRange stored on the matching per-kind
// column.
//
// Nested queries: builder_begin pushes the previous s->active builder
// onto the new builder's `prev` field; builder_end restores it. So an
// outer infer_body that recursively triggers fn_signature on another
// def correctly switches active builders for the duration of the inner
// query, then resumes its own writes.
//
// Push semantics: builder_push silently no-ops if no active builder OR
// if the node falls outside the builder's [node_min, node_max] range.
// This is what lets a sub-query's recursive sema_resolve_type_expr
// touch nodes the outer query "owns" — those writes are dropped
// because the inner builder doesn't cover them, and they'll be picked
// up when the outer query's own walk visits them.
typedef struct NodeTypeBuilder {
  struct NodeTypeBuilder *prev;  // saved s->active_node_type_builder
  FileId   file_local;
  uint32_t types_off;            // start offset in db.node_types_pool
  uint32_t node_min;
  uint32_t node_max;             // inclusive
  uint32_t types_len;            // node_max - node_min + 1
  Fingerprint fp;                // accumulated over (node.idx, type.v) pairs
} NodeTypeBuilder;

// Begin: allocate types_len = (node_max - node_min + 1) IP_NONE slots
// in db.node_types_pool, save off+min on the builder, push it onto the
// active stack (s->active_node_type_builder = b). Caller-supplied
// builder lives on the stack of the calling query body. node_min ==
// node_max == 0 produces an empty range (cycle / no-coverage case);
// pushes nothing to the pool.
void sema_node_type_builder_begin(struct db *s, NodeTypeBuilder *b,
                                  FileId file_local,
                                  uint32_t node_min, uint32_t node_max);

// Push (node, type) onto the active builder. No-op if no builder is
// active OR if node falls outside [node_min, node_max]. Idempotent on
// repeat writes for the same node (latest write wins; same shape as
// the old sema_cache_node_type).
void sema_node_type_builder_push(struct db *s, AstNodeId node, IpIndex type);

// Finalize the builder: pop it off the active stack (restore prev),
// return the assembled NodeTypesRange. The caller stores this range on
// the matching per-kind column. `out_fp` receives the accumulated
// fingerprint; pass NULL to discard.
NodeTypesRange sema_node_type_builder_end(struct db *s, NodeTypeBuilder *b,
                                          Fingerprint *out_fp);

// Lookup helper (used by db_query_node_type's router): given a range,
// return the type for `node`. Returns IP_NONE for out-of-range nodes
// or for an empty range.
IpIndex sema_node_types_range_lookup(struct db *s, NodeTypesRange range,
                                     AstNodeId node);

// AST sub-tree range — walk every descendant of `root`, return the
// min/max AstNodeId.idx seen. Used by query bodies to size their
// NodeTypeBuilder's range over the AST they own. (For NONE root, both
// out params are zeroed.)
void sema_ast_subtree_range(ASTStore *ast, AstNodeId root,
                            uint32_t *out_min, uint32_t *out_max);

// (sema_cache_node_type removed 2026-05-24 — Option-C migration.
//  Per-node cache writes go through sema_node_type_builder_push into
//  the active per-decl query's pool range. Callers that used to call
//  sema_cache_node_type now call sema_node_type_builder_push directly.)

// (sema_stamp_file_types removed 2026-05-24 — Option-C migration.
//  Per-decl salsa queries now own their own NodeTypesRange in
//  db.node_types_pool; the unified node_type router reads from those
//  ranges directly. No post-typecheck walker needed.)

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
//
// `found_out` (may be NULL) distinguishes "found, type unknown" (the
// bind exists but its RHS didn't type — e.g., unimplemented builtin)
// from "name truly not in any body scope". When NULL, callers can't
// tell those apart from the IpIndex alone — both yield IP_NONE.
// Callers that need to emit "undefined identifier" diagnostics MUST
// pass &found and gate the diag on found == false.
IpIndex sema_body_scope_lookup(struct db *s, DefId fn_def,
                               AstNodeId use_node, StrId name,
                               bool *found_out);

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
