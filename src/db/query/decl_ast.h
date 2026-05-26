#ifndef ORE_DB_QUERY_DECL_AST_H
#define ORE_DB_QUERY_DECL_AST_H

#include "../ids/ids.h"
#include "../../syntax/syntax.h"

struct db;

// QUERY_DECL_AST — per-decl green-tree handle, keyed by
// (FileId, SyntaxNodePtr).
//
// Depends on QUERY_FILE_AST(fid) — so it re-runs on any edit to the
// file — but its fingerprint is a POSITION-INDEPENDENT structural hash
// of just this decl's green-tree subtree (trivia-stripped, computed
// via the hash_subtree_no_trivia helper). Editing a sibling decl
// re-runs this query but reproduces the same fingerprint, so this
// decl's sema consumers (type_of_def / fn_signature / infer_body /
// body_scopes) early-cut.
//
// Returns the decl's current SyntaxNodePtr (a stable reparse-anchor),
// or a zero-initialized SyntaxNodePtr when node_ptr is not present in
// the file's current green tree.
SyntaxNodePtr db_query_decl_ast(struct db *s, FileId fid,
                                SyntaxNodePtr node_ptr);

#endif // ORE_DB_QUERY_DECL_AST_H
