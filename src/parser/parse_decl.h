#ifndef ORE_PARSE_DECL_H
#define ORE_PARSE_DECL_H

#include "./parser.h"

// Parse a single top-level or scoped declaration
AstNodeId parse_decl(Parser *p);

// Parses all top-level declarations and creates the root AST_DECL_MODULE node.
void parse_top_level_decls(Parser *p);

// Parse a type expression
AstNodeId parse_type(Parser *p);

#endif // ORE_PARSE_DECL_H
