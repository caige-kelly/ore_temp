#ifndef ORE_SEMA_TYPE_DECL_DATA_H
#define ORE_SEMA_TYPE_DECL_DATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../parser/ast.h"
#include "../ids/ids.h"
#include "../query/query.h"

struct Sema;
struct Type;

// Per-kind decl details — held off `struct DefInfo` so identity stays
// thin. Mirrors rust-analyzer's per-kind data queries (`function_data`
// / `function_signature`, `struct_data`, etc. — see
// `crates/hir-def/src/signatures.rs`). DefInfo is the *handle* (kind,
// name, span, scope position); detail data lives in side tables on
// `Sema`, populated by their own queries.
//
// Today we have:
//
//   FnSignature  — per-fn-DefId signature: param types, ret type,
//                  modifiers (is_comptime, has_effects). Computed by
//                  `query_fn_signature(fn_def)`. Separated from the
//                  fn's body-typing query so a body Ident referencing
//                  a param can read the signature without re-entering
//                  the parent fn's outer type query (which would be
//                  RUNNING and produce a CYCLE).
//
//   ParamLocator — per-param-DefId locator: which fn this param
//                  belongs to, and at what positional index. Set
//                  during scope_walk's `define_param`. Read by
//                  `query_type_of_def(DECL_PARAM)` to look up the
//                  param's type via FnSignature.param_types[index].
//                  Mirrors how rust-analyzer's `LocalFieldId` indexes
//                  into a struct's `Arena<FieldData>`.
//
// Stage E.3 additions:
//   StructSignature / FieldData / FieldLocator — for struct fields,
//     including C-style anonymous unions flattened into the field
//     arena via `union_group`.
//   EnumSignature / VariantData / VariantLocator — for enum variants,
//     including const-evaluated explicit values with C-style auto-
//     increment for implicit ones.
//
// Coming later:
//   FnSignature gains `effect_annotation`, `generic_params`.

// =====================================================================
// FnSignature
// =====================================================================

struct FnSignature {
    // Per-param details, parallel arrays indexed by declaration order.
    struct Type **param_types;       // arena-owned, length = param_count
    ParamKind   *param_kinds;        // arena-owned, length = param_count
    size_t       param_count;

    struct Type *ret_type;

    // Modifiers migrated off DefInfo (had no live readers there; here
    // they're co-located with the rest of the fn signature where
    // typecheck / codegen consumers naturally find them).
    bool is_comptime;     // `comptime fn foo(...)`
    bool has_effects;     // `<E>` annotation present

    struct QuerySlot query;
};

// Compute (or fetch cached) the signature for a fn-shaped DECL_USER
// def. Returns NULL if `def` is invalid, not a fn, or has no Lambda
// origin. The returned FnSignature is owned by Sema; do not free.
//
// This query is the producer for both:
//   - query_type_of_def(fn_def) — wraps signature into a TY_FN
//   - query_type_of_def(param_def) — indexes signature.param_types
// Splitting it out from query_type_of_def avoids a cycle when the
// fn's body queries reference a param while the outer fn-type query
// is still computing.
struct FnSignature *query_fn_signature(struct Sema *s, DefId fn_def);

// =====================================================================
// ParamLocator
// =====================================================================

struct ParamLocator {
    DefId    parent_fn;
    uint32_t index;        // position in parent_fn's param list (0-based)
};

// Populated by scope_walk's `define_param` once per param DefId.
void param_locator_set(struct Sema *s, DefId param_def, DefId parent_fn,
                       uint32_t index);

// Returns NULL if no entry exists. Borrowed pointer.
struct ParamLocator *param_locator_get(struct Sema *s, DefId param_def);

// =====================================================================
// StructSignature
// =====================================================================
//
// Per-field detail held in a flat arena on the parent struct's
// signature. C-style anonymous unions are flattened into the same
// arena: each union arm gets its own FieldData entry, with a non-zero
// `union_group` marking which union it belongs to. Lookup by name is
// uniform across standalone fields and union arms — `obj.x` walks the
// flat array regardless. Memory-layout grouping rides on `union_group`
// for codegen; standalone fields have `union_group == 0`.

struct FieldData {
    StrId name_id;
    struct Span   span;
    Visibility    vis;
    struct Type  *type;            // resolved field type
    uint32_t      union_group;     // 0 = standalone; >0 = anon union N (1-indexed)
};

struct StructSignature {
    struct FieldData *fields;       // arena array, length = field_count
    size_t            field_count;
    struct QuerySlot  query;
};

struct FieldLocator {
    DefId    parent_struct;
    uint32_t index;                // 0-based position in parent's fields
};

// Compute (or fetch cached) the signature for a struct-shaped DECL_USER
// def. Returns NULL if `def` is invalid or not a struct.
struct StructSignature *query_struct_signature(struct Sema *s, DefId struct_def);

void                    field_locator_set(struct Sema *s, DefId field_def,
                                          DefId parent_struct, uint32_t index);
struct FieldLocator    *field_locator_get(struct Sema *s, DefId field_def);

// =====================================================================
// EnumSignature
// =====================================================================
//
// Variant detail. Ore uses C-style enums: each variant has an
// auto-incrementing int value, optionally overridden by an
// `explicit_value` Expr that's const-evaluated at signature time.
// No payload variants — sum types are out of scope.

struct VariantData {
    StrId name_id;
    struct Span    span;
    struct Expr   *explicit_value;   // borrowed AST; NULL if implicit
    int64_t        value;            // const-eval'd; auto-incremented from prev when implicit
};

struct EnumSignature {
    struct VariantData *variants;
    size_t              variant_count;
    struct QuerySlot    query;
};

struct VariantLocator {
    DefId    parent_enum;
    uint32_t index;
};

struct EnumSignature  *query_enum_signature(struct Sema *s, DefId enum_def);

void                   variant_locator_set(struct Sema *s, DefId variant_def,
                                           DefId parent_enum, uint32_t index);
struct VariantLocator *variant_locator_get(struct Sema *s, DefId variant_def);

#endif
