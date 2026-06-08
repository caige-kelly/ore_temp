#ifndef ORE_AST_TYPE_H
#define ORE_AST_TYPE_H

#include "./ast.h"


// ---- RefType (SK_REF_TYPE) ------------------------------------------
//
//   T   — a bare type identifier (e.g. `i32`, `MyStruct`)
//
typedef struct { SyntaxNode *syntax; } RefType;
bool         RefType_cast(const SyntaxNode *n, RefType *out);
SyntaxToken *RefType_name(const RefType *r);


// ---- PtrType (SK_PTR_TYPE) — `^T` -----------------------------------

typedef struct { SyntaxNode *syntax; } PtrType;
bool        PtrType_cast(const SyntaxNode *n, PtrType *out);
SyntaxNode *PtrType_pointee(const PtrType *p);


// ---- SliceType (SK_SLICE_TYPE) — `[]T` -----------------------------

typedef struct { SyntaxNode *syntax; } SliceType;
bool        SliceType_cast(const SyntaxNode *n, SliceType *out);
SyntaxNode *SliceType_element(const SliceType *s);


// ---- ArrayType (SK_ARRAY_TYPE) — `[N]T` ----------------------------

typedef struct { SyntaxNode *syntax; } ArrayType;
bool        ArrayType_cast(const SyntaxNode *n, ArrayType *out);
SyntaxNode *ArrayType_size   (const ArrayType *a);  // expr node
SyntaxNode *ArrayType_element(const ArrayType *a);  // type node


// ---- ManyPtrType (SK_MANY_PTR_TYPE) — `[^]T` -----------------------

typedef struct { SyntaxNode *syntax; } ManyPtrType;
bool        ManyPtrType_cast(const SyntaxNode *n, ManyPtrType *out);
SyntaxNode *ManyPtrType_element(const ManyPtrType *m);


// ---- FnType (SK_FN_TYPE) — `Fn(params) <effects> -> ret` ------------

typedef struct { SyntaxNode *syntax; } FnType;
bool        FnType_cast(const SyntaxNode *n, FnType *out);
SyntaxNode *FnType_params     (const FnType *f);  // SK_PARAM_LIST
SyntaxNode *FnType_effect_row (const FnType *f);  // optional SK_EFFECT_ROW_TYPE
SyntaxNode *FnType_return_type(const FnType *f);  // optional; skips effect row


// ---- EffectRowType (SK_EFFECT_ROW_TYPE) — `<H, ..e>` / `<...>` ------
//
// Labels in SK_EFFECT_LABEL_LIST; optional `..e` / `...` tail in
// SK_EFFECT_ROW_TAIL (find-by-kind, not a token-scan).
typedef struct { SyntaxNode *syntax; } EffectRowType;
bool         EffectRowType_cast(const SyntaxNode *n, EffectRowType *out);
SyntaxNode  *EffectRowType_labels  (const EffectRowType *r); // SK_EFFECT_LABEL_LIST
SyntaxNode  *EffectRowType_tail    (const EffectRowType *r); // optional SK_EFFECT_ROW_TAIL
SyntaxToken *EffectRowType_tail_var(const EffectRowType *r); // IDENT in `..e`; NULL for `...`/closed


// ---- OptionalType (SK_OPTIONAL_TYPE) — `?T` -------------------------

typedef struct { SyntaxNode *syntax; } OptionalType;
bool        OptionalType_cast(const SyntaxNode *n, OptionalType *out);
SyntaxNode *OptionalType_inner(const OptionalType *o);


// ---- ConstType (SK_CONST_TYPE) — `const T` --------------------------

typedef struct { SyntaxNode *syntax; } ConstType;
bool        ConstType_cast(const SyntaxNode *n, ConstType *out);
SyntaxNode *ConstType_inner(const ConstType *c);


// ---- handler type CONSTRUCTOR (`handler E`) ------------------------
//
// 7.2: the struct/enum/union/effect kind-QUALIFIERS were removed — types are
// referenced BARE. `handler E` survives because it CONSTRUCTS a new type
// (a handler discharging effect E), with no bare equivalent. Shape:
// { `handler` keyword, IDENT }; `_name` is the effect name token. The `...Ref`
// suffix avoids colliding with the db-layer result structs.

typedef struct { SyntaxNode *syntax; } HandlerTypeRef;
bool         HandlerTypeRef_cast(const SyntaxNode *n, HandlerTypeRef *out);
SyntaxToken *HandlerTypeRef_name(const HandlerTypeRef *t);


// ---- DistinctType (SK_DISTINCT_TYPE) — `distinct T` (Slice 6.19) ----
//
// A nominal newtype former. Unlike the kind-qualified `*Ref` types above
// (keyword + IDENT), the operand here is a full backing TYPE node — so the
// shape mirrors OptionalType/ConstType, and `_backing` is the wrapped type.

typedef struct { SyntaxNode *syntax; } DistinctType;
bool        DistinctType_cast(const SyntaxNode *n, DistinctType *out);
SyntaxNode *DistinctType_backing(const DistinctType *d);


// ---- BitFieldType (SK_BIT_FIELD_TYPE) — `bit_field T { f: ty | w }` (6.22) --
//
// Odin-style bitpacking former. `_backing` is the lone DIRECT type-node child
// (field types are grandchildren under SK_BIT_FIELD_LIST), so it mirrors
// DistinctType. `_fields` returns the SK_BIT_FIELD_LIST wrapper for the later
// sema pass (per-field name/type/width accessors deferred to that slice).

typedef struct { SyntaxNode *syntax; } BitFieldType;
bool        BitFieldType_cast(const SyntaxNode *n, BitFieldType *out);
SyntaxNode *BitFieldType_backing(const BitFieldType *bf);
SyntaxNode *BitFieldType_fields(const BitFieldType *bf);


#endif  // ORE_AST_TYPE_H
