// db_emit dispatch in this file: emits route to the active QueryFrame's
// installed sink. Both TYPE_OF_DECL and FN_SIGNATURE open their own
// bundle sinks at compute entry, so emits from this file land in the
// per-def type_of_decl_diags or per-fn signature_diags bundle. When
// resolve_type_expr is called from INFER_BODY's body walk the top
// frame is INFER_BODY's, so emits land in fn_body_diags — that's the
// sink-of-the-top-frame rule.
//
// Type layer — the DECLARED-INTERFACE queries: turn a DefId into
// an interned type.
//
//   type_of_def(def)      — a decl's overall type (IpIndex). Nominal types
//                           (struct/union/enum) are built here; functions
//                           delegate to fn_signature; typed binds resolve their
//                           annotation. Inferred binds + bodies are D2.4.
//   fn_signature(def)      — a function's (params → ret) type + per-param hover
//                           node-types (FnSignature).
//   resolve_type_expr(ctx) — resolve a type-position syntax node to an IpIndex.
//
// Ported from src/sema/{type_resolve,fn_signature,type_of_def}.c: the type
// algorithms are unchanged; only the dep plumbing is rewired onto the D1 layer.
// A decl's CURRENT location comes from top_level_entry(nsid, name) (the content
// firewall) — there is no decl_ast / multi-file loop / TOP_LEVEL_INDEX side
// effect. The green root is read raw from files.green_roots so type_of_def does
// NOT record a file_ast dep (that would defeat the content firewall — a sibling
// edit would re-run it); the firewall dep is top_level_entry's alone.

#define ORE_ENGINE_PRIVATE
#include "capability.h" // db_read_file_ast, db_get_def_*_untracked
#include "engine.h"
#include "engine_internal.h"
#include "result_columns.h" // db.h, ids.h, intern_pool.h, syntax.h
#include "type_layer.h"
#include "coerce.h"
#include "builtins.h" // db_builtin_kind_of — @TypeOf in type position
#include "eval.h"       // eval_expr (Phase 2 unified evaluator)
#include "typed_value.h" // TypedValue, TYPED_VALUE_NONE (Phase 1 plumbing)

// db_body_scope_lookup lives in body_scopes.c — used by resolve_type_expr's
// SK_REF_EXPR case to find a `t: type` (or any local) binding's type.
extern SyntaxNodePtr db_body_scope_lookup(db_query_ctx *ctx, DefId fn_def,
                                          SyntaxNode *use_node, StrId name);

#include "../diag/diag.h" // db_emit, diag_anchor_of_node, DIAG_ERROR

#include "../../ast/ast.h" // ast_first_token
#include "../../ast/ast_decl.h"
#include "../../ast/ast_expr.h"
#include "../../ast/ast_type.h"
#include "../../support/data_structure/hashmap.h"
#include "../../support/data_structure/stringpool.h"
#include "../../syntax/syntax_kind.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// --- Cross-layer queries (parse.c / scope.c) --------------------------------
extern FileArray db_query_namespace_items(db_query_ctx *ctx, NamespaceId nsid);
extern TopLevelEntry db_query_top_level_entry(db_query_ctx *ctx,
                                              NamespaceId nsid, StrId name);
extern NamespaceScopes db_query_namespace_scopes(db_query_ctx *ctx,
                                                 NamespaceId nsid);
extern DefId db_query_resolve_ref(db_query_ctx *ctx, ScopeId scope, StrId name);
// Phase-3.1 follow-up — build the wrapper-relative DeclAstIdMap. Walked
// preorder so emit-time span_of can map any node inside the wrapper
// (signature + body for fns; whole wrapper for non-fns) to its
// structural RelAstId. Defined in body_scopes.c.
extern void decl_ast_id_map_refresh(struct db *s, DefId def,
                                    SyntaxNode *wrapper_node);
extern DefId db_query_def_identity(db_query_ctx *ctx, NamespaceId nsid,
                                   AstId id);

// --- This layer (mutually recursive) ----------------------------------------
IpIndex db_query_type_of_def(db_query_ctx *ctx, DefId def);
const FnSignature *db_query_fn_signature(db_query_ctx *ctx, DefId def);

// ============================================================================
// NodeTypeBuilder — per-decl resolved-types map (RA InferenceResult).
// ============================================================================

void node_type_builder_begin(struct db *s, NodeTypeBuilder *b,
                             FileId file_local) {
  (void)s;
  b->file_local = file_local;
  hashmap_init(&b->types);
  b->fp = db_fp_u64(0);
}

void node_type_builder_push(const SemaCtx *ctx, SyntaxNode *node,
                            IpIndex type) {
  if (!ctx || !ctx->types || !node)
    return;
  NodeTypeBuilder *b = ctx->types;
  uint32_t rel = 0;
  uint64_t key;
  if (ctx->decl_ast_map && decl_ast_id_lookup(ctx->decl_ast_map, node, &rel)) {
    key = (uint64_t)rel;
  } else {
    key = syntax_node_ptr_hash(syntax_node_ptr_new(node));
  }
  // Phase 1 packed encoding: lower 32 bits = type.v, upper 32 = value.v.
  // Legacy "type-only" push uses value = IP_NONE (== 0), so the encoding is
  // byte-identical to the pre-Phase-1 layout. db_lookup_node_type and
  // node_types_range_lookup truncate to uint32_t (lower 32 bits) and
  // continue to work unchanged.
  hashmap_put(&b->types, key, (void *)(uintptr_t)type.v);
  // POSITION-INDEPENDENT: fold ONLY the type value, in push order. The node
  // key (kind+byte-range) is deliberately NOT folded — a pure trivia shift
  // would change it without changing any type, so including it would break
  // the firewall. Push order is structural (the descent is position-
  // independent), so order-folded type values catch add/remove/reorder/
  // retype; AST-structure changes reach AST-consumers via their own deps.
  b->fp = db_fp_combine(b->fp, db_fp_u64((uint64_t)type.v));
}

// Phase 1 — push both the type AND the comptime value at `node`. The two
// IpIndexes are packed into the HashMap slot (lower 32 = type.v, upper 32 =
// value.v). IP_NONE.v == 0, so passing value == IP_NONE produces the same
// encoding as legacy `node_type_builder_push`. Fingerprint folds value
// ONLY when non-NONE so legacy callers' fingerprints are unchanged.
void node_typed_value_push(const SemaCtx *ctx, SyntaxNode *node, IpIndex type,
                           IpIndex value) {
  if (!ctx || !ctx->types || !node)
    return;
  NodeTypeBuilder *b = ctx->types;
  uint32_t rel = 0;
  uint64_t key;
  if (ctx->decl_ast_map && decl_ast_id_lookup(ctx->decl_ast_map, node, &rel)) {
    key = (uint64_t)rel;
  } else {
    key = syntax_node_ptr_hash(syntax_node_ptr_new(node));
  }
  uint64_t packed = ((uint64_t)value.v << 32) | (uint64_t)type.v;
  hashmap_put(&b->types, key, (void *)(uintptr_t)packed);
  b->fp = db_fp_combine(b->fp, db_fp_u64((uint64_t)type.v));
  // Skip folding IP_NONE so legacy fingerprints stay identical when callers
  // migrate from node_type_builder_push to node_typed_value_push with value
  // = IP_NONE. Only a non-NONE value contributes a new fold.
  if (value.v != IP_NONE.v)
    b->fp = db_fp_combine(b->fp, db_fp_u64((uint64_t)value.v));
}

NodeTypesRange node_type_builder_end(NodeTypeBuilder *b, Fingerprint *out_fp) {
  if (out_fp)
    *out_fp = b->fp;
  NodeTypesRange out = {.types = b->types};
  b->types = (HashMap){0};
  return out;
}

IpIndex node_types_range_lookup(struct db *s, NodeTypesRange range,
                                uint64_t key) {
  (void)s;
  if (!hashmap_is_initialized(&range.types))
    return IP_NONE;
  // Presence via the occupied bitset — NOT `v != NULL`. (Historically
  // bool was intern index 0 and stored (void*)0; post the IP_NONE=0 flip
  // no real type is index 0, but the bitset remains the only truth.)
  if (!hashmap_contains(&range.types, key))
    return IP_NONE;
  void *v = hashmap_get(&range.types, key);
  // Phase 1 packed encoding: lower 32 bits = type, upper 32 = value. The
  // uint32_t cast truncates to the lower half, preserving legacy semantics
  // for type lookups across the packing change.
  return (IpIndex){.v = (uint32_t)(uintptr_t)v};
}

// Phase 1 — persisted-layer value lookup. Extracts the upper 32 bits of
// the packed HashMap entry (legacy entries with no value stored have those
// bits zero → returns IP_NONE).
IpIndex node_types_range_value_lookup(struct db *s, NodeTypesRange range,
                                      uint64_t key) {
  (void)s;
  if (!hashmap_is_initialized(&range.types))
    return IP_NONE;
  if (!hashmap_contains(&range.types, key))
    return IP_NONE;
  void *v = hashmap_get(&range.types, key);
  return (IpIndex){.v = (uint32_t)((uintptr_t)v >> 32)};
}

IpIndex db_lookup_node_type(const SemaCtx *ctx, SyntaxNode *node) {
  if (!ctx || !ctx->types || !node)
    return IP_NONE;
  NodeTypeBuilder *b = ctx->types;
  if (!hashmap_is_initialized(&b->types))
    return IP_NONE;
  uint32_t rel = 0;
  uint64_t key;
  if (ctx->decl_ast_map && decl_ast_id_lookup(ctx->decl_ast_map, node, &rel)) {
    key = (uint64_t)rel;
  } else {
    key = syntax_node_ptr_hash(syntax_node_ptr_new(node));
  }
  // Presence via the occupied bitset — NOT `v != NULL`. (Historically
  // bool was intern index 0 and stored (void*)0; post the IP_NONE=0 flip
  // no real type is index 0, but the bitset remains the only truth.)
  if (!hashmap_contains(&b->types, key))
    return IP_NONE;
  void *v = hashmap_get(&b->types, key);
  // Phase 1 packed encoding: lower 32 = type; uint32_t cast truncates.
  return (IpIndex){.v = (uint32_t)(uintptr_t)v};
}

// Phase 1 — look up the comptime VALUE at `node`. Returns IP_NONE if the
// node has no entry OR was pushed via legacy node_type_builder_push (which
// stores value = IP_NONE in the upper 32 bits).
IpIndex db_lookup_node_value(const SemaCtx *ctx, SyntaxNode *node) {
  if (!ctx || !ctx->types || !node)
    return IP_NONE;
  NodeTypeBuilder *b = ctx->types;
  if (!hashmap_is_initialized(&b->types))
    return IP_NONE;
  uint32_t rel = 0;
  uint64_t key;
  if (ctx->decl_ast_map && decl_ast_id_lookup(ctx->decl_ast_map, node, &rel)) {
    key = (uint64_t)rel;
  } else {
    key = syntax_node_ptr_hash(syntax_node_ptr_new(node));
  }
  if (!hashmap_contains(&b->types, key))
    return IP_NONE;
  void *v = hashmap_get(&b->types, key);
  return (IpIndex){.v = (uint32_t)((uintptr_t)v >> 32)};
}

// Phase 1 — look up the (type, value) pair at `node` in one call. Returns
// TYPED_VALUE_NONE if the node has no entry.
TypedValue db_lookup_node_typed_value(const SemaCtx *ctx, SyntaxNode *node) {
  if (!ctx || !ctx->types || !node)
    return TYPED_VALUE_NONE;
  NodeTypeBuilder *b = ctx->types;
  if (!hashmap_is_initialized(&b->types))
    return TYPED_VALUE_NONE;
  uint32_t rel = 0;
  uint64_t key;
  if (ctx->decl_ast_map && decl_ast_id_lookup(ctx->decl_ast_map, node, &rel)) {
    key = (uint64_t)rel;
  } else {
    key = syntax_node_ptr_hash(syntax_node_ptr_new(node));
  }
  if (!hashmap_contains(&b->types, key))
    return TYPED_VALUE_NONE;
  void *v = hashmap_get(&b->types, key);
  uintptr_t u = (uintptr_t)v;
  return (TypedValue){
      .type  = (IpIndex){.v = (uint32_t)(u & 0xFFFFFFFFu)},
      .value = (IpIndex){.v = (uint32_t)(u >> 32)}};
}

// ============================================================================
// Small helpers.
// ============================================================================

static StrId intern_tok(struct db *s, SyntaxToken *t) {
  if (!t)
    return (StrId){0};
  const char *txt = syntax_token_text(t);
  uint32_t len = syntax_token_text_range(t).length;
  return pool_intern(&s->strings, txt, len);
}

// Bare int-literal value (underscores stripped; 0x/0b/0o prefixes via strtoll).
static int64_t literal_int_signed(SyntaxNode *node) {
  Literal lit;
  if (!Literal_cast(node, &lit) || Literal_kind(&lit) != SK_INT_LIT)
    return 0;
  SyntaxToken *tok = Literal_token(&lit);
  if (!tok)
    return 0;
  const char *txt = syntax_token_text(tok);
  uint32_t len = syntax_token_text_range(tok).length;
  char buf[64];
  int64_t v = 0;
  if (len < sizeof(buf)) {
    uint32_t w = 0;
    for (uint32_t i = 0; i < len; i++)
      if (txt[i] != '_')
        buf[w++] = txt[i];
    buf[w] = '\0';
    v = (int64_t)strtoll(buf, NULL, 0);
  }
  syntax_token_release(tok);
  return v;
}

uint64_t parse_int_literal(SyntaxToken *tok) {
  const char *txt = syntax_token_text(tok);
  uint32_t len = syntax_token_text_range(tok).length;
  char buf[64];
  if (len >= sizeof(buf))
    return 0;
  uint32_t w = 0;
  for (uint32_t i = 0; i < len; i++)
    if (txt[i] != '_')
      buf[w++] = txt[i];
  buf[w] = '\0';
  return strtoull(buf, NULL, 0);
}

// Last IDENT child of a PATH node (the leaf name in type position).
// Non-static in Phase 2 — eval.c reuses it for SK_PATH name extraction.
StrId path_expr_leaf_name(struct db *s, SyntaxNode *path) {
  uint32_t count = syntax_node_num_children(path);
  StrId last = {0};
  for (uint32_t i = 0; i < count; i++) {
    GreenElement g = green_node_child(syntax_node_green(path), i);
    if (g.kind == GREEN_ELEM_TOKEN && green_token_kind(g.token) == SK_IDENT) {
      const char *txt = green_token_text(g.token);
      uint32_t len = green_token_text_len(g.token);
      last = pool_intern(&s->strings, txt, len);
    }
  }
  return last;
}

// Non-static in Phase 2 — eval.c reuses it for diag anchors.
DiagAnchor span_of(const SemaCtx *ctx, SyntaxNode *node) {
  // Prefer a structural DIAG_ANCHOR_BODY anchor when the active
  // SemaCtx has a decl_ast_map (built by TYPE_OF_DECL over the
  // wrapper). RelAstId is the node's preorder position in the
  // wrapper subtree — survives sibling reparses by re-walking the
  // current tree at publish time. See ast_id.h.
  if (ctx->decl_ast_map && node) {
    uint32_t rel;
    if (decl_ast_id_lookup(ctx->decl_ast_map, node, &rel)) {
      return diag_anchor_body((uint16_t)ctx->file_local.idx,
                              (DeclKey)ctx->decl_key, (RelAstId)rel);
    } else {
      assert(false && "span_of: node present in sema context but missing from decl_ast_map");
    }
  }
  // Fallback — FILE_RAW byte anchor. Reached for synthetic nodes
  // never visited by the wrapper walk, or callers outside any cached
  // query (where decl_ast_map is NULL). Drifts under byte-shifting
  // sibling edits — see docs/diag-anchor-audit.md.
  return diag_anchor_of_node((uint16_t)ctx->file_local.idx, node); // LINT_FILE_RAW_OK: span_of fallback when decl_ast_map miss
}

// The value / type-annotation node of a `::`/`:=` bind wrapper (+1; release).
static SyntaxNode *bind_value(SyntaxNode *wrapper) {
  BindDef b;
  if (BindDef_cast(wrapper, &b))
    return BindDef_value(&b);
  return NULL;
}
static SyntaxNode *bind_type_annot(SyntaxNode *wrapper) {
  BindDef b;
  if (BindDef_cast(wrapper, &b))
    return BindDef_type(&b);
  return NULL;
}

// User-defined name resolution: internal scope → resolve_ref → type_of_def.
// Records the salsa deps so renaming/retyping the target invalidates us.
// The def kinds that DENOTE a type (usable in type position). One namespace
// (Zig/Odin types-as-values): a name can resolve to a value just as easily as
// a type, so a use-site kind-gate is what keeps `x: FOO` (FOO a value) honest.
// Type-alias kinds are future work (6.18-era) — a `Foo :: i32` const is
// KIND_CONSTANT and is (correctly, for now) rejected as a value.
static bool def_kind_is_type(DefKind k) {
  switch (k) {
  case KIND_STRUCT:
  case KIND_UNION:
  case KIND_ENUM:
  case KIND_DISTINCT:
  case KIND_EFFECT:
    return true;
  default: // FUNCTION / VARIABLE / CONSTANT / NONE — a value (or unresolved)
    return false;
  }
}

// Resolve a bare name in TYPE position to the type it denotes, emitting the
// right diagnostic on failure: "unknown type" when the name does not resolve,
// "'X' is a value, not a type" when it resolves to a non-type def (the
// one-namespace foot-gun). Returns IP_ERROR_TYPE (sticky) on either failure;
// IP_NONE only for an empty name (caller leaves its result IP_NONE, no diag).
// Non-static in Phase 2 — eval.c reuses it for SK_REF name resolution.
IpIndex resolve_type_name_checked(const SemaCtx *ctx, SyntaxNode *node,
                                         StrId name) {
  struct db *s = ctx->s;
  if (name.idx == 0)
    return IP_NONE;
  NamespaceScopes sc = db_query_namespace_scopes(s, ctx->nsid); // dep
  DefId target = DEF_ID_NONE;
  if (sc.internal.idx != SCOPE_ID_NONE.idx)
    target = db_query_resolve_ref(s, sc.internal, name); // dep
  if (target.idx == DEF_ID_NONE.idx) {
    db_emit(s, DIAG_ERROR, span_of(ctx, node), "unknown type '%S'", name);
    return IP_ERROR_TYPE; // sticky — consumer absorbs without re-diag
  }
  // Primitives (i32, anytype, …) are seeded with KIND_NONE — they skip the
  // per-kind tables, so the kind-gate below can't see them. They ARE types:
  // accept via the primitive discriminator before the gate (type_of_def
  // short-circuits the same way, but returning `prim` keeps it explicit).
  IpIndex prim = db_primitive_type_for(s, target);
  if (ip_index_is_valid(prim))
    return prim;
  if (!def_kind_is_type(db_def_kind(s, target))) {
    db_emit(s, DIAG_ERROR, span_of(ctx, node), "'%S' is a value, not a type",
            name);
    return IP_ERROR_TYPE;
  }
  return db_query_type_of_def(s, target); // dep
}

// ============================================================================
// build_effect_row — interned IPK_EFFECT_ROW from an SK_EFFECT_ROW_TYPE node.
// Walks the syntax children: each type-expression child contributes a label;
// a `..e` or `...` token sets a fresh IPK_ROW_VAR tail (open row).
// NULL node → IP_EMPTY_EFFECT_ROW (pure / closed).
// ============================================================================

static int defid_cmp(const void *a, const void *b) {
  uint32_t ai = ((const DefId *)a)->idx;
  uint32_t bi = ((const DefId *)b)->idx;
  return (ai > bi) - (ai < bi);
}

IpIndex build_effect_row(const SemaCtx *ctx, SyntaxNode *er_node) {
  struct db *s = ctx->s;
  if (!er_node)
    return IP_EMPTY_EFFECT_ROW;

  EffectRowType row;
  if (!EffectRowType_cast(er_node, &row))
    return IP_EMPTY_EFFECT_ROW; // defensive — callers pass SK_EFFECT_ROW_TYPE

  // Tail (6.31) — find-by-kind SK_EFFECT_ROW_TAIL marker. Present → open row;
  // an inner SK_IDENT is the named row var (`..e`), absent → anonymous `...`.
  bool has_tail_var = false;
  StrId tail_name = {0};
  SyntaxNode *tail_node = EffectRowType_tail(&row);
  if (tail_node) {
    has_tail_var = true;
    SyntaxToken *tv = ast_first_token(tail_node, SK_IDENT);
    if (tv) {
      tail_name = intern_tok(s, tv);
      syntax_token_release(tv);
    }
    syntax_node_release(tail_node);
  }

  // Labels (6.31) — node children of the SK_EFFECT_LABEL_LIST wrapper.
  SyntaxNode *label_list = EffectRowType_labels(&row);
  uint32_t n_labels = 0;
  if (label_list) {
    uint32_t nc = syntax_node_num_children(label_list);
    for (uint32_t i = 0; i < nc; i++) {
      GreenElement g = green_node_child(syntax_node_green(label_list), i);
      if (g.kind == GREEN_ELEM_NODE)
        n_labels++;
    }
  }

  DefId *labels = NULL;
  if (n_labels > 0) {
    labels = arena_alloc(&s->request_arena, n_labels * sizeof(DefId));
    if (!labels) {
      if (label_list)
        syntax_node_release(label_list);
      return IP_NONE;
    }
  }

  // Resolve each label to a DefId (any def kind for now; Phase 4 enforces
  // KIND_EFFECT). Labels are nominal idents resolved via the internal scope.
  uint32_t out = 0;
  bool had_bad_label = false;
  if (label_list) {
    uint32_t nc = syntax_node_num_children(label_list);
    for (uint32_t i = 0; i < nc; i++) {
      SyntaxElement el = syntax_node_child_or_token(label_list, i);
      if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
        if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
          syntax_token_release(el.token);
        continue;
      }
      SyntaxToken *name_tok = ast_first_token(el.node, SK_IDENT);
      StrId name = intern_tok(s, name_tok);
      if (name_tok)
        syntax_token_release(name_tok);
      DefId target = DEF_ID_NONE;
      if (name.idx != 0) {
        NamespaceScopes sc = db_query_namespace_scopes(s, ctx->nsid);
        if (sc.internal.idx != SCOPE_ID_NONE.idx)
          target = db_query_resolve_ref(s, sc.internal, name);
      }
      if (target.idx == DEF_ID_NONE.idx) {
        // Diag THIS label and keep validating the siblings — the old
        // first-bail meant `<bad1, bad2>` only ever reported bad1. The
        // row still fails (error return below), but every bad label gets
        // its own diag in one compile.
        if (name.idx != 0)
          db_emit(s, DIAG_ERROR, span_of(ctx, el.node),
                  "unknown effect '%S'", name);
        else
          db_emit(s, DIAG_ERROR, span_of(ctx, el.node),
                  "malformed effect label");
        had_bad_label = true;
        syntax_node_release(el.node);
        continue;
      }
      labels[out++] = target;
      node_type_builder_push(ctx, el.node, IP_NONE); // hover placeholder
      syntax_node_release(el.node);
    }
    syntax_node_release(label_list);
  }
  if (had_bad_label)
    return IP_ERROR_TYPE;

  // Sort labels by DefId.idx (duplicates allowed — Koka's scoped-labels
  // semantics keep adjacent duplicates).
  if (out > 1)
    qsort(labels, out, sizeof(DefId), defid_cmp);

  // Tail: anonymous `...` (tail_name.idx == 0) or a missing name-map gets a
  // fresh row var per occurrence. A NAMED `..e` with a live row_name_map
  // dedupes by StrId so two `..e` references in one build_fn_type frame share
  // ONE IPK_ROW_VAR (first allocates + inserts; subsequent reuse).
  IpIndex tail;
  if (!has_tail_var) {
    tail = IP_EMPTY_EFFECT_ROW;
  } else if (tail_name.idx != 0 && ctx->row_name_map) {
    uint64_t key = (uint64_t)tail_name.idx;
    void *cached = hashmap_get(ctx->row_name_map, key);
    if (cached) {
      tail = (IpIndex){.v = (uint32_t)(uintptr_t)cached};
    } else {
      tail = ip_fresh_row_var(&s->intern);
      hashmap_put(ctx->row_name_map, key, (void *)(uintptr_t)tail.v);
    }
  } else {
    tail = ip_fresh_row_var(&s->intern);
  }

  IpKey key = {.kind = IPK_EFFECT_ROW,
               .effect_row = {.labels = labels,
                              .n_labels = out,
                              .tail = tail},
               .src_arena = labels ? &s->request_arena : NULL,
               .src_gen = s->request_arena.generation};
  IpIndex result = ip_get(&s->intern, key);
  node_type_builder_push(ctx, er_node, result);
  return result;
}

// ============================================================================
// build_fn_type — interned fn type from a param-list + return-type node.
// Shared by the lambda path (fn_signature) and the type-position SK_FN_TYPE.
// Identity is structural (ret, modifiers, params, effect_row). Scratch params
// come from request_arena (reset at db_request_end); n_params has no compile
// cap.
//
// PER-SLOT POISONING: the result is ALWAYS a structurally-valid FN_TYPE —
// a param/ret/effect-row that fails to type stores IP_ERROR_TYPE in ITS
// slot (after a diag, per the poison contract) and construction continues.
// Never a whole-sig collapse: arity, sibling param types, hover pushes and
// body checking all survive one bad slot. The only IP_NONE returns left are
// the arena-OOM paths. Consumers tolerate error entries via explicit
// guards (check_expr force-walk, coerce absorption, terminator/effect-gate
// skips, row-accumulator skips); fixing the bad slot re-interns a DIFFERENT
// index, so the FN_SIGNATURE fingerprint flips and callers recompute.
// ============================================================================

IpIndex build_fn_type(const SemaCtx *ctx, SyntaxNode *ret_node,
                      SyntaxNode *param_list,
                      SyntaxNode *effect_row_node) {
  struct db *s = ctx->s;

  // Effects-5 — row-variable name scope. The OUTERMOST build_fn_type
  // frame allocates a stack-local StrId→IpIndex map; recursive descent
  // (param fn types, return fn types) reuses it so two `..e` mentions
  // intern ONE IPK_ROW_VAR. Anonymous `...` ignores the map. A nested
  // build_fn_type that inherits a live map from its caller passes the
  // SAME ctx through (no re-allocation, no fresh scope).
  HashMap row_name_map_local = {0};
  HashMap type_name_map_local = {0};
  SemaCtx sub_ctx;
  const SemaCtx *eff_ctx = ctx;
  if (!ctx->row_name_map) {
    hashmap_init(&row_name_map_local);
    hashmap_init(&type_name_map_local);
    sub_ctx = *ctx;
    sub_ctx.row_name_map = &row_name_map_local;
    sub_ctx.type_name_map = &type_name_map_local;
    eff_ctx = &sub_ctx;
  }

  uint32_t n_params = 0;
  if (param_list) {
    uint32_t pc = syntax_node_num_children(param_list);
    for (uint32_t i = 0; i < pc; i++) {
      GreenElement g = green_node_child(syntax_node_green(param_list), i);
      if (g.kind == GREEN_ELEM_NODE && green_node_kind(g.node) == SK_PARAM)
        n_params++;
    }
  }

  // Phase 3 — per-param flags. bit i set ⇒ arg i is comptime-evaluated by
  // eval_expr at the call site (Zig-style). typevalued_bits[i] ⇒ the comptime
  // result IS a type (param was annotated `: type`). Replaces TYPE_VAR.kind.
  uint32_t comptime_bits = 0;
  uint32_t typevalued_bits = 0;

  IpIndex *params = NULL;
  if (n_params > 0) {
    params = arena_alloc(&s->request_arena, n_params * sizeof(IpIndex));
    if (!params) {
      if (eff_ctx == &sub_ctx) {
        hashmap_free(&row_name_map_local);
        hashmap_free(&type_name_map_local);
      }
      return IP_NONE;
    }
  }

  if (param_list) {
    uint32_t pc = syntax_node_num_children(param_list);
    uint32_t out = 0;
    for (uint32_t i = 0; i < pc; i++) {
      SyntaxElement el = syntax_node_child_or_token(param_list, i);
      if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
        if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
          syntax_token_release(el.token);
        continue;
      }
      Param p;
      if (!Param_cast(el.node, &p)) {
        syntax_node_release(el.node);
        continue;
      }
      SyntaxNode *ptype = Param_type(&p);
      IpIndex pti = ptype ? resolve_type_expr(eff_ctx, ptype) : IP_NONE;
      // Slice 6.13 Fix C — per-param hard-error when no type annotation is
      // declared AND no bidirectional inference supplied one. Inference
      // routes (SK_HANDLER_EXPR op-clauses via Fix E, future SK_CALL_EXPR
      // arg-position lambdas) PRE-push the param's type into the node-type
      // map; if we reach build_fn_type with ptype == NULL, no inference
      // source is available — the user must annotate.
      //
      // Previously this site silently returned IP_NONE for the whole fn,
      // which cascaded as `bin: fn(anytype) -> void`-style hover leaks
      // (workflow 2026-06-04: that cascade was the visible symptom of
      // exactly this swallow).
      if (!ptype) {
        SyntaxToken *name_tok = Param_name(&p);
        StrId pname = intern_tok(s, name_tok);
        if (name_tok)
          syntax_token_release(name_tok);
        if (pname.idx != 0) {
          db_emit(s, DIAG_ERROR, span_of(eff_ctx, el.node),
                  "parameter '%S' has no type annotation and cannot be "
                  "inferred from context",
                  pname);
        } else {
          db_emit(s, DIAG_ERROR, span_of(eff_ctx, el.node),
                  "parameter has no type annotation and cannot be inferred "
                  "from context");
        }
        pti = IP_ERROR_TYPE;
      }
      if (ptype)
        syntax_node_release(ptype);
      // Per-slot poisoning — a failed param stores IP_ERROR_TYPE in ITS
      // slot and the loop continues. The old whole-sig bail here made one
      // bad param hide every param from the body (sig_is_fn false → the
      // param-push loop in infer_body skipped → "function can't see its
      // params" + every return-value subtree released unvisited). The
      // signature stays structurally valid: arity survives, sibling
      // params keep their types and hover, and the body is still checked.
      // pti == IP_NONE (a resolve path that produced no diag of its own)
      // gets the catch-all diag here — poison only after a diagnostic.
      if (pti.v == IP_NONE.v) {
        SyntaxToken *name_tok = Param_name(&p);
        StrId pname = intern_tok(s, name_tok);
        if (name_tok)
          syntax_token_release(name_tok);
        if (pname.idx != 0)
          db_emit(s, DIAG_ERROR, span_of(eff_ctx, el.node),
                  "parameter '%S' has a malformed type", pname);
        else
          db_emit(s, DIAG_ERROR, span_of(eff_ctx, el.node),
                  "parameter has a malformed type");
        pti = IP_ERROR_TYPE;
      }
      // Phase 3 — generic params mint a fresh IPK_TYPE_VAR hole, and the
      // SIGNATURE (not the hole) records how the call-site arg is evaluated.
      //   `t: type`  → comptime_bits[i]=1, typevalued_bits[i]=1; arg is a
      //     type expression. Body refs to `t` in type position resolve to
      //     the hole (via type_name_map), which type_resolve chases.
      //   `anytype`  → comptime_bits[i]=1, typevalued_bits[i]=0; arg is a
      //     value. To USE the param's type in the body, write `@TypeOf(x)`.
      // The param NAME is recorded in type_name_map so later `@TypeOf(x)`
      // (or a body ref to `t`) resolves to the SAME hole.
      bool ann_is_type    = (pti.v == IP_TYPE_TYPE.v);
      bool ann_is_anytype = (pti.v == IP_ANYTYPE_TYPE.v);
      bool param_is_generic = ann_is_type || ann_is_anytype;
      // Phase 6 Batch 6 — explicit `comptime n: u32` keyword form. The param
      // is value-shaped (no TYPE_VAR hole) but call-site dispatch must still
      // route it through eval_expr to receive its value half. typevalued_bits
      // stays 0 — the bind target is the value's type, not the value itself.
      bool has_comptime_kw = false;
      {
        uint32_t pcc = syntax_node_num_children(el.node);
        for (uint32_t cj = 0; cj < pcc; cj++) {
          GreenElement g = green_node_child(syntax_node_green(el.node), cj);
          if (g.kind == GREEN_ELEM_TOKEN &&
              green_token_kind(g.token) == SK_COMPTIME_KW) {
            has_comptime_kw = true;
            break;
          }
        }
      }
      if (has_comptime_kw && !param_is_generic) {
        if (out >= 32) {
          db_emit(s, DIAG_ERROR, span_of(eff_ctx, el.node),
                  "comptime parameters beyond position 32 are not supported");
        } else {
          comptime_bits |= (1u << out);
        }
      }
      if (param_is_generic && eff_ctx->type_name_map) {
        if (out >= 32) {
          db_emit(s, DIAG_ERROR, span_of(eff_ctx, el.node),
                  "comptime parameters beyond position 32 are not supported");
        } else {
          comptime_bits |= (1u << out);
          if (ann_is_type) typevalued_bits |= (1u << out);
        }
        IpIndex hole = ip_fresh_type_var(&s->intern);
        SyntaxToken *nm = Param_name(&p);
        StrId pn = intern_tok(s, nm);
        if (nm)
          syntax_token_release(nm);
        if (pn.idx != 0)
          hashmap_put(eff_ctx->type_name_map, (uint64_t)pn.idx,
                      (void *)(uintptr_t)hole.v);
        pti = hole;
      }
      params[out++] = pti;
      node_type_builder_push(eff_ctx, el.node, pti); // hover on the param node
      syntax_node_release(el.node);
    }
  }

  // Per-slot poisoning (same rule as the param loop): a failed return type
  // stores IP_ERROR_TYPE in the ret slot, never collapses the signature.
  // Consumers guard: the terminator gate and naked-`return;` diag skip an
  // error ret; check_expr absorbs-and-force-walks return values against it.
  IpIndex ret;
  if (!ret_node) {
    ret = IP_VOID_TYPE; // implicit void
  } else {
    ret = resolve_type_expr(eff_ctx, ret_node);
    if (ret.v == IP_NONE.v) {
      db_emit(s, DIAG_ERROR, span_of(eff_ctx, ret_node),
              "malformed return type");
      ret = IP_ERROR_TYPE;
    } else if (ret.v == IP_ANYTYPE_TYPE.v) {
      // Zig-faithful: `anytype` is only legal as a parameter annotation.
      // Generic returns must come from a comptime type parameter
      // (`fn(t: type) -> t`) or `@TypeOf(<param>)` (`fn(x: anytype) ->
      // @TypeOf(x)`). A bare `anytype` return has nothing to bind from.
      db_emit(s, DIAG_ERROR, span_of(eff_ctx, ret_node),
              "'anytype' is not a valid return type; use '@TypeOf(<arg>)' "
              "or a comptime type parameter");
      ret = IP_ERROR_TYPE;
    }
  }

  // Effects-1 — interned effect row. NULL → IP_EMPTY_EFFECT_ROW (pure).
  // A failed row (unknown effect label, diag'd at the label) stores
  // IP_ERROR_TYPE in the row slot; the row consumers (soundness gates,
  // row_union accumulators, fn-type variance) all skip an error slot.
  // The hash/eq IP_NONE→EMPTY normalization doesn't touch it (error ≠ 0).
  IpIndex er = build_effect_row(eff_ctx, effect_row_node);
  if (er.v == IP_NONE.v && effect_row_node) {
    db_emit(s, DIAG_ERROR, span_of(eff_ctx, effect_row_node),
            "malformed effect row");
    er = IP_ERROR_TYPE;
  } else if (er.v == IP_NONE.v) {
    er = IP_EMPTY_EFFECT_ROW; // defensive: NULL row node is pure
  }

  IpKey key = {.kind = IPK_FN_TYPE,
               .fn_type = {.ret = ret,
                           .modifiers = 0,
                           .comptime_bits = comptime_bits,
                           .typevalued_bits = typevalued_bits,
                           .params = params,
                           .n_params = n_params,
                           .effect_row = er},
               .src_arena = params ? &s->request_arena : NULL,
               .src_gen = s->request_arena.generation};
  IpIndex out = ip_get(&s->intern, key);
  if (eff_ctx == &sub_ctx) {
    hashmap_free(&row_name_map_local);
    hashmap_free(&type_name_map_local);
  }
  return out;
}

// ============================================================================
// resolve_type_expr — a type-position node → IpIndex.
// ============================================================================

// Phase 2 — resolve_type_expr is now a thin wrapper around eval_expr,
// gating the (type, value) result on "expression IS a type": either its
// type field is IP_TYPE_TYPE (the metatype) and the value is a concrete
// type, or the value itself is a TYPE_VAR hole (generic-position).
//
// The previous case-by-case dispatch (~420 lines) was DELETED in this
// commit. Every kind it handled — SK_REF/PATH, SK_BUILTIN_EXPR (@TypeOf),
// SK_CONST_TYPE, SK_OPTIONAL_TYPE, SK_PTR/SLICE/MANY_PTR_TYPE,
// SK_ARRAY_TYPE, SK_FN_TYPE, SK_DISTINCT_TYPE — has been ported to
// eval_expr in src/db/query/eval.c. M2(f) (the silent-acceptance bug
// where a value-typed local was usable in type position) is closed by
// the new "type field == IP_TYPE_TYPE" gate.
IpIndex resolve_type_expr(const SemaCtx *ctx, SyntaxNode *node) {
  if (!node)
    return IP_NONE;
  struct db *s = ctx->s;
  TypedValue tv = eval_expr(ctx, node);
  IpIndex result;
  if (tv.type.v == IP_TYPE_TYPE.v && tv.value.v != IP_NONE.v &&
      !ip_is_error(tv.value)) {
    // Standard case: expression evaluated to a value of metatype `type`.
    result = tv.value;
  } else if (ip_tag(&s->intern, tv.value) == IP_TAG_TYPE_VAR) {
    // Generic-hole position; the hole IS the type expression's resolved
    // type (chases via type_resolve at instance time).
    result = tv.value;
  } else if (ip_is_error(tv.type) || ip_is_error(tv.value)) {
    // eval_expr emitted its own diag — just pass the error through.
    result = IP_ERROR_TYPE;
  } else if (tv.type.v == IP_NONE.v) {
    // Graceful "no type produced" — empty name, parse recovery, etc.
    result = IP_NONE;
  } else {
    // Value-of-non-type-type in type position (e.g., `c :: 5; x : c`).
    // This is the M2(f) gate.
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "expected type, got value of type %T", tv.type);
    result = IP_ERROR_TYPE;
  }
  // Preserve the legacy side-effect: stamp the resolved type at the
  // node for hover. (eval_expr also stamps both halves at its dispatch
  // points; this push is harmless — same key, same lower-32 type bits.)
  node_type_builder_push(ctx, node, result);
  return result;
}

// ============================================================================
// build_struct_type / build_enum_type — nominal type construction.
// ============================================================================

// Build an SK_STRUCT_DECL / SK_UNION_DECL type. Identity is nominal (zir =
// def.idx) and inline-encoded, so `ip_get(IPK_STRUCT_TYPE, {zir})` returns a
// STABLE deduped index up front — published into the def's type cell BEFORE the
// field loop so a self/mutually-referential field (`^Self`) cycling back into
// type_of_def reads the cell (on_cycle) and gets this very index. The field
// list goes into db.struct_field_pool (recompute-friendly, decl_pool pattern),
// NOT the intern pool. Fields are resolved into a request-arena scratch first,
// then bulk-appended AFTER the loop — resolve_type_expr can recursively append
// other structs' fields to the same pool, so deferring our append keeps our
// range contiguous. A field that fails to resolve stores type=IP_ERROR_TYPE
// after a diag (the struct stays a valid nominal — no whole-type cancel;
// IP_NONE is reserved for "field absent"). *fp_out =
// combine(IpIndex.v, ⊕ field name+type): the index is stable, so this content
// fold is what flips type_of_def's fp on a field-type edit.
static IpIndex build_struct_type(const SemaCtx *base, SyntaxNode *agg,
                                 DefId def, Fingerprint *fp_out) {
  struct db *s = base->s;
  DefKind dk = db_def_kind(s, def); // KIND_STRUCT or KIND_UNION

  IpIndex idx =
      ip_get(&s->intern, (IpKey){.kind = IPK_STRUCT_TYPE,
                                 .struct_type = {.zir_node_id = def.idx}});
  *db_def_type_cell(s, def) = idx; // self-reference anchor
  Fingerprint content = db_fp_u64((uint64_t)idx.v);

  SyntaxNode *field_list = NULL;
  SyntaxKind ak = syntax_node_kind(agg);
  if (ak == SK_STRUCT_DECL) {
    StructDef sd;
    if (StructDef_cast(agg, &sd))
      field_list = StructDef_fields(&sd);
  } else if (ak == SK_UNION_DECL) {
    UnionDef ud;
    if (UnionDef_cast(agg, &ud))
      field_list = UnionDef_variants(&ud);
  }

  // Count SK_FIELD slots, flattening anonymous-nested aggregates (a `union`
  // or `struct` block inside a struct body parses as SK_FIELD with no name
  // + an inner SK_UNION_DECL/SK_STRUCT_DECL; its inner fields contribute to
  // the parent's flat list per D2.1b). One level of nesting handled here;
  // deeper nesting would need a recursive walk.
  uint32_t n_fields = 0;
  if (field_list) {
    uint32_t list_count = syntax_node_num_children(field_list);
    for (uint32_t i = 0; i < list_count; i++) {
      GreenElement g = green_node_child(syntax_node_green(field_list), i);
      if (g.kind != GREEN_ELEM_NODE || green_node_kind(g.node) != SK_FIELD)
        continue;
      // Peek inside this field for an anonymous nested aggregate. The green
      // node's children are tokens + nodes; a named field has an SK_IDENT
      // token, an anonymous nested aggregate has only a SK_UNION_DECL or
      // SK_STRUCT_DECL node child.
      bool has_name = false;
      const struct GreenNode *fgn = g.node;
      uint32_t fc = green_node_num_children(fgn);
      const struct GreenNode *nested = NULL;
      SyntaxKind nested_kind = SYNTAX_KIND_NONE;
      for (uint32_t j = 0; j < fc; j++) {
        GreenElement fg = green_node_child(fgn, j);
        if (fg.kind == GREEN_ELEM_TOKEN &&
            green_token_kind(fg.token) == SK_IDENT) {
          has_name = true;
          break;
        }
        if (fg.kind == GREEN_ELEM_NODE) {
          SyntaxKind k = green_node_kind(fg.node);
          if (k == SK_UNION_DECL || k == SK_STRUCT_DECL) {
            nested = fg.node;
            nested_kind = k;
          }
        }
      }
      if (!has_name && nested) {
        // Flatten: count this nested aggregate's SK_FIELDs.
        const struct GreenNode *inner_list = NULL;
        uint32_t nc = green_node_num_children(nested);
        for (uint32_t j = 0; j < nc; j++) {
          GreenElement ng = green_node_child(nested, j);
          if (ng.kind == GREEN_ELEM_NODE &&
              green_node_kind(ng.node) == SK_FIELD_LIST) {
            inner_list = ng.node;
            break;
          }
        }
        if (inner_list) {
          uint32_t ic = green_node_num_children(inner_list);
          for (uint32_t j = 0; j < ic; j++) {
            GreenElement ig = green_node_child(inner_list, j);
            if (ig.kind == GREEN_ELEM_NODE &&
                green_node_kind(ig.node) == SK_FIELD)
              n_fields++;
          }
        }
        (void)nested_kind;
      } else {
        n_fields++;
      }
    }
  }
  AggregateFieldEntry *scratch =
      n_fields ? arena_alloc(&s->request_arena,
                             n_fields * sizeof(AggregateFieldEntry))
               : NULL;

  // Per-field-annotation hover types are stored only for structs
  // (db.structs.type_result.field_node_types); unions have no such column, so
  // skip the builder for them — a NULL ctx->types makes node_type_builder_push
  // a no-op (no wasted hover map). Member-list storage happens for both.
  bool want_hover = (dk == KIND_STRUCT);
  NodeTypeBuilder fb;
  if (want_hover)
    node_type_builder_begin(s, &fb, base->file_local);
  SemaCtx fctx = {.s = s,
                  .file_green_root = base->file_green_root,
                  .nsid = base->nsid,
                  .enclosing_fn = DEF_ID_NONE,
                  .file_local = base->file_local,
                  .types = want_hover ? &fb : NULL,
                  .decl_ast_map = base->decl_ast_map,
                  .decl_key = base->decl_key,
                  .expected_ret_override = IP_NONE};

  uint32_t fout = 0;
  if (field_list) {
    uint32_t list_count = syntax_node_num_children(field_list);
    for (uint32_t i = 0; i < list_count; i++) {
      SyntaxElement el = syntax_node_child_or_token(field_list, i);
      if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
        if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
          syntax_token_release(el.token);
        continue;
      }
      if (syntax_node_kind(el.node) != SK_FIELD) {
        syntax_node_release(el.node);
        continue;
      }
      Field fld;
      if (!Field_cast(el.node, &fld)) {
        syntax_node_release(el.node);
        continue;
      }
      SyntaxToken *fname_tok = Field_name(&fld);
      SyntaxNode *ftype = Field_type(&fld);
      StrId fname = intern_tok(s, fname_tok);
      if (fname_tok)
        syntax_token_release(fname_tok);

      // Anonymous nested aggregate (`struct { ... union { i, f } ... }`):
      // flatten the inner SK_FIELDs into the parent's flat field list. The
      // wrapper SK_FIELD itself isn't a real member; its INNER fields are
      // siblings of the named outer fields. Per D2.1b.
      //
      // SK_UNION_DECL / SK_STRUCT_DECL sit in the DECL kind-range, not the
      // TYPE range — so Field_type (which filters by is_type_node) returns
      // NULL for these anon-nested wrappers. Search the SK_FIELD's children
      // directly for an SK_UNION_DECL / SK_STRUCT_DECL.
      if (fname.idx == 0) {
        SyntaxNode *nested = NULL;
        SyntaxKind nested_kind = SYNTAX_KIND_NONE;
        uint32_t fcn = syntax_node_num_children(el.node);
        for (uint32_t j = 0; j < fcn; j++) {
          SyntaxElement fel = syntax_node_child_or_token(el.node, j);
          if (fel.kind == SYNTAX_ELEM_NODE && fel.node) {
            SyntaxKind k2 = syntax_node_kind(fel.node);
            if (!nested &&
                (k2 == SK_UNION_DECL || k2 == SK_STRUCT_DECL)) {
              nested = fel.node;
              nested_kind = k2;
            } else {
              syntax_node_release(fel.node);
            }
          } else if (fel.kind == SYNTAX_ELEM_TOKEN && fel.token) {
            syntax_token_release(fel.token);
          }
        }
        if (nested) {
          SyntaxNode *inner_list = NULL;
          if (nested_kind == SK_STRUCT_DECL) {
            StructDef sd2;
            if (StructDef_cast(nested, &sd2))
              inner_list = StructDef_fields(&sd2);
          } else {
            UnionDef ud2;
            if (UnionDef_cast(nested, &ud2))
              inner_list = UnionDef_variants(&ud2);
          }
          if (inner_list) {
            uint32_t ic = syntax_node_num_children(inner_list);
            for (uint32_t j = 0; j < ic; j++) {
              SyntaxElement iel = syntax_node_child_or_token(inner_list, j);
              if (iel.kind != SYNTAX_ELEM_NODE || !iel.node) {
                if (iel.kind == SYNTAX_ELEM_TOKEN && iel.token)
                  syntax_token_release(iel.token);
                continue;
              }
              if (syntax_node_kind(iel.node) != SK_FIELD) {
                syntax_node_release(iel.node);
                continue;
              }
              Field ifld;
              if (!Field_cast(iel.node, &ifld)) {
                syntax_node_release(iel.node);
                continue;
              }
              SyntaxToken *iname_tok = Field_name(&ifld);
              SyntaxNode *itype = Field_type(&ifld);
              StrId iname = intern_tok(s, iname_tok);
              if (iname_tok)
                syntax_token_release(iname_tok);
              IpIndex itypei = IP_ERROR_TYPE;
              if (itype) {
                itypei = resolve_type_expr(&fctx, itype);
                syntax_node_release(itype);
              } else if (iname.idx != 0) {
                db_emit(s, DIAG_ERROR, span_of(&fctx, iel.node),
                        "field '%S' is missing a type annotation", iname);
              } else {
                db_emit(s, DIAG_ERROR, span_of(&fctx, iel.node),
                        "field is missing a type annotation");
              }
              node_type_builder_push(&fctx, iel.node, itypei);
              if (fout < n_fields)
                scratch[fout++] = (AggregateFieldEntry){.name = iname,
                                                        .type = itypei};
              content = db_fp_combine(
                  content, db_fp_combine(db_fp_u64((uint64_t)iname.idx),
                                         db_fp_u64((uint64_t)itypei.v)));
              syntax_node_release(iel.node);
            }
            syntax_node_release(inner_list);
          }
          syntax_node_release(nested);
          if (ftype)
            syntax_node_release(ftype);
          syntax_node_release(el.node);
          continue;
        }
      }

      // DC5 (revised, follow-ups #20): a failed field type stores
      // IP_ERROR_TYPE (resolve_type_expr returns IP_ERROR_TYPE after
      // emitting the unknown-type diag; a MISSING annotation node gets its
      // own diag here). The aggregate is still a valid nominal type, just
      // with one sticky-error member — `u.role` reads on the field then
      // read back IP_ERROR_TYPE and the SK_FIELD_EXPR absorber catches it
      // silently (no cascade "no field 'role' in User"). IP_NONE is now
      // STRICTLY the "field doesn't exist" sentinel inside
      // db_aggregate_field_type's miss path — present-but-bad is always
      // IP_ERROR_TYPE (poison contract: no silent IP_NONE store).
      IpIndex ftypei = IP_ERROR_TYPE;
      if (ftype) {
        ftypei = resolve_type_expr(&fctx, ftype);
        syntax_node_release(ftype);
      } else if (fname.idx != 0) {
        db_emit(s, DIAG_ERROR, span_of(&fctx, el.node),
                "field '%S' is missing a type annotation", fname);
      } else {
        db_emit(s, DIAG_ERROR, span_of(&fctx, el.node),
                "field is missing a type annotation");
      }
      node_type_builder_push(&fctx, el.node, ftypei); // no-op for unions
      scratch[fout++] = (AggregateFieldEntry){.name = fname, .type = ftypei};
      content =
          db_fp_combine(content, db_fp_combine(db_fp_u64((uint64_t)fname.idx),
                                               db_fp_u64((uint64_t)ftypei.v)));
      syntax_node_release(el.node);
    }
    syntax_node_release(field_list);
  }

  // Bulk-append the member range to the shared pool AFTER resolution (nested
  // type_of_def appends to the same pool during field resolution, so deferring
  // keeps our range contiguous), then stamp (field_lo,field_len) on the
  // struct or union table.
  uint32_t lo = (uint32_t)s->aggregate_field_pool.count;
  for (uint32_t i = 0; i < fout; i++)
    vec_push(&s->aggregate_field_pool, &scratch[i]);
  uint32_t row = db_def_row(s, def, dk);
  if (dk == KIND_STRUCT) {
    *(uint32_t *)paged_get(&s->structs.field_lo, row) = lo;
    *(uint32_t *)paged_get(&s->structs.field_len, row) = fout;
  } else { // KIND_UNION
    *(uint32_t *)paged_get(&s->unions.field_lo, row) = lo;
    *(uint32_t *)paged_get(&s->unions.field_len, row) = fout;
  }

  if (want_hover) {
    NodeTypesRange field_range = node_type_builder_end(&fb, NULL);
    type_of_decl_node_types_write(s, def, field_range);
  }

  *fp_out = content;
  return idx;
}

// Build an SK_ENUM_DECL type. Nominal stable index (zir) + variants in
// db.enum_variant_pool. Variant values: explicit bare-literal-int, else
// auto-numbered. *fp_out = combine(IpIndex.v, ⊕ variant name+value).
static IpIndex build_enum_type(const SemaCtx *base, SyntaxNode *enum_node,
                               DefId def, Fingerprint *fp_out) {
  struct db *s = base->s;

  // Stable nominal index. No type-cell publish needed (enums have no
  // self-referential variants); type_of_def's epilogue writes the cell.
  IpIndex idx =
      ip_get(&s->intern, (IpKey){.kind = IPK_ENUM_TYPE,
                                 .enum_type = {.zir_node_id = def.idx}});
  Fingerprint content = db_fp_u64((uint64_t)idx.v);

  EnumDef ed;
  SyntaxNode *variants_list = NULL;
  if (EnumDef_cast(enum_node, &ed))
    variants_list = EnumDef_variants(&ed);

  uint32_t n_variants = 0;
  if (variants_list) {
    uint32_t list_count = syntax_node_num_children(variants_list);
    for (uint32_t i = 0; i < list_count; i++) {
      GreenElement g = green_node_child(syntax_node_green(variants_list), i);
      if (g.kind == GREEN_ELEM_NODE && green_node_kind(g.node) == SK_VARIANT)
        n_variants++;
    }
  }
  EnumVariantEntry *scratch =
      n_variants ? arena_alloc(&s->request_arena,
                               n_variants * sizeof(EnumVariantEntry))
                 : NULL;

  uint32_t vout = 0;
  if (variants_list) {
    uint32_t list_count = syntax_node_num_children(variants_list);
    for (uint32_t i = 0; i < list_count; i++) {
      SyntaxElement el = syntax_node_child_or_token(variants_list, i);
      if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
        if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
          syntax_token_release(el.token);
        continue;
      }
      if (syntax_node_kind(el.node) != SK_VARIANT) {
        syntax_node_release(el.node);
        continue;
      }
      Variant v;
      if (!Variant_cast(el.node, &v)) {
        syntax_node_release(el.node);
        continue;
      }
      SyntaxToken *vname_tok = Variant_name(&v);
      SyntaxNode *v_val = Variant_value(&v);
      StrId vname = intern_tok(s, vname_tok);
      if (vname_tok)
        syntax_token_release(vname_tok);
      // An explicit value must be an int literal (const_eval is deferred).
      // literal_int_signed returns 0 for anything else, which used to
      // SILENTLY value the variant 0 — wrong switch-coverage arithmetic
      // downstream with no diag. Diag + fall back to the positional value
      // so coverage math stays sane.
      int64_t val = (int64_t)vout;
      if (v_val) {
        Literal vlit;
        if (Literal_cast(v_val, &vlit) && Literal_kind(&vlit) == SK_INT_LIT) {
          val = literal_int_signed(v_val);
        } else {
          db_emit(s, DIAG_ERROR, span_of(base, v_val),
                  "enum value must be an integer literal (const_eval not "
                  "yet implemented)");
        }
        syntax_node_release(v_val);
      }
      scratch[vout++] = (EnumVariantEntry){.name = vname, .value = val};
      content =
          db_fp_combine(content, db_fp_combine(db_fp_u64((uint64_t)vname.idx),
                                               db_fp_u64((uint64_t)val)));
      syntax_node_release(el.node);
    }
    syntax_node_release(variants_list);
  }

  uint32_t lo = (uint32_t)s->enum_variant_pool.count;
  for (uint32_t i = 0; i < vout; i++)
    vec_push(&s->enum_variant_pool, &scratch[i]);
  uint32_t row = db_def_row(s, def, KIND_ENUM);
  *(uint32_t *)paged_get(&s->enums.variant_lo, row) = lo;
  *(uint32_t *)paged_get(&s->enums.variant_len, row) = vout;

  *fp_out = content;
  return idx;
}

// ============================================================================
// The declared sort of an effect op `name :: <sort>(params) ret`, read from
// the SK_OP_KIND wrapper's keyword token. `direct`/`ctl`/`val`/`final-ctl` are
// contextual idents the lexer already interned into s->strings — recognized by
// StrId equality against the pre-interned s->names.{DIRECT,CTL,VAL,FINAL_CTL}
// (the same `tok.string_id.idx == s->names.X.idx` pattern the parser uses, no
// strcmp). `fn` is NOT an op sort (it's the value-lambda keyword; the parser
// rejects it in op position). Missing/unrecognized ⇒ OP_CTL (most general — a
// malformed sort must not over-constrain its clauses).
static OpSort op_sort_from_field(struct db *s, SyntaxNode *field) {
  OpSort sort = OP_CTL;
  SyntaxNode *opk = ast_first_child(field, SK_OP_KIND);
  if (!opk)
    return sort;
  uint32_t n = syntax_node_num_children(opk);
  for (uint32_t i = 0; i < n; i++) {
    SyntaxElement e = syntax_node_child_or_token(opk, i);
    if (e.kind == SYNTAX_ELEM_TOKEN && e.token) {
      StrId k = pool_lookup(&s->strings, syntax_token_text(e.token),
                            syntax_token_text_range(e.token).length);
      if (k.idx == s->names.DIRECT.idx)
        sort = OP_DIRECT;
      else if (k.idx == s->names.CTL.idx)
        sort = OP_CTL;
      else if (k.idx == s->names.VAL.idx)
        sort = OP_VAL;
      else if (k.idx == s->names.FINAL_CTL.idx)
        sort = OP_FINAL_CTL;
      syntax_token_release(e.token);
      break; // the op-kind keyword is the first token of SK_OP_KIND
    } else if (e.kind == SYNTAX_ELEM_NODE && e.node) {
      syntax_node_release(e.node);
    }
  }
  syntax_node_release(opk);
  return sort;
}

// build_effect_type — Effects-4a. Intern the IPK_EFFECT_TYPE (identity =
// declaring def, mirrors struct/enum) and, for each op SK_FIELD in the
// effect body, build a fn type with the parent effect baked into its
// effect_row. The {name, fn_type, sort} land in the dedicated effect-op SoA
// (effect_op_names/_types/_sorts) with the range stamped on
// effects.(op_lo,op_len).
//
// Each op's fn type's effect_row is `⟨parent⟩` (a closed one-label row)
// so that an SK_CALL_EXPR on `allocator.malloc(...)` accumulates
// `⟨allocator⟩` into the enclosing fn's body row via row_union. No
// special-case "is this an effect op" check is needed at the call site —
// the type carries the right row already.
// ============================================================================

static IpIndex build_effect_type(const SemaCtx *base, SyntaxNode *eff_node,
                                 DefId def, Fingerprint *fp_out) {
  struct db *s = base->s;

  // Stable nominal index — identity = def.idx. Cell publish so any
  // self-reference in op signatures resolves through the intern type.
  IpIndex idx =
      ip_get(&s->intern, (IpKey){.kind = IPK_EFFECT_TYPE,
                                 .effect_type = {.zir_node_id = def.idx}});
  *db_def_type_cell(s, def) = idx;
  Fingerprint content = db_fp_u64((uint64_t)idx.v);

  // Parent-effect row: a single-label closed row that every op's fn type
  // carries. Built once per effect decl.
  DefId parent_label[1] = {def};
  IpIndex parent_row = row_intern(s, parent_label, 1, IP_EMPTY_EFFECT_ROW);

  SyntaxNode *field_list = NULL;
  EffectDef ed;
  if (EffectDef_cast(eff_node, &ed))
    field_list = EffectDef_ops(&ed);

  // Count ops first so we can arena-stage entries and bulk-append after,
  // keeping the pool range contiguous.
  uint32_t n_ops = 0;
  if (field_list) {
    uint32_t list_count = syntax_node_num_children(field_list);
    for (uint32_t i = 0; i < list_count; i++) {
      GreenElement g = green_node_child(syntax_node_green(field_list), i);
      if (g.kind == GREEN_ELEM_NODE && green_node_kind(g.node) == SK_FIELD)
        n_ops++;
    }
  }
  AggregateFieldEntry *scratch =
      n_ops ? arena_alloc(&s->request_arena,
                          n_ops * sizeof(AggregateFieldEntry))
            : NULL;
  OpSort *sort_scratch =
      n_ops ? arena_alloc(&s->request_arena, n_ops * sizeof(OpSort)) : NULL;

  uint32_t out = 0;
  if (field_list) {
    uint32_t list_count = syntax_node_num_children(field_list);
    for (uint32_t i = 0; i < list_count; i++) {
      SyntaxElement el = syntax_node_child_or_token(field_list, i);
      if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
        if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
          syntax_token_release(el.token);
        continue;
      }
      if (syntax_node_kind(el.node) != SK_FIELD) {
        syntax_node_release(el.node);
        continue;
      }

      // Extract name (first SK_IDENT), op-sort (SK_OP_KIND), SK_PARAM_LIST
      // (optional), and return-type (first type-kind node child).
      SyntaxToken *name_tok = ast_first_token(el.node, SK_IDENT);
      StrId op_name = intern_tok(s, name_tok);
      if (name_tok)
        syntax_token_release(name_tok);
      OpSort op_sort = op_sort_from_field(s, el.node);

      SyntaxNode *params = NULL;
      SyntaxNode *ret_node = NULL;
      uint32_t fc = syntax_node_num_children(el.node);
      for (uint32_t j = 0; j < fc; j++) {
        SyntaxElement fe = syntax_node_child_or_token(el.node, j);
        if (fe.kind != SYNTAX_ELEM_NODE || !fe.node) {
          if (fe.kind == SYNTAX_ELEM_TOKEN && fe.token)
            syntax_token_release(fe.token);
          continue;
        }
        SyntaxKind fk = syntax_node_kind(fe.node);
        if (fk == SK_PARAM_LIST && !params) {
          params = fe.node; // keep ref
          continue;
        }
        if (!ret_node && ore_kind_is_type_node((OreSyntaxKind)fk)) {
          ret_node = fe.node;
          continue;
        }
        syntax_node_release(fe.node);
      }

      // Build the bare fn type (no user-declared effect row on ops), then
      // re-intern with parent_row injected. row_union with the empty row
      // is the identity, so the result row is exactly ⟨parent⟩.
      IpIndex bare_ft = build_fn_type(base, ret_node, params, NULL);
      if (params) syntax_node_release(params);
      if (ret_node) syntax_node_release(ret_node);
      syntax_node_release(el.node);

      if (op_name.idx == 0)
        continue;
      // A failed op signature stores the op WITH IP_ERROR_TYPE instead of
      // dropping it: a dropped op vanishes from the table and every use
      // site then errors "no op '%S' in effect" with no root diag. Also
      // load-bearing for memory safety — `ip_key()` on the non-FN_TYPE
      // IP_ERROR_TYPE primitive would union-read uninitialized
      // `.fn_type.effect_row` bytes into row_union below.
      if (bare_ft.v == IP_NONE.v || ip_is_error(bare_ft)) {
        content = db_fp_combine(content, db_fp_u64((uint64_t)op_name.idx));
        content = db_fp_combine(content, db_fp_u64((uint64_t)IP_ERROR_TYPE.v));
        content = db_fp_combine(content, db_fp_u64((uint64_t)op_sort));
        sort_scratch[out] = op_sort;
        scratch[out++] =
            (AggregateFieldEntry){.name = op_name, .type = IP_ERROR_TYPE};
        continue;
      }

      IpKey k = ip_key(&s->intern, bare_ft);
      k.fn_type.effect_row =
          // NULL node — parent_row injection is a synthetic per-op
          // transform with no surface SyntaxNode; row_union's row_unify
          // only emits if a cycle is detected here (would be a compiler
          // bug, never user code), so the file-start fallback is fine.
          row_union(base, k.fn_type.effect_row, parent_row, NULL);
      // k.params still points into the intern arena (stable for pool
      // lifetime) so the re-intern is safe with no copy.
      IpIndex op_ft = ip_get(&s->intern, k);

      if (op_ft.v == IP_NONE.v)
        continue;

      content = db_fp_combine(content, db_fp_u64((uint64_t)op_name.idx));
      content = db_fp_combine(content, db_fp_u64((uint64_t)op_ft.v));
      content = db_fp_combine(content, db_fp_u64((uint64_t)op_sort));
      sort_scratch[out] = op_sort;
      scratch[out++] = (AggregateFieldEntry){.name = op_name, .type = op_ft};
    }
  }
  if (field_list)
    syntax_node_release(field_list);

  // Bulk-append to the dedicated effect-op SoA after resolution (nested
  // type_of_def calls during op-signature resolution append to the same pools,
  // so deferring keeps our range contiguous). The three columns grow in
  // lockstep — every writer pushes all three — so they stay index-aligned.
  uint32_t lo = (uint32_t)s->effect_op_names.count;
  for (uint32_t i = 0; i < out; i++) {
    vec_push(&s->effect_op_names, &scratch[i].name);
    vec_push(&s->effect_op_types, &scratch[i].type);
    uint8_t sb = (uint8_t)sort_scratch[i];
    vec_push(&s->effect_op_sorts, &sb);
  }
  uint32_t row = db_def_row(s, def, KIND_EFFECT);
  *(uint32_t *)paged_get(&s->effects.op_lo, row) = lo;
  *(uint32_t *)paged_get(&s->effects.op_len, row) = out;

  *fp_out = content;
  return idx;
}

// ============================================================================
// FN_SIGNATURE — a function's (params → ret) type + per-param hover types.
// ============================================================================

const FnSignature *db_query_fn_signature(db_query_ctx *ctx, DefId def) {
  struct db *s = (struct db *)ctx;
  // FN_SIGNATURE is KIND_FUNCTION-only at the routing layer; refuse non-fns
  // BEFORE the guard so the query is TOTAL (a non-fn caller gets NULL, not the
  // db_query_begin "slot kind not wired" assert).
  if (db_def_kind(s, def) != KIND_FUNCTION)
    return NULL;
  DB_QUERY_GUARD(ctx, QUERY_FN_SIGNATURE, (uint64_t)def.idx,
                 /* on_cached */ fn_signature_read(s, def),
                 /* on_cycle  */ NULL,
                 /* on_error  */ NULL);

  // Phase P cutover — install the per-fn signature diag sink.
  DiagBundle *sig_bundle = fn_signature_diags_slot(s, def);
  if (sig_bundle)
    diag_bundle_reset(sig_bundle);
  DiagSink sig_sink = fn_signature_sink_open(s, def);
  db_query_frame_set_sink(ctx, sig_bundle ? &sig_sink : NULL);

  // Producer-side identity reads: this query IS the producer of
  // FN_SIGNATURE for `def`; name/parent_module are self-data, not
  // cross-query inputs. Untracked.
  StrId name = db_get_def_name_untracked(s, def);
  NamespaceId nsid = db_get_def_parent_module_untracked(s, def);

  IpIndex fn_ty = IP_NONE;
  NodeTypesRange sig_range = {0};

  TopLevelEntry e = db_query_top_level_entry(ctx, nsid, name); // CONTENT dep
  if (e.node_ptr.kind != SYNTAX_KIND_NONE) {
    // LINT_UNTRACKED_OK — TOP_LEVEL_ENTRY above carries the body-stable
    // invalidation; a tracked FILE_AST read here would record the
    // whole-file hash as a dep, killing per-decl salsa granularity.
    struct GreenNode *groot = db_read_file_ast_untracked(ctx, e.file);
    if (groot) {
      SyntaxTree *tree = syntax_tree_new(groot);
      SyntaxNode *rroot = syntax_tree_root(tree);
      SyntaxNode *wrapper = syntax_node_ptr_resolve(e.node_ptr, rroot);
      syntax_node_release(rroot);
      if (wrapper) {
        // Phase-3.1 follow-up — FN_SIGNATURE owns the decl_ast_id_map
        // for fns because TYPE_OF_DECL for KIND_FUNCTION delegates here
        // without resolving the wrapper. Build the wrapper-preorder
        // map BEFORE the type walk so emit-time span_of can produce
        // structural anchors.
        decl_ast_id_map_refresh(s, def, wrapper);
        const DeclAstIdMap *decl_map =
            db_get_decl_ast_id_map_untracked(s, def);
        AstId decl_key_id = *(AstId *)vec_get(&s->defs.identity_keys, def.idx); // LINT_UNTRACKED_OK: producer self-data
        SyntaxNode *value = bind_value(wrapper); // SK_LAMBDA_EXPR
        LambdaExpr lam;
        if (value && LambdaExpr_cast(value, &lam)) {
          SyntaxNode *params = LambdaExpr_params(&lam);
          SyntaxNode *ret_node = LambdaExpr_return_type(&lam);
          SyntaxNode *er = LambdaExpr_effect_row(&lam);
          NodeTypeBuilder sb;
          node_type_builder_begin(s, &sb, e.file);
          SemaCtx sctx = {.s = s,
                          .file_green_root = groot,
                          .nsid = nsid,
                          .enclosing_fn = def,
                          .file_local = e.file,
                          .types = &sb,
                          .decl_ast_map = decl_map,
                          .decl_key = decl_key_id.idx,
                          .expected_ret_override = IP_NONE};
          fn_ty = build_fn_type(&sctx, ret_node, params, er);
          sig_range = node_type_builder_end(&sb, NULL);
          if (params)
            syntax_node_release(params);
          if (ret_node)
            syntax_node_release(ret_node);
          if (er)
            syntax_node_release(er);
        }
        if (value)
          syntax_node_release(value);
        syntax_node_release(wrapper);
      }
      syntax_tree_free(tree);
    }
  }

  FnSignature result = {.type = fn_ty, .node_types = sig_range};
  fn_signature_write(s, def, result); // frees old node_types
  db_query_succeed(ctx, QUERY_FN_SIGNATURE, (uint64_t)def.idx,
                   ip_index_is_valid(fn_ty) ? db_fp_u64((uint64_t)fn_ty.v)
                                            : FINGERPRINT_NONE);
  return fn_signature_read(s, def);
}

// build_distinct_type — Slice 6.19. Intern the nominal IPK_DISTINCT_TYPE
// (identity = declaring def, like struct/enum/effect) carrying the resolved
// backing type. No field/variant pool — the backing is the whole payload and
// lives in the interned key. The nominal barrier (`MyT` ≠ its backing) then
// falls out of IpIndex inequality; no coerce code is needed for it.
static IpIndex build_distinct_type(const SemaCtx *base, SyntaxNode *node,
                                   DefId def, Fingerprint *fp_out) {
  struct db *s = base->s;

  // The DistinctType node's child is a normal backing type (`u8` in
  // `distinct u8`), so resolve_type_expr handles it.
  IpIndex backing = IP_ERROR_TYPE;
  DistinctType dt;
  if (DistinctType_cast(node, &dt)) {
    SyntaxNode *bnode = DistinctType_backing(&dt);
    if (bnode) {
      backing = resolve_type_expr(base, bnode);
      syntax_node_release(bnode);
    }
  }

  IpIndex idx = ip_get(
      &s->intern,
      (IpKey){.kind = IPK_DISTINCT_TYPE,
              .distinct_type = {.zir_node_id = def.idx, .backing = backing}});
  *db_def_type_cell(s, def) = idx;
  *fp_out = db_fp_combine(db_fp_u64((uint64_t)idx.v),
                          db_fp_u64((uint64_t)backing.v));
  return idx;
}

// ============================================================================
// TYPE_OF_DECL — a decl's overall type.
// ============================================================================

IpIndex db_query_type_of_def(db_query_ctx *ctx, DefId def) {
  struct db *s = (struct db *)ctx;
  // Primitives (i32, bool, …) are seeded DefIds with no per-kind slot, so
  // they must short-circuit BEFORE the guard — otherwise db_query_begin
  // routes to a non-existent KIND_NONE slot. They're immutable: no dep, no
  // succeed. (resolve_ref → type_of_def reaches here for `x: i32`.)
  IpIndex prim = db_primitive_type_for(s, def);
  if (ip_index_is_valid(prim))
    return prim;
  // on_cycle returns the (wip-published) type cell, NOT IP_NONE — this is how
  // a self-referential nominal (`Node { next: ^Node }`) sees its own
  // in-progress IpIndex during the field loop.
  DB_QUERY_GUARD(ctx, QUERY_TYPE_OF_DECL, (uint64_t)def.idx,
                 /* on_cached */ type_of_decl_read(s, def),
                 /* on_cycle  */ type_of_decl_read(s, def),
                 /* on_error  */ IP_NONE);

  // Phase P cutover — install the per-def TYPE_OF_DECL diag sink.
  DiagBundle *tod_bundle = type_of_decl_diags_slot(s, def);
  if (tod_bundle)
    diag_bundle_reset(tod_bundle);
  DiagSink tod_sink = type_of_decl_sink_open(s, def);
  db_query_frame_set_sink(ctx, tod_bundle ? &tod_sink : NULL);

  DefKind kind = db_def_kind(s, def);

  // Functions delegate to fn_signature (which owns its own top_level_entry
  // dep). No wrapper resolve here, and crucially no top_level_entry dep — so
  // type_of_def(fn) cuts off on a body edit (fn_signature's fp is stable).
  if (kind == KIND_FUNCTION) {
    const FnSignature *sig = db_query_fn_signature(ctx, def);
    IpIndex result = sig ? sig->type : IP_NONE;
    type_of_decl_write(s, def, result);
    db_query_succeed(ctx, QUERY_TYPE_OF_DECL, (uint64_t)def.idx,
                     ip_index_is_valid(result) ? db_fp_u64((uint64_t)result.v)
                                               : FINGERPRINT_NONE);
    return result;
  }

  // Producer-side: TYPE_OF_DECL is computing FOR `def`; reading own
  // identity is self-data, not a cross-query dep.
  StrId name = db_get_def_name_untracked(s, def);
  NamespaceId nsid = db_get_def_parent_module_untracked(s, def);

  IpIndex result = IP_NONE;
  Fingerprint fp = FINGERPRINT_NONE;

  TopLevelEntry e = db_query_top_level_entry(ctx, nsid, name); // CONTENT dep
  if (e.node_ptr.kind != SYNTAX_KIND_NONE) {
    // LINT_UNTRACKED_OK — TOP_LEVEL_ENTRY above is the content firewall;
    // tracked FILE_AST here would force every TYPE_OF_DECL to recompute on any file edit.
    struct GreenNode *groot = db_read_file_ast_untracked(ctx, e.file);
    if (groot) {
      SyntaxTree *tree = syntax_tree_new(groot);
      SyntaxNode *rroot = syntax_tree_root(tree);
      SyntaxNode *wrapper = syntax_node_ptr_resolve(e.node_ptr, rroot);
      syntax_node_release(rroot);
      if (wrapper) {
        // Phase-3.1 follow-up — build the wrapper-relative
        // DeclAstIdMap BEFORE any diag emit. TYPE_OF_DECL owns the
        // map row for this def; every cached downstream query
        // (FN_SIGNATURE / INFER_BODY / BODY_SCOPES) reads it via
        // db_get_decl_ast_id_map_untracked so emit-time span_of can
        // produce structural anchors that survive sibling reparses.
        decl_ast_id_map_refresh(s, def, wrapper);
        const DeclAstIdMap *decl_map =
            db_get_decl_ast_id_map_untracked(s, def);
        AstId decl_key_id = *(AstId *)vec_get(&s->defs.identity_keys, def.idx); // LINT_UNTRACKED_OK: producer self-data
        SemaCtx base = {.s = s,
                        .file_green_root = groot,
                        .nsid = nsid,
                        .enclosing_fn = DEF_ID_NONE,
                        .file_local = e.file,
                        .types = NULL,
                        .decl_ast_map = decl_map,
                        .decl_key = decl_key_id.idx,
                        .expected_ret_override = IP_NONE};

        if (kind == KIND_STRUCT || kind == KIND_UNION) {
          SyntaxNode *val = bind_value(wrapper); // SK_STRUCT/UNION_DECL
          if (val) {
            result = build_struct_type(&base, val, def, &fp);
            syntax_node_release(val);
          }
        } else if (kind == KIND_ENUM) {
          SyntaxNode *val = bind_value(wrapper); // SK_ENUM_DECL
          if (val) {
            result = build_enum_type(&base, val, def, &fp);
            syntax_node_release(val);
          }
        } else if (kind == KIND_CONSTANT || kind == KIND_VARIABLE) {
          // Typed bind: the annotation is the type. RHS check + inferred
          // binds (no annotation) are D2.4.
          SyntaxNode *annot = bind_type_annot(wrapper);
          if (annot) {
            NodeTypeBuilder vb;
            node_type_builder_begin(s, &vb, e.file);
            SemaCtx vctx = base;
            vctx.types = &vb;
            result = resolve_type_expr(&vctx, annot);
            // (D3.4b) ALSO check the value against the annotation so the
            // value's nodes (literals, refs) land in node_types. Without
            // this, hover on `42` in `FOO : i32 : 42` returns IP_NONE
            // (the annotation walked, the value did not). The
            // bidirectional restamp converts comptime_int → i32 at the
            // literal, matching the user-visible coerced type.
            SyntaxNode *val = bind_value(wrapper);
            if (val) {
              if (ip_index_is_valid(result))
                (void)check_expr(&vctx, val, result);
              else
                (void)type_of_expr(&vctx, val);
              syntax_node_release(val);
            }
            NodeTypesRange vr = node_type_builder_end(&vb, NULL);
            type_of_decl_node_types_write(s, def, vr);
            syntax_node_release(annot);
            fp = ip_index_is_valid(result) ? db_fp_u64((uint64_t)result.v)
                                           : FINGERPRINT_NONE;
          } else {
            // Inferred bind (no annotation): the type is the RHS's
            // type (e.g. `x :: 42` → comptime_int). enclosing_fn =
            // NONE → no body scope; refs resolve top-level.
            SyntaxNode *val = bind_value(wrapper);
            if (val) {
              NodeTypeBuilder vb;
              node_type_builder_begin(s, &vb, e.file);
              SemaCtx vctx = base;
              vctx.types = &vb;
              result = type_of_expr(&vctx, val);
              // Fix-D mirror (body-level binds got this guard first): an
              // RHS that produced NO type stores sticky error after a diag
              // instead of a silent IP_NONE. A silent IP_NONE here was the
              // module-wide silence amplifier — e.g. a failed top-level
              // `io :: @import("typo.ore")` typed IP_NONE, and every
              // `io.foo` downstream absorbed without a single diagnostic.
              if (result.v == IP_NONE.v) {
                db_emit(s, DIAG_ERROR, span_of(&vctx, val),
                        "cannot infer type of binding (right-hand side did "
                        "not produce a type)");
                result = IP_ERROR_TYPE;
              }
              NodeTypesRange vr = node_type_builder_end(&vb, NULL);
              type_of_decl_node_types_write(s, def, vr);
              syntax_node_release(val);
              fp = ip_index_is_valid(result) ? db_fp_u64((uint64_t)result.v)
                                             : FINGERPRINT_NONE;
            }
          }
        } else if (kind == KIND_EFFECT) {
          // Effects-4a — nominal effect type + per-op fn types stored
          // in db.aggregate_field_pool, range stamped on db.effects.
          // (op_lo,op_len). Each op's fn type carries the parent effect
          // in its row.
          SyntaxNode *val = bind_value(wrapper); // SK_EFFECT_DECL
          if (val) {
            result = build_effect_type(&base, val, def, &fp);
            syntax_node_release(val);
          }
        } else if (kind == KIND_DISTINCT) {
          // Slice 6.19 — nominal newtype: IPK_DISTINCT_TYPE{def, backing}.
          // The RHS is the SK_DISTINCT_TYPE node; build_distinct_type
          // resolves its backing child.
          SyntaxNode *val = bind_value(wrapper); // SK_DISTINCT_TYPE
          if (val) {
            result = build_distinct_type(&base, val, def, &fp);
            syntax_node_release(val);
          }
        } else {
          // Unreachable for valid input: KIND_FUNCTION returns early and
          // primitives short-circuit before the guard, so every def reaching
          // here is one of the kinds above. A new DefKind wired into the
          // pipeline but not here lands in this else — a loud tripwire beats a
          // silently-IP_NONE type that would mis-propagate downstream.
          assert(0 && "type_of_def: unhandled DefKind — add a case");
        }
        syntax_node_release(wrapper);
      }
      syntax_tree_free(tree);
    }
  }

  // For nominals the cell already holds the wip index (== result); for binds
  // this records the annotation type. Either way the slot's result column is
  // current before succeed.
  type_of_decl_write(s, def, result);
  db_query_succeed(ctx, QUERY_TYPE_OF_DECL, (uint64_t)def.idx, fp);
  return result;
}

// ============================================================================
// NAMESPACE_TYPE — the file-as-namespace export type (Phase D2.2).
//
// A namespace's type is the nominal struct whose members are its `pub`
// top-level decls. Identity is the nsid (inline-encoded in the pool, stable +
// deduped — like struct/enum). The exported (name → DefId) member list lives
// in db.namespace_field_pool (range stamped on db.namespaces.(field_lo,len)),
// NOT the intern pool. Member TYPES are resolved LAZILY by consumers via
// db_query_type_of_def(member.def) — so namespace_type depends only on
// NAMESPACE_ITEMS (membership, incl. meta/pub) + def_identity per pub member,
// and is firewalled from body edits. fp folds (member name, member def), so it
// flips on add/remove/rename/visibility-toggle of a pub decl, stable otherwise.
// ============================================================================

IpIndex db_query_namespace_type(db_query_ctx *ctx, NamespaceId nsid) {
  struct db *s = (struct db *)ctx;
  DB_QUERY_GUARD(ctx, QUERY_NAMESPACE_TYPE, (uint64_t)nsid.idx,
                 /* on_cached */ namespace_type_read(s, nsid),
                 /* on_cycle  */ IP_NONE,
                 /* on_error  */ IP_NONE);

  FileArray items = db_query_namespace_items(ctx, nsid); // membership dep
  const NamespaceItem *arr = (const NamespaceItem *)items.data;

  // Append each pub member (name → DefId) directly into the shared pool — the
  // range stays contiguous because nothing else appends to namespace_field_pool
  // during this loop (db_query_def_identity only touches the defs table). lo is
  // captured before the loop; (field_lo,field_len) stamped after.
  uint32_t lo = (uint32_t)s->namespace_field_pool.count;
  Fingerprint content = FINGERPRINT_NONE;
  for (uint32_t i = 0; i < items.count; i++) {
    if ((arr[i].meta & META_VIS_MASK) != VIS_PUBLIC)
      continue;
    DefId def = db_query_def_identity(ctx, nsid, arr[i].id); // identity dep
    DeclEntry m = {.name = arr[i].name, .def = def};
    vec_push(&s->namespace_field_pool, &m);
    content = db_fp_combine(content,
                            db_fp_combine(db_fp_u64((uint64_t)arr[i].name.idx),
                                          db_fp_u64((uint64_t)def.idx)));
  }
  // Producer-side writes: NAMESPACE_TYPE owns these columns. The
  // typed setter in capability.c encodes the safe-co-product
  // contract (same nsid both sides of the slot writes).
  db_write_namespace_type_outputs(
      s, nsid, lo, (uint32_t)s->namespace_field_pool.count - lo);

  IpIndex result =
      ip_get(&s->intern, (IpKey){.kind = IPK_NAMESPACE_TYPE,
                                 .namespace_type = {.nsid = nsid}});
  namespace_type_write(s, nsid, result);
  // fp = the (name,def) content fold ONLY — for an inline nominal result.v is a
  // per-nsid constant, so folding it would add no discrimination.
  db_query_succeed(ctx, QUERY_NAMESPACE_TYPE, (uint64_t)nsid.idx, content);
  return result;
}
