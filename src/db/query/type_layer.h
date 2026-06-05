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
//
// Capability-frame invariant: the "must-be-inside-a-query-frame" rule is
// enforced at the db_read_* call sites in capability.c, NOT at SemaCtx
// construction. This is deliberate — SemaCtx is also used by pure intern /
// row helpers (row_union, row_unify, structural coerce with node=NULL) that
// don't need a frame, and tools/effect_test.c + tools/coerce_test.c
// construct SemaCtx without one to exercise those paths. Hoisting the
// assertion to SemaCtx birth would be strictly stronger than the actual
// invariant.
//
// Post-threading shape (see engine.h's db_query_ctx seam + migration.md
// "QueryFrame stack moves from db.query_stack to thread-local"):
//
//   struct {
//       db_query_ctx *qctx;  // engine handle (per-thread frame stack +
//                            //  cancel flag) — needed for db_query_* and
//                            //  db_read_* calls
//       struct db    *s;     // engine state (intern, arena, names,
//                            //  scopes, files) — needed for db_emit,
//                            //  ip_tag/ip_key, request_arena, etc.
//       ... existing per-traversal fields ...
//   };
//
// Rationale for carrying BOTH after the split (not just qctx): ~9 sites
// read ctx->s->intern / ctx->s->request_arena directly (coerce.c row
// interning + subst arena), and ~14 sites call helpers (db_emit,
// db_const_eval, coerce_structural_ctx) that take struct db *. Re-
// deriving `s = qctx->db` at each site would be a no-op cast but a wider
// diff than caching it on the ctx. The current `struct db *s = ctx->s;`
// capture lines stay as-is post-split; only the SemaCtx initializers
// gain a `.qctx = qctx` field. Engine APIs already take db_query_ctx *
// (wire-compatible via today's typedef), so no call-site edits beyond
// the initializer pattern.
//
// Deferred per the codebase's "don't build for hypothetical futures"
// rule (migration.md DEFERRED item H, ~lines 1314-1318) — the threading
// phase reworks the engine anyway; flipping SemaCtx in isolation
// produces churn without correctness or perf benefit.
typedef struct SemaCtx_ {
    struct db        *s;
    struct GreenNode *file_green_root;  // files.green_roots[file_local]
    NamespaceId       nsid;
    DefId             enclosing_fn;     // DEF_ID_NONE outside fn bodies
    FileId            file_local;
    NodeTypeBuilder  *types;            // active builder; NULL = pushes dropped
    // Decl-wrapper structural anchor inputs. Set inside any cached
    // query frame that owns a decl (TYPE_OF_DECL, FN_SIGNATURE,
    // INFER_BODY, BODY_SCOPES); NULL/0 elsewhere. When set, span_of()
    // prefers a DIAG_ANCHOR_BODY{decl_key, rel} anchor over the
    // legacy DIAG_ANCHOR_FILE_RAW — resolution survives sibling
    // reparse via decl_ast_id_resolve's preorder walk over the current
    // wrapper subtree. When NULL or on a wrapper-lookup miss, the
    // FILE_RAW fallback is emitted (correct but byte-frozen; see
    // docs/diag-anchor-audit.md for why this matters).
    const struct DeclAstIdMap *decl_ast_map;
    uint32_t                   decl_key; // AstId.idx as DeclKey
    // Effects-3 — per-inference-frame row-variable substitution table.
    // Interned rows are immutable, so unification CANNOT rewrite them in
    // place. Each binding "row-var id → IpIndex" lives here and is
    // chased by subst_resolve when an effect row's tail is read. NULL
    // outside of an active infer/sig frame (callers that don't unify
    // get the old strict-equality behavior).
    //   Key:   uint32_t  row-var id (IPK_ROW_VAR.id)
    //   Value: uint32_t  bound IpIndex.v (resolved row or row-var chain)
    HashMap                   *row_subst;
    // Effects-4 — accumulated effect row for the body being walked. Set
    // by the enclosing INFER_BODY / fn_signature frame to point at a
    // stack-local IpIndex initialized to IP_EMPTY_EFFECT_ROW; every
    // SK_CALL_EXPR unifies its callee's effect row into here, every
    // SK_HANDLE_EXPR discharges its targeted effect. NULL means "no
    // accumulator wired" (existing callers / tests pre-Effects-4) — the
    // call arms skip accumulation in that case so old behavior holds.
    IpIndex                   *body_effect_row;
    // Effects-5 — per-build_fn_type frame name-scope for row variables.
    // In `apply :: fn(f: fn() <..e> i32) <..e> i32`, the two `..e`
    // occurrences must intern the SAME IpIndex (one fresh IPK_ROW_VAR)
    // so the input row var propagates to the return. The outermost
    // build_fn_type allocates a stack-local HashMap (StrId.idx → row-var
    // IpIndex.v) and writes its address here on a sub-SemaCtx; inner
    // recursive build_fn_type / build_effect_row calls find a non-NULL
    // map and share it. Anonymous `...` rows ignore the map (always
    // fresh). NULL outside an active build_fn_type frame.
    //   Key:   uint32_t  StrId.idx
    //   Value: uint32_t  IpIndex.v of a fresh IPK_ROW_VAR
    HashMap                   *row_name_map;
    // Slice 5B — labeled-block result-type tracking. Linked-list of
    // frames pushed on entry to a labeled SK_BLOCK_STMT, popped on exit.
    // `break :label v` inside the block walks the chain for a matching
    // name and peer-unifies `v`'s type into the frame's accumulator.
    // The labeled block's final type is the accumulator (instead of void).
    // NULL when no labeled block is active.
    struct LabelFrame         *label_scope;
    // Slice 6.14 Step 0 (Fix F) — `return`-target override. Set by
    // sub-fn-scope walks that aren't themselves a top-level def: op
    // clause bodies (the op's declared return type), nested-lambda
    // bodies once Step 3 lands (the lambda's signature ret). When
    // non-IP_NONE, SK_RETURN_STMT prefers this over the
    // enclosing-DefId-derived signature. IP_NONE means "fall back to
    // enclosing_fn's sig" (the default for top-level fn bodies).
    IpIndex                    expected_ret_override;
} SemaCtx;

// Build a ConstDiagAnchorCtx from a SemaCtx (Fix B — drift-stable anchors for
// db_const_eval's 5 emit sites). Inlined at each db_const_eval / *_with_enum_ctx
// call so the anchor rides with the call without an extra named local.
// Requires ConstDiagAnchorCtx visible at use site (include const_eval.h).
#define SEMA_CONST_ANCHOR(ctx)                                             \
  ((ConstDiagAnchorCtx){ .decl_ast_map = (ctx)->decl_ast_map,              \
                         .decl_key     = (ctx)->decl_key })

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

// Sticky error sentinel predicate (follow-ups #20). Distinguishes the
// "already-diagnosed — absorb silently" sentinel (IP_ERROR_TYPE) from
// the "no value to report" sentinel (IP_NONE). Consumer arms check
// this BEFORE shape-inspection to suppress cascading diagnostics;
// producer paths return IP_ERROR_TYPE (not IP_NONE) after emitting
// the root diag so the sentinel propagates up.
static inline bool ip_is_error(IpIndex t) { return t.v == IP_ERROR_TYPE.v; }

// Look up `node`'s type in a sealed range. IP_NONE if absent / empty range.
IpIndex node_types_range_lookup(struct db *s, NodeTypesRange range,
                                SyntaxNode *node);

// Cache-peek (follow-ups #1) — read `node`'s type from the in-flight
// NodeTypeBuilder on ctx WITHOUT firing any compute or accumulating
// effects. Returns IP_NONE if ctx/builder/node is NULL, the builder is
// uninitialized, or the node hasn't been pushed yet by the current
// frame. Use this in passes that walk the body AFTER check_expr has
// populated the builder (the classic case is block_always_terminates'
// noreturn-callee check — re-typing the callee would double its effect
// row, the cache-peek avoids the side effect entirely).
IpIndex db_lookup_node_type(const SemaCtx *ctx, SyntaxNode *node);

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

// Build an interned fn type from a return-type node + param-list node + an
// optional effect-row node (each may be NULL). Shared by fn_signature (top-
// level fns) + type_of_expr's nested SK_LAMBDA_EXPR case. Pushes per-param
// hover types into ctx's builder. effect_row_node == NULL → pure (IP_EMPTY
// _EFFECT_ROW); otherwise build_effect_row interprets it.
IpIndex build_fn_type(const SemaCtx *ctx, SyntaxNode *ret_node,
                      SyntaxNode *param_list,
                      SyntaxNode *effect_row_node);

// Effects-1 — interned IPK_EFFECT_ROW from an SK_EFFECT_ROW_TYPE syntax
// node. NULL → IP_EMPTY_EFFECT_ROW. Emits diags for unknown labels.
IpIndex build_effect_row(const SemaCtx *ctx, SyntaxNode *er_node);

#endif // ORE_DB_QUERY_TYPE_LAYER_H
