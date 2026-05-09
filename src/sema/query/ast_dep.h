#ifndef ORE_SEMA_QUERY_AST_DEP_H
#define ORE_SEMA_QUERY_AST_DEP_H

// AST-dependency recording helpers.
//
// Any query body that reads expression structure (signature queries,
// const-eval, comptime predicate, etc.) must record a dep on the
// owning module's AST slot. Without that dep, edits to the source
// don't invalidate the cached result — the slot's revalidate walker
// sees no changed inputs and serves stale data. F2-F4 (PR 1) all
// belong to this class of bug.
//
// Two entry points, picked by what handle the caller has on hand:
//
//   record_ast_dep_for_def(s, def)   — caller has a DefId. Walks
//                                      DefInfo → ScopeInfo →
//                                      owner_module → query_module_ast.
//                                      Replaces the per-file
//                                      sig_record_ast_dep helper that
//                                      lived in decl_data.c.
//
//   record_ast_dep_for_span(s, span) — caller has an Expr (just pass
//                                      its span) or any other
//                                      span-bearing node. Resolves
//                                      the module via module_for_span.
//                                      Replaces record_ast_dep_for_expr
//                                      from const_eval.c.
//
// Both are no-ops when called outside an active query frame (the
// underlying record_dep_on_parent is similarly defensive).

#include "../ids/ids.h"  // DefId

struct Sema;
struct Span;

void record_ast_dep_for_def(struct Sema *s, DefId def);
void record_ast_dep_for_span(struct Sema *s, struct Span span);

#endif  // ORE_SEMA_QUERY_AST_DEP_H
