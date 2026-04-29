#ifndef ORE_SEMA_EFFECTS_H
#define ORE_SEMA_EFFECTS_H

#include <stdbool.h>
#include <stdint.h>

#include "../common/vec.h"
#include "../name_resolution/name_resolution.h"
#include "../parser/ast.h"

struct Sema;

typedef enum {
    EFFECT_TERM_UNKNOWN,
    EFFECT_TERM_NAMED,
    EFFECT_TERM_SCOPED,
    EFFECT_TERM_ROW,
} EffectTermKind;

struct EffectTerm {
    EffectTermKind kind;
    struct Expr* expr;
    struct Decl* decl;
    uint32_t name_id;
    uint32_t scope_token_id;
    uint32_t row_name_id;
};

struct EffectSig {
    struct Expr* source;
    Vec* terms;
    bool is_open;
    uint32_t row_name_id;
    struct Decl* row_decl;
};

struct EffectSig* sema_effect_sig_from_expr(struct Sema* sema, struct Expr* effect);
void sema_print_effect_sig(struct Sema* sema, struct EffectSig* sig);

#endif // ORE_SEMA_EFFECTS_H