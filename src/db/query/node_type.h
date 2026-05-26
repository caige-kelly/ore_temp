#ifndef ORE_DB_QUERY_NODE_TYPE_H
#define ORE_DB_QUERY_NODE_TYPE_H

#include "../db.h"

// db_query_node_type — the unified node-to-type router.
//
// Given a SyntaxNode in a file, returns the resolved IpIndex for that
// node. Internally:
//   1. Walks SyntaxNode parents to find the innermost enclosing
//      top-level def (via files.node_to_def sparse HashMap).
//   2. Drives the per-decl queries that own per-body type tables
//      (db_query_infer_body / db_query_fn_signature / db_query_type_of_def
//      for struct field types). Salsa-cached; cheap on repeat calls.
//   3. Looks up the node in the matching per-body
//      HashMap<SyntaxNodePtr, IpIndex> via sema_node_types_range_lookup.
//
// Returns IP_NONE for:
//   - NULL node / invalid FileId
//   - node not in any classified decl's body (e.g. module-level trivia)
//   - cycle / build-failed decls whose table is empty
IpIndex db_query_node_type(struct db *s, FileId fid, SyntaxNode *node);

#endif // ORE_DB_QUERY_NODE_TYPE_H
