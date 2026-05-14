#ifndef ORE_PARSE_STMT_H
#define ORE_PARSE_STMT_H

#include "./parser.h"

// Parse a single statement.
AstNodeId parse_stmt(Parser *p);

// Parse a block `{ stmt; stmt; }`.
AstNodeId parse_block(Parser *p);

#endif // ORE_PARSE_STMT_H
