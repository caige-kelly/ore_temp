#ifndef ORE_DB_QUERY_INDEX_H
#define ORE_DB_QUERY_INDEX_H

#include "query.h"

struct db;

// QUERY_TOP_LEVEL_INDEX — the module's top-level index. A DERIVED
// aggregation over the module's files: it parses each (recording a
// QUERY_FILE_AST dep) and folds each file's identity + every top-level
// decl's (name, meta, ast_id) into the slot fingerprint.
//
// This query IS the module-composition observation: its fingerprint
// changes when the file set changes or any decl's shape changes. Any
// query that reads the module's file list (db_get_namespace_files) must
// record a dep on it — call this — or it will not invalidate when a
// file is added to / removed from the module.
Fingerprint db_query_top_level_index(struct db *s, NamespaceId mod);

#endif // ORE_DB_QUERY_INDEX_H
