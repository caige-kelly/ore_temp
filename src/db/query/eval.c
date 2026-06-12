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

#include "../diag/diag.h"

#include "../../ast/ast.h"
#include "../../ast/ast_decl.h"
#include "../../ast/ast_expr.h"
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

// Resolve a (possibly local-or-namespace) name into a TypedValue. Shared
// between SK_REF_* and SK_PATH_* (the latter already extracts the leaf
// name via path_expr_leaf_name). Returns TYPED_VALUE_NONE for an empty
// name (no diag); emits an "unknown name" diag and returns error sentinel
// on a real unresolved lookup.
static TypedValue eval_name(const SemaCtx *ctx, SyntaxNode *node, StrId name) {
  struct db *s = ctx->s;
  if (name.idx == 0)
    return TYPED_VALUE_NONE;

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
  case KIND_FUNCTION:
  case KIND_VARIABLE:
  default: {
    // Value-kind def — fn pointer, top-level mutable. Carry its type.
    IpIndex ty = db_query_type_of_def(s, tgt);
    return (TypedValue){.type = ty, .value = IP_NONE};
  }
  }
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
    if (!ip_is_error(inner_tv.type) && inner_tv.type.v != IP_NONE.v &&
        inner_tv.value.v == IP_NONE.v) {
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

  default:
    // Phase 4+5 — delegate to the value-position synthesis helper for
    // any arm not yet hoisted into eval_expr's switch. value half is
    // IP_NONE here; each arm that hoists into eval_expr above will
    // populate its own value half as the TypedValue threading lands.
    result = (TypedValue){.type = infer_value_position(ctx, node),
                          .value = IP_NONE};
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
