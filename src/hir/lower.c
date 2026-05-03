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
        case expr_Bin: {
            struct HirInstr* h = alloc_with_facts(s, HIR_BIN, expr);
            if (!h) return NULL;
            h->bin.op = (HirBinOp)expr->bin.op;
            h->bin.left  = lower_expr(ctx, expr->bin.Left);
            h->bin.right = lower_expr(ctx, expr->bin.Right);
            return h;
        }
        case expr_Unary: {
            struct HirInstr* h = alloc_with_facts(s, HIR_UNARY, expr);
            if (!h) return NULL;
            h->unary.op = expr->unary.op;
            h->unary.postfix = expr->unary.postfix;
            h->unary.operand = lower_expr(ctx, expr->unary.operand);
            return h;
        }
        case expr_Assign: {
            struct HirInstr* h = alloc_with_facts(s, HIR_ASSIGN, expr);
            if (!h) return NULL;
            h->assign.target = lower_expr(ctx, expr->assign.target);
            h->assign.value  = lower_expr(ctx, expr->assign.value);
            return h;
        }
        case expr_Field: {
            struct HirInstr* h = alloc_with_facts(s, HIR_FIELD, expr);
            if (!h) return NULL;
            h->field.object = lower_expr(ctx, expr->field.object);
            h->field.field_decl = expr->field.field.resolved;
            h->field.field_name_id = expr->field.field.string_id;
            return h;
        }
        case expr_Index: {
            struct HirInstr* h = alloc_with_facts(s, HIR_INDEX, expr);
            if (!h) return NULL;
            h->index.object = lower_expr(ctx, expr->index.object);
            h->index.index = lower_expr(ctx, expr->index.index);
            return h;
        }
        case expr_Product: {
            // C1.5: positional-only — field names are dropped at the HIR
            // level. Sema already validated against the expected struct's
            // field set, so HIR consumers (effect solver, future codegen)
            // can work from positions. If a downstream pass needs names
            // back, we extend HirProductPayload then.
            struct HirInstr* h = alloc_with_facts(s, HIR_PRODUCT, expr);
            if (!h) return NULL;
            h->product.type_hint = h->type;
            h->product.fields = vec_new_in(s->arena, sizeof(struct HirInstr*));
            if (expr->product.Fields) {
                for (size_t i = 0; i < expr->product.Fields->count; i++) {
                    struct ProductField* f = (struct ProductField*)
                        vec_get(expr->product.Fields, i);
                    if (!f || !f->value) continue;
                    struct HirInstr* v = lower_expr(ctx, f->value);
                    if (v) vec_push(h->product.fields, &v);
                }
            }
            return h;
        }
        case expr_ArrayLit: {
            struct HirInstr* h = alloc_with_facts(s, HIR_ARRAY_LIT, expr);
            if (!h) return NULL;
            h->array_lit.size = expr->array_lit.size
                ? lower_expr(ctx, expr->array_lit.size) : NULL;
            h->array_lit.elem_type = expr->array_lit.elem_type
                ? sema_type_of(s, expr->array_lit.elem_type) : NULL;
            h->array_lit.initializer = expr->array_lit.initializer
                ? lower_expr(ctx, expr->array_lit.initializer) : NULL;
            return h;
        }
        case expr_EnumRef: {
            struct HirInstr* h = alloc_with_facts(s, HIR_ENUM_REF, expr);
            if (!h) return NULL;
            // sema_check_expr fills name.resolved lazily when the
            // surrounding context provided an expected enum type. May
            // still be NULL in dump-only / error paths — that's fine,
            // variant_name_id is always set.
            h->enum_ref.variant_decl = expr->enum_ref_expr.name.resolved;
            h->enum_ref.variant_name_id = expr->enum_ref_expr.name.string_id;
            return h;
        }
        case expr_Block: {
            // Flatten model: every stmt before the last is emitted
            // directly into ctx->current_block as a side-effect; the
            // last stmt is returned as the value-producing instr for
            // the caller to emit (or attach structurally). This means
            // a block-bodied function's body_block contains the full
            // sequence of statements with no nesting marker — matches
            // how HirIf.then_block and friends will work in C2.
            if (!expr->block.stmts || expr->block.stmts->count == 0) {
                struct HirInstr* h = alloc_with_facts(s, HIR_CONST, expr);
                if (h) h->constant.value = NULL;
                return h;
            }
            Vec* stmts = expr->block.stmts;
            for (size_t i = 0; i + 1 < stmts->count; i++) {
                struct Expr** ep = (struct Expr**)vec_get(stmts, i);
                struct Expr* stmt = ep ? *ep : NULL;
                if (!stmt) continue;
                struct HirInstr* h = lower_expr(ctx, stmt);
                if (h) vec_push(ctx->current_block, &h);
            }
            struct Expr** ep = (struct Expr**)vec_get(stmts, stmts->count - 1);
            struct Expr* last = ep ? *ep : NULL;
            if (!last) return error_placeholder(s, expr);
            return lower_expr(ctx, last);
        }
        case expr_Bind: {
            struct HirInstr* h = alloc_with_facts(s, HIR_BIND, expr);
            if (!h) return NULL;
            h->bind.decl = expr->bind.name.resolved;
            h->bind.init = expr->bind.value
                ? lower_expr(ctx, expr->bind.value) : NULL;
            return h;
        }
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
