// Body inference (Phase D2.4) — the per-fn body type checker.
//
//   type_of_expr(ctx, node)  — synthesize an expression's type (HELPER, not
//                              memoized); pushes every visited node's type into
//                              the active NodeTypeBuilder (ctx->types).
//   check_expr(ctx, node, T)  — bidirectional check against an expected type;
//                              coercion + diags. Propagates `expected` into
//                              block tails + if branches.
//   infer_body(def)          — the QUERY: type a fn body against its declared
//                              return type, accumulate the node→type map.
//
// Ported from src/sema/{type_of_expr,check_expr,infer_body}.c. The type
// algorithms are unchanged; the dep plumbing is rewired onto the D1–D2.3 layer:
//   - body_scopes is now STRUCTURAL (D2.3): a local binding carries a bind_site
//     node, not a type. So infer OWNS local types — it stores each binding's
//     type in the node→type map keyed by the bind_site node, and a local ref
//     resolves via db_body_scope_lookup → bind_site → that map. No separate
//     locals map.
//   - nominal field/variant/member lists moved out of the intern pool (D2.1b/
//     D2.2) into db pools — field access reads db_aggregate_field_*/
//     db_enum_variants/db_namespace_member_* keyed by the inline zir/nsid.
//   - preamble is top_level_entry + raw green root (no decl_ast /
//   TOP_LEVEL_INDEX
//     ensure / multi-file loop).

#define ORE_ENGINE_PRIVATE
#include "engine.h"
#include "engine_internal.h"
#include "result_columns.h" // db.h, ids.h, intern_pool.h, syntax.h
#include "type_layer.h"
#include "builtins.h"       // D3.2: SK_BUILTIN_EXPR dispatch

#include "../diag/diag.h" // db_emit, diag_anchor_of_node, DIAG_*

#include "../../ast/ast_decl.h"
#include "../../ast/ast_expr.h"
#include "../../ast/ast_stmt.h"
#include "../../ast/ast_type.h" // ArrayType_cast/_size/_element (D2.6 inferred-size)
#include "../../support/data_structure/arena.h"
#include "../../support/data_structure/hashmap.h"
#include "../../support/data_structure/stringpool.h"
#include "../../syntax/syntax_kind.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// --- Cross-layer queries -----------------------------------------------------
extern TopLevelEntry db_query_top_level_entry(db_query_ctx *ctx,
                                              NamespaceId nsid, StrId name);
extern uint64_t parse_int_literal(SyntaxToken *tok);
extern NamespaceScopes db_query_namespace_scopes(db_query_ctx *ctx,
                                                 NamespaceId nsid);
extern DefId db_query_resolve_ref(db_query_ctx *ctx, ScopeId scope, StrId name);
extern IpIndex db_query_type_of_def(db_query_ctx *ctx, DefId def);
extern const FnSignature *db_query_fn_signature(db_query_ctx *ctx, DefId def);
extern IpIndex db_query_namespace_type(db_query_ctx *ctx, NamespaceId nsid);
extern const FnBody *db_query_body_scopes(db_query_ctx *ctx, DefId def);
extern SyntaxNodePtr db_body_scope_lookup(db_query_ctx *ctx, DefId fn_def,
                                          SyntaxNode *use_node, StrId name);

// --- Forward decls (mutually recursive) --------------------------------------
IpIndex type_of_expr(const SemaCtx *ctx, SyntaxNode *node);
bool check_expr(const SemaCtx *ctx, SyntaxNode *node, IpIndex expected);

// ============================================================================
// Numeric predicates + arith unification (Zig-style). Reserved primitives have
// IpIndex.v < 32, so each predicate is a shift+mask over the index.
// ============================================================================

#define IP_BIT(name) (1u << IP_INDEX_##name##_TYPE)

static const uint32_t CONCRETE_INT_MASK =
    IP_BIT(U8) | IP_BIT(I8) | IP_BIT(U16) | IP_BIT(I16) | IP_BIT(U32) |
    IP_BIT(I32) | IP_BIT(U64) | IP_BIT(I64) | IP_BIT(USIZE) | IP_BIT(ISIZE);
static const uint32_t CONCRETE_FLOAT_MASK = IP_BIT(F32) | IP_BIT(F64);
static const uint32_t COMPTIME_NUMERIC_MASK =
    IP_BIT(COMPTIME_INT) | IP_BIT(COMPTIME_FLOAT);
static const uint32_t NUMERIC_MASK =
    CONCRETE_INT_MASK | CONCRETE_FLOAT_MASK | COMPTIME_NUMERIC_MASK;

static bool is_concrete_int(IpIndex t) {
  return t.v < 32u && ((CONCRETE_INT_MASK >> t.v) & 1u);
}
static bool is_concrete_float(IpIndex t) {
  return t.v < 32u && ((CONCRETE_FLOAT_MASK >> t.v) & 1u);
}
static bool is_numeric(IpIndex t) {
  return t.v < 32u && ((NUMERIC_MASK >> t.v) & 1u);
}

static IpIndex unify_arith(IpIndex a, IpIndex b) {
  if (a.v == IP_NONE.v || b.v == IP_NONE.v)
    return IP_NONE;
  if (a.v == b.v)
    return a;
  if ((a.v == IP_COMPTIME_INT_TYPE.v && b.v == IP_COMPTIME_FLOAT_TYPE.v) ||
      (b.v == IP_COMPTIME_INT_TYPE.v && a.v == IP_COMPTIME_FLOAT_TYPE.v))
    return IP_COMPTIME_FLOAT_TYPE;
  if (a.v == IP_COMPTIME_INT_TYPE.v &&
      (is_concrete_int(b) || is_concrete_float(b)))
    return b;
  if (b.v == IP_COMPTIME_INT_TYPE.v &&
      (is_concrete_int(a) || is_concrete_float(a)))
    return a;
  if (a.v == IP_COMPTIME_FLOAT_TYPE.v && is_concrete_float(b))
    return b;
  if (b.v == IP_COMPTIME_FLOAT_TYPE.v && is_concrete_float(a))
    return a;
  return IP_NONE;
}

static const char *opkind_name(SyntaxKind k) {
  switch (k) {
  case SK_PLUS:
    return "+";
  case SK_MINUS:
    return "-";
  case SK_STAR:
    return "*";
  case SK_SLASH:
    return "/";
  case SK_PERCENT:
    return "%";
  case SK_STAR_STAR:
    return "**";
  case SK_EQ_EQ:
    return "==";
  case SK_BANG_EQ:
    return "!=";
  case SK_LT:
    return "<";
  case SK_LE:
    return "<=";
  case SK_GT:
    return ">";
  case SK_GE:
    return ">=";
  case SK_AMP_AMP:
    return "&&";
  case SK_PIPE_PIPE:
    return "||";
  case SK_AMP:
    return "&";
  case SK_PIPE:
    return "|";
  case SK_CARET:
    return "^";
  case SK_SHL:
    return "<<";
  case SK_SHR:
    return ">>";
  case SK_BANG:
    return "!";
  case SK_TILDE:
    return "~";
  case SK_QUESTION:
    return "?";
  default:
    return "?";
  }
}

static IpIndex type_from_lit_token(SyntaxKind tk) {
  switch (tk) {
  case SK_INT_LIT:
    return IP_COMPTIME_INT_TYPE;
  case SK_FLOAT_LIT:
    return IP_COMPTIME_FLOAT_TYPE;
  case SK_TRUE_KW:
  case SK_FALSE_KW:
    return IP_BOOL_TYPE;
  case SK_BYTE_LIT:
    return IP_U8_TYPE;
  case SK_STRING_LIT:
    return IP_STRING_SLICE_TYPE;
  case SK_NIL_KW:
    return IP_NIL_TYPE;
  default:
    return IP_NONE;
  }
}

static DiagAnchor span_of(const SemaCtx *ctx, SyntaxNode *node) {
  return diag_anchor_of_node((uint16_t)ctx->file_local.idx, node);
}
static StrId intern_tok(struct db *s, SyntaxToken *t) {
  if (!t)
    return (StrId){0};
  const char *txt = syntax_token_text(t);
  uint32_t len = syntax_token_text_range(t).length;
  return pool_intern(&s->strings, txt, len);
}

// ============================================================================
// Coercion (check_expr's table). v1 Zig variance; comptime→concrete accepted
// structurally (range-check is Phase-6 const_eval).
// ============================================================================

static bool can_coerce(struct db *s, IpIndex actual, IpIndex expected) {
  if (actual.v == IP_NONE.v || expected.v == IP_NONE.v)
    return true;
  if (actual.v == expected.v)
    return true;
  if (actual.v == IP_NORETURN_TYPE.v)
    return true;

  IpTag at = ip_tag(&s->intern, actual);
  IpTag et = ip_tag(&s->intern, expected);

  // Pointer / slice / many-ptr variance: drop mut (X → const X), same elem.
  if ((at == IP_TAG_PTR_TYPE || at == IP_TAG_PTR_CONST_TYPE) &&
      (et == IP_TAG_PTR_TYPE || et == IP_TAG_PTR_CONST_TYPE)) {
    IpKey ak = ip_key(&s->intern, actual), ek = ip_key(&s->intern, expected);
    if (ak.ptr_type.elem.v == ek.ptr_type.elem.v &&
        (at != IP_TAG_PTR_CONST_TYPE || et == IP_TAG_PTR_CONST_TYPE))
      return true;
  }
  if ((at == IP_TAG_SLICE_TYPE || at == IP_TAG_SLICE_CONST_TYPE) &&
      (et == IP_TAG_SLICE_TYPE || et == IP_TAG_SLICE_CONST_TYPE)) {
    IpKey ak = ip_key(&s->intern, actual), ek = ip_key(&s->intern, expected);
    if (ak.slice_type.elem.v == ek.slice_type.elem.v &&
        (at != IP_TAG_SLICE_CONST_TYPE || et == IP_TAG_SLICE_CONST_TYPE))
      return true;
  }
  if ((at == IP_TAG_MANY_PTR_TYPE || at == IP_TAG_MANY_PTR_CONST_TYPE) &&
      (et == IP_TAG_MANY_PTR_TYPE || et == IP_TAG_MANY_PTR_CONST_TYPE)) {
    IpKey ak = ip_key(&s->intern, actual), ek = ip_key(&s->intern, expected);
    if (ak.many_ptr_type.elem.v == ek.many_ptr_type.elem.v &&
        (at != IP_TAG_MANY_PTR_CONST_TYPE || et == IP_TAG_MANY_PTR_CONST_TYPE))
      return true;
  }
  // Array-ptr decay: ^[N]T → []T / [^]T (const flows).
  if (at == IP_TAG_PTR_TYPE || at == IP_TAG_PTR_CONST_TYPE) {
    IpKey ak = ip_key(&s->intern, actual);
    if (ip_tag(&s->intern, ak.ptr_type.elem) == IP_TAG_ARRAY_TYPE) {
      IpKey arrk = ip_key(&s->intern, ak.ptr_type.elem);
      bool a_const = (at == IP_TAG_PTR_CONST_TYPE);
      if (et == IP_TAG_SLICE_TYPE || et == IP_TAG_SLICE_CONST_TYPE) {
        IpKey ek = ip_key(&s->intern, expected);
        if (arrk.array_type.elem.v == ek.slice_type.elem.v &&
            (!a_const || et == IP_TAG_SLICE_CONST_TYPE))
          return true;
      }
      if (et == IP_TAG_MANY_PTR_TYPE || et == IP_TAG_MANY_PTR_CONST_TYPE) {
        IpKey ek = ip_key(&s->intern, expected);
        if (arrk.array_type.elem.v == ek.many_ptr_type.elem.v &&
            (!a_const || et == IP_TAG_MANY_PTR_CONST_TYPE))
          return true;
      }
    }
  }
  // nil → ?T / ^T / [^]T / []T
  if (actual.v == IP_NIL_TYPE.v &&
      (et == IP_TAG_OPTIONAL_TYPE || et == IP_TAG_PTR_TYPE ||
       et == IP_TAG_PTR_CONST_TYPE || et == IP_TAG_MANY_PTR_TYPE ||
       et == IP_TAG_MANY_PTR_CONST_TYPE || et == IP_TAG_SLICE_TYPE ||
       et == IP_TAG_SLICE_CONST_TYPE))
    return true;
  // Optional lift: T → ?T
  if (et == IP_TAG_OPTIONAL_TYPE) {
    IpKey ek = ip_key(&s->intern, expected);
    if (can_coerce(s, actual, ek.optional_type.elem))
      return true;
  }
  // Comptime numeric coercions.
  if (actual.v == IP_COMPTIME_INT_TYPE.v &&
      (is_concrete_int(expected) || is_concrete_float(expected) ||
       expected.v == IP_COMPTIME_FLOAT_TYPE.v))
    return true;
  if (actual.v == IP_COMPTIME_FLOAT_TYPE.v && is_concrete_float(expected))
    return true;
  return false;
}

static bool kind_is_discard_construct(SyntaxKind k) {
  return k == SK_CONST_DECL || k == SK_VAR_DECL || k == SK_RETURN_STMT ||
         k == SK_BREAK_STMT || k == SK_CONTINUE_STMT || k == SK_DEFER_STMT ||
         k == SK_LOOP_EXPR || k == SK_BLOCK_STMT || k == SK_IF_EXPR ||
         k == SK_SWITCH_EXPR || k == SK_ASSIGN_EXPR || k == SK_EXPR_STMT;
}

// ============================================================================
// Value-position identifier resolution.
//   1. local: db_body_scope_lookup → bind_site → the binding's type in the
//      ACTIVE node→type map (set when the bind / param was walked).
//   2. top-level: namespace_scopes.internal → resolve_ref → type_of_def.
//   3. miss → "undefined identifier" diag.
// ============================================================================

static IpIndex resolve_value_path(const SemaCtx *ctx, SyntaxNode *use_node,
                                  StrId name) {
  if (name.idx == 0)
    return IP_NONE;
  struct db *s = ctx->s;

  if (ctx->enclosing_fn.idx != DEF_ID_NONE.idx) {
    SyntaxNodePtr bind =
        db_body_scope_lookup(s, ctx->enclosing_fn, use_node, name);
    if (bind.kind != SYNTAX_KIND_NONE) {
      // Bound locally. Its type was pushed at the bind_site node when the
      // walk processed the binding (top-down: binds precede uses).
      if (ctx->types) {
        uint64_t h = syntax_node_ptr_hash(bind);
        void *v = hashmap_get(&ctx->types->types, h);
        if (v)
          return (IpIndex){.v = (uint32_t)(uintptr_t)v};
      }
      return IP_NONE; // bound but not yet typed (forward ref) — no diag
    }
  }

  NamespaceScopes sc = db_query_namespace_scopes(s, ctx->nsid);
  if (sc.internal.idx != SCOPE_ID_NONE.idx) {
    DefId target = db_query_resolve_ref(s, sc.internal, name);
    if (target.idx != DEF_ID_NONE.idx)
      return db_query_type_of_def(s, target);
  }
  db_emit(s, DIAG_ERROR, span_of(ctx, use_node), "undefined identifier '%S'",
          name);
  return IP_NONE;
}

// ============================================================================
// Arg-list collection (request-arena scratch; caller releases each node).
// ============================================================================

static SyntaxNode **collect_arg_nodes(struct db *s, SyntaxNode *arg_list,
                                      uint32_t *out_count) {
  *out_count = 0;
  if (!arg_list)
    return NULL;
  uint32_t total = syntax_node_num_children(arg_list);
  uint32_t n = 0;
  for (uint32_t i = 0; i < total; i++)
    if (green_node_child(syntax_node_green(arg_list), i).kind ==
        GREEN_ELEM_NODE)
      n++;
  if (n == 0)
    return NULL;
  SyntaxNode **out = arena_alloc(&s->request_arena, n * sizeof(SyntaxNode *));
  if (!out)
    return NULL;
  uint32_t k = 0;
  for (uint32_t i = 0; i < total && k < n; i++) {
    SyntaxElement el = syntax_node_child_or_token(arg_list, i);
    if (el.kind == SYNTAX_ELEM_NODE && el.node)
      out[k++] = el.node;
    else if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
      syntax_token_release(el.token);
  }
  *out_count = k;
  return out;
}
static void release_arg_nodes(SyntaxNode **args, uint32_t count) {
  for (uint32_t i = 0; i < count; i++)
    if (args[i])
      syntax_node_release(args[i]);
}

// ============================================================================
// Statement helpers (the completed-common-statements work).
// ============================================================================

// The annotation / RHS node of a let-bind wrapper (borrowed; release).
static void bind_parts(SyntaxNode *decl, SyntaxKind k, SyntaxNode **type_out,
                       SyntaxNode **val_out) {
  *type_out = NULL;
  *val_out = NULL;
  if (k == SK_CONST_DECL) {
    ConstDef c;
    if (ConstDef_cast(decl, &c)) {
      *type_out = ConstDef_type(&c);
      *val_out = ConstDef_value(&c);
    }
  } else {
    VarDef v;
    if (VarDef_cast(decl, &v)) {
      *type_out = VarDef_type(&v);
      *val_out = VarDef_value(&v);
    }
  }
}

// Type a let-bind statement: annotation wins (RHS then checked against it),
// else infer from RHS. The binding's type is pushed at the decl node (its
// bind_site) so local refs resolve through it. Returns the binding's type.
static IpIndex type_let_bind(const SemaCtx *ctx, SyntaxNode *decl,
                             SyntaxKind k) {
  SyntaxNode *type_node, *value_node;
  bind_parts(decl, k, &type_node, &value_node);
  IpIndex bt = IP_NONE;
  if (type_node) {
    bt = resolve_type_expr(ctx, type_node);
    if (value_node && bt.v != IP_NONE.v)
      (void)check_expr(ctx, value_node, bt);
  } else if (value_node) {
    bt = type_of_expr(ctx, value_node);
  }
  if (type_node)
    syntax_node_release(type_node);
  if (value_node)
    syntax_node_release(value_node);
  return bt;
}

// The if-condition: an if-let bind (cond is a let-decl) types its RHS, unwraps
// the optional, and pushes the element type at the cond node (its bind_site).
// A plain cond is checked against bool. Shared by type_of_expr + check_expr.
static void handle_if_cond(const SemaCtx *ctx, SyntaxNode *cond) {
  if (!cond)
    return;
  struct db *s = ctx->s;
  SyntaxKind ck = syntax_node_kind(cond);
  if (ck == SK_CONST_DECL || ck == SK_VAR_DECL) {
    SyntaxNode *type_node, *rhs;
    bind_parts(cond, ck, &type_node, &rhs);
    IpIndex rt = rhs ? type_of_expr(ctx, rhs) : IP_NONE;
    IpIndex elem = IP_NONE;
    if (rt.v != IP_NONE.v) {
      if (ip_tag(&s->intern, rt) == IP_TAG_OPTIONAL_TYPE)
        elem = ip_key(&s->intern, rt).optional_type.elem;
      else
        db_emit(s, DIAG_ERROR, span_of(ctx, cond),
                "if-let pattern requires optional type, got %T", rt);
    }
    node_type_builder_push(ctx, cond, elem); // bind_site = cond node
    if (type_node)
      syntax_node_release(type_node);
    if (rhs)
      syntax_node_release(rhs);
  } else {
    (void)check_expr(ctx, cond, IP_BOOL_TYPE);
  }
}

// A bare `_` wildcard pattern parses as SK_LITERAL_EXPR wrapping SK_UNDERSCORE.
static bool pattern_is_wildcard(SyntaxNode *p) {
  if (syntax_node_kind(p) != SK_LITERAL_EXPR)
    return false;
  // `_` parses as SK_LITERAL_EXPR wrapping the SK_UNDERSCORE token, but
  // SK_UNDERSCORE isn't in the TCF_LITERAL_TOKEN flag set that Literal_kind
  // consults — so probe the wrapper's children directly for the token.
  uint32_t n = syntax_node_num_children(p);
  for (uint32_t i = 0; i < n; i++) {
    SyntaxElement el = syntax_node_child_or_token(p, i);
    if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      bool is_us = (syntax_token_kind(el.token) == SK_UNDERSCORE);
      syntax_token_release(el.token);
      if (is_us)
        return true;
    } else if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      syntax_node_release(el.node);
    }
  }
  return false;
}

// switch (scrutinee) { pat | pat => body … } — shared by type_of_expr (synth,
// expected == IP_NONE) and check_expr (bidirectional, expected != NONE).
// Patterns are checked against the scrutinee type; arm bodies are checked
// against `expected` (check) or synthesized + unified (synth). Basic enum
// exhaustiveness: every variant covered or a `_` wildcard present.
static IpIndex infer_switch(const SemaCtx *ctx, SyntaxNode *node,
                            IpIndex expected) {
  struct db *s = ctx->s;
  SwitchExpr sw;
  if (!SwitchExpr_cast(node, &sw))
    return IP_NONE;
  bool check_mode = (expected.v != IP_NONE.v);

  SyntaxNode *scrutinee = SwitchExpr_scrutinee(&sw);
  IpIndex scrut = scrutinee ? type_of_expr(ctx, scrutinee) : IP_NONE;
  if (scrutinee)
    syntax_node_release(scrutinee);

  IpIndex result = expected;
  bool result_set = check_mode;
  bool wildcard = false;
  // Covered enum-variant names (for exhaustiveness). Dynamic so there is NO
  // silent cliff: vec_init doesn't allocate until the first matched variant.
  Vec covered;
  vec_init(&covered, sizeof(StrId));

  SyntaxNode *arms = SwitchExpr_arms(&sw);
  if (arms) {
    uint32_t n = syntax_node_num_children(arms);
    for (uint32_t i = 0; i < n; i++) {
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
      // Walk the arm's node children: every node but the LAST is a pattern;
      // the last is the body (handles `|`-alternation, which SwitchArm_body
      // can't).
      uint32_t an = syntax_node_num_children(arm);
      SyntaxNode *prev = NULL;
      for (uint32_t j = 0; j < an; j++) {
        SyntaxElement pel = syntax_node_child_or_token(arm, j);
        if (pel.kind == SYNTAX_ELEM_NODE && pel.node) {
          if (prev) { // prev is a pattern (a non-last node child)
            if (pattern_is_wildcard(prev)) {
              wildcard = true;
            } else {
              if (syntax_node_kind(prev) == SK_ENUM_REF_EXPR) {
                EnumRefExpr er;
                if (EnumRefExpr_cast(prev, &er)) {
                  SyntaxToken *vt = EnumRefExpr_variant(&er);
                  StrId vn = intern_tok(s, vt);
                  if (vt)
                    syntax_token_release(vt);
                  if (vn.idx)
                    vec_push(&covered, &vn);
                }
              }
              (void)check_expr(ctx, prev, scrut);
            }
            syntax_node_release(prev);
          }
          prev = pel.node;
        } else if (pel.kind == SYNTAX_ELEM_TOKEN && pel.token) {
          syntax_token_release(pel.token);
        }
      }
      if (prev) { // the body
        if (check_mode) {
          (void)check_expr(ctx, prev, expected);
        } else {
          IpIndex bt = type_of_expr(ctx, prev);
          if (!result_set) {
            result = bt;
            result_set = true;
          } else if (bt.v != IP_NONE.v && result.v != IP_NONE.v &&
                     bt.v != result.v) {
            IpIndex u = unify_arith(result, bt);
            if (u.v != IP_NONE.v)
              result = u;
            else
              db_emit(s, DIAG_ERROR, span_of(ctx, prev),
                      "switch arm has type %T, expected %T", bt, result);
          }
        }
        syntax_node_release(prev);
      }
      syntax_node_release(arm);
    }
    syntax_node_release(arms);
  }

  // Basic enum exhaustiveness: all variants covered, or a `_` wildcard.
  if (!wildcard && scrut.v != IP_NONE.v &&
      ip_tag(&s->intern, scrut) == IP_TAG_ENUM_TYPE) {
    DefId ed = {.idx = ip_key(&s->intern, scrut).enum_type.zir_node_id};
    (void)db_query_type_of_def(s, ed);
    uint32_t nv = 0;
    const EnumVariantEntry *vs = db_enum_variants(s, ed, &nv);
    const StrId *cov = (const StrId *)covered.data;
    for (uint32_t v = 0; v < nv; v++) {
      bool seen = false;
      for (size_t c = 0; c < covered.count; c++)
        if (cov[c].idx == vs[v].name.idx) {
          seen = true;
          break;
        }
      if (!seen)
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "non-exhaustive switch: missing variant '%S'", vs[v].name);
    }
  }

  vec_free(&covered);
  return check_mode ? expected : (result_set ? result : IP_VOID_TYPE);
}

// ============================================================================
// Typed-construction helpers (shared by type_of_expr_impl + check_expr).
//
// `resolve_product_target` resolves the TYPE prefix of an `SK_PRODUCT_EXPR`
// (`T{…}`), including the `[_]T{…}` inferred-size form — for that one shape,
// the size comes from the init-list count, not the type expression itself.
//
// `walk_init_list` is the single bidirectional aggregate checker: dispatch on
// the EXPECTED type's tag and check every `SK_INIT_FIELD` value against its
// declared field/element type. Loud diags on shape mismatches (named init
// against an array, positional against a struct, count vs declared size,
// non-aggregate target). No silent fallbacks.
// ============================================================================

// First non-INIT_LIST node child of an SK_PRODUCT_EXPR — the type prefix.
// We don't use ProductExpr_type because its is_type_node predicate misses the
// case where the prefix parses as a value-kind expression (e.g. `origin ::
// P{...}` at top-level — `P` lands as SK_REF_EXPR, not SK_REF_TYPE). Anonymous
// `.{...}` correctly returns NULL (the only node child is the SK_INIT_LIST).
// Returns +1 ref; caller releases.
static SyntaxNode *product_expr_prefix(SyntaxNode *prod) {
  uint32_t n = syntax_node_num_children(prod);
  for (uint32_t i = 0; i < n; i++) {
    SyntaxElement el = syntax_node_child_or_token(prod, i);
    if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      if (syntax_node_kind(el.node) != SK_INIT_LIST)
        return el.node; // +1 ref
      syntax_node_release(el.node);
    } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      syntax_token_release(el.token);
    }
  }
  return NULL;
}

static IpIndex resolve_product_target(const SemaCtx *ctx, SyntaxNode *pty,
                                      SyntaxNode *init_list) {
  struct db *s = ctx->s;
  if (!pty)
    return IP_NONE;
  // Inferred-size: `[_]T{…}` — the parser consumes `_` as a raw token
  // (parse_expr.c:980), so SK_ARRAY_TYPE has NO expression-node child for
  // size and ArrayType_size returns NULL. That's the in-band marker.
  if (syntax_node_kind(pty) == SK_ARRAY_TYPE) {
    ArrayType at;
    if (ArrayType_cast(pty, &at)) {
      SyntaxNode *size_node = ArrayType_size(&at);
      bool inferred = (size_node == NULL);
      if (size_node)
        syntax_node_release(size_node);
      if (inferred) {
        SyntaxNode *elem_node = ArrayType_element(&at);
        IpIndex elem = elem_node ? resolve_type_expr(ctx, elem_node) : IP_NONE;
        if (elem_node)
          syntax_node_release(elem_node);
        if (elem.v == IP_NONE.v)
          return IP_NONE;
        uint32_t count = 0;
        if (init_list) {
          uint32_t total = syntax_node_num_children(init_list);
          for (uint32_t i = 0; i < total; i++) {
            SyntaxElement el = syntax_node_child_or_token(init_list, i);
            if (el.kind == SYNTAX_ELEM_NODE && el.node) {
              if (syntax_node_kind(el.node) == SK_INIT_FIELD)
                count++;
              syntax_node_release(el.node);
            } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
              syntax_token_release(el.token);
            }
          }
        }
        IpKey key = {.kind = IPK_ARRAY_TYPE,
                     .array_type = {.elem = elem, .size = count}};
        return ip_get(&s->intern, key);
      }
    }
  }
  // Value-position type prefix: at top-level inferred binds (`origin ::
  // P{...}`) the parser leaves the name in value position, so `pty` parses
  // as SK_REF_EXPR (not SK_REF_TYPE). resolve_type_expr handles type-kind
  // nodes; for SK_REF_EXPR we do the equivalent name → DefId → type_of_def
  // lookup ourselves (same as the top-level resolve_ref fallback used by
  // db_query_node_type).
  if (syntax_node_kind(pty) == SK_REF_EXPR) {
    RefExpr r;
    if (RefExpr_cast(pty, &r)) {
      SyntaxToken *nt = RefExpr_name(&r);
      if (nt) {
        StrId name = pool_intern(&s->strings, syntax_token_text(nt),
                                 syntax_token_text_range(nt).length);
        syntax_token_release(nt);
        if (name.idx != 0) {
          ScopeId internal = db_query_namespace_scopes(s, ctx->nsid).internal;
          if (internal.idx != SCOPE_ID_NONE.idx) {
            DefId target = db_query_resolve_ref(s, internal, name);
            if (target.idx != 0)
              return db_query_type_of_def(s, target);
          }
        }
      }
    }
  }
  return resolve_type_expr(ctx, pty);
}

static bool walk_init_list(const SemaCtx *ctx, SyntaxNode *init_list,
                           IpIndex expected) {
  struct db *s = ctx->s;
  if (!init_list)
    return true; // empty literal — nothing to check
  if (!ip_index_is_valid(expected)) {
    // Best-effort type each value so the node-type map still gets entries;
    // the lack of context is already a real diag at the call site.
    uint32_t total = syntax_node_num_children(init_list);
    for (uint32_t i = 0; i < total; i++) {
      SyntaxElement el = syntax_node_child_or_token(init_list, i);
      if (el.kind == SYNTAX_ELEM_NODE && el.node) {
        if (syntax_node_kind(el.node) == SK_INIT_FIELD) {
          InitField ifld;
          if (InitField_cast(el.node, &ifld)) {
            SyntaxNode *fval = InitField_value(&ifld);
            if (fval) {
              (void)type_of_expr(ctx, fval);
              syntax_node_release(fval);
            }
          }
        }
        syntax_node_release(el.node);
      } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
        syntax_token_release(el.token);
      }
    }
    return false;
  }
  IpTag tag = ip_tag(&s->intern, expected);
  if (tag == IP_TAG_STRUCT_TYPE) {
    DefId d = {.idx = ip_key(&s->intern, expected).struct_type.zir_node_id};
    (void)db_query_type_of_def(s, d); // ensure fields populated + dep recorded
    bool ok = true;
    uint32_t total = syntax_node_num_children(init_list);
    for (uint32_t i = 0; i < total; i++) {
      SyntaxElement el = syntax_node_child_or_token(init_list, i);
      if (el.kind == SYNTAX_ELEM_NODE && el.node) {
        if (syntax_node_kind(el.node) == SK_INIT_FIELD) {
          InitField ifld;
          if (InitField_cast(el.node, &ifld)) {
            SyntaxToken *iname_tok = InitField_name(&ifld);
            SyntaxNode *fval = InitField_value(&ifld);
            if (!iname_tok) {
              db_emit(s, DIAG_ERROR, span_of(ctx, el.node),
                      "positional initializer not allowed in struct literal; "
                      "use '.field = value'");
              ok = false;
              if (fval)
                (void)type_of_expr(ctx, fval);
            } else {
              StrId fname =
                  pool_intern(&s->strings, syntax_token_text(iname_tok),
                              syntax_token_text_range(iname_tok).length);
              syntax_token_release(iname_tok);
              IpIndex field_ty = db_aggregate_field_type(s, d, fname);
              if (field_ty.v == IP_NONE.v) {
                db_emit(s, DIAG_ERROR, span_of(ctx, el.node),
                        "no field '%S' in %T", fname, expected);
                ok = false;
                if (fval)
                  (void)type_of_expr(ctx, fval);
              } else if (fval) {
                if (!check_expr(ctx, fval, field_ty))
                  ok = false;
              }
            }
            if (fval)
              syntax_node_release(fval);
          }
        }
        syntax_node_release(el.node);
      } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
        syntax_token_release(el.token);
      }
    }
    return ok;
  }
  if (tag == IP_TAG_ARRAY_TYPE) {
    IpKey k = ip_key(&s->intern, expected);
    IpIndex elem = k.array_type.elem;
    uint64_t declared_size = k.array_type.size;
    uint32_t count = 0;
    bool ok = true;
    uint32_t total = syntax_node_num_children(init_list);
    for (uint32_t i = 0; i < total; i++) {
      SyntaxElement el = syntax_node_child_or_token(init_list, i);
      if (el.kind == SYNTAX_ELEM_NODE && el.node) {
        if (syntax_node_kind(el.node) == SK_INIT_FIELD) {
          InitField ifld;
          if (InitField_cast(el.node, &ifld)) {
            SyntaxToken *iname_tok = InitField_name(&ifld);
            SyntaxNode *fval = InitField_value(&ifld);
            if (iname_tok) {
              db_emit(s, DIAG_ERROR, span_of(ctx, el.node),
                      "named initializer not allowed in array literal");
              ok = false;
              syntax_token_release(iname_tok);
            }
            if (fval) {
              if (!check_expr(ctx, fval, elem))
                ok = false;
              syntax_node_release(fval);
            }
            count++;
          }
        }
        syntax_node_release(el.node);
      } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
        syntax_token_release(el.token);
      }
    }
    if (declared_size != (uint64_t)count) {
      db_emit(s, DIAG_ERROR, span_of(ctx, init_list),
              "array literal has %d element(s), expected %d", (int32_t)count,
              (int32_t)declared_size);
      ok = false;
    }
    return ok;
  }
  db_emit(s, DIAG_ERROR, span_of(ctx, init_list),
          "%T is not constructible with '{...}'", expected);
  return false;
}

// ============================================================================
// type_of_expr — synthesize. Thin wrapper pushes the result into the builder.
// ============================================================================

static IpIndex type_of_expr_impl(const SemaCtx *ctx, SyntaxNode *node);

IpIndex type_of_expr(const SemaCtx *ctx, SyntaxNode *node) {
  if (!node)
    return IP_NONE;
  IpIndex result = type_of_expr_impl(ctx, node);
  node_type_builder_push(ctx, node, result);
  return result;
}

static IpIndex type_of_expr_impl(const SemaCtx *ctx, SyntaxNode *node) {
  if (!node)
    return IP_NONE;
  struct db *s = ctx->s;
  DefId enclosing_fn = ctx->enclosing_fn;
  SyntaxKind k = syntax_node_kind(node);

  switch (k) {

  case SK_LITERAL_EXPR: {
    Literal lit;
    if (!Literal_cast(node, &lit))
      return IP_NONE;
    return type_from_lit_token(Literal_kind(&lit));
  }

  case SK_REF_EXPR: {
    RefExpr r;
    if (!RefExpr_cast(node, &r))
      return IP_NONE;
    SyntaxToken *nt = RefExpr_name(&r);
    StrId name = intern_tok(s, nt);
    if (nt)
      syntax_token_release(nt);
    return resolve_value_path(ctx, node, name);
  }

  case SK_PATH_EXPR: {
    StrId last = {0};
    uint32_t count = syntax_node_num_children(node);
    for (uint32_t i = 0; i < count; i++) {
      GreenElement g = green_node_child(syntax_node_green(node), i);
      if (g.kind == GREEN_ELEM_TOKEN && green_token_kind(g.token) == SK_IDENT)
        last = pool_intern(&s->strings, green_token_text(g.token),
                           green_token_text_len(g.token));
    }
    return resolve_value_path(ctx, node, last);
  }

  case SK_PAREN_EXPR: {
    ParenExpr pe;
    if (!ParenExpr_cast(node, &pe))
      return IP_NONE;
    SyntaxNode *inner = ParenExpr_inner(&pe);
    IpIndex t = inner ? type_of_expr(ctx, inner) : IP_NONE;
    if (inner)
      syntax_node_release(inner);
    return t;
  }

  case SK_BIN_EXPR: {
    BinExpr be;
    if (!BinExpr_cast(node, &be))
      return IP_NONE;
    SyntaxKind opk = BinExpr_op_kind(&be);
    SyntaxNode *lhs = BinExpr_lhs(&be), *rhs = BinExpr_rhs(&be);
    IpIndex lt = lhs ? type_of_expr(ctx, lhs) : IP_NONE;
    IpIndex rt = rhs ? type_of_expr(ctx, rhs) : IP_NONE;
    if (lhs)
      syntax_node_release(lhs);
    if (rhs)
      syntax_node_release(rhs);
    if (lt.v == IP_NONE.v || rt.v == IP_NONE.v)
      return IP_NONE;

    bool is_arith =
        (opk == SK_PLUS || opk == SK_MINUS || opk == SK_STAR ||
         opk == SK_SLASH || opk == SK_PERCENT || opk == SK_STAR_STAR);
    bool is_compare = (opk == SK_EQ_EQ || opk == SK_BANG_EQ || opk == SK_LT ||
                       opk == SK_LE || opk == SK_GT || opk == SK_GE);
    bool is_logical = (opk == SK_AMP_AMP || opk == SK_PIPE_PIPE);
    bool is_bitop = (opk == SK_AMP || opk == SK_PIPE || opk == SK_CARET ||
                     opk == SK_SHL || opk == SK_SHR);

    if (is_arith) {
      // Many-pointer arithmetic: `[^]T + int`, `int + [^]T`, `[^]T - int`
      // all yield the many-pointer type. `[^]T - [^]T` yields usize
      // (element-count difference). `^T` and slice types are NOT
      // arithmetic — pointer arithmetic is many-pointer-specific.
      if (opk == SK_PLUS || opk == SK_MINUS) {
        IpTag lk = ip_tag(&s->intern, lt);
        IpTag rk = ip_tag(&s->intern, rt);
        bool l_mp = (lk == IP_TAG_MANY_PTR_TYPE ||
                     lk == IP_TAG_MANY_PTR_CONST_TYPE);
        bool r_mp = (rk == IP_TAG_MANY_PTR_TYPE ||
                     rk == IP_TAG_MANY_PTR_CONST_TYPE);
        bool l_int = (lt.v == IP_COMPTIME_INT_TYPE.v) || is_concrete_int(lt);
        bool r_int = (rt.v == IP_COMPTIME_INT_TYPE.v) || is_concrete_int(rt);
        if (l_mp && r_int)
          return lt; // [^]T + int → [^]T
        if (r_mp && l_int && opk == SK_PLUS)
          return rt; // int + [^]T → [^]T (commutative for +)
        if (l_mp && r_mp && opk == SK_MINUS) {
          // [^]T - [^]T → usize, requires same elem + constness.
          if (lt.v == rt.v)
            return IP_USIZE_TYPE;
          db_emit(s, DIAG_ERROR, span_of(ctx, node),
                  "pointer difference requires same many-pointer type, "
                  "got %T and %T", lt, rt);
          return IP_NONE;
        }
      }
      IpIndex u = unify_arith(lt, rt);
      if (u.v == IP_NONE.v || !is_numeric(u)) {
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "cannot apply '%s' to operands of type %T and %T",
                opkind_name(opk), lt, rt);
        return IP_NONE;
      }
      return u;
    }
    if (is_compare) {
      // Pointer equality: `[^]T == [^]T`, `^T == ^T` (same intern → same
      // .v). Same-type accept is sufficient; cross-type ptr comparison
      // isn't supported.
      IpTag lk = ip_tag(&s->intern, lt);
      bool both_ptr = (lt.v == rt.v) &&
                      (lk == IP_TAG_PTR_TYPE || lk == IP_TAG_PTR_CONST_TYPE ||
                       lk == IP_TAG_MANY_PTR_TYPE ||
                       lk == IP_TAG_MANY_PTR_CONST_TYPE);
      if (both_ptr && (opk == SK_EQ_EQ || opk == SK_BANG_EQ))
        return IP_BOOL_TYPE;
      if (lt.v != rt.v && unify_arith(lt, rt).v == IP_NONE.v) {
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "cannot apply '%s' to operands of type %T and %T",
                opkind_name(opk), lt, rt);
        return IP_NONE;
      }
      return IP_BOOL_TYPE;
    }
    if (is_logical) {
      if (lt.v != IP_BOOL_TYPE.v || rt.v != IP_BOOL_TYPE.v) {
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "logical '%s' requires bool operands, got %T", opkind_name(opk),
                (lt.v != IP_BOOL_TYPE.v) ? lt : rt);
        return IP_NONE;
      }
      return IP_BOOL_TYPE;
    }
    if (is_bitop) {
      IpIndex u = unify_arith(lt, rt);
      bool lt_ok = (lt.v == IP_COMPTIME_INT_TYPE.v) || is_concrete_int(lt);
      bool rt_ok = (rt.v == IP_COMPTIME_INT_TYPE.v) || is_concrete_int(rt);
      if (u.v == IP_NONE.v || !lt_ok || !rt_ok) {
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "bitwise '%s' requires integer operands, got %T and %T",
                opkind_name(opk), lt, rt);
        return IP_NONE;
      }
      return u;
    }
    if (opk == SK_ORELSE_KW) {
      // `a orelse b`: a must be optional (?T); result is T (b — possibly
      // `noreturn` via break — is the fallback coerced to T).
      if (ip_tag(&s->intern, lt) == IP_TAG_OPTIONAL_TYPE)
        return ip_key(&s->intern, lt).optional_type.elem;
      db_emit(s, DIAG_ERROR, span_of(ctx, node),
              "'orelse' requires an optional left operand, got %T", lt);
      return IP_NONE;
    }
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "binary operator '%s' not yet supported in type inference",
            opkind_name(opk));
    return IP_NONE;
  }

  case SK_PREFIX_EXPR: {
    PrefixExpr pe;
    if (!PrefixExpr_cast(node, &pe))
      return IP_NONE;
    SyntaxKind opk = PrefixExpr_op_kind(&pe);
    SyntaxNode *operand = PrefixExpr_operand(&pe);
    if (!operand)
      return IP_NONE;

    if (opk == SK_AMP) {
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
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "address-of '&' requires an l-value (variable, field, index, "
                "or deref)");
        syntax_node_release(operand);
        return IP_NONE;
      }
      IpIndex t = type_of_expr(ctx, operand);
      syntax_node_release(operand);
      if (t.v == IP_NONE.v)
        return IP_NONE;
      return ip_get(&s->intern,
                    (IpKey){.kind = IPK_PTR_TYPE,
                            .ptr_type = {.elem = t, .is_const = false}});
    }
    IpIndex t = type_of_expr(ctx, operand);
    syntax_node_release(operand);
    if (t.v == IP_NONE.v)
      return IP_NONE;
    if (opk == SK_MINUS) {
      if (!is_numeric(t)) {
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "unary '-' requires numeric operand, got %T", t);
        return IP_NONE;
      }
      return t;
    }
    if (opk == SK_TILDE) {
      if (t.v != IP_COMPTIME_INT_TYPE.v && !is_concrete_int(t)) {
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "unary '~' requires integer operand, got %T", t);
        return IP_NONE;
      }
      return t;
    }
    if (opk == SK_BANG) {
      if (t.v != IP_BOOL_TYPE.v) {
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "unary '!' requires bool, got %T", t);
        return IP_NONE;
      }
      return IP_BOOL_TYPE;
    }
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "prefix operator '%s' not yet supported in type inference",
            opkind_name(opk));
    return IP_NONE;
  }

  case SK_POSTFIX_EXPR: {
    PostfixExpr po;
    if (!PostfixExpr_cast(node, &po))
      return IP_NONE;
    SyntaxKind opk = PostfixExpr_op_kind(&po);
    SyntaxNode *operand = PostfixExpr_operand(&po);
    if (!operand)
      return IP_NONE;
    IpIndex t = type_of_expr(ctx, operand);
    syntax_node_release(operand);
    if (t.v == IP_NONE.v)
      return IP_NONE;
    if (opk == SK_CARET) {
      IpTag tag = ip_tag(&s->intern, t);
      if (tag != IP_TAG_PTR_TYPE && tag != IP_TAG_PTR_CONST_TYPE) {
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "cannot dereference non-pointer type %T", t);
        return IP_NONE;
      }
      return ip_key(&s->intern, t).ptr_type.elem;
    }
    if (opk == SK_QUESTION) {
      if (ip_tag(&s->intern, t) != IP_TAG_OPTIONAL_TYPE) {
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "'.?' requires optional type, got %T", t);
        return IP_NONE;
      }
      return ip_key(&s->intern, t).optional_type.elem;
    }
    return t; // ++ / -- : type as operand (statement-like)
  }

  case SK_CALL_EXPR: {
    CallExpr ce;
    if (!CallExpr_cast(node, &ce))
      return IP_NONE;
    SyntaxNode *callee = CallExpr_callee(&ce);
    SyntaxNode *arg_list = CallExpr_args(&ce);
    IpIndex callee_ty = callee ? type_of_expr(ctx, callee) : IP_NONE;
    if (callee_ty.v == IP_NONE.v) {
      if (callee)
        syntax_node_release(callee);
      if (arg_list)
        syntax_node_release(arg_list);
      return IP_NONE;
    }
    if (ip_tag(&s->intern, callee_ty) != IP_TAG_FN_TYPE) {
      db_emit(s, DIAG_ERROR, span_of(ctx, callee ? callee : node),
              "value of type %T is not callable", callee_ty);
      syntax_node_release(callee);
      if (arg_list)
        syntax_node_release(arg_list);
      return IP_NONE;
    }
    syntax_node_release(callee);
    IpKey key = ip_key(&s->intern, callee_ty);
    uint32_t n_args = 0;
    SyntaxNode **args = collect_arg_nodes(s, arg_list, &n_args);
    if (arg_list)
      syntax_node_release(arg_list);
    if (key.fn_type.n_params != n_args) {
      db_emit(s, DIAG_ERROR, span_of(ctx, node), "call expects %d args, got %d",
              (int32_t)key.fn_type.n_params, (int32_t)n_args);
      release_arg_nodes(args, n_args);
      return IP_NONE;
    }
    for (uint32_t i = 0; i < n_args; i++)
      (void)check_expr(ctx, args[i], key.fn_type.params[i]);
    release_arg_nodes(args, n_args);
    return key.fn_type.ret;
  }

  case SK_FIELD_EXPR: {
    FieldExpr fe;
    if (!FieldExpr_cast(node, &fe))
      return IP_NONE;
    SyntaxNode *base = FieldExpr_base(&fe);
    SyntaxToken *fname_tok = FieldExpr_field(&fe);
    StrId fname = intern_tok(s, fname_tok);
    if (fname_tok)
      syntax_token_release(fname_tok);
    IpIndex recv = base ? type_of_expr(ctx, base) : IP_NONE;
    DiagAnchor fspan = span_of(ctx, base ? base : node);
    if (base)
      syntax_node_release(base);
    if (recv.v == IP_NONE.v || fname.idx == 0)
      return IP_NONE;

    IpTag tag = ip_tag(&s->intern, recv);
    if (tag == IP_TAG_PTR_TYPE ||
        tag == IP_TAG_PTR_CONST_TYPE) { // auto-deref single ptr
      recv = ip_key(&s->intern, recv).ptr_type.elem;
      tag = ip_tag(&s->intern, recv);
    }
    switch (tag) {
    case IP_TAG_STRUCT_TYPE: {
      DefId d = {.idx = ip_key(&s->intern, recv).struct_type.zir_node_id};
      (void)db_query_type_of_def(s, d); // dep + ensure fields built
      IpIndex ft = db_aggregate_field_type(s, d, fname);
      if (ft.v == IP_NONE.v)
        db_emit(s, DIAG_ERROR, fspan, "no field '%S' in %T", fname, recv);
      return ft;
    }
    case IP_TAG_ENUM_TYPE: {
      DefId d = {.idx = ip_key(&s->intern, recv).enum_type.zir_node_id};
      (void)db_query_type_of_def(s, d);
      uint32_t nv = 0;
      const EnumVariantEntry *vs = db_enum_variants(s, d, &nv);
      for (uint32_t i = 0; i < nv; i++)
        if (vs[i].name.idx == fname.idx)
          return recv;
      db_emit(s, DIAG_ERROR, fspan, "no variant '%S' in %T", fname, recv);
      return IP_NONE;
    }
    case IP_TAG_SLICE_TYPE:
    case IP_TAG_SLICE_CONST_TYPE: {
      if (fname.idx == s->names.LEN.idx)
        return IP_USIZE_TYPE;
      if (fname.idx == s->names.PTR.idx) {
        IpKey sk = ip_key(&s->intern, recv);
        return ip_get(
            &s->intern,
            (IpKey){.kind = IPK_MANY_PTR_TYPE,
                    .many_ptr_type = {.elem = sk.slice_type.elem,
                                      .is_const =
                                          (tag == IP_TAG_SLICE_CONST_TYPE)}});
      }
      db_emit(s, DIAG_ERROR, fspan,
              "no field '%S' on slice (only '.len' and '.ptr')", fname);
      return IP_NONE;
    }
    case IP_TAG_ARRAY_TYPE:
      if (fname.idx == s->names.LEN.idx)
        return IP_USIZE_TYPE;
      db_emit(s, DIAG_ERROR, fspan, "no field '%S' on array (only '.len')",
              fname);
      return IP_NONE;
    case IP_TAG_NAMESPACE_TYPE: {
      NamespaceId ns = ip_key(&s->intern, recv).namespace_type.nsid;
      (void)db_query_namespace_type(s, ns); // dep + ensure members built
      uint32_t n = db_namespace_member_count(s, ns);
      for (uint32_t i = 0; i < n; i++) {
        DeclEntry m = db_namespace_member_at(s, ns, i);
        if (m.name.idx == fname.idx)
          return db_query_type_of_def(s, m.def);
      }
      db_emit(s, DIAG_ERROR, fspan, "no member '%S' in %T", fname, recv);
      return IP_NONE;
    }
    default:
      db_emit(s, DIAG_ERROR, fspan, "field access on non-aggregate type %T",
              recv);
      return IP_NONE;
    }
  }

  case SK_INDEX_EXPR: {
    IndexExpr ie;
    if (!IndexExpr_cast(node, &ie))
      return IP_NONE;
    SyntaxNode *base = IndexExpr_base(&ie), *idx = IndexExpr_index(&ie);
    IpIndex obj = base ? type_of_expr(ctx, base) : IP_NONE;
    if (idx)
      (void)type_of_expr(ctx, idx);
    DiagAnchor bspan = span_of(ctx, base ? base : node);
    if (base)
      syntax_node_release(base);
    if (idx)
      syntax_node_release(idx);
    if (obj.v == IP_NONE.v)
      return IP_NONE;
    IpKey key = ip_key(&s->intern, obj);
    switch (ip_tag(&s->intern, obj)) {
    case IP_TAG_ARRAY_TYPE:
      return key.array_type.elem;
    case IP_TAG_SLICE_TYPE:
    case IP_TAG_SLICE_CONST_TYPE:
      return key.slice_type.elem;
    case IP_TAG_MANY_PTR_TYPE:
    case IP_TAG_MANY_PTR_CONST_TYPE:
      return key.many_ptr_type.elem;
    default:
      db_emit(s, DIAG_ERROR, bspan, "value of type %T is not indexable", obj);
      return IP_NONE;
    }
  }

  case SK_SLICE_EXPR: {
    SliceExpr se;
    if (!SliceExpr_cast(node, &se))
      return IP_NONE;
    SyntaxNode *base = SliceExpr_base(&se), *lo = SliceExpr_lo(&se),
               *hi = SliceExpr_hi(&se);
    IpIndex obj = base ? type_of_expr(ctx, base) : IP_NONE;
    if (lo)
      (void)type_of_expr(ctx, lo);
    if (hi)
      (void)type_of_expr(ctx, hi);
    DiagAnchor bspan = span_of(ctx, base ? base : node);
    if (obj.v == IP_NONE.v) {
      if (base) syntax_node_release(base);
      if (lo) syntax_node_release(lo);
      if (hi) syntax_node_release(hi);
      return IP_NONE;
    }
    IpKey key = ip_key(&s->intern, obj);
    IpTag obj_tag = ip_tag(&s->intern, obj);
    IpIndex elem = IP_NONE;
    bool is_const = false;
    switch (obj_tag) {
    case IP_TAG_ARRAY_TYPE:
      elem = key.array_type.elem;
      break;
    case IP_TAG_SLICE_TYPE:
      elem = key.slice_type.elem;
      break;
    case IP_TAG_SLICE_CONST_TYPE:
      elem = key.slice_type.elem;
      is_const = true;
      break;
    case IP_TAG_MANY_PTR_TYPE:
      elem = key.many_ptr_type.elem;
      break;
    case IP_TAG_MANY_PTR_CONST_TYPE:
      elem = key.many_ptr_type.elem;
      is_const = true;
      break;
    default:
      db_emit(s, DIAG_ERROR, bspan, "value of type %T is not sliceable", obj);
      if (base) syntax_node_release(base);
      if (lo) syntax_node_release(lo);
      if (hi) syntax_node_release(hi);
      return IP_NONE;
    }

    // Const-bounded array slice: `arr[L..H]` with comptime int bounds on
    // an `[N]T` receiver returns `^[H-L]T` (matches Zig's `*[H-L]T`).
    // Open ends fold against N: `arr[L..]` → `^[N-L]T`, `arr[..H]` →
    // `^[H]T`. Range failures (mismatched bounds, out-of-range) fall
    // through to the slice path.
    if (obj_tag == IP_TAG_ARRAY_TYPE) {
      uint64_t arr_len = key.array_type.size;
      bool lo_lit = !lo || (Literal_cast(lo, &(Literal){0}) ? true : false);
      bool hi_lit = !hi || (Literal_cast(hi, &(Literal){0}) ? true : false);
      if (lo_lit && hi_lit) {
        uint64_t lo_v = 0, hi_v = arr_len;
        bool ok = true;
        if (lo) {
          Literal l;
          if (Literal_cast(lo, &l) && Literal_kind(&l) == SK_INT_LIT) {
            SyntaxToken *t = Literal_token(&l);
            if (t) {
              lo_v = parse_int_literal(t);
              syntax_token_release(t);
            } else ok = false;
          } else ok = false;
        }
        if (ok && hi) {
          Literal l;
          if (Literal_cast(hi, &l) && Literal_kind(&l) == SK_INT_LIT) {
            SyntaxToken *t = Literal_token(&l);
            if (t) {
              hi_v = parse_int_literal(t);
              syntax_token_release(t);
            } else ok = false;
          } else ok = false;
        }
        if (ok && lo_v <= hi_v && hi_v <= arr_len) {
          IpIndex inner = ip_get(
              &s->intern,
              (IpKey){.kind = IPK_ARRAY_TYPE,
                      .array_type = {.elem = elem, .size = hi_v - lo_v}});
          IpIndex result = ip_get(
              &s->intern,
              (IpKey){.kind = IPK_PTR_TYPE,
                      .ptr_type = {.elem = inner, .is_const = false}});
          if (base) syntax_node_release(base);
          if (lo) syntax_node_release(lo);
          if (hi) syntax_node_release(hi);
          return result;
        }
      }
    }

    if (base) syntax_node_release(base);
    if (lo) syntax_node_release(lo);
    if (hi) syntax_node_release(hi);
    return ip_get(&s->intern,
                  (IpKey){.kind = IPK_SLICE_TYPE,
                          .slice_type = {.elem = elem, .is_const = is_const}});
  }

  case SK_RETURN_STMT: {
    ReturnStmt rs;
    if (!ReturnStmt_cast(node, &rs))
      return IP_NORETURN_TYPE;
    SyntaxNode *val = ReturnStmt_value(&rs);
    if (enclosing_fn.idx != DEF_ID_NONE.idx && val) {
      const FnSignature *sig = db_query_fn_signature(s, enclosing_fn);
      IpIndex sigty = sig ? sig->type : IP_NONE;
      if (sigty.v != IP_NONE.v && ip_tag(&s->intern, sigty) == IP_TAG_FN_TYPE)
        (void)check_expr(ctx, val, ip_key(&s->intern, sigty).fn_type.ret);
    }
    if (val)
      syntax_node_release(val);
    return IP_NORETURN_TYPE;
  }

  case SK_BLOCK_STMT:
  case SK_BLOCK_EXPR: {
    BlockStmt bs = {.syntax = node}; // kind validated by the case label
    SyntaxNode *stmts = BlockStmt_stmts(&bs);
    if (!stmts)
      return IP_VOID_TYPE;
    uint32_t total = syntax_node_num_children(stmts);
    IpIndex last = IP_VOID_TYPE;
    bool saw = false;
    for (uint32_t i = 0; i < total; i++) {
      SyntaxElement el = syntax_node_child_or_token(stmts, i);
      if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
        if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
          syntax_token_release(el.token);
        continue;
      }
      last = type_of_expr(ctx, el.node);
      saw = true;
      syntax_node_release(el.node);
    }
    syntax_node_release(stmts);
    return saw ? last : IP_VOID_TYPE;
  }

  // --- let-bind statement (body_scopes no longer types these) -------------
  case SK_CONST_DECL:
  case SK_VAR_DECL:
    return type_let_bind(ctx, node, k);

  // --- if (statement or value): cond + branches, synth ---------------------
  case SK_IF_EXPR: {
    IfExpr ie;
    if (!IfExpr_cast(node, &ie))
      return IP_NONE;
    SyntaxNode *cond = IfExpr_condition(&ie);
    SyntaxNode *then_b = IfExpr_then_branch(&ie);
    SyntaxNode *else_b = IfExpr_else_branch(&ie);
    handle_if_cond(ctx, cond);
    IpIndex tt = then_b ? type_of_expr(ctx, then_b) : IP_VOID_TYPE;
    IpIndex et = else_b ? type_of_expr(ctx, else_b) : IP_VOID_TYPE;
    if (cond)
      syntax_node_release(cond);
    if (then_b)
      syntax_node_release(then_b);
    if (else_b)
      syntax_node_release(else_b);
    // Join the branches like switch arms do: unify_arith folds comptime↔
    // concrete and same-type (so `if c then 1 else x:i32` yields i32, not
    // void). A real mismatch or a missing else → void (if-as-statement).
    IpIndex u = else_b ? unify_arith(tt, et) : IP_NONE;
    return (u.v != IP_NONE.v) ? u : IP_VOID_TYPE;
  }

  // --- loop: header (init/cond/step) + body in the loop scope, yields void.
  // Iterate all node children in source order so for-style headers
  // (`loop (i := 0; i < N; i++) body`) type the init binding before
  // cond/step see it, and the body sees the binding via body_scope_lookup
  // (which body_scopes' matching walk_children has tagged into loop_scope).
  case SK_LOOP_EXPR: {
    uint32_t total = syntax_node_num_children(node);
    for (uint32_t i = 0; i < total; i++) {
      SyntaxElement el = syntax_node_child_or_token(node, i);
      if (el.kind == SYNTAX_ELEM_NODE && el.node) {
        (void)type_of_expr(ctx, el.node);
        syntax_node_release(el.node);
      } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
        syntax_token_release(el.token);
      }
    }
    return IP_VOID_TYPE;
  }

  // --- assignment: rhs must coerce to lhs; yields void ---------------------
  case SK_ASSIGN_EXPR: {
    AssignExpr ae;
    if (!AssignExpr_cast(node, &ae))
      return IP_NONE;
    SyntaxNode *lhs = AssignExpr_lhs(&ae), *rhs = AssignExpr_rhs(&ae);
    IpIndex lt = lhs ? type_of_expr(ctx, lhs) : IP_NONE;
    if (rhs)
      (void)check_expr(ctx, rhs, lt);
    if (lhs)
      syntax_node_release(lhs);
    if (rhs)
      syntax_node_release(rhs);
    return IP_VOID_TYPE;
  }

  // --- defer / expr-statement: recurse the inner expr ----------------------
  case SK_DEFER_STMT:
  case SK_EXPR_STMT: {
    uint32_t total = syntax_node_num_children(node);
    IpIndex t = IP_VOID_TYPE;
    for (uint32_t i = 0; i < total; i++) {
      SyntaxElement el = syntax_node_child_or_token(node, i);
      if (el.kind == SYNTAX_ELEM_NODE && el.node) {
        t = type_of_expr(ctx, el.node);
        syntax_node_release(el.node);
      } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
        syntax_token_release(el.token);
    }
    return (k == SK_DEFER_STMT) ? IP_VOID_TYPE : t;
  }

  // break / continue transfer control — no value (label child not typed).
  case SK_BREAK_STMT:
  case SK_CONTINUE_STMT:
    return IP_NORETURN_TYPE;

  // --- switch (synth) ------------------------------------------------------
  case SK_SWITCH_EXPR:
    return infer_switch(ctx, node, IP_NONE);

  // --- nested lambda: signature only (body inference deferred — body_scopes
  //     doesn't isolate nested-lambda param scopes). -------------------------
  case SK_LAMBDA_EXPR: {
    LambdaExpr lam;
    if (!LambdaExpr_cast(node, &lam))
      return IP_NONE;
    SyntaxNode *params = LambdaExpr_params(&lam);
    SyntaxNode *ret = LambdaExpr_return_type(&lam);
    IpIndex t = build_fn_type(ctx, ret, params);
    if (params)
      syntax_node_release(params);
    if (ret)
      syntax_node_release(ret);
    return t;
  }

  // @builtin(...) — name lookup → sealed-switch dispatch in builtins.c.
  // The handler runs inside this infer_body frame so any dep on imported
  // namespaces (@import → db_query_namespace_type) records here for free.
  case SK_BUILTIN_EXPR: {
    BuiltinExpr be;
    if (!BuiltinExpr_cast(node, &be))
      return IP_NONE;
    SyntaxToken *name_tok = BuiltinExpr_name(&be);
    SyntaxNode *arg_list = BuiltinExpr_args(&be);
    StrId name = intern_tok(s, name_tok);
    DiagAnchor anchor = span_of(ctx, node);
    if (name_tok)
      syntax_token_release(name_tok);

    BuiltinKind k = db_builtin_kind_of(s, name);
    if (k == BUILTIN_KIND_UNKNOWN) {
      db_emit(s, DIAG_ERROR, anchor, "unknown builtin @%S", name);
      if (arg_list)
        syntax_node_release(arg_list);
      return IP_NONE;
    }
    uint32_t n_args = 0;
    SyntaxNode **args = collect_arg_nodes(s, arg_list, &n_args);
    IpIndex result = IP_NONE;

    // @intCast(T, v) — arg-0 is a type expression (resolve_type_expr),
    // arg-1 is a value (type_of_expr). Result type IS arg-0's type.
    // Handled here because builtins.c dispatcher only sees raw nodes
    // + lacks SemaCtx access for resolve_type_expr.
    if (k == BUILTIN_INTCAST) {
      if (n_args >= 2 && args[0] && args[1]) {
        IpIndex target = resolve_type_expr(ctx, args[0]);
        (void)type_of_expr(ctx, args[1]); // record refs + push node-type
        result = target;
      } else {
        db_emit(s, DIAG_ERROR, anchor,
                "@intCast expects 2 arguments (target type, value)");
      }
    } else {
      // Pre-type each arg only when the metadata says it's a value
      // expression. TYPE-position args (@sizeOf, @alignOf, @typeName,
      // @import) skip — type_of_expr on a SK_ARRAY_TYPE / SK_PTR_TYPE
      // hits "kind not yet implemented in type inference". This is
      // also why @sizeOf(MyType)-style refs don't currently record
      // their TYPE_OF_DECL dep; tracked as a follow-up (the right fix
      // is the handler calling resolve_type_expr, which needs SemaCtx
      // plumbing through dispatch).
      const BuiltinMeta *m = db_builtin_meta(k);
      if (m && m->evaluates_args) {
        for (uint32_t i = 0; i < n_args; i++)
          (void)type_of_expr(ctx, args[i]);
      }
      result = db_dispatch_builtin(s, ctx->nsid, k, args, n_args, anchor);
    }
    release_arg_nodes(args, n_args);
    if (arg_list)
      syntax_node_release(arg_list);
    return result;
  }

  // T{...} / .{...} — typed construction. Synth requires an explicit type
  // prefix (`pty`); anonymous `.{...}` here is a real error (no context).
  // The bidirectional checking lives in walk_init_list, called from BOTH this
  // case and check_expr's SK_PRODUCT_EXPR case (which is allowed to use the
  // expected type as the target when `pty` is absent).
  case SK_PRODUCT_EXPR: {
    ProductExpr pe;
    if (!ProductExpr_cast(node, &pe))
      return IP_NONE;
    SyntaxNode *pty = product_expr_prefix(node);
    SyntaxNode *init_list = ProductExpr_init(&pe);
    IpIndex target = IP_NONE;
    if (pty) {
      target = resolve_product_target(ctx, pty, init_list);
      syntax_node_release(pty);
    } else {
      db_emit(s, DIAG_ERROR, span_of(ctx, node),
              "anonymous typed construction '.{...}' requires a target type "
              "from context");
    }
    (void)walk_init_list(ctx, init_list, target);
    if (init_list)
      syntax_node_release(init_list);
    return target;
  }
  // Standalone aggregate literal — checkable only, no target here. Loud diag;
  // not a silent fallback. SK_INIT_FIELD gets no case (only reachable inside
  // walk_init_list; if it ever hits the default, that IS the bug surfacing).
  case SK_INIT_LIST:
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "aggregate literal '{...}' needs a target type; wrap in "
            "'Type{...}' or assign to a typed binding");
    return IP_NONE;

  default:
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "expression kind %d not yet implemented in type inference", (int)k);
    return IP_NONE;
  }
}

// ============================================================================
// check_expr — bidirectional type checking.
//
// CONTRACT (principled bidirectional, Pierce / Dunfield style):
//
//   check_expr(e, τ) has two modes, in priority order:
//
//   1. CHECKABLE kinds (introduction forms whose type comes from context —
//      aggregate literals, if/switch with branch-typed result, blocks with a
//      checked tail, lambdas-against-fn-type, etc.) have an EXPLICIT handler
//      in the `if (expected != IP_NONE)` block below. The handler is
//      COMPLETE — it covers every shape of the kind and propagates `τ` into
//      every subterm that benefits from it.
//
//   2. SYNTHESIZABLE kinds (terms whose type is fully determined by themselves
//      — refs, literals, calls, field/index access, prefix/postfix, lambdas-
//      with-annotations, etc.) fall through to the tail at the bottom:
//      `actual = type_of_expr(e); can_coerce(actual, τ)`. That is the
//      principled bidirectional MODE-SWITCH (subsumption) rule, NOT a
//      catch-all. The fallback is honest precisely because every kind that
//      reaches it is genuinely synthesizable.
//
// IF YOU ADD A NEW KIND THAT NEEDS `expected` FOR CORRECTNESS, add an
// explicit checkable handler in the block below — never extend the
// synth-then-coerce tail with another `if (k == ...)` after the fact, and
// never paper over a missing rule with a silent `IP_NONE`. Aggregate-literal
// checking is centralized in `walk_init_list`; reuse it.
//
// Standalone use of an INHERENTLY-CHECKABLE kind in `type_of_expr` (where no
// context exists) emits a real diagnostic ("needs a target type from context")
// — see the `SK_INIT_LIST` arm in `type_of_expr_impl`.
// ============================================================================

bool check_expr(const SemaCtx *ctx, SyntaxNode *node, IpIndex expected) {
  if (!node)
    return true;
  struct db *s = ctx->s;
  SyntaxKind k = syntax_node_kind(node);

  if (expected.v != IP_NONE.v) {

    // Bidirectional `&x` against `^T` / `^const T`: propagate the
    // pointee type into the operand. Without this, `take_addr :: fn() ^i32\n
    // x := 5\n &x` synthesizes `&x` as `^comptime_int` and can_coerce
    // against `^i32` fails. With it, the operand `x` is checked against
    // `i32` — comptime_int restamps cleanly.
    if (k == SK_PREFIX_EXPR) {
      IpTag et = ip_tag(&s->intern, expected);
      if (et == IP_TAG_PTR_TYPE || et == IP_TAG_PTR_CONST_TYPE) {
        PrefixExpr pe;
        if (PrefixExpr_cast(node, &pe) &&
            PrefixExpr_op_kind(&pe) == SK_AMP) {
          SyntaxNode *operand = PrefixExpr_operand(&pe);
          if (operand) {
            IpIndex elem = ip_key(&s->intern, expected).ptr_type.elem;
            bool ok = check_expr(ctx, operand, elem);
            syntax_node_release(operand);
            return ok;
          }
        }
      }
    }

    if (k == SK_ENUM_REF_EXPR &&
        ip_tag(&s->intern, expected) == IP_TAG_ENUM_TYPE) {
      EnumRefExpr er;
      if (EnumRefExpr_cast(node, &er)) {
        SyntaxToken *vtok = EnumRefExpr_variant(&er);
        StrId vname = intern_tok(s, vtok);
        if (vtok)
          syntax_token_release(vtok);
        DefId d = {.idx = ip_key(&s->intern, expected).enum_type.zir_node_id};
        (void)db_query_type_of_def(s, d);
        uint32_t nv = 0;
        const EnumVariantEntry *vs = db_enum_variants(s, d, &nv);
        for (uint32_t i = 0; i < nv; i++)
          if (vs[i].name.idx == vname.idx)
            return true;
        db_emit(s, DIAG_ERROR, span_of(ctx, node), "no such variant in %T",
                expected);
        return false;
      }
    }

    // SK_PRODUCT_EXPR (T{...} / .{...}) — fully bidirectional.
    // Target type comes from the explicit `pty` if present, else from
    // `expected` (anonymous form). walk_init_list does the per-field
    // checking for both struct and array targets. Final subsumption
    // check verifies `target` fits `expected`.
    if (k == SK_PRODUCT_EXPR) {
      ProductExpr pe;
      if (ProductExpr_cast(node, &pe)) {
        SyntaxNode *pty = product_expr_prefix(node);
        SyntaxNode *init_list = ProductExpr_init(&pe);
        IpIndex target = expected;
        if (pty) {
          target = resolve_product_target(ctx, pty, init_list);
          syntax_node_release(pty);
        }
        bool ok = walk_init_list(ctx, init_list, target);
        if (init_list)
          syntax_node_release(init_list);
        // Subsumption: the constructed value must fit `expected`.
        if (ok && ip_index_is_valid(target) && target.v != expected.v &&
            !can_coerce(s, target, expected)) {
          db_emit(s, DIAG_ERROR, span_of(ctx, node), "expected %T, got %T",
                  expected, target);
          ok = false;
        }
        if (ip_index_is_valid(target))
          node_type_builder_push(ctx, node, target);
        return ok;
      }
    }

    if (k == SK_SWITCH_EXPR) {
      (void)infer_switch(ctx, node,
                         expected); // checks each arm body vs expected
      return true;
    }

    if (k == SK_BLOCK_STMT || k == SK_BLOCK_EXPR) {
      BlockStmt bs = {.syntax = node}; // kind validated above
      {
        SyntaxNode *stmts = BlockStmt_stmts(&bs);
        if (!stmts) {
          if (!can_coerce(s, IP_VOID_TYPE, expected)) {
            db_emit(s, DIAG_ERROR, span_of(ctx, node),
                    "empty block returns void; expected %T", expected);
            return false;
          }
          return true;
        }
        uint32_t total = syntax_node_num_children(stmts);
        uint32_t node_count = 0;
        for (uint32_t i = 0; i < total; i++) {
          SyntaxElement el = syntax_node_child_or_token(stmts, i);
          if (el.kind == SYNTAX_ELEM_NODE && el.node) {
            node_count++;
            syntax_node_release(el.node);
          } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
            syntax_token_release(el.token);
        }
        if (node_count == 0) {
          syntax_node_release(stmts);
          if (!can_coerce(s, IP_VOID_TYPE, expected)) {
            db_emit(s, DIAG_ERROR, span_of(ctx, node),
                    "empty block returns void; expected %T", expected);
            return false;
          }
          return true;
        }
        bool ok = true;
        uint32_t seen = 0;
        for (uint32_t i = 0; i < total; i++) {
          SyntaxElement el = syntax_node_child_or_token(stmts, i);
          if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
            if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
              syntax_token_release(el.token);
            continue;
          }
          SyntaxNode *stmt = el.node;
          if (seen == node_count - 1) {
            if (!check_expr(ctx, stmt, expected))
              ok = false;
          } else {
            IpIndex t = type_of_expr(ctx, stmt);
            if (t.v != IP_NONE.v && t.v != IP_VOID_TYPE.v &&
                t.v != IP_NORETURN_TYPE.v &&
                !kind_is_discard_construct(syntax_node_kind(stmt)))
              db_emit(s, DIAG_WARNING, span_of(ctx, stmt),
                      "unused value of type %T", t);
          }
          seen++;
          syntax_node_release(stmt);
        }
        syntax_node_release(stmts);
        return ok;
      }
    }

    if (k == SK_IF_EXPR) {
      IfExpr ie;
      if (IfExpr_cast(node, &ie)) {
        SyntaxNode *cond = IfExpr_condition(&ie);
        SyntaxNode *then_b = IfExpr_then_branch(&ie);
        SyntaxNode *else_b = IfExpr_else_branch(&ie);
        bool ok = true;
        handle_if_cond(ctx, cond);
        if (then_b && !check_expr(ctx, then_b, expected))
          ok = false;
        if (else_b && !check_expr(ctx, else_b, expected))
          ok = false;
        if (cond)
          syntax_node_release(cond);
        if (then_b)
          syntax_node_release(then_b);
        if (else_b)
          syntax_node_release(else_b);
        return ok;
      }
    }

    // Arith/bitwise binop — propagate expected to both operands. The
    // propagation is only correct when the operator's natural OPERAND
    // type equals its RESULT type. For `+`/`-` this is ambiguous —
    // could be int-arith (operand types == result type, propagate OK)
    // or pointer-arith (`[^]T + int → [^]T`, `[^]T - [^]T → usize`;
    // operands DO NOT match result, propagate gives wrong errors).
    // Peek-synth the LHS to decide; the second synth pass in the tail
    // is a cached hit. Forward-compatible with arbitrary-bit ints —
    // see plan's "Int architecture" section.
    if (k == SK_BIN_EXPR && is_numeric(expected)) {
      BinExpr be;
      if (BinExpr_cast(node, &be)) {
        SyntaxKind opk = BinExpr_op_kind(&be);
        bool propagate_eligible =
            (opk == SK_PLUS || opk == SK_MINUS || opk == SK_STAR ||
             opk == SK_SLASH || opk == SK_PERCENT || opk == SK_STAR_STAR ||
             opk == SK_AMP || opk == SK_PIPE || opk == SK_CARET);
        if (propagate_eligible) {
          SyntaxNode *lhs = BinExpr_lhs(&be), *rhs = BinExpr_rhs(&be);
          IpIndex lhs_synth = lhs ? type_of_expr(ctx, lhs) : IP_NONE;
          if (is_numeric(lhs_synth)) {
            bool ok = true;
            if (lhs && !check_expr(ctx, lhs, expected))
              ok = false;
            if (rhs && !check_expr(ctx, rhs, expected))
              ok = false;
            if (lhs)
              syntax_node_release(lhs);
            if (rhs)
              syntax_node_release(rhs);
            node_type_builder_push(ctx, node, expected);
            return ok;
          }
          // LHS is non-numeric (pointer/struct/etc.) — fall through to
          // the synth-then-coerce tail (handles ptr-arith correctly).
          if (lhs)
            syntax_node_release(lhs);
          if (rhs)
            syntax_node_release(rhs);
        }
      }
    }

    // Comptime-numeric leaves — re-stamp the contextual concrete type.
    if (k == SK_LITERAL_EXPR || k == SK_REF_EXPR || k == SK_PATH_EXPR) {
      IpIndex actual = type_of_expr(ctx, node);
      bool actual_comptime = (actual.v == IP_COMPTIME_INT_TYPE.v ||
                              actual.v == IP_COMPTIME_FLOAT_TYPE.v);
      bool expected_comptime = (expected.v == IP_COMPTIME_INT_TYPE.v ||
                                expected.v == IP_COMPTIME_FLOAT_TYPE.v);
      if (actual_comptime && !expected_comptime &&
          can_coerce(s, actual, expected)) {
        node_type_builder_push(ctx, node, expected);
        return true;
      }
      if (can_coerce(s, actual, expected))
        return true;
      db_emit(s, DIAG_ERROR, span_of(ctx, node), "expected %T, got %T",
              expected, actual);
      return false;
    }
  }

  // Synth-then-compare.
  IpIndex actual = type_of_expr(ctx, node);
  if (expected.v == IP_NONE.v)
    return actual.v != IP_NONE.v;
  if (can_coerce(s, actual, expected))
    return true;
  db_emit(s, DIAG_ERROR, span_of(ctx, node), "expected %T, got %T", expected,
          actual);
  return false;
}

// ============================================================================
// INFER_BODY query — type a fn body against its declared return type.
// ============================================================================

NodeTypesRange db_query_infer_body(db_query_ctx *ctx, DefId def) {
  struct db *s = (struct db *)ctx;
  NodeTypesRange empty = {0};
  // INFER_BODY is KIND_FUNCTION-only at the routing layer (db_engine_route_slot
  // returns false for non-fns → db_query_begin would assert). Refuse non-fns
  // BEFORE the guard so the query is TOTAL: a non-fn caller gets an empty
  // result instead of a hard abort. (No memoization needed — a non-fn has no
  // body; nothing depends on infer_body(non-fn).)
  if (db_def_kind(s, def) != KIND_FUNCTION)
    return empty;
  DB_QUERY_GUARD(ctx, QUERY_INFER_BODY, (uint64_t)def.idx,
                 /* on_cached */ infer_body_read(s, def),
                 /* on_cycle  */ empty,
                 /* on_error  */ empty);

  StrId name = *(StrId *)vec_get(&s->defs.names, def.idx);
  NamespaceId nsid = *(NamespaceId *)vec_get(&s->defs.parent_modules, def.idx);

  const FnSignature *sig =
      db_query_fn_signature(ctx, def);  // dep: declared return
  (void)db_query_body_scopes(ctx, def); // dep: scope structure
  IpIndex sigty = sig ? sig->type : IP_NONE;

  TopLevelEntry e =
      db_query_top_level_entry(ctx, nsid, name); // CONTENT firewall
  SyntaxTree *tree = NULL;
  SyntaxNode *lambda_node = NULL;
  if (e.node_ptr.kind != SYNTAX_KIND_NONE) {
    uint32_t local = file_id_local(e.file);
    struct GreenNode *groot =
        *(struct GreenNode **)vec_get(&s->files.green_roots, local);
    if (groot) {
      tree = syntax_tree_new(groot);
      SyntaxNode *rroot = syntax_tree_root(tree);
      SyntaxNode *wrapper = syntax_node_ptr_resolve(e.node_ptr, rroot);
      syntax_node_release(rroot);
      if (wrapper) {
        SyntaxNode *val = NULL;
        SyntaxKind wk = syntax_node_kind(wrapper);
        if (wk == SK_CONST_DECL) {
          ConstDef cd;
          if (ConstDef_cast(wrapper, &cd))
            val = ConstDef_value(&cd);
        } else if (wk == SK_VAR_DECL) {
          VarDef vd;
          if (VarDef_cast(wrapper, &vd))
            val = VarDef_value(&vd);
        }
        if (val) {
          if (syntax_node_kind(val) == SK_LAMBDA_EXPR)
            lambda_node = val;
          else
            syntax_node_release(val);
        }
        syntax_node_release(wrapper);
      }
    }
  }

  Fingerprint fp = FINGERPRINT_NONE;
  if (lambda_node) {
    LambdaExpr lam;
    if (LambdaExpr_cast(lambda_node, &lam)) {
      SyntaxNode *params = LambdaExpr_params(&lam);
      SyntaxNode *body = LambdaExpr_body(&lam);
      NodeTypeBuilder b;
      node_type_builder_begin(s, &b, e.file);
      SemaCtx walk = {.s = s,
                      .file_green_root = NULL,
                      .nsid = nsid,
                      .enclosing_fn = def,
                      .file_local = e.file,
                      .types = &b};
      bool sig_is_fn =
          (sigty.v != IP_NONE.v && ip_tag(&s->intern, sigty) == IP_TAG_FN_TYPE);

      // Type the params: push each Param's signature type into the node→
      // type map keyed by the Param node (its bind_site), so body refs to
      // params resolve via db_body_scope_lookup → bind_site → this map.
      if (params && sig_is_fn) {
        IpKey fk = ip_key(&s->intern, sigty);
        uint32_t total = syntax_node_num_children(params);
        size_t pi = 0;
        for (uint32_t i = 0; i < total && pi < fk.fn_type.n_params; i++) {
          SyntaxElement el = syntax_node_child_or_token(params, i);
          if (el.kind == SYNTAX_ELEM_NODE && el.node) {
            if (syntax_node_kind(el.node) == SK_PARAM)
              node_type_builder_push(&walk, el.node, fk.fn_type.params[pi++]);
            syntax_node_release(el.node);
          } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
            syntax_token_release(el.token);
          }
        }
      }
      if (params)
        syntax_node_release(params);

      IpIndex expected_ret =
          sig_is_fn ? ip_key(&s->intern, sigty).fn_type.ret : IP_NONE;
      if (body) {
        (void)check_expr(&walk, body, expected_ret);
        syntax_node_release(body);
      }
      NodeTypesRange range = node_type_builder_end(&b, &fp);
      infer_body_write(s, def, range); // frees prior map
    }
    syntax_node_release(lambda_node);
  } else {
    infer_body_write(s, def, empty);
  }
  if (tree)
    syntax_tree_free(tree);

  db_query_succeed(ctx, QUERY_INFER_BODY, (uint64_t)def.idx, fp);
  return infer_body_read(s, def);
}
