#ifndef ORE_DB_QUERY_FILE_IMPORTS_H
#define ORE_DB_QUERY_FILE_IMPORTS_H

// Pulls FileArray + struct db (FileArray is a typedef of an anonymous
// struct in db.h, so it can't be forward-declared the usual way).
#include "../db.h"

// One @import("path") reference found in a file's AST.
typedef struct {
    StrId     path;  // the literal string arg of @import
    AstNodeId site; // the AST_EXPR_BUILTIN node — for diagnostics
} ImportRef;

// QUERY_FILE_IMPORTS — walks the file's AST and collects every
// AST_EXPR_BUILTIN(name == "import") whose first arg is a string
// literal. Returns FileArray<ImportRef> stored in the file's arena;
// pointer is valid until the next QUERY_FILE_AST reparse.
//
// Pure: depends only on QUERY_FILE_AST. The workspace coordinator's
// discovery loop calls this to know what relative paths a file
// imports (so it can resolve + load them before sema runs).
//
// Empty FileArray for files with no imports or for FILE_ID_NONE.
FileArray *db_query_file_imports(struct db *s, FileId fid);

#endif // ORE_DB_QUERY_FILE_IMPORTS_H
