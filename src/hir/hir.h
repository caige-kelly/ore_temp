// HIR — High-level Intermediate Representation
//
// HIR sits between the AST (post-parser, post-name-resolution) and any
// future codegen. It's the form sema produces *as a side effect of
// type-checking*, replacing the per-Expr `CheckedBody.facts` table.
//
// Why HIR exists (one-paragraph version):
//   - The AST is a faithful record of source syntax. Walking it for
//     analysis means re-implementing semantics in every pass and
//     re-deciding "what does this expression mean" each time.
//   - HIR is a structured form where every instruction carries its
//     resolved type, semantic kind, and any decl back-pointers. Passes
//     that read it (effect solver, future codegen) walk a single
//     uniform shape — no expr-kind switch with 35 arms.
//   - Comptime branch dissolution becomes natural: `comptime if` is
//     evaluated *during* AST→HIR lowering and only the live branch is
//     emitted. Op resolution against the resulting HIR sees exactly
//     one `with` block, not both arms.
//
// Phase B scope (this file): types only. No lowering, no consumer, no
// behavior change. The lowering pass and consumer migration are
// separate phases per docs/pre-hir-audit.md and the HIR plan.

#ifndef ORE_HIR_H
#define ORE_HIR_H

#include <stdbool.h>
#include <stdint.h>

#include "../common/vec.h"
#include "../lexer/token.h"
#include "../parser/ast.h"
#include "../sema/ids/ids.h"
#include "../sema/scope/scope.h"

struct Type;
struct EffectSig;
struct ConstValue;
struct HirInstr;
struct HirFn;

// All HIR instruction kinds. One-to-one with the analysis-relevant AST
// kinds; type-position AST kinds (Struct/Enum/Effect/EffectRow/
// ArrayType/SliceType/ManyPtrType) don't appear here — they've already
// been resolved to `Type*` by the time HIR is built.
typedef enum {
    HIR_CONST,            // literal value or a comptime-folded constant
    HIR_REF,              // reference to a Decl (param, local, top-level)
    HIR_BIN,              // binary operation
    HIR_UNARY,            // unary operation
    HIR_CALL,             // function call (non-effect-op callee)
    HIR_FIELD,            // x.f
    HIR_INDEX,            // x[i]
    HIR_IF,               // structural if (post-splice for comptime if)
    HIR_LOOP,             // structural loop
    HIR_SWITCH,           // structural switch
    HIR_BIND,             // local declaration with optional initializer
    HIR_ASSIGN,           // store to l-value
    HIR_RETURN,           // return from enclosing fn
    HIR_BREAK,            // break out of enclosing loop
    HIR_CONTINUE,         // continue enclosing loop
    HIR_DEFER,            // defer the inner expression's effects to scope exit
    HIR_HANDLER_INSTALL,  // `with handler {…} body` — pushes evidence frame
    HIR_HANDLER_VALUE,    // `handler {…}` in value position (HandlerOf<E,R>)
    HIR_OP_PERFORM,       // call to an effect op (resolved against effect E)
    HIR_PRODUCT,          // .{ field = val, … } literal
    HIR_ARRAY_LIT,        // [N]T{…} literal
    HIR_ENUM_REF,         // .Variant — enum variant reference
    HIR_ASM,              // inline assembly placeholder
    HIR_TYPE_VALUE,       // a type used as a value (e.g. RHS of `T :: i32`)
    HIR_BUILTIN,          // `@name(args...)` — sizeOf, alignOf, import, etc.
                          // Discriminated by name_id; consumers (codegen,
                          // const_eval) decide what each does. Comptime
                          // builtin routing through queries is a deferred
                          // audit-doc item.
    HIR_LAMBDA,            // anonymous function value (a callback / action arg).
                           // Phase B punted this in favor of HirRef, but
                           // anonymous lambdas in arg position have no Decl
                           // to reference — a dedicated kind is cleaner.
    HIR_ERROR,            // placeholder for failed lowering — keeps the
                          // HIR walk-able even when an error occurred
} HirInstrKind;

// Binary and unary op enums reuse the AST's representation directly —
// binops use TokenKind (matching `struct BinExpr.op` in ast.h), unary
// ops use the dedicated UnaryOp enum.
typedef enum TokenKind HirBinOp;
typedef enum UnaryOp HirUnaryOp;

// ----- Per-kind payloads -----

struct HirConstPayload {
    // Either a literal value (string_id for strings; int_val for ints;
    // f64_val for floats; bool_val for bools) or a folded ConstValue
    // attached for downstream consumers. The `value` pointer is owned
    // by the per-instr arena and may be NULL for literals that can be
    // reconstructed from the kind+raw fields alone.
    struct ConstValue* value;
};

struct HirRefPayload {
    DefId def;                    // resolved via query_resolve_ref
};

struct HirBinPayload {
    HirBinOp op;
    struct HirInstr* left;
    struct HirInstr* right;
};

struct HirUnaryPayload {
    HirUnaryOp op;
    bool postfix;
    struct HirInstr* operand;
};

struct HirCallPayload {
    struct HirInstr* callee;      // typically a HIR_REF to the function
    Vec* args;                    // Vec of HirInstr*
    DefId callee_def;             // resolved callee — duplicates callee.ref
                                  // for the common case but lets passes skip
                                  // the indirection
    struct ConstValue* folded_value; // populated when sema folded the call
                                  // at compile time (NULL otherwise). Used
                                  // by --dump-tyck's "folded calls" section
                                  // and reachable to future codegen.
};

struct HirFieldPayload {
    struct HirInstr* object;
    DefId field_def;              // resolved field; INVALID until sema fills
    uint32_t field_name_id;       // name when field_def unresolved
};

struct HirIndexPayload {
    struct HirInstr* object;
    struct HirInstr* index;
};

struct HirIfPayload {
    struct HirInstr* condition;
    Vec* then_block;              // Vec of HirInstr*
    Vec* else_block;              // Vec of HirInstr*; NULL when no else
    // Capture for unwrap-style `if (opt) |x| then`. INVALID when not used.
    DefId capture;
};

struct HirLoopPayload {
    // C-style: `init; while (cond) { body; step }`. Any of init/cond/step
    // may be NULL (infinite loop, no init, etc.). Capture for unwrap-style
    // `while (opt) |x| body`.
    struct HirInstr* init;
    struct HirInstr* condition;
    struct HirInstr* step;
    Vec* body_block;              // Vec of HirInstr*
    DefId capture;
};

struct HirSwitchArm {
    Vec* patterns;                // Vec of HirInstr* — pattern values to match
    Vec* body_block;              // Vec of HirInstr*
};

struct HirSwitchPayload {
    struct HirInstr* scrutinee;
    Vec* arms;                    // Vec of HirSwitchArm*
};

struct HirBindPayload {
    DefId def;                    // the binding being introduced
    struct HirInstr* init;        // initializer, may be NULL for declarations
                                  // without an init expression
};

struct HirAssignPayload {
    struct HirInstr* target;      // l-value
    struct HirInstr* value;
};

struct HirReturnPayload {
    struct HirInstr* value;       // NULL for `return;`
};

struct HirDeferPayload {
    struct HirInstr* value;
};

// `with [binder :=] handler {…} body` — install a handler for an
// effect, run body, pop. The binder def is valid for named-handler
// installs (`with f := named handler {…}`); body sees `f` in scope.
struct HirHandlerInstallPayload {
    DefId effect_def;             // the matched effect E
    struct HirInstr* handler;     // a HIR_HANDLER_VALUE
    DefId binder;                 // INVALID for anonymous installs
    Vec* body_block;              // Vec of HirInstr*
};

// One op implementation inside a HIR_HANDLER_VALUE. Carries the source
// effect's op def plus the lowered body that runs when the op is
// performed.
struct HirHandlerOp {
    DefId op_def;                 // the effect's op def this implements
    Vec* params;                  // Vec of DefId — op param defs
    Vec* body_block;              // Vec of HirInstr* — implementation
    bool is_ctl;                  // ctl op (multi-shot capable) vs fn op
};

struct HirHandlerValuePayload {
    DefId effect_def;             // matched effect E
    Vec* operations;              // Vec of HirHandlerOp*
    Vec* initially_block;         // Vec of HirInstr*; NULL when absent
    Vec* finally_block;           // Vec of HirInstr*; NULL when absent
    Vec* return_block;            // Vec of HirInstr*; NULL when absent
};

struct HirOpPerformPayload {
    DefId effect_def;             // E
    DefId op_def;                 // the op being performed
    Vec* args;                    // Vec of HirInstr*
};

struct HirProductPayload {
    Vec* fields;                  // Vec of HirInstr* — positional or named
    struct Type* type_hint;       // expected type, if known at lowering
};

struct HirArrayLitPayload {
    struct HirInstr* size;        // optional explicit size
    struct Type* elem_type;
    struct HirInstr* initializer; // optional fill expression
};

struct HirEnumRefPayload {
    DefId variant_def;            // resolved when the surrounding context
                                  // supplied an expected enum type
    uint32_t variant_name_id;     // raw name, set unconditionally
};

struct HirAsmPayload {
    uint32_t string_id;
};

struct HirTypeValuePayload {
    struct Type* type;
};

struct HirBuiltinPayload {
    uint32_t name_id;             // intern-id of the builtin name
    Vec* args;                    // Vec of HirInstr*
};

struct HirLambdaPayload {
    struct HirFn* fn;             // anonymous fn (source==NULL)
    bool is_ctl;                  // true when lowered from expr_Ctl
};

// ----- The instruction node -----
//
// Tagged union mirroring `struct Expr`'s shape. Per-instr storage of
// `type` and `semantic_kind` is the whole point — passes read these
// directly instead of going through the per-Expr facts table.
struct HirInstr {
    HirInstrKind kind;
    struct Span span;
    struct Type* type;            // resolved at lowering time; never NULL
                                  // post-lowering (use s->error_type for
                                  // failed-lowering nodes)
    SemanticKind semantic_kind;
    uint32_t region_id;
    union {
        struct HirConstPayload constant;
        struct HirRefPayload ref;
        struct HirBinPayload bin;
        struct HirUnaryPayload unary;
        struct HirCallPayload call;
        struct HirFieldPayload field;
        struct HirIndexPayload index;
        struct HirIfPayload if_instr;
        struct HirLoopPayload loop;
        struct HirSwitchPayload switch_instr;
        struct HirBindPayload bind;
        struct HirAssignPayload assign;
        struct HirReturnPayload return_instr;
        struct HirDeferPayload defer;
        struct HirHandlerInstallPayload handler_install;
        struct HirHandlerValuePayload handler_value;
        struct HirOpPerformPayload op_perform;
        struct HirProductPayload product;
        struct HirArrayLitPayload array_lit;
        struct HirEnumRefPayload enum_ref;
        struct HirAsmPayload asm_instr;
        struct HirTypeValuePayload type_value;
        struct HirBuiltinPayload builtin;
        struct HirLambdaPayload lambda;
        // HIR_BREAK, HIR_CONTINUE, HIR_ERROR have no payload — kind +
        // span are sufficient.
    };
};

// ----- Per-fn HIR -----

struct HirParam {
    DefId def;                    // the param's def
    struct Type* type;             // param type
    bool is_comptime;
    bool is_inferred_comptime;
};

struct HirFn {
    DefId source;                 // the source-side def this lowered from
    Vec* params;                  // Vec of HirParam*
    struct Type* ret_type;
    struct EffectSig* effect_sig; // declared effect row, or NULL for `<>`
    Vec* body_block;              // Vec of HirInstr*
    struct Span span;             // source span (the `fn` keyword)
};

// ----- Per-module HIR -----

struct HirModule {
    ModuleId source;              // source-side module
    Vec* functions;               // Vec of HirFn*
};

// ----- Constructors / sanity helpers -----
//
// Every alloc takes the Sema arena (passed in by the caller, since the
// hir module doesn't depend on the full Sema struct). All pointers
// stable for the lifetime of that arena.

struct HirInstr* hir_instr_new(Arena* arena, HirInstrKind kind,
                               struct Span span);
struct HirFn* hir_fn_new(Arena* arena, DefId source, struct Span span);
struct HirModule* hir_module_new(Arena* arena, ModuleId source);

// === Layer 5 HIR construction helpers ===
//
// Thin allocators that future sema_check_expr (and any HIR builder)
// calls when emitting resolved-name HIR. Each takes the Sema arena
// plus the resolution result directly — no AST mutation, no second
// pass. Result types stay NULL (filled by typecheck) and
// semantic_kind defaults to SEM_UNKNOWN until typecheck stamps it.

struct HirInstr* hir_make_ref(Arena* arena, DefId def, struct Span span);
struct HirInstr* hir_make_field(Arena* arena, struct HirInstr* object,
                                DefId field_def, uint32_t field_name_id,
                                struct Span span);
struct HirInstr* hir_make_op_perform(Arena* arena, DefId effect_def,
                                     DefId op_def, Vec* args,
                                     struct Span span);
struct HirInstr* hir_make_bind(Arena* arena, DefId def,
                               struct HirInstr* init, struct Span span);

// Returns a stable string name for a HirInstrKind — for `--dump-hir` and
// diagnostics. Never NULL; unknown kinds return "?".
const char* hir_kind_str(HirInstrKind kind);

// ----- Per-instruction accessors -----
//
// HirInstr already exposes type / semantic_kind / region_id as direct
// fields. These thin inline accessors give consumers a stable API
// independent of the field layout, mirroring the per-Expr trio
// (sema_type_of, sema_semantic_of, sema_region_of) that the HIR
// migration replaces. NULL-safe — return defaults when h is NULL.

static inline struct Type* hir_type_of(struct HirInstr* h) {
    return h ? h->type : NULL;
}

static inline SemanticKind hir_semantic_of(struct HirInstr* h) {
    return h ? h->semantic_kind : SEM_UNKNOWN;
}

static inline uint32_t hir_region_of(struct HirInstr* h) {
    return h ? h->region_id : 0;
}

#endif // ORE_HIR_H
