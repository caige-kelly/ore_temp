#ifndef ORE_DB_QUERY_DECL_AST_H
#define ORE_DB_QUERY_DECL_AST_H

#include "../ids/ids.h"

struct db;

// QUERY_DECL_AST — per-decl AST handle, keyed by (FileId, AstId).
//
// Depends on QUERY_FILE_AST(fid) — so it re-runs on any edit to the
// file — but its fingerprint is a POSITION-INDEPENDENT structural hash
// of just this decl's AST subtree (ast_subtree_fingerprint). Editing a
// sibling decl re-runs this query but reproduces the same fingerprint,
// so this decl's sema consumers (type_of_def / fn_signature /
// infer_body / body_scopes) early-cut.
//
// Returns the decl's current AstNodeId, or AST_NODE_ID_NONE when the
// ast_id is not present in the file.
AstNodeId db_query_decl_ast(struct db *s, FileId fid, AstId ast_id);

#endif // ORE_DB_QUERY_DECL_AST_H
