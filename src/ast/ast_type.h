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


// ---- OptionalType (SK_OPTIONAL_TYPE) — `?T` -------------------------

typedef struct { SyntaxNode *syntax; } OptionalType;
bool        OptionalType_cast(const SyntaxNode *n, OptionalType *out);
SyntaxNode *OptionalType_inner(const OptionalType *o);


// ---- ConstType (SK_CONST_TYPE) — `const T` --------------------------

typedef struct { SyntaxNode *syntax; } ConstType;
bool        ConstType_cast(const SyntaxNode *n, ConstType *out);
SyntaxNode *ConstType_inner(const ConstType *c);


// ---- Kind-qualified nominal types (Slice 6.18) ---------------------
//
//   struct Foo / union Foo / enum Color / handler Bar / effect Foo
//
// All share one shape: { kind-keyword, IDENT }. The node KIND carries the
// asserted kind; `_name` is the referenced nominal's name token. Sema
// resolves the name and asserts the resolved decl matches the kind. The
// `...Ref` suffix avoids colliding with the db-layer StructType / EnumType
// result structs.

typedef struct { SyntaxNode *syntax; } StructTypeRef;
bool         StructTypeRef_cast(const SyntaxNode *n, StructTypeRef *out);
SyntaxToken *StructTypeRef_name(const StructTypeRef *t);

typedef struct { SyntaxNode *syntax; } UnionTypeRef;
bool         UnionTypeRef_cast(const SyntaxNode *n, UnionTypeRef *out);
SyntaxToken *UnionTypeRef_name(const UnionTypeRef *t);

typedef struct { SyntaxNode *syntax; } EnumTypeRef;
bool         EnumTypeRef_cast(const SyntaxNode *n, EnumTypeRef *out);
SyntaxToken *EnumTypeRef_name(const EnumTypeRef *t);

typedef struct { SyntaxNode *syntax; } HandlerTypeRef;
bool         HandlerTypeRef_cast(const SyntaxNode *n, HandlerTypeRef *out);
SyntaxToken *HandlerTypeRef_name(const HandlerTypeRef *t);

typedef struct { SyntaxNode *syntax; } EffectTypeRef;
bool         EffectTypeRef_cast(const SyntaxNode *n, EffectTypeRef *out);
SyntaxToken *EffectTypeRef_name(const EffectTypeRef *t);


#endif  // ORE_AST_TYPE_H
