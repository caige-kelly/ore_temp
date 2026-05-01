#ifndef ORE_SEMA_INTERNAL_H
#define ORE_SEMA_INTERNAL_H

#include "sema.h"

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
};

// Lookup-or-create. Returns a stable pointer for the lifetime of `sema`.
struct SemaDeclInfo* sema_decl_info(struct Sema* sema, struct Decl* decl);

// Convenience read accessors. Return defaults when the Decl has no entry yet.
struct Type* sema_decl_type(struct Sema* sema, struct Decl* decl);
struct EffectSig* sema_decl_effect_sig(struct Sema* sema, struct Decl* decl);
struct EffectSet* sema_decl_body_effects(struct Sema* sema, struct Decl* decl);

void sema_error(struct Sema* sema, struct Span span, const char* fmt, ...);

// ----- Fact recording -----
//
// Two entry points, one storage:
//
//   body_record_fact(sema, body, ...)
//     Canonical: callers that *have* a CheckedBody in hand pass it directly.
//     Use this when you've just allocated/entered a body (e.g. instantiation
//     re-walk, per-decl body checking) and want explicit control.
//
//   sema_record_fact(sema, ...)
//     Convenience for the common checker walk: writes into sema->current_body.
//     If current_body is NULL it warns once on stderr and drops the fact —
//     this is a *bug indicator*, not a normal path. Don't rely on the no-op.
//
// Both end up in the same SemaFact vec on the body. Prefer the explicit form
// in any new code; reach for the convenience form only inside the recursive
// checker that pushes/pops bodies as part of its walk.
void body_record_fact(struct Sema* sema, struct CheckedBody* body, struct Expr* expr,
    struct Type* type, SemanticKind semantic_kind, uint32_t region_id);
void sema_record_fact(struct Sema* sema, struct Expr* expr, struct Type* type,
    SemanticKind semantic_kind, uint32_t region_id);

struct CheckedBody* sema_body_new(struct Sema* sema, struct Decl* decl,
    struct Module* module, struct Instantiation* instantiation);
struct CheckedBody* sema_enter_body(struct Sema* sema, struct CheckedBody* body);
void sema_leave_body(struct Sema* sema, struct CheckedBody* previous);

// bypass circular reference
// Bound comptime arguments for one instantiation of a generic decl. The values
// vector mirrors the generic decl's comptime parameters in declaration order.
struct ComptimeArgTuple {
    Vec* values;  // Vec of ConstValue
};

#endif // ORE_SEMA_INTERNAL_H