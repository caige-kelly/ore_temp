#ifndef ORE_PARSER_NEW_PARSE_DECL_H
#define ORE_PARSER_NEW_PARSE_DECL_H

#include "./parser.h"

// Parses the body of SK_SOURCE_FILE: a sequence of top-level decls
// (binds via `::` / `:=` / `:`). Called once from parse_file_green
// after the outer SK_SOURCE_FILE start_node.
void parse_top_level_decls(Parser *p);

#endif // ORE_PARSER_NEW_PARSE_DECL_H
