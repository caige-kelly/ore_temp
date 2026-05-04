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
#include "../sema/checker.h"
#include "../sema/const_eval.h"
#include "../sema/instantiate.h"
#include "../name_resolution/name_resolution.h"

// Forward decls: these helpers are defined after lower_expr because
// they re-enter lowering for nested bodies. Used by various arms in
// lower_expr below.
static struct HirFn* lower_fn_like(struct Sema* s,
                                    struct Decl* source,
                                    Vec* params,
                                    struct Type* ret_ty,
                                    struct Expr* body,
                                    struct Span span);
static Vec* lower_block_into(struct LowerCtx* ctx, struct Expr* expr);

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

// Lower a HandlerExpr's body (operations + lifecycle clauses) onto a
// fresh HIR_HANDLER_VALUE instruction. Shared between the
// `expr_Handler` value-position case and `expr_With`'s caller field
// (a HandlerExpr* directly after the parser's With narrow). Returns
// NULL if h is NULL — caller filters.
static struct HirInstr* lower_handler_value(struct LowerCtx* ctx,
                                             struct HandlerExpr* h,
                                             struct Span span) {
    if (!ctx || !h) return NULL;
    struct Sema* s = ctx->sema;
    struct HirInstr* hi = hir_instr_new(s->arena, HIR_HANDLER_VALUE, span);
    if (!hi) return NULL;
    hi->type = s->unknown_type;
    hi->semantic_kind = SEM_VALUE;
    hi->handler_value.effect_decl = h->effect_decl;
    hi->handler_value.operations = vec_new_in(s->arena, sizeof(struct HirHandlerOp*));
    if (h->operations) {
        for (size_t i = 0; i < h->operations->count; i++) {
            struct HandlerOp** opp = (struct HandlerOp**)vec_get(h->operations, i);
            struct HandlerOp* op = opp ? *opp : NULL;
            if (!op) continue;
            struct HirHandlerOp* hop = arena_alloc(s->arena, sizeof(struct HirHandlerOp));
            if (!hop) continue;
            hop->is_ctl = op->is_ctl;
            hop->params = vec_new_in(s->arena, sizeof(struct Decl*));
            if (op->params) {
                for (size_t j = 0; j < op->params->count; j++) {
                    struct Param* p = (struct Param*)vec_get(op->params, j);
                    if (!p) continue;
                    struct Decl* pd = p->name.resolved;
                    vec_push(hop->params, &pd);
                }
            }
            // Match the source op against the effect's child_scope so
            // downstream consumers (Phase E discharge) can map the
            // implementation back to its op_decl.
            hop->op_decl = NULL;
            if (h->effect_decl && h->effect_decl->child_scope) {
                hop->op_decl = (struct Decl*)hashmap_get(
                    &h->effect_decl->child_scope->name_index,
                    (uint64_t)op->name.string_id);
            }
            hop->body_block = op->body
                ? lower_block_into(ctx, op->body) : NULL;
            vec_push(hi->handler_value.operations, &hop);
        }
    }
    hi->handler_value.initially_block = h->initially_clause
        ? lower_block_into(ctx, h->initially_clause) : NULL;
    hi->handler_value.finally_block = h->finally_clause
        ? lower_block_into(ctx, h->finally_clause) : NULL;
    hi->handler_value.return_block = h->return_clause
        ? lower_block_into(ctx, h->return_clause) : NULL;
    return hi;
}

// Lower an expression into a fresh sub-block (Vec<HirInstr*>). Used by
// HIR_IF / HIR_LOOP / HIR_SWITCH carriers whose bodies are nested
// blocks, not part of the surrounding flatten stream. Saves and
// restores ctx->current_block around the call so Block flattening
// inside `expr` lands in the new sub-block.
//
// Returns NULL when expr is NULL (caller filters).
static Vec* lower_block_into(struct LowerCtx* ctx, struct Expr* expr) {
    if (!ctx || !expr) return NULL;
    Vec* sub = vec_new_in(ctx->sema->arena, sizeof(struct HirInstr*));
    Vec* saved = ctx->current_block;
    ctx->current_block = sub;
    struct HirInstr* tail = lower_expr(ctx, expr);
    if (tail) vec_push(sub, &tail);
    ctx->current_block = saved;
    return sub;
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
        case expr_If: {
            // Phase D: comptime splicing. `comptime if` evaluates the
            // condition during lowering and emits only the live branch
            // — the dead arm never reaches HIR. Op resolution against
            // the spliced HIR sees exactly one branch, so a body like
            //   comptime if (debug) with handler_a else with handler_b
            //   alloc(...)
            // resolves cleanly: only the live `with` block is in scope
            // when `alloc` is reached.
            //
            // If condition eval fails (CONST_INVALID — typically because
            // sema already diagnosed it elsewhere), fall through to the
            // runtime HIR_IF so the walk stays well-formed and the
            // diagnostic the user actually sees comes from sema.
            //
            // elif chains: the parser desugars `if/elif/else` into nested
            // expr_If nodes, but only the outermost If carries
            // `is_comptime`. We walk the else-chain inline so a
            // `comptime if/elif/.../else` dissolves *every* arm, not just
            // the first split.
            if (expr->is_comptime && expr->if_expr.condition) {
                struct Expr* probe = expr;
                bool unfoldable = false;
                while (probe && probe->kind == expr_If && probe->if_expr.condition) {
                    struct EvalResult er = sema_const_eval_expr(s,
                        probe->if_expr.condition, NULL);
                    if (er.value.kind != CONST_BOOL) {
                        unfoldable = true;
                        break;
                    }
                    if (er.value.bool_val) {
                        if (probe->if_expr.then_branch) {
                            return lower_expr(ctx, probe->if_expr.then_branch);
                        }
                        struct HirInstr* h = alloc_with_facts(s, HIR_CONST, expr);
                        if (h) { h->constant.value = NULL; h->type = s->void_type; }
                        return h;
                    }
                    probe = probe->if_expr.else_branch;
                }
                if (!unfoldable) {
                    if (probe) return lower_expr(ctx, probe);
                    // All elif conditions were false and no terminal else.
                    struct HirInstr* h = alloc_with_facts(s, HIR_CONST, expr);
                    if (h) { h->constant.value = NULL; h->type = s->void_type; }
                    return h;
                }
                // CONST_INVALID somewhere — fall through to runtime HIR_IF.
            }
            struct HirInstr* h = alloc_with_facts(s, HIR_IF, expr);
            if (!h) return NULL;
            h->if_instr.condition = lower_expr(ctx, expr->if_expr.condition);
            h->if_instr.then_block = lower_block_into(ctx, expr->if_expr.then_branch);
            h->if_instr.else_block = expr->if_expr.else_branch
                ? lower_block_into(ctx, expr->if_expr.else_branch) : NULL;
            // Capture name (`if (opt) |x| then`) — sema fills `.resolved`
            // on the unwrap-bind decl. NULL for plain conditions.
            h->if_instr.capture = expr->if_expr.capture.resolved;
            return h;
        }
        case expr_Switch: {
            // Phase D: comptime switch splicing. Eval the scrutinee at
            // compile time, find the matching arm, lower only that arm.
            // Same fallback as comptime if: CONST_INVALID -> runtime
            // HIR_SWITCH so sema's diagnostic surfaces.
            if (expr->is_comptime && expr->switch_expr.scrutinee &&
                expr->switch_expr.arms) {
                struct EvalResult er = sema_const_eval_expr(s,
                    expr->switch_expr.scrutinee, NULL);
                if (sema_const_value_is_valid(er.value)) {
                    for (size_t i = 0; i < expr->switch_expr.arms->count; i++) {
                        struct SwitchArm* arm = (struct SwitchArm*)vec_get(
                            expr->switch_expr.arms, i);
                        if (!arm || !arm->patterns) continue;
                        for (size_t j = 0; j < arm->patterns->count; j++) {
                            struct Expr** pp = (struct Expr**)vec_get(
                                arm->patterns, j);
                            if (!pp || !*pp) continue;
                            struct EvalResult pe = sema_const_eval_expr(
                                s, *pp, NULL);
                            if (sema_const_value_is_valid(pe.value) &&
                                pe.value.kind == er.value.kind) {
                                bool match = false;
                                switch (er.value.kind) {
                                    case CONST_INT:    match = pe.value.int_val == er.value.int_val; break;
                                    case CONST_BOOL:   match = pe.value.bool_val == er.value.bool_val; break;
                                    case CONST_STRING: match = pe.value.string_id == er.value.string_id; break;
                                    case CONST_TYPE:   match = pe.value.type_val == er.value.type_val; break;
                                    default: break;
                                }
                                if (match && arm->body) {
                                    return lower_expr(ctx, arm->body);
                                }
                            }
                        }
                    }
                }
                // No arm matched (or eval failed): fall through to a
                // runtime HIR_SWITCH so consumers see the full shape.
            }
            struct HirInstr* h = alloc_with_facts(s, HIR_SWITCH, expr);
            if (!h) return NULL;
            h->switch_instr.scrutinee = lower_expr(ctx, expr->switch_expr.scrutinee);
            h->switch_instr.arms = vec_new_in(s->arena, sizeof(struct HirSwitchArm*));
            if (expr->switch_expr.arms) {
                for (size_t i = 0; i < expr->switch_expr.arms->count; i++) {
                    struct SwitchArm* arm = (struct SwitchArm*)vec_get(
                        expr->switch_expr.arms, i);
                    if (!arm) continue;
                    struct HirSwitchArm* harm = arena_alloc(s->arena,
                        sizeof(struct HirSwitchArm));
                    if (!harm) continue;
                    harm->patterns = vec_new_in(s->arena, sizeof(struct HirInstr*));
                    if (arm->patterns) {
                        for (size_t j = 0; j < arm->patterns->count; j++) {
                            struct Expr** pp = (struct Expr**)vec_get(arm->patterns, j);
                            if (!pp || !*pp) continue;
                            struct HirInstr* pat = lower_expr(ctx, *pp);
                            if (pat) vec_push(harm->patterns, &pat);
                        }
                    }
                    harm->body_block = lower_block_into(ctx, arm->body);
                    vec_push(h->switch_instr.arms, &harm);
                }
            }
            return h;
        }
        case expr_Loop: {
            struct HirInstr* h = alloc_with_facts(s, HIR_LOOP, expr);
            if (!h) return NULL;
            h->loop.init = expr->loop_expr.init
                ? lower_expr(ctx, expr->loop_expr.init) : NULL;
            h->loop.condition = expr->loop_expr.condition
                ? lower_expr(ctx, expr->loop_expr.condition) : NULL;
            h->loop.step = expr->loop_expr.step
                ? lower_expr(ctx, expr->loop_expr.step) : NULL;
            h->loop.body_block = lower_block_into(ctx, expr->loop_expr.body);
            h->loop.capture = expr->loop_expr.capture.resolved;
            return h;
        }
        case expr_Return: {
            struct HirInstr* h = alloc_with_facts(s, HIR_RETURN, expr);
            if (!h) return NULL;
            h->return_instr.value = expr->return_expr.value
                ? lower_expr(ctx, expr->return_expr.value) : NULL;
            return h;
        }
        case expr_Break:
            return alloc_with_facts(s, HIR_BREAK, expr);
        case expr_Continue:
            return alloc_with_facts(s, HIR_CONTINUE, expr);
        case expr_Defer: {
            struct HirInstr* h = alloc_with_facts(s, HIR_DEFER, expr);
            if (!h) return NULL;
            h->defer.value = expr->defer_expr.value
                ? lower_expr(ctx, expr->defer_expr.value) : NULL;
            return h;
        }
        case expr_Call: {
            // Phase F: a Call whose callee resolves to a DECL_FIELD
            // owned by a SCOPE_EFFECT scope is an effect op call —
            // emit HIR_OP_PERFORM directly. Phase F.2 deleted the
            // synthetic-decl indirection; bare op names now resolve
            // straight to the source op decl in the effect's
            // child_scope, so the probe is "owner.kind == SCOPE_EFFECT"
            // instead of "kind == DECL_EFFECT_OP".
            //
            // Named-form ops (`x.op()`) stay as HIR_CALL with a
            // HIR_FIELD callee — they're already disambiguated by the
            // field access. Even though the field also resolves to a
            // DECL_FIELD in SCOPE_EFFECT, the callee shape (Field vs
            // Ident) tells us which dispatch model applies.
            struct Decl* callee_decl = ast_resolved_decl_of(expr->call.callee);
            bool is_op_call = callee_decl && callee_decl->kind == DECL_FIELD &&
                callee_decl->owner && callee_decl->owner->kind == SCOPE_EFFECT &&
                expr->call.callee && expr->call.callee->kind == expr_Ident;
            if (is_op_call) {
                // Walk owner→parent to find the effect Decl. Mirror of
                // effect_decl_for_op's DECL_FIELD path.
                struct Scope* eff_scope = callee_decl->owner;
                struct Scope* parent = eff_scope->parent;
                struct Decl* effect_decl = NULL;
                if (parent && parent->decls) {
                    for (size_t i = 0; i < parent->decls->count; i++) {
                        struct Decl** dp = (struct Decl**)vec_get(parent->decls, i);
                        struct Decl* d = dp ? *dp : NULL;
                        if (d && d->child_scope == eff_scope &&
                            d->semantic_kind == SEM_EFFECT) {
                            effect_decl = d;
                            break;
                        }
                    }
                }
                struct HirInstr* h = alloc_with_facts(s, HIR_OP_PERFORM, expr);
                if (!h) return NULL;
                h->op_perform.effect_decl = effect_decl;
                h->op_perform.op_decl = callee_decl;
                h->op_perform.args = vec_new_in(s->arena, sizeof(struct HirInstr*));
                if (expr->call.args) {
                    for (size_t i = 0; i < expr->call.args->count; i++) {
                        struct Expr** ap = (struct Expr**)vec_get(expr->call.args, i);
                        struct Expr* arg = ap ? *ap : NULL;
                        if (!arg) continue;
                        struct HirInstr* a = lower_expr(ctx, arg);
                        if (a) vec_push(h->op_perform.args, &a);
                    }
                }
                return h;
            }
            struct HirInstr* h = alloc_with_facts(s, HIR_CALL, expr);
            if (!h) return NULL;
            h->call.callee = lower_expr(ctx, expr->call.callee);
            h->call.callee_decl = ast_resolved_decl_of(expr->call.callee);
            h->call.args = vec_new_in(s->arena, sizeof(struct HirInstr*));
            // If sema comptime-folded this call, attach the value so the
            // HIR carries the result. Lets dump_tyck (and future codegen)
            // read the folded value off HIR without consulting facts.
            struct SemaFact* fact = sema_fact_of(s, expr);
            if (fact && fact->value.kind != CONST_INVALID) {
                struct ConstValue* v = arena_alloc(s->arena, sizeof(struct ConstValue));
                if (v) { *v = fact->value; h->call.folded_value = v; }
            }
            if (expr->call.args) {
                for (size_t i = 0; i < expr->call.args->count; i++) {
                    struct Expr** ap = (struct Expr**)vec_get(expr->call.args, i);
                    struct Expr* arg = ap ? *ap : NULL;
                    if (!arg) continue;
                    struct HirInstr* a = lower_expr(ctx, arg);
                    if (a) vec_push(h->call.args, &a);
                }
            }
            return h;
        }
        case expr_Lambda: {
            // Anonymous function value. `source` is NULL — there's no
            // Decl this lambda came from. Phase E will inspect callee
            // signatures to lower lambda bodies with the right
            // expected-effect-row in scope; for now we just lower the
            // body straight.
            struct Type* lty = sema_type_of(s, expr);
            struct HirInstr* h = alloc_with_facts(s, HIR_LAMBDA, expr);
            if (!h) return NULL;
            h->lambda.fn = lower_fn_like(s, NULL, expr->lambda.params,
                lty ? lty->ret : NULL,
                expr->lambda.body, expr->span);
            h->lambda.is_ctl = false;
            return h;
        }
        case expr_Ctl: {
            // ctl op (multi-shot continuation form). Same shape as
            // Lambda; the is_ctl flag is what consumers (codegen,
            // effect runtime) discriminate on later.
            struct Type* cty = sema_type_of(s, expr);
            struct HirInstr* h = alloc_with_facts(s, HIR_LAMBDA, expr);
            if (!h) return NULL;
            h->lambda.fn = lower_fn_like(s, NULL, expr->ctl.params,
                cty ? cty->ret : NULL,
                expr->ctl.body, expr->span);
            h->lambda.is_ctl = true;
            return h;
        }
        case expr_Handler: {
            // Value-position handler literal: lowers via the shared
            // payload helper, then transcribes sema's facts onto the
            // resulting instr (the helper sets defaults that we may
            // need to override with sema_type_of's HandlerOf<E,R>).
            struct HirInstr* h = lower_handler_value(ctx, &expr->handler, expr->span);
            if (!h) return error_placeholder(s, expr);
            copy_facts_from(s, h, expr);
            return h;
        }
        case expr_With: {
            // Handler install. caller is HandlerExpr* (parser narrow).
            // body lowers into a fresh sub-block so the install's body
            // is isolated from the surrounding flatten stream.
            struct HirInstr* h = alloc_with_facts(s, HIR_HANDLER_INSTALL, expr);
            if (!h) return NULL;
            h->handler_install.effect_decl = expr->with.caller
                ? expr->with.caller->effect_decl : NULL;
            h->handler_install.handler = lower_handler_value(ctx,
                expr->with.caller, expr->span);
            h->handler_install.binder = expr->with.binder.string_id != 0
                ? expr->with.binder.resolved : NULL;
            h->handler_install.body_block = expr->with.body
                ? lower_block_into(ctx, expr->with.body) : NULL;
            return h;
        }
        case expr_Builtin: {
            struct HirInstr* h = alloc_with_facts(s, HIR_BUILTIN, expr);
            if (!h) return NULL;
            h->builtin.name_id = expr->builtin.name_id;
            h->builtin.args = vec_new_in(s->arena, sizeof(struct HirInstr*));
            if (expr->builtin.args) {
                for (size_t i = 0; i < expr->builtin.args->count; i++) {
                    struct Expr** ap = (struct Expr**)vec_get(expr->builtin.args, i);
                    struct Expr* arg = ap ? *ap : NULL;
                    if (!arg) continue;
                    struct HirInstr* a = lower_expr(ctx, arg);
                    if (a) vec_push(h->builtin.args, &a);
                }
            }
            return h;
        }
        // Type-position kinds in value position (`T :: struct {…}`,
        // `T :: []u8`, etc.) lower to HIR_TYPE_VALUE wrapping the
        // denoted Type. We use sema_infer_type_expr (cached, idempotent
        // via the query system) rather than sema_type_of, because
        // sema_type_of returns "the type of this expression" which for
        // a type-expression is TYPE_TYPE — not what HIR_TYPE_VALUE
        // wants. We want the type the expression denotes.
        case expr_Struct:
        case expr_Enum:
        case expr_Effect:
        case expr_EffectRow:
        case expr_ArrayType:
        case expr_SliceType:
        case expr_ManyPtrType: {
            struct HirInstr* h = alloc_with_facts(s, HIR_TYPE_VALUE, expr);
            if (!h) return NULL;
            struct Type* denoted = sema_infer_type_expr(s, expr);
            h->type_value.type = denoted ? denoted : s->unknown_type;
            // The instruction's *own* type is TYPE_TYPE (it's a type
            // value); leave the copy_facts_from result alone unless
            // sema didn't record one, in which case use s->type_type.
            if (!h->type || h->type == s->unknown_type) h->type = s->type_type;
            h->semantic_kind = SEM_TYPE;
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

// Lower a function-like body (Lambda or Ctl) into a fresh HirFn. Used
// by both lower_decl (top-level fn bindings) and the value-position
// Lambda/Ctl arms in lower_expr (anonymous fns passed as args, etc.).
//
// `source` is the Decl back-pointer when the fn comes from a named
// decl; NULL for inline anonymous lambdas. `params`, `ret_ty`, `body`
// come from either LambdaExpr or CtlExpr — they share the same shape.
static struct HirFn* lower_fn_like(struct Sema* s,
                                    struct Decl* source,
                                    Vec* params,
                                    struct Type* ret_ty,
                                    struct Expr* body,
                                    struct Span span) {
    struct HirFn* fn = hir_fn_new(s->arena, source, span);
    if (!fn) return NULL;
    fn->ret_type = ret_ty;
    if (params) {
        for (size_t i = 0; i < params->count; i++) {
            struct Param* p = (struct Param*)vec_get(params, i);
            if (!p) continue;
            struct HirParam* hp = arena_alloc(s->arena, sizeof(struct HirParam));
            if (!hp) continue;
            hp->decl = p->name.resolved;
            hp->type = hp->decl ? sema_type_of(s, p->type_ann) : NULL;
            hp->is_comptime = (p->kind != PARAM_RUNTIME);
            hp->is_inferred_comptime = (p->kind == PARAM_INFERRED_COMPTIME);
            vec_push(fn->params, &hp);
        }
    }
    if (body) {
        struct LowerCtx ctx = {
            .sema = s,
            .fn = fn,
            .current_block = fn->body_block,
        };
        struct HirInstr* tail = lower_expr(&ctx, body);
        if (tail) vec_push(fn->body_block, &tail);
    }
    return fn;
}

struct HirFn* lower_decl(struct Sema* s, struct Decl* decl) {
    if (!s || !decl_is_function_shaped(decl)) return NULL;
    struct Expr* lambda = decl->node->bind.value;
    struct Type* fn_ty = sema_type_of(s, lambda);
    return lower_fn_like(s, decl, lambda->lambda.params,
        fn_ty ? fn_ty->ret : NULL,
        lambda->lambda.body, decl->name.span);
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
        if (fn) {
            vec_push(hmod->functions, &fn);
            // Populate the per-decl shortcut so consumers
            // (sema_body_effects_of, future codegen) can find a fn's
            // HIR in O(1) without scanning module->functions.
            hashmap_put(&s->decl_hir, (uint64_t)(uintptr_t)d, fn);
        }
    }
    return hmod;
}

struct HirFn* lower_instantiation(struct Sema* s, struct Instantiation* inst) {
    if (!s || !inst || !inst->generic) return NULL;
    if (inst->hir) return inst->hir;
    struct Decl* generic = inst->generic;
    if (!decl_is_function_shaped(generic)) return NULL;
    struct Expr* lambda = generic->node->bind.value;

    // Per-instantiation facts live on inst->body — make it the active
    // lookup target for the duration so sema_type_of / _semantic_of /
    // _region_of return the specialized values rather than the
    // generic's. sema_fact_of's "scan all bodies" fallback would still
    // find the per-instantiation facts since instantiations are added
    // later than the generic body, but setting current_body explicitly
    // matches the current_body-first probe and avoids depending on
    // body-list ordering.
    struct CheckedBody* prev_body = s->current_body;
    if (inst->body) s->current_body = inst->body;

    struct Type* ret = inst->specialized_type
        ? inst->specialized_type->ret : NULL;
    inst->hir = lower_fn_like(s, generic, lambda->lambda.params, ret,
        lambda->lambda.body, generic->name.span);

    s->current_body = prev_body;
    return inst->hir;
}
