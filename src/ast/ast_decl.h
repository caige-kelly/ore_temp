#ifndef ORE_AST_DECL_H
#define ORE_AST_DECL_H

// =====================================================================
// Typed wrappers for declaration nodes.
// =====================================================================
//
// Every wrapper holds a borrowed SyntaxNode*; accessors return
// RETURNS_OWNED handles (caller releases) per the convention in
// [ast.h](./ast.h).

#include "./ast.h"


// ---- FnDef (SK_FN_DECL) ---------------------------------------------
//
//   fn name(params...) -> ret_type { body }
//
typedef struct { SyntaxNode *syntax; } FnDef;
bool         FnDef_cast(const SyntaxNode *n, FnDef *out);
SyntaxToken *FnDef_name(const FnDef *f);          // SK_IDENT
SyntaxNode  *FnDef_params(const FnDef *f);        // SK_PARAM_LIST
SyntaxNode  *FnDef_return_type(const FnDef *f);   // any *_TYPE node, optional
SyntaxNode  *FnDef_body(const FnDef *f);          // SK_BLOCK_STMT


// ---- StructDef (SK_STRUCT_DECL) -------------------------------------
//
//   struct Name { fields... }
//
typedef struct { SyntaxNode *syntax; } StructDef;
bool         StructDef_cast(const SyntaxNode *n, StructDef *out);
SyntaxToken *StructDef_name(const StructDef *s);  // SK_IDENT
SyntaxNode  *StructDef_fields(const StructDef *s); // SK_FIELD_LIST


// ---- EnumDef (SK_ENUM_DECL) -----------------------------------------
//
//   enum Name { variants... }
//
typedef struct { SyntaxNode *syntax; } EnumDef;
bool         EnumDef_cast(const SyntaxNode *n, EnumDef *out);
SyntaxToken *EnumDef_name(const EnumDef *e);      // SK_IDENT
SyntaxNode  *EnumDef_variants(const EnumDef *e);  // SK_VARIANT_LIST


// ---- UnionDef (SK_UNION_DECL) ---------------------------------------
//
//   union Name { variants... }
//
typedef struct { SyntaxNode *syntax; } UnionDef;
bool         UnionDef_cast(const SyntaxNode *n, UnionDef *out);
SyntaxToken *UnionDef_name(const UnionDef *u);
SyntaxNode  *UnionDef_variants(const UnionDef *u); // SK_FIELD_LIST (parser-shared with struct; name is historic)


// ---- EffectDef (SK_EFFECT_DECL) -------------------------------------
//
//   effect Name { ops... }
//
typedef struct { SyntaxNode *syntax; } EffectDef;
bool         EffectDef_cast(const SyntaxNode *n, EffectDef *out);
SyntaxToken *EffectDef_name(const EffectDef *e);
SyntaxNode  *EffectDef_ops(const EffectDef *e);    // SK_FIELD_LIST (ops are
                                                    // field-shaped: name + sig)


// ---- BindDef (SK_BIND_DECL) -----------------------------------------
//
//   name :: value            (const — compile-time immutable)
//   name := value            (var   — runtime-mutable)
//   name : type : value      (const-typed)
//   name : type = value      (var-typed)
//   name : type              (bare typed decl)
//
// Mutability is a PROPERTY, read from the bind-op token (`::` → const,
// `:=`/`:` → var) — NOT a node kind. Matches Odin's Ast_ValueDecl +
// is_mutable and Zig's mut_token (7.0a). The DefKind (KIND_CONSTANT /
// KIND_VARIABLE) is derived in decl_classify (db/query/parse.c).
typedef struct { SyntaxNode *syntax; } BindDef;
bool         BindDef_cast(const SyntaxNode *n, BindDef *out);
SyntaxToken *BindDef_name(const BindDef *b);
SyntaxNode  *BindDef_type(const BindDef *b);      // optional
SyntaxNode  *BindDef_value(const BindDef *b);     // optional, an expression node
bool         BindDef_is_const(const BindDef *b);  // true iff the bind-op is `::`


// NOTE: imports are EXCLUSIVELY the `@import("path")` builtin (an
// SK_BUILTIN_EXPR, see ast_expr.h) — there is no decl form. The former
// SK_IMPORT_DECL kind + ImportDef wrapper were vestigial (never emitted by
// the parser) and have been removed.


// ---- Param (SK_PARAM) -----------------------------------------------
//
//   name: type
//
typedef struct { SyntaxNode *syntax; } Param;
bool         Param_cast(const SyntaxNode *n, Param *out);
SyntaxToken *Param_name(const Param *p);
SyntaxNode  *Param_type(const Param *p);


// ---- Field (SK_FIELD) -----------------------------------------------
//
//   name: type [= default]
//
typedef struct { SyntaxNode *syntax; } Field;
bool         Field_cast(const SyntaxNode *n, Field *out);
SyntaxToken *Field_name(const Field *f);
SyntaxNode  *Field_type(const Field *f);
SyntaxNode  *Field_default(const Field *f);   // optional


// ---- Variant (SK_VARIANT) -------------------------------------------
//
//   Name | Name(payload) | Name { fields }
//
typedef struct { SyntaxNode *syntax; } Variant;
bool         Variant_cast(const SyntaxNode *n, Variant *out);
SyntaxToken *Variant_name(const Variant *v);
SyntaxNode  *Variant_payload(const Variant *v); // optional: SK_PARAM_LIST
                                                  // or SK_FIELD_LIST
// Optional explicit discriminant value: `Red = 5`. The expression node
// follows an SK_EQ token in the variant body. NULL if the variant has
// no explicit value (auto-numbered by the enum).
SyntaxNode  *Variant_value(const Variant *v);


#endif  // ORE_AST_DECL_H
