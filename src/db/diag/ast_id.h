#ifndef ORE_DB_DIAG_AST_ID_H
#define ORE_DB_DIAG_AST_ID_H

// Phase P P1 — AstId infrastructure.
//
// Two SoA tables that decouple diagnostic anchoring from absolute byte
// offsets. Sibling-edit reparses no longer break cached diags whose
// owning body wasn't itself recomputed (the user-reported "sticky
// squiggle" class of bug).
//
// FileAstIdMap (per file, file-relative)
//   - Built by db_query_file_ast after a successful parse.
//   - Walks the green root in preorder over the "interesting kinds"
//     allow-list: top-level decls, items, and SK_ERROR recovery
//     nodes. Indices into `ptrs` are FileAstIds.
//   - Used by file-scoped diags (parse, nameres, type-of-decl). When
//     the file's text changes, FILE_AST re-runs and rebuilds the map
//     — so file-scoped IDs only need to be valid within a single
//     parse generation.
//
// BodyAstIdMap (per fn, body-relative; paired with the stable DeclKey)
//   - Built by db_query_body_scopes immediately after the body green
//     root is fetched.
//   - Walks the body subtree in preorder (nodes only, no tokens).
//     Indices are RelAstIds.
//   - Used by body-internal diags (INFER_BODY, BODY_SCOPES). Local
//     indices align with salsa's INFER_BODY cutoff: a sibling reparse
//     doesn't invalidate cached INFER results, and the body's own
//     AstIdMap is recomputed only when INFER reruns. This is the
//     property that makes cached body-anchored diags survive sibling
//     reparses (S1 fix in the Phase P plan).
//
// Resolution helpers (file_ast_id_resolve / body_ast_id_resolve) live
// in src/db/getters/diag.c — they fetch the file's / body's current
// red root and ptr_resolve the stored SyntaxNodePtr against it.

#include "../../support/data_structure/hashmap.h"
#include "../../support/data_structure/vec.h"
#include "../../syntax/syntax.h"  // SyntaxNodePtr

typedef struct {
    Vec     ptrs;    // Vec<SyntaxNodePtr> — preorder walk over file-scoped
                     // nodes (decls/items/SK_ERROR). FileAstId is the index.
    HashMap rev;     // SyntaxNodePtr-hash → FileAstId — for emit-time lookup
                     // (sema needs to translate a SyntaxNode it has in hand
                     //  into the matching FileAstId).
} FileAstIdMap;

typedef struct {
    Vec      ptrs;    // Vec<SyntaxNodePtr> — preorder over the body subtree.
                      // RelAstId is the index.
    HashMap  rev;     // SyntaxNodePtr-hash → RelAstId
    uint64_t built_for_file_ast_fp; // Cache key: the FILE_AST fp the
                                    // recorded absolute byte ranges
                                    // were taken under. db_query_body_scopes
                                    // refreshes the map whenever this
                                    // disagrees with the file's current
                                    // FILE_AST fp — covers the case where
                                    // body_scopes salsa-cuts off but a
                                    // sibling edit shifted f's absolute
                                    // positions (S1 in Phase P plan).
} BodyAstIdMap;

// Lifecycle: explicit init / free since both structs hold heap (Vec +
// HashMap data). Init leaves the struct in a valid empty state.
void file_ast_id_map_init(FileAstIdMap *m);
void file_ast_id_map_free(FileAstIdMap *m);
void body_ast_id_map_init(BodyAstIdMap *m);
void body_ast_id_map_free(BodyAstIdMap *m);

#endif // ORE_DB_DIAG_AST_ID_H
