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
#include "../common/hashmap.h"

struct Sema;
struct Decl;
struct Module;
struct Instantiation;

// Per-call state threaded through `lower_expr`. Created by
// `lower_decl`; passed by pointer so callees can rebind
// `current_block` for nested-block contexts (used in Phase C2 onward).
//
// Phase H.B.2+: also serves as the bridge for sema's per-Expr HIR
// emission. `expr_hir` maps each AST Expr* to the HirInstr that sema
// allocated for it. Sema arms register their HirInstr after computing
// the type; subsequent recursion looks up sub-expressions' HirInstrs
// here. The map's lifetime matches LowerCtx's — per fn / instantiation.
struct LowerCtx {
    struct Sema* sema;
    struct HirFn* fn;          // function we're currently lowering into
    Vec* current_block;        // emit destination for new instrs
    HashMap expr_hir;          // Expr* (uint64_t) -> HirInstr*
                               // populated by sema arms during H.B
                               // migration; consulted by sub-expr
                               // wire-up (H.B.3+).
};

// Lower every function-shaped decl in `mod` into a `HirFn`. Returns a
// `HirModule` owned by the sema arena. Stored on Sema so consumers
// (`--dump-hir`, future codegen) can find it later.
struct HirModule* lower_module(struct Sema* sema, struct Module* mod);

// Lower one decl's body into a `HirFn`. NULL if decl isn't function-
// shaped (e.g. a type alias) — caller filters.
struct HirFn* lower_decl(struct Sema* sema, struct Decl* decl);

// Lower one generic instantiation's body. Each instantiation produces a
// distinct HirFn because comptime args produce structurally different
// bodies. Stored on `inst->hir`. Called from `sema_lower_modules` for
// every cached instantiation after the per-decl lowering pass.
//
// Sets sema->current_body to inst->body for the duration so per-Expr
// fact lookups land on the per-instantiation facts (not the generic's).
struct HirFn* lower_instantiation(struct Sema* sema,
                                   struct Instantiation* inst);

// Lower one expression. May emit side-instrs into `ctx->current_block`
// (e.g. block flattening). Returns the value-producing HirInstr; never
// NULL — failed lowering returns a `HIR_ERROR` placeholder so the HIR
// walk stays well-formed.
struct HirInstr* lower_expr(struct LowerCtx* ctx, struct Expr* expr);

#endif // ORE_HIR_LOWER_H
