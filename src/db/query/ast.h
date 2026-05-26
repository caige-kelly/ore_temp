#ifndef ORE_DB_QUERY_AST_H
#define ORE_DB_QUERY_AST_H

#include "../db.h"
#include "query_engine.h"

// Lexes, layouts, and parses one file, then flattens its side-tables
// into the db.files SoA columns. Keyed by FileId — editing one file
// reparses only its row. Returns the durable fingerprint.
Fingerprint db_query_file_ast(struct db *s, FileId fid);

// Recursive trivia-stripped structural hash of a green-tree subtree.
// Used as the durable fingerprint for QUERY_FILE_AST (over the
// SK_SOURCE_FILE root) and QUERY_DECL_AST (over a top-level decl).
// Reformatting-only edits produce identical fingerprints; structural
// changes (renaming, retyping, reordering tokens) change the fp so
// consumers re-run.
Fingerprint db_green_subtree_fingerprint(const struct GreenNode *n);

#endif // ORE_DB_QUERY_AST_H
