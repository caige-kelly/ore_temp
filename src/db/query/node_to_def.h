#ifndef ORE_DB_QUERY_NODE_TO_DEF_H
#define ORE_DB_QUERY_NODE_TO_DEF_H

#include "query.h"

struct db;

// QUERY_NODE_TO_DECL — the per-file node→DefId reverse index.
//
// Stamps each top-level decl's AstNodeId with its canonical DefId into
// the file's ModuleNodeData.defs array; db_get_def_for_node walks the
// parent chain to that array to map any node to its enclosing decl.
//
// Keyed by FileId. A first-class query (not a side effect of
// module_exports): module_exports now early-cuts on body-only edits, so
// it can no longer be relied on to re-stamp the index. This query deps
// on the file's QUERY_FILE_AST (node→ast_id is pure AST) and on
// QUERY_DEF_IDENTITY per top-level decl (ast_id→DefId), so it re-runs
// exactly when the file reparses or a decl's identity changes.
//
// Returns true on success. The "result" is the stamped defs array;
// callers read it via db_get_def_for_node.
bool db_query_node_to_def(struct db *s, FileId fid);

#endif // ORE_DB_QUERY_NODE_TO_DEF_H
