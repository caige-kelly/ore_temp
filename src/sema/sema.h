#ifndef SEMA_H
#define SEMA_H

#include <stdbool.h>
#include <stdint.h>

#include "../common/arena.h"
#include "../common/stringpool.h"
#include "../common/vec.h"
#include "../name_resolution/name_resolution.h"
#include "../parser/ast.h"
#include "../diag/diag.h"
#include "type.h"
#include "effects.h"

struct Compiler;

struct SemaFact {
    struct Expr* expr;
    struct Type* type;
    SemanticKind semantic_kind;
    uint32_t region_id;
};

struct Sema {
    struct Compiler* compiler;
    Arena* arena;
    StringPool* pool;
    struct Resolver* resolver;
    struct DiagBag* diags;
    Vec* facts;                // Vec of SemaFact
    Vec* effect_sigs;          // Vec of EffectSig* collected from function annotations
    bool has_errors;

    struct Type* unknown_type;
    struct Type* error_type;
    struct Type* void_type;
    struct Type* bool_type;
    struct Type* comptime_int_type;
    struct Type* comptime_float_type;
    struct Type* u8_type;
    struct Type* u16_type;
    struct Type* u32_type;
    struct Type* u64_type;
    struct Type* i8_type;
    struct Type* i16_type;
    struct Type* i32_type;
    struct Type* i64_type;
    struct Type* usize_type;
    struct Type* isize_type;
    struct Type* f64_type;
    struct Type* f32_type;
    struct Type* string_type;
    struct Type* nil_type;
    struct Type* type_type;
    struct Type* anytype_type;
    struct Type* module_type;
    struct Type* effect_type;
    struct Type* effect_row_type;
    struct Type* scope_token_type;
};

struct Sema sema_new(struct Compiler* compiler, struct Resolver* resolver);
bool sema_check(struct Sema* sema);
struct SemaFact* sema_fact_of(struct Sema* sema, struct Expr* expr);
struct Type* sema_type_of(struct Sema* sema, struct Expr* expr);
SemanticKind sema_semantic_of(struct Sema* sema, struct Expr* expr);
uint32_t sema_region_of(struct Sema* sema, struct Expr* expr);
struct EffectSig* sema_effect_sig_of(struct Sema* sema, struct Expr* expr);
void dump_sema(struct Sema* sema);
void dump_sema_effects(struct Sema* sema);
void dump_tyck(struct Sema* sema);

#endif // SEMA_H
