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


// ---- ConstDef (SK_CONST_DECL) ---------------------------------------
//
//   const name: type = value
//   const name := value          (type inferred)
//
typedef struct { SyntaxNode *syntax; } ConstDef;
bool         ConstDef_cast(const SyntaxNode *n, ConstDef *out);
SyntaxToken *ConstDef_name(const ConstDef *c);
SyntaxNode  *ConstDef_type(const ConstDef *c);    // optional
SyntaxNode  *ConstDef_value(const ConstDef *c);   // an expression node


// ---- VarDef (SK_VAR_DECL) -------------------------------------------
//
//   name: type = value
//   name := value
//
typedef struct { SyntaxNode *syntax; } VarDef;
bool         VarDef_cast(const SyntaxNode *n, VarDef *out);
SyntaxToken *VarDef_name(const VarDef *v);
SyntaxNode  *VarDef_type(const VarDef *v);        // optional
SyntaxNode  *VarDef_value(const VarDef *v);       // optional


// ---- ImportDef (SK_IMPORT_DECL) -------------------------------------
//
//   import path
//
typedef struct { SyntaxNode *syntax; } ImportDef;
bool         ImportDef_cast(const SyntaxNode *n, ImportDef *out);
SyntaxNode  *ImportDef_path(const ImportDef *i); // SK_PATH_EXPR or similar


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
