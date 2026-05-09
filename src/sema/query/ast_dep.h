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
#include "query.h"       // sema_mark_frame_untracked (debug)

struct Sema;
struct Span;

void record_ast_dep_for_def(struct Sema *s, DefId def);
void record_ast_dep_for_span(struct Sema *s, struct Span span);

// SEMA_READ_UNTRACKED — wrap a non-query read inside a query body to
// declare "this read is intentional and not deps-tracked." In debug
// builds, marks the active query frame so the revalidate walker
// forces RECOMPUTE for the producing slot (Salsa's DerivedUntracked
// pattern). In release builds, the macro is a no-op shim around the
// expression — zero overhead.
//
// `s` is the Sema*. `expr` is evaluated and its value returned;
// `why` is a string literal documenting the reason this read can
// bypass the dep graph (greppable in code review).
//
// Use sparingly. The right answer for a read inside a query body is
// almost always to call a query function (which records its own dep)
// or to wrap the data in a slot of its own. Reach for this macro
// only when reading immutable infrastructure (the string pool,
// interned types, append-only registries) or when implementing
// invalidation walkers themselves.
//
// Examples:
//
//   const char *name = SEMA_READ_UNTRACKED(s,
//       pool_get(&s->pool, name_id, 0),
//       "string pool is append-only and immutable across revisions");
//
//   struct LayoutEntry *e = SEMA_READ_UNTRACKED(s,
//       hashmap_get(&s->layout_of_type, key),
//       "invalidation walker peeks at the slot to mark it dirty");
//
// See bug_of_bugs.md #16 / R2 for the full rationale.
#ifdef ORE_DEBUG_QUERIES
#define SEMA_READ_UNTRACKED(s, expr, why)                                  \
    (sema_mark_frame_untracked((s), (why)), (expr))
#else
#define SEMA_READ_UNTRACKED(s, expr, why) (expr)
#endif

#endif  // ORE_SEMA_QUERY_AST_DEP_H
