#ifndef ORE_SEMA_INTERNAL_H
#define ORE_SEMA_INTERNAL_H

#include "sema.h"
#include "const_eval.h"
#include "../hir/hir.h"

// ----- Per-Decl sema cache -----
//
// Sema-only fields used to live directly on `struct Decl`. They've been
// pulled into this side-table so that name resolution (which doesn't run
// sema) doesn't carry unused storage and so a future incremental compiler
// can invalidate per-Decl sema state by clearing entries in this map.
struct SemaDeclInfo {
    struct QuerySlot type_query;
    struct QuerySlot effect_sig_query;
    struct QuerySlot body_effects_query;
    struct Type* type;
    struct EffectSig* effect_sig;
    struct EffectSet* body_effects;
    struct ConstValue value;
    bool fold_in_progress;

};

// Lookup-or-create. Returns a stable pointer for the lifetime of `sema`.
struct SemaDeclInfo* sema_decl_info(struct Sema* sema, struct Decl* decl);

// Convenience read accessors. Return defaults when the Decl has no entry yet.
struct Type* sema_decl_type(struct Sema* sema, struct Decl* decl);
struct EffectSig* sema_decl_effect_sig(struct Sema* sema, struct Decl* decl);
struct EffectSet* sema_decl_body_effects(struct Sema* sema, struct Decl* decl);

void sema_error(struct Sema* sema, struct Span span, const char* fmt, ...);

// Sema-side HIR emission. Allocates a HirInstr, registers it in
// the current body's expr_hir map, returns it for the arm to populate.
// Per-body keying handles per-instantiation correctly. Returns NULL
// only when no current_body is set (sig-resolution body should always
// be active during walking).
struct HirInstr* sema_emit_hir_instr(struct Sema* sema, struct Expr* expr,
    HirInstrKind kind);

// ----- HIR recording -----
//
// Two entry points, one storage (the per-CheckedBody expr_hir map):
//
//   body_record_hir(sema, body, ...)
//     Canonical: callers that *have* a CheckedBody in hand pass it directly.
//     Use this when you've just allocated/entered a body (e.g. instantiation
//     re-walk, per-decl body checking) and want explicit control.
//
//   sema_record_hir(sema, ...)
//     Convenience for the common checker walk: writes into sema->current_body.
//     If current_body is NULL it warns once on stderr and drops the record —
//     this is a *bug indicator*, not a normal path. Don't rely on the no-op.
//
// Both allocate (or update) a HirInstr in body->expr_hir and populate its
// type / semantic_kind / region_id / payload. Prefer the explicit form in
// new code; reach for the convenience form only inside the recursive
// checker that pushes/pops bodies as part of its walk.
void body_record_hir(struct Sema* sema, struct CheckedBody* body, struct Expr* expr,
    struct Type* type, SemanticKind semantic_kind, uint32_t region_id);
void sema_record_hir(struct Sema* sema, struct Expr* expr, struct Type* type,
    SemanticKind semantic_kind, uint32_t region_id);

struct CheckedBody* sema_body_new(struct Sema* sema, struct Decl* decl,
    struct Module* module, struct Instantiation* instantiation);
struct CheckedBody* sema_enter_body(struct Sema* sema, struct CheckedBody* body);
void sema_leave_body(struct Sema* sema, struct CheckedBody* previous);


#endif // ORE_SEMA_INTERNAL_H