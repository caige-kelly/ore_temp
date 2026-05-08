#ifndef ORE_SEMA_EFFECTS_H
#define ORE_SEMA_EFFECTS_H

#include <stdbool.h>
#include <stdint.h>

#include "../common/vec.h"
#include "scope/scope.h"
#include "../parser/ast.h"

struct Sema;

// EffectTermKind: terms are the *concrete* effects in a sig/set. Open-row
// variables (the `e` in `<H | e>`) are NOT terms — they live in is_open /
// row_name_id on the parent EffectSig/EffectSet. UNKNOWN is reserved for
// effect annotations that the resolver couldn't tie to an effect Decl;
// adders skip it.
typedef enum {
    EFFECT_TERM_UNKNOWN,
    EFFECT_TERM_NAMED,
    EFFECT_TERM_SCOPED,
} EffectTermKind;

struct EffectTerm {
    EffectTermKind kind;
    struct Expr* expr;
    struct Decl* decl;
    uint32_t name_id;
    uint32_t scope_token_id;
};

struct EffectSig {
    struct Expr* source;
    Vec* terms;                 // Vec of EffectTerm — only NAMED/SCOPED
    bool is_open;
    uint32_t row_name_id;
    struct Decl* row_decl;
};

// The set of effects a body actually performs (as opposed to EffectSig, which
// is the *declared* annotation). Built by walking a CheckedBody and unioning
// each callee's declared effects. Open rows live in `open` / `open_row_name_id`.
struct EffectSet {
    Vec* terms;                 // Vec of EffectTerm — only NAMED/SCOPED
    bool open;
    uint32_t open_row_name_id;
};

// One handler frame in scope at a given program point. Frames are stored in
// push order: frames[0] is the outermost `with`, frames[n-1] is the innermost.
// Codegen reverses on the way to libmprompt if it wants ev[0] = innermost.
//
// (A future EvidenceKind will distinguish tail-resumptive handlers — which
// can take libmprompt's fast path — from general handlers that need
// mp_prompt. The inference for that doesn't exist yet, so we don't carry a
// kind field that nobody can fill in correctly.)
struct EvidenceFrame {
    struct Decl* effect_decl;       // the effect this frame discharges
    struct Decl* handler_decl;      // resolved decl of with.func (the handler), if any
    uint32_t scope_token_id;        // 0 unless the effect is `scoped effect<s>`
};

// Snapshot of the active handler stack. See EvidenceFrame for ordering.
// Codegen lowers a perform-effect call by searching frames for the matching
// effect_decl and using the position to pick the libmprompt evidence slot.
struct EvidenceVector {
    Vec* frames;                    // Vec of EvidenceFrame, push order
};

struct EffectSig* sema_effect_sig_from_expr(struct Sema* sema, struct Expr* effect);
void sema_print_effect_sig(struct Sema* sema, struct EffectSig* sig);

#endif // ORE_SEMA_EFFECTS_H