#ifndef ORE_DB_DIAG_AST_ID_H
#define ORE_DB_DIAG_AST_ID_H

// Phase P (Phase-3.1 follow-up) — decl-wrapper-relative AstId infrastructure.
//
// `DeclAstIdMap` decouples decl-anchored diagnostic emission from
// absolute byte offsets. At TYPE_OF_DECL compute time, the producer
// preorder-walks the entire decl WRAPPER (signature + body for fns,
// the whole decl for non-fns) and records each SyntaxNode's preorder
// position as a `RelAstId`. At emit time inside any cached query
// (TYPE_OF_DECL / FN_SIGNATURE / INFER_BODY / BODY_SCOPES), `rev`
// hashes a SyntaxNode to its RelAstId. At publish time,
// `decl_ast_id_resolve` walks the CURRENT wrapper subtree in
// preorder and returns the i-th node.
//
// The preorder index is structurally invariant under salsa cutoff:
// if the wrapper's green structure is unchanged, the same preorder
// walk visits the same nodes in the same order. That's the
// architectural property that makes cached decl-anchored
// diagnostics survive sibling reparses without going stale against
// frozen byte offsets.
//
// Why per-decl wrapper, not just per-body: post-Phase-3.1, every
// query that records a TOP_LEVEL_ENTRY dep caches across edits. Any
// FILE_RAW anchor those queries emit goes stale on byte-shifting
// sibling edits — see docs/diag-anchor-audit.md.

#include "../../support/data_structure/hashmap.h"
#include "../../syntax/syntax.h"  // SyntaxNode

// `rev` (SyntaxNodePtr-hash → RelAstId+1) is the ONLY consumer of
// the map at emit time. The map is built on the COMPUTE path of
// TYPE_OF_DECL; on the cached path the map is intentionally NOT
// refreshed (downstream queries cut off in lockstep, so no fresh
// emits happen). `next_id` is the monotonic counter the walker
// advances; always equals rev.count after each push.
//
// Resolution at publish time does NOT consult any per-map state —
// `decl_ast_id_resolve` walks the live wrapper subtree from scratch
// and returns the RelAstId-th node. The map is opaque to publishers.
typedef struct DeclAstIdMap {
    HashMap  rev;      // SyntaxNodePtr-hash → RelAstId+1 (emit-time lookup)
    uint32_t next_id;  // monotonic counter; equals rev.count after each push
} DeclAstIdMap;

// Lifecycle: explicit init / free since the HashMap holds heap.
// Init leaves the struct in a valid empty state.
void decl_ast_id_map_init(DeclAstIdMap *m);
void decl_ast_id_map_free(DeclAstIdMap *m);

// Emit-time SyntaxNode → RelAstId lookup. Returns false on miss
// (e.g. a synthetic node never visited by the wrapper walk, or a
// stale lookup against an evicted map); caller falls back to a
// FILE_RAW anchor. RelAstId lives in diag.h; we use uint32_t * here
// to avoid forcing this header to include diag.h.
bool decl_ast_id_lookup(const DeclAstIdMap *m, const SyntaxNode *node,
                        uint32_t *out_rel);

#endif // ORE_DB_DIAG_AST_ID_H
