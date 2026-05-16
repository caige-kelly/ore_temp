#ifndef ORE_PARSE_DECL_H
#define ORE_PARSE_DECL_H

#include "./parser.h"

// Drive the top-level loop and build the root AST_DECL_MODULE node.
// There is no separate decl parser — binds are a guarded Pratt infix
// (parse_expr.c); this only lifts each bind into mod->top_level_index.
void parse_top_level_decls(Parser *p);

#endif // ORE_PARSE_DECL_H
