#ifndef ORE_DB_QUERY_TYPE_LAYER_H
#define ORE_DB_QUERY_TYPE_LAYER_H

// Type layer (Phase D2) — shared traversal context + per-decl resolved-types
// builder used by type.c (declared interface), body_scopes.c, and infer.c.
// Ported from the dissolving src/sema/sema.h; the algorithms are unchanged,
// only the dep plumbing is rewired onto the D1 query layer.

#include "../db.h"   // db, NodeTypesRange, IpIndex, DefId/FileId/NamespaceId,
                     // HashMap, Fingerprint, SyntaxNode, GreenNode

// --- NodeTypeBuilder — rust-analyzer's InferenceResult, keyed by
//     SyntaxNodePtr ----------------------------------------------------------
//
// Each per-decl query that types a sub-tree (fn_signature, type_of_def's
// struct/bind paths, infer_body) opens one of these at the top of its body and
// stamps a pointer on its SemaCtx; every type-resolving call writes into the
// map via ctx->types. builder_end seals it into a NodeTypesRange that lands on
// a db result column (the HashMap's ownership transfers).
typedef struct NodeTypeBuilder {
    FileId      file_local;
    HashMap     types;   // SyntaxNodePtr-hash → IpIndex.v
    Fingerprint fp;      // folded over the assigned types, in push order
} NodeTypeBuilder;

// --- SemaCtx — per-traversal context (RA's InferenceContext / Zig's Sema) ----
//
// Stable for one query body's recursive descent; threaded as `const SemaCtx *`
// through every recursive resolve. Cross-query boundaries construct fresh
// ctxs (each query owns its stack-local ctx + builder).
typedef struct {
    struct db        *s;
    struct GreenNode *file_green_root;  // files.green_roots[file_local]
    NamespaceId       nsid;
    DefId             enclosing_fn;     // DEF_ID_NONE outside fn bodies
    FileId            file_local;
    NodeTypeBuilder  *types;            // active builder; NULL = pushes dropped
} SemaCtx;

// Initialize the builder's HashMap. Caller sets ctx->types = b afterward.
void node_type_builder_begin(struct db *s, NodeTypeBuilder *b, FileId file_local);

// Push (node, type) onto ctx's active builder. No-op if ctx->types is NULL.
// Latest write wins. Accumulates ONLY the type value into the fp, in push
// order — position-independent (a pure trivia shift leaves it unchanged), so
// the body fp behaves like the parse-layer structural-hash firewalls.
void node_type_builder_push(const SemaCtx *ctx, SyntaxNode *node, IpIndex type);

// Seal the builder into a NodeTypesRange (which now owns the HashMap).
// `out_fp` (may be NULL) receives the accumulated fingerprint.
NodeTypesRange node_type_builder_end(NodeTypeBuilder *b, Fingerprint *out_fp);

// Look up `node`'s type in a sealed range. IP_NONE if absent / empty range.
IpIndex node_types_range_lookup(struct db *s, NodeTypesRange range,
                                SyntaxNode *node);

// Resolve a type-position syntax node to an IpIndex. Handles primitives (via
// resolve_ref → type_of_def), constructors (^T / []T / [N]T / ?T / [^]T /
// fn(…)→R), and user-defined names. Recursive visits stamp ctx's builder.
IpIndex resolve_type_expr(const SemaCtx *ctx, SyntaxNode *node);

// Body inference helpers (infer.c, Phase D2.4). Not memoized — they push every
// visited node's type into ctx->types. `type_of_expr` synthesizes; `check_expr`
// checks against an expected type (coercion + diags). type_of_def's inferred-
// bind path calls type_of_expr with enclosing_fn = DEF_ID_NONE.
IpIndex type_of_expr(const SemaCtx *ctx, SyntaxNode *node);
bool    check_expr(const SemaCtx *ctx, SyntaxNode *node, IpIndex expected);

// Build an interned fn type from a return-type node + param-list node (each may
// be NULL). Shared by fn_signature (top-level fns) + type_of_expr's nested
// SK_LAMBDA_EXPR case. Pushes per-param hover types into ctx's builder.
IpIndex build_fn_type(const SemaCtx *ctx, SyntaxNode *ret_node, SyntaxNode *param_list);

#endif // ORE_DB_QUERY_TYPE_LAYER_H
