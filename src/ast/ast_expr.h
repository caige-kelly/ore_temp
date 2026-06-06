#ifndef ORE_AST_EXPR_H
#define ORE_AST_EXPR_H

// =====================================================================
// Typed wrappers for expression nodes.
// =====================================================================
//
// Operator-collapsed nodes (BinExpr, AssignExpr, PrefixExpr,
// PostfixExpr, Literal) expose `<Type>_op_kind` returning the
// SyntaxKind of the op token. Use [syntax_kind.h](../syntax/syntax_kind.h)
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

// ---- ComptimeExpr (SK_COMPTIME_EXPR) --------------------------------
//
//   comptime <inner>
//
// Sema uses this marker to route through sema_comptime_select: for
// comptime if/switch it picks the live arm via const_eval; for any
// other inner kind it forces the expression to const-fold.
//
typedef struct { SyntaxNode *syntax; } ComptimeExpr;
bool        ComptimeExpr_cast(const SyntaxNode *n, ComptimeExpr *out);
SyntaxNode *ComptimeExpr_inner(const ComptimeExpr *c);  // first expr child


// ---- Literal (SK_LITERAL_EXPR) --------------------------------------
//
//   Holds one literal token: int / float / string / byte / asm /
//   true / false / nil. The token kind disambiguates.
//
typedef struct { SyntaxNode *syntax; } Literal;
bool         Literal_cast(const SyntaxNode *n, Literal *out);
SyntaxToken *Literal_token(const Literal *l);
SyntaxKind   Literal_kind(const Literal *l);

// RETURNS_BORROWED text. If `n` is a string-literal expression
// (SK_LITERAL_EXPR wrapping SK_STRING_LIT), sets *out_text / *out_len to
// the literal's content with surrounding double-quotes stripped, and
// returns true. The text is owned by `n`'s green tree — valid only while
// that tree is alive; callers needing it longer must copy/intern. Any
// other node returns false (out params untouched). Shared by @import path
// extraction (file_imports + sema/builtins.c) so the quote-stripping
// lives in one place.
bool         ast_string_literal_text(const SyntaxNode *n,
                                      const char **out_text, uint32_t *out_len);


// ---- IfExpr (SK_IF_EXPR) --------------------------------------------
//
//   if (cond) then [else else_branch]
//   if (opt)  <x> then [else else_branch]   -- capture-unwrap form
//
typedef struct { SyntaxNode *syntax; } IfExpr;
bool        IfExpr_cast(const SyntaxNode *n, IfExpr *out);
SyntaxNode *IfExpr_condition(const IfExpr *i);    // first expr child
SyntaxNode *IfExpr_capture(const IfExpr *i);      // optional SK_CAPTURE child
SyntaxNode *IfExpr_then_branch(const IfExpr *i);  // first block child
SyntaxNode *IfExpr_else_branch(const IfExpr *i);  // optional, second block


// ---- LoopExpr (SK_LOOP_EXPR) ----------------------------------------
//
//   loop body                       -- infinite
//   loop (bool_cond) body           -- while-style
//   loop (opt_cond) <x> body        -- while-let: iterates while cond non-nil
//   loop (range)    <i> body        -- range iteration; `i` is the index
//
typedef struct { SyntaxNode *syntax; } LoopExpr;
bool        LoopExpr_cast(const SyntaxNode *n, LoopExpr *out);
SyntaxNode *LoopExpr_condition(const LoopExpr *l); // optional cond expr
SyntaxNode *LoopExpr_capture(const LoopExpr *l);   // optional SK_CAPTURE child
SyntaxNode *LoopExpr_body(const LoopExpr *l);
SyntaxNode *LoopExpr_continue(const LoopExpr *l);    // optional SK_LOOP_CONTINUE
SyntaxNode *LoopExpr_else_branch(const LoopExpr *l); // optional block after `else`


// ---- Capture (SK_CAPTURE) ------------------------------------------
//
//   <ident>   -- wraps the angle brackets + the bound-name token. The
//                bind_site for body_scopes / per-node type entries for
//                infer / hover target lookups all use this wrapping
//                node, so the unwrapped value's type lives in standard
//                node-keyed storage.
//
typedef struct { SyntaxNode *syntax; } Capture;
bool         Capture_cast(const SyntaxNode *n, Capture *out);
SyntaxToken *Capture_name(const Capture *c);


// ---- SwitchExpr (SK_SWITCH_EXPR) — the `switch` expression -----------
//
//   switch (scrutinee) { pattern => body ... }
//
typedef struct { SyntaxNode *syntax; } SwitchExpr;
bool        SwitchExpr_cast(const SyntaxNode *n, SwitchExpr *out);
SyntaxNode *SwitchExpr_scrutinee(const SwitchExpr *m); // first expr child
SyntaxNode *SwitchExpr_arms(const SwitchExpr *m);      // SK_STMT_LIST of arms


// ---- LambdaExpr (SK_LAMBDA_EXPR / SK_CTL_LAMBDA / SK_FINAL_CTL_LAMBDA)
//
//   fn(params) [<eff>] [-> T] body          (SK_LAMBDA_EXPR)
//   ctl(params) [<eff>] [-> T] body         (SK_CTL_LAMBDA)
//   final-ctl(params) [<eff>] [-> T] body   (SK_FINAL_CTL_LAMBDA)
//
// One wrapper for all three lambda-shaped kinds — same accessors; the
// node kind is the op-sort (fn value vs ctl op vs final-ctl op). Read the
// sort via syntax_node_kind(); use ore_kind_is_lambda() to test. The
// cast accepts any of the three (see ast_expr.c).
typedef struct { SyntaxNode *syntax; } LambdaExpr;
bool        LambdaExpr_cast(const SyntaxNode *n, LambdaExpr *out);
SyntaxNode *LambdaExpr_params(const LambdaExpr *l);  // SK_PARAM_LIST
SyntaxNode *LambdaExpr_effect_row(const LambdaExpr *l);  // optional SK_EFFECT_ROW_TYPE
// Optional return-type node sitting between the param list (and any
// SK_EFFECT_ROW_TYPE) and the body block. NULL when the lambda has no
// annotated return type. Disambiguated by position: first node child
// AFTER SK_PARAM_LIST + optional SK_EFFECT_ROW_TYPE, BEFORE the body
// block (SK_BLOCK_STMT).
SyntaxNode *LambdaExpr_return_type(const LambdaExpr *l);
SyntaxNode *LambdaExpr_body(const LambdaExpr *l);    // SK_BLOCK_STMT


// ---- HandleExpr (SK_HANDLE_EXPR) ------------------------------------
//
//   [named|override] handle [scoped] [<row>] (action) { clauses... }
//
// Nested shape: SK_HANDLE_EXPR wraps an inner SK_HANDLER_EXPR that
// holds just the clauses (no keyword/modifiers/row of its own).
// `.body()` returns the action expression; `.handler()` returns the
// nested SK_HANDLER_EXPR.
//
typedef struct { SyntaxNode *syntax; } HandleExpr;
bool        HandleExpr_cast(const SyntaxNode *n, HandleExpr *out);
SyntaxNode *HandleExpr_body(const HandleExpr *h);     // action expr
SyntaxNode *HandleExpr_handler(const HandleExpr *h);  // nested SK_HANDLER_EXPR
SyntaxNode *HandleExpr_effect(const HandleExpr *h);   // optional SK_EFFECT_ROW_TYPE


// ---- HandlerExpr (SK_HANDLER_EXPR) ----------------------------------
//
//   [named|override] handler [scoped] [<row>] { clauses... }
//
// May appear standalone (with the handler keyword) or nested inside a
// SK_HANDLE_EXPR (without the keyword — just the clause body). The
// `kw()` accessor returns NULL for nested handlers; sema should not
// rely on it for ownership of effect-row/scoped.
//
typedef struct { SyntaxNode *syntax; } HandlerExpr;
bool        HandlerExpr_cast(const SyntaxNode *n, HandlerExpr *out);
SyntaxNode *HandlerExpr_effect(const HandlerExpr *h);          // optional SK_EFFECT_ROW_TYPE
SyntaxNode *HandlerExpr_first_clause(const HandlerExpr *h);    // first SK_*_CLAUSE child;
                                                                 // walk siblings for the rest


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


// ---- Handler clauses -----------------------------------------------
//
// Children of SK_HANDLER_EXPR / SK_HANDLE_EXPR's nested handler. The
// op-kind keyword (`fn`/`ctl`/`val`/`final ctl`/`raw ctl`) is preserved
// as a child token of SK_OP_CLAUSE; `OpClause_sort_kind` walks children
// to find it.

// `name :: [pub] (fn|ctl|val|final ctl|raw ctl) (params) body`.
typedef struct { SyntaxNode *syntax; } OpClause;
bool         OpClause_cast(const SyntaxNode *n, OpClause *out);
SyntaxToken *OpClause_name(const OpClause *c);          // first SK_IDENT
SyntaxKind   OpClause_sort_kind(const OpClause *c);     // SK_FN_KW, or
                                                          // for ctl/val/final/raw
                                                          // returns SYNTAX_KIND_NONE —
                                                          // sema does a TOK_IS walk
                                                          // (contextual kw)
SyntaxNode  *OpClause_params(const OpClause *c);        // optional SK_PARAM_LIST
SyntaxNode  *OpClause_body(const OpClause *c);          // first expr child after params

// `return (params) { body }` — the handler's return-continuation slot.
typedef struct { SyntaxNode *syntax; } ReturnClause;
bool         ReturnClause_cast(const SyntaxNode *n, ReturnClause *out);
SyntaxNode  *ReturnClause_params(const ReturnClause *c);  // optional SK_PARAM_LIST
SyntaxNode  *ReturnClause_body(const ReturnClause *c);    // expression body

// `initially expr` — the handler's initially lifecycle clause.
typedef struct { SyntaxNode *syntax; } InitiallyClause;
bool         InitiallyClause_cast(const SyntaxNode *n, InitiallyClause *out);
SyntaxNode  *InitiallyClause_body(const InitiallyClause *c);

// `finally expr` — the handler's finally lifecycle clause.
typedef struct { SyntaxNode *syntax; } FinallyClause;
bool         FinallyClause_cast(const SyntaxNode *n, FinallyClause *out);
SyntaxNode  *FinallyClause_body(const FinallyClause *c);


#endif  // ORE_AST_EXPR_H
