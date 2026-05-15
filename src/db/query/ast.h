#ifndef ORE_DB_QUERY_AST_H
#define ORE_DB_QUERY_AST_H

#include "../db.h"
#include "query_engine.h"

// Lexes, layouts, and parses a module, then flattens its side-tables
// into the db.modules SoA columns. Returns the durable fingerprint.
Fingerprint db_query_module_ast(struct db *s, ModuleId mod);

#endif // ORE_DB_QUERY_AST_H
