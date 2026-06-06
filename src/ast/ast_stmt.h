#ifndef ORE_AST_STMT_H
#define ORE_AST_STMT_H

#include "./ast.h"


// ---- BlockStmt (SK_BLOCK_STMT) --------------------------------------
//
//   { stmt; stmt; ... }
//
typedef struct { SyntaxNode *syntax; } BlockStmt;
bool        BlockStmt_cast(const SyntaxNode *n, BlockStmt *out);
SyntaxNode *BlockStmt_stmts(const BlockStmt *b);  // SK_STMT_LIST


// ---- ReturnStmt (SK_RETURN_STMT) ------------------------------------
//
//   return value
//
typedef struct { SyntaxNode *syntax; } ReturnStmt;
bool        ReturnStmt_cast(const SyntaxNode *n, ReturnStmt *out);
SyntaxNode *ReturnStmt_value(const ReturnStmt *r);  // optional


// (No IfStmt/LoopStmt/SwitchStmt wrappers — `if`/`loop`/`switch` are
// EXPRESSION forms: see ast_expr.h's IfExpr / LoopExpr / SwitchExpr.)


// ---- SwitchArm (SK_SWITCH_ARM) --------------------------------------
//
//   pat (, pat)* => body        (patterns wrapped in SK_SWITCH_PATTERN_LIST)
//
typedef struct { SyntaxNode *syntax; } SwitchArm;
bool        SwitchArm_cast(const SyntaxNode *n, SwitchArm *out);
SyntaxNode *SwitchArm_patterns(const SwitchArm *a); // SK_SWITCH_PATTERN_LIST
SyntaxNode *SwitchArm_pattern(const SwitchArm *a);  // first pattern (in the list)
SyntaxNode *SwitchArm_body   (const SwitchArm *a);  // node after `=>`


// ---- BreakStmt (SK_BREAK_STMT) --------------------------------------
//
//   break [label]
//
typedef struct { SyntaxNode *syntax; } BreakStmt;
bool         BreakStmt_cast(const SyntaxNode *n, BreakStmt *out);
SyntaxToken *BreakStmt_label(const BreakStmt *b);    // optional


// ---- ContinueStmt (SK_CONTINUE_STMT) --------------------------------
//
//   continue [label]
//
typedef struct { SyntaxNode *syntax; } ContinueStmt;
bool         ContinueStmt_cast(const SyntaxNode *n, ContinueStmt *out);
SyntaxToken *ContinueStmt_label(const ContinueStmt *c);


// ---- DeferStmt (SK_DEFER_STMT) --------------------------------------
//
//   defer expr
//
typedef struct { SyntaxNode *syntax; } DeferStmt;
bool        DeferStmt_cast(const SyntaxNode *n, DeferStmt *out);
SyntaxNode *DeferStmt_body(const DeferStmt *d);


// ---- ExprStmt (SK_EXPR_STMT) ----------------------------------------
//
//   expr;
//
typedef struct { SyntaxNode *syntax; } ExprStmt;
bool        ExprStmt_cast(const SyntaxNode *n, ExprStmt *out);
SyntaxNode *ExprStmt_expr(const ExprStmt *e);


#endif  // ORE_AST_STMT_H
