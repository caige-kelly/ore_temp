#ifndef ORE_SEMA_EVIDENCE_H
#define ORE_SEMA_EVIDENCE_H

#include <stdbool.h>
#include <stdint.h>

#include "../common/vec.h"

struct Sema;
struct Expr;
struct Decl;
struct EvidenceVector;
struct EvidenceFrame;
struct CheckedBody;

// Lifecycle helpers for the active evidence stack. The Sema carries one
// EvidenceVector* (s->current_evidence) updated in step with the checker's
// lexical walk: push on `with`-entry, pop on `with`-exit.
struct EvidenceVector* sema_evidence_new(struct Sema* sema);
struct EvidenceVector* sema_evidence_clone(struct Sema* sema, struct EvidenceVector* src);
void sema_evidence_push(struct EvidenceVector* ev, struct EvidenceFrame frame);
void sema_evidence_pop(struct EvidenceVector* ev);
size_t sema_evidence_len(const struct EvidenceVector* ev);

// Public queries.
//   sema_evidence_at: snapshot of evidence active at the given expression.
//                     Currently records on every expr_Call during typechecking;
//                     this lookup returns the recorded snapshot if available,
//                     else the body's entry evidence.
//   sema_evidence_of_body: snapshot taken when this CheckedBody began.
struct EvidenceVector* sema_evidence_at(struct Sema* sema, struct Expr* expr);
struct EvidenceVector* sema_evidence_of_body(struct Sema* sema, struct CheckedBody* body);

// Record a per-call snapshot of the active evidence vector. Called by checker
// at every expr_Call so codegen can later look up the right `ev` slot for the
// effect being performed.
void sema_evidence_record_call(struct Sema* sema, struct Expr* call_expr);

#endif // ORE_SEMA_EVIDENCE_H
