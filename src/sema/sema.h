#ifndef SEMA_H
#define SEMA_H

#include <stdbool.h>
#include <stdint.h>

#include "../common/arena.h"
#include "../common/hashmap.h"
#include "../common/stringpool.h"
#include "../common/vec.h"
#include "../name_resolution/name_resolution.h"
#include "../parser/ast.h"
#include "../diag/diag.h"
#include "query.h"
#include "target.h"
#include "type.h"
#include "effects.h"
#include "decls.h"
#include "const_eval.h"

struct Compiler;
struct Instantiation;

struct SemaFact {
    struct Expr* expr;
    struct Type* type;
    SemanticKind semantic_kind;
    uint32_t region_id;
    struct ConstValue value;  
};

struct ComptimeArgTuple {
    Vec* values;  // Vec of ConstValue
};

// A CheckedBody owns the facts derived from one type-checked unit:
//   - top-level expressions in a module
//   - a function/handler body (one body per Decl, by default)
//   - a specialized function body (one body per Instantiation, in Phase 5)
//
// Facts are not global: "this Expr has this Type" is only meaningful inside
// the checked body that produced it. The module body acts as the catch-all
// for top-level facts.
struct CheckedBody {
    struct Decl* decl;                  // owning decl, NULL for the module body
    struct Module* module;              // module this body lives in
    struct Instantiation* instantiation;// non-NULL when the body is a generic specialization
    Vec* facts;                         // Vec of SemaFact
    struct EvidenceVector* entry_evidence; // evidence stack on body entry
    HashMap call_evidence;              // Expr* (uint64_t) -> EvidenceVector*: per-call snapshot
};

struct Sema {
    struct Compiler* compiler;
    Arena* arena;
    StringPool* pool;
    struct Resolver* resolver;
    struct DiagBag* diags;
    Vec* bodies;               // Vec of CheckedBody*
    struct CheckedBody* current_body;
    Vec* instantiations;       // Vec of Instantiation* (insertion order, for iteration)
    HashMap instantiation_buckets; // Decl* (uint64_t) -> Vec<Instantiation*>* (per-decl bucket)
    HashMap decl_info;         // Decl* (uint64_t) -> SemaDeclInfo*: per-Decl sema cache
    struct ComptimeEnv* current_env;
    struct EvidenceVector* current_evidence; // active handler stack during checker walk
    HashMap effect_sig_cache;  // Expr* (uint64_t) -> EffectSig* — interning by source annotation
    Vec* query_stack;          // Vec of QueryFrame for cycle/debug context
    int comptime_call_depth;   // guard against infinite comptime recursion
    HashMap call_cache;       // Decl* → Vec<ComptimeCallCacheEntry*>
    int64_t comptime_body_evals;   // instrumentation: how many times we've actually run a body
    bool has_errors;

    struct Type* unknown_type;
    struct Type* error_type;
    struct Type* void_type;
    struct Type* noreturn_type;
    struct Type* bool_type;
    struct Type* comptime_int_type;
    struct Type* comptime_float_type;
    struct Type* u8_type;
    // Cached `const u8` for hot paths — string indexing returns this since
    // strings are conceptually `[]const u8`. Built lazily during sema init.
    struct Type* const_u8_type;
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

    // Pre-interned name IDs for keyword-like names compared in hot paths
    // (sema_infer_expr's expr_Builtin switch, const_eval's eval_builtin
    // and target-field chain). Each one removes a per-call
    // strcmp(pool_get(id), "name") via `sema_name_is`. Compute once at
    // sema_new; compare uint32_t ids thereafter.
    uint32_t name_import;
    uint32_t name_sizeOf;
    uint32_t name_alignOf;
    uint32_t name_intCast;
    uint32_t name_TypeOf;
    uint32_t name_target;
    uint32_t name_true;
    uint32_t name_false;
    uint32_t name_returnType;

    // string_id (uint64_t) -> struct Type* for primitive names
    // (i32, bool, void, comptime_int, ...). Replaces a 22-arm strcmp
    // chain in `sema_primitive_type_for_name` with one hashmap lookup.
    HashMap primitive_types;

    // Module* (uint64_t) -> struct HirModule*. Populated by
    // `sema_lower_modules` after `sema_check` completes; consumers
    // (`--dump-hir`, future codegen, post-C4 effect solver) read from
    // here. NULL entries mean lowering hasn't run yet for that module.
    HashMap module_hir;
};

struct Sema sema_new(struct Compiler* compiler, struct Resolver* resolver);
bool sema_check(struct Sema* sema);
// Run AST→HIR lowering for every loaded module. Stores results in
// `sema->module_hir`. Must be called after a successful `sema_check`.
// Phase C1: per-module HIR is built but no consumer reads from it
// yet beyond `--dump-hir`. Idempotent — safe to call once.
void sema_lower_modules(struct Sema* sema);
struct SemaFact* sema_fact_of(struct Sema* sema, struct Expr* expr);
struct Type* sema_type_of(struct Sema* sema, struct Expr* expr);
SemanticKind sema_semantic_of(struct Sema* sema, struct Expr* expr);
uint32_t sema_region_of(struct Sema* sema, struct Expr* expr);
struct EffectSig* sema_effect_sig_of(struct Sema* sema, struct Expr* expr);
void dump_sema(struct Sema* sema);
void dump_sema_effects(struct Sema* sema);
void dump_sema_evidence(struct Sema* sema);
void dump_tyck(struct Sema* sema);
void sema_record_call_value(struct Sema* s, struct Expr* call_expr, struct ConstValue v);

#endif // SEMA_H
