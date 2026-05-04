#include "sema.h"

#include <stdarg.h>
#include <stdio.h>

#include "checker.h"
#include "const_eval.h"
#include "decls.h"
#include "effects.h"
#include "evidence.h"
#include "instantiate.h"
#include "sema_internal.h"
#include "const_eval.h"
#include "type.h"
#include "../compiler/compiler.h"
#include "../hir/lower.h"
#include "../hir/hir.h"

// ----- Per-Decl sema cache (see sema_internal.h::SemaDeclInfo) -----

struct SemaDeclInfo* sema_decl_info(struct Sema* s, struct Decl* decl) {
    if (!s || !decl) return NULL;
    struct SemaDeclInfo* info = (struct SemaDeclInfo*)hashmap_get(
        &s->decl_info, (uint64_t)(uintptr_t)decl);
    if (info) return info;
    info = arena_alloc(s->arena, sizeof(struct SemaDeclInfo));
    if (!info) return NULL;
    sema_query_slot_init(&info->type_query, QUERY_TYPE_OF_DECL);
    sema_query_slot_init(&info->effect_sig_query, QUERY_EFFECT_SIG);
    sema_query_slot_init(&info->body_effects_query, QUERY_BODY_EFFECTS);
    info->type = NULL;
    info->effect_sig = NULL;
    info->body_effects = NULL;
    hashmap_put(&s->decl_info, (uint64_t)(uintptr_t)decl, info);
    return info;
}

struct Type* sema_decl_type(struct Sema* s, struct Decl* decl) {
    struct SemaDeclInfo* info = sema_decl_info(s, decl);
    return info ? info->type : NULL;
}

struct EffectSig* sema_decl_effect_sig(struct Sema* s, struct Decl* decl) {
    struct SemaDeclInfo* info = sema_decl_info(s, decl);
    return info ? info->effect_sig : NULL;
}

struct EffectSet* sema_decl_body_effects(struct Sema* s, struct Decl* decl) {
    struct SemaDeclInfo* info = sema_decl_info(s, decl);
    return info ? info->body_effects : NULL;
}

void sema_error(struct Sema* s, struct Span span, const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (s->diags) {
        diag_error(s->diags, span, "%s", msg);
    }
    s->has_errors = true;
}

struct CheckedBody* sema_body_new(struct Sema* s, struct Decl* decl,
    struct Module* module, struct Instantiation* instantiation) {
    if (!s || !s->arena) return NULL;
    struct CheckedBody* body = arena_alloc(s->arena, sizeof(struct CheckedBody));
    if (!body) return NULL;
    body->decl = decl;
    body->module = module;
    body->instantiation = instantiation;
    body->facts = vec_new_in(s->arena, sizeof(struct SemaFact));
    body->entry_evidence = sema_evidence_clone(s, s->current_evidence);
    hashmap_init_in(&body->call_evidence, s->arena);
    if (s->bodies) vec_push(s->bodies, &body);
    return body;
}

struct CheckedBody* sema_enter_body(struct Sema* s, struct CheckedBody* body) {
    if (!s) return NULL;
    struct CheckedBody* prev = s->current_body;
    s->current_body = body;
    return prev;
}

void sema_leave_body(struct Sema* s, struct CheckedBody* previous) {
    if (!s) return;
    s->current_body = previous;
}

// Map an AST ExprKind to its corresponding HirInstrKind. Per-arm
// payload population happens later (H.B.3+); for now the kind tag
// is enough to mark the HirInstr's identity. ExprKinds that don't
// produce a per-instr HIR shape (Block, DestructureBind) lower to
// HIR_ERROR — Block flattens its statements into the surrounding
// stream rather than producing a single instruction.
static HirInstrKind hir_kind_for_expr(struct Expr* expr) {
    if (!expr) return HIR_ERROR;
    switch (expr->kind) {
        case expr_Lit:           return HIR_CONST;
        case expr_Ident:         return HIR_REF;
        case expr_Bin:           return HIR_BIN;
        case expr_Assign:        return HIR_ASSIGN;
        case expr_Unary:         return HIR_UNARY;
        case expr_Call:          return HIR_CALL;
        case expr_Builtin:       return HIR_BUILTIN;
        case expr_If:            return HIR_IF;
        case expr_Switch:        return HIR_SWITCH;
        case expr_Product:       return HIR_PRODUCT;
        case expr_Bind:          return HIR_BIND;
        case expr_Ctl:           return HIR_LAMBDA;  // is_ctl set later
        case expr_Handler:       return HIR_HANDLER_VALUE;
        case expr_With:          return HIR_HANDLER_INSTALL;
        case expr_Field:         return HIR_FIELD;
        case expr_Index:         return HIR_INDEX;
        case expr_Lambda:        return HIR_LAMBDA;
        case expr_Loop:          return HIR_LOOP;
        case expr_EnumRef:       return HIR_ENUM_REF;
        case expr_Asm:           return HIR_ASM;
        case expr_Return:        return HIR_RETURN;
        case expr_Break:         return HIR_BREAK;
        case expr_Continue:      return HIR_CONTINUE;
        case expr_Defer:         return HIR_DEFER;
        case expr_ArrayLit:      return HIR_ARRAY_LIT;
        case expr_Struct:
        case expr_Enum:
        case expr_Effect:
        case expr_EffectRow:
        case expr_ArrayType:
        case expr_SliceType:
        case expr_ManyPtrType:   return HIR_TYPE_VALUE;
        case expr_Block:
        case expr_DestructureBind:
        case expr_Wildcard:      return HIR_ERROR;
    }
    return HIR_ERROR;
}

// Populate the kind-specific payload of a HirInstr from its source
// Expr. Called from body_record_fact when an active lower_ctx exists.
// Per-arm population migrates in H.B.3-B.6 batches; cases not yet
// migrated leave the payload zero-initialized (HIR_ERROR placeholder
// effectively). Sub-expression wire-up (e.g. HIR_BIN.left/right) reads
// pre-allocated HirInstrs from lower_ctx->expr_hir, populated by
// recursion having reached child arms first.
// Look up the HirInstr for a sub-expression. Recursion order
// guarantees the sub-expr's body_record_fact fired before its parent's,
// so the lookup hits. Returns NULL only when the sub-expr is itself
// NULL (legitimately optional in many AST nodes — e.g. else-less if).
static struct HirInstr* lookup_hir(struct Sema* s, struct Expr* sub) {
    if (!s || !s->lower_ctx || !sub) return NULL;
    return (struct HirInstr*)hashmap_get(
        &s->lower_ctx->expr_hir, (uint64_t)(uintptr_t)sub);
}

// Build a Vec<HirInstr*> for a block-shaped sub-expression. The "block"
// at HIR level is just a sequence of instructions; AST nodes that
// participate in such a sequence are either:
//   - expr_Block: iterate stmts, look up each
//   - other kinds: single-element vec with the expr's own HirInstr
//
// Sema visited every stmt and populated the map; this helper just
// translates AST order → HIR vec. Returns NULL for NULL input (caller
// uses for optional carriers like else-less if).
static Vec* hir_instrs_for_block_expr(struct Sema* s, struct Expr* expr) {
    if (!s || !s->lower_ctx || !expr) return NULL;
    Vec* block = vec_new_in(s->arena, sizeof(struct HirInstr*));
    if (expr->kind == expr_Block) {
        if (expr->block.stmts) {
            for (size_t i = 0; i < expr->block.stmts->count; i++) {
                struct Expr** ep = (struct Expr**)vec_get(expr->block.stmts, i);
                struct HirInstr* h = lookup_hir(s, ep ? *ep : NULL);
                if (h) vec_push(block, &h);
            }
        }
    } else {
        struct HirInstr* h = lookup_hir(s, expr);
        if (h) vec_push(block, &h);
    }
    return block;
}

static void populate_hir_payload(struct Sema* s, struct Expr* expr,
                                  struct HirInstr* h) {
    if (!s || !expr || !h) return;
    switch (expr->kind) {
        // H.B.3 — leaf kinds.
        case expr_Lit:
            h->constant.value = NULL;
            return;
        case expr_Ident:
            h->ref.decl = expr->ident.resolved;
            return;
        case expr_Asm:
            h->asm_instr.string_id = expr->asm_expr.string_id;
            return;
        case expr_Wildcard:
            return;
        // H.B.4 — structural kinds. Sub-expressions' HirInstrs are
        // already in the map (recursion visited them first).
        case expr_Bin:
            h->bin.op    = (HirBinOp)expr->bin.op;
            h->bin.left  = lookup_hir(s, expr->bin.Left);
            h->bin.right = lookup_hir(s, expr->bin.Right);
            return;
        case expr_Unary:
            h->unary.op      = expr->unary.op;
            h->unary.postfix = expr->unary.postfix;
            h->unary.operand = lookup_hir(s, expr->unary.operand);
            return;
        case expr_Assign:
            h->assign.target = lookup_hir(s, expr->assign.target);
            h->assign.value  = lookup_hir(s, expr->assign.value);
            return;
        case expr_Field:
            h->field.object        = lookup_hir(s, expr->field.object);
            h->field.field_decl    = expr->field.field.resolved;
            h->field.field_name_id = expr->field.field.string_id;
            return;
        case expr_Index:
            h->index.object = lookup_hir(s, expr->index.object);
            h->index.index  = lookup_hir(s, expr->index.index);
            return;
        // H.B.5 — aggregates.
        case expr_Product:
            h->product.type_hint = h->type;
            h->product.fields = vec_new_in(s->arena, sizeof(struct HirInstr*));
            if (expr->product.Fields) {
                for (size_t i = 0; i < expr->product.Fields->count; i++) {
                    struct ProductField* f = (struct ProductField*)
                        vec_get(expr->product.Fields, i);
                    if (!f || !f->value) continue;
                    struct HirInstr* v = lookup_hir(s, f->value);
                    if (v) vec_push(h->product.fields, &v);
                }
            }
            return;
        case expr_ArrayLit:
            h->array_lit.size = lookup_hir(s, expr->array_lit.size);
            h->array_lit.elem_type = expr->array_lit.elem_type
                ? sema_infer_type_expr(s, expr->array_lit.elem_type) : NULL;
            h->array_lit.initializer = lookup_hir(s, expr->array_lit.initializer);
            return;
        case expr_EnumRef:
            h->enum_ref.variant_decl    = expr->enum_ref_expr.name.resolved;
            h->enum_ref.variant_name_id = expr->enum_ref_expr.name.string_id;
            return;
        // H.B.5 / H.B.7.a — control flow. Block-shaped sub-fields use
        // hir_instrs_for_block_expr to translate AST stmt order into
        // a HirInstr vec; immediate-child instrs (condition, scrutinee,
        // init/cond/step, capture) come from lookup_hir.
        case expr_If:
            h->if_instr.condition = lookup_hir(s, expr->if_expr.condition);
            h->if_instr.capture   = expr->if_expr.capture.resolved;
            // Comptime-if dissolution: if `comptime if` evaluated to a
            // bool, emit only the live branch into then_block; leave
            // else_block NULL. Mirrors lower.c's splice; the dead
            // branch's HirInstrs orphan in the map (cost: memory only).
            if (expr->is_comptime && expr->if_expr.condition) {
                struct EvalResult er = sema_const_eval_expr(s,
                    expr->if_expr.condition, NULL);
                if (er.value.kind == CONST_BOOL) {
                    struct Expr* live = er.value.bool_val
                        ? expr->if_expr.then_branch
                        : expr->if_expr.else_branch;
                    h->if_instr.then_block = hir_instrs_for_block_expr(s, live);
                    h->if_instr.else_block = NULL;
                    return;
                }
                // CONST_INVALID — fall through to runtime shape.
            }
            h->if_instr.then_block = hir_instrs_for_block_expr(s, expr->if_expr.then_branch);
            h->if_instr.else_block = expr->if_expr.else_branch
                ? hir_instrs_for_block_expr(s, expr->if_expr.else_branch) : NULL;
            return;
        case expr_Switch: {
            h->switch_instr.scrutinee = lookup_hir(s, expr->switch_expr.scrutinee);
            // Comptime-switch dissolution: pick the matching arm at
            // compile time, emit only that arm. Mirrors lower.c.
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
                            struct Expr** pp = (struct Expr**)vec_get(arm->patterns, j);
                            if (!pp || !*pp) continue;
                            struct EvalResult pe = sema_const_eval_expr(s, *pp, NULL);
                            if (!sema_const_value_is_valid(pe.value)) continue;
                            if (pe.value.kind != er.value.kind) continue;
                            bool match = false;
                            switch (er.value.kind) {
                                case CONST_INT:    match = pe.value.int_val == er.value.int_val; break;
                                case CONST_BOOL:   match = pe.value.bool_val == er.value.bool_val; break;
                                case CONST_STRING: match = pe.value.string_id == er.value.string_id; break;
                                case CONST_TYPE:   match = pe.value.type_val == er.value.type_val; break;
                                default: break;
                            }
                            if (match && arm->body) {
                                // Emit a synthetic single-arm switch with
                                // only the matching arm. Pattern vec is
                                // empty (sema-time match already happened).
                                h->switch_instr.arms = vec_new_in(s->arena, sizeof(struct HirSwitchArm*));
                                struct HirSwitchArm* harm = arena_alloc(s->arena, sizeof(struct HirSwitchArm));
                                if (harm) {
                                    harm->patterns = vec_new_in(s->arena, sizeof(struct HirInstr*));
                                    harm->body_block = hir_instrs_for_block_expr(s, arm->body);
                                    vec_push(h->switch_instr.arms, &harm);
                                }
                                return;
                            }
                        }
                    }
                }
                // Fall through to runtime shape if eval failed or no match.
            }
            h->switch_instr.arms = vec_new_in(s->arena, sizeof(struct HirSwitchArm*));
            if (expr->switch_expr.arms) {
                for (size_t i = 0; i < expr->switch_expr.arms->count; i++) {
                    struct SwitchArm* arm = (struct SwitchArm*)vec_get(
                        expr->switch_expr.arms, i);
                    if (!arm) continue;
                    struct HirSwitchArm* harm = arena_alloc(s->arena, sizeof(struct HirSwitchArm));
                    if (!harm) continue;
                    harm->patterns = vec_new_in(s->arena, sizeof(struct HirInstr*));
                    if (arm->patterns) {
                        for (size_t j = 0; j < arm->patterns->count; j++) {
                            struct Expr** pp = (struct Expr**)vec_get(arm->patterns, j);
                            struct HirInstr* p = lookup_hir(s, pp ? *pp : NULL);
                            if (p) vec_push(harm->patterns, &p);
                        }
                    }
                    harm->body_block = hir_instrs_for_block_expr(s, arm->body);
                    vec_push(h->switch_instr.arms, &harm);
                }
            }
            return;
        }
        case expr_Loop:
            h->loop.init      = lookup_hir(s, expr->loop_expr.init);
            h->loop.condition = lookup_hir(s, expr->loop_expr.condition);
            h->loop.step      = lookup_hir(s, expr->loop_expr.step);
            h->loop.capture   = expr->loop_expr.capture.resolved;
            h->loop.body_block = hir_instrs_for_block_expr(s, expr->loop_expr.body);
            return;
        case expr_Return:
            h->return_instr.value = lookup_hir(s, expr->return_expr.value);
            return;
        case expr_Break:
        case expr_Continue:
            return;  // no payload
        case expr_Defer:
            h->defer.value = lookup_hir(s, expr->defer_expr.value);
            return;
        // H.B.6 — Bind / Call / Builtin / type-position kinds. Lambda
        // / Ctl / Handler / With still need block-flattening machinery
        // (sub-block construction during sema walk) — those migrate
        // in H.B.7 when sema gains LowerCtx.current_block discipline.
        case expr_Bind:
            h->bind.decl = expr->bind.name.resolved;
            h->bind.init = lookup_hir(s, expr->bind.value);
            return;
        case expr_Call: {
            // Op-call detection: a Call whose resolved callee is a
            // DECL_FIELD owned by a SCOPE_EFFECT scope is an effect op
            // perform (HIR_OP_PERFORM), not a regular call. The default
            // kind from hir_kind_for_expr was HIR_CALL — re-tag here.
            struct Decl* callee_decl = ast_resolved_decl_of(expr->call.callee);
            bool is_op_call = callee_decl && callee_decl->kind == DECL_FIELD &&
                callee_decl->owner && callee_decl->owner->kind == SCOPE_EFFECT &&
                expr->call.callee && expr->call.callee->kind == expr_Ident;
            if (is_op_call) {
                // Walk owner→parent to find the effect Decl.
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
                h->kind = HIR_OP_PERFORM;
                h->op_perform.effect_decl = effect_decl;
                h->op_perform.op_decl = callee_decl;
                h->op_perform.args = vec_new_in(s->arena, sizeof(struct HirInstr*));
                if (expr->call.args) {
                    for (size_t i = 0; i < expr->call.args->count; i++) {
                        struct Expr** ap = (struct Expr**)vec_get(expr->call.args, i);
                        struct HirInstr* a = lookup_hir(s, ap ? *ap : NULL);
                        if (a) vec_push(h->op_perform.args, &a);
                    }
                }
                return;
            }
            h->call.callee = lookup_hir(s, expr->call.callee);
            h->call.callee_decl = callee_decl;
            h->call.args = vec_new_in(s->arena, sizeof(struct HirInstr*));
            // folded_value: if sema already attached a comptime fold
            // to this call's fact, mirror it onto the HIR. The fact
            // value is set by sema_record_call_value, which fires
            // AFTER body_record_fact, so the value isn't available
            // here yet — sema_record_call_value patches the HirInstr
            // separately (TODO: wire that path in H.B.7).
            h->call.folded_value = NULL;
            if (expr->call.args) {
                for (size_t i = 0; i < expr->call.args->count; i++) {
                    struct Expr** ap = (struct Expr**)vec_get(expr->call.args, i);
                    struct HirInstr* a = lookup_hir(s, ap ? *ap : NULL);
                    if (a) vec_push(h->call.args, &a);
                }
            }
            return;
        }
        case expr_Builtin:
            h->builtin.name_id = expr->builtin.name_id;
            h->builtin.args = vec_new_in(s->arena, sizeof(struct HirInstr*));
            if (expr->builtin.args) {
                for (size_t i = 0; i < expr->builtin.args->count; i++) {
                    struct Expr** ap = (struct Expr**)vec_get(expr->builtin.args, i);
                    struct HirInstr* a = lookup_hir(s, ap ? *ap : NULL);
                    if (a) vec_push(h->builtin.args, &a);
                }
            }
            return;
        case expr_Struct:
        case expr_Enum:
        case expr_Effect:
        case expr_EffectRow:
        case expr_ArrayType:
        case expr_SliceType:
        case expr_ManyPtrType: {
            // HIR_TYPE_VALUE.type is the denoted type, not h->type
            // (which is TYPE_TYPE for type expressions). sema_infer_type_expr
            // is cached/idempotent.
            struct Type* denoted = sema_infer_type_expr(s, expr);
            h->type_value.type = denoted ? denoted : s->unknown_type;
            return;
        }
        // H.B.7+: Block, Bind (already done), Lambda, Ctl, Handler, With.
        default:
            return;
    }
}

void body_record_fact(struct Sema* s, struct CheckedBody* body, struct Expr* expr,
    struct Type* type, SemanticKind semantic_kind, uint32_t region_id) {
    if (!s || !body || !body->facts || !expr) return;
    struct SemaFact fact = {
        .expr = expr,
        .type = type ? type : s->unknown_type,
        .semantic_kind = semantic_kind,
        .region_id = region_id,
    };
    vec_push(body->facts, &fact);
    // H.B.2: when sema runs under a lowering context, also allocate
    // a HirInstr for this expression and stash type/semantic/region
    // on it. H.B.3+: populate the kind-specific payload via
    // populate_hir_payload. Multiple record_fact calls for the same
    // expr (rare — happens in sema_check_expr's special-case branches)
    // reuse the existing HirInstr instead of overwriting.
    if (s->lower_ctx) {
        struct HirInstr* h = (struct HirInstr*)hashmap_get(
            &s->lower_ctx->expr_hir, (uint64_t)(uintptr_t)expr);
        if (!h) {
            h = sema_emit_hir_instr(s, expr, hir_kind_for_expr(expr));
        }
        if (h) {
            h->type = type ? type : s->unknown_type;
            h->semantic_kind = semantic_kind;
            h->region_id = region_id;
            populate_hir_payload(s, expr, h);
        }
    }
}

void sema_record_fact(struct Sema* s, struct Expr* expr, struct Type* type,
    SemanticKind semantic_kind, uint32_t region_id) {
    if (!s || !expr) return;
    if (!s->current_body) {
        // Surface the bug instead of silently dropping. Once per process is
        // enough — the same site usually fires repeatedly otherwise.
        static bool warned = false;
        if (!warned) {
            fprintf(stderr,
                "warning: sema_record_fact called with no current_body "
                "(line %d); fact discarded\n", expr->span.line);
            warned = true;
        }
        return;
    }
    body_record_fact(s, s->current_body, expr, type, semantic_kind, region_id);
}

static struct SemaFact* find_fact_in_body(struct CheckedBody* body, struct Expr* expr) {
    if (!body || !body->facts) return NULL;
    for (size_t i = body->facts->count; i > 0; i--) {
        struct SemaFact* fact = (struct SemaFact*)vec_get(body->facts, i - 1);
        if (fact && fact->expr == expr) return fact;
    }
    return NULL;
}

// TODO(perf): on miss, this walks every CheckedBody linearly. Fine while the
// only consumers are dump/diagnostic readers. When codegen starts looking up
// facts at scale, add a Sema-side `Expr* -> CheckedBody*` reverse index that
// each body update keeps current.
struct SemaFact* sema_fact_of(struct Sema* s, struct Expr* expr) {
    if (!s || !expr) return NULL;
    struct SemaFact* hit = find_fact_in_body(s->current_body, expr);
    if (hit) return hit;
    if (s->bodies) {
        for (size_t i = s->bodies->count; i > 0; i--) {
            struct CheckedBody** body_p = (struct CheckedBody**)vec_get(s->bodies, i - 1);
            struct CheckedBody* body = body_p ? *body_p : NULL;
            if (body == s->current_body) continue;
            struct SemaFact* found = find_fact_in_body(body, expr);
            if (found) return found;
        }
    }
    return NULL;
}

struct Type* sema_type_of(struct Sema* s, struct Expr* expr) {
    struct SemaFact* fact = sema_fact_of(s, expr);
    return fact ? fact->type : NULL;
}

SemanticKind sema_semantic_of(struct Sema* s, struct Expr* expr) {
    struct SemaFact* fact = sema_fact_of(s, expr);
    return fact ? fact->semantic_kind : SEM_UNKNOWN;
}

uint32_t sema_region_of(struct Sema* s, struct Expr* expr) {
    struct SemaFact* fact = sema_fact_of(s, expr);
    return fact ? fact->region_id : 0;
}

struct EffectSig* sema_effect_sig_of(struct Sema* s, struct Expr* expr) {
    struct Type* type = sema_type_of(s, expr);
    if (type && type->effect_sig) return type->effect_sig;
    if (!s || !expr) return NULL;
    return (struct EffectSig*)hashmap_get(&s->effect_sig_cache,
        (uint64_t)(uintptr_t)expr);
}

struct Sema sema_new(struct Compiler* compiler, struct Resolver* resolver) {
    struct Sema s = {0};
    if (!compiler) return s;

    s.compiler = compiler;
    s.arena = &compiler->arena;
    s.pool = &compiler->pool;
    s.resolver = resolver;
    s.diags = &compiler->diags;
    s.bodies = vec_new_in(&compiler->arena, sizeof(struct CheckedBody*));
    s.current_body = NULL;
    s.instantiations = vec_new_in(&compiler->arena, sizeof(struct Instantiation*));
    hashmap_init_in(&s.instantiation_buckets, &compiler->arena);
    hashmap_init_in(&s.decl_info, &compiler->arena);
    s.current_env = NULL;
    s.current_evidence = sema_evidence_new(&s);
    s.lower_ctx = NULL;
    hashmap_init_in(&s.effect_sig_cache, &compiler->arena);
    hashmap_init_in(&s.module_hir, &compiler->arena);
    hashmap_init_in(&s.decl_hir, &compiler->arena);
    s.query_stack = vec_new_in(&compiler->arena, sizeof(struct QueryFrame));
    s.comptime_call_depth = 0;
    hashmap_init_in(&s.call_cache, &compiler->arena);
    s.comptime_body_evals = 0;
    s.has_errors = false;

    s.unknown_type = sema_type_new(&s, TYPE_UNKNOWN);
    s.error_type = sema_type_new(&s, TYPE_ERROR);
    s.void_type = sema_type_new(&s, TYPE_VOID);
    s.noreturn_type = sema_type_new(&s, TYPE_NORETURN);
    s.bool_type = sema_type_new(&s, TYPE_BOOL);
    s.comptime_int_type = sema_type_new(&s, TYPE_COMPTIME_INT);
    s.comptime_float_type = sema_type_new(&s, TYPE_COMPTIME_FLOAT);
    s.u8_type = sema_type_new(&s, TYPE_U8);
    s.const_u8_type = sema_const_qualified_type(&s, s.u8_type);
    s.u16_type = sema_type_new(&s, TYPE_U16);
    s.u32_type = sema_type_new(&s, TYPE_U32);
    s.u64_type = sema_type_new(&s, TYPE_U64);
    s.usize_type = sema_type_new(&s, TYPE_USIZE);
    s.i8_type = sema_type_new(&s, TYPE_I8);
    s.i16_type = sema_type_new(&s, TYPE_I16);
    s.i32_type = sema_type_new(&s, TYPE_I32);
    s.i64_type = sema_type_new(&s, TYPE_I64);
    s.isize_type = sema_type_new(&s, TYPE_ISIZE);
    s.f32_type = sema_type_new(&s, TYPE_F32);
    s.f64_type = sema_type_new(&s, TYPE_F64);
    s.string_type = sema_type_new(&s, TYPE_STRING);
    s.nil_type = sema_type_new(&s, TYPE_NIL);
    s.type_type = sema_type_new(&s, TYPE_TYPE);
    s.anytype_type = sema_type_new(&s, TYPE_ANYTYPE);
    s.module_type = sema_type_new(&s, TYPE_MODULE);
    s.effect_type = sema_type_new(&s, TYPE_EFFECT);
    s.effect_row_type = sema_type_new(&s, TYPE_EFFECT_ROW);
    s.scope_token_type = sema_type_new(&s, TYPE_SCOPE_TOKEN);

    // Pre-intern hot-path name IDs (see sema.h for rationale).
    s.name_import  = pool_intern(s.pool, "import",  6);
    s.name_sizeOf  = pool_intern(s.pool, "sizeOf",  6);
    s.name_alignOf = pool_intern(s.pool, "alignOf", 7);
    s.name_intCast = pool_intern(s.pool, "intCast", 7);
    s.name_TypeOf  = pool_intern(s.pool, "TypeOf",  6);
    s.name_target     = pool_intern(s.pool, "target",     6);
    s.name_true       = pool_intern(s.pool, "true",       4);
    s.name_false      = pool_intern(s.pool, "false",      5);
    s.name_returnType = pool_intern(s.pool, "returnType", 10);

    // Build the primitive-name → Type* table once. Mirrors the
    // resolver's `register_primitives` list plus the comptime numerics
    // that aren't user-facing identifiers.
    hashmap_init_in(&s.primitive_types, &compiler->arena);
    #define ORE_REG_PRIM(NAME, TYPE_PTR) \
        hashmap_put(&s.primitive_types, \
            (uint64_t)pool_intern(s.pool, NAME, sizeof(NAME) - 1), \
            (TYPE_PTR))
    ORE_REG_PRIM("void",           s.void_type);
    ORE_REG_PRIM("noreturn",       s.noreturn_type);
    ORE_REG_PRIM("bool",           s.bool_type);
    ORE_REG_PRIM("type",           s.type_type);
    ORE_REG_PRIM("anytype",        s.anytype_type);
    ORE_REG_PRIM("Scope",          s.type_type);
    ORE_REG_PRIM("nil",            s.nil_type);
    ORE_REG_PRIM("u8",             s.u8_type);
    ORE_REG_PRIM("u16",            s.u16_type);
    ORE_REG_PRIM("u32",            s.u32_type);
    ORE_REG_PRIM("u64",            s.u64_type);
    ORE_REG_PRIM("usize",          s.usize_type);
    ORE_REG_PRIM("i8",             s.i8_type);
    ORE_REG_PRIM("i16",            s.i16_type);
    ORE_REG_PRIM("i32",            s.i32_type);
    ORE_REG_PRIM("i64",            s.i64_type);
    ORE_REG_PRIM("isize",          s.isize_type);
    ORE_REG_PRIM("f32",            s.f32_type);
    ORE_REG_PRIM("f64",            s.f64_type);
    ORE_REG_PRIM("comptime_int",   s.comptime_int_type);
    ORE_REG_PRIM("comptime_float", s.comptime_float_type);
    // `true` / `false` are values typed as bool; the resolver classifies
    // them, but `sema_primitive_type_for_name` historically returned bool
    // for either name so we preserve that.
    hashmap_put(&s.primitive_types, (uint64_t)s.name_true,  s.bool_type);
    hashmap_put(&s.primitive_types, (uint64_t)s.name_false, s.bool_type);
    #undef ORE_REG_PRIM
    return s;
}

void sema_record_call_value(struct Sema* s, struct Expr* call_expr, struct ConstValue v) {
    struct SemaFact* fact = sema_fact_of(s, call_expr);
    if (!fact) return;
    fact->value = v;
}

bool sema_check(struct Sema* s) {
    if (!s || !s->resolver) return false;

    // Signature resolution can produce facts (typechecking inside type
    // annotations, default values, etc.) before per-module bodies exist.
    // We give it a dedicated scratch body so:
    //   - sema_record_fact's no-current-body warning stays meaningful (it
    //     only fires for genuine bugs, not for signature work);
    //   - facts produced during sig resolution survive in case future
    //     analyses (cross-decl type queries) want them;
    //   - the body shows up in --dump-evidence as <sig-resolution-scratch>
    //     so it's visibly distinct from real per-decl/per-module bodies.
    // It has decl=NULL, module=NULL, instantiation=NULL — that triple is
    // the tell.
    struct CheckedBody* sig_body = sema_body_new(s, NULL, NULL, NULL);
    struct CheckedBody* prev = sema_enter_body(s, sig_body);

    bool ok_decls = sema_collect_declarations(s);

    sema_leave_body(s, prev);

    if (!ok_decls) return false;
    if (!sema_check_expressions(s)) return false;

    return !s->has_errors;
}

// ----- HIR walking for dump consumers (Phase G.4) -----
//
// `--dump-tyck` and `--dump-sema` previously aggregated stats over
// `body->facts`. Now they walk every HirInstr across all module HIR
// and per-instantiation HIR. The data they need (type, semantic_kind,
// folded call values) is on the HirInstr directly — no per-Expr fact
// lookup required.

typedef void (*HirInstrVisitor)(struct HirInstr* h, void* user);

static void walk_hir_block(Vec* block, HirInstrVisitor fn, void* user);

static void walk_hir_instr(struct HirInstr* h, HirInstrVisitor fn, void* user) {
    if (!h) return;
    fn(h, user);
    switch (h->kind) {
        case HIR_BIN:
            walk_hir_instr(h->bin.left, fn, user);
            walk_hir_instr(h->bin.right, fn, user);
            break;
        case HIR_UNARY:
            walk_hir_instr(h->unary.operand, fn, user);
            break;
        case HIR_ASSIGN:
            walk_hir_instr(h->assign.target, fn, user);
            walk_hir_instr(h->assign.value, fn, user);
            break;
        case HIR_FIELD:
            walk_hir_instr(h->field.object, fn, user);
            break;
        case HIR_INDEX:
            walk_hir_instr(h->index.object, fn, user);
            walk_hir_instr(h->index.index, fn, user);
            break;
        case HIR_IF:
            walk_hir_instr(h->if_instr.condition, fn, user);
            walk_hir_block(h->if_instr.then_block, fn, user);
            walk_hir_block(h->if_instr.else_block, fn, user);
            break;
        case HIR_LOOP:
            walk_hir_instr(h->loop.init, fn, user);
            walk_hir_instr(h->loop.condition, fn, user);
            walk_hir_instr(h->loop.step, fn, user);
            walk_hir_block(h->loop.body_block, fn, user);
            break;
        case HIR_SWITCH:
            walk_hir_instr(h->switch_instr.scrutinee, fn, user);
            if (h->switch_instr.arms) {
                for (size_t i = 0; i < h->switch_instr.arms->count; i++) {
                    struct HirSwitchArm** ap = (struct HirSwitchArm**)
                        vec_get(h->switch_instr.arms, i);
                    if (!ap || !*ap) continue;
                    if ((*ap)->patterns) {
                        for (size_t j = 0; j < (*ap)->patterns->count; j++) {
                            struct HirInstr** pp = (struct HirInstr**)
                                vec_get((*ap)->patterns, j);
                            if (pp && *pp) walk_hir_instr(*pp, fn, user);
                        }
                    }
                    walk_hir_block((*ap)->body_block, fn, user);
                }
            }
            break;
        case HIR_BIND:
            walk_hir_instr(h->bind.init, fn, user);
            break;
        case HIR_RETURN:
            walk_hir_instr(h->return_instr.value, fn, user);
            break;
        case HIR_DEFER:
            walk_hir_instr(h->defer.value, fn, user);
            break;
        case HIR_CALL:
            walk_hir_instr(h->call.callee, fn, user);
            if (h->call.args) {
                for (size_t i = 0; i < h->call.args->count; i++) {
                    struct HirInstr** ap = (struct HirInstr**)vec_get(h->call.args, i);
                    if (ap && *ap) walk_hir_instr(*ap, fn, user);
                }
            }
            break;
        case HIR_OP_PERFORM:
            if (h->op_perform.args) {
                for (size_t i = 0; i < h->op_perform.args->count; i++) {
                    struct HirInstr** ap = (struct HirInstr**)
                        vec_get(h->op_perform.args, i);
                    if (ap && *ap) walk_hir_instr(*ap, fn, user);
                }
            }
            break;
        case HIR_HANDLER_INSTALL:
            walk_hir_instr(h->handler_install.handler, fn, user);
            walk_hir_block(h->handler_install.body_block, fn, user);
            break;
        case HIR_HANDLER_VALUE:
            if (h->handler_value.operations) {
                for (size_t i = 0; i < h->handler_value.operations->count; i++) {
                    struct HirHandlerOp** opp = (struct HirHandlerOp**)
                        vec_get(h->handler_value.operations, i);
                    if (opp && *opp) walk_hir_block((*opp)->body_block, fn, user);
                }
            }
            walk_hir_block(h->handler_value.initially_block, fn, user);
            walk_hir_block(h->handler_value.finally_block, fn, user);
            walk_hir_block(h->handler_value.return_block, fn, user);
            break;
        case HIR_LAMBDA:
            if (h->lambda.fn) walk_hir_block(h->lambda.fn->body_block, fn, user);
            break;
        case HIR_PRODUCT:
            if (h->product.fields) {
                for (size_t i = 0; i < h->product.fields->count; i++) {
                    struct HirInstr** ap = (struct HirInstr**)
                        vec_get(h->product.fields, i);
                    if (ap && *ap) walk_hir_instr(*ap, fn, user);
                }
            }
            break;
        case HIR_ARRAY_LIT:
            walk_hir_instr(h->array_lit.size, fn, user);
            walk_hir_instr(h->array_lit.initializer, fn, user);
            break;
        case HIR_BUILTIN:
            if (h->builtin.args) {
                for (size_t i = 0; i < h->builtin.args->count; i++) {
                    struct HirInstr** ap = (struct HirInstr**)
                        vec_get(h->builtin.args, i);
                    if (ap && *ap) walk_hir_instr(*ap, fn, user);
                }
            }
            break;
        // Pure leaves and type-only kinds — no recursion.
        case HIR_CONST:
        case HIR_REF:
        case HIR_BREAK:
        case HIR_CONTINUE:
        case HIR_TYPE_VALUE:
        case HIR_ENUM_REF:
        case HIR_ASM:
        case HIR_ERROR:
            break;
    }
}

static void walk_hir_block(Vec* block, HirInstrVisitor fn, void* user) {
    if (!block) return;
    for (size_t i = 0; i < block->count; i++) {
        struct HirInstr** ip = (struct HirInstr**)vec_get(block, i);
        if (ip && *ip) walk_hir_instr(*ip, fn, user);
    }
}

// Visit every HirInstr in every module's HIR plus every per-instantiation HIR.
static void walk_all_hir(struct Sema* s, HirInstrVisitor fn, void* user) {
    if (!s || !s->compiler || !s->compiler->modules) return;
    Vec* modules = s->compiler->modules;
    for (size_t i = 0; i < modules->count; i++) {
        struct Module** mp = (struct Module**)vec_get(modules, i);
        struct Module* mod = mp ? *mp : NULL;
        if (!mod) continue;
        struct HirModule* hmod = (struct HirModule*)hashmap_get(
            &s->module_hir, (uint64_t)(uintptr_t)mod);
        if (!hmod || !hmod->functions) continue;
        for (size_t j = 0; j < hmod->functions->count; j++) {
            struct HirFn** fp = (struct HirFn**)vec_get(hmod->functions, j);
            if (fp && *fp) walk_hir_block((*fp)->body_block, fn, user);
        }
    }
    if (s->instantiations) {
        for (size_t i = 0; i < s->instantiations->count; i++) {
            struct Instantiation** ip = (struct Instantiation**)
                vec_get(s->instantiations, i);
            struct Instantiation* inst = ip ? *ip : NULL;
            if (inst && inst->hir) walk_hir_block(inst->hir->body_block, fn, user);
        }
    }
}

static void hir_count_visitor(struct HirInstr* h, void* user) {
    (void)h;
    (*(size_t*)user)++;
}

static size_t total_hir_instr_count(struct Sema* s) {
    size_t n = 0;
    walk_all_hir(s, hir_count_visitor, &n);
    return n;
}

struct HirKindHistCtx {
    size_t* counts;
};

static void hir_kind_visitor(struct HirInstr* h, void* user) {
    struct HirKindHistCtx* ctx = (struct HirKindHistCtx*)user;
    if (h && h->type && h->type->kind <= TYPE_PRODUCT) {
        ctx->counts[h->type->kind]++;
    }
}

static void tally_hir_by_kind(struct Sema* s, size_t counts[TYPE_PRODUCT + 1]) {
    struct HirKindHistCtx ctx = { .counts = counts };
    walk_all_hir(s, hir_kind_visitor, &ctx);
}

struct HirSampleCtx {
    struct Sema* s;
    size_t shown;
    size_t limit;
};

static void hir_sample_visitor(struct HirInstr* h, void* user) {
    struct HirSampleCtx* ctx = (struct HirSampleCtx*)user;
    if (!h || !h->type || ctx->shown >= ctx->limit) return;
    printf("    line %d col %d: %s -> %s",
        h->span.line, h->span.column,
        sema_semantic_kind_str(h->semantic_kind),
        sema_type_kind_str(h->type->kind));
    if (h->region_id) printf(" @region#%u", h->region_id);
    printf("\n");
    ctx->shown++;
}

struct HirFoldedCallCtx {
    struct Sema* s;
    bool printed_header;
};

static void hir_folded_call_visitor(struct HirInstr* h, void* user) {
    if (!h || h->kind != HIR_CALL || !h->call.folded_value) return;
    if (h->call.folded_value->kind == CONST_INVALID) return;
    struct HirFoldedCallCtx* ctx = (struct HirFoldedCallCtx*)user;
    if (!ctx->printed_header) {
        printf("  folded calls:\n");
        ctx->printed_header = true;
    }
    printf("    line %d: ", h->span.line);
    sema_print_const_value(*h->call.folded_value, ctx->s);
    printf("\n");
}

// ----- hashmap-foreach visitors for the dump functions -----

struct EffectSigDumpCtx {
    struct Sema* s;
    size_t shown;
    size_t limit;       // 0 = unlimited
    const char* prefix; // line prefix (indentation)
};

static bool dump_effect_sig_visitor(uint64_t key, void* value, void* user) {
    (void)key;
    struct EffectSigDumpCtx* c = user;
    if (c->limit && c->shown >= c->limit) return false;
    struct EffectSig* sig = (struct EffectSig*)value;
    if (!sig) return true;
    printf("%sline %d col %d: ", c->prefix ? c->prefix : "",
        sig->source ? sig->source->span.line : 0,
        sig->source ? sig->source->span.column : 0);
    sema_print_effect_sig(c->s, sig);
    if (sig->row_decl && sig->row_decl->semantic_kind == SEM_EFFECT_ROW) {
        const char* row_name = pool_get(c->s->pool, sig->row_name_id, 0);
        printf("  open-row=%s", row_name ? row_name : "?");
    }
    printf("\n");
    c->shown++;
    return true;
}

struct CallEvidenceDumpCtx {
    struct Sema* s;
    const char* prefix;
};

static void print_evidence_vector(struct Sema* s, struct EvidenceVector* ev,
    const char* prefix);

static bool dump_call_evidence_visitor(uint64_t key, void* value, void* user) {
    struct CallEvidenceDumpCtx* c = user;
    struct Expr* call = (struct Expr*)(uintptr_t)key;
    struct EvidenceVector* ev = (struct EvidenceVector*)value;
    printf("      call @ line %d col %d:\n",
        call ? call->span.line : 0,
        call ? call->span.column : 0);
    print_evidence_vector(c->s, ev, "        ");
    return true;
}

// H.B.2 helper: when sema is type-checking under an active lowering
// context, allocate a HirInstr for `expr` and stash it in the ctx's
// per-Expr map so later sub-expression wire-up can find it. Returns
// NULL when no ctx is active (today's pure-fact-recording mode), in
// which case sema arms proceed as before. Type / semantic_kind /
// region_id / payload are populated by the caller after the type is
// computed.
struct HirInstr* sema_emit_hir_instr(struct Sema* s, struct Expr* expr,
                                      HirInstrKind kind) {
    if (!s || !s->lower_ctx || !expr) return NULL;
    struct HirInstr* h = hir_instr_new(s->arena, kind, expr->span);
    if (!h) return NULL;
    h->type = s->unknown_type;  // floor; arm overwrites with real type
    hashmap_put(&s->lower_ctx->expr_hir, (uint64_t)(uintptr_t)expr, h);
    return h;
}

void sema_lower_modules(struct Sema* s) {
    if (!s || !s->compiler || !s->compiler->modules) return;
    Vec* modules = s->compiler->modules;
    for (size_t i = 0; i < modules->count; i++) {
        struct Module** mp = (struct Module**)vec_get(modules, i);
        struct Module* mod = mp ? *mp : NULL;
        if (!mod) continue;
        if (hashmap_get(&s->module_hir, (uint64_t)(uintptr_t)mod)) continue;
        struct HirModule* hmod = lower_module(s, mod);
        if (hmod) {
            hashmap_put(&s->module_hir, (uint64_t)(uintptr_t)mod, hmod);
        }
    }
    // Per-instantiation HIR: a generic decl has one source body but N
    // specializations, each with its own facts (per-instantiation
    // CheckedBody). Lower each instantiation into its own HirFn so
    // Phase G's per-instantiation effect verification can walk HIR
    // instead of re-walking the AST under inst->env.
    if (s->instantiations) {
        for (size_t i = 0; i < s->instantiations->count; i++) {
            struct Instantiation** ip = (struct Instantiation**)
                vec_get(s->instantiations, i);
            struct Instantiation* inst = ip ? *ip : NULL;
            if (inst && !inst->hir) lower_instantiation(s, inst);
        }
    }
}

void dump_tyck(struct Sema* s) {
    if (!s) return;
    printf("\n=== sema typechecking ===\n");
    // "instructions" replaces "facts" — the underlying count is per-HirInstr,
    // not per-fact. Reflects that HIR is now the source of truth for
    // type/semantic_kind data.
    printf("  instructions:  %zu\n", total_hir_instr_count(s));

    size_t counts[TYPE_PRODUCT + 1] = {0};
    tally_hir_by_kind(s, counts);

    printf("  type instructions:\n");
    for (int i = 0; i <= TYPE_PRODUCT; i++) {
        if (counts[i] == 0) continue;
        printf("    %-12s %zu\n", sema_type_kind_str((TypeKind)i), counts[i]);
    }

    if (s->compiler && s->compiler->modules) {
        bool printed_header = false;
        for (size_t i = 0; i < s->compiler->modules->count; i++) {
            struct Module** mod_p = (struct Module**)vec_get(s->compiler->modules, i);
            struct Module* mod = mod_p ? *mod_p : NULL;
            if (!mod || !mod->ast) continue;

            for (size_t j = 0; j < mod->ast->count; j++) {
                struct Expr** expr_p = (struct Expr**)vec_get(mod->ast, j);
                struct Expr* expr = expr_p ? *expr_p : NULL;
                if (!expr || expr->kind != expr_Bind) continue;
                if (!expr->bind.name.resolved) continue;

                struct ConstValue v = sema_decl_value(s, expr->bind.name.resolved);
                if (v.kind == CONST_INVALID) continue;

                if (!printed_header) {
                    printf("  comptime values:\n");
                    printed_header = true;
                }

                const char* name = s->pool
                    ? pool_get(s->pool, expr->bind.name.string_id, 0) : NULL;
                printf("    %-12s = ", name ? name : "?");
                sema_print_const_value(v, s);
                printf("\n");
            }
        }
    }

    // Folded call values are carried on HIR_CALL.folded_value, populated
    // during lowering from the SemaFact's value field (which itself was
    // written by sema_record_call_value at type-check time).
    struct HirFoldedCallCtx fold_ctx = { .s = s, .printed_header = false };
    walk_all_hir(s, hir_folded_call_visitor, &fold_ctx);
}

void dump_sema(struct Sema* s) {
    if (!s) return;
    printf("\n=== sema skeleton ===\n");
    size_t total = total_hir_instr_count(s);
    printf("  instructions:  %zu\n", total);
    printf("  effect sigs: %zu\n", s->effect_sig_cache.count);
    printf("  errors: %zu\n", s->diags ? s->diags->error_count : 0);

    size_t counts[TYPE_PRODUCT + 1] = {0};
    tally_hir_by_kind(s, counts);

    printf("  type instructions:\n");
    for (int i = 0; i <= TYPE_PRODUCT; i++) {
        if (counts[i] == 0) continue;
        printf("    %-12s %zu\n", sema_type_kind_str((TypeKind)i), counts[i]);
    }

    if (s->effect_sig_cache.count > 0) {
        printf("  effect signatures:\n");
        struct EffectSigDumpCtx ctx = {
            .s = s, .shown = 0, .limit = 12, .prefix = "    "
        };
        hashmap_foreach(&s->effect_sig_cache, dump_effect_sig_visitor, &ctx);
    }

    if (total > 0) {
        printf("  first instructions (semantic -> type):\n");
        struct HirSampleCtx ctx = { .s = s, .shown = 0, .limit = 12 };
        walk_all_hir(s, hir_sample_visitor, &ctx);
    }
}

static void print_evidence_vector(struct Sema* s, struct EvidenceVector* ev,
    const char* prefix) {
    if (!ev || !ev->frames || ev->frames->count == 0) {
        printf("%s<empty>\n", prefix);
        return;
    }
    for (size_t i = 0; i < ev->frames->count; i++) {
        struct EvidenceFrame* f = (struct EvidenceFrame*)vec_get(ev->frames, i);
        if (!f) continue;
        const char* eff_name = (f->effect_decl && s->pool)
            ? pool_get(s->pool, f->effect_decl->name.string_id, 0) : NULL;
        const char* h_name = (f->handler_decl && s->pool)
            ? pool_get(s->pool, f->handler_decl->name.string_id, 0) : NULL;
        const char* depth_label = (i == 0) ? "outermost"
            : (i + 1 == ev->frames->count) ? "innermost" : "         ";
        printf("%s[%zu %s] effect=%s handler=%s",
            prefix, i, depth_label,
            eff_name ? eff_name : "?",
            h_name ? h_name : "?");
        if (f->scope_token_id) printf(" scope#%u", f->scope_token_id);
        printf("\n");
    }
}

void dump_sema_evidence(struct Sema* s) {
    if (!s) return;
    printf("\n=== evidence vectors ===\n");
    if (!s->bodies) { printf("  no bodies\n"); return; }

    for (size_t i = 0; i < s->bodies->count; i++) {
        struct CheckedBody** bp = (struct CheckedBody**)vec_get(s->bodies, i);
        struct CheckedBody* body = bp ? *bp : NULL;
        if (!body) continue;
        const char* nm;
        if (body->decl && s->pool) {
            nm = pool_get(s->pool, body->decl->name.string_id, 0);
        } else if (body->module) {
            nm = "<module>";
        } else {
            nm = "<sig-resolution-scratch>";
        }
        printf("  body '%s'%s:\n",
            nm ? nm : "<unnamed>",
            body->instantiation ? " (instantiation)" : "");
        printf("    entry-evidence:\n");
        print_evidence_vector(s, body->entry_evidence, "      ");

        if (body->call_evidence.count == 0) continue;
        printf("    per-call snapshots: %zu\n", body->call_evidence.count);
        struct CallEvidenceDumpCtx ctx = { .s = s };
        hashmap_foreach(&body->call_evidence, dump_call_evidence_visitor, &ctx);
    }
}

void dump_sema_effects(struct Sema* s) {
    if (!s) return;
    printf("\n=== effect signatures ===\n");
    printf("  count: %zu\n", s->effect_sig_cache.count);

    struct EffectSigDumpCtx ctx = {
        .s = s, .shown = 0, .limit = 0, .prefix = "  "
    };
    hashmap_foreach(&s->effect_sig_cache, dump_effect_sig_visitor, &ctx);
}
