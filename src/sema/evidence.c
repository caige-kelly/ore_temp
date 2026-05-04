#include "evidence.h"

#include <stdio.h>
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

void sema_evidence_record_call(struct Sema* s, struct Expr* call_expr) {
    if (!s || !call_expr) return;
    if (!s->current_body) {
        // Match body_record_hir's policy: warn once so missing-body
        // bugs surface instead of silently dropping evidence.
        static bool warned = false;
        if (!warned) {
            fprintf(stderr,
                "warning: sema_evidence_record_call called with no current_body "
                "(line %d); snapshot discarded\n", call_expr->span.line);
            warned = true;
        }
        return;
    }
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
