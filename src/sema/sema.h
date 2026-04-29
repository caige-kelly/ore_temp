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

struct Compiler;

// This is intentionally a skeleton, not the final type checker. It gives
// later Koka-style effects, Zig-style comptime, and borrow-lite escape
// analysis a single typed-semantic place to hang facts.
typedef enum {
    TYPE_UNKNOWN,
    TYPE_ERROR,
    TYPE_VOID,
    TYPE_BOOL,
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_STRING,
    TYPE_NIL,
    TYPE_TYPE,
    TYPE_ANYTYPE,
    TYPE_MODULE,
    TYPE_STRUCT,
    TYPE_ENUM,
    TYPE_EFFECT,
    TYPE_EFFECT_ROW,
    TYPE_SCOPE_TOKEN,
    TYPE_FUNCTION,
    TYPE_POINTER,
    TYPE_SLICE,
    TYPE_ARRAY,
    TYPE_PRODUCT,
} TypeKind;

typedef enum {
    EFFECT_TERM_UNKNOWN,
    EFFECT_TERM_NAMED,
    EFFECT_TERM_SCOPED,
    EFFECT_TERM_ROW,
} EffectTermKind;

struct EffectTerm {
    EffectTermKind kind;
    struct Expr* expr;          // source expression for named/scoped terms
    struct Decl* decl;          // resolved effect/row decl when known
    uint32_t name_id;           // effect display name when known
    uint32_t scope_token_id;    // region/color handle for scoped effects
    uint32_t row_name_id;       // effect-row variable name for EFFECT_TERM_ROW
};

struct EffectSig {
    struct Expr* source;        // source effect annotation expression
    Vec* terms;                 // Vec of EffectTerm
    bool is_open;               // true for <... | e> / <|e>
    uint32_t row_name_id;       // open row variable name, if any
    struct Decl* row_decl;      // resolved DECL_EFFECT_ROW when known
};

struct Type {
    TypeKind kind;
    uint32_t name_id;          // optional display/canonical name
    struct Decl* decl;         // optional source declaration
    struct Type* elem;         // pointer/slice/array element
    struct Type* ret;          // function return type
    Vec* params;               // Vec of Type* for function params
    Vec* effects;              // Vec of EffectSig* for function effects
    struct EffectSig* effect_sig; // explicit function effect annotation, if any
    uint32_t region_id;        // reserved borrow-lite scope color (0 = none)
};

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
    struct Type* int_type;
    struct Type* float_type;
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

#endif // SEMA_H
