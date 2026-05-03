// HIR lowering — Phase C1 sub-commit 1: skeleton.
//
// Every expression lowers to HIR_ERROR for now. Subsequent C1
// sub-commits replace each arm with the real lowering. The placeholder
// approach keeps the HIR walk well-formed (every Expr produces exactly
// one HirInstr) so consumers like `--dump-hir` work end-to-end before
// individual arms are filled in.

#include "lower.h"

#include "../sema/sema.h"
#include "../sema/type.h"
#include "../name_resolution/name_resolution.h"

// ----- helpers -----

// Transcribe sema's per-Expr facts onto a freshly-allocated HirInstr.
// Floors `type` at `s->unknown_type` so HirInstr.type is never NULL —
// downstream walkers can rely on that invariant. Used by every real
// lowering arm.
static void copy_facts_from(struct Sema* s, struct HirInstr* h, struct Expr* expr) {
    if (!h) return;
    struct Type* t = sema_type_of(s, expr);
    h->type = t ? t : s->unknown_type;
    h->semantic_kind = sema_semantic_of(s, expr);
    h->region_id = sema_region_of(s, expr);
}

static struct HirInstr* alloc_with_facts(struct Sema* s, HirInstrKind kind,
                                          struct Expr* expr) {
    struct Span span = expr ? expr->span : (struct Span){0};
    struct HirInstr* h = hir_instr_new(s->arena, kind, span);
    if (!h) return NULL;
    copy_facts_from(s, h, expr);
    return h;
}

static struct HirInstr* error_placeholder(struct Sema* s, struct Expr* expr) {
    struct Span span = expr ? expr->span : (struct Span){0};
    struct HirInstr* h = hir_instr_new(s->arena, HIR_ERROR, span);
    if (!h) return NULL;
    h->type = s->error_type;
    h->semantic_kind = SEM_UNKNOWN;
    return h;
}

// ----- public API -----

struct HirInstr* lower_expr(struct LowerCtx* ctx, struct Expr* expr) {
    if (!ctx || !expr) return NULL;
    struct Sema* s = ctx->sema;

    switch (expr->kind) {
        case expr_Lit: {
            // C1.3: HIR_CONST carries the parsed value via its
            // ConstValue payload. The literal's source string lives
            // in the pool (expr->lit.string_id); attaching the parsed
            // value is left for when const_eval-of-lit is wired in
            // (C1 doesn't depend on the value, only the type).
            struct HirInstr* h = alloc_with_facts(s, HIR_CONST, expr);
            if (h) h->constant.value = NULL;
            return h;
        }
        case expr_Ident: {
            struct HirInstr* h = alloc_with_facts(s, HIR_REF, expr);
            if (h) h->ref.decl = expr->ident.resolved;
            return h;
        }
        case expr_Asm: {
            struct HirInstr* h = alloc_with_facts(s, HIR_ASM, expr);
            if (h) h->asm_instr.string_id = expr->asm_expr.string_id;
            return h;
        }
        case expr_Wildcard:
            // Wildcard is a pattern-position placeholder; reaching it in
            // value position is an upstream error caught by sema. Emit a
            // HIR_ERROR placeholder so the walk stays well-formed.
            return error_placeholder(s, expr);
        default:
            // Every kind not yet covered emits a HIR_ERROR placeholder.
            // C1.4-C1.7 narrow the placeholder set; phase D and beyond
            // delete the remainder.
            return error_placeholder(s, expr);
    }
}

// Function-shaped means: a Bind whose value is a Lambda. Type aliases,
// effect declarations, struct/enum decls, and module-level value binds
// produce HIR through different paths in later phases.
static bool decl_is_function_shaped(struct Decl* decl) {
    if (!decl || !decl->node) return false;
    if (decl->node->kind != expr_Bind) return false;
    struct Expr* value = decl->node->bind.value;
    return value && value->kind == expr_Lambda;
}

struct HirFn* lower_decl(struct Sema* s, struct Decl* decl) {
    if (!s || !decl_is_function_shaped(decl)) return NULL;
    struct HirFn* fn = hir_fn_new(s->arena, decl, decl->name.span);
    if (!fn) return NULL;

    struct Expr* lambda = decl->node->bind.value;
    fn->ret_type = sema_type_of(s, lambda) ? sema_type_of(s, lambda)->ret : NULL;

    // Lower the lambda body into the fn's body block. The body may be
    // any Expr (typically a Block); lower_expr emits side-instrs into
    // ctx.current_block as it goes. The returned final instr is the
    // value of the body — for now we discard it because Phase C1's
    // skeleton doesn't yet model "the function returns this value."
    // Real return wiring lands in C2 (return / control flow).
    struct LowerCtx ctx = {
        .sema = s,
        .fn = fn,
        .current_block = fn->body_block,
    };
    if (lambda->lambda.body) {
        struct HirInstr* tail = lower_expr(&ctx, lambda->lambda.body);
        if (tail) vec_push(fn->body_block, &tail);
    }
    return fn;
}

struct HirModule* lower_module(struct Sema* s, struct Module* mod) {
    if (!s || !mod) return NULL;
    struct HirModule* hmod = hir_module_new(s->arena, mod);
    if (!hmod || !mod->scope || !mod->scope->decls) return hmod;

    Vec* decls = mod->scope->decls;
    for (size_t i = 0; i < decls->count; i++) {
        struct Decl** dp = (struct Decl**)vec_get(decls, i);
        struct Decl* d = dp ? *dp : NULL;
        if (!d) continue;
        struct HirFn* fn = lower_decl(s, d);
        if (fn) vec_push(hmod->functions, &fn);
    }
    return hmod;
}
