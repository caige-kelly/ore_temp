// Phase 2 — unified expression evaluator. See eval.h for the contract.
//
// This file is the SINGLE source of truth for "what does this expression
// evaluate to (type + comptime value)." It replaces `resolve_type_expr`'s
// case-by-case dispatch — the public `resolve_type_expr` now lives in
// type.c as a thin wrapper that calls `eval_expr` and gates the result on
// "type == IP_TYPE_TYPE OR a TYPE_VAR hole."
//
// `type_of_expr` (in infer.c) and `const_eval.c` are NOT migrated in
// Phase 2 — they continue to exist as parallel evaluators. Phase 3+
// folds them into eval_expr. There is no glue between old and new;
// eval_expr is self-contained.

#define ORE_ENGINE_PRIVATE

#include "eval.h"

#include "capability.h"      // db_read_file_ast, db_get_def_*_untracked
#include "engine.h"
#include "engine_internal.h"
#include "result_columns.h"  // db.h, ids.h, intern_pool.h, syntax.h
#include "type_layer.h"
#include "coerce.h"          // type_resolve
#include "builtins.h"        // db_builtin_kind_of
#include "layout.h"          // db_layout_of_type (Phase 6 Batch 3 @sizeOf/@alignOf)
#include "tv_inspect.h"      // tv_value_semantic_eq / tv_value_in_range (Phase 6 Batch 4c)

#include "../diag/diag.h"

#include "../../ast/ast.h"
#include "../../ast/ast_decl.h"
#include "../../ast/ast_expr.h"
#include "../../ast/ast_stmt.h" // BlockStmt / ReturnStmt (CTFE body evaluator)
#include "../../ast/ast_type.h"
#include "../../support/data_structure/hashmap.h"
#include "../../support/data_structure/stringpool.h"
#include "../../syntax/syntax_kind.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>  // strtod for parse_float_literal_text

// Externs reused from type.c — these are the namespace + body lookup
// chain that today's `resolve_type_expr` uses; eval_expr's SK_REF arm
// will call them in Phase 2's ref-port.
extern NamespaceScopes db_query_namespace_scopes(db_query_ctx *ctx,
                                                 NamespaceId nsid);
extern DefId           db_query_resolve_ref(db_query_ctx *ctx, ScopeId scope,
                                            StrId name);
extern IpIndex         db_query_type_of_def(db_query_ctx *ctx, DefId def);
extern SyntaxNodePtr   db_body_scope_lookup(db_query_ctx *ctx, DefId fn_def,
                                            SyntaxNode *use_node, StrId name);
extern IpIndex         db_primitive_type_for(struct db *s, DefId def);
extern DefKind         db_def_kind(struct db *s, DefId d);

// Helpers from type.c / infer.c that eval_expr reuses.
extern IpIndex resolve_type_name_checked(const SemaCtx *ctx, SyntaxNode *node,
                                         StrId name);
extern IpIndex build_fn_type(const SemaCtx *ctx, SyntaxNode *ret,
                             SyntaxNode *param_list,
                             SyntaxNode *effect_row_node);
extern IpIndex  type_from_lit_token(SyntaxKind tk);  // infer.c
extern uint64_t parse_int_literal(SyntaxToken *tok); // type.c
extern StrId    path_expr_leaf_name(struct db *s, SyntaxNode *path); // type.c
extern DiagAnchor span_of(const SemaCtx *ctx, SyntaxNode *node);     // type.c

// Phase 4+5 — value-position synthesis fallback. Formerly type_of_expr_impl
// inside infer.c; renamed + exported so eval_expr's default arm delegates
// here for arms that haven't been hoisted into eval_expr's switch yet.
// Returns IpIndex (the type half); the value half is IP_NONE at the
// delegation site until each arm is hoisted with TypedValue threading.
extern IpIndex infer_value_position(const SemaCtx *ctx, SyntaxNode *node);

// Phase 6 Batch 2 — diag-string helper for unsupported prefix ops.
extern const char *opkind_name(SyntaxKind k);

// Phase 6 Batch 3 — top-level lookup for SK_FIELD_EXPR namespace-member arm.
extern TopLevelEntry db_query_top_level_entry(db_query_ctx *ctx,
                                              NamespaceId nsid, StrId name);
extern DefId         db_query_def_identity(db_query_ctx *ctx,
                                           NamespaceId nsid, AstId id);

// Phase 6 Batch 4b — peer_unify (already non-static from Phase 3) +
// handle_if_cond (newly promoted) for SK_IF_EXPR arm.
extern IpIndex peer_unify(IpIndex a, IpIndex b);
extern void    handle_if_cond(const SemaCtx *ctx, SyntaxNode *cond,
                              SyntaxNode *capture);

// Phase 6 Batch 4c — SK_SWITCH_EXPR arm helpers.
extern bool    pattern_is_wildcard(SyntaxNode *p);
extern IpIndex infer_switch(const SemaCtx *ctx, SyntaxNode *node,
                            IpIndex expected);

// Phase 6 Batch 4a — TypedValue-returning binop helpers (promoted in Batch 1).
extern TypedValue binop_arith(const SemaCtx *ctx, SyntaxNode *node,
                              SyntaxKind opk, TypedValue l, TypedValue r);
extern TypedValue binop_compare(const SemaCtx *ctx, SyntaxNode *node,
                                SyntaxKind opk, TypedValue l, TypedValue r);
extern TypedValue binop_logical(const SemaCtx *ctx, SyntaxNode *node,
                                SyntaxKind opk, TypedValue l, TypedValue r);
extern TypedValue binop_bitop(const SemaCtx *ctx, SyntaxNode *node,
                              SyntaxKind opk, TypedValue l, TypedValue r);
extern TypedValue binop_orelse(const SemaCtx *ctx, SyntaxNode *node,
                               TypedValue l);

// Phase 6 — parse a float literal token's text. Handles `_` separators and
// the `1.5e10` form via strtod. Returns false on parse failure. Duplicate
// of const_eval.c's static helper; both die together when const_eval.c is
// deleted in Batch 6, leaving this as the sole copy. Used by Batch 2's
// SK_FLOAT_LIT value-half wiring (currently unused until that arm lands).
// Phase 6 Batch 2 — prefix-op value folders. Each takes an interned value
// and returns either a fresh interned result or IP_NONE if not foldable
// (mixed kind / overflow / unsupported type combo).
static IpIndex tv_prefix_neg(struct db *s, IpIndex v) {
  if (v.v == IP_NONE.v || ip_is_error(v)) return IP_NONE;
  IpTag tag = ip_tag(&s->intern, v);
  if (tag == IP_TAG_INT_VALUE) {
    IpKey k = ip_key(&s->intern, v);
    if (k.int_value.value == INT64_MIN) return IP_NONE;  // overflow
    IpKey nk = {.kind = IPK_INT_VALUE,
                .int_value = {.type = k.int_value.type,
                              .value = -k.int_value.value}};
    return ip_get(&s->intern, nk);
  }
  if (tag == IP_TAG_FLOAT_VALUE) {
    IpKey k = ip_key(&s->intern, v);
    IpKey nk = {.kind = IPK_FLOAT_VALUE,
                .float_value = {.type = k.float_value.type,
                                .value = -k.float_value.value}};
    return ip_get(&s->intern, nk);
  }
  return IP_NONE;
}

static IpIndex tv_prefix_comp(struct db *s, IpIndex v) {
  if (v.v == IP_NONE.v || ip_is_error(v)) return IP_NONE;
  if (ip_tag(&s->intern, v) != IP_TAG_INT_VALUE) return IP_NONE;
  IpKey k = ip_key(&s->intern, v);
  IpKey nk = {.kind = IPK_INT_VALUE,
              .int_value = {.type = k.int_value.type,
                            .value = ~k.int_value.value}};
  return ip_get(&s->intern, nk);
}

static IpIndex tv_prefix_not(IpIndex v) {
  if (v.v == IP_BOOL_TRUE.v) return IP_BOOL_FALSE;
  if (v.v == IP_BOOL_FALSE.v) return IP_BOOL_TRUE;
  return IP_NONE;
}

__attribute__((unused))
static bool parse_float_literal_text(SyntaxToken *tok, double *out) {
  const char *txt = syntax_token_text(tok);
  uint32_t len = syntax_token_text_range(tok).length;
  char buf[64];
  if (len >= sizeof(buf))
    return false;
  uint32_t w = 0;
  for (uint32_t i = 0; i < len; i++)
    if (txt[i] != '_')
      buf[w++] = txt[i];
  buf[w] = '\0';
  char *end = NULL;
  double v = strtod(buf, &end);
  if (end == buf)
    return false;
  *out = v;
  return true;
}

// Cycle guard for SK_BIND_DECL RHS recursion. A `c :: c` self-ref or a
// mutual `a :: b; b :: a` cycle would loop forever otherwise. The compiler
// is single-threaded, so a file-local linked-list stack suffices. Each
// frame is identified by (file, ptr-derived-hash) — same key shape that
// const_eval.c uses, so the diag stays consistent with M2(g)/(h) wording.
typedef struct EvalCycleFrame {
  struct EvalCycleFrame *prev;
  uint32_t              file_idx;
  uint64_t              key_hash;
} EvalCycleFrame;

static EvalCycleFrame *g_eval_cycle_top = NULL;

static bool eval_cycle_contains(uint32_t file_idx, uint64_t key_hash) {
  for (EvalCycleFrame *f = g_eval_cycle_top; f; f = f->prev)
    if (f->file_idx == file_idx && f->key_hash == key_hash)
      return true;
  return false;
}

// Phase 6 — depth-cap walk. Returns the current chain length (0 when empty).
// Callers bail with "const chain too deep (max 64)" when >= 64 before push.
#define EVAL_CYCLE_DEPTH_MAX 64
static uint32_t eval_cycle_depth(void) {
  uint32_t n = 0;
  for (EvalCycleFrame *f = g_eval_cycle_top; f; f = f->prev) n++;
  return n;
}

// CTFE (comptime function execution) — a call frame binding the executing
// comptime fn's params + locals to their folded values. `eval_name` consults
// the TOP frame (Tier-0) BEFORE any body-scope lookup, so a param/local ref
// inside a comptime body resolves to the ARGUMENT / initializer value rather
// than the static SyntaxNode. File-local + stack-chained like EvalCycleFrame
// (SemaCtx is const, so a frame can't live on it). Top-frame-only: a comptime
// fn sees ITS own params/locals, never the caller's (function scoping; no
// comptime closures in the MVP).
typedef struct EvalBinding {
  StrId      name;
  TypedValue val;
} EvalBinding;

typedef struct EvalFrame {
  struct EvalFrame *prev;
  EvalBinding      *bindings; // caller-owned stack array, length `cap`
  uint32_t          count;
  uint32_t          cap;
} EvalFrame;

static EvalFrame *g_eval_frame_top = NULL;

// Resolve a name in the TOP CTFE frame only. Returns true + fills *out on hit.
static bool eval_frame_lookup(StrId name, TypedValue *out) {
  EvalFrame *f = g_eval_frame_top;
  if (!f)
    return false;
  for (uint32_t i = 0; i < f->count; i++)
    if (f->bindings[i].name.idx == name.idx) {
      *out = f->bindings[i].val;
      return true;
    }
  return false;
}

// Bind/overwrite `name` → `val` in frame `f` (a later local shadows a param).
// Returns false iff the fixed-cap array is full (caller bails — CTFE_DEFER_FRAME_OVERFLOW).
static bool eval_frame_bind(EvalFrame *f, StrId name, TypedValue val) {
  for (uint32_t i = 0; i < f->count; i++)
    if (f->bindings[i].name.idx == name.idx) {
      f->bindings[i].val = val;
      return true;
    }
  if (f->count >= f->cap)
    return false;
  f->bindings[f->count].name = name;
  f->bindings[f->count].val = val;
  f->count++;
  return true;
}

// CTFE fuel — Zig's branch-quota analog. Counts comptime CALLS (and, once loops
// land, loop-backedges; a recursive call costs 1). Reset at the OUTERMOST
// comptime call; caps total WORK so exponential-but-shallow recursion (fib(40))
// errors instead of hanging the compiler. Default matches Zig's
// `default_branch_quota`. The depth cap (EVAL_CYCLE_DEPTH_MAX) is a SEPARATE,
// stack-safety bound — Zig has none, which can segfault its compiler.
#define EVAL_BRANCH_QUOTA_DEFAULT 1000
static uint32_t g_eval_branch = 0;

// Resolve a (possibly local-or-namespace) name into a TypedValue. Shared
// between SK_REF_* and SK_PATH_* (the latter already extracts the leaf
// name via path_expr_leaf_name). Returns TYPED_VALUE_NONE for an empty
// name (no diag); emits an "unknown name" diag and returns error sentinel
// on a real unresolved lookup.
static TypedValue eval_name(const SemaCtx *ctx, SyntaxNode *node, StrId name) {
  struct db *s = ctx->s;
  if (name.idx == 0)
    return TYPED_VALUE_NONE;

  // 0. CTFE binding frame. Inside an executing comptime fn body, a param/local
  //    NAME resolves to its folded value here FIRST — before the static
  //    body-scope lookup (which would read the param SyntaxNode, not the call's
  //    argument value). Only fires when a comptime call is in flight
  //    (g_eval_frame_top != NULL), so normal type-checking is byte-identical.
  {
    TypedValue bound;
    if (eval_frame_lookup(name, &bound))
      return bound;
  }

  // 1. Local body-scope lookup. `t: type` params and local `::` constants
  //    both surface here. The bind-site's node-type carries the stored
  //    info: a TYPE_VAR hole for `t: type` params (chased via type_resolve);
  //    the binding's RHS-evaluated type for `c :: u32` / `c :: 5`. For
  //    SK_BIND_DECL bindings we recursively eval the RHS to get the full
  //    TypedValue — that's the type-valued-vs-value-valued distinction.
  if (ctx->enclosing_fn.idx != DEF_ID_NONE.idx) {
    SyntaxNodePtr bind =
        db_body_scope_lookup(s, ctx->enclosing_fn, node, name);
    if (bind.kind != SYNTAX_KIND_NONE) {
      // Phase 4+5 — body-scope lookup hit. Key the node-type-builder
      // lookup off the BIND ptr's hash directly (matches
      // resolve_value_path's key shape — using decl_ast_map.rev for the
      // remap, NOT decl_ast_id_lookup on a freshly-resolved node, which
      // can miss when the bind is outside the decl-wrapper subtree).
      // Read the lower 32 bits (type half) first; both halves come from
      // the packed slot if a value was stamped via node_typed_value_push.
      if (ctx->types) {
        uint64_t bh = syntax_node_ptr_hash(bind);
        uint64_t bkey = bh;
        if (ctx->decl_ast_map) {
          void *vrel = hashmap_get(&ctx->decl_ast_map->rev, bh);
          if (vrel)
            bkey = (uint64_t)((uintptr_t)vrel - 1);
        }
        if (hashmap_contains(&ctx->types->types, bkey)) {
          void *vraw = hashmap_get(&ctx->types->types, bkey);
          uintptr_t up = (uintptr_t)vraw;
          IpIndex bty  = (IpIndex){.v = (uint32_t)(up & 0xFFFFFFFFu)};
          IpIndex bval = (IpIndex){.v = (uint32_t)(up >> 32)};
          IpIndex resolved_ty  = type_resolve(ctx, bty);
          IpIndex resolved_val = type_resolve(ctx, bval);
          // SK_BIND_DECL targets need RHS recursion to surface a
          // type-valued binding's value half; that path still uses the
          // resolve-bind-node mechanism below. For SK_PARAM and other
          // direct binds the packed slot IS the answer.
          if (bind.kind != SK_BIND_DECL) {
            return (TypedValue){.type = resolved_ty, .value = resolved_val};
          }
          // Fall through into the SK_BIND_DECL recurse branch below.
        } else {
          // Bound but not yet typed (use-before-decl, or a residual
          // ordering case). Mirror resolve_value_path: hard error if the
          // use textually precedes the bind, quiet IP_NONE otherwise.
          TextRange ur = syntax_node_text_range(node);
          if (ur.start < bind.range.start + bind.range.length) {
            db_emit(s, DIAG_ERROR, span_of(ctx, node),
                    "'%S' used before its declaration", name);
            return (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
          }
          return TYPED_VALUE_NONE;
        }
      }
      // Look up the bind-site's stored (type, value) via the Phase 1
      // helpers. We need to convert the SyntaxNodePtr to a node first to
      // call db_lookup_node_typed_value (which takes SyntaxNode*).
      // The stored type tells us the binding's category; the value (if
      // pushed) is the comptime-folded RHS — but for `t: type` params and
      // most legacy pushes, value is IP_NONE.
      struct GreenNode *bgroot = db_read_file_ast_untracked(s, ctx->file_local);
      if (bgroot) {
        SyntaxTree *btree = syntax_tree_new(bgroot);
        SyntaxNode *broot = syntax_tree_root(btree);
        SyntaxNode *bind_node = syntax_node_ptr_resolve(bind, broot);
        syntax_node_release(broot);
        TypedValue tv = TYPED_VALUE_NONE;
        if (bind_node) {
          TypedValue stored = db_lookup_node_typed_value(ctx, bind_node);
          // Chase a TYPE_VAR hole to its concrete binding (per-instance
          // substitution). Identity on non-holes / unbound holes.
          IpIndex resolved_ty = type_resolve(ctx, stored.type);
          if (syntax_node_kind(bind_node) == SK_BIND_DECL) {
            // Local `::` (or `:=`) — recurse on the RHS to get a complete
            // TypedValue. This naturally folds `c :: u32` to
            // {IP_TYPE_TYPE, IP_U32_TYPE} and `c :: 5` to
            // {comptime_int, interned 5}.
            //
            // Cycle guard — `c :: c` / `a :: b; b :: a` would otherwise loop.
            uint64_t bind_hash = syntax_node_ptr_hash(bind);
            uint32_t bind_file = ctx->file_local.idx;
            if (eval_cycle_contains(bind_file, bind_hash)) {
              db_emit(s, DIAG_ERROR, span_of(ctx, node),
                      "circular const dependency through '%S'", name);
              tv = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
            } else if (eval_cycle_depth() >= EVAL_CYCLE_DEPTH_MAX) {
              db_emit(s, DIAG_ERROR, span_of(ctx, node),
                      "const chain too deep (max %d)", EVAL_CYCLE_DEPTH_MAX);
              tv = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
            } else {
              BindDef bd;
              if (BindDef_cast(bind_node, &bd)) {
                SyntaxNode *ann = BindDef_type(&bd);
                bool has_ann = (ann != NULL);
                if (ann)
                  syntax_node_release(ann);
                bool is_const = BindDef_is_const(&bd);
                SyntaxNode *rhs = BindDef_value(&bd);
                if (rhs) {
                  EvalCycleFrame frame = {.prev = g_eval_cycle_top,
                                          .file_idx = bind_file,
                                          .key_hash = bind_hash};
                  g_eval_cycle_top = &frame;
                  tv = eval_expr(ctx, rhs);
                  g_eval_cycle_top = frame.prev;
                  syntax_node_release(rhs);
                }
                // Annotation governs the TYPE half (matches type_let_bind):
                // `name : T = rhs` has declared type T regardless of the RHS's
                // own type. The RHS recursion still supplies the VALUE half, so
                // a type-valued const `c : type = u32` keeps {IP_TYPE_TYPE,
                // IP_U32_TYPE}. Without this, `p : ?^Foo = first` read back as
                // `^Foo` and `offset : usize = @sizeOf(T)` as `comptime_int`.
                if (has_ann && resolved_ty.v != IP_NONE.v &&
                    !ip_is_error(tv.type))
                  tv.type = resolved_ty;
                // A mutable binding is not comptime-stable — a later read after
                // reassignment must not see a stale folded value. (`::` consts
                // keep the folded value half for type-valued locals.)
                if (!is_const)
                  tv.value = IP_NONE;
              }
              // RHS missing / not foldable — fall back to the stored
              // (type, value) pair.
              if (tv.type.v == IP_NONE.v && tv.value.v == IP_NONE.v) {
                tv.type = resolved_ty;
                tv.value = stored.value;
              }
            }
          } else {
            // Phase 3 — SK_PARAM bind sites push the right TypedValue shape
            // directly (build_fn_type + infer_body + db_query_infer_instance):
            //   `t: type` → (IP_TYPE_TYPE, hole_or_concrete)
            //   `anytype` → (hole_or_concrete, IP_NONE)
            //   `x: i32`  → (i32, IP_NONE)
            // type_resolve chases either half through the per-instance
            // substitution table; no special-case wrapping is needed.
            IpIndex resolved_val = type_resolve(ctx, stored.value);
            tv = (TypedValue){.type = resolved_ty, .value = resolved_val};
          }
          syntax_node_release(bind_node);
        }
        syntax_tree_free(btree);
        if (tv.type.v != IP_NONE.v || tv.value.v != IP_NONE.v)
          return tv;
      }
    }
  }

  // 2. Signature-frame type_name_map — `t: type` / `anytype` param
  //    references INSIDE the same signature (e.g. `^t` in another param,
  //    `[]t` in the return). Resolves to the hole minted at param time.
  if (ctx->type_name_map) {
    void *hole = hashmap_get(ctx->type_name_map, (uint64_t)name.idx);
    if (hole) {
      IpIndex hole_idx = (IpIndex){.v = (uint32_t)(uintptr_t)hole};
      return (TypedValue){.type = IP_TYPE_TYPE, .value = hole_idx};
    }
  }

  // 3. Namespace resolution. Primitives, top-level decls.
  NamespaceScopes sc = db_query_namespace_scopes(s, ctx->nsid);
  DefId tgt = DEF_ID_NONE;
  if (sc.internal.idx != SCOPE_ID_NONE.idx)
    tgt = db_query_resolve_ref(s, sc.internal, name);
  if (tgt.idx == DEF_ID_NONE.idx) {
    db_emit(s, DIAG_ERROR, span_of(ctx, node), "unknown name '%S'", name);
    return (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
  }

  // Primitive type (u32, bool, void, …).
  IpIndex prim = db_primitive_type_for(s, tgt);
  if (ip_index_is_valid(prim))
    return (TypedValue){.type = IP_TYPE_TYPE, .value = prim};

  DefKind dk = db_def_kind(s, tgt);
  switch (dk) {
  case KIND_STRUCT:
  case KIND_UNION:
  case KIND_ENUM:
  case KIND_DISTINCT:
  case KIND_EFFECT: {
    // Nominal type. Its identity IS the type-value.
    IpIndex ty = db_query_type_of_def(s, tgt);
    return (TypedValue){.type = IP_TYPE_TYPE, .value = ty};
  }
  case KIND_CONSTANT: {
    // Top-level `::` binding. Its declared type tells us the category:
    // metatype `type` → type-valued (recurse on RHS for the type-value);
    // anything else → value-valued (return declared type + IP_NONE).
    IpIndex declared = db_query_type_of_def(s, tgt);
    // Find the SK_BIND_DECL for this def via top_level_entry — we need
    // its RHS to recurse.
    extern TopLevelEntry db_query_top_level_entry(db_query_ctx *,
                                                  NamespaceId, StrId);
    TopLevelEntry e = db_query_top_level_entry(s, ctx->nsid, name);
    if (e.node_ptr.kind == SYNTAX_KIND_NONE)
      return (TypedValue){.type = declared, .value = IP_NONE};
    struct GreenNode *cgroot = db_read_file_ast_untracked(s, e.file);
    if (!cgroot)
      return (TypedValue){.type = declared, .value = IP_NONE};
    SyntaxTree *ctree = syntax_tree_new(cgroot);
    SyntaxNode *croot = syntax_tree_root(ctree);
    SyntaxNode *cbind = syntax_node_ptr_resolve(e.node_ptr, croot);
    syntax_node_release(croot);
    TypedValue tv = (TypedValue){.type = declared, .value = IP_NONE};
    if (cbind && syntax_node_kind(cbind) == SK_BIND_DECL) {
      BindDef cbd;
      if (BindDef_cast(cbind, &cbd) && BindDef_is_const(&cbd)) {
        SyntaxNode *rhs = BindDef_value(&cbd);
        if (rhs) {
          // Phase 4+5 — cross-decl recursion: switch the diag-anchor
          // context to the TARGET decl's map so span_of inside the RHS
          // walk resolves nodes that belong to `tgt`, not to the
          // calling decl. Mirror of const_eval_anchor_for_target's
          // saved_anchor trick.
          //
          // Phase 6 — also push the cycle frame here so cross-decl ref
          // chains (`A :: B; B :: A`, including cross-file) trip the
          // same diag as local SK_BIND_DECL self-refs. Depth-cap walk
          // guards pathological deep chains.
          uint64_t bind_hash = syntax_node_ptr_hash(e.node_ptr);
          uint32_t bind_file = e.file.idx;
          if (eval_cycle_contains(bind_file, bind_hash)) {
            db_emit(s, DIAG_ERROR, span_of(ctx, node),
                    "circular const dependency through '%S'", name);
            tv = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
          } else if (eval_cycle_depth() >= EVAL_CYCLE_DEPTH_MAX) {
            db_emit(s, DIAG_ERROR, span_of(ctx, node),
                    "const chain too deep (max %d)", EVAL_CYCLE_DEPTH_MAX);
            tv = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
          } else {
            SemaCtx local = *ctx;
            local.decl_ast_map = db_get_decl_ast_id_map_untracked(s, tgt);
            local.decl_key     = e.id.idx;
            local.file_local   = e.file;
            EvalCycleFrame frame = {.prev = g_eval_cycle_top,
                                    .file_idx = bind_file,
                                    .key_hash = bind_hash};
            g_eval_cycle_top = &frame;
            tv = eval_expr(&local, rhs);
            g_eval_cycle_top = frame.prev;
          }
          syntax_node_release(rhs);
        }
      }
    }
    if (cbind)
      syntax_node_release(cbind);
    syntax_tree_free(ctree);
    return tv;
  }
  case KIND_FUNCTION: {
    // Function reference — a comptime value carrying its DefId (Zig's `func`
    // value). A call site reads this to recover the callee uniformly (bare or
    // qualified), which keys monomorphization. Inline-encoded, no arena payload.
    IpIndex ty = db_query_type_of_def(s, tgt);
    IpKey fk = {.kind = IPK_FN_VALUE, .fn_value = {.def = tgt}};
    return (TypedValue){.type = ty, .value = ip_get(&s->intern, fk)};
  }
  case KIND_VARIABLE:
  default: {
    // Value-kind def — top-level mutable. Carry its type.
    IpIndex ty = db_query_type_of_def(s, tgt);
    return (TypedValue){.type = ty, .value = IP_NONE};
  }
  }
}

// `::` consts are immutable. Returns true (and emits a diag) iff `target` is a
// bare reference to an immutable `::` binding — a const local (SK_BIND_DECL with
// `::`) or a top-level KIND_CONSTANT. Place targets (field / index / deref) and
// mutable bindings / parameters return false: mutating through a pointer / field
// / element rebinds nothing. Mirrors eval_name's bind-site resolution (local
// scope first, then namespace).
bool reject_const_mutation(const SemaCtx *ctx, SyntaxNode *target,
                           const char *verb) {
  if (!target)
    return false;
  struct db *s = ctx->s;
  SyntaxKind k = syntax_node_kind(target);

  // `(x) = …` targets x — unwrap parens.
  if (k == SK_PAREN_EXPR) {
    ParenExpr pe;
    if (!ParenExpr_cast(target, &pe))
      return false;
    SyntaxNode *inner = ParenExpr_inner(&pe);
    if (!inner)
      return false;
    bool r = reject_const_mutation(ctx, inner, verb);
    syntax_node_release(inner);
    return r;
  }

  // Only a bare name can name a binding. Field / index / deref are places —
  // assigning through them mutates the pointee / element, not the binding.
  StrId name = (StrId){0};
  if (k == SK_REF_EXPR) {
    SyntaxToken *name_tok = ast_first_token(target, SK_IDENT);
    if (name_tok) {
      name = pool_intern(&s->strings, syntax_token_text(name_tok),
                         syntax_token_text_range(name_tok).length);
      syntax_token_release(name_tok);
    }
  } else if (k == SK_PATH_EXPR) {
    name = path_expr_leaf_name(s, target);
  } else {
    return false;
  }
  if (name.idx == 0)
    return false;

  // Local binding wins (it may shadow a top-level const).
  if (ctx->enclosing_fn.idx != DEF_ID_NONE.idx) {
    SyntaxNodePtr bind = db_body_scope_lookup(s, ctx->enclosing_fn, target, name);
    if (bind.kind != SYNTAX_KIND_NONE) {
      bool is_const_bind = false;
      struct GreenNode *groot = db_read_file_ast_untracked(s, ctx->file_local);
      if (groot) {
        SyntaxTree *tree = syntax_tree_new(groot);
        SyntaxNode *root = syntax_tree_root(tree);
        SyntaxNode *bind_node = syntax_node_ptr_resolve(bind, root);
        syntax_node_release(root);
        if (bind_node) {
          BindDef bd;
          if (BindDef_cast(bind_node, &bd) && BindDef_is_const(&bd))
            is_const_bind = true;
          syntax_node_release(bind_node);
        }
        syntax_tree_free(tree);
      }
      if (is_const_bind) {
        db_emit(s, DIAG_ERROR, span_of(ctx, target),
                "cannot %s immutable binding '%S' (declared with `::`); use "
                "`:=` for a mutable binding",
                verb, name);
        return true;
      }
      return false; // local but mutable (`:=`) or a parameter — allowed
    }
  }

  // Top-level decl: KIND_CONSTANT is a `::` const.
  NamespaceScopes sc = db_query_namespace_scopes(s, ctx->nsid);
  DefId tgt = DEF_ID_NONE;
  if (sc.internal.idx != SCOPE_ID_NONE.idx)
    tgt = db_query_resolve_ref(s, sc.internal, name);
  if (tgt.idx != DEF_ID_NONE.idx && db_def_kind(s, tgt) == KIND_CONSTANT) {
    db_emit(s, DIAG_ERROR, span_of(ctx, target),
            "cannot %s immutable binding '%S' (declared with `::`); use `:=` "
            "for a mutable binding",
            verb, name);
    return true;
  }
  return false;
}

// Phase 6 Batch 3 — set enum-context hint on a local SemaCtx copy and
// dispatch to eval_expr. Single-shot semantics — the hint is consumed by
// SK_ENUM_REF_EXPR and reset by every other recursing arm.
TypedValue eval_expr_with_enum_hint(const SemaCtx *ctx, SyntaxNode *node,
                                    DefId enum_def) {
  if (!node) return TYPED_VALUE_NONE;
  SemaCtx local = *ctx;
  local.enum_ctx_hint = enum_def;
  return eval_expr(&local, node);
}

// ===========================================================================
// CTFE — run a pure, non-generic comptime fn CALL and fold it to a scalar.
// Every non-foldable case returns the SAFE runtime fallback
// {infer_value_position, IP_NONE} (never a wrong value, never a spurious error;
// the purity + budget diags are the only intentional errors). See the plan.
// ===========================================================================

// A scalar comptime value: an int/float value or a bool. MVP folds these only;
// aggregates (struct/array/...) bail (CTFE_DEFER_AGGREGATE).
static bool ctfe_is_scalar_value(struct db *s, IpIndex v) {
  if (v.v == IP_NONE.v || ip_is_error(v))
    return false;
  if (v.v == IP_BOOL_TRUE.v || v.v == IP_BOOL_FALSE.v)
    return true;
  return ip_tag(&s->intern, v) == IP_TAG_INT_VALUE ||
         ip_tag(&s->intern, v) == IP_TAG_FLOAT_VALUE;
}

// Result of evaluating a statement list: the value, whether a `return` fired
// (stops + propagates), and whether everything folded (false → caller abandons
// to the runtime fallback; the no-stopgaps escape hatch).
typedef struct EvalStmtResult {
  TypedValue val;
  bool       returned;
  bool       folded;
} EvalStmtResult;

// Drive an SK_STMT_LIST: bind local `::`/`:=` decls into the frame, stop at the
// first `return`. Any other statement kind abandons folding (CTFE_DEFER_STMT —
// e.g. loop, assign, if/switch-with-`return`, defer, bare expr-stmt).
static EvalStmtResult eval_stmt_list(const SemaCtx *ctx, SyntaxNode *stmts,
                                     EvalFrame *frame) {
  EvalStmtResult r = {.val = TYPED_VALUE_NONE, .returned = false, .folded = true};
  if (!stmts)
    return r;
  uint32_t total = syntax_node_num_children(stmts);
  for (uint32_t i = 0; i < total; i++) {
    SyntaxElement el = syntax_node_child_or_token(stmts, i);
    if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
      if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
        syntax_token_release(el.token);
      continue;
    }
    SyntaxNode *stmt = el.node;
    SyntaxKind k = syntax_node_kind(stmt);
    if (k == SK_RETURN_STMT) {
      ReturnStmt rs;
      if (ReturnStmt_cast(stmt, &rs)) {
        SyntaxNode *rv = ReturnStmt_value(&rs);
        if (rv) {
          r.val = eval_expr(ctx, rv);
          r.folded = r.val.value.v != IP_NONE.v && !ip_is_error(r.val.value);
          syntax_node_release(rv);
        } else {
          r.folded = false; // `return;` (void) — nothing to fold
        }
      } else {
        r.folded = false;
      }
      r.returned = true;
      syntax_node_release(stmt);
      return r;
    }
    if (k == SK_BIND_DECL) {
      BindDef bd;
      if (BindDef_cast(stmt, &bd)) {
        SyntaxToken *nm = BindDef_name(&bd);
        StrId lname = nm ? pool_intern(&ctx->s->strings, syntax_token_text(nm),
                                       syntax_token_text_range(nm).length)
                         : (StrId){0};
        if (nm)
          syntax_token_release(nm);
        SyntaxNode *rhs = BindDef_value(&bd);
        if (rhs && lname.idx != 0) {
          TypedValue lv = eval_expr(ctx, rhs);
          if (!ctfe_is_scalar_value(ctx->s, lv.value) ||
              !eval_frame_bind(frame, lname, lv))
            r.folded = false; // local didn't fold / frame full → abandon
        } else {
          r.folded = false;
        }
        if (rhs)
          syntax_node_release(rhs);
      } else {
        r.folded = false;
      }
      syntax_node_release(stmt);
      if (!r.folded)
        return r;
      continue;
    }
    syntax_node_release(stmt);
    r.folded = false; // CTFE_DEFER_STMT — out of MVP scope
    return r;
  }
  return r; // fell off the end with no `return` → void body (won't fold)
}

// Evaluate a comptime fn body to a value: bare-expr body → the expr IS the
// value; block body → drive the statement list.
static TypedValue eval_body(const SemaCtx *ctx, SyntaxNode *body,
                            EvalFrame *frame) {
  if (!body)
    return TYPED_VALUE_NONE;
  if (syntax_node_kind(body) != SK_BLOCK_STMT)
    return eval_expr(ctx, body); // bare-expr body
  BlockStmt bs;
  if (!BlockStmt_cast(body, &bs))
    return TYPED_VALUE_NONE;
  SyntaxNode *stmts = BlockStmt_stmts(&bs);
  EvalStmtResult r = eval_stmt_list(ctx, stmts, frame);
  if (stmts)
    syntax_node_release(stmts);
  return r.folded ? r.val : TYPED_VALUE_NONE;
}

static TypedValue eval_call(const SemaCtx *ctx, SyntaxNode *node) {
  struct db *s = ctx->s;
  // SAFE default: runtime-only (type from normal inference, no folded value).
  TypedValue out = {.type = infer_value_position(ctx, node), .value = IP_NONE};
  IpIndex fallback_type = out.type;

  CallExpr ce;
  if (!CallExpr_cast(node, &ce))
    return out;
  if (CallExpr_is_with(&ce) || CallExpr_is_handle(&ce))
    return out; // CTFE_DEFER_WITH_HANDLE

  SyntaxNode *callee = CallExpr_callee(&ce);
  SyntaxNode *arg_list = CallExpr_args(&ce);
  SyntaxTree *tree = NULL;
  SyntaxNode *fdecl = NULL, *value = NULL, *params = NULL, *body = NULL;
  TypedValue arg_vals[32];
  uint32_t n_args = 0;

  if (!callee)
    goto done;

  // --- Recover the callee DefId from its fn-value. ---
  TypedValue callee_tv = eval_expr(ctx, callee);
  if (callee_tv.value.v == IP_NONE.v || ip_is_error(callee_tv.value) ||
      ip_tag(&s->intern, callee_tv.value) != IP_TAG_FN_VALUE)
    goto done; // runtime / unresolved / fn-pointer callee
  DefId d = ip_key(&s->intern, callee_tv.value).fn_value.def;

  // --- Gates: non-generic + pure. ---
  IpIndex fnty = db_query_type_of_def(s, d);
  if (ip_is_error(fnty) || ip_tag(&s->intern, fnty) != IP_TAG_FN_TYPE)
    goto done;
  IpKey fk = ip_key(&s->intern, fnty);
  if (sig_has_unbound_hole(ctx, fnty) || fk.fn_type.comptime_bits != 0 ||
      fk.fn_type.typevalued_bits != 0)
    goto done; // CTFE_DEFER_GENERIC — MVP is non-generic only
  IpIndex er = fk.fn_type.effect_row;
  if (er.v != IP_NONE.v && er.v != IP_EMPTY_EFFECT_ROW.v && !ip_is_error(er)) {
    if (ctx->in_comptime) {
      db_emit(s, DIAG_ERROR, span_of(ctx, node),
              "comptime context cannot call effectful function (effects %T)", er);
      out = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
    }
    goto done; // not in comptime → a runtime call to an effectful fn is fine
  }

  // --- Evaluate args (each must fold to a scalar). ---
  bool args_ok = true;
  if (arg_list) {
    uint32_t atotal = syntax_node_num_children(arg_list);
    for (uint32_t i = 0; i < atotal; i++) {
      SyntaxElement el = syntax_node_child_or_token(arg_list, i);
      if (el.kind == SYNTAX_ELEM_NODE && el.node) {
        TypedValue av = eval_expr(ctx, el.node);
        if (n_args < 32 && ctfe_is_scalar_value(s, av.value))
          arg_vals[n_args] = av;
        else
          args_ok = false;
        n_args++;
        syntax_node_release(el.node);
      } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
        syntax_token_release(el.token);
      }
    }
  }
  if (!args_ok || n_args != fk.fn_type.n_params)
    goto done; // unfoldable arg / arity mismatch → let normal typing handle it

  // --- Fetch the callee body + params (SK_FIELD_EXPR member-fetch template). ---
  StrId fname = db_get_def_name_untracked(s, d);
  NamespaceId fns = db_get_def_parent_module_untracked(s, d);
  TopLevelEntry e = db_query_top_level_entry(s, fns, fname);
  if (e.node_ptr.kind == SYNTAX_KIND_NONE)
    goto done;
  struct GreenNode *groot = db_read_file_ast_untracked(s, e.file);
  if (!groot)
    goto done;
  tree = syntax_tree_new(groot);
  SyntaxNode *froot = syntax_tree_root(tree);
  fdecl = syntax_node_ptr_resolve(e.node_ptr, froot);
  syntax_node_release(froot);
  // A top-level fn is an SK_BIND_DECL wrapper whose RHS is the fn LAMBDA
  // (`name :: fn(..) -> T body`). Mirror type.c's bind_value + LambdaExpr_*.
  BindDef bd;
  LambdaExpr lam;
  if (fdecl && BindDef_cast(fdecl, &bd)) {
    value = BindDef_value(&bd);
    if (value && LambdaExpr_cast(value, &lam)) {
      params = LambdaExpr_params(&lam);
      body = LambdaExpr_body(&lam);
    }
  }
  if (!body)
    goto cleanup_tree; // not a plain fn (extern / non-lambda RHS) → runtime

  // --- Budgets: fuel (total work) + depth (stack safety). ---
  if (g_eval_frame_top == NULL)
    g_eval_branch = 0; // outermost comptime call resets the fuel
  if (++g_eval_branch > EVAL_BRANCH_QUOTA_DEFAULT) {
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "comptime evaluation exceeded %d backwards branches",
            EVAL_BRANCH_QUOTA_DEFAULT);
    out = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
    goto cleanup_tree;
  }
  if (eval_cycle_depth() >= EVAL_CYCLE_DEPTH_MAX) {
    db_emit(s, DIAG_ERROR, span_of(ctx, node), "comptime call too deep (max %d)",
            EVAL_CYCLE_DEPTH_MAX);
    out = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
    goto cleanup_tree;
  }

  // --- Bind params → arg values in a fresh frame. ---
  EvalBinding binds[32];
  EvalFrame frame = {
      .prev = g_eval_frame_top, .bindings = binds, .count = 0, .cap = 32};
  uint32_t pi = 0;
  bool bind_ok = true;
  if (params) {
    uint32_t ptotal = syntax_node_num_children(params);
    for (uint32_t i = 0; i < ptotal && bind_ok; i++) {
      SyntaxElement el = syntax_node_child_or_token(params, i);
      if (el.kind == SYNTAX_ELEM_NODE && el.node) {
        if (syntax_node_kind(el.node) == SK_PARAM) {
          Param p;
          if (Param_cast(el.node, &p)) {
            SyntaxToken *pn = Param_name(&p);
            StrId pname = pn ? pool_intern(&s->strings, syntax_token_text(pn),
                                           syntax_token_text_range(pn).length)
                             : (StrId){0};
            if (pn)
              syntax_token_release(pn);
            if (pname.idx != 0 && pi < n_args)
              bind_ok = eval_frame_bind(&frame, pname, arg_vals[pi]);
            pi++;
          }
        }
        syntax_node_release(el.node);
      } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
        syntax_token_release(el.token);
      }
    }
  }
  if (!bind_ok)
    goto cleanup_tree; // CTFE_DEFER_FRAME_OVERFLOW

  // --- Swap context to the callee, push frames, eval body, pop. ---
  SemaCtx cc = *ctx;
  cc.enclosing_fn = d;
  cc.nsid = fns;
  cc.file_local = e.file;
  cc.decl_ast_map = db_get_decl_ast_id_map_untracked(s, d);
  cc.decl_key = e.id.idx;
  cc.enum_ctx_hint = DEF_ID_NONE;
  cc.in_comptime = true;
  EvalCycleFrame cyc = {.prev = g_eval_cycle_top,
                        .file_idx = e.file.idx,
                        .key_hash = syntax_node_ptr_hash(e.node_ptr)};
  g_eval_cycle_top = &cyc;
  g_eval_frame_top = &frame;
  TypedValue res = eval_body(&cc, body, &frame);
  g_eval_frame_top = frame.prev;
  g_eval_cycle_top = cyc.prev;

  // --- Scalar gate; the call's TYPE is its normally-inferred return type. ---
  if (ctfe_is_scalar_value(s, res.value))
    out = (TypedValue){.type = fallback_type, .value = res.value};
  else if (ip_is_error(res.value))
    out = res; // propagate a nested budget/purity error

cleanup_tree:
  if (params)
    syntax_node_release(params);
  if (body)
    syntax_node_release(body);
  if (value)
    syntax_node_release(value);
  if (fdecl)
    syntax_node_release(fdecl);
  if (tree)
    syntax_tree_free(tree);
done:
  if (callee)
    syntax_node_release(callee);
  if (arg_list)
    syntax_node_release(arg_list);
  return out;
}

TypedValue eval_expr(const SemaCtx *ctx, SyntaxNode *node) {
  if (!node)
    return TYPED_VALUE_NONE;

  // Phase 2 — stub dispatch. Each arm gets ported in a follow-up
  // commit. Until then this returns TYPED_VALUE_NONE for every kind,
  // which is also the "unknown" sentinel — the thin resolve_type_expr
  // wrapper interprets that as "graceful no-result" (returns IP_NONE,
  // no diag). So while eval_expr is incomplete, type-expression
  // resolution silently fails — that's the migration's accepted
  // breakage per user direction.
  SyntaxKind k = syntax_node_kind(node);
  TypedValue result = TYPED_VALUE_NONE;

  switch ((OreSyntaxKind)k) {
  case SK_LITERAL_EXPR: {
    // Type comes from the literal's token kind (int_lit → comptime_int,
    // float_lit → comptime_float, true/false → bool, unreachable → noreturn,
    // etc.) — same mapping infer.c's type_of_expr uses. For numeric literals
    // we ALSO intern the comptime value (IPK_INT_VALUE / IPK_FLOAT_VALUE)
    // so callers downstream of resolve_type_expr (e.g., SK_ARRAY_TYPE's size
    // expression) can read the actual value back.
    Literal lit;
    if (!Literal_cast(node, &lit)) {
      result = TYPED_VALUE_NONE;
      break;
    }
    SyntaxKind tk = Literal_kind(&lit);
    IpIndex ty = type_from_lit_token(tk);
    IpIndex value = IP_NONE;
    if (tk == SK_INT_LIT) {
      SyntaxToken *tok = Literal_token(&lit);
      if (tok) {
        uint64_t u = parse_int_literal(tok);
        IpKey ikey = {.kind = IPK_INT_VALUE,
                      .int_value = {.type = ty, .value = (int64_t)u}};
        value = ip_get(&ctx->s->intern, ikey);
        syntax_token_release(tok);
      }
    } else if (tk == SK_BYTE_LIT) {
      // Byte literal — single u8 character. parse_int_literal handles the
      // numeric token; the type is IP_U8_TYPE from type_from_lit_token.
      SyntaxToken *tok = Literal_token(&lit);
      if (tok) {
        uint64_t u = parse_int_literal(tok);
        IpKey ikey = {.kind = IPK_INT_VALUE,
                      .int_value = {.type = ty, .value = (int64_t)u}};
        value = ip_get(&ctx->s->intern, ikey);
        syntax_token_release(tok);
      }
    } else if (tk == SK_FLOAT_LIT) {
      // Phase 6 Batch 2 — float literal value. parse_float_literal_text
      // strips `_` and runs strtod; intern as IPK_FLOAT_VALUE so binop_arith
      // (Batch 2) and tv_fits_in (Batch 5) can decode the scalar.
      SyntaxToken *tok = Literal_token(&lit);
      if (tok) {
        double d = 0.0;
        if (parse_float_literal_text(tok, &d)) {
          IpKey fkey = {.kind = IPK_FLOAT_VALUE,
                        .float_value = {.type = ty, .value = d}};
          value = ip_get(&ctx->s->intern, fkey);
        }
        syntax_token_release(tok);
      }
    } else if (tk == SK_TRUE_KW) {
      // Phase 6 Batch 2 — reserved IP_BOOL_TRUE (no IPK_BOOL_VALUE kind).
      value = IP_BOOL_TRUE;
    } else if (tk == SK_FALSE_KW) {
      value = IP_BOOL_FALSE;
    }
    // Inline asm performs the primitive `<asm>` effect — union it into the
    // body's effect row so an asm-bodied fn honestly carries `<asm>` (the io
    // floor). Guarded on `body_effect_row` being live (a body walk only; NULL
    // in type-position / const-eval contexts).
    if (tk == SK_ASM_LIT && ctx->body_effect_row)
      *ctx->body_effect_row =
          row_union(ctx, *ctx->body_effect_row, db_asm_effect_row(ctx->s), node);
    // SK_NIL_KW/SK_UNREACHABLE_KW/SK_ASM_LIT/SK_STRING_LIT leave value =
    // IP_NONE; the type field carries the category info.
    result = (TypedValue){.type = ty, .value = value};
    break;
  }

  // Phase 6 Batch 3 — field access. Three paths:
  //   (1) base.value tag IP_TAG_NAMESPACE_VALUE → member lookup; for
  //       KIND_CONSTANT member, recurse on RHS with cycle push + SemaCtx
  //       swap (mirror of eval_name's KIND_CONSTANT path).
  //   (2) base evaluates to a TYPE whose underlying tag is IP_TAG_ENUM_TYPE
  //       (qualified `Enum.variant`) → produce IPK_ENUM_VARIANT_VALUE.
  //   (3) Anything else → delegate type half to infer_value_position;
  //       value half stays IP_NONE. (Batch 5 will extract infer_field_expr
  //       so we don't re-eval base.)
  case SK_FIELD_EXPR: {
    FieldExpr fe;
    if (!FieldExpr_cast(node, &fe)) {
      result = TYPED_VALUE_NONE;
      break;
    }
    SyntaxNode *base = FieldExpr_base(&fe);
    SyntaxToken *fname_tok = FieldExpr_field(&fe);
    StrId fname = fname_tok ? pool_intern(&ctx->s->strings,
                                          syntax_token_text(fname_tok),
                                          syntax_token_text_range(fname_tok).length)
                            : (StrId){0};
    if (fname_tok) syntax_token_release(fname_tok);

    // Reset enum_ctx_hint on base recursion — base is not in `.variant`
    // pattern position.
    SemaCtx base_ctx = *ctx;
    base_ctx.enum_ctx_hint = DEF_ID_NONE;
    TypedValue base_tv = base ? eval_expr(&base_ctx, base) : TYPED_VALUE_NONE;
    if (base) syntax_node_release(base);
    if (ip_is_error(base_tv.type)) {
      result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
      break;
    }
    if (base_tv.type.v == IP_NONE.v || fname.idx == 0) {
      result = TYPED_VALUE_NONE;
      break;
    }

    // Path 1 — namespace member.
    if (base_tv.value.v != IP_NONE.v && !ip_is_error(base_tv.value) &&
        ip_tag(&ctx->s->intern, base_tv.value) == IP_TAG_NAMESPACE_VALUE) {
      NamespaceId ns = ip_key(&ctx->s->intern, base_tv.value).namespace_value.nsid;
      TopLevelEntry e = db_query_top_level_entry(ctx->s, ns, fname);
      if (e.node_ptr.kind == SYNTAX_KIND_NONE) {
        db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node),
                "no member '%S' in namespace", fname);
        result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
        break;
      }
      DefId mtgt = db_query_def_identity(ctx->s, ns, e.id);
      IpIndex member_type = db_query_type_of_def(ctx->s, mtgt);
      DefKind mk = db_def_kind(ctx->s, mtgt);
      result = (TypedValue){.type = member_type, .value = IP_NONE};
      if (mk == KIND_FUNCTION) {
        // Qualified function reference — carry its DefId as a value, exactly as
        // a bare ref does (eval_name), so the call site recovers the callee
        // uniformly and cross-module generics instantiate + get body-checked.
        IpKey fk = {.kind = IPK_FN_VALUE, .fn_value = {.def = mtgt}};
        result.value = ip_get(&ctx->s->intern, fk);
      }
      if (mk == KIND_CONSTANT) {
        struct GreenNode *cgroot = db_read_file_ast_untracked(ctx->s, e.file);
        if (cgroot) {
          SyntaxTree *ctree = syntax_tree_new(cgroot);
          SyntaxNode *croot = syntax_tree_root(ctree);
          SyntaxNode *cbind = syntax_node_ptr_resolve(e.node_ptr, croot);
          syntax_node_release(croot);
          if (cbind && syntax_node_kind(cbind) == SK_BIND_DECL) {
            BindDef cbd;
            if (BindDef_cast(cbind, &cbd) && BindDef_is_const(&cbd)) {
              SyntaxNode *rhs = BindDef_value(&cbd);
              if (rhs) {
                uint64_t bind_hash = syntax_node_ptr_hash(e.node_ptr);
                uint32_t bind_file = e.file.idx;
                if (eval_cycle_contains(bind_file, bind_hash)) {
                  db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node),
                          "circular const dependency through '%S'", fname);
                  result = (TypedValue){.type = IP_ERROR_TYPE,
                                        .value = IP_ERROR_TYPE};
                } else if (eval_cycle_depth() >= EVAL_CYCLE_DEPTH_MAX) {
                  db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node),
                          "const chain too deep (max %d)",
                          EVAL_CYCLE_DEPTH_MAX);
                  result = (TypedValue){.type = IP_ERROR_TYPE,
                                        .value = IP_ERROR_TYPE};
                } else {
                  SemaCtx local = *ctx;
                  local.decl_ast_map =
                      db_get_decl_ast_id_map_untracked(ctx->s, mtgt);
                  local.decl_key = e.id.idx;
                  local.file_local = e.file;
                  local.enum_ctx_hint = DEF_ID_NONE;
                  EvalCycleFrame frame = {.prev = g_eval_cycle_top,
                                          .file_idx = bind_file,
                                          .key_hash = bind_hash};
                  g_eval_cycle_top = &frame;
                  result = eval_expr(&local, rhs);
                  g_eval_cycle_top = frame.prev;
                }
                syntax_node_release(rhs);
              }
            }
            syntax_node_release(cbind);
          }
          syntax_tree_free(ctree);
        }
      }
      break;
    }

    // Path 1b — namespace member via the base's namespace TYPE. Reliable even
    // when the base didn't fold to a namespace VALUE (an @import's value-fold is
    // order-dependent; its TYPE — IP_TAG_NAMESPACE_TYPE — is deterministic via
    // type_of_def). Produces the function-VALUE for a fn member so a QUALIFIED
    // callee `mod.f` carries its DefId → cross-module monomorphization. Mirrors
    // infer.c's IP_TAG_NAMESPACE_TYPE field path. Non-fn members fall through to
    // Path 3 unchanged (their value half is unused here).
    if (ip_tag(&ctx->s->intern, base_tv.type) == IP_TAG_NAMESPACE_TYPE) {
      NamespaceId ns =
          ip_key(&ctx->s->intern, base_tv.type).namespace_type.nsid;
      TopLevelEntry e = db_query_top_level_entry(ctx->s, ns, fname);
      if (e.node_ptr.kind != SYNTAX_KIND_NONE) {
        DefId mtgt = db_query_def_identity(ctx->s, ns, e.id);
        if (db_def_kind(ctx->s, mtgt) == KIND_FUNCTION) {
          IpIndex member_type = db_query_type_of_def(ctx->s, mtgt);
          IpKey fk = {.kind = IPK_FN_VALUE, .fn_value = {.def = mtgt}};
          result = (TypedValue){.type = member_type,
                                .value = ip_get(&ctx->s->intern, fk)};
          break;
        }
      }
    }

    // Path 2 — qualified `Enum.variant`. Base resolves to a TYPE value
    // whose underlying tag is IP_TAG_ENUM_TYPE.
    if (base_tv.type.v == IP_TYPE_TYPE.v && base_tv.value.v != IP_NONE.v &&
        !ip_is_error(base_tv.value) &&
        ip_tag(&ctx->s->intern, base_tv.value) == IP_TAG_ENUM_TYPE) {
      DefId d = {.idx = ip_key(&ctx->s->intern,
                               base_tv.value).enum_type.zir_node_id};
      (void)db_query_type_of_def(ctx->s, d);
      uint32_t nv = 0;
      const EnumVariantEntry *vs = db_enum_variants(ctx->s, d, &nv);
      uint32_t found = UINT32_MAX;
      for (uint32_t i = 0; i < nv; i++) {
        if (vs[i].name.idx == fname.idx) { found = i; break; }
      }
      if (found == UINT32_MAX) {
        db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node),
                "no variant '%S' in enum", fname);
        result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
        break;
      }
      IpKey vk = {.kind = IPK_ENUM_VARIANT_VALUE,
                  .enum_variant_value = {.enum_def = d, .variant_idx = found}};
      result = (TypedValue){.type = base_tv.value,
                            .value = ip_get(&ctx->s->intern, vk)};
      break;
    }

    // Path 2.5 — `Effect.op` member access. Base resolves to a TYPE value
    // whose underlying tag is IP_TAG_EFFECT_TYPE. db_effect_op_type's fn-type
    // carries the parent effect in its row, so the enclosing SK_CALL_EXPR
    // accumulates it (this is what closes the effect-row cascade). Without
    // this arm a type-name base (now {IP_TYPE_TYPE, effect}) drops to Path 3,
    // where infer_value_position reads only the type half (IP_TYPE_TYPE) and
    // can't dispatch — "field access on non-aggregate type type".
    if (base_tv.type.v == IP_TYPE_TYPE.v && base_tv.value.v != IP_NONE.v &&
        !ip_is_error(base_tv.value) &&
        ip_tag(&ctx->s->intern, base_tv.value) == IP_TAG_EFFECT_TYPE) {
      DefId d = {.idx = ip_key(&ctx->s->intern,
                               base_tv.value).effect_type.zir_node_id};
      (void)db_query_type_of_def(ctx->s, d); // dep + ensure ops built
      IpIndex ft = db_effect_op_type(ctx->s, d, fname);
      if (ip_is_error(ft)) {
        result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
        break;
      }
      if (ft.v == IP_NONE.v) {
        db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node),
                "no op '%S' in effect %T", fname, base_tv.value);
        result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
        break;
      }
      result = (TypedValue){.type = ft, .value = IP_NONE};
      break;
    }

    // Path 3 — runtime (struct field / slice .len / .ptr / array .len /
    // effect op). Delegate the type half to infer_value_position. Re-evals
    // base but that's harmless until Batch 5 extracts infer_field_expr.
    result = (TypedValue){.type = infer_value_position(ctx, node),
                          .value = IP_NONE};
    break;
  }

  // Phase 6 Batch 3 — bare `.variant`. Read ctx->enum_ctx_hint (set by
  // SK_SWITCH_EXPR pattern recurse or by SK_BIN_EXPR EQ_EQ/BANG_EQ retry
  // via eval_expr_with_enum_hint). If no hint, this is an error — bare
  // `.variant` requires an enum-typed context. Produces IPK_ENUM_VARIANT_VALUE.
  case SK_ENUM_REF_EXPR: {
    if (ctx->enum_ctx_hint.idx == DEF_ID_NONE.idx) {
      db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node),
              "bare '.variant' requires an enum-typed context");
      result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
      break;
    }
    EnumRefExpr er;
    if (!EnumRefExpr_cast(node, &er)) {
      result = TYPED_VALUE_NONE;
      break;
    }
    SyntaxToken *vtok = EnumRefExpr_variant(&er);
    StrId vname = vtok ? pool_intern(&ctx->s->strings,
                                     syntax_token_text(vtok),
                                     syntax_token_text_range(vtok).length)
                       : (StrId){0};
    if (vtok) syntax_token_release(vtok);
    DefId enum_def = ctx->enum_ctx_hint;
    // Force the nominal enum type to be computed so db_enum_variants is
    // populated (mirror of check.c's existing pattern).
    IpIndex enum_type = db_query_type_of_def(ctx->s, enum_def);
    uint32_t nv = 0;
    const EnumVariantEntry *vs = db_enum_variants(ctx->s, enum_def, &nv);
    uint32_t found = UINT32_MAX;
    for (uint32_t i = 0; i < nv; i++) {
      if (vs[i].name.idx == vname.idx) { found = i; break; }
    }
    if (found == UINT32_MAX) {
      db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node),
              "no variant '%S' in enum", vname);
      result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
      break;
    }
    IpKey vk = {.kind = IPK_ENUM_VARIANT_VALUE,
                .enum_variant_value = {.enum_def = enum_def,
                                       .variant_idx = found}};
    IpIndex vidx = ip_get(&ctx->s->intern, vk);
    result = (TypedValue){.type = enum_type, .value = vidx};
    break;
  }

  // Phase 6 Batch 3 — comptime wrapper. Recurse inner on a local SemaCtx
  // with in_comptime=true so SK_CALL_EXPR's effectful-call diag fires.
  // If inner doesn't fold (value=IP_NONE and not an error) emit the
  // foldability diag. Replaces sema_comptime_select.
  case SK_COMPTIME_EXPR: {
    ComptimeExpr ce;
    if (!ComptimeExpr_cast(node, &ce)) {
      result = TYPED_VALUE_NONE;
      break;
    }
    SyntaxNode *inner = ComptimeExpr_inner(&ce);
    if (!inner) {
      result = TYPED_VALUE_NONE;
      break;
    }
    SemaCtx local = *ctx;
    local.in_comptime = true;
    local.enum_ctx_hint = DEF_ID_NONE;
    TypedValue inner_tv = eval_expr(&local, inner);
    syntax_node_release(inner);
    // A function reference now carries a value (IP_TAG_FN_VALUE), but it is not
    // a foldable comptime RESULT — keep the existing "must be comptime-foldable"
    // diagnostic for `@comptime { somefn }` (preserve pre-fn-value behavior).
    bool inner_unfoldable =
        inner_tv.value.v == IP_NONE.v ||
        (!ip_is_error(inner_tv.value) &&
         ip_tag(&ctx->s->intern, inner_tv.value) == IP_TAG_FN_VALUE);
    if (!ip_is_error(inner_tv.type) && inner_tv.type.v != IP_NONE.v &&
        inner_unfoldable) {
      db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node),
              "comptime expression must be comptime-foldable");
      result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
      break;
    }
    result = inner_tv;
    break;
  }

  // Phase 6 Batch 2 — `(expr)` is a transparent passthrough; forward the
  // inner TypedValue verbatim including the value half.
  case SK_PAREN_EXPR: {
    ParenExpr pe;
    if (!ParenExpr_cast(node, &pe)) {
      result = TYPED_VALUE_NONE;
      break;
    }
    SyntaxNode *inner = ParenExpr_inner(&pe);
    if (!inner) {
      result = TYPED_VALUE_NONE;
      break;
    }
    result = eval_expr(ctx, inner);
    syntax_node_release(inner);
    break;
  }

  // Phase 6 Batch 4c — switch-expression. Eval scrutinee via eval_expr;
  // if it folds (scrut.value != IP_NONE), walk arms and match patterns
  // via tv_value_semantic_eq + tv_value_in_range (decoded scalar). On
  // match, recurse on the arm body and forward its TypedValue. If
  // scrutinee doesn't fold, delegate to infer_switch for runtime type
  // synthesis (value half stays IP_NONE).
  case SK_SWITCH_EXPR: {
    SwitchExpr sw;
    if (!SwitchExpr_cast(node, &sw)) {
      result = TYPED_VALUE_NONE;
      break;
    }
    SyntaxNode *scrutinee = SwitchExpr_scrutinee(&sw);
    SyntaxNode *arms = SwitchExpr_arms(&sw);

    SemaCtx scrut_ctx = *ctx;
    scrut_ctx.enum_ctx_hint = DEF_ID_NONE;
    TypedValue scrut_tv = scrutinee ? eval_expr(&scrut_ctx, scrutinee)
                                    : TYPED_VALUE_NONE;
    if (scrutinee) syntax_node_release(scrutinee);

    // Runtime scrut → delegate to infer_switch.
    if (scrut_tv.value.v == IP_NONE.v || ip_is_error(scrut_tv.value)) {
      if (arms) syntax_node_release(arms);
      result = (TypedValue){.type = infer_switch(ctx, node, IP_NONE),
                            .value = IP_NONE};
      break;
    }

    // Enum-typed scrutinee → set hint for bare `.variant` patterns.
    DefId pat_enum_ctx = DEF_ID_NONE;
    IpTag stag = ip_tag(&ctx->s->intern, scrut_tv.value);
    if (stag == IP_TAG_ENUM_VARIANT_VALUE) {
      pat_enum_ctx =
          ip_key(&ctx->s->intern, scrut_tv.value).enum_variant_value.enum_def;
    }

    // Walk arms; first match wins.
    SyntaxNode *winner_body = NULL;
    bool matched = false;
    if (arms) {
      uint32_t na = syntax_node_num_children(arms);
      for (uint32_t i = 0; i < na; i++) {
        SyntaxElement ael = syntax_node_child_or_token(arms, i);
        if (ael.kind != SYNTAX_ELEM_NODE || !ael.node) {
          if (ael.kind == SYNTAX_ELEM_TOKEN && ael.token)
            syntax_token_release(ael.token);
          continue;
        }
        SyntaxNode *arm = ael.node;
        if (syntax_node_kind(arm) != SK_SWITCH_ARM) {
          syntax_node_release(arm);
          continue;
        }
        SyntaxNode *body = NULL;
        bool arm_matched = false;
        uint32_t an = syntax_node_num_children(arm);
        for (uint32_t j = 0; j < an; j++) {
          SyntaxElement pel = syntax_node_child_or_token(arm, j);
          if (pel.kind == SYNTAX_ELEM_TOKEN && pel.token) {
            syntax_token_release(pel.token);
            continue;
          }
          if (pel.kind != SYNTAX_ELEM_NODE || !pel.node) continue;
          if (syntax_node_kind(pel.node) == SK_SWITCH_PATTERN_LIST) {
            uint32_t pn = syntax_node_num_children(pel.node);
            for (uint32_t pj = 0; pj < pn; pj++) {
              SyntaxElement pp = syntax_node_child_or_token(pel.node, pj);
              if (pp.kind != SYNTAX_ELEM_NODE || !pp.node) {
                if (pp.kind == SYNTAX_ELEM_TOKEN && pp.token)
                  syntax_token_release(pp.token);
                continue;
              }
              SyntaxNode *pat = pp.node;
              BinExpr rbe;
              if (arm_matched || matched) {
                // already won — nothing to test
              } else if (pattern_is_wildcard(pat)) {
                arm_matched = true;
              } else if (syntax_node_kind(pat) == SK_BIN_EXPR &&
                         BinExpr_cast(pat, &rbe) &&
                         (BinExpr_op_kind(&rbe) == SK_DOT_DOT_LT ||
                          BinExpr_op_kind(&rbe) == SK_DOT_DOT_EQ)) {
                if (tv_value_in_range(ctx, pat, scrut_tv.value))
                  arm_matched = true;
              } else {
                TypedValue pat_tv = pat_enum_ctx.idx != DEF_ID_NONE.idx
                    ? eval_expr_with_enum_hint(ctx, pat, pat_enum_ctx)
                    : eval_expr(&scrut_ctx, pat);
                if (pat_tv.value.v != IP_NONE.v && !ip_is_error(pat_tv.value)
                    && tv_value_semantic_eq(ctx->s, pat_tv.value,
                                            scrut_tv.value))
                  arm_matched = true;
              }
              syntax_node_release(pat);
            }
            syntax_node_release(pel.node);
          } else {
            if (body) syntax_node_release(body);
            body = pel.node;
          }
        }
        if (body) {
          if (arm_matched && !matched) {
            winner_body = body;
            matched = true;
          } else {
            syntax_node_release(body);
          }
        }
        syntax_node_release(arm);
      }
      syntax_node_release(arms);
    }

    if (!matched || !winner_body) {
      if (winner_body) syntax_node_release(winner_body);
      // Scrut folded but no arm matched — fall back to runtime infer_switch
      // (which handles exhaustiveness diagnostics for the general case).
      result = (TypedValue){.type = infer_switch(ctx, node, IP_NONE),
                            .value = IP_NONE};
      break;
    }
    result = eval_expr(ctx, winner_body);
    syntax_node_release(winner_body);
    break;
  }

  // Phase 6 Batch 4b — if-expression. Cond folds via eval_expr; if
  // cond.value is IP_BOOL_TRUE/FALSE, recurse only on the taken branch
  // and forward its TypedValue (so a comptime-if can fold its value).
  // Otherwise recurse both branches, peer_unify types, value=IP_NONE.
  // Subsumes infer_comptime_if (Batch 4b deletion).
  case SK_IF_EXPR: {
    IfExpr ie;
    if (!IfExpr_cast(node, &ie)) {
      result = TYPED_VALUE_NONE;
      break;
    }
    SyntaxNode *cond    = IfExpr_condition(&ie);
    SyntaxNode *capture = IfExpr_capture(&ie);
    SyntaxNode *then_b  = IfExpr_then_branch(&ie);
    SyntaxNode *else_b  = IfExpr_else_branch(&ie);
    bool had_else = (else_b != NULL);
    // Eval cond ONCE. Inline what handle_if_cond does so we don't
    // double-eval (which would double-diag for an erroring cond like
    // `^usize == 0` in W3).
    SemaCtx sub_ctx = *ctx;
    sub_ctx.enum_ctx_hint = DEF_ID_NONE;
    TypedValue cond_tv = cond ? eval_expr(&sub_ctx, cond) : TYPED_VALUE_NONE;
    if (capture) {
      // While-let / if-let style: cond must be optional. Push the
      // unwrapped elem type at the capture node.
      IpIndex elem = IP_NONE;
      IpIndex ct = cond_tv.type;
      if (ip_is_error(ct)) {
        elem = IP_ERROR_TYPE;  // sticky — already diag'd
      } else if (ct.v != IP_NONE.v) {
        if (ip_tag(&ctx->s->intern, ct) == IP_TAG_OPTIONAL_TYPE) {
          elem = ip_key(&ctx->s->intern, ct).optional_type.elem;
        } else {
          db_emit(ctx->s, DIAG_ERROR, span_of(ctx, capture),
                  "capture binding requires optional condition; got %T", ct);
        }
      }
      node_typed_value_push(ctx, capture, elem, IP_NONE);
    } else if (cond) {
      // No capture — cond must be bool. Don't double-emit if it already
      // erred (e.g. W3's ptr-vs-int). Skip on IP_NONE / error.
      if (!ip_is_error(cond_tv.type) && cond_tv.type.v != IP_NONE.v &&
          cond_tv.type.v != IP_BOOL_TYPE.v) {
        db_emit(ctx->s, DIAG_ERROR, span_of(ctx, cond),
                "if condition must be bool, got %T", cond_tv.type);
      }
    }
    if (cond)    syntax_node_release(cond);
    if (capture) syntax_node_release(capture);

    // Comptime fold: if cond folded to IP_BOOL_TRUE/FALSE, recurse only
    // on the taken branch and forward its TypedValue.
    bool cond_true  = (cond_tv.value.v == IP_BOOL_TRUE.v);
    bool cond_false = (cond_tv.value.v == IP_BOOL_FALSE.v);
    if (cond_true || cond_false) {
      SyntaxNode *winner = cond_true ? then_b : else_b;
      SyntaxNode *loser  = cond_true ? else_b : then_b;
      if (winner) {
        result = eval_expr(ctx, winner);
        syntax_node_release(winner);
      } else {
        // Taken branch missing (e.g. `comptime if (true) X` with no else
        // when cond_false). Default to void.
        result = (TypedValue){.type = IP_VOID_TYPE, .value = IP_NONE};
      }
      if (loser) syntax_node_release(loser);
      break;
    }

    // Runtime cond — peer-unify branch types; value=IP_NONE.
    IpIndex tt = then_b ? eval_expr(ctx, then_b).type : IP_VOID_TYPE;
    IpIndex et = had_else ? eval_expr(ctx, else_b).type : IP_VOID_TYPE;
    if (then_b) syntax_node_release(then_b);
    if (else_b) syntax_node_release(else_b);
    IpIndex u = had_else ? peer_unify(tt, et) : IP_NONE;
    if (had_else && u.v == IP_NONE.v && tt.v != IP_NONE.v &&
        et.v != IP_NONE.v && tt.v != IP_VOID_TYPE.v &&
        et.v != IP_VOID_TYPE.v) {
      db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node),
              "if branches have incompatible types (%T and %T)", tt, et);
      result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
      break;
    }
    result = (TypedValue){.type = (u.v != IP_NONE.v) ? u : IP_VOID_TYPE,
                          .value = IP_NONE};
    break;
  }

  // Phase 6 Batch 4a — binary operator. Folds value half through the
  // promoted binop_* helpers (Batches 1-2). Bidirectional enum-variant
  // retry: `enum_val == .variant` (or the reverse) re-evaluates the bare
  // `.variant` side with enum_ctx_hint set to the typed side's enum_def,
  // producing IPK_ENUM_VARIANT_VALUE that tv_value_semantic_eq decodes.
  case SK_BIN_EXPR: {
    BinExpr be;
    if (!BinExpr_cast(node, &be)) {
      result = TYPED_VALUE_NONE;
      break;
    }
    SyntaxKind opk = BinExpr_op_kind(&be);
    SyntaxNode *lhs_n = BinExpr_lhs(&be);
    SyntaxNode *rhs_n = BinExpr_rhs(&be);

    // Reset enum_ctx_hint on operand recursion EXCEPT in EQ_EQ/BANG_EQ
    // where we may need to push it back for the bare-variant side.
    SemaCtx op_ctx = *ctx;
    op_ctx.enum_ctx_hint = DEF_ID_NONE;

    // Bidirectional enum-variant compare: one side is bare `.variant`
    // (SK_ENUM_REF_EXPR), the other resolves to an enum-typed value.
    if ((opk == SK_EQ_EQ || opk == SK_BANG_EQ)) {
      bool lhs_bare = lhs_n && syntax_node_kind(lhs_n) == SK_ENUM_REF_EXPR;
      bool rhs_bare = rhs_n && syntax_node_kind(rhs_n) == SK_ENUM_REF_EXPR;
      if (lhs_bare != rhs_bare) {
        SyntaxNode *typed_n = lhs_bare ? rhs_n : lhs_n;
        SyntaxNode *bare_n  = lhs_bare ? lhs_n : rhs_n;
        TypedValue typed_tv = typed_n ? eval_expr(&op_ctx, typed_n)
                                      : TYPED_VALUE_NONE;
        TypedValue ltv, rtv;
        if (ip_is_error(typed_tv.type)) {
          result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
          if (lhs_n) syntax_node_release(lhs_n);
          if (rhs_n) syntax_node_release(rhs_n);
          break;
        }
        if (typed_tv.type.v == IP_NONE.v ||
            ip_tag(&ctx->s->intern, typed_tv.type) != IP_TAG_ENUM_TYPE) {
          // Typed side isn't an enum — fall through to the regular compare
          // path. Just evaluate bare side without hint; it'll error.
          TypedValue bare_tv = bare_n ? eval_expr(&op_ctx, bare_n)
                                      : TYPED_VALUE_NONE;
          ltv = lhs_bare ? bare_tv : typed_tv;
          rtv = lhs_bare ? typed_tv : bare_tv;
        } else {
          // Typed side is an enum. Push enum_ctx_hint to its def for the
          // bare side's recursion (single-shot via eval_expr_with_enum_hint).
          DefId enum_def = {
              .idx = ip_key(&ctx->s->intern, typed_tv.type).enum_type.zir_node_id};
          TypedValue bare_tv = bare_n
              ? eval_expr_with_enum_hint(ctx, bare_n, enum_def)
              : TYPED_VALUE_NONE;
          ltv = lhs_bare ? bare_tv : typed_tv;
          rtv = lhs_bare ? typed_tv : bare_tv;
        }
        if (lhs_n) syntax_node_release(lhs_n);
        if (rhs_n) syntax_node_release(rhs_n);
        result = binop_compare(ctx, node, opk, ltv, rtv);
        break;
      }
    }

    // Regular path: both operands evaluate with enum_ctx_hint cleared.
    TypedValue ltv = lhs_n ? eval_expr(&op_ctx, lhs_n) : TYPED_VALUE_NONE;
    TypedValue rtv = rhs_n ? eval_expr(&op_ctx, rhs_n) : TYPED_VALUE_NONE;
    if (lhs_n) syntax_node_release(lhs_n);
    if (rhs_n) syntax_node_release(rhs_n);
    if (ip_is_error(ltv.type) || ip_is_error(rtv.type)) {
      result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
      break;
    }
    if (ltv.type.v == IP_NONE.v || rtv.type.v == IP_NONE.v) {
      result = TYPED_VALUE_NONE;
      break;
    }
    switch (opk) {
    case SK_PLUS: case SK_MINUS: case SK_STAR:
    case SK_SLASH: case SK_PERCENT: case SK_STAR_STAR:
      result = binop_arith(ctx, node, opk, ltv, rtv);
      break;
    case SK_EQ_EQ: case SK_BANG_EQ: case SK_LT:
    case SK_LE: case SK_GT: case SK_GE:
      result = binop_compare(ctx, node, opk, ltv, rtv);
      break;
    case SK_AMP_AMP: case SK_PIPE_PIPE:
      result = binop_logical(ctx, node, opk, ltv, rtv);
      break;
    case SK_AMP: case SK_PIPE: case SK_CARET:
    case SK_SHL: case SK_SHR:
      result = binop_bitop(ctx, node, opk, ltv, rtv);
      break;
    case SK_ORELSE_KW:
      result = binop_orelse(ctx, node, ltv);
      break;
    case SK_DOT_DOT_LT:
    case SK_DOT_DOT_EQ:
      db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node),
              "range expressions are only allowed in a loop header "
              "(`loop (lo..<hi) <i>`) or a switch pattern; stored Range "
              "values are not supported yet");
      result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
      break;
    default:
      db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node),
              "binary operator '%s' not yet supported in type inference",
              opkind_name(opk));
      result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
      break;
    }
    break;
  }

  // Phase 6 Batch 2 — prefix ops (& - ~ !). Address-of stays runtime
  // (value=IP_NONE); the others fold their value half via tv_prefix_*
  // when the operand carries a known scalar.
  case SK_PREFIX_EXPR: {
    PrefixExpr pe;
    if (!PrefixExpr_cast(node, &pe)) {
      result = TYPED_VALUE_NONE;
      break;
    }
    SyntaxKind opk = PrefixExpr_op_kind(&pe);
    SyntaxNode *operand = PrefixExpr_operand(&pe);
    if (!operand) {
      result = TYPED_VALUE_NONE;
      break;
    }
    if (opk == SK_AMP) {
      // Address-of: l-value check before recursion.
      SyntaxKind ck = syntax_node_kind(operand);
      bool is_lvalue = (ck == SK_REF_EXPR || ck == SK_PATH_EXPR ||
                        ck == SK_FIELD_EXPR || ck == SK_INDEX_EXPR);
      if (!is_lvalue && ck == SK_POSTFIX_EXPR) {
        PostfixExpr in;
        if (PostfixExpr_cast(operand, &in) &&
            PostfixExpr_op_kind(&in) == SK_CARET)
          is_lvalue = true;
      }
      if (!is_lvalue) {
        db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node),
                "address-of '&' requires an l-value (variable, field, index, "
                "or deref)");
        syntax_node_release(operand);
        result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
        break;
      }
      IpIndex t = eval_expr(ctx, operand).type;
      syntax_node_release(operand);
      if (ip_is_error(t)) {
        result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
        break;
      }
      if (t.v == IP_NONE.v) {
        result = TYPED_VALUE_NONE;
        break;
      }
      IpKey ptr_key = {.kind = IPK_PTR_TYPE,
                       .ptr_type = {.elem = t, .is_const = false}};
      result = (TypedValue){.type = ip_get(&ctx->s->intern, ptr_key),
                            .value = IP_NONE};
      break;
    }
    TypedValue op_tv = eval_expr(ctx, operand);
    syntax_node_release(operand);
    if (ip_is_error(op_tv.type)) {
      result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
      break;
    }
    if (op_tv.type.v == IP_NONE.v) {
      result = TYPED_VALUE_NONE;
      break;
    }
    if (opk == SK_MINUS) {
      if (!is_numeric(op_tv.type)) {
        db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node),
                "unary '-' requires numeric operand, got %T", op_tv.type);
        result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
        break;
      }
      result = (TypedValue){.type = op_tv.type,
                            .value = tv_prefix_neg(ctx->s, op_tv.value)};
      break;
    }
    if (opk == SK_TILDE) {
      if (op_tv.type.v != IP_COMPTIME_INT_TYPE.v &&
          !is_concrete_int(op_tv.type)) {
        db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node),
                "unary '~' requires integer operand, got %T", op_tv.type);
        result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
        break;
      }
      result = (TypedValue){.type = op_tv.type,
                            .value = tv_prefix_comp(ctx->s, op_tv.value)};
      break;
    }
    if (opk == SK_BANG) {
      if (op_tv.type.v != IP_BOOL_TYPE.v) {
        db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node),
                "unary '!' requires bool, got %T", op_tv.type);
        result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
        break;
      }
      result = (TypedValue){.type = IP_BOOL_TYPE,
                            .value = tv_prefix_not(op_tv.value)};
      break;
    }
    db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node),
            "prefix operator '%s' not yet supported in type inference",
            opkind_name(opk));
    result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
    break;
  }

  // Phase 6 Batch 2 — postfix ops (^ deref, .? optional unwrap, ++/--).
  // All runtime-by-nature; value half stays IP_NONE. Type matches
  // infer_value_position's existing logic.
  case SK_POSTFIX_EXPR: {
    PostfixExpr po;
    if (!PostfixExpr_cast(node, &po)) {
      result = TYPED_VALUE_NONE;
      break;
    }
    SyntaxKind opk = PostfixExpr_op_kind(&po);
    SyntaxNode *operand = PostfixExpr_operand(&po);
    if (!operand) {
      result = TYPED_VALUE_NONE;
      break;
    }
    // `::` consts can't be mutated by `++` / `--`.
    if (opk == SK_PLUS_PLUS || opk == SK_MINUS_MINUS)
      (void)reject_const_mutation(ctx, operand, "modify");
    IpIndex t = eval_expr(ctx, operand).type;
    syntax_node_release(operand);
    if (ip_is_error(t)) {
      result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
      break;
    }
    if (t.v == IP_NONE.v) {
      result = TYPED_VALUE_NONE;
      break;
    }
    if (opk == SK_CARET) {
      IpTag tag = ip_tag(&ctx->s->intern, t);
      if (tag != IP_TAG_PTR_TYPE && tag != IP_TAG_PTR_CONST_TYPE) {
        db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node),
                "cannot dereference non-pointer type %T", t);
        result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
        break;
      }
      result = (TypedValue){.type = ip_key(&ctx->s->intern, t).ptr_type.elem,
                            .value = IP_NONE};
      break;
    }
    if (opk == SK_QUESTION) {
      if (ip_tag(&ctx->s->intern, t) != IP_TAG_OPTIONAL_TYPE) {
        db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node),
                "'.?' requires optional type, got %T", t);
        result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
        break;
      }
      result = (TypedValue){
          .type = ip_key(&ctx->s->intern, t).optional_type.elem,
          .value = IP_NONE};
      break;
    }
    // ++ / -- : type matches operand (statement-like)
    result = (TypedValue){.type = t, .value = IP_NONE};
    break;
  }

  case SK_REF_EXPR:
  case SK_REF_TYPE: {
    SyntaxToken *name_tok = ast_first_token(node, SK_IDENT);
    StrId name = name_tok ? pool_intern(&ctx->s->strings,
                                        syntax_token_text(name_tok),
                                        syntax_token_text_range(name_tok).length)
                          : (StrId){0};
    if (name_tok)
      syntax_token_release(name_tok);
    result = eval_name(ctx, node, name);
    break;
  }
  case SK_PATH_EXPR:
  case SK_PATH_TYPE: {
    StrId name = path_expr_leaf_name(ctx->s, node);
    result = eval_name(ctx, node, name);
    break;
  }

  case SK_BUILTIN_EXPR: {
    // Phase 2 supports `@TypeOf(x)` in type position (the canonical
    // generic-return idiom — resolves to the same TYPE_VAR hole the
    // parameter `x` was bound to via type_name_map). Other builtins
    // (`@sizeOf`, `@alignOf`, `@typeName`) are VALUE-producing and route
    // through const_eval today; Phase 3 folds them in. Until then,
    // emit the same "not usable in type position" diag as before.
    BuiltinExpr be;
    if (!BuiltinExpr_cast(node, &be)) {
      db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node),
              "malformed builtin expression");
      result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
      break;
    }
    SyntaxToken *bname_tok = BuiltinExpr_name(&be);
    StrId bname = bname_tok ? pool_intern(&ctx->s->strings,
                                          syntax_token_text(bname_tok),
                                          syntax_token_text_range(bname_tok).length)
                            : (StrId){0};
    if (bname_tok)
      syntax_token_release(bname_tok);
    SyntaxNode *arg_list = BuiltinExpr_args(&be);
    IpIndex hole_match = IP_NONE;
    if (arg_list && ctx->type_name_map &&
        db_builtin_kind_of(ctx->s, bname) == BUILTIN_TYPEOF) {
      uint32_t ac = syntax_node_num_children(arg_list);
      for (uint32_t i = 0; i < ac; i++) {
        SyntaxElement ae = syntax_node_child_or_token(arg_list, i);
        if (ae.kind != SYNTAX_ELEM_NODE || !ae.node) {
          if (ae.kind == SYNTAX_ELEM_TOKEN && ae.token)
            syntax_token_release(ae.token);
          continue;
        }
        SyntaxKind ak = syntax_node_kind(ae.node);
        if (ak == SK_REF_EXPR || ak == SK_REF_TYPE) {
          SyntaxToken *rt = ast_first_token(ae.node, SK_IDENT);
          StrId rn = rt ? pool_intern(&ctx->s->strings, syntax_token_text(rt),
                                      syntax_token_text_range(rt).length)
                        : (StrId){0};
          if (rt)
            syntax_token_release(rt);
          void *hole = rn.idx != 0
                           ? hashmap_get(ctx->type_name_map, (uint64_t)rn.idx)
                           : NULL;
          if (hole)
            hole_match = (IpIndex){.v = (uint32_t)(uintptr_t)hole};
        }
        syntax_node_release(ae.node);
        break; // @TypeOf takes a single arg
      }
    }
    if (hole_match.v != IP_NONE.v) {
      // Type-position @TypeOf inside a signature scope — resolve to the
      // hole minted for the parameter named in the arg.
      if (arg_list) syntax_node_release(arg_list);
      result = (TypedValue){.type = IP_TYPE_TYPE, .value = hole_match};
      break;
    }
    // Phase 6 Batch 3 — value-half production for the foldable builtins.
    // @sizeOf/@alignOf → IPK_INT_VALUE via db_layout_of_type.
    // @import → IPK_NAMESPACE_VALUE via s->virtual_by_name lookup.
    // Everything else falls back to infer_value_position (type-only).
    IpIndex val_half = IP_NONE;
    BuiltinKind bk = db_builtin_kind_of(ctx->s, bname);
    if ((bk == BUILTIN_SIZEOF || bk == BUILTIN_ALIGNOF) && arg_list) {
      // First node child of arg_list is the type expression.
      SyntaxNode *type_arg = NULL;
      uint32_t total = syntax_node_num_children(arg_list);
      for (uint32_t i = 0; i < total; i++) {
        SyntaxElement el = syntax_node_child_or_token(arg_list, i);
        if (el.kind == SYNTAX_ELEM_NODE && el.node) {
          if (!type_arg) type_arg = el.node;
          else syntax_node_release(el.node);
        } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
          syntax_token_release(el.token);
        }
      }
      if (type_arg) {
        // The arg is a type expression; eval_expr's type-arm produces
        // {IP_TYPE_TYPE, type_idx}. Read the value half to get the type.
        TypedValue arg_tv = eval_expr(ctx, type_arg);
        syntax_node_release(type_arg);
        if (arg_tv.value.v != IP_NONE.v && !ip_is_error(arg_tv.value)) {
          OreLayout L = db_layout_of_type(ctx->s, arg_tv.value);
          if (L.is_known) {
            int64_t v = (bk == BUILTIN_SIZEOF) ? (int64_t)L.size
                                               : (int64_t)L.align;
            IpKey ikey = {.kind = IPK_INT_VALUE,
                          .int_value = {.type = IP_USIZE_TYPE, .value = v}};
            val_half = ip_get(&ctx->s->intern, ikey);
          }
        }
      }
    } else if (bk == BUILTIN_IMPORT && arg_list) {
      // Pull the first string-literal token from the first arg's children.
      SyntaxToken *str_tok = NULL;
      uint32_t total = syntax_node_num_children(arg_list);
      for (uint32_t i = 0; i < total; i++) {
        SyntaxElement el = syntax_node_child_or_token(arg_list, i);
        if (el.kind == SYNTAX_ELEM_NODE && el.node) {
          if (!str_tok) {
            uint32_t inner = syntax_node_num_children(el.node);
            for (uint32_t j = 0; j < inner; j++) {
              SyntaxElement ie = syntax_node_child_or_token(el.node, j);
              if (ie.kind == SYNTAX_ELEM_TOKEN && ie.token &&
                  syntax_token_kind(ie.token) == SK_STRING_LIT && !str_tok) {
                str_tok = ie.token;
              } else if (ie.kind == SYNTAX_ELEM_TOKEN && ie.token) {
                syntax_token_release(ie.token);
              } else if (ie.kind == SYNTAX_ELEM_NODE && ie.node) {
                syntax_node_release(ie.node);
              }
            }
          }
          syntax_node_release(el.node);
        } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
          syntax_token_release(el.token);
        }
      }
      if (str_tok) {
        const char *txt = syntax_token_text(str_tok);
        uint32_t len = syntax_token_text_range(str_tok).length;
        syntax_token_release(str_tok);
        if (len >= 2 && txt[0] == '"' && txt[len - 1] == '"') {
          StrId path = pool_intern(&ctx->s->strings, txt + 1, len - 2);
          if (path.idx != 0) {
            void *v = hashmap_get(&ctx->s->virtual_by_name,
                                  (uint64_t)path.idx);
            if (v) {
              SourceId vsrc = {.idx = (uint32_t)(uintptr_t)v};
              FileId vfid = db_lookup_file_by_source(ctx->s, vsrc);
              if (vfid.idx != 0) {
                NamespaceId vns = db_get_file_namespace(ctx->s, vfid);
                if (vns.idx != 0) {
                  IpKey nk = {.kind = IPK_NAMESPACE_VALUE,
                              .namespace_value = {.nsid = vns}};
                  val_half = ip_get(&ctx->s->intern, nk);
                }
              }
            }
          }
        }
      }
      // arg_list children already released in the loop above; release the
      // arg_list itself below.
    }
    if (arg_list) syntax_node_release(arg_list);
    // Type half via infer_value_position (which handles every builtin
    // including the ones we don't fold).
    IpIndex ty_half = infer_value_position(ctx, node);
    result = (TypedValue){.type = ty_half, .value = val_half};
    break;
  }

  case SK_CONST_TYPE: {
    // `const T` — unwrap. const-ness is binding-level metadata; the type
    // layer just unwraps it. The result of `const T` IS the type T.
    ConstType ct;
    if (ConstType_cast(node, &ct)) {
      SyntaxNode *inner = ConstType_inner(&ct);
      if (inner) {
        result = eval_expr(ctx, inner);
        syntax_node_release(inner);
        break;
      }
    }
    db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node), "malformed type expression");
    result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
    break;
  }
  case SK_OPTIONAL_TYPE: {
    OptionalType ot;
    if (!OptionalType_cast(node, &ot)) {
      db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node), "malformed type expression");
      result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
      break;
    }
    SyntaxNode *inner = OptionalType_inner(&ot);
    TypedValue elem_tv = inner ? eval_expr(ctx, inner) : TYPED_VALUE_NONE;
    if (inner)
      syntax_node_release(inner);
    if (ip_is_error(elem_tv.value)) {
      result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
      break;
    }
    if (elem_tv.value.v == IP_NONE.v) {
      db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node), "malformed type expression");
      result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
      break;
    }
    IpKey key = {.kind = IPK_OPTIONAL_TYPE,
                 .optional_type = {.elem = elem_tv.value}};
    result = (TypedValue){.type = IP_TYPE_TYPE,
                          .value = ip_get(&ctx->s->intern, key)};
    break;
  }
  case SK_PTR_TYPE:
  case SK_SLICE_TYPE:
  case SK_MANY_PTR_TYPE: {
    // `^T`, `[]T`, `[*]T` — possibly wrapped around `const T` (const flag).
    SyntaxNode *child = NULL;
    if (k == SK_PTR_TYPE) {
      PtrType pt;
      if (PtrType_cast(node, &pt))
        child = PtrType_pointee(&pt);
    } else if (k == SK_SLICE_TYPE) {
      SliceType st;
      if (SliceType_cast(node, &st))
        child = SliceType_element(&st);
    } else {
      ManyPtrType mt;
      if (ManyPtrType_cast(node, &mt))
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
    TypedValue elem_tv = child ? eval_expr(ctx, child) : TYPED_VALUE_NONE;
    if (child)
      syntax_node_release(child);
    if (ip_is_error(elem_tv.value)) {
      result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
      break;
    }
    if (elem_tv.value.v == IP_NONE.v) {
      db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node), "malformed type expression");
      result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
      break;
    }
    IpKey key = {0};
    if (k == SK_PTR_TYPE) {
      key.kind = IPK_PTR_TYPE;
      key.ptr_type.elem = elem_tv.value;
      key.ptr_type.is_const = is_const;
    } else if (k == SK_SLICE_TYPE) {
      key.kind = IPK_SLICE_TYPE;
      key.slice_type.elem = elem_tv.value;
      key.slice_type.is_const = is_const;
    } else {
      key.kind = IPK_MANY_PTR_TYPE;
      key.many_ptr_type.elem = elem_tv.value;
      key.many_ptr_type.is_const = is_const;
    }
    result = (TypedValue){.type = IP_TYPE_TYPE,
                          .value = ip_get(&ctx->s->intern, key)};
    break;
  }
  case SK_ARRAY_TYPE: {
    // `[N]T`. eval_expr the size; expect a comptime integer. eval_expr
    // the element; expect a type. Intern IPK_ARRAY_TYPE. Phase 2's port
    // through eval_expr means named-const sizes work (Phase A's literal-
    // only restriction is lifted as a happy side effect).
    ArrayType at;
    if (!ArrayType_cast(node, &at))
      break;
    SyntaxNode *size_node = ArrayType_size(&at);
    SyntaxNode *elem_node = ArrayType_element(&at);
    if (!size_node) {
      db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node),
              "array type missing size expression");
      result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
      if (elem_node) syntax_node_release(elem_node);
      break;
    }
    TypedValue size_tv = eval_expr(ctx, size_node);
    uint64_t size = 0;
    bool size_ok = false;
    if (size_tv.value.v != IP_NONE.v && !ip_is_error(size_tv.value) &&
        ip_tag(&ctx->s->intern, size_tv.value) == IP_TAG_INT_VALUE) {
      IpKey sk = ip_key(&ctx->s->intern, size_tv.value);
      if (sk.int_value.value >= 0) {
        size = (uint64_t)sk.int_value.value;
        size_ok = true;
      }
    }
    if (!size_ok) {
      db_emit(ctx->s, DIAG_ERROR, span_of(ctx, size_node),
              "array size must be a non-negative comptime integer");
      result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
      syntax_node_release(size_node);
      if (elem_node) syntax_node_release(elem_node);
      break;
    }
    TypedValue elem_tv = elem_node ? eval_expr(ctx, elem_node) : TYPED_VALUE_NONE;
    if (elem_node) syntax_node_release(elem_node);
    syntax_node_release(size_node);
    if (ip_is_error(elem_tv.value) || elem_tv.value.v == IP_NONE.v) {
      result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
      break;
    }
    IpKey key = {.kind = IPK_ARRAY_TYPE,
                 .array_type = {.elem = elem_tv.value, .size = size}};
    result = (TypedValue){.type = IP_TYPE_TYPE,
                          .value = ip_get(&ctx->s->intern, key)};
    break;
  }

  case SK_FN_TYPE: {
    FnType ft;
    if (!FnType_cast(node, &ft))
      break;
    SyntaxNode *ret = FnType_return_type(&ft);
    SyntaxNode *params = FnType_params(&ft);
    SyntaxNode *er = FnType_effect_row(&ft);
    IpIndex fn_ty = build_fn_type(ctx, ret, params, er);
    if (ret)
      syntax_node_release(ret);
    if (params)
      syntax_node_release(params);
    if (er)
      syntax_node_release(er);
    result = (TypedValue){.type = IP_TYPE_TYPE, .value = fn_ty};
    break;
  }
  case SK_DISTINCT_TYPE:
    db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node),
            "distinct must be named: `MyT :: distinct <type>`");
    result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
    break;

  // Phase 6 Batch 5a — exhaustive dispatch. All remaining SK_* arms get
  // explicit case labels here that delegate to infer_value_position
  // (which holds the arm bodies until Batch 5b extracts per-kind helpers).
  // The point of the explicit case labels is the architectural endpoint:
  // eval_expr's switch is the single dispatch surface. Default becomes
  // an ICE — no fallback through default to infer_value_position.
  // CTFE — a comptime call folds to a scalar value when the callee is a pure,
  // non-generic user fn and every arg folds; otherwise eval_call returns the
  // SAFE runtime fallback (type from inference, value IP_NONE), exactly as the
  // old stub did. See eval_call above.
  case SK_CALL_EXPR:
    result = eval_call(ctx, node);
    break;

  case SK_INDEX_EXPR:
  case SK_SLICE_EXPR:
  case SK_RETURN_STMT:
  case SK_BLOCK_STMT:
  case SK_BIND_DECL:
  case SK_LOOP_EXPR:
  case SK_ASSIGN_EXPR:
  case SK_DEFER_STMT:
  case SK_EXPR_STMT:
  case SK_BREAK_STMT:
  case SK_CONTINUE_STMT:
  case SK_HANDLER_EXPR:
  case SK_LAMBDA_EXPR:
  case SK_PRODUCT_EXPR:
  case SK_INIT_LIST:
    result = (TypedValue){.type = infer_value_position(ctx, node),
                          .value = IP_NONE};
    break;

  default:
    // Phase 6 Batch 5a — ICE: every expression/statement kind that the
    // parser can produce should have an explicit case arm above. A
    // default hit means a new SyntaxKind was added without an arm.
    db_emit(ctx->s, DIAG_ERROR, span_of(ctx, node),
            "internal: expression kind %d has no eval rule (please file)",
            (int)syntax_node_kind(node));
    result = (TypedValue){.type = IP_ERROR_TYPE, .value = IP_ERROR_TYPE};
    break;
  }

  // Phase 1 plumbing — stamp both halves at this node for hover + downstream
  // consumers. No-op if ctx->types is NULL or both halves are IP_NONE.
  if (ctx && ctx->types &&
      (result.type.v != IP_NONE.v || result.value.v != IP_NONE.v)) {
    node_typed_value_push(ctx, node, result.type, result.value);
  }
  return result;
}
