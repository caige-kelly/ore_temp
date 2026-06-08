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
  uint64_t key = syntax_node_ptr_hash(syntax_node_ptr_new(node));
  hashmap_put(&b->types, key, (void *)(uintptr_t)type.v);
  // POSITION-INDEPENDENT: fold ONLY the type value, in push order. The node
  // key (kind+byte-range) is deliberately NOT folded — a pure trivia shift
  // would change it without changing any type, so including it would break
  // the firewall. Push order is structural (the descent is position-
  // independent), so order-folded type values catch add/remove/reorder/
  // retype; AST-structure changes reach AST-consumers via their own deps.
  b->fp = db_fp_combine(b->fp, db_fp_u64((uint64_t)type.v));
}

NodeTypesRange node_type_builder_end(NodeTypeBuilder *b, Fingerprint *out_fp) {
  if (out_fp)
    *out_fp = b->fp;
  NodeTypesRange out = {.types = b->types};
  b->types = (HashMap){0};
  return out;
}

IpIndex node_types_range_lookup(struct db *s, NodeTypesRange range,
                                SyntaxNode *node) {
  (void)s;
  if (!node || !hashmap_is_initialized(&range.types))
    return IP_NONE;
  uint64_t key = syntax_node_ptr_hash(syntax_node_ptr_new(node));
  void *v = hashmap_get(&range.types, key);
  if (!v)
    return IP_NONE;
  return (IpIndex){.v = (uint32_t)(uintptr_t)v};
}

IpIndex db_lookup_node_type(const SemaCtx *ctx, SyntaxNode *node) {
  if (!ctx || !ctx->types || !node)
    return IP_NONE;
  NodeTypeBuilder *b = ctx->types;
  if (!hashmap_is_initialized(&b->types))
    return IP_NONE;
  uint64_t key = syntax_node_ptr_hash(syntax_node_ptr_new(node));
  void *v = hashmap_get(&b->types, key);
  if (!v)
    return IP_NONE;
  return (IpIndex){.v = (uint32_t)(uintptr_t)v};
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
static StrId path_expr_leaf_name(struct db *s, SyntaxNode *path) {
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

static DiagAnchor span_of(const SemaCtx *ctx, SyntaxNode *node) {
  // Prefer a structural DIAG_ANCHOR_BODY anchor when the active
  // SemaCtx has a decl_ast_map (built by TYPE_OF_DECL over the
  // wrapper). RelAstId is the node's preorder position in the
  // wrapper subtree — survives sibling reparses by re-walking the
  // current tree at publish time. See ast_id.h.
  if (ctx->decl_ast_map && node) {
    uint32_t rel;
    if (decl_ast_id_lookup(ctx->decl_ast_map, node, &rel))
      return diag_anchor_body((uint16_t)ctx->file_local.idx,
                              (DeclKey)ctx->decl_key, (RelAstId)rel);
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
static IpIndex resolve_user_type_name(struct db *s, NamespaceId nsid,
                                      StrId name) {
  if (name.idx == 0)
    return IP_NONE;
  NamespaceScopes sc = db_query_namespace_scopes(s, nsid); // dep
  if (sc.internal.idx == SCOPE_ID_NONE.idx)
    return IP_NONE;
  DefId target = db_query_resolve_ref(s, sc.internal, name); // dep
  if (target.idx == DEF_ID_NONE.idx)
    return IP_NONE;
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
        if (name.idx != 0)
          db_emit(s, DIAG_ERROR, span_of(ctx, el.node),
                  "unknown effect '%S'", name);
        syntax_node_release(el.node);
        syntax_node_release(label_list);
        return IP_ERROR_TYPE;
      }
      labels[out++] = target;
      node_type_builder_push(ctx, el.node, IP_NONE); // hover placeholder
      syntax_node_release(el.node);
    }
    syntax_node_release(label_list);
  }

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
  SemaCtx sub_ctx;
  const SemaCtx *eff_ctx = ctx;
  if (!ctx->row_name_map) {
    hashmap_init(&row_name_map_local);
    sub_ctx = *ctx;
    sub_ctx.row_name_map = &row_name_map_local;
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

  IpIndex *params = NULL;
  if (n_params > 0) {
    params = arena_alloc(&s->request_arena, n_params * sizeof(IpIndex));
    if (!params) {
      if (eff_ctx == &sub_ctx)
        hashmap_free(&row_name_map_local);
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
      if (pti.v == IP_NONE.v || ip_is_error(pti)) {
        syntax_node_release(el.node);
        if (eff_ctx == &sub_ctx)
          hashmap_free(&row_name_map_local);
        return ip_is_error(pti) ? IP_ERROR_TYPE : IP_NONE;
      }
      params[out++] = pti;
      node_type_builder_push(eff_ctx, el.node, pti); // hover on the param node
      syntax_node_release(el.node);
    }
  }

  IpIndex ret;
  if (!ret_node) {
    ret = IP_VOID_TYPE; // implicit void
  } else {
    ret = resolve_type_expr(eff_ctx, ret_node);
    if (ret.v == IP_NONE.v || ip_is_error(ret)) {
      if (eff_ctx == &sub_ctx)
        hashmap_free(&row_name_map_local);
      return ip_is_error(ret) ? IP_ERROR_TYPE : IP_NONE;
    }
  }

  // Effects-1 — interned effect row. NULL → IP_EMPTY_EFFECT_ROW (pure).
  IpIndex er = build_effect_row(eff_ctx, effect_row_node);
  if (er.v == IP_NONE.v || ip_is_error(er)) {
    if (eff_ctx == &sub_ctx)
      hashmap_free(&row_name_map_local);
    return ip_is_error(er) ? IP_ERROR_TYPE : IP_NONE;
  }

  IpKey key = {.kind = IPK_FN_TYPE,
               .fn_type = {.ret = ret,
                           .modifiers = 0,
                           .params = params,
                           .n_params = n_params,
                           .effect_row = er},
               .src_arena = params ? &s->request_arena : NULL,
               .src_gen = s->request_arena.generation};
  IpIndex out = ip_get(&s->intern, key);
  if (eff_ctx == &sub_ctx)
    hashmap_free(&row_name_map_local);
  return out;
}

// ============================================================================
// resolve_type_expr — a type-position node → IpIndex.
// ============================================================================

// Cross-layer entry: const_eval.c calls into resolve_type_expr from a
// place that doesn't have a SemaCtx in scope (the @sizeOf / @alignOf
// comptime-eval path). Builds a minimal type-position SemaCtx (no fn
// frame, no node_types builder, no body anchors) and dispatches.
// const_eval is the ONLY authorized caller — do not generalize without
// a second consumer to justify the type_layer.h surface widening.
//
// J3: takes FileId directly. Pre-J3 the shim took NamespaceId and
// grabbed files[0] from the namespace, which mis-attributed any diag
// spans (and would mis-resolve any future body_scope-using type
// expressions) for multi-file modules.
IpIndex resolve_type_expr_from_const_eval(struct db *s, FileId fid,
                                          SyntaxNode *node) {
  struct GreenNode *groot = NULL;
  if (file_id_valid(fid))
    groot = db_read_file_ast(s, fid);
  SemaCtx ctx = {.s = s,
                 .file_green_root = groot,
                 .nsid = db_get_file_namespace(s, fid),
                 .enclosing_fn = DEF_ID_NONE,
                 .file_local = fid,
                 .types = NULL,
                 .expected_ret_override = IP_NONE};
  return resolve_type_expr(&ctx, node);
}

IpIndex resolve_type_expr(const SemaCtx *ctx, SyntaxNode *node) {
  if (!node)
    return IP_NONE;
  struct db *s = ctx->s;
  NamespaceId nsid = ctx->nsid;
  SyntaxKind k = syntax_node_kind(node);
  IpIndex result = IP_NONE;

  switch ((OreSyntaxKind)k) {
  // Slice 6.19 note: bare `MyT` resolving to a KIND_DISTINCT decl already works
  // here (resolve_user_type_name → type_of_def → IPK_DISTINCT_TYPE). When the
  // 6.18 "bare ref must be qualified" rule lands, its bare-permitted allow-list
  // must include KIND_DISTINCT (and any future type-alias kind).
  case SK_REF_TYPE:
  case SK_REF_EXPR: {
    SyntaxToken *name_tok = ast_first_token(node, SK_IDENT);
    StrId name = intern_tok(s, name_tok);
    if (name_tok)
      syntax_token_release(name_tok);
    result = resolve_user_type_name(s, nsid, name);
    if (result.v == IP_NONE.v && name.idx != 0) {
      db_emit(s, DIAG_ERROR, span_of(ctx, node), "unknown type '%S'", name);
      result = IP_ERROR_TYPE; // sticky — consumer absorbs without re-diag
    }
    break;
  }
  case SK_PATH_TYPE:
  case SK_PATH_EXPR: {
    StrId name = path_expr_leaf_name(s, node);
    result = resolve_user_type_name(s, nsid, name);
    if (result.v == IP_NONE.v && name.idx != 0) {
      db_emit(s, DIAG_ERROR, span_of(ctx, node), "unknown type '%S'", name);
      result = IP_ERROR_TYPE;
    }
    break;
  }

  case SK_CONST_TYPE: {
    // Standalone `const T` → the underlying type (const-ness is a binding
    // property in DefMeta, not an intern-pool type modifier).
    ConstType ct;
    if (ConstType_cast(node, &ct)) {
      SyntaxNode *inner = ConstType_inner(&ct);
      if (inner) {
        result = resolve_type_expr(ctx, inner);
        syntax_node_release(inner);
      }
    }
    break;
  }

  case SK_OPTIONAL_TYPE: {
    OptionalType ot;
    if (OptionalType_cast(node, &ot)) {
      SyntaxNode *inner = OptionalType_inner(&ot);
      IpIndex elem = resolve_type_expr(ctx, inner);
      if (inner)
        syntax_node_release(inner);
      if (ip_is_error(elem)) {
        result = IP_ERROR_TYPE; // don't intern ?error
      } else if (elem.v != IP_NONE.v) {
        IpKey key = {.kind = IPK_OPTIONAL_TYPE,
                     .optional_type = {.elem = elem}};
        result = ip_get(&s->intern, key);
      }
    }
    break;
  }

  case SK_PTR_TYPE:
  case SK_SLICE_TYPE:
  case SK_MANY_PTR_TYPE: {
    SyntaxNode *child = NULL;
    if (k == SK_PTR_TYPE) {
      PtrType pt;
      PtrType_cast(node, &pt);
      child = PtrType_pointee(&pt);
    } else if (k == SK_SLICE_TYPE) {
      SliceType st;
      SliceType_cast(node, &st);
      child = SliceType_element(&st);
    } else {
      ManyPtrType mt;
      ManyPtrType_cast(node, &mt);
      child = ManyPtrType_element(&mt);
    }
    bool is_const = false;
    if (child && syntax_node_kind(child) == SK_CONST_TYPE) {
      ConstType inner;
      if (ConstType_cast(child, &inner)) {
        SyntaxNode *unwrap = ConstType_inner(&inner);
        is_const = true;
        syntax_node_release(child);
        child = unwrap;
      }
    }
    IpIndex elem = child ? resolve_type_expr(ctx, child) : IP_NONE;
    if (child)
      syntax_node_release(child);
    if (ip_is_error(elem)) {
      result = IP_ERROR_TYPE; // don't intern ^error / []error / [^]error
    } else if (elem.v != IP_NONE.v) {
      IpKey key = {0};
      if (k == SK_PTR_TYPE) {
        key.kind = IPK_PTR_TYPE;
        key.ptr_type.elem = elem;
        key.ptr_type.is_const = is_const;
      } else if (k == SK_SLICE_TYPE) {
        key.kind = IPK_SLICE_TYPE;
        key.slice_type.elem = elem;
        key.slice_type.is_const = is_const;
      } else {
        key.kind = IPK_MANY_PTR_TYPE;
        key.many_ptr_type.elem = elem;
        key.many_ptr_type.is_const = is_const;
      }
      result = ip_get(&s->intern, key);
    }
    break;
  }

  case SK_ARRAY_TYPE: {
    // Literal-int sizes only; non-literal sizes need const_eval (deferred).
    ArrayType at;
    if (!ArrayType_cast(node, &at))
      break;
    SyntaxNode *size_node = ArrayType_size(&at);
    SyntaxNode *elem_node = ArrayType_element(&at);
    if (!size_node) {
      db_emit(s, DIAG_ERROR, span_of(ctx, node),
              "array type missing size expression");
      if (elem_node)
        syntax_node_release(elem_node);
      result = IP_ERROR_TYPE;
      break;
    }
    Literal lit;
    if (!Literal_cast(size_node, &lit) || Literal_kind(&lit) != SK_INT_LIT) {
      db_emit(
          s, DIAG_ERROR, span_of(ctx, size_node),
          "array size must be a literal int (const_eval not yet implemented)");
      syntax_node_release(size_node);
      if (elem_node)
        syntax_node_release(elem_node);
      result = IP_ERROR_TYPE;
      break;
    }
    SyntaxToken *size_tok = Literal_token(&lit);
    uint64_t size = size_tok ? parse_int_literal(size_tok) : 0;
    if (size_tok)
      syntax_token_release(size_tok);
    IpIndex elem = elem_node ? resolve_type_expr(ctx, elem_node) : IP_NONE;
    // (D3.4b) Stamp the size literal as comptime_int. Type-position
    // literals never reach body-inference (they're inside struct-field
    // type annotations at top level), so without this push hover at the
    // literal returns IP_NONE.
    node_type_builder_push(ctx, size_node, IP_COMPTIME_INT_TYPE);
    syntax_node_release(size_node);
    if (elem_node)
      syntax_node_release(elem_node);
    if (ip_is_error(elem)) {
      result = IP_ERROR_TYPE; // don't intern [N]error
      break;
    }
    if (elem.v == IP_NONE.v)
      break;
    IpKey key = {.kind = IPK_ARRAY_TYPE,
                 .array_type = {.elem = elem, .size = size}};
    result = ip_get(&s->intern, key);
    break;
  }

  case SK_FN_TYPE: {
    FnType ft;
    if (!FnType_cast(node, &ft))
      break;
    SyntaxNode *ret = FnType_return_type(&ft);
    SyntaxNode *params = FnType_params(&ft);
    SyntaxNode *er = FnType_effect_row(&ft);
    result = build_fn_type(ctx, ret, params, er);
    if (ret)
      syntax_node_release(ret);
    if (params)
      syntax_node_release(params);
    if (er)
      syntax_node_release(er);
    break;
  }

  // Slice 6.19: a distinct type-former reaching resolve_type_expr means it
  // appeared INLINE (a param/field/return annotation), not as a named bind's
  // RHS — that legitimate case goes through type_of_def's KIND_DISTINCT arm
  // and never lands here. An anonymous distinct has no decl to anchor its
  // nominal identity, so it is rejected.
  case SK_DISTINCT_TYPE:
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "distinct must be named: `MyT :: distinct <type>`");
    result = IP_ERROR_TYPE;
    break;

  default:
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "type-expression kind %d not yet supported", (int)k);
    result = IP_ERROR_TYPE;
    break;
  }

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
// range contiguous. A field that fails to resolve stores type=IP_NONE (the
// struct stays a valid nominal — no whole-type cancel). *fp_out =
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
              IpIndex itypei =
                  itype ? resolve_type_expr(&fctx, itype) : IP_NONE;
              if (itype)
                syntax_node_release(itype);
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
      // emitting the unknown-type diag). The aggregate is still a valid
      // nominal type, just with one sticky-error member — `u.role` reads
      // on the field then read back IP_ERROR_TYPE and the SK_FIELD_EXPR
      // absorber catches it silently (no cascade "no field 'role' in
      // User"). IP_NONE remains the per-field sentinel for "field doesn't
      // exist" inside db_aggregate_field_type's miss path.
      IpIndex ftypei = ftype ? resolve_type_expr(&fctx, ftype) : IP_NONE;
      if (ftype)
        syntax_node_release(ftype);
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
      int64_t val = v_val ? literal_int_signed(v_val) : (int64_t)vout;
      if (v_val)
        syntax_node_release(v_val);
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
// build_effect_type — Effects-4a. Intern the IPK_EFFECT_TYPE (identity =
// declaring def, mirrors struct/enum) and, for each op SK_FIELD in the
// effect body, build a fn type with the parent effect baked into its
// effect_row. The (name → fn_type) entries land in the shared
// aggregate_field_pool with the range stamped on effects.(op_lo,op_len).
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

      // Extract name (first SK_IDENT), SK_PARAM_LIST (optional), and
      // return-type (first type-kind node child).
      SyntaxToken *name_tok = ast_first_token(el.node, SK_IDENT);
      StrId op_name = intern_tok(s, name_tok);
      if (name_tok)
        syntax_token_release(name_tok);

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

      if (bare_ft.v == IP_NONE.v || op_name.idx == 0)
        continue;

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
      scratch[out++] = (AggregateFieldEntry){.name = op_name, .type = op_ft};
    }
  }
  if (field_list)
    syntax_node_release(field_list);

  // Bulk-append to the shared pool after resolution (nested type_of_def
  // calls during op-signature resolution append to the same pool, so
  // deferring keeps our range contiguous).
  uint32_t lo = (uint32_t)s->aggregate_field_pool.count;
  for (uint32_t i = 0; i < out; i++)
    vec_push(&s->aggregate_field_pool, &scratch[i]);
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
    // LINT_UNTRACKED_OK — TOP_LEVEL_ENTRY above carries body-stable
    // invalidation; tracked FILE_AST here kills per-decl salsa
    // granularity (Phase 3.1).
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
    // LINT_UNTRACKED_OK — TOP_LEVEL_ENTRY above is the body-stable
    // content firewall; tracked FILE_AST here would force every TYPE_OF_DECL
    // to recompute on any file edit (Phase 3.1).
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
        }
        // Anything else: IP_NONE.
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
