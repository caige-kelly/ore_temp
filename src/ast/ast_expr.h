#ifndef ORE_AST_EXPR_H
#define ORE_AST_EXPR_H

// =====================================================================
// Typed wrappers for expression nodes.
// =====================================================================
//
// Operator-collapsed nodes (BinExpr, AssignExpr, PrefixExpr,
// PostfixExpr, Literal) expose `<Type>_op_kind` returning the
// SyntaxKind of the op token. Use [syntax_kind.h](../parser/syntax_kind.h)
// classifiers (`ore_kind_is_bin_op_token`, etc.) to verify or dispatch
// further.

#include "./ast.h"


// ---- BinExpr (SK_BIN_EXPR) ------------------------------------------
//
//   lhs <op> rhs
//
typedef struct { SyntaxNode *syntax; } BinExpr;
bool         BinExpr_cast(const SyntaxNode *n, BinExpr *out);
SyntaxNode  *BinExpr_lhs(const BinExpr *b);     // first expr child
SyntaxNode  *BinExpr_rhs(const BinExpr *b);     // second expr child
SyntaxToken *BinExpr_op(const BinExpr *b);      // the bin-op token
SyntaxKind   BinExpr_op_kind(const BinExpr *b); // SK_PLUS, SK_STAR, ...
                                                  // SK_NONE if missing


// ---- AssignExpr (SK_ASSIGN_EXPR) ------------------------------------
//
//   lhs <op>= rhs   (or := / =)
//
typedef struct { SyntaxNode *syntax; } AssignExpr;
bool         AssignExpr_cast(const SyntaxNode *n, AssignExpr *out);
SyntaxNode  *AssignExpr_lhs(const AssignExpr *a);
SyntaxNode  *AssignExpr_rhs(const AssignExpr *a);
SyntaxToken *AssignExpr_op(const AssignExpr *a);
SyntaxKind   AssignExpr_op_kind(const AssignExpr *a);


// ---- PrefixExpr (SK_PREFIX_EXPR) ------------------------------------
//
//   <op> operand     e.g. -x, !x, ~x, &x
//
typedef struct { SyntaxNode *syntax; } PrefixExpr;
bool         PrefixExpr_cast(const SyntaxNode *n, PrefixExpr *out);
SyntaxToken *PrefixExpr_op(const PrefixExpr *p);
SyntaxKind   PrefixExpr_op_kind(const PrefixExpr *p);
SyntaxNode  *PrefixExpr_operand(const PrefixExpr *p);


// ---- PostfixExpr (SK_POSTFIX_EXPR) ----------------------------------
//
//   operand <op>     e.g. x++, x--, x^, x?
//
typedef struct { SyntaxNode *syntax; } PostfixExpr;
bool         PostfixExpr_cast(const SyntaxNode *n, PostfixExpr *out);
SyntaxNode  *PostfixExpr_operand(const PostfixExpr *p);
SyntaxToken *PostfixExpr_op(const PostfixExpr *p);
SyntaxKind   PostfixExpr_op_kind(const PostfixExpr *p);


// ---- CallExpr (SK_CALL_EXPR) ----------------------------------------
//
//   callee(args...)
//
typedef struct { SyntaxNode *syntax; } CallExpr;
bool         CallExpr_cast(const SyntaxNode *n, CallExpr *out);
SyntaxNode  *CallExpr_callee(const CallExpr *c);  // first expr child
SyntaxNode  *CallExpr_args(const CallExpr *c);    // SK_ARG_LIST


// ---- IndexExpr (SK_INDEX_EXPR) --------------------------------------
//
//   base[index]
//
typedef struct { SyntaxNode *syntax; } IndexExpr;
bool         IndexExpr_cast(const SyntaxNode *n, IndexExpr *out);
SyntaxNode  *IndexExpr_base(const IndexExpr *i);   // first expr child
SyntaxNode  *IndexExpr_index(const IndexExpr *i);  // second expr child


// ---- SliceExpr (SK_SLICE_EXPR) --------------------------------------
//
//   base[lo..hi]
//
typedef struct { SyntaxNode *syntax; } SliceExpr;
bool         SliceExpr_cast(const SyntaxNode *n, SliceExpr *out);
SyntaxNode  *SliceExpr_base(const SliceExpr *s);
SyntaxNode  *SliceExpr_lo(const SliceExpr *s);     // optional
SyntaxNode  *SliceExpr_hi(const SliceExpr *s);     // optional


// ---- FieldExpr (SK_FIELD_EXPR) --------------------------------------
//
//   base.field
//
typedef struct { SyntaxNode *syntax; } FieldExpr;
bool         FieldExpr_cast(const SyntaxNode *n, FieldExpr *out);
SyntaxNode  *FieldExpr_base(const FieldExpr *f);
SyntaxToken *FieldExpr_field(const FieldExpr *f);


// ---- PathExpr (SK_PATH_EXPR) ----------------------------------------
//
//   foo::bar::baz
//
typedef struct { SyntaxNode *syntax; } PathExpr;
bool      PathExpr_cast(const SyntaxNode *n, PathExpr *out);
// PathExpr_segments would return an iterator — callers walk
// children of kind SK_IDENT manually for now. (Sema dispatch uses
// ast_nth_child(SK_IDENT, n).)


// ---- RefExpr (SK_REF_EXPR) ------------------------------------------
//
//   name   (an identifier reference)
//
typedef struct { SyntaxNode *syntax; } RefExpr;
bool         RefExpr_cast(const SyntaxNode *n, RefExpr *out);
SyntaxToken *RefExpr_name(const RefExpr *r);


// ---- ParenExpr (SK_PAREN_EXPR) --------------------------------------
//
//   (inner)
//
typedef struct { SyntaxNode *syntax; } ParenExpr;
bool        ParenExpr_cast(const SyntaxNode *n, ParenExpr *out);
SyntaxNode *ParenExpr_inner(const ParenExpr *p);   // first expr child


// ---- Literal (SK_LITERAL_EXPR) --------------------------------------
//
//   Holds one literal token: int / float / string / byte / asm /
//   true / false / nil. The token kind disambiguates.
//
typedef struct { SyntaxNode *syntax; } Literal;
bool         Literal_cast(const SyntaxNode *n, Literal *out);
SyntaxToken *Literal_token(const Literal *l);
SyntaxKind   Literal_kind(const Literal *l);


// ---- BlockExpr (SK_BLOCK_EXPR) --------------------------------------
//
//   { stmt; stmt; tail-expr }
//
typedef struct { SyntaxNode *syntax; } BlockExpr;
bool        BlockExpr_cast(const SyntaxNode *n, BlockExpr *out);
SyntaxNode *BlockExpr_stmts(const BlockExpr *b);  // SK_STMT_LIST


// ---- IfExpr (SK_IF_EXPR) --------------------------------------------
//
//   if cond { then } else { else_branch }
//
typedef struct { SyntaxNode *syntax; } IfExpr;
bool        IfExpr_cast(const SyntaxNode *n, IfExpr *out);
SyntaxNode *IfExpr_condition(const IfExpr *i);    // first expr child
SyntaxNode *IfExpr_then_branch(const IfExpr *i);  // first block child
SyntaxNode *IfExpr_else_branch(const IfExpr *i);  // optional, second block


// ---- LoopExpr (SK_LOOP_EXPR) ----------------------------------------
//
//   loop { body }
//
typedef struct { SyntaxNode *syntax; } LoopExpr;
bool        LoopExpr_cast(const SyntaxNode *n, LoopExpr *out);
SyntaxNode *LoopExpr_body(const LoopExpr *l);


// ---- MatchExpr (SK_MATCH_EXPR) — switch-as-expression ---------------
//
//   switch scrutinee { arms... }
//
typedef struct { SyntaxNode *syntax; } MatchExpr;
bool        MatchExpr_cast(const SyntaxNode *n, MatchExpr *out);
SyntaxNode *MatchExpr_scrutinee(const MatchExpr *m); // first expr child
SyntaxNode *MatchExpr_arms(const MatchExpr *m);      // SK_STMT_LIST of arms


// ---- LambdaExpr (SK_LAMBDA_EXPR) ------------------------------------
//
//   fn (params) => body   or   |params| body
//
typedef struct { SyntaxNode *syntax; } LambdaExpr;
bool        LambdaExpr_cast(const SyntaxNode *n, LambdaExpr *out);
SyntaxNode *LambdaExpr_params(const LambdaExpr *l);  // SK_PARAM_LIST
SyntaxNode *LambdaExpr_body(const LambdaExpr *l);    // expr or block


// ---- HandleExpr (SK_HANDLE_EXPR) ------------------------------------
//
//   handle expr with handler
//
typedef struct { SyntaxNode *syntax; } HandleExpr;
bool        HandleExpr_cast(const SyntaxNode *n, HandleExpr *out);
SyntaxNode *HandleExpr_body(const HandleExpr *h);
SyntaxNode *HandleExpr_handler(const HandleExpr *h);


// ---- HandlerExpr (SK_HANDLER_EXPR) ----------------------------------
//
//   handler effect { op_handlers... }
//
typedef struct { SyntaxNode *syntax; } HandlerExpr;
bool        HandlerExpr_cast(const SyntaxNode *n, HandlerExpr *out);
SyntaxNode *HandlerExpr_effect(const HandlerExpr *h);
SyntaxNode *HandlerExpr_body(const HandlerExpr *h);


// ---- MaskExpr (SK_MASK_EXPR) ----------------------------------------
//
//   mask effect_name in body
//
typedef struct { SyntaxNode *syntax; } MaskExpr;
bool        MaskExpr_cast(const SyntaxNode *n, MaskExpr *out);
SyntaxNode *MaskExpr_effect(const MaskExpr *m);
SyntaxNode *MaskExpr_body(const MaskExpr *m);


// ---- ProductExpr (SK_PRODUCT_EXPR) ----------------------------------
//
//   Type{ field: value, ... }   or   .{ field: value, ... }
//
typedef struct { SyntaxNode *syntax; } ProductExpr;
bool        ProductExpr_cast(const SyntaxNode *n, ProductExpr *out);
SyntaxNode *ProductExpr_type(const ProductExpr *p);   // optional
SyntaxNode *ProductExpr_init(const ProductExpr *p);   // SK_INIT_LIST


// ---- EnumRefExpr (SK_ENUM_REF_EXPR) ---------------------------------
//
//   .Variant
//
typedef struct { SyntaxNode *syntax; } EnumRefExpr;
bool         EnumRefExpr_cast(const SyntaxNode *n, EnumRefExpr *out);
SyntaxToken *EnumRefExpr_variant(const EnumRefExpr *e);


// ---- BuiltinExpr (SK_BUILTIN_EXPR) ----------------------------------
//
//   @name(args...)
//
typedef struct { SyntaxNode *syntax; } BuiltinExpr;
bool         BuiltinExpr_cast(const SyntaxNode *n, BuiltinExpr *out);
SyntaxToken *BuiltinExpr_name(const BuiltinExpr *b);
SyntaxNode  *BuiltinExpr_args(const BuiltinExpr *b);  // SK_ARG_LIST


// ---- InitField (SK_INIT_FIELD) — child of SK_INIT_LIST --------------
//
//   .name = value   (struct/product initializer entry)
//
typedef struct { SyntaxNode *syntax; } InitField;
bool         InitField_cast(const SyntaxNode *n, InitField *out);
SyntaxToken *InitField_name(const InitField *i);
SyntaxNode  *InitField_value(const InitField *i);


#endif  // ORE_AST_EXPR_H
