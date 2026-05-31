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

typedef struct FileAstIdMap {
    Vec     ptrs;    // Vec<SyntaxNodePtr> — preorder walk over file-scoped
                     // nodes (decls/items/SK_ERROR). FileAstId is the index.
    HashMap rev;     // SyntaxNodePtr-hash → FileAstId — for emit-time lookup
                     // (sema needs to translate a SyntaxNode it has in hand
                     //  into the matching FileAstId).
} FileAstIdMap;

// rev (SyntaxNodePtr-hash → RelAstId+1) is the ONLY consumer at
// emit time — sema has a SyntaxNode in hand and needs the
// matching RelAstId. The map is built on the COMPUTE path of
// db_query_body_scopes; on the cached path it is intentionally NOT
// refreshed, because nothing reads it under those conditions: if
// body_scopes cut off, INFER_BODY also cut off (it depends on
// body_scopes' fingerprint), so no fresh emits happen this revision.
//
// Resolution at publish time does NOT consult ptrs — RelAstId is the
// preorder index of the node inside the body subtree, and that index
// is structurally invariant under salsa cutoff (cutoff means the
// body's green structure is unchanged, so the same preorder walk
// visits the same nodes in the same order). See body_ast_id_resolve
// in src/db/getters/diag.c: it walks the CURRENT body subtree
// preorder and returns the i-th node. ptrs is kept anyway for
// debugging and for the kind-match assertion in the keep-zone test.
typedef struct BodyAstIdMap {
    Vec      ptrs;    // Vec<SyntaxNodePtr> — preorder over the body subtree.
                      // RelAstId is the index.
    HashMap  rev;     // SyntaxNodePtr-hash → RelAstId+1 (emit-time lookup)
} BodyAstIdMap;

// Lifecycle: explicit init / free since both structs hold heap (Vec +
// HashMap data). Init leaves the struct in a valid empty state.
void file_ast_id_map_init(FileAstIdMap *m);
void file_ast_id_map_free(FileAstIdMap *m);
void body_ast_id_map_init(BodyAstIdMap *m);
void body_ast_id_map_free(BodyAstIdMap *m);

// Phase P S4 — emit-time SyntaxNode → RelAstId / FileAstId lookups.
// Sema has a node in hand inside a sink-owning frame and needs the
// matching id to build a DIAG_ANCHOR_BODY / DIAG_ANCHOR_FILE anchor.
// Returns false on miss (caller falls back to a non-AstId anchor —
// e.g. a node from a sub-query that walked outside the body subtree).
// Types FileAstId / RelAstId live in diag.h; we use uint32_t * here
// to avoid forcing this header to include diag.h.
bool body_ast_id_lookup(const BodyAstIdMap *m, const SyntaxNode *node,
                        uint32_t *out_rel);
bool file_ast_id_lookup(const FileAstIdMap *m, const SyntaxNode *node,
                        uint32_t *out_file_id);

#endif // ORE_DB_DIAG_AST_ID_H
