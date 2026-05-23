#ifndef ORE_DB_QUERY_TYPE_OF_DEF_H
#define ORE_DB_QUERY_TYPE_OF_DEF_H

#include "../db.h"
#include "../intern_pool/intern_pool.h"

// Type of a top-level decl. Reads the decl's RHS via
// defs.ast_ids[def] + the owning module's files' ast_id_maps. Records
// deps on def_identity(nsid, ast_id) and file_ast(fid) for each file
// walked.
//
// Chunk 1 scope: literal-RHS const/var binds WITHOUT a type annotation
// map to the IP_* type of the literal (INT → IP_COMPTIME_INT_TYPE, etc.).
// Everything else (typed binds, non-literal RHS, fn/struct/enum decls)
// returns IP_NONE — filled in by later chunks (2 = type expressions,
// 3 = aggregates, 4 = fn signatures, 5 = expr types, …).
IpIndex db_query_type_of_def(struct db *s, DefId def);

#endif // ORE_DB_QUERY_TYPE_OF_DEF_H
