#ifndef ORE_DB_QUERY_AST_H
#define ORE_DB_QUERY_AST_H

#include "../db.h"
#include "query_engine.h"

// Lexes, layouts, and parses one file, then flattens its side-tables
// into the db.files SoA columns. Keyed by FileId — editing one file
// reparses only its row. Returns the durable fingerprint.
Fingerprint db_query_file_ast(struct db *s, FileId fid);

#endif // ORE_DB_QUERY_AST_H
