// HIR lowering — AST → HIR transcription.
//
// Runs after `sema_check_expressions` completes. Sema's per-Expr facts
// (type, semantic kind, region id) are already populated; lowering
// transcribes them onto the corresponding HirInstr nodes. Phases B-G
// build HIR alongside the existing facts table; Phase H deletes the
// facts after consumers migrate to HirInstr-keyed lookups.
//
// One-paragraph design:
//   - `lower_module(sema, mod)` walks a module's top-level decls and
//     lowers each function-shaped Bind into a `HirFn`. Type aliases and
//     other non-function decls are left to future phases.
//   - `lower_decl(sema, decl)` produces a single HirFn from one Decl.
//   - `lower_expr(ctx, expr)` lowers one expression, emitting any
//     side-instrs into `ctx->current_block` and returning the value-
//     producing HirInstr.
//
// Phase C1 scope (this commit): skeleton + entry hookup. Every
// expression lowers to `HIR_ERROR` until subsequent C1 sub-commits
// fill in the real cases. The point is to prove the entry pipeline
// works end-to-end before we start filling in lowering arms.

#ifndef ORE_HIR_LOWER_H
#define ORE_HIR_LOWER_H

#include "hir.h"

struct Sema;
struct Decl;
struct Module;

// Per-call state threaded through `lower_expr`. Created by
// `lower_decl`; passed by pointer so callees can rebind
// `current_block` for nested-block contexts (used in Phase C2 onward).
struct LowerCtx {
    struct Sema* sema;
    struct HirFn* fn;          // function we're currently lowering into
    Vec* current_block;        // emit destination for new instrs
};

// Lower every function-shaped decl in `mod` into a `HirFn`. Returns a
// `HirModule` owned by the sema arena. Stored on Sema so consumers
// (`--dump-hir`, future codegen) can find it later.
struct HirModule* lower_module(struct Sema* sema, struct Module* mod);

// Lower one decl's body into a `HirFn`. NULL if decl isn't function-
// shaped (e.g. a type alias) — caller filters.
struct HirFn* lower_decl(struct Sema* sema, struct Decl* decl);

// Lower one expression. May emit side-instrs into `ctx->current_block`
// (e.g. block flattening). Returns the value-producing HirInstr; never
// NULL — failed lowering returns a `HIR_ERROR` placeholder so the HIR
// walk stays well-formed.
struct HirInstr* lower_expr(struct LowerCtx* ctx, struct Expr* expr);

#endif // ORE_HIR_LOWER_H
