#ifndef ORE_SEMA_H
#define ORE_SEMA_H

#include "../db/db.h"
#include "../db/intern_pool/intern_pool.h"
#include "../parser/ast.h"

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
//     can read/write the db's columns (defs.types, defs.local_scopes, …)
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
IpIndex sema_infer_body(struct db *s, DefId def);
IpIndex sema_type_of_expr(struct db *s, ASTStore *ast, AstNodeId node,
                          ModuleId mid, DefId enclosing_fn);

// === Type-resolution helpers ================================================

// Resolve a type-position AST expression to an IpIndex. Handles
// primitives (i32, bool, …), keyword type forms (void, type),
// constructors (^T, []T, [N]T, ?T, [^]T, const T, Fn(…) → R), and
// user-defined identifiers via resolve_ref → type_of_def.
IpIndex sema_resolve_type_expr(struct db *s, ASTStore *ast, AstNodeId id,
                               ModuleId mid);

// Read a StrId from an AST node that's an emit_ident'd AST_EXPR_PATH.
// AST_DECL_PARAM, AST_DECL_FIELD, and AST_DECL_VARIANT all store their
// "name" extras slot as an AstNodeId pointing to a path node, NOT a
// StrId directly — the real interned string lives on the path node's
// data.string_id. Returns {0} for anonymous (NONE) slots or for nodes
// that aren't paths.
StrId sema_decl_name_from_node(ASTStore *ast, uint32_t name_node_idx);

// Read a local-scope entry by name for `enclosing_fn`. Returns IP_NONE
// if the def has no local scope (infer_body hasn't run, or it's not
// a fn) or if the name isn't bound locally. Callers fall through to
// module scope on IP_NONE.
IpIndex sema_local_scope_lookup(struct db *s, DefId enclosing_fn,
                                StrId name);

// === Orchestration / dumps (called from the driver) =========================

// Run the full type-checking pipeline for `mid`: module_exports first,
// then def_identity + type_of_def + infer_body for every top-level
// decl. Idempotent — repeated calls hit the salsa cache after the
// first.
void sema_check_module(struct db *s, ModuleId mid);

// Verification dump: top-level decl types + per-fn local scopes.
// The driver guards this on !ORE_NO_DUMP.
void sema_dump_module(struct db *s, ModuleId mid);

#endif // ORE_SEMA_H
