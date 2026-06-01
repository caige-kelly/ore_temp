#ifndef ORE_DB_DIAG_AST_ID_H
#define ORE_DB_DIAG_AST_ID_H

// Phase P — body-relative AstId infrastructure.
//
// BodyAstIdMap decouples body-anchored diagnostic emission from
// absolute byte offsets. A SemaCtx inside an INFER_BODY frame has a
// SyntaxNode in hand; rev hashes that node to its preorder index
// inside the body (RelAstId). At publish time, body_ast_id_resolve
// walks the CURRENT body subtree in preorder and returns the i-th
// node — the preorder index is structurally invariant under salsa
// cutoff (cutoff means the body's green structure is unchanged, so
// the same preorder walk visits the same nodes in the same order).
//
// That's the architectural property that makes cached body-anchored
// diagnostics survive sibling reparses without becoming "sticky"
// against the prior byte offsets.

#include "../../support/data_structure/hashmap.h"
#include "../../syntax/syntax.h"  // SyntaxNode

// rev (SyntaxNodePtr-hash → RelAstId+1) is the ONLY consumer of the
// map at emit time. The map is built on the COMPUTE path of
// db_query_body_scopes; on the cached path it is intentionally NOT
// refreshed (INFER_BODY cuts off in lockstep, so no fresh emits
// happen). next_id is the monotonic counter the walker advances; it
// always equals rev.count after each push.
//
// Resolution at publish time does NOT consult any per-map state —
// body_ast_id_resolve walks the live tree from scratch and returns
// the RelAstId-th node. The map is opaque to publishers.
typedef struct BodyAstIdMap {
    HashMap  rev;      // SyntaxNodePtr-hash → RelAstId+1 (emit-time lookup)
    uint32_t next_id;  // monotonic counter; equals rev.count after each push
} BodyAstIdMap;

// Lifecycle: explicit init / free since the HashMap holds heap.
// Init leaves the struct in a valid empty state.
void body_ast_id_map_init(BodyAstIdMap *m);
void body_ast_id_map_free(BodyAstIdMap *m);

// Emit-time SyntaxNode → RelAstId lookup. Returns false on miss
// (e.g. a node from a sub-query that walked outside the body
// subtree); caller falls back to a FILE_RAW anchor. RelAstId lives
// in diag.h; we use uint32_t * here to avoid forcing this header to
// include diag.h.
bool body_ast_id_lookup(const BodyAstIdMap *m, const SyntaxNode *node,
                        uint32_t *out_rel);

#endif // ORE_DB_DIAG_AST_ID_H
