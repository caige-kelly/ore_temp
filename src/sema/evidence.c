#include "evidence.h"

#include <string.h>

#include "effects.h"
#include "sema.h"
#include "../parser/ast.h"

static struct EvidenceVector* alloc_vector(struct Sema* s) {
    if (!s || !s->arena) return NULL;
    struct EvidenceVector* ev = arena_alloc(s->arena, sizeof(struct EvidenceVector));
    if (!ev) return NULL;
    ev->frames = vec_new_in(s->arena, sizeof(struct EvidenceFrame));
    return ev;
}

struct EvidenceVector* sema_evidence_new(struct Sema* s) {
    return alloc_vector(s);
}

struct EvidenceVector* sema_evidence_clone(struct Sema* s, struct EvidenceVector* src) {
    struct EvidenceVector* copy = alloc_vector(s);
    if (!copy || !src || !src->frames) return copy;
    for (size_t i = 0; i < src->frames->count; i++) {
        struct EvidenceFrame* f = (struct EvidenceFrame*)vec_get(src->frames, i);
        if (f) vec_push(copy->frames, f);
    }
    return copy;
}

void sema_evidence_push(struct EvidenceVector* ev, struct EvidenceFrame frame) {
    if (!ev || !ev->frames) return;
    vec_push(ev->frames, &frame);
}

void sema_evidence_pop(struct EvidenceVector* ev) {
    if (!ev || !ev->frames || ev->frames->count == 0) return;
    ev->frames->count--;
}

size_t sema_evidence_len(const struct EvidenceVector* ev) {
    return (ev && ev->frames) ? ev->frames->count : 0;
}

// Resolve an Expr* to its resolved Decl* (Ident/Field shortcuts).
static struct Decl* resolved_decl(struct Expr* expr) {
    if (!expr) return NULL;
    if (expr->kind == expr_Ident) return expr->ident.resolved;
    if (expr->kind == expr_Field) return expr->field.field.resolved;
    if (expr->kind == expr_Call)  return resolved_decl(expr->call.callee);
    return NULL;
}

static struct Decl* effect_for_decl(struct Decl* d) {
    if (!d) return NULL;
    if (d->child_scope && d->child_scope->kind == SCOPE_EFFECT) return d;
    return NULL;
}

// Mirrors resolver Case 2: walk a function's effect annotation looking for an
// effect Decl. The annotation is Bin(Pipe), Call (Allocator(s)), or Field for
// namespaced effects.
static struct Decl* effect_from_annotation(struct Expr* eff) {
    if (!eff) return NULL;
    struct Expr* stack[16];
    int top = 0;
    stack[top++] = eff;
    while (top > 0) {
        struct Expr* e = stack[--top];
        if (!e) continue;
        struct Decl* ed = resolved_decl(e);
        struct Decl* eff_d = effect_for_decl(ed);
        if (eff_d) return eff_d;
        if (e->kind == expr_Bin && top + 2 < 16) {
            if (e->bin.Left)  stack[top++] = e->bin.Left;
            if (e->bin.Right) stack[top++] = e->bin.Right;
        } else if (e->kind == expr_Call) {
            if (e->call.callee && top < 16) stack[top++] = e->call.callee;
        } else if (e->kind == expr_Field) {
            if (e->field.object && top < 16) stack[top++] = e->field.object;
        } else if (e->kind == expr_EffectRow) {
            if (e->effect_row.head && top < 16) stack[top++] = e->effect_row.head;
        }
    }
    return NULL;
}

struct Decl* sema_evidence_effect_for_with_func(struct Sema* s, struct Expr* func) {
    if (!s || !func) return NULL;

    // Case 1: func itself names an effect Decl directly.
    struct Decl* d = resolved_decl(func);
    struct Decl* direct = effect_for_decl(d);
    if (direct) return direct;

    // Otherwise: did the resolver already cache the answer on the parent
    // expr_With? Sema callers pass `with.func`, but the cache lives on the
    // expr_With node itself. Callers that have it should read with.handled_effect
    // directly; this function is for cases where they only have the func.

    // Case 2: func is a handler function — walk its lambda's effect annotation
    // for the *handled* effect. Convention: handler is `fn(action: fn(...) <H> R) <propagated> R`,
    // so the effect we discharge lives in the action's first-param annotation.
    if (d && d->node && d->node->kind == expr_Bind &&
        d->node->bind.value &&
        d->node->bind.value->kind == expr_Lambda) {
        struct LambdaExpr* L = &d->node->bind.value->lambda;
        if (L->params && L->params->count > 0) {
            struct Param* p = (struct Param*)vec_get(L->params, 0);
            if (p && p->type_ann && p->type_ann->kind == expr_Lambda) {
                struct Decl* eff = effect_from_annotation(p->type_ann->lambda.effect);
                if (eff) return eff;
            }
        }
        // Fallback: look at the handler's own effect annotation.
        struct Decl* eff = effect_from_annotation(L->effect);
        if (eff) return eff;
    }

    // Case 3: convention fallback — capitalize the identifier and look it up.
    if (func->kind == expr_Ident && s->pool) {
        const char* nm = pool_get(s->pool, func->ident.string_id, 0);
        if (nm && nm[0] >= 'a' && nm[0] <= 'z' && d && d->owner) {
            char buf[256];
            size_t len = strlen(nm);
            if (len < sizeof(buf)) {
                memcpy(buf, nm, len + 1);
                buf[0] = (char)(buf[0] - 'a' + 'A');
                uint32_t cap_id = pool_intern(s->pool, buf, len);
                // Walk parent chain for the capitalized name.
                for (struct Scope* cur = d->owner; cur; cur = cur->parent) {
                    struct Decl* cd = (struct Decl*)hashmap_get(&cur->name_index, (uint64_t)cap_id);
                    struct Decl* eff = effect_for_decl(cd);
                    if (eff) return eff;
                }
            }
        }
    }

    return NULL;
}

void sema_evidence_record_call(struct Sema* s, struct Expr* call_expr) {
    if (!s || !call_expr || !s->current_body) return;
    if (!s->current_evidence || sema_evidence_len(s->current_evidence) == 0) return;
    struct EvidenceVector* snap = sema_evidence_clone(s, s->current_evidence);
    hashmap_put(&s->current_body->call_evidence, (uint64_t)(uintptr_t)call_expr, snap);
}

struct EvidenceVector* sema_evidence_at(struct Sema* s, struct Expr* expr) {
    if (!s || !expr || !s->current_body) return NULL;
    void* hit = hashmap_get(&s->current_body->call_evidence, (uint64_t)(uintptr_t)expr);
    if (hit) return (struct EvidenceVector*)hit;
    return s->current_body->entry_evidence;
}

struct EvidenceVector* sema_evidence_of_body(struct Sema* s, struct CheckedBody* body) {
    (void)s;
    return body ? body->entry_evidence : NULL;
}
