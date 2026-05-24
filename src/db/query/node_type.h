#ifndef ORE_DB_QUERY_NODE_TYPE_H
#define ORE_DB_QUERY_NODE_TYPE_H

#include "../db.h"

// db_query_node_type — the unified node-to-type router.
//
// Given a position in a file (FileId + AstNodeId), returns the resolved
// IpIndex for that node. Internally:
//   1. Walks parents to find the innermost enclosing top-level def.
//   2. Drives the per-decl queries that own ranges of node types
//      (db_query_infer_body / db_query_fn_signature / db_query_type_of_def
//      for struct field types). Salsa-cached; cheap on repeat calls.
//   3. Looks up the node in the matching NodeTypesRange via
//      sema_node_types_range_lookup.
//
// This replaces the legacy direct read of FileNodeData.types[node.idx]
// from hover.c / completion.c. The legacy field is being demolished in
// the Option-C migration (see plan file); during the bridge period both
// paths populate but readers should prefer this router.
//
// Returns IP_NONE for:
//   - invalid FileId / AstNodeId
//   - node not in any classified decl's range (e.g., module-level
//     trivia, an unclassified outermost ancestor)
//   - cycle / build-failed decls whose range is empty
IpIndex db_query_node_type(struct db *s, FileId fid, AstNodeId node);

#endif // ORE_DB_QUERY_NODE_TYPE_H
