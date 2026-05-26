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


// ---- FnType (SK_FN_TYPE) — `Fn(params) -> ret` ----------------------

typedef struct { SyntaxNode *syntax; } FnType;
bool        FnType_cast(const SyntaxNode *n, FnType *out);
SyntaxNode *FnType_params     (const FnType *f);  // SK_PARAM_LIST
SyntaxNode *FnType_return_type(const FnType *f);  // optional


// ---- OptionalType (SK_OPTIONAL_TYPE) — `?T` -------------------------

typedef struct { SyntaxNode *syntax; } OptionalType;
bool        OptionalType_cast(const SyntaxNode *n, OptionalType *out);
SyntaxNode *OptionalType_inner(const OptionalType *o);


// ---- ConstType (SK_CONST_TYPE) — `const T` --------------------------

typedef struct { SyntaxNode *syntax; } ConstType;
bool        ConstType_cast(const SyntaxNode *n, ConstType *out);
SyntaxNode *ConstType_inner(const ConstType *c);


#endif  // ORE_AST_TYPE_H
