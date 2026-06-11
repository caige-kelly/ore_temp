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
#include "capability.h"     // db_read_file_ast, db_get_def_*_untracked
#include "const_eval.h"     // B6: db_const_eval / ConstValue for comptime if/switch
#include "engine.h"
#include "engine_internal.h"
#include "result_columns.h" // db.h, ids.h, intern_pool.h, syntax.h
#include "type_layer.h"
#include "builtins.h"       // D3.2: SK_BUILTIN_EXPR dispatch

#include "../diag/diag.h" // db_emit, diag_anchor_of_node, DIAG_*
#include "coerce.h"      // Coercion / coerce / coerce_or_diag + predicates

#include "../../ast/ast_decl.h"
#include "../../ast/ast_expr.h"
#include "../../ast/ast_stmt.h"
#include "../../ast/ast_type.h" // ArrayType_cast/_size/_element (D2.6 inferred-size)
#include "../../support/data_structure/arena.h"
#include "../../support/data_structure/hashmap.h"
#include "../../support/data_structure/stringpool.h"
#include "../../syntax/syntax_kind.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h> // qsort (switch int-range coverage)


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

// --- Slice 5B: Labeled-block result-type tracking ----------------------------
// A linked-list frame pushed on `SemaCtx.label_scope` when typing a labeled
// block; popped on exit. `break :label v` inside the block walks the chain
// for a matching name and peer-unifies `v`'s type into `result_accum`. The
// block's final type comes from `result_accum`.
//
// `result_accum` starts as IP_NONE meaning "no break-with-value site has
// contributed yet". The first contribution sets it. Subsequent ones use
// `unify_arith` to fold into a common type (Zig-style peer-type resolution).
struct LabelFrame {
  StrId             name;
  IpIndex           result_accum;
  bool              is_loop;       // loop frame (vs labeled block) — an
                                   // unlabeled `break v` targets the nearest one
  struct LabelFrame *parent;
};

// Extract the label name (StrId) from a node's first SK_LABEL token child.
// Returns {0} if no label is present. Strips the leading ':' from the token
// text — `:blk` interns as `blk`.
static StrId extract_label_name(struct db *s, SyntaxNode *node) {
  if (!node)
    return (StrId){0};
  SyntaxToken *tok = ast_first_token(node, SK_LABEL);
  if (!tok)
    return (StrId){0};
  const char *txt = syntax_token_text(tok);
  uint32_t len = syntax_token_text_range(tok).length;
  syntax_token_release(tok);
  // Token text is `:name` — skip the leading ':'.
  if (len < 2 || txt[0] != ':')
    return (StrId){0};
  return pool_intern(&s->strings, txt + 1, len - 1);
}

// Walk SemaCtx's label-scope chain for a matching name. Returns NULL if no
// frame matches.
static struct LabelFrame *find_label_frame(const SemaCtx *ctx, StrId name) {
  if (name.idx == 0)
    return NULL;
  for (struct LabelFrame *f = ctx->label_scope; f != NULL; f = f->parent) {
    if (f->name.idx == name.idx)
      return f;
  }
  return NULL;
}

// Walk the chain for the nearest loop frame — the target of an unlabeled
// `break v` (Zig: a bare break exits the innermost loop).
static struct LabelFrame *find_innermost_loop_frame(const SemaCtx *ctx) {
  for (struct LabelFrame *f = ctx->label_scope; f != NULL; f = f->parent) {
    if (f->is_loop)
      return f;
  }
  return NULL;
}

// Locate the value expression child of a SK_BREAK_STMT, if any. Skips the
// `break` keyword token, the optional SK_LABEL token, and trivia; returns
// the first remaining expression node (caller owns +1 ref) or NULL.
static SyntaxNode *break_value_expr(SyntaxNode *break_node) {
  if (!break_node)
    return NULL;
  uint32_t n = syntax_node_num_children(break_node);
  for (uint32_t i = 0; i < n; i++) {
    SyntaxElement el = syntax_node_child_or_token(break_node, i);
    if (el.kind == SYNTAX_ELEM_NODE && el.node)
      return el.node; // caller owns +1 ref
    if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
      syntax_token_release(el.token);
  }
  return NULL;
}

// C3 — collapses the 87+ repetitions of:
//     SyntaxNode *x = X_Y(...);
//     IpIndex t = x ? type_of_expr(ctx, x) : IP_NONE;
//     if (x) syntax_node_release(x);
// down to a single call. AST_*_FIELD getters return +1 refs that we
// own; visit_child types the child and releases the ref in one step.
static inline IpIndex visit_child(const SemaCtx *ctx, SyntaxNode *child) {
  if (!child)
    return IP_NONE;
  IpIndex t = type_of_expr(ctx, child);
  syntax_node_release(child);
  return t;
}


// ============================================================================
// Arith unification (Zig-style). Numeric predicates live in coerce.h.
// ============================================================================

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

// Peer-type resolution for control-flow joins (switch arms, if/else branches,
// labeled-break sites). `noreturn` is the bottom type and is ABSORBED — a
// diverging peer never constrains the result (mirrors Zig's resolvePeerTypes,
// which filters noreturn/undefined peers). A sticky-error peer folds the
// JOIN to error WITHOUT a diag: the bad arm was already diagnosed, and
// unify-failing every healthy sibling against `error` would spam N−1 bogus
// "arm has type X, expected error" diags. Non-noreturn, non-error peers
// fold via unify_arith. Used at every peer site (switch / if-else /
// labeled-break / loop-else).
static IpIndex peer_unify(IpIndex a, IpIndex b) {
  if (a.v == IP_NORETURN_TYPE.v)
    return b;
  if (b.v == IP_NORETURN_TYPE.v)
    return a;
  if (ip_is_error(a) || ip_is_error(b))
    return IP_ERROR_TYPE;
  return unify_arith(a, b);
}

// ---------------------------------------------------------------------
// Switch int coverage — a port of Zig's RangeSet (zig/src/RangeSet.zig).
// ore's int types are all <= 64-bit, so the bignum in Zig's `spans` is
// dropped: intervals are inclusive [lo,hi] in i64. Used by infer_switch for
// overlap detection + integer-range exhaustiveness.
// ---------------------------------------------------------------------
typedef struct {
  int64_t lo, hi; // inclusive
} IntInterval;

// addAssumeCapacity's overlap test: does [lo,hi] intersect any stored interval?
static bool interval_overlaps(const Vec *set, int64_t lo, int64_t hi) {
  const IntInterval *iv = (const IntInterval *)set->data;
  for (size_t i = 0; i < set->count; i++)
    if (lo <= iv[i].hi && iv[i].lo <= hi)
      return true;
  return false;
}

static int interval_cmp(const void *a, const void *b) {
  int64_t x = ((const IntInterval *)a)->lo, y = ((const IntInterval *)b)->lo;
  return (x > y) - (x < y);
}

// spans(): do the intervals cover [min,max] gap-free? Sorts the set; assumes
// overlaps were already rejected on insert (so adjacency means prev.hi+1==cur.lo).
static bool intervals_span(Vec *set, int64_t min, int64_t max) {
  if (set->count == 0)
    return false;
  qsort(set->data, set->count, sizeof(IntInterval), interval_cmp);
  const IntInterval *iv = (const IntInterval *)set->data;
  if (iv[0].lo != min || iv[set->count - 1].hi != max)
    return false;
  for (size_t i = 1; i < set->count; i++)
    if (iv[i - 1].hi == INT64_MAX || iv[i].lo != iv[i - 1].hi + 1)
      return false; // gap
  return true;
}

// [min,max] of an int primitive as i64. Returns false for types whose full
// range can't be represented/covered in i64 (u64/usize, comptime_int) — those
// can never be exhaustive without a `_` arm.
static bool int_type_bounds(IpIndex ty, int64_t *min, int64_t *max) {
  int64_t lo, hi;
  if (ty.v == IP_U8_TYPE.v) { lo = 0; hi = 255; }
  else if (ty.v == IP_U16_TYPE.v) { lo = 0; hi = 65535; }
  else if (ty.v == IP_U32_TYPE.v) { lo = 0; hi = 4294967295LL; }
  else if (ty.v == IP_I8_TYPE.v) { lo = -128; hi = 127; }
  else if (ty.v == IP_I16_TYPE.v) { lo = -32768; hi = 32767; }
  else if (ty.v == IP_I32_TYPE.v) { lo = INT32_MIN; hi = INT32_MAX; }
  else if (ty.v == IP_I64_TYPE.v || ty.v == IP_ISIZE_TYPE.v) { lo = INT64_MIN; hi = INT64_MAX; }
  else return false;
  *min = lo;
  *max = hi;
  return true;
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
  case SK_UNREACHABLE_KW:
    // `unreachable` types as noreturn — the bottom type. coerce_structural_ctx
    // already accepts noreturn flowing into any expected type, so
    // `return unreachable` works regardless of the enclosing fn's return
    // type, and a `final-ctl` clause body using `return unreachable`
    // discharges `b` cleanly. Diverges at runtime (codegen lowers to a
    // trap / __builtin_unreachable — deferred until the codegen pass).
    return IP_NORETURN_TYPE;
  case SK_ASM_LIT:
    // Inline asm-block — types as void. Rejected: IP_NORETURN_TYPE
    // would mark post-asm code unreachable (asm-blocks normally
    // return from svc/syscall). Output bindings model naturally as
    // lvalue writes; the block itself yields no value.
    return IP_VOID_TYPE;
  default:
    return IP_NONE;
  }
}

static DiagAnchor span_of(const SemaCtx *ctx, SyntaxNode *node) {
  // Phase P S6 — prefer a structural body anchor when sema is inside
  // a body-owning frame AND the node was visited by the body's
  // preorder walker (the rev map sees it). Body anchors resolve via
  // decl_ast_id_resolve's preorder walk in the publish path, which
  // survives sibling reparse byte-shifts. On miss (e.g. a sub-query
  // walked outside the body), fall back to the legacy FILE_RAW anchor
  // — still correct, just position-fragile.
  if (ctx->decl_ast_map && node) {
    uint32_t rel;
    if (decl_ast_id_lookup(ctx->decl_ast_map, node, &rel)) {
      // F5 (Phase P audit) — file_id == 0 would silently drop this
      // diag at collect time (file_id_eq filter). INFER_BODY always
      // sets ctx->file_local from a TopLevelEntry's e.file, so a zero
      // here is a structural bug, not a runtime condition.
      assert(ctx->file_local.idx != 0 &&
             "span_of: BODY anchor with file_local.idx == 0");
      return diag_anchor_body((uint16_t)ctx->file_local.idx,
                              (DeclKey)ctx->decl_key, (RelAstId)rel);
    } else {
      assert(false && "span_of: node present in sema context but missing from decl_ast_map");
    }
  }
  return diag_anchor_of_node((uint16_t)ctx->file_local.idx, node); // LINT_FILE_RAW_OK: span_of fallback when decl_ast_map miss (synthetic node, no map context)
}
static StrId intern_tok(struct db *s, SyntaxToken *t) {
  if (!t)
    return (StrId){0};
  const char *txt = syntax_token_text(t);
  uint32_t len = syntax_token_text_range(t).length;
  return pool_intern(&s->strings, txt, len);
}

// Coerce table + range-check are owned by coerce.{c,h} (Phase H). Use
// coerce_or_diag() at call sites — it emits the Zig-parity diag on
// failure.

static bool kind_is_discard_construct(SyntaxKind k) {
  return k == SK_BIND_DECL || k == SK_RETURN_STMT ||
         k == SK_BREAK_STMT || k == SK_CONTINUE_STMT || k == SK_DEFER_STMT ||
         k == SK_LOOP_EXPR || k == SK_BLOCK_STMT || k == SK_IF_EXPR ||
         k == SK_SWITCH_EXPR || k == SK_ASSIGN_EXPR || k == SK_EXPR_STMT;
}

// ============================================================================
// Phase-B terminator pass: "control reaches end of non-void function".
//
// `block_always_terminates(ctx, node)` returns true iff every execution
// path through `node` ends in an EXPLICIT terminator — `return`, a
// `noreturn` callee (panic / exit / non-resuming effect op), or an
// infinite loop with no reachable `break`.
//
// Slice 5 cutover: the prior implicit-last-expression slot (the body's
// trailing expression counted as termination if its type coerced to the
// fn's declared return type) is GONE. Under Zig-strict, only the explicit
// terminators above count. The `expected` parameter that gated the
// implicit-return path has been removed from the signature.
//
// Conservative everywhere: unknown / unhandled node kinds default to
// false. False negatives produce harmless diagnostics; false positives
// would silence the safety gate (and re-introduce the "ret with garbage"
// class of bugs the whole pass is here to prevent).
//
// IP_NORETURN_TYPE callees are detected via db_lookup_node_type — a
// side-effect-free cache-peek into the in-flight NodeTypeBuilder. Re-
// typing the callee would re-accumulate its effect row into
// ctx->body_effect_row; the cache-peek skips compute entirely.
// ============================================================================

static bool block_always_terminates(const SemaCtx *ctx, SyntaxNode *node);
static bool pattern_is_wildcard(SyntaxNode *p); // defined further down
// Monomorphization — demanded from the SK_CALL_EXPR hook; defined below.
IpIndex db_query_infer_instance(db_query_ctx *ctx, IpIndex inst);

// Scan `body` for any reachable `SK_BREAK_STMT`. Descends through every
// node kind EXCEPT nested `SK_LOOP_EXPR` (their breaks belong to them)
// and nested `SK_LAMBDA_EXPR` (their breaks belong to the inner fn's
// loops). Labelled-break tracking is deferred to Phase B+1.
static bool loop_has_reachable_break(SyntaxNode *body) {
  if (!body)
    return false;
  SyntaxKind k = syntax_node_kind(body);
  if (k == SK_BREAK_STMT)
    return true;
  if (k == SK_LOOP_EXPR || k == SK_LAMBDA_EXPR)
    return false; // breaks inside an inner scope target that scope.
  uint32_t n = syntax_node_num_children(body);
  bool found = false;
  for (uint32_t i = 0; i < n; i++) {
    SyntaxElement el = syntax_node_child_or_token(body, i);
    if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      if (!found && loop_has_reachable_break(el.node))
        found = true;
      syntax_node_release(el.node);
    } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      syntax_token_release(el.token);
    }
  }
  return found;
}

static bool block_always_terminates(const SemaCtx *ctx, SyntaxNode *node) {
  if (!node)
    return false;
  struct db *s = ctx->s;
  SyntaxKind k = syntax_node_kind(node);

  // `return` exits the function unconditionally.
  if (k == SK_RETURN_STMT)
    return true;

  // Any expression that TYPES to noreturn (the bottom type) terminates the
  // path: `unreachable` (SK_LITERAL_EXPR/SK_UNREACHABLE_KW → IP_NORETURN_TYPE),
  // a bare noreturn-fn call, etc. Cache-peek ONLY (db_lookup_node_type) — never
  // type_of_expr, which would re-accumulate the node's effect row into
  // ctx->body_effect_row (the same constraint the SK_CALL_EXPR arm documents
  // below). `unreachable` reaches here via the SK_EXPR_STMT recursion on its
  // inner literal node; the dedicated SK_CALL_EXPR arm still handles the
  // `with`-continuation (CPS) case, whose node types void, not noreturn. Defer
  // bodies are unaffected — SK_DEFER_STMT types void and is never recursed.
  {
    IpIndex nt = db_lookup_node_type(ctx, node);
    if (nt.v == IP_NORETURN_TYPE.v)
      return true;
  }

  // Noreturn-callee detection (`panic`, `exit`, non-resuming effect
  // ops). We can't call type_of_expr on the callee — it would re-
  // accumulate the callee's effect row into ctx->body_effect_row and
  // double it (breaks row-poly soundness; was the original Phase B
  // deferral reason). Instead we cache-peek via db_lookup_node_type:
  // pure HashMap read against the in-flight builder that check_expr
  // populated on its earlier descent. No re-typing, no effects.
  if (k == SK_CALL_EXPR) {
    CallExpr ce;
    if (!CallExpr_cast(node, &ce))
      return false;
    bool is_with = CallExpr_is_with(&ce);
    // A `with`-call's continuation is the rest of THIS fn (CPS): its `return`
    // IS the enclosing fn's return (it's typed against the enclosing fn's ret).
    // So a tail with-call whose continuation body always terminates terminates
    // the enclosing fn. Only `with` (the SK_WITH_KW marker) reaches here;
    // `handle`'s action isn't a continuation lambda → with_continuation NULL.
    if (is_with) {
      SyntaxNode *cont = CallExpr_with_continuation(&ce);
      bool term = false;
      if (cont) {
        LambdaExpr lam;
        if (LambdaExpr_cast(cont, &lam)) {
          SyntaxNode *cb = LambdaExpr_body(&lam);
          if (cb) {
            term = block_always_terminates(ctx, cb);
            syntax_node_release(cb);
          }
        }
        syntax_node_release(cont);
      }
      if (term)
        return true;
    }
    // Noreturn-head fallback (`with panic()` / a noreturn callee). CallExpr_head
    // skips the loose `x :=` binder / action arg-list that plain
    // CallExpr_callee would mis-pick.
    SyntaxNode *callee = (is_with || CallExpr_is_handle(&ce))
                             ? CallExpr_head(&ce)
                             : CallExpr_callee(&ce);
    IpIndex cty = callee ? db_lookup_node_type(ctx, callee) : IP_NONE;
    if (callee)
      syntax_node_release(callee);
    if (cty.v == IP_NONE.v)
      return false;
    if (ip_tag(&s->intern, cty) != IP_TAG_FN_TYPE)
      return false;
    return ip_key(&s->intern, cty).fn_type.ret.v == IP_NORETURN_TYPE.v;
  }

  // `if (...) <x> then else` — both branches must terminate AND an else
  // must exist. No else → control may drop past the if.
  if (k == SK_IF_EXPR) {
    IfExpr ie;
    if (!IfExpr_cast(node, &ie))
      return false;
    SyntaxNode *then_b = IfExpr_then_branch(&ie);
    SyntaxNode *else_b = IfExpr_else_branch(&ie);
    bool ok = then_b && else_b &&
              block_always_terminates(ctx, then_b) &&
              block_always_terminates(ctx, else_b);
    if (then_b) syntax_node_release(then_b);
    if (else_b) syntax_node_release(else_b);
    return ok;
  }

  // `switch (s) { ... }` — every arm body must terminate AND the switch
  // must be exhaustive (we approximate exhaustiveness by checking for a
  // wildcard `_` arm; the typecheck side does the full enum-coverage
  // check at infer.c:529-548 and will diag if missing).
  if (k == SK_SWITCH_EXPR) {
    SwitchExpr se;
    if (!SwitchExpr_cast(node, &se))
      return false;
    SyntaxNode *arms = SwitchExpr_arms(&se);
    if (!arms)
      return false;
    bool all_term = true;
    bool has_wildcard = false;
    uint32_t na = syntax_node_num_children(arms);
    for (uint32_t i = 0; i < na && all_term; i++) {
      SyntaxElement ael = syntax_node_child_or_token(arms, i);
      if (ael.kind != SYNTAX_ELEM_NODE || !ael.node) {
        if (ael.kind == SYNTAX_ELEM_TOKEN && ael.token)
          syntax_token_release(ael.token);
        continue;
      }
      SyntaxNode *arm = ael.node;
      if (syntax_node_kind(arm) == SK_SWITCH_ARM) {
        SwitchArm sa;
        if (SwitchArm_cast(arm, &sa)) {
          SyntaxNode *pat = SwitchArm_pattern(&sa);
          SyntaxNode *body = SwitchArm_body(&sa);
          if (pat) {
            if (pattern_is_wildcard(pat))
              has_wildcard = true;
            syntax_node_release(pat);
          }
          if (body) {
            if (!block_always_terminates(ctx, body))
              all_term = false;
            syntax_node_release(body);
          } else {
            all_term = false;
          }
        }
      }
      syntax_node_release(arm);
    }
    syntax_node_release(arms);
    // Without a wildcard, an enum switch is terminating iff it is exhaustive
    // — and infer_switch already diagnoses any missing variant. So trust an
    // enum scrutinee here (a coverage gap is reported there, not as a spurious
    // "control reaches end"); a non-enum scrutinee still needs a `_` arm.
    bool scrut_finite = false; // enum / bool / int — a type infer_switch
                               // exhaustiveness-checks (so a coverage gap is
                               // reported there, not as a spurious "control
                               // reaches end")
    SyntaxNode *scrut = SwitchExpr_scrutinee(&se);
    if (scrut) {
      IpIndex st = db_lookup_node_type(ctx, scrut);
      scrut_finite = st.v == IP_BOOL_TYPE.v || is_concrete_int(st) ||
                     (st.v != IP_NONE.v &&
                      ip_tag(&ctx->s->intern, st) == IP_TAG_ENUM_TYPE);
      syntax_node_release(scrut);
    }
    return all_term && (has_wildcard || scrut_finite);
  }

  // `loop body`, `loop (cond) body`, `loop (range) <i> body`, etc.
  // - With a header expression: cond may go false initially or per-iter,
  //   so control may drop past the loop → false.
  // - Infinite loop (no header expr): terminating iff body contains no
  //   reachable `break` targeting this loop.
  if (k == SK_LOOP_EXPR) {
    LoopExpr le;
    if (!LoopExpr_cast(node, &le))
      return false;
    SyntaxNode *cond = LoopExpr_condition(&le);
    SyntaxNode *body = LoopExpr_body(&le);
    bool ok = false;
    if (!cond) {
      // Infinite loop. Scan the body for any reachable `break`.
      if (body && !loop_has_reachable_break(body))
        ok = true;
    }
    if (cond) syntax_node_release(cond);
    if (body) syntax_node_release(body);
    return ok;
  }

  if (k == SK_BLOCK_STMT) {
    // Slice 5 — Zig-strict: a block "terminates" only if some statement
    // is itself a terminator (explicit `return`, `break`, noreturn-callee,
    // infinite-loop-without-reachable-break). The prior "implicit-return
    // slot: last expr-kind child counts as termination" path is GONE —
    // values out of a block flow ONLY through explicit return / break.
    BlockStmt bs = {.syntax = node};
    SyntaxNode *stmts = BlockStmt_stmts(&bs);
    if (!stmts)
      return false;
    uint32_t total = syntax_node_num_children(stmts);
    bool ok = false;
    for (uint32_t i = 0; i < total && !ok; i++) {
      SyntaxElement el = syntax_node_child_or_token(stmts, i);
      if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
        if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
          syntax_token_release(el.token);
        continue;
      }
      SyntaxNode *stmt = el.node;
      if (block_always_terminates(ctx, stmt))
        ok = true; // rest is dead code; whole block terminates.
      syntax_node_release(stmt);
    }
    syntax_node_release(stmts);
    return ok;
  }

  // Expr-statement / paren wrappers: recurse into the inner expression.
  // A naked terminator wrapped in one of these still terminates the
  // enclosing fn (e.g. `(return x)` or `return x;` as an expr-stmt).
  //
  // SK_DEFER_STMT is INTENTIONALLY EXCLUDED. Defer is scope-exit cleanup
  // that runs immediately before the actual return — by the time defer's
  // body executes, the enclosing fn has ALREADY decided to return (the
  // return value is computed first, then defer runs, then control leaves).
  // So defer's own body-termination characteristics MUST NOT contribute
  // to the fn's missing-return analysis. Specifically:
  //   - `defer loop { ... }` (infinite loop in defer) shouldn't satisfy
  //     the fn's missing-return check — the fn still falls through.
  //   - `defer return X` inside defer is semantically dubious (return
  //     from the enclosing fn AFTER another return is meaningless); we
  //     don't credit it as a fn terminator either.
  if (k == SK_EXPR_STMT || k == SK_PAREN_EXPR) {
    bool ok = false;
    uint32_t n = syntax_node_num_children(node);
    for (uint32_t i = 0; i < n && !ok; i++) {
      SyntaxElement el = syntax_node_child_or_token(node, i);
      if (el.kind == SYNTAX_ELEM_NODE && el.node) {
        if (block_always_terminates(ctx, el.node))
          ok = true;
        syntax_node_release(el.node);
      } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
        syntax_token_release(el.token);
      }
    }
    return ok;
  }

  // Everything else (literals, refs, fields, indexing, binops, etc.)
  // does not by itself terminate execution.
  return false;
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
        uint64_t key = h;
        if (ctx->decl_ast_map) {
          void *vrel = hashmap_get(&ctx->decl_ast_map->rev, h);
          if (vrel)
            key = (uint64_t)((uintptr_t)vrel - 1);
        }
        // Presence via the occupied bitset, NOT `v != NULL`: a zero-valued
        // entry is a legal stored index (the bitset is the truth).
        if (hashmap_contains(&ctx->types->types, key)) {
          void *v = hashmap_get(&ctx->types->types, key);
          return (IpIndex){.v = (uint32_t)(uintptr_t)v};
        }
      }
      // Bound but not yet typed. Body bindings are order-dependent
      // (Zig-like): db_body_scope_lookup is NOT position-aware (a bind is
      // visible to its whole scope), so a use that sits textually BEFORE
      // its binding completes — `print(x); x := 5`, or `x` inside its own
      // initializer `x := x` — lands here. This used to return a silent
      // IP_NONE: zero diags, every consumer absorbed, and the binding's
      // RHS effects (instance demands, reference edges) vanished.
      {
        TextRange ur = syntax_node_text_range(use_node);
        if (ur.start < bind.range.start + bind.range.length) {
          db_emit(s, DIAG_ERROR, span_of(ctx, use_node),
                  "'%S' used before its declaration", name);
          return IP_ERROR_TYPE;
        }
      }
      // Residual (use after the bind completed, type still missing) — a
      // walk-order state, not user error; stays quiet by design.
      return IP_NONE;
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
  return IP_ERROR_TYPE;
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

// The enclosing fn's declared return type, or IP_NONE when there is no
// enclosing DefId / its signature isn't a fn-type. Mirrors the fall-back arm
// of SK_RETURN_STMT so a `with` continuation's `return X` and the enclosing fn
// agree on the target type.
static IpIndex fn_ret_of(const SemaCtx *ctx) {
  if (ctx->enclosing_fn.idx == DEF_ID_NONE.idx)
    return IP_NONE;
  struct db *s = ctx->s;
  const FnSignature *sig = db_query_fn_signature(s, ctx->enclosing_fn);
  IpIndex sigty = sig ? sig->type : IP_NONE;
  if (sigty.v != IP_NONE.v && ip_tag(&s->intern, sigty) == IP_TAG_FN_TYPE)
    return ip_key(&s->intern, sigty).fn_type.ret;
  return IP_NONE;
}

// Type an effect-handler op-clause body against the value it must produce
// (`direct`/`val` resume their value to the caller ⇒ `expected` = the op
// result; `ctl`/`final-ctl` produce the handler answer ⇒ `expected` = `b`).
// Mirrors the fn-body path exactly: a BLOCK body flows its value through
// explicit `return`s (checked via expected_ret_override) and is gated by the
// missing-return terminator check; any other (bare expression / value `if` /
// `switch`) is checked bidirectionally with check_expr. `expected` IP_NONE
// (e.g. a pass-through handler whose answer isn't known) ⇒ synthesize only.
static void check_op_clause_body(const SemaCtx *ctx, SyntaxNode *body,
                                 IpIndex expected) {
  struct db *s = ctx->s;
  // ip_is_error: an op whose RESULT type failed to resolve (sticky error in
  // the op table) must not fire "reaches end without producing a value
  // (expected error)" — the signature diag already covers it.
  bool want = expected.v != IP_NONE.v && expected.v != IP_VOID_TYPE.v &&
              expected.v != IP_NORETURN_TYPE.v && !ip_is_error(expected);
  SemaCtx cc = *ctx;
  if (syntax_node_kind(body) == SK_BLOCK_STMT) {
    if (expected.v != IP_NONE.v)
      cc.expected_ret_override = expected;
    (void)type_of_expr(&cc, body);
    if (want && !block_always_terminates(&cc, body))
      db_emit(s, DIAG_ERROR, span_of(&cc, body),
              "operation handler reaches end without producing a value "
              "(expected %T)",
              expected);
  } else if (want) {
    (void)check_expr(&cc, body, expected);
  } else {
    (void)type_of_expr(&cc, body);
  }
}

// Type a with-continuation lambda's BODY inline (in the enclosing fn's ctx, so
// its callee rows fold in via the normal SK_CALL_EXPR accumulation), isolated
// via a snapshot of body_effect_row, and return its inferred effect row. The
// caller folds it back: a handler DISCHARGES it; a higher-order head flows it
// through `f`'s effect-parameter (attach it to the continuation's fn-type, then
// the arg coercion's row_unify carries it). The snapshot keeps the continuation
// delta separate from the enclosing-so-far row.
static IpIndex type_continuation_body(const SemaCtx *ctx, SyntaxNode *cont) {
  IpIndex row = IP_EMPTY_EFFECT_ROW;
  LambdaExpr lam;
  if (!cont || !ctx->body_effect_row || !LambdaExpr_cast(cont, &lam))
    return row;
  SyntaxNode *cb = LambdaExpr_body(&lam);
  if (cb) {
    IpIndex before = *ctx->body_effect_row;
    *ctx->body_effect_row = IP_EMPTY_EFFECT_ROW;
    (void)type_of_expr(ctx, cb); // callee rows fold into the snapshot
    row = row_flatten(ctx, *ctx->body_effect_row);
    *ctx->body_effect_row = before;
    syntax_node_release(cb);
  }
  return row;
}

// Isolate a handler-call ACTION's effect row + capture its result type, for
// BOTH surfaces: `with` (the action is a continuation lambda — row from its
// body, ret from its signature) and `handle` (the action is a bare expression
// like `risky()` — type it isolated; its value is the result). The caller
// discharges the returned row, so both forms get the handled effect removed.
static IpIndex type_action_isolated(const SemaCtx *ctx, SyntaxNode *action,
                                    IpIndex *out_ret) {
  *out_ret = IP_VOID_TYPE;
  IpIndex row = IP_EMPTY_EFFECT_ROW;
  if (!action || !ctx->body_effect_row)
    return row;
  struct db *s = ctx->s;
  LambdaExpr lam;
  if (LambdaExpr_cast(action, &lam)) { // `with` continuation thunk
    IpIndex sig = type_of_expr(ctx, action);
    if (!ip_is_error(sig) && ip_tag(&s->intern, sig) == IP_TAG_FN_TYPE)
      *out_ret = ip_key(&s->intern, sig).fn_type.ret;
    row = type_continuation_body(ctx, action);
  } else { // `handle` action expression
    IpIndex before = *ctx->body_effect_row;
    *ctx->body_effect_row = IP_EMPTY_EFFECT_ROW;
    *out_ret = type_of_expr(ctx, action); // value = result; effects isolated
    row = row_flatten(ctx, *ctx->body_effect_row);
    *ctx->body_effect_row = before;
  }
  return row;
}

// Check a with-continuation arg against its callee parameter (non-handler
// heads). Types the continuation body (so `rest` is checked) + infers its
// effect row, attaches that row to the continuation's fn-type, and coerces it
// against `param` — so the existing fn-effect-row unification flows the row
// through the callee's type (Koka: it surfaces iff the callee references its
// cont-param's effect). NOT a direct accumulation.
static void check_continuation_arg(const SemaCtx *ctx, SyntaxNode *cont,
                                   IpIndex param) {
  struct db *s = ctx->s;
  IpIndex cont_row = type_continuation_body(ctx, cont);
  IpIndex sig = type_of_expr(ctx, cont); // signature (params/ret, empty row)
  IpIndex cont_fn = sig;
  if (!ip_is_error(sig) && ip_tag(&s->intern, sig) == IP_TAG_FN_TYPE) {
    IpKey ck = ip_key(&s->intern, sig);
    // The synthetic continuation's signature ret is void, but its `return`s
    // are checked against the ENCLOSING fn's ret (CPS forward) — so adopt the
    // callee's cont-param ret here, else `with f \n return v` mis-coerces
    // (void vs the param's ret). Effect row is the inferred body row.
    IpIndex ret = ck.fn_type.ret;
    if (!ip_is_error(param) && ip_tag(&s->intern, param) == IP_TAG_FN_TYPE)
      ret = ip_key(&s->intern, param).fn_type.ret;
    IpKey nk = {.kind = IPK_FN_TYPE,
                .fn_type = {.ret = ret,
                            .modifiers = ck.fn_type.modifiers,
                            .params = ck.fn_type.params,
                            .n_params = ck.fn_type.n_params,
                            .effect_row = cont_row},
                .src_arena = NULL, // params reused — no new arena storage
                .src_gen = s->request_arena.generation};
    cont_fn = ip_get(&s->intern, nk);
  }
  (void)coerce_or_diag(ctx, cont, cont_fn, param);
}

// ============================================================================
// Statement helpers (the completed-common-statements work).
// ============================================================================

// The annotation / RHS node of a let-bind wrapper (borrowed; release).
static void bind_parts(SyntaxNode *decl, SyntaxKind k, SyntaxNode **type_out,
                       SyntaxNode **val_out) {
  *type_out = NULL;
  *val_out = NULL;
  (void)k; // mutability is no longer a node kind; both share one accessor
  BindDef b;
  if (BindDef_cast(decl, &b)) {
    *type_out = BindDef_type(&b);
    *val_out = BindDef_value(&b);
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
  // Slice 6.13 Fix D — INFERRED let-bind (no type annotation) whose RHS
  // produced IP_NONE is a silent typing hole. Emit a hard diag so the
  // failure is named at the binding site rather than cascading through
  // downstream hovers and inference. IP_ERROR_TYPE is left alone —
  // upstream already diagnosed (undefined ident, unknown type, etc.).
  // IP_NONE specifically indicates "no type produced" — usually a
  // not-yet-implemented builtin or an inference path that silently
  // bailed.
  if (!type_node && value_node && bt.v == IP_NONE.v) {
    struct db *s = ctx->s;
    db_emit(s, DIAG_ERROR, span_of(ctx, decl),
            "cannot infer type of binding (right-hand side did not produce "
            "a type)");
    bt = IP_ERROR_TYPE;
  }
  // W1 — mutable binding (`:=`) cannot hold a comptime-only type. Zig
  // rule (Sema.zig 3032/3238/3736): `var x = 42` errors with "variable
  // of type 'comptime_int' must be const or comptime". Mutating an
  // unsized comptime numeric at runtime is meaningless; either use
  // `::` (immutable, comptime-friendly) or annotate with a concrete
  // type so the RHS narrows via the existing range-check coerce.
  //
  // Gated on (no annotation present), since the annotation path already
  // ran check_expr against a concrete dest. Skip if bt is the error
  // sentinel — upstream already diagnosed.
  BindDef b;
  if (BindDef_cast(decl, &b) && !BindDef_is_const(&b) && !type_node &&
      !ip_is_error(bt) && is_comptime_only(bt)) {
    struct db *s = ctx->s;
    const char *kind_name =
        (bt.v == IP_COMPTIME_INT_TYPE.v)   ? "comptime_int"
        : (bt.v == IP_COMPTIME_FLOAT_TYPE.v) ? "comptime_float"
                                             : "type";
    const char *hint =
        (bt.v == IP_COMPTIME_INT_TYPE.v)
            ? "annotate with a concrete integer type (e.g. `name: usize = …`) "
              "or use `::` for an immutable binding"
        : (bt.v == IP_COMPTIME_FLOAT_TYPE.v)
            ? "annotate with a concrete float type (e.g. `name: f32 = …`) "
              "or use `::` for an immutable binding"
            : "use `::` for an immutable binding (the `type` type has "
              "no runtime representation)";
    db_emit(s, DIAG_ERROR, span_of(ctx, decl),
            "mutable variable cannot hold value of type '%s'; %s",
            kind_name, hint);
    bt = IP_ERROR_TYPE;
  }
  if (type_node)
    syntax_node_release(type_node);
  if (value_node)
    syntax_node_release(value_node);
  return bt;
}

// The if-condition: when a capture is present, type the cond as an
// optional (?T), unwrap, and push the element type at the capture node
// (the capture is the bind_site). A plain cond (no capture) is checked
// against bool. Shared by type_of_expr + check_expr.
static void handle_if_cond(const SemaCtx *ctx, SyntaxNode *cond,
                           SyntaxNode *capture) {
  if (!cond)
    return;
  struct db *s = ctx->s;
  if (capture) {
    IpIndex ct = type_of_expr(ctx, cond);
    IpIndex elem = IP_NONE;
    if (ip_is_error(ct)) {
      elem = IP_ERROR_TYPE; // sticky — capture binding poisoned silently
    } else if (ct.v != IP_NONE.v) {
      if (ip_tag(&s->intern, ct) == IP_TAG_OPTIONAL_TYPE) {
        elem = ip_key(&s->intern, ct).optional_type.elem;
      } else {
        db_emit(s, DIAG_ERROR, span_of(ctx, capture),
                "capture binding requires optional condition; got %T", ct);
      }
    }
    node_type_builder_push(ctx, capture, elem); // bind_site = capture node
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
// Forward decl — defined after infer_switch (with the comptime helpers).
static IpIndex infer_switch_folded(const SemaCtx *ctx, SyntaxNode *node,
                                   IpIndex expected, ConstValue scrut_val,
                                   DefId enum_ctx);

static IpIndex infer_switch(const SemaCtx *ctx, SyntaxNode *node,
                            IpIndex expected) {
  struct db *s = ctx->s;
  SwitchExpr sw;
  if (!SwitchExpr_cast(node, &sw))
    return IP_NONE;
  bool check_mode = (expected.v != IP_NONE.v);

  SyntaxNode *scrutinee = SwitchExpr_scrutinee(&sw);
  IpIndex scrut = scrutinee ? type_of_expr(ctx, scrutinee) : IP_NONE;

  // Path A — constant folding is implicit. If the scrutinee folds to a
  // known value at compile time, do dead-arm elimination automatically:
  // only the matching arm is type-checked, exhaustiveness is skipped.
  // The explicit `comptime switch` keyword is a strictness marker
  // (errors if scrutinee can't fold) — both paths share
  // infer_switch_folded once the value is known. This matches Zig's
  // plain-`switch` behavior on comptime-known scrutinees, and removes
  // the need to annotate inner switches inside comptime-arm bodies
  // (their scrutinees fold via the outer's selection).
  if (scrutinee) {
    ConstValue cv = db_const_eval(s, ctx->file_local, scrutinee,
                                  SEMA_CONST_ANCHOR(ctx));
    if (cv.kind != CONST_NONE) {
      DefId enum_ctx = {0};
      if (cv.kind == CONST_ENUM_VARIANT)
        enum_ctx = cv.enum_variant.enum_def;
      syntax_node_release(scrutinee);
      return infer_switch_folded(ctx, node, expected, cv, enum_ctx);
    }
    syntax_node_release(scrutinee);
  }

  IpIndex result = expected;
  bool result_set = check_mode;
  bool wildcard = false;
  bool saw_true = false, saw_false = false; // bool-scrutinee coverage
  // Covered enum-variant names (for exhaustiveness). Dynamic so there is NO
  // silent cliff: vec_init doesn't allocate until the first matched variant.
  Vec covered;
  vec_init(&covered, sizeof(StrId));
  // Int-scrutinee coverage: the set of value/range intervals, for overlap
  // detection + integer-range exhaustiveness (Zig RangeSet, ported above).
  bool scrut_is_int = is_concrete_int(scrut);
  Vec ivals;
  vec_init(&ivals, sizeof(IntInterval));

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
      // Arm = [SK_SWITCH_PATTERN_LIST, body] (6.27 — the comma-separated
      // patterns are wrapped in a list). Iterate the list's children as the
      // patterns; the arm's other (non-list) node child is the body.
      uint32_t an = syntax_node_num_children(arm);
      SyntaxNode *body = NULL;
      for (uint32_t j = 0; j < an; j++) {
        SyntaxElement pel = syntax_node_child_or_token(arm, j);
        if (pel.kind == SYNTAX_ELEM_TOKEN && pel.token) {
          syntax_token_release(pel.token);
          continue;
        }
        if (pel.kind != SYNTAX_ELEM_NODE || !pel.node)
          continue;
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
            bool is_range = syntax_node_kind(pat) == SK_BIN_EXPR &&
                            BinExpr_cast(pat, &rbe) &&
                            (BinExpr_op_kind(&rbe) == SK_DOT_DOT_LT ||
                             BinExpr_op_kind(&rbe) == SK_DOT_DOT_EQ);
            if (pattern_is_wildcard(pat)) {
              wildcard = true;
            } else if (is_range) {
              // Range pattern `lo..<hi` / `lo..=hi`: check both bounds against
              // the scrutinee type (don't route the whole SK_BIN_EXPR through
              // the generic binop dispatch, which rejects ranges). For an int
              // scrutinee, fold the bounds into the coverage set (`..<` drops
              // the upper end, `..=` keeps it) for overlap + exhaustiveness.
              SyntaxNode *lo = BinExpr_lhs(&rbe);
              SyntaxNode *hi = BinExpr_rhs(&rbe);
              if (lo)
                (void)check_expr(ctx, lo, scrut);
              if (hi)
                (void)check_expr(ctx, hi, scrut);
              if (scrut_is_int && lo && hi) {
                ConstValue lv = db_const_eval(s, ctx->file_local, lo,
                                              SEMA_CONST_ANCHOR(ctx));
                ConstValue hv = db_const_eval(s, ctx->file_local, hi,
                                              SEMA_CONST_ANCHOR(ctx));
                if (lv.kind == CONST_INT && hv.kind == CONST_INT) {
                  int64_t l = (int64_t)lv.int_val;
                  int64_t h = BinExpr_op_kind(&rbe) == SK_DOT_DOT_EQ
                                  ? (int64_t)hv.int_val
                                  : (int64_t)hv.int_val - 1;
                  if (l > h)
                    db_emit(s, DIAG_ERROR, span_of(ctx, pat),
                            "empty or reversed range pattern");
                  else if (interval_overlaps(&ivals, l, h))
                    db_emit(s, DIAG_ERROR, span_of(ctx, pat),
                            "switch case overlaps an earlier case");
                  else {
                    IntInterval iv = {l, h};
                    vec_push(&ivals, &iv);
                  }
                }
              }
              if (lo)
                syntax_node_release(lo);
              if (hi)
                syntax_node_release(hi);
            } else {
              if (syntax_node_kind(pat) == SK_ENUM_REF_EXPR) {
                EnumRefExpr er;
                if (EnumRefExpr_cast(pat, &er)) {
                  SyntaxToken *vt = EnumRefExpr_variant(&er);
                  StrId vn = intern_tok(s, vt);
                  if (vt)
                    syntax_token_release(vt);
                  if (vn.idx)
                    vec_push(&covered, &vn);
                }
              } else if (scrut.v == IP_BOOL_TYPE.v) {
                // Bool scrutinee: fold each pattern to track true/false coverage.
                ConstValue pv = db_const_eval(s, ctx->file_local, pat,
                                              SEMA_CONST_ANCHOR(ctx));
                if (pv.kind == CONST_BOOL) {
                  if (pv.bool_val)
                    saw_true = true;
                  else
                    saw_false = true;
                }
              } else if (scrut_is_int) {
                // Int value pattern: a singleton interval [v,v] for overlap +
                // exhaustiveness.
                ConstValue pv = db_const_eval(s, ctx->file_local, pat,
                                              SEMA_CONST_ANCHOR(ctx));
                if (pv.kind == CONST_INT) {
                  int64_t v = (int64_t)pv.int_val;
                  if (interval_overlaps(&ivals, v, v))
                    db_emit(s, DIAG_ERROR, span_of(ctx, pat),
                            "duplicate switch case");
                  else {
                    IntInterval iv = {v, v};
                    vec_push(&ivals, &iv);
                  }
                }
              }
              (void)check_expr(ctx, pat, scrut);
            }
            syntax_node_release(pat);
          }
          syntax_node_release(pel.node);
        } else { // the arm body (the lone non-list node child)
          if (body)
            syntax_node_release(body);
          body = pel.node;
        }
      }
      if (body) {
        if (check_mode) {
          (void)check_expr(ctx, body, expected);
        } else {
          IpIndex bt = type_of_expr(ctx, body);
          if (!result_set) {
            result = bt;
            result_set = true;
          } else if (bt.v != IP_NONE.v && result.v != IP_NONE.v &&
                     bt.v != result.v) {
            IpIndex u = peer_unify(result, bt);
            if (u.v != IP_NONE.v)
              result = u;
            else
              db_emit(s, DIAG_ERROR, span_of(ctx, body),
                      "switch arm has type %T, expected %T", bt, result);
          }
        }
        syntax_node_release(body);
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

  // Bool exhaustiveness: both `true` and `false`, or a `_` wildcard.
  if (!wildcard && scrut.v == IP_BOOL_TYPE.v && !(saw_true && saw_false)) {
    if (!saw_true)
      db_emit(s, DIAG_ERROR, span_of(ctx, node),
              "non-exhaustive switch: missing 'true'");
    if (!saw_false)
      db_emit(s, DIAG_ERROR, span_of(ctx, node),
              "non-exhaustive switch: missing 'false'");
  }

  // Integer-range exhaustiveness (Zig RangeSet.spans): the value/range cases
  // must cover the scrutinee type's full [min,max], else a `_` is required.
  // Wide/64-bit types can't be fully covered, so they always need `_`.
  if (!wildcard && scrut_is_int) {
    int64_t imin, imax;
    if (!int_type_bounds(scrut, &imin, &imax) ||
        !intervals_span(&ivals, imin, imax))
      db_emit(s, DIAG_ERROR, span_of(ctx, node),
              "non-exhaustive switch over %T: cover all values or add a `_` arm",
              scrut);
  }

  vec_free(&ivals);
  vec_free(&covered);
  return check_mode ? expected : (result_set ? result : IP_VOID_TYPE);
}

// ============================================================================
// Route A — comptime if/switch dispatch (B6).
//
// `comptime if (cond) X else Y` and `comptime switch (scrut) { arms }`
// eval the condition/scrutinee at compile time and type-check ONLY the
// taken branch. Dead branches are skipped entirely — no exhaustiveness
// check, no per-arm type unification, no hover info on dead-branch
// nodes. This is Zig's `inline switch` / `if (comptime cond)` model.
//
// Separate from runtime infer_switch / SK_IF_EXPR — no `is_comptime`
// flag threaded through shared helpers. Shared work (folding the
// scrutinee + arm patterns) lives in const_eval; these two functions
// own the comptime fast-path. Future kinds (more block forms for #7a,
// SK_REF_EXPR for comptime-let resolution, `comptime fn` calls) each
// add a NEW arm to sema_comptime_select, never touch the runtime arms.
//
// The dead branch sits in the tree with no node-type entries — invisible
// to hover. A future `comptime_selections` HashMap on NodeTypesRange
// could record the winner for codegen / inlay hints; deferred until
// codegen exists (see plan Open architectural notes).
// ============================================================================

static IpIndex infer_comptime_if(const SemaCtx *ctx, SyntaxNode *node,
                                 IpIndex expected) {
  struct db *s = ctx->s;
  IfExpr ie;
  if (!IfExpr_cast(node, &ie))
    return IP_NONE;
  SyntaxNode *cond = IfExpr_condition(&ie);
  SyntaxNode *then_b = IfExpr_then_branch(&ie);
  SyntaxNode *else_b = IfExpr_else_branch(&ie);

  if (!cond) {
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "comptime if requires a condition");
    if (then_b) syntax_node_release(then_b);
    if (else_b) syntax_node_release(else_b);
    return IP_ERROR_TYPE;
  }

  // Visit the cond so the unused-decl tracker sees its name refs — the
  // fold path below only calls db_const_eval, which doesn't go through
  // the sema visit machinery. The type result is discarded; this is
  // purely for ref-tracking + hover info on the cond's subtree.
  (void)type_of_expr(ctx, cond);
  ConstValue cv = db_const_eval(s, ctx->file_local, cond,
                                SEMA_CONST_ANCHOR(ctx));
  syntax_node_release(cond);

  if (cv.kind != CONST_BOOL) {
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "comptime if condition must be a comptime-known bool");
    if (then_b) syntax_node_release(then_b);
    if (else_b) syntax_node_release(else_b);
    return IP_ERROR_TYPE;
  }

  SyntaxNode *winner = cv.bool_val ? then_b : else_b;
  SyntaxNode *loser  = cv.bool_val ? else_b : then_b;
  IpIndex result;
  if (winner) {
    if (expected.v != IP_NONE.v) {
      (void)check_expr(ctx, winner, expected);
      result = expected;
    } else {
      result = type_of_expr(ctx, winner);
    }
    syntax_node_release(winner);
  } else {
    // `comptime if (true) X` with no else, taken-branch missing: void.
    result = IP_VOID_TYPE;
  }
  if (loser)
    syntax_node_release(loser);
  return result;
}

// Shared switch-folding helper: given a comptime-known scrutinee
// value, walk arms, pick the matching one, type-check ONLY its body.
// Used by:
//   - `comptime switch` (errors if scrutinee won't fold; calls here on success)
//   - plain `switch` where the scrutinee happens to fold (no error if not;
//     callers fall back to the regular runtime exhaustiveness path)
//
// Returns the type of the winning arm, or IP_ERROR_TYPE if no arm
// matches (emits "no arm matches" diag).

// Comptime equality for switch-arm matching — the subset of ConstValue kinds
// a folded scrutinee/pattern can take.
static bool const_val_eq(ConstValue a, ConstValue b) {
  if (a.kind != b.kind)
    return false;
  switch (a.kind) {
  case CONST_ENUM_VARIANT:
    return a.enum_variant.enum_def.idx == b.enum_variant.enum_def.idx &&
           a.enum_variant.variant_idx == b.enum_variant.variant_idx;
  case CONST_INT:
    return a.int_val == b.int_val;
  case CONST_BOOL:
    return a.bool_val == b.bool_val;
  case CONST_NAMESPACE:
    return a.nsid.idx == b.nsid.idx;
  default:
    return false;
  }
}

// Does the comptime-int scrutinee fall in the range pattern `lo..<hi` (half
// open) / `lo..=hi` (inclusive)? Both bounds must fold to ints.
static bool const_in_range(const SemaCtx *ctx, SyntaxNode *range,
                           ConstValue scrut) {
  if (scrut.kind != CONST_INT)
    return false;
  BinExpr be;
  if (!BinExpr_cast(range, &be))
    return false;
  SyntaxKind op = BinExpr_op_kind(&be);
  struct db *s = ctx->s;
  SyntaxNode *lo_n = BinExpr_lhs(&be);
  SyntaxNode *hi_n = BinExpr_rhs(&be);
  ConstValue lo = lo_n ? db_const_eval(s, ctx->file_local, lo_n,
                                       SEMA_CONST_ANCHOR(ctx))
                       : (ConstValue){0};
  ConstValue hi = hi_n ? db_const_eval(s, ctx->file_local, hi_n,
                                       SEMA_CONST_ANCHOR(ctx))
                       : (ConstValue){0};
  if (lo_n)
    syntax_node_release(lo_n);
  if (hi_n)
    syntax_node_release(hi_n);
  if (lo.kind != CONST_INT || hi.kind != CONST_INT)
    return false;
  int64_t v = (int64_t)scrut.int_val, l = (int64_t)lo.int_val,
          h = (int64_t)hi.int_val;
  return op == SK_DOT_DOT_EQ ? (v >= l && v <= h) : (v >= l && v < h);
}

static IpIndex infer_switch_folded(const SemaCtx *ctx, SyntaxNode *node,
                                   IpIndex expected, ConstValue scrut_val,
                                   DefId enum_ctx) {
  struct db *s = ctx->s;
  SwitchExpr sw;
  if (!SwitchExpr_cast(node, &sw))
    return IP_NONE;
  SyntaxNode *arms = SwitchExpr_arms(&sw);
  if (!arms) {
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "comptime switch: no arm matches value");
    return IP_ERROR_TYPE;
  }

  SyntaxNode *winner_body = NULL;
  bool matched = false;
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
    // Arm = [SK_SWITCH_PATTERN_LIST, body] (6.27). Match scrut_val against
    // each pattern in the list — wildcard, range (`..<`/`..=`), or a folded
    // value (bare `.variant` resolves against the scrutinee's enum via
    // enum_ctx). The other node child is the body.
    uint32_t an = syntax_node_num_children(arm);
    SyntaxNode *body = NULL;
    bool arm_matched = false;
    for (uint32_t j = 0; j < an; j++) {
      SyntaxElement pel = syntax_node_child_or_token(arm, j);
      if (pel.kind == SYNTAX_ELEM_TOKEN && pel.token) {
        syntax_token_release(pel.token);
        continue;
      }
      if (pel.kind != SYNTAX_ELEM_NODE || !pel.node)
        continue;
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
            if (const_in_range(ctx, pat, scrut_val))
              arm_matched = true;
          } else {
            ConstValue pv =
                enum_ctx.idx ? db_const_eval_with_enum_ctx(
                                   s, ctx->file_local, pat, enum_ctx,
                                   SEMA_CONST_ANCHOR(ctx))
                             : db_const_eval(s, ctx->file_local, pat,
                                             SEMA_CONST_ANCHOR(ctx));
            if (pv.kind != CONST_NONE && const_val_eq(pv, scrut_val))
              arm_matched = true;
          }
          syntax_node_release(pat);
        }
        syntax_node_release(pel.node);
      } else {
        if (body)
          syntax_node_release(body);
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

  if (!matched || !winner_body) {
    if (winner_body) syntax_node_release(winner_body);
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "comptime switch: no arm matches scrutinee value");
    return IP_ERROR_TYPE;
  }

  IpIndex result;
  if (expected.v != IP_NONE.v) {
    (void)check_expr(ctx, winner_body, expected);
    result = expected;
  } else {
    result = type_of_expr(ctx, winner_body);
  }
  syntax_node_release(winner_body);
  return result;
}

// `comptime switch (scrut) { arms }` — strict form. Scrutinee MUST
// fold; errors if it doesn't. On success delegates to infer_switch_folded.
//
// Plain `switch (scrut)` whose scrutinee happens to fold goes through
// infer_switch's own foldability probe (Path A — constant folding is
// implicit). The explicit `comptime` keyword on switch is a strictness
// marker: "ERROR if I can't be folded."
static IpIndex infer_comptime_switch(const SemaCtx *ctx, SyntaxNode *node,
                                     IpIndex expected) {
  struct db *s = ctx->s;
  SwitchExpr sw;
  if (!SwitchExpr_cast(node, &sw))
    return IP_NONE;
  SyntaxNode *scrutinee = SwitchExpr_scrutinee(&sw);
  if (!scrutinee) {
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "comptime switch requires a scrutinee");
    return IP_ERROR_TYPE;
  }
  // Visit so the unused-decl tracker sees the scrutinee's name refs —
  // the fold path bypasses sema visit machinery. Result discarded.
  (void)type_of_expr(ctx, scrutinee);
  ConstValue scrut_val = db_const_eval(s, ctx->file_local, scrutinee,
                                       SEMA_CONST_ANCHOR(ctx));
  syntax_node_release(scrutinee);
  if (scrut_val.kind == CONST_NONE) {
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "comptime switch scrutinee must be comptime-known");
    return IP_ERROR_TYPE;
  }
  DefId enum_ctx = {0};
  if (scrut_val.kind == CONST_ENUM_VARIANT)
    enum_ctx = scrut_val.enum_variant.enum_def;
  return infer_switch_folded(ctx, node, expected, scrut_val, enum_ctx);
}

// Single dispatch surface for `comptime <expr>` — owns the comptime
// fast-path for if/switch; transparent default for the foldable-value
// case (`comptime 1 + 2`). Future kinds (block, ref, fn-call) add arms
// here, never modify the runtime arms.
static IpIndex sema_comptime_select(const SemaCtx *ctx, SyntaxNode *child,
                                    IpIndex expected) {
  assert(child && "sema_comptime_select: SK_COMPTIME_EXPR must have a child");
  SyntaxKind k = syntax_node_kind(child);
  switch (k) {
  case SK_IF_EXPR:
    return infer_comptime_if(ctx, child, expected);
  case SK_SWITCH_EXPR:
    return infer_comptime_switch(ctx, child, expected);
  default:
    // `comptime <expr>` forces a compile-time fold; type follows the
    // wrapped expression. Const-fold check exists so a non-foldable
    // expression diags loudly (e.g., `comptime runtime_var`).
    {
      ConstValue cv = db_const_eval(ctx->s, ctx->file_local, child,
                                    SEMA_CONST_ANCHOR(ctx));
      if (cv.kind == CONST_NONE) {
        db_emit(ctx->s, DIAG_ERROR, span_of(ctx, child),
                "comptime expression must be comptime-foldable");
        return IP_ERROR_TYPE;
      }
      return expected.v != IP_NONE.v ? (check_expr(ctx, child, expected),
                                        expected)
                                     : type_of_expr(ctx, child);
    }
  }
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
        bool has_broadcast = false;
        if (init_list) {
          uint32_t total = syntax_node_num_children(init_list);
          for (uint32_t i = 0; i < total; i++) {
            SyntaxElement el = syntax_node_child_or_token(init_list, i);
            if (el.kind == SYNTAX_ELEM_NODE && el.node) {
              if (syntax_node_kind(el.node) == SK_INIT_FIELD) {
                count++;
                // Array-init §C — peek for the broadcast marker. `[_]` +
                // `x...` is cyclic (size depends on type-demanded fill;
                // fill depends on the inferred size).
                uint32_t fcc =
                    green_node_num_children(syntax_node_green(el.node));
                for (uint32_t j = 0; j < fcc; j++) {
                  GreenElement g =
                      green_node_child(syntax_node_green(el.node), j);
                  if (g.kind == GREEN_ELEM_TOKEN &&
                      green_token_kind(g.token) == SK_DOT_DOT_DOT) {
                    has_broadcast = true;
                    break;
                  }
                }
              }
              syntax_node_release(el.node);
            } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
              syntax_token_release(el.token);
            }
          }
        }
        if (has_broadcast) {
          db_emit(s, DIAG_ERROR, span_of(ctx, pty),
                  "cannot infer array size from a broadcast initializer; "
                  "use an explicit size in '[...]' or remove the '...'");
          return IP_ERROR_TYPE;
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
  if (!ip_index_is_valid(expected) || ip_is_error(expected)) {
    // Best-effort type each value so the node-type map still gets entries
    // AND the values surface their own internal errors (DC8). When
    // expected is IP_NONE the lack of context is already a real diag at
    // the call site; when expected is IP_ERROR_TYPE the root diag fired
    // upstream — either way, no fresh "not constructible" cascade.
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

    // Array-init §B/§C — pre-scan the init list to dispatch the
    // empty-default + broadcast cases BEFORE the per-element walk.
    // We count SK_INIT_FIELDs and also detect whether any of them
    // carries a postfix SK_DOT_DOT_DOT broadcast marker.
    uint32_t pre_count = 0;
    uint32_t broadcast_count = 0;
    SyntaxNode *broadcast_field = NULL;
    {
      uint32_t total_children = syntax_node_num_children(init_list);
      for (uint32_t i = 0; i < total_children; i++) {
        SyntaxElement el = syntax_node_child_or_token(init_list, i);
        if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
          if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
            syntax_token_release(el.token);
          continue;
        }
        if (syntax_node_kind(el.node) == SK_INIT_FIELD) {
          pre_count++;
          // Detect the broadcast marker: any SK_DOT_DOT_DOT child token
          // on this SK_INIT_FIELD makes it a broadcast initializer.
          uint32_t fcc = green_node_num_children(syntax_node_green(el.node));
          bool is_bcast = false;
          for (uint32_t j = 0; j < fcc; j++) {
            GreenElement g =
                green_node_child(syntax_node_green(el.node), j);
            if (g.kind == GREEN_ELEM_TOKEN &&
                green_token_kind(g.token) == SK_DOT_DOT_DOT) {
              is_bcast = true;
              break;
            }
          }
          if (is_bcast) {
            broadcast_count++;
            if (!broadcast_field) {
              broadcast_field = el.node; // keep this ref alive
              continue;                  // skip the release below
            }
          }
        }
        syntax_node_release(el.node);
      }
    }

    // §C validation: a broadcast field MUST be the only field. Mixing
    // broadcast and exact-elements `{a, b...}` is a parse-time-shape
    // error (caught at sema for now since the parser doesn't
    // currently reject it).
    if (broadcast_count > 0 && pre_count > 1) {
      db_emit(s, DIAG_ERROR, span_of(ctx, init_list),
              "broadcast '...' must be the sole element of an init list");
      if (broadcast_field) syntax_node_release(broadcast_field);
      return false;
    }

    // §B — `[N]T{}` empty-literal default fill.
    if (pre_count == 0) {
      IpIndex def = ip_default_value(s, elem);
      if (def.v == IP_NONE.v) {
        db_emit(s, DIAG_ERROR, span_of(ctx, init_list),
                "array element type %T has no default; "
                "{} init-shorthand requires a defaultable element type",
                elem);
        return false;
      }
      // Sema accepts. Codegen lowers via ip_default_value at the
      // element's tag.
      return true;
    }

    // §C — `[N]T{ x... }` broadcast fill.
    if (broadcast_count == 1) {
      // Extract the broadcast value: the first non-SK_DOT_DOT_DOT child
      // node of the broadcast SK_INIT_FIELD (skipping any leading `.name =`
      // tokens; named-fields-with-broadcast isn't meaningful so we don't
      // worry about that case here — the value walk picks the expression).
      SyntaxNode *bcast_value = NULL;
      uint32_t fcc = syntax_node_num_children(broadcast_field);
      for (uint32_t j = 0; j < fcc; j++) {
        SyntaxElement sub =
            syntax_node_child_or_token(broadcast_field, j);
        if (sub.kind == SYNTAX_ELEM_NODE && sub.node) {
          if (!bcast_value) {
            bcast_value = sub.node;
            continue;
          }
          syntax_node_release(sub.node);
        } else if (sub.kind == SYNTAX_ELEM_TOKEN && sub.token) {
          syntax_token_release(sub.token);
        }
      }
      bool ok = true;
      if (bcast_value) {
        if (!check_expr(ctx, bcast_value, elem))
          ok = false;
        syntax_node_release(bcast_value);
      }
      syntax_node_release(broadcast_field);
      return ok;
    }
    if (broadcast_field) syntax_node_release(broadcast_field);

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
              "array literal has %d element(s), expected %d. "
              "Hint: use {} for the type's default, or {x...} to "
              "broadcast one value.",
              (int32_t)count, (int32_t)declared_size);
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

// ============================================================================
// C1 — Binary-operator type rules, decomposed by operator family. SK_BIN_EXPR
// in type_of_expr_impl dispatches a single switch over `opk` into these.
// All helpers assume lt / rt are already non-NONE (caller checks).
// ============================================================================

static IpIndex binop_arith(const SemaCtx *ctx, SyntaxNode *node, SyntaxKind opk,
                           IpIndex lt, IpIndex rt) {
  struct db *s = ctx->s;
  // Many-pointer arithmetic: `[^]T + int`, `int + [^]T`, `[^]T - int`
  // all yield the many-pointer type. `[^]T - [^]T` yields usize
  // (element-count difference). `^T` and slice types are NOT
  // arithmetic — pointer arithmetic is many-pointer-specific.
  if (opk == SK_PLUS || opk == SK_MINUS) {
    IpTag lk = ip_tag(&s->intern, lt);
    IpTag rk = ip_tag(&s->intern, rt);
    bool l_mp =
        (lk == IP_TAG_MANY_PTR_TYPE || lk == IP_TAG_MANY_PTR_CONST_TYPE);
    bool r_mp =
        (rk == IP_TAG_MANY_PTR_TYPE || rk == IP_TAG_MANY_PTR_CONST_TYPE);
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
              "got %T and %T",
              lt, rt);
      return IP_ERROR_TYPE;
    }
  }
  IpIndex u = unify_arith(lt, rt);
  if (u.v == IP_NONE.v || !is_numeric(u)) {
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "cannot apply '%s' to operands of type %T and %T",
            opkind_name(opk), lt, rt);
    return IP_ERROR_TYPE;
  }
  return u;
}

static IpIndex binop_compare(const SemaCtx *ctx, SyntaxNode *node,
                             SyntaxKind opk, IpIndex lt, IpIndex rt) {
  struct db *s = ctx->s;
  // Pointer equality: `[^]T == [^]T`, `^T == ^T` (same intern → same .v).
  // Same-type accept is sufficient; cross-type ptr comparison isn't
  // supported.
  IpTag lk = ip_tag(&s->intern, lt);
  bool both_ptr = (lt.v == rt.v) &&
                  (lk == IP_TAG_PTR_TYPE || lk == IP_TAG_PTR_CONST_TYPE ||
                   lk == IP_TAG_MANY_PTR_TYPE ||
                   lk == IP_TAG_MANY_PTR_CONST_TYPE);
  if (both_ptr && (opk == SK_EQ_EQ || opk == SK_BANG_EQ))
    return IP_BOOL_TYPE;
  // nil ↔ optional: any `?T` admits `==` / `!=` against nil, yielding
  // bool. The coerce rule (coerce.c) already accepts `nil → ?T` in
  // assigns; this mirrors it in the comparison path so the strict-nil
  // model holds bidirectionally. Raw pointers / slices / many-pointers
  // are NOT eligible — §H established that nil only fits `?T`.
  if ((opk == SK_EQ_EQ || opk == SK_BANG_EQ) &&
      ((lt.v == IP_NIL_TYPE.v &&
        ip_tag(&s->intern, rt) == IP_TAG_OPTIONAL_TYPE) ||
       (rt.v == IP_NIL_TYPE.v &&
        ip_tag(&s->intern, lt) == IP_TAG_OPTIONAL_TYPE)))
    return IP_BOOL_TYPE;
  if (lt.v != rt.v && unify_arith(lt, rt).v == IP_NONE.v) {
    // W3 — pointer-vs-integer comparison: produce an actionable diag.
    // `^T == int_lit` (or `^T == nil` against a non-optional `^T`) is a
    // common typo where the user meant to deref-and-compare or expected
    // C-style 0/null-coercion. Two distinct fixes (deref OR make the
    // pointer type optional + use `nil`) — surface both. Generic diag
    // stays as the fall-through for everything else.
    if (opk == SK_EQ_EQ || opk == SK_BANG_EQ) {
      IpTag rk = ip_tag(&s->intern, rt);
      bool l_is_ptr = (lk == IP_TAG_PTR_TYPE || lk == IP_TAG_PTR_CONST_TYPE);
      bool r_is_ptr = (rk == IP_TAG_PTR_TYPE || rk == IP_TAG_PTR_CONST_TYPE);
      bool l_is_int = (lt.v == IP_COMPTIME_INT_TYPE.v) || is_concrete_int(lt);
      bool r_is_int = (rt.v == IP_COMPTIME_INT_TYPE.v) || is_concrete_int(rt);
      bool l_is_nil = lt.v == IP_NIL_TYPE.v;
      bool r_is_nil = rt.v == IP_NIL_TYPE.v;
      if ((l_is_ptr && (r_is_int || r_is_nil)) ||
          (r_is_ptr && (l_is_int || l_is_nil))) {
        IpIndex ptr_ty = l_is_ptr ? lt : rt;
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "cannot compare pointer type %T to %T. To compare the "
                "pointed-to value, dereference the pointer (e.g. "
                "`p^ == 0`). For a null check, the pointer type must "
                "be optional (e.g. '?%T') and the comparand must be 'nil'.",
                ptr_ty, l_is_ptr ? rt : lt, ptr_ty);
        return IP_ERROR_TYPE;
      }
    }
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "cannot apply '%s' to operands of type %T and %T",
            opkind_name(opk), lt, rt);
    return IP_ERROR_TYPE;
  }
  return IP_BOOL_TYPE;
}

static IpIndex binop_logical(const SemaCtx *ctx, SyntaxNode *node,
                             SyntaxKind opk, IpIndex lt, IpIndex rt) {
  struct db *s = ctx->s;
  if (lt.v != IP_BOOL_TYPE.v || rt.v != IP_BOOL_TYPE.v) {
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "logical '%s' requires bool operands, got %T", opkind_name(opk),
            (lt.v != IP_BOOL_TYPE.v) ? lt : rt);
    return IP_ERROR_TYPE;
  }
  return IP_BOOL_TYPE;
}

static IpIndex binop_bitop(const SemaCtx *ctx, SyntaxNode *node, SyntaxKind opk,
                           IpIndex lt, IpIndex rt) {
  struct db *s = ctx->s;
  IpIndex u = unify_arith(lt, rt);
  bool lt_ok = (lt.v == IP_COMPTIME_INT_TYPE.v) || is_concrete_int(lt);
  bool rt_ok = (rt.v == IP_COMPTIME_INT_TYPE.v) || is_concrete_int(rt);
  if (u.v == IP_NONE.v || !lt_ok || !rt_ok) {
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "bitwise '%s' requires integer operands, got %T and %T",
            opkind_name(opk), lt, rt);
    return IP_ERROR_TYPE;
  }
  return u;
}

static IpIndex binop_orelse(const SemaCtx *ctx, SyntaxNode *node, IpIndex lt) {
  struct db *s = ctx->s;
  // `a orelse b`: a must be optional (?T); result is T (b — possibly
  // `noreturn` via break — is the fallback coerced to T).
  if (ip_tag(&s->intern, lt) == IP_TAG_OPTIONAL_TYPE)
    return ip_key(&s->intern, lt).optional_type.elem;
  db_emit(s, DIAG_ERROR, span_of(ctx, node),
          "'orelse' requires an optional left operand, got %T", lt);
  return IP_ERROR_TYPE;
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
    return visit_child(ctx, ParenExpr_inner(&pe));
  }

  case SK_BIN_EXPR: {
    BinExpr be;
    if (!BinExpr_cast(node, &be))
      return IP_NONE;
    SyntaxKind opk = BinExpr_op_kind(&be);

    // Bidirectional enum-variant comparison: `enum_val == .variant`
    // or `.variant == enum_val`. A bare SK_ENUM_REF_EXPR has no
    // type_of_expr arm (it requires an enum-typed context); without
    // this short-circuit, visit_child on the bare side returns an
    // ICE before binop_compare runs. Detect the shape and type the
    // bare side via check_expr against the typed side's type.
    //
    // Refcounts: BinExpr_lhs / _rhs return +1; visit_child releases;
    // check_expr does NOT release (callsite is responsible). Acquire both
    // operand nodes ONCE here — every exit below releases each exactly once
    // (mirrors const_eval.c eval_bin, which solves this same enum-variant
    // comparison). The old shape re-acquired per path (here AND again at the
    // normal-path visit_child calls), leaking the first pair whenever the
    // EQ_EQ/BANG_EQ enum branch was not taken — i.e. on every ordinary
    // `p == 0` / `b == 0`.
    SyntaxNode *lhs_n = BinExpr_lhs(&be);
    SyntaxNode *rhs_n = BinExpr_rhs(&be);

    // Bidirectional enum-variant comparison: `enum_val == .variant` or
    // `.variant == enum_val`. A bare SK_ENUM_REF_EXPR has no type_of_expr
    // arm (it requires an enum-typed context); detect the shape and type the
    // bare side via check_expr against the typed side's enum type.
    if (opk == SK_EQ_EQ || opk == SK_BANG_EQ) {
      bool lhs_bare = lhs_n && syntax_node_kind(lhs_n) == SK_ENUM_REF_EXPR;
      bool rhs_bare = rhs_n && syntax_node_kind(rhs_n) == SK_ENUM_REF_EXPR;
      if (lhs_bare != rhs_bare) { // exactly one side is bare
        SyntaxNode *typed_n = lhs_bare ? rhs_n : lhs_n;
        SyntaxNode *bare_n  = lhs_bare ? lhs_n : rhs_n;
        IpIndex other = typed_n ? type_of_expr(ctx, typed_n) : IP_NONE;
        IpIndex result;
        if (ip_is_error(other)) {
          result = IP_ERROR_TYPE;
        } else if (other.v == IP_NONE.v ||
                   ip_tag(&s->intern, other) != IP_TAG_ENUM_TYPE) {
          result = IP_BOOL_TYPE;
        } else {
          (void)check_expr(ctx, bare_n, other); // does NOT release bare_n
          result = IP_BOOL_TYPE;
        }
        // lhs_n + rhs_n ARE typed_n + bare_n — release each exactly once.
        if (lhs_n) syntax_node_release(lhs_n);
        if (rhs_n) syntax_node_release(rhs_n);
        return result;
      }
    }

    IpIndex lt = visit_child(ctx, lhs_n); // releases lhs_n
    IpIndex rt = visit_child(ctx, rhs_n); // releases rhs_n
    if (ip_is_error(lt) || ip_is_error(rt))
      return IP_ERROR_TYPE;
    if (lt.v == IP_NONE.v || rt.v == IP_NONE.v)
      return IP_NONE;
    switch (opk) {
    case SK_PLUS: case SK_MINUS: case SK_STAR:
    case SK_SLASH: case SK_PERCENT: case SK_STAR_STAR:
      return binop_arith(ctx, node, opk, lt, rt);
    case SK_EQ_EQ: case SK_BANG_EQ: case SK_LT:
    case SK_LE: case SK_GT: case SK_GE:
      return binop_compare(ctx, node, opk, lt, rt);
    case SK_AMP_AMP: case SK_PIPE_PIPE:
      return binop_logical(ctx, node, opk, lt, rt);
    case SK_AMP: case SK_PIPE: case SK_CARET:
    case SK_SHL: case SK_SHR:
      return binop_bitop(ctx, node, opk, lt, rt);
    case SK_ORELSE_KW:
      return binop_orelse(ctx, node, lt);
    case SK_DOT_DOT_LT:
    case SK_DOT_DOT_EQ:
      // Range expressions (`..<`/`..=`) carry a type only inside a loop
      // header or a switch pattern (both handled before this generic binop
      // dispatch). Anywhere else there is no Range value type yet — surfaces
      // as a clean error rather than a silent fold.
      db_emit(s, DIAG_ERROR, span_of(ctx, node),
              "range expressions are only allowed in a loop header "
              "(`loop (lo..<hi) <i>`) or a switch pattern; stored Range "
              "values are not supported yet");
      return IP_ERROR_TYPE;
    default:
      db_emit(s, DIAG_ERROR, span_of(ctx, node),
              "binary operator '%s' not yet supported in type inference",
              opkind_name(opk));
      return IP_ERROR_TYPE;
    }
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
        return IP_ERROR_TYPE;
      }
      IpIndex t = type_of_expr(ctx, operand);
      syntax_node_release(operand);
      if (ip_is_error(t))
        return IP_ERROR_TYPE;
      if (t.v == IP_NONE.v)
        return IP_NONE;
      return ip_get(&s->intern,
                    (IpKey){.kind = IPK_PTR_TYPE,
                            .ptr_type = {.elem = t, .is_const = false}});
    }
    IpIndex t = type_of_expr(ctx, operand);
    syntax_node_release(operand);
    if (ip_is_error(t))
      return IP_ERROR_TYPE;
    if (t.v == IP_NONE.v)
      return IP_NONE;
    if (opk == SK_MINUS) {
      if (!is_numeric(t)) {
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "unary '-' requires numeric operand, got %T", t);
        return IP_ERROR_TYPE;
      }
      return t;
    }
    if (opk == SK_TILDE) {
      if (t.v != IP_COMPTIME_INT_TYPE.v && !is_concrete_int(t)) {
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "unary '~' requires integer operand, got %T", t);
        return IP_ERROR_TYPE;
      }
      return t;
    }
    if (opk == SK_BANG) {
      if (t.v != IP_BOOL_TYPE.v) {
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "unary '!' requires bool, got %T", t);
        return IP_ERROR_TYPE;
      }
      return IP_BOOL_TYPE;
    }
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "prefix operator '%s' not yet supported in type inference",
            opkind_name(opk));
    return IP_ERROR_TYPE;
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
    if (ip_is_error(t))
      return IP_ERROR_TYPE;
    if (t.v == IP_NONE.v)
      return IP_NONE;
    if (opk == SK_CARET) {
      IpTag tag = ip_tag(&s->intern, t);
      if (tag != IP_TAG_PTR_TYPE && tag != IP_TAG_PTR_CONST_TYPE) {
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "cannot dereference non-pointer type %T", t);
        return IP_ERROR_TYPE;
      }
      return ip_key(&s->intern, t).ptr_type.elem;
    }
    if (opk == SK_QUESTION) {
      if (ip_tag(&s->intern, t) != IP_TAG_OPTIONAL_TYPE) {
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "'.?' requires optional type, got %T", t);
        return IP_ERROR_TYPE;
      }
      return ip_key(&s->intern, t).optional_type.elem;
    }
    return t; // ++ / -- : type as operand (statement-like)
  }

  case SK_CALL_EXPR: {
    CallExpr ce;
    if (!CallExpr_cast(node, &ce))
      return IP_NONE;
    // Slice 6.12 — loose-node call forms: `with EXPR` (WITH_KW marker; may
    // carry a leading SK_PARAM binder `with x := HEAD`) and action-first
    // `handle (action) <E> {…}` (HANDLE_KW marker; the action's SK_ARG_LIST is
    // BEFORE the handler). For both, the head is CallExpr_head — NOT nth-node(0),
    // which would mis-pick the binder / arg-list.
    bool is_with_desugar = CallExpr_is_with(&ce);
    SyntaxNode *callee = (is_with_desugar || CallExpr_is_handle(&ce))
                             ? CallExpr_head(&ce)
                             : CallExpr_callee(&ce);
    SyntaxNode *arg_list = CallExpr_args(&ce);

    // If the head is itself a SK_CALL_EXPR, FLATTEN per Koka's
    // `applyToContinuation` ([koka/src/Syntax/Parse.hs:1655-1665](koka/src/Syntax/Parse.hs#L1655)):
    // `with f(a, b) body` → `f(a, b, fn(){body})`, not the curried
    // `(f(a, b))(fn(){body})`. Other heads (handler value, ref, atom) use the
    // regular curried form below.
    if (is_with_desugar && callee &&
        syntax_node_kind(callee) == SK_CALL_EXPR) {
      CallExpr inner_ce;
      if (CallExpr_cast(callee, &inner_ce)) {
        SyntaxNode *inner_callee = CallExpr_callee(&inner_ce);
        SyntaxNode *inner_args = CallExpr_args(&inner_ce);
        IpIndex inner_callee_ty =
            inner_callee ? type_of_expr(ctx, inner_callee) : IP_NONE;
        if (inner_callee_ty.v != IP_NONE.v && !ip_is_error(inner_callee_ty) &&
            ip_tag(&s->intern, inner_callee_ty) == IP_TAG_FN_TYPE) {
          IpKey key = ip_key(&s->intern, inner_callee_ty);
          uint32_t n_in = 0, n_out = 0;
          SyntaxNode **in_args = collect_arg_nodes(s, inner_args, &n_in);
          SyntaxNode **out_args = collect_arg_nodes(s, arg_list, &n_out);
          if (inner_args)
            syntax_node_release(inner_args);
          if (arg_list)
            syntax_node_release(arg_list);
          if (inner_callee)
            syntax_node_release(inner_callee);
          if (callee)
            syntax_node_release(callee);
          uint32_t n_total = n_in + n_out;
          if (key.fn_type.n_params != n_total) {
            db_emit(s, DIAG_ERROR, span_of(ctx, node),
                    "with-call expects %d args, got %d (after flattening "
                    "trailing thunk)",
                    (int32_t)key.fn_type.n_params, (int32_t)n_total);
            for (uint32_t i = 0; i < n_in; i++)
              (void)type_of_expr(ctx, in_args[i]);
            for (uint32_t i = 0; i < n_out; i++)
              (void)type_of_expr(ctx, out_args[i]);
            release_arg_nodes(in_args, n_in);
            release_arg_nodes(out_args, n_out);
            return IP_ERROR_TYPE;
          }
          for (uint32_t i = 0; i < n_in; i++)
            (void)check_expr(ctx, in_args[i], key.fn_type.params[i]);
          for (uint32_t i = 0; i < n_out; i++) {
            // The trailing out_arg is the continuation thunk — flow its
            // inferred effect row through f's cont-param (Koka-faithful).
            if (i == n_out - 1)
              check_continuation_arg(ctx, out_args[i],
                                     key.fn_type.params[n_in + i]);
            else
              (void)check_expr(ctx, out_args[i], key.fn_type.params[n_in + i]);
          }
          release_arg_nodes(in_args, n_in);
          release_arg_nodes(out_args, n_out);
          // A poisoned effect-row slot (bad effect label, already diag'd)
          // must NOT reach row_union: union with a non-row folds the whole
          // accumulated body row to IP_NONE, which then fails the end-of-
          // body soundness gate with a spurious "declares X but performs Y".
          if (ctx->body_effect_row &&
              key.fn_type.effect_row.v != IP_NONE.v &&
              !ip_is_error(key.fn_type.effect_row)) {
            IpIndex merged = row_union(
                ctx, *ctx->body_effect_row, key.fn_type.effect_row, node);
            // Genuine merge failure (non-unifiable tails) — diag HERE at
            // the call, then sticky: the old silent IP_NONE store only
            // surfaced as a mis-located end-of-body soundness diag.
            if (merged.v == IP_NONE.v &&
                ctx->body_effect_row->v != IP_NONE.v) {
              db_emit(s, DIAG_ERROR, span_of(ctx, node),
                      "cannot combine effect rows %T and %T",
                      *ctx->body_effect_row, key.fn_type.effect_row);
              merged = IP_ERROR_TYPE;
            }
            *ctx->body_effect_row = merged;
          }
          return apply_type_subst(ctx, key.fn_type.ret);
        }
        if (inner_callee)
          syntax_node_release(inner_callee);
        if (inner_args)
          syntax_node_release(inner_args);
        // Fall through to normal call typing on the outer SK_CALL_EXPR —
        // gives a clean "not callable" diag on the inner-call's result.
      }
    }

    IpIndex callee_ty = callee ? type_of_expr(ctx, callee) : IP_NONE;

    // Slice 6.12 — `with EXPR` where EXPR is a handler value
    // (IP_TAG_HANDLER_TYPE). The call's result is normally the handler's
    // `.ret` type; when `.ret` is IP_NONE (the pass-through sentinel for a
    // handler without an explicit `return(x) val` clause — emits a
    // diagnostic if any ctl/final-ctl clauses were present) substitute the
    // ACTION's return type as a recovery default so consumers see SOMETHING
    // typed for the handle expression.
    if (callee_ty.v != IP_NONE.v && !ip_is_error(callee_ty) &&
        ip_tag(&s->intern, callee_ty) == IP_TAG_HANDLER_TYPE) {
      IpKey hk = ip_key(&s->intern, callee_ty);
      uint32_t n_args = 0;
      SyntaxNode **args = collect_arg_nodes(s, arg_list, &n_args);
      if (arg_list)
        syntax_node_release(arg_list);
      if (callee)
        syntax_node_release(callee);
      // args[0] is the continuation thunk holding `rest`. Type its BODY INLINE
      // (this fn's ctx) so `rest` is checked, isolating its accumulated effect
      // row via a snapshot so the handled effect can be discharged below. (The
      // loose binder `x` — the resume value — isn't derivable from
      // handler_type{.effect,.ret} yet, so it stays untyped: D2 follow-up.)
      // Remaining args typed for hover.
      IpIndex action_ret = IP_VOID_TYPE;
      IpIndex cont_row = IP_EMPTY_EFFECT_ROW;

      // Surface split (proven at parse time): `with` ⇒ args[0] is a synthetic
      // continuation SK_LAMBDA_EXPR that greedily captured the rest of this
      // block (so the with-call is always statement-tail, never an
      // expression); `handle` ⇒ args[0] is a bare action expression.
      LambdaExpr lam_chk;
      bool is_continuation =
          n_args > 0 && args[0] && LambdaExpr_cast(args[0], &lam_chk);

      // The handler relates action-result `a`, answer `b`, effect `l` (Koka:
      // return(x:a) body:b). `a` is IP_NONE without a `return(x:T)`
      // annotation, `b` is IP_NONE for a pass-through handler (rule now
      // requires `return(x:T) body` when any ctl/final-ctl present; diag fires
      // at SK_HANDLER_EXPR entry).
      //
      // Resolution of the missing `a` differs per surface:
      //   `with` (statement-tail) — the action IS the rest of the fn, so its
      //     return type EQUALS the enclosing fn ret; pre-seeding `a = fn_ret`
      //     lets `expected_ret_override` flow into the cont body BEFORE it
      //     types.
      //   `handle` (bare expression) — the action is an arbitrary expression
      //     whose result type is only known AFTER it types. Deferred.
      IpIndex fn_ret = fn_ret_of(ctx);
      IpIndex a;
      if (hk.handler_type.action.v != IP_NONE.v) {
        a = hk.handler_type.action;
      } else if (is_continuation) {
        a = fn_ret;
      } else {
        a = IP_NONE; // resolved post-action typing below
      }
      IpIndex b = (hk.handler_type.ret.v == IP_NONE.v) ? a : hk.handler_type.ret;

      for (uint32_t i = 0; i < n_args; i++) {
        if (i == 0) {
          if (is_continuation) {
            // `with` — the continuation's `return X` must check X against the
            // action-result `a`, NOT the enclosing fn ret. Override only the
            // return-target on a shallow copy (shares body_effect_row /
            // row_subst pointers, so effect accumulation + the snapshot inside
            // type_action_isolated are unaffected).
            SemaCtx cc = *ctx;
            if (a.v != IP_NONE.v)
              cc.expected_ret_override = a;
            cont_row = type_action_isolated(&cc, args[0], &action_ret);
          } else {
            // `handle` — the action is a bare expression; its real result must
            // coerce to the handler's declared `a` (the return-clause param
            // type).
            cont_row = type_action_isolated(ctx, args[0], &action_ret);
            if (hk.handler_type.action.v != IP_NONE.v &&
                action_ret.v != IP_NONE.v)
              coerce_or_diag(ctx, args[0], action_ret, hk.handler_type.action);
            // The `return(x) body` clause is the SOLE source of a handler's
            // answer type. A missing/under-specified return clause (b stayed
            // IP_NONE) was already diagnosed at SK_HANDLER_EXPR (Part B.5's
            // "must declare an explicit 'return(x: T) body' clause"), so poison
            // the answer — consumers absorb IP_ERROR_TYPE per the poison
            // contract. NO implicit action→answer flow-through: the action's
            // result never silently becomes the handler's answer.
            if (b.v == IP_NONE.v)
              b = IP_ERROR_TYPE;
          }
        } else {
          (void)type_of_expr(ctx, args[i]); // hover
        }
      }

      // For a fn-tail TRANSFORMING `with` (a ≠ b, continuation diverges/
      // returns), the answer `b` flows to the enclosing fn ret. Gate on the
      // continuation always-terminating so a `with` nested in a non-tail branch
      // doesn't mis-coerce its answer against fn_ret (D-followup: thread an
      // expected-type-at-position signal for the nested case).
      if (is_continuation && b.v != IP_NONE.v && fn_ret.v != IP_NONE.v &&
          b.v != a.v) {
        SyntaxNode *cont_body = NULL;
        LambdaExpr lam_b;
        if (LambdaExpr_cast(args[0], &lam_b))
          cont_body = LambdaExpr_body(&lam_b);
        bool cont_terminates =
            cont_body && block_always_terminates(ctx, cont_body);
        if (cont_body)
          syntax_node_release(cont_body);
        if (cont_terminates)
          coerce_or_diag(ctx, node, b, fn_ret);
      }

      release_arg_nodes(args, n_args);

      // DISCHARGE — the handler removes its own effect labels from the
      // continuation's row (Koka's removeLocalEffect,
      // [koka/src/Type/Infer.hs:1071-1076](koka/src/Type/Infer.hs#L1071)); the
      // residual folds into the enclosing fn's row, so a handled effect no
      // longer escapes. The handler clauses' own effects already folded in when
      // their bodies typed.
      // A handler whose effect annotation failed to resolve (sticky error,
      // diag'd at the bad label) skips the discharge fold ENTIRELY: it
      // can't subtract what it failed to name, and folding the action's
      // undischarged row into the enclosing fn would fire a far-away
      // spurious "declares X but performs Y" downstream of the real error.
      if (ctx->body_effect_row && !ip_is_error(hk.handler_type.effect)) {
        IpIndex residual = cont_row;
        if (hk.handler_type.effect.v != IP_NONE.v) {
          IpIndex hflat = row_flatten(ctx, hk.handler_type.effect);
          if (ip_tag(&s->intern, hflat) == IP_TAG_EFFECT_ROW) {
            IpKey hek = ip_key(&s->intern, hflat);
            for (size_t i = 0; i < hek.effect_row.n_labels; i++)
              residual = row_without(ctx, residual, hek.effect_row.labels[i]);
          }
        }
        IpIndex merged =
            row_union(ctx, *ctx->body_effect_row, residual, node);
        // Merge failure → diag at the with/handle site, sticky store.
        if (merged.v == IP_NONE.v && ctx->body_effect_row->v != IP_NONE.v &&
            residual.v != IP_NONE.v) {
          db_emit(s, DIAG_ERROR, span_of(ctx, node),
                  "cannot combine effect rows %T and %T",
                  *ctx->body_effect_row, residual);
          merged = IP_ERROR_TYPE;
        }
        *ctx->body_effect_row = merged;
      }
      // `with` is statement-tail (consumed the rest of its block at parse time)
      // — return void so the block walker emits no "unused value" warning; its
      // answer already flowed to fn_ret above. `handle` is a real expression —
      // return its answer `b`.
      return is_continuation ? IP_VOID_TYPE : b;
    }

    // DC8 sibling-masking guard: even when the callee fails to type, we
    // still walk the args so their own internal errors (e.g. an arg-
    // local "type mismatch in `a + "string"`") surface. Matches the
    // SK_INDEX_EXPR precedent at infer.c:1642 ("typed for hover; result
    // unused"). Skipping arg typing here would hide independent root
    // errors under the callee's diag — the developer fixes the callee,
    // recompiles, and gets blindsided by errors they couldn't see.
    if (callee_ty.v == IP_NONE.v || ip_is_error(callee_ty) ||
        ip_tag(&s->intern, callee_ty) != IP_TAG_FN_TYPE) {
      // DC4: when callee is sticky error, suppress the "not callable"
      // cascade — the root diag was emitted at the callee's failure site.
      // IP_NONE is similarly silent (graceful: never typed yet / forward
      // ref). Only emit when callee_ty is a real non-fn value type.
      bool emitted_not_callable = false;
      if (callee_ty.v != IP_NONE.v && !ip_is_error(callee_ty)) {
        db_emit(s, DIAG_ERROR, span_of(ctx, callee ? callee : node),
                "value of type %T is not callable", callee_ty);
        emitted_not_callable = true;
      }
      if (callee)
        syntax_node_release(callee);
      uint32_t n_force = 0;
      SyntaxNode **force = collect_arg_nodes(s, arg_list, &n_force);
      for (uint32_t i = 0; i < n_force; i++)
        (void)type_of_expr(ctx, force[i]);
      release_arg_nodes(force, n_force);
      if (arg_list)
        syntax_node_release(arg_list);
      // Sticky if we emitted a diag OR the callee was already sticky.
      // Only return graceful IP_NONE for "IP_NONE in, IP_NONE out" —
      // the upstream is still discovering / no error has been diag'd.
      return (emitted_not_callable || ip_is_error(callee_ty))
                 ? IP_ERROR_TYPE
                 : IP_NONE;
    }
    // Effects-4.5b — call-site instantiation. Polymorphic top-level fns
    // need a fresh row var per call site (else `apply(io_action)` followed
    // by `apply(exn_action)` in one body would share state and reject the
    // second call). Skip when the callee is a body-scope local/param —
    // those refer to row vars in the ENCLOSING signature that we don't
    // want to copy. SK_REF_EXPR is the only callee shape that can hit a
    // body-scope binding (lambda calls, field expressions, etc. are
    // always non-local).
    bool instantiate_callee = true;
    // Monomorphization — also recover the callee's top-level DefId here (the
    // only place the callee SyntaxNode is still alive). A body-local callee
    // can't be a generic top-level def, so it both disables row-var
    // instantiation AND is skipped for monomorphization (callee_def stays
    // NONE).
    DefId callee_def = DEF_ID_NONE;
    if (callee && syntax_node_kind(callee) == SK_REF_EXPR &&
        ctx->enclosing_fn.idx != DEF_ID_NONE.idx) {
      SyntaxToken *name_tok = ast_first_token(callee, SK_IDENT);
      if (name_tok) {
        const char *txt = syntax_token_text(name_tok);
        uint32_t len = syntax_token_text_range(name_tok).length;
        StrId name = pool_intern(&s->strings, txt, len);
        syntax_token_release(name_tok);
        if (name.idx != 0) {
          SyntaxNodePtr bind =
              db_body_scope_lookup(s, ctx->enclosing_fn, callee, name);
          if (bind.kind != SYNTAX_KIND_NONE) {
            instantiate_callee = false;
          } else {
            // Top-level callee — resolve its DefId for the instance key
            // (mirror resolve_value_path's lookup).
            NamespaceScopes sc = db_query_namespace_scopes(s, ctx->nsid);
            if (sc.internal.idx != SCOPE_ID_NONE.idx)
              callee_def = db_query_resolve_ref(s, sc.internal, name);
          }
        }
      }
    }
    syntax_node_release(callee);
    IpIndex effective_callee_ty =
        instantiate_callee ? instantiate_fn_for_call_site(s, callee_ty)
                           : callee_ty;
    IpKey key = ip_key(&s->intern, effective_callee_ty);
    uint32_t n_args = 0;
    SyntaxNode **args = collect_arg_nodes(s, arg_list, &n_args);
    if (arg_list)
      syntax_node_release(arg_list);
    if (key.fn_type.n_params != n_args) {
      db_emit(s, DIAG_ERROR, span_of(ctx, node), "call expects %d args, got %d",
              (int32_t)key.fn_type.n_params, (int32_t)n_args);
      // DC8: type each arg via synth so internal arg errors still surface
      // (no params to check against — fall back to type_of_expr).
      for (uint32_t i = 0; i < n_args; i++)
        (void)type_of_expr(ctx, args[i]);
      release_arg_nodes(args, n_args);
      return IP_ERROR_TYPE;
    }
    // Monomorphization — decide BEFORE the coercion loop, while the
    // (freshened) signature holes are still unbound and thus detectable.
    bool callee_is_generic = callee_def.idx != DEF_ID_NONE.idx &&
                             !is_with_desugar &&
                             sig_has_unbound_hole(ctx, effective_callee_ty);
    for (uint32_t i = 0; i < n_args; i++) {
      // A with-call's LAST arg is the continuation thunk — flow its inferred
      // effect row through f's cont-param (Koka-faithful); others coerce normally.
      if (is_with_desugar && i == n_args - 1) {
        check_continuation_arg(ctx, args[i], key.fn_type.params[i]);
        continue;
      }
      IpIndex pty = key.fn_type.params[i];
      // Type-kind hole (`t: type` param): the arg is a TYPE EXPRESSION, not
      // a value. Resolve it as a type and bind the hole to the resolved
      // type DIRECTLY. The all-concrete-args loop below reads the recorded
      // node type to build the instance key, so push the resolved type at
      // the arg node too. A shared hole (later param re-uses this hole's
      // id) is rank-1 — only bind if currently unbound; the first arg wins.
      if (ip_tag(&s->intern, pty) == IP_TAG_TYPE_VAR) {
        IpKey pk = ip_key(&s->intern, pty);
        if (pk.type_var.kind == TYPE_VAR_TYPE) {
          IpIndex bound = resolve_type_expr(ctx, args[i]);
          if (bound.v != IP_NONE.v && !ip_is_error(bound)) {
            if (type_resolve(ctx, pty).v == pty.v)
              type_subst_bind(ctx, pk.type_var.id, bound);
            node_type_builder_push(ctx, args[i], bound);
          }
          continue;
        }
      }
      (void)check_expr(ctx, args[i], key.fn_type.params[i]);
    }
    // Monomorphization — if the callee is generic and every hole-filling arg
    // resolved to a concrete (hole-free, non-error) type, intern the
    // (def, concrete args) instance and demand its body re-check; its return
    // type replaces the generic one. Done while args[] is still alive.
    bool did_mono = false;
    IpIndex inst_ret = IP_NONE;
    if (callee_is_generic) {
      if (s->mono_depth >= ORE_MONO_DEPTH_LIMIT) {
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "monomorphization too deep (max %d)",
                (int32_t)ORE_MONO_DEPTH_LIMIT);
        did_mono = true;
        inst_ret = IP_ERROR_TYPE;
      } else {
        IpIndex *key_args =
            arena_alloc(&s->request_arena, n_args * sizeof(IpIndex));
        bool all_concrete = key_args != NULL;
        for (uint32_t i = 0; i < n_args && all_concrete; i++) {
          IpIndex pty = key.fn_type.params[i];
          if (ip_tag(&s->intern, pty) == IP_TAG_TYPE_VAR) {
            // Hole position — fill with the arg's concrete (resolved) type.
            // Read the type the check_expr loop already recorded for this arg
            // (occupied-bitset lookup, index-0 safe) instead of re-running
            // type_of_expr, which would re-emit an erroneous arg's diagnostic.
            IpIndex at = db_lookup_node_type(ctx, args[i]);
            if (at.v == IP_NONE.v)
              at = type_of_expr(ctx, args[i]); // not recorded — synthesize
            at = apply_type_subst(ctx, at);
            if (at.v == IP_NONE.v || ip_is_error(at) ||
                !type_is_concrete(ctx, at)) {
              all_concrete = false;
              break;
            }
            key_args[i] = at;
          } else {
            key_args[i] = pty; // declared concrete param type (canonicalizes)
          }
        }
        if (all_concrete) {
          IpIndex inst = ip_instance_intern(s, callee_def, key_args, n_args);
          inst_ret = db_query_infer_instance((db_query_ctx *)s, inst);
          did_mono = true;
        }
      }
    }
    release_arg_nodes(args, n_args);
    // Effects-4c — accumulate the callee's effect row (now with the cont-param
    // effect var bound to the continuation's inferred row, if f references it).
    // Skip a poisoned row slot (bad effect label, already diag'd): row_union
    // on a non-row folds the body row to IP_NONE → spurious soundness diag.
    if (ctx->body_effect_row &&
        key.fn_type.effect_row.v != IP_NONE.v &&
        !ip_is_error(key.fn_type.effect_row)) {
      IpIndex merged =
          row_union(ctx, *ctx->body_effect_row, key.fn_type.effect_row, node);
      // Merge failure → diag at the call, sticky store (see with-path twin).
      if (merged.v == IP_NONE.v && ctx->body_effect_row->v != IP_NONE.v) {
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "cannot combine effect rows %T and %T",
                *ctx->body_effect_row, key.fn_type.effect_row);
        merged = IP_ERROR_TYPE;
      }
      *ctx->body_effect_row = merged;
    }
    // Monomorphization — a duck-typed instance was demanded: use its
    // concrete return type (the body re-check resolved it against the real
    // arg types). IP_NONE means the instance was UNAVAILABLE (e.g. a generic
    // fn reached via a const alias, so no lambda to re-check) — fall through
    // to the Layer-2 parametric resolve rather than silently typing the call
    // as IP_NONE. IP_ERROR_TYPE (the deliberate too-deep bail) still
    // short-circuits.
    if (did_mono && inst_ret.v != IP_NONE.v)
      return inst_ret;
    return apply_type_subst(ctx, key.fn_type.ret);
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
    if (ip_is_error(recv))
      return IP_ERROR_TYPE;
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
      // Distinguishes "field doesn't exist" (IP_NONE → diag + propagate
      // error) from "field exists with sticky error type" (DC5 revised:
      // ft IS IP_ERROR_TYPE from build_struct_type → no diag, propagate).
      if (ip_is_error(ft))
        return IP_ERROR_TYPE;
      if (ft.v == IP_NONE.v) {
        db_emit(s, DIAG_ERROR, fspan, "no field '%S' in %T", fname, recv);
        return IP_ERROR_TYPE;
      }
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
      return IP_ERROR_TYPE;
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
      return IP_ERROR_TYPE;
    }
    case IP_TAG_ARRAY_TYPE:
      if (fname.idx == s->names.LEN.idx)
        return IP_USIZE_TYPE;
      db_emit(s, DIAG_ERROR, fspan, "no field '%S' on array (only '.len')",
              fname);
      return IP_ERROR_TYPE;
    case IP_TAG_NAMESPACE_TYPE: {
      NamespaceId ns = ip_key(&s->intern, recv).namespace_type.nsid;
      // Tracked wrappers fire NAMESPACE_TYPE internally to record the
      // dep — no manual db_query_namespace_type anchor needed. They
      // take db_query_ctx* (== struct db*); SemaCtx wraps that, so we
      // pass the inner `s`.
      uint32_t n = db_read_namespace_member_count(s, ns);
      for (uint32_t i = 0; i < n; i++) {
        DeclEntry m = db_read_namespace_member_at(s, ns, i);
        if (m.name.idx == fname.idx)
          return db_query_type_of_def(s, m.def);
      }
      db_emit(s, DIAG_ERROR, fspan, "no member '%S' in %T", fname, recv);
      return IP_ERROR_TYPE;
    }
    case IP_TAG_EFFECT_TYPE: {
      // Effects-4b — `allocator.malloc` style op lookup. The op's fn
      // type carries `<allocator>` in its effect_row (Phase 4a injects
      // the parent effect at type_of_def time), so an enclosing
      // SK_CALL_EXPR accumulates that effect via row_union without any
      // call-site special-casing.
      DefId d = {.idx = ip_key(&s->intern, recv).effect_type.zir_node_id};
      (void)db_query_type_of_def(s, d); // dep + ensure ops built
      IpIndex ft = db_effect_op_type(s, d, fname);
      if (ip_is_error(ft))
        return IP_ERROR_TYPE;
      if (ft.v == IP_NONE.v) {
        db_emit(s, DIAG_ERROR, fspan, "no op '%S' in effect %T", fname, recv);
        return IP_ERROR_TYPE;
      }
      return ft;
    }
    default:
      db_emit(s, DIAG_ERROR, fspan, "field access on non-aggregate type %T",
              recv);
      return IP_ERROR_TYPE;
    }
  }

  case SK_INDEX_EXPR: {
    IndexExpr ie;
    if (!IndexExpr_cast(node, &ie))
      return IP_NONE;
    SyntaxNode *base = IndexExpr_base(&ie);
    IpIndex obj = base ? type_of_expr(ctx, base) : IP_NONE;
    (void)visit_child(ctx, IndexExpr_index(&ie)); // index typed for hover; result unused
    DiagAnchor bspan = span_of(ctx, base ? base : node);
    if (base)
      syntax_node_release(base);
    if (ip_is_error(obj))
      return IP_ERROR_TYPE;
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
      return IP_ERROR_TYPE;
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
    if (ip_is_error(obj)) {
      if (base) syntax_node_release(base);
      if (lo) syntax_node_release(lo);
      if (hi) syntax_node_release(hi);
      return IP_ERROR_TYPE;
    }
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
      return IP_ERROR_TYPE;
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
    // Slice 6.14 Step 0 (Fix F) — prefer the override (set by op-clause /
    // nested-lambda body walks that aren't themselves a DefId) over the
    // enclosing-fn signature lookup. The override is the op's / lambda's
    // declared return type; falling back to enclosing_fn's signature
    // gives the wrong target ("expected void" for code inside an i32-op
    // clause whose outer handler-fn returns void).
    IpIndex ret_ty = ctx->expected_ret_override;
    if (ret_ty.v == IP_NONE.v && enclosing_fn.idx != DEF_ID_NONE.idx) {
      const FnSignature *sig = db_query_fn_signature(s, enclosing_fn);
      IpIndex sigty = sig ? sig->type : IP_NONE;
      if (sigty.v != IP_NONE.v && ip_tag(&s->intern, sigty) == IP_TAG_FN_TYPE)
        ret_ty = ip_key(&s->intern, sigty).fn_type.ret;
    }
    if (ret_ty.v != IP_NONE.v) {
      if (val) {
        // `return X;` — X must coerce to declared return. `return X`
        // in a void fn is rejected by check_expr's coerce diag
        // ("expected type 'void', found '%T'") without a special case.
        (void)check_expr(ctx, val, ret_ty);
      } else if (ret_ty.v != IP_VOID_TYPE.v && !ip_is_error(ret_ty)) {
        // `return;` (no payload) in a non-void fn — hard error.
        // Naked `return;` is a valid guard-clause idiom for void fns
        // but never legal when a value is required. Skipped when the
        // declared return type itself is sticky-error ("requires a value
        // (returns error)" would be cascade noise on a diag'd signature).
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "non-void function (returns %T) requires a value in "
                "`return`", ret_ty);
      }
    }
    if (val)
      syntax_node_release(val);
    return IP_NORETURN_TYPE;
  }

  case SK_BLOCK_STMT: {
    // Slice 5 — Zig-strict block:
    //   - Walk statements via type_of_expr (side effects: populate
    //     node-type-builder, emit discardedness warnings for unused
    //     non-void / non-noreturn results).
    //   - If labeled, push a LabelFrame so `break :label v` sites
    //     inside the block contribute their value types to the frame's
    //     peer-unified accumulator (see SK_BREAK_STMT case).
    //   - Result type: IP_VOID_TYPE for unlabeled blocks, or the
    //     accumulator (or IP_VOID_TYPE if no break-with-value fired)
    //     for labeled blocks.
    BlockStmt bs = {.syntax = node};

    StrId label_name = extract_label_name(s, node);
    struct LabelFrame frame = {
        .name = label_name,
        .result_accum = IP_NONE,
        .is_loop = false,
        .parent = ctx->label_scope,
    };
    if (label_name.idx != 0)
      ((SemaCtx *)ctx)->label_scope = &frame;

    SyntaxNode *stmts = BlockStmt_stmts(&bs);
    if (stmts) {
      uint32_t total = syntax_node_num_children(stmts);
      for (uint32_t i = 0; i < total; i++) {
        SyntaxElement el = syntax_node_child_or_token(stmts, i);
        if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
          if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
            syntax_token_release(el.token);
          continue;
        }
        SyntaxNode *stmt = el.node;
        IpIndex t = type_of_expr(ctx, stmt);
        if (t.v != IP_NONE.v && t.v != IP_VOID_TYPE.v &&
            t.v != IP_NORETURN_TYPE.v &&
            !kind_is_discard_construct(syntax_node_kind(stmt)))
          db_emit(s, DIAG_WARNING, span_of(ctx, stmt),
                  "unused value of type %T", t);
        syntax_node_release(stmt);
      }
      syntax_node_release(stmts);
    }

    IpIndex result = (label_name.idx != 0 &&
                      frame.result_accum.v != IP_NONE.v)
                         ? frame.result_accum
                         : IP_VOID_TYPE;
    // Zig: a block whose every path diverges (return / break-out /
    // unreachable) is `noreturn`, not `void` — so a diverging branch peers
    // away at an if/switch join (peer_unify absorbs noreturn). Only the
    // fall-through void case is overridden; a labeled break-with-value
    // already produced a real type above.
    if (result.v == IP_VOID_TYPE.v && block_always_terminates(ctx, node))
      result = IP_NORETURN_TYPE;
    if (label_name.idx != 0)
      ((SemaCtx *)ctx)->label_scope = frame.parent;
    return result;
  }

  // --- let-bind statement (body_scopes no longer types these) -------------
  case SK_BIND_DECL:
    return type_let_bind(ctx, node, k);

  // --- if (statement or value): cond + branches, synth ---------------------
  case SK_IF_EXPR: {
    IfExpr ie;
    if (!IfExpr_cast(node, &ie))
      return IP_NONE;
    SyntaxNode *cond = IfExpr_condition(&ie);
    SyntaxNode *capture = IfExpr_capture(&ie);
    SyntaxNode *then_b = IfExpr_then_branch(&ie);
    SyntaxNode *else_b = IfExpr_else_branch(&ie);
    bool had_else = (else_b != NULL);
    handle_if_cond(ctx, cond, capture);
    if (cond)    syntax_node_release(cond);
    if (capture) syntax_node_release(capture);
    IpIndex tt = then_b ? visit_child(ctx, then_b) : IP_VOID_TYPE;
    IpIndex et = had_else ? visit_child(ctx, else_b) : IP_VOID_TYPE;
    // Join the branches like switch arms do: peer_unify folds comptime↔
    // concrete + same-type AND absorbs a `noreturn` branch (Zig's peer
    // resolution filters noreturn), so `if c { return } else x:i32` yields
    // i32. A missing else → void (if-as-statement). A REAL branch mismatch
    // (both branches typed, neither poisoned, no unification) used to
    // silently yield void — `x := if c 1 else "s"` typed void with zero
    // diags (the acknowledged Slice-5 hole). Now loud; poisoned/none
    // branches stay quiet (their producer already diag'd / lost the type).
    IpIndex u = had_else ? peer_unify(tt, et) : IP_NONE;
    if (had_else && u.v == IP_NONE.v && tt.v != IP_NONE.v &&
        et.v != IP_NONE.v && tt.v != IP_VOID_TYPE.v && et.v != IP_VOID_TYPE.v) {
      db_emit(s, DIAG_ERROR, span_of(ctx, node),
              "if branches have incompatible types (%T and %T)", tt, et);
      return IP_ERROR_TYPE;
    }
    return (u.v != IP_NONE.v) ? u : IP_VOID_TYPE;
  }

  // --- loop: cond (optional) + capture (optional) + body. Yields void.
  //
  // Capture handling depends on the cond's shape:
  //   - cond is a range expression (SK_BIN_EXPR with a `..<`/`..=` op):
  //     bind the capture to the range's element type (the unified type
  //     of lo and hi).
  //   - cond is an optional type: bind the capture to the unwrapped
  //     element (while-let semantics).
  //   - cond is bool (no capture): plain while-loop.
  //
  // No special IPK_RANGE_TYPE intern-pool tag yet — ranges are only
  // valid INLINE inside the loop header. Stored Range values await a
  // future feature.
  case SK_LOOP_EXPR: {
    LoopExpr le;
    if (!LoopExpr_cast(node, &le))
      return IP_VOID_TYPE;
    SyntaxNode *cond    = LoopExpr_condition(&le);
    SyntaxNode *capture = LoopExpr_capture(&le);
    SyntaxNode *body    = LoopExpr_body(&le);

    bool cond_present = (cond != NULL);
    if (cond) {
      bool is_range = false;
      IpIndex elem = IP_NONE;
      if (syntax_node_kind(cond) == SK_BIN_EXPR) {
        BinExpr be;
        if (BinExpr_cast(cond, &be) &&
            (BinExpr_op_kind(&be) == SK_DOT_DOT_LT ||
             BinExpr_op_kind(&be) == SK_DOT_DOT_EQ)) {
          is_range = true;
          IpIndex lt = visit_child(ctx, BinExpr_lhs(&be));
          IpIndex rt = visit_child(ctx, BinExpr_rhs(&be));
          if (ip_is_error(lt) || ip_is_error(rt)) {
            elem = IP_ERROR_TYPE; // sticky — bounds already diagnosed
          } else {
            IpIndex u = unify_arith(lt, rt);
            if (u.v == IP_NONE.v || !is_numeric(u)) {
              db_emit(s, DIAG_ERROR, span_of(ctx, cond),
                      "range bounds must be a common numeric type, got %T and %T",
                      lt, rt);
            } else {
              elem = u;
            }
          }
          node_type_builder_push(ctx, cond, elem);
        }
      }
      if (!is_range) {
        IpIndex ct = type_of_expr(ctx, cond);
        if (capture) {
          // while-let: cond must be optional.
          if (ip_is_error(ct)) {
            elem = IP_ERROR_TYPE; // sticky — silently propagate to capture
          } else if (ct.v != IP_NONE.v &&
              ip_tag(&s->intern, ct) == IP_TAG_OPTIONAL_TYPE) {
            elem = ip_key(&s->intern, ct).optional_type.elem;
          } else if (ct.v != IP_NONE.v) {
            db_emit(s, DIAG_ERROR, span_of(ctx, capture),
                    "loop capture requires a range or optional condition; "
                    "got %T", ct);
          }
        } else {
          // No capture — must be bool.
          (void)check_expr(ctx, cond, IP_BOOL_TYPE);
        }
      }
      if (capture) node_type_builder_push(ctx, capture, elem);
      syntax_node_release(cond);
    }
    // --- loop-as-expression (Zig's while): the loop's value is the peer
    // resolution of every `break <value>` operand plus the `else` value
    // (the normal-exit / cond-false value).
    SyntaxNode *else_b = LoopExpr_else_branch(&le);
    bool has_else = (else_b != NULL);

    // Push a loop frame so break-values — bare (this loop) or `:label` —
    // accumulate into result_accum.
    struct LabelFrame frame = {
        .name = extract_label_name(s, node),
        .result_accum = IP_NONE,
        .is_loop = true,
        .parent = ctx->label_scope,
    };
    ((SemaCtx *)ctx)->label_scope = &frame;

    // continue-expr `: (step)` — runs each iteration in loop scope; value
    // discarded. body_scopes now scopes its sub-tree, so type it to catch a
    // malformed step.
    SyntaxNode *cont = LoopExpr_continue(&le);
    if (cont) {
      SyntaxNode *step = ast_nth_node(cont, 0);
      if (step) {
        (void)type_of_expr(ctx, step);
        syntax_node_release(step);
      }
      syntax_node_release(cont);
    }

    if (body)
      (void)type_of_expr(ctx, body); // breaks inside fold into result_accum

    ((SemaCtx *)ctx)->label_scope = frame.parent; // pop before `else`
    IpIndex break_accum = frame.result_accum;

    // `else` — the normal-exit value, typed OUTSIDE the loop frame so a
    // break in it targets the enclosing loop (Zig). A block else `{…}` is
    // void unless it diverges; a bare-expr else `else 0` yields its value —
    // identical to if/else branch semantics.
    IpIndex else_ty = IP_NONE;
    if (else_b) {
      else_ty = type_of_expr(ctx, else_b);
      syntax_node_release(else_b);
    }

    IpIndex result;
    if (!cond_present) {
      // Infinite loop — no normal exit. Value comes only from breaks; with
      // none, it diverges (runs forever / returns / panics) → noreturn, so a
      // non-void fn ending in `loop { return X }` still typechecks.
      if (break_accum.v != IP_NONE.v)
        result = break_accum;
      else if (body && loop_has_reachable_break(body))
        result = IP_VOID_TYPE; // valueless `break`
      else
        result = IP_NORETURN_TYPE;
    } else {
      // Conditional loop — normal exit (cond false) yields the else value,
      // else void; peer that with the break-values.
      IpIndex normal_exit = has_else ? else_ty : IP_VOID_TYPE;
      if (break_accum.v == IP_NONE.v) {
        result = normal_exit;
      } else {
        IpIndex u = peer_unify(break_accum, normal_exit);
        if (u.v != IP_NONE.v) {
          result = u;
        } else if (has_else) {
          db_emit(s, DIAG_ERROR, span_of(ctx, node),
                  "loop break value %T does not unify with else value %T",
                  break_accum, else_ty);
          result = IP_ERROR_TYPE; // sticky — suppress downstream cascade
        } else {
          db_emit(s, DIAG_ERROR, span_of(ctx, node),
                  "loop yields %T via break but falls through as void; "
                  "add an `else` branch",
                  break_accum);
          result = IP_ERROR_TYPE;
        }
      }
    }
    if (capture) syntax_node_release(capture);
    if (body) syntax_node_release(body);
    return result;
  }

  // --- assignment: rhs must coerce to lhs; yields void ---------------------
  case SK_ASSIGN_EXPR: {
    AssignExpr ae;
    if (!AssignExpr_cast(node, &ae))
      return IP_NONE;
    SyntaxNode *lhs = AssignExpr_lhs(&ae), *rhs = AssignExpr_rhs(&ae);
    IpIndex lt = lhs ? type_of_expr(ctx, lhs) : IP_NONE;
    if (rhs) {
      // `_ = expr` (and any other no-expected-type LHS) must still TYPE the
      // RHS: check_expr's IP_NONE gate absorbs without walking, which used
      // to skip the whole RHS — `_ = getx(v)` never demanded the instance,
      // never recorded getx's reference (false "unused"), and swallowed
      // every error inside the call. Discard means "evaluate and ignore",
      // not "don't check" — synthesize when there's no target type.
      if (lt.v == IP_NONE.v)
        (void)type_of_expr(ctx, rhs);
      else
        (void)check_expr(ctx, rhs, lt);
    }
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

  // break / continue transfer control.
  // Slice 5B: `break :label v` contributes v's type to the labeled block's
  // result accumulator. Continue never takes a value. break with no label
  // OR no value works as before (control-only transfer).
  case SK_BREAK_STMT: {
    StrId label_name = extract_label_name(s, node);
    SyntaxNode *value = break_value_expr(node);
    if (value) {
      IpIndex vt = type_of_expr(ctx, value);
      syntax_node_release(value);
      // Target: a bare `break v` exits the innermost loop; `break :l v` the
      // frame named `l` (loop or labeled block). Both peer-fold into the
      // frame's result accumulator (Zig-style break-value resolution).
      struct LabelFrame *frame =
          (label_name.idx == 0) ? find_innermost_loop_frame(ctx)
                                : find_label_frame(ctx, label_name);
      if (!frame) {
        if (label_name.idx == 0)
          db_emit(s, DIAG_ERROR, span_of(ctx, node),
                  "break with a value must be inside a loop");
        else
          db_emit(s, DIAG_ERROR, span_of(ctx, node),
                  "label '%S' is not in scope", label_name);
      } else if (frame->result_accum.v == IP_NONE.v) {
        frame->result_accum = vt;
      } else {
        IpIndex u = peer_unify(frame->result_accum, vt);
        if (u.v != IP_NONE.v) {
          frame->result_accum = u;
        } else {
          db_emit(s, DIAG_ERROR, span_of(ctx, node),
                  "break value type %T does not unify with prior %T", vt,
                  frame->result_accum);
        }
      }
    }
    return IP_NORETURN_TYPE;
  }
  case SK_CONTINUE_STMT:
    return IP_NORETURN_TYPE;

  // --- switch (synth) ------------------------------------------------------
  case SK_SWITCH_EXPR:
    return infer_switch(ctx, node, IP_NONE);

  // Effects-4e — bare `handler { ... }` value. Types as IPK_HANDLER_TYPE.
  // Clause bodies type in the current (outer) ctx so their effects bubble
  // up to the surrounding fn; effect discharge happens at the eventual
  // call (with-handler) site, not here.
  case SK_HANDLER_EXPR: {
    HandlerExpr hr;
    if (!HandlerExpr_cast(node, &hr))
      return IP_NONE;
    // Handlers with any ctl/final-ctl clause MUST declare an explicit
    // `return(x: T) body` clause. With no return clause, IP_NONE means
    // "no answer-type was given" — we emit a diagnostic (below, after the
    // return-clause walk) instead of silently propagating a default. The
    // ret_ty stays IP_NONE in recovery, which keeps op-clause bodies in
    // "synth-only" mode (no enforcement) just for the duration of the
    // error-recovery path. Explicit `return(x) void` remains
    // distinguishable as IP_VOID_TYPE.
    IpIndex ret_ty = IP_NONE;     // b — answer (return-clause body type)
    IpIndex action_ty = IP_NONE;  // a — action result (return(x: T)'s T)
    IpIndex eff_row = IP_EMPTY_EFFECT_ROW;
    // Read the optional handler-level effect row annotation.
    SyntaxNode *eff_node = HandlerExpr_effect(&hr);
    if (eff_node) {
      eff_row = build_effect_row(ctx, eff_node);
      if (eff_row.v == IP_NONE.v)
        eff_row = IP_EMPTY_EFFECT_ROW;
      syntax_node_release(eff_node);
    }
    // Walk clauses to find the `return` clause — the only clause kind that
    // contributes to the handler's type (its body type IS the handler's ret
    // type). Op clauses are SK_BIND_DECL binds, typed via the normal decl
    // path, not here. Slice 3 routed clause parsing through `parse_block`, so
    // clauses live inside an SK_BLOCK_STMT > SK_STMT_LIST under
    // SK_HANDLER_EXPR — descend through that wrapper first.
    SyntaxNode *clause_block = ast_first_child(node, SK_BLOCK_STMT);
    SyntaxNode *clause_list = NULL;
    SyntaxNode *iter_parent = node;
    if (clause_block) {
      clause_list = ast_first_child(clause_block, SK_STMT_LIST);
      if (clause_list)
        iter_parent = clause_list;
    }
    uint32_t nch = syntax_node_num_children(iter_parent);
    bool saw_return_clause = false; // an explicit `return(x) body` clause exists
    for (uint32_t i = 0; i < nch; i++) {
      SyntaxElement el = syntax_node_child_or_token(iter_parent, i);
      if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
        if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
          syntax_token_release(el.token);
        continue;
      }
      if (syntax_node_kind(el.node) == SK_RETURN_CLAUSE) {
        saw_return_clause = true; // present regardless of its body's type
        // `a` — the action's result type — comes from the `return(x: T)`
        // annotation (ore has no type-var inference, so it must be declared).
        // Push it at the param node so `x` resolves+types in the body. No
        // annotation ⇒ `a` stays IP_NONE (identity pass-through, `x` untyped).
        SyntaxNode *plist = ast_first_child(el.node, SK_PARAM_LIST);
        if (plist) {
          SyntaxNode *param = ast_first_child(plist, SK_PARAM);
          if (param) {
            Param pp;
            if (Param_cast(param, &pp)) {
              SyntaxNode *ann = Param_type(&pp);
              if (ann) {
                action_ty = resolve_type_expr(ctx, ann);
                syntax_node_release(ann);
              }
            }
            if (action_ty.v != IP_NONE.v)
              node_type_builder_push(ctx, param, action_ty);
            syntax_node_release(param);
          }
          syntax_node_release(plist);
        }
        // The clause body is the last non-token expression child; its type
        // becomes the handler's answer type `b`.
        uint32_t cn = syntax_node_num_children(el.node);
        SyntaxNode *body = NULL;
        for (uint32_t j = cn; j > 0; j--) {
          SyntaxElement sub = syntax_node_child_or_token(el.node, j - 1);
          if (sub.kind == SYNTAX_ELEM_NODE && sub.node) {
            if (!body) {
              body = sub.node; // keep ref
              continue;
            }
            syntax_node_release(sub.node);
          } else if (sub.kind == SYNTAX_ELEM_TOKEN && sub.token) {
            syntax_token_release(sub.token);
          }
        }
        if (body) {
          IpIndex body_ty = type_of_expr(ctx, body);
          if (body_ty.v != IP_NONE.v)
            ret_ty = body_ty;
          syntax_node_release(body);
        }
      }
      syntax_node_release(el.node);
    }

    // ---- Part B.5: missing-return-clause diagnostic -----------------------
    // A handler with any ctl/final-ctl clause MUST declare an explicit
    // `return(x: T) body` clause; the handler's answer type `b` cannot
    // otherwise be known forward-pass (Ore has no unifier to infer it from
    // clause bodies + call sites the way Koka does). Pre-scan clauses for
    // any ctl/final-ctl RHS; if found AND no return clause was DECLARED, emit
    // ONE diagnostic at the handler node. Gated on `!saw_return_clause`, NOT
    // `ret_ty == IP_NONE`: a return clause whose body types to IP_NONE (a
    // silent-poison body) WAS declared — misreporting it as "missing" hid the
    // real error. (That poison body is a separate, broader gap.) Non-fatal —
    // op-clause typing continues with expected = IP_NONE (synth-only recovery).
    if (!saw_return_clause) {
      bool needs_return_clause = false;
      for (uint32_t i = 0; i < nch && !needs_return_clause; i++) {
        SyntaxElement el = syntax_node_child_or_token(iter_parent, i);
        if (el.kind == SYNTAX_ELEM_NODE && el.node) {
          BindDef bd;
          if (syntax_node_kind(el.node) == SK_BIND_DECL &&
              BindDef_cast(el.node, &bd)) {
            SyntaxNode *rhs = BindDef_value(&bd);
            if (rhs) {
              SyntaxKind rk = syntax_node_kind(rhs);
              if (rk == SK_CTL_LAMBDA || rk == SK_FINAL_CTL_LAMBDA)
                needs_return_clause = true;
              syntax_node_release(rhs);
            }
          }
          syntax_node_release(el.node);
        } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
          syntax_token_release(el.token);
        }
      }
      if (needs_return_clause)
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "handler with ctl/final-ctl clauses must declare an explicit "
                "'return(x: T) body' clause");
    }

    // ---- Part C: op-clause validation + body typing ----------------------
    // The handled effect's DefId is the sole label of the handler effect row
    // (mirrors the discharge loop); a bare/multi-effect handler can't be
    // attributed to one op table, so op validation is skipped there.
    DefId eff_def = DEF_ID_NONE;
    if (eff_row.v != IP_NONE.v) {
      IpIndex flat = row_flatten(ctx, eff_row);
      if (ip_tag(&s->intern, flat) == IP_TAG_EFFECT_ROW) {
        IpKey ek = ip_key(&s->intern, flat);
        if (ek.effect_row.n_labels == 1)
          eff_def = ek.effect_row.labels[0];
      }
    }
    bool linear = false;
    if (eff_def.idx != DEF_ID_NONE.idx) {
      // Canonical meta read (no leaf db_def_meta) — records a dep so a `linear`
      // edit on the effect decl re-runs this handler's infer_body.
      TopLevelEntry te = db_query_top_level_entry(
          s, db_read_def_parent_module(s, eff_def),
          db_read_def_name(s, eff_def));
      linear = (te.meta & META_LINEAR) != 0;
    }
    uint64_t seen = 0;
    for (uint32_t i = 0; i < nch; i++) {
      SyntaxElement el = syntax_node_child_or_token(iter_parent, i);
      if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
        if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
          syntax_token_release(el.token);
        continue;
      }
      BindDef bd;
      if (syntax_node_kind(el.node) != SK_BIND_DECL ||
          !BindDef_cast(el.node, &bd)) {
        syntax_node_release(el.node);
        continue;
      }
      SyntaxToken *nmtok = BindDef_name(&bd);
      StrId opname =
          nmtok ? pool_intern(&s->strings, syntax_token_text(nmtok),
                              syntax_token_text_range(nmtok).length)
                : (StrId){0};
      if (nmtok)
        syntax_token_release(nmtok);
      SyntaxNode *rhs = BindDef_value(&bd);
      SyntaxKind rk = rhs ? syntax_node_kind(rhs) : SK_NONE;
      OpSort clause_sort = rk == SK_CTL_LAMBDA         ? OP_CTL
                           : rk == SK_FINAL_CTL_LAMBDA ? OP_FINAL_CTL
                           : rk == SK_DIRECT_LAMBDA    ? OP_DIRECT
                                                       : OP_VAL;

      // --- validate the clause against the op declaration ---
      if (eff_def.idx != DEF_ID_NONE.idx && opname.idx != 0) {
        uint32_t idx = db_effect_op_index(s, eff_def, opname);
        if (idx == UINT32_MAX) {
          db_emit(s, DIAG_ERROR, span_of(ctx, el.node),
                  "operation '%S' is not part of the handled effect", opname);
        } else {
          // `seen` drives the coverage check below. A duplicate op-clause is
          // already reported as a redefinition by name-res (two same-named
          // binds in the clause scope), so we don't double-diag it here.
          if (idx < 64)
            seen |= (1ull << idx);
          OpSort decl_sort = db_effect_op_sort(s, eff_def, idx);
          // A clause may use ≤ the declared control; using MORE is rejected.
          if (clause_sort > decl_sort)
            db_emit(s, DIAG_ERROR, span_of(ctx, el.node),
                    "operation '%S' uses a more general control than declared",
                    opname);
          if (decl_sort == OP_VAL && clause_sort != OP_VAL)
            db_emit(s, DIAG_ERROR, span_of(ctx, el.node),
                    "'val' operation '%S' must be handled with a value clause",
                    opname);
          if (linear && clause_sort > OP_DIRECT)
            db_emit(s, DIAG_ERROR, span_of(ctx, el.node),
                    "linear effect operation '%S' cannot use a control clause",
                    opname);
        }
      }

      // --- type the clause body against the value it must produce ---
      // The op's declared result (`opresult`) drives both the `resume` type
      // and the body target for `direct`/`val` (their body value resumes to
      // the caller). `ctl`/`final-ctl` bodies produce the handler answer `b`
      // (= ret_ty). ctl/final-ctl/direct have their own handler-only node
      // kinds (params + ctl's `resume` scoped in body_scopes); `val` is a plain
      // value expression.
      IpIndex op_ft = (eff_def.idx != DEF_ID_NONE.idx)
                          ? db_effect_op_type(s, eff_def, opname)
                          : IP_NONE;
      bool op_is_fn =
          op_ft.v != IP_NONE.v && ip_tag(&s->intern, op_ft) == IP_TAG_FN_TYPE;
      IpIndex opresult = op_is_fn ? ip_key(&s->intern, op_ft).fn_type.ret
                                  : IP_NONE;
      IpIndex expected =
          (clause_sort == OP_DIRECT || clause_sort == OP_VAL) ? opresult
                                                              : ret_ty;
      LambdaExpr lam;
      if ((rk == SK_CTL_LAMBDA || rk == SK_FINAL_CTL_LAMBDA ||
           rk == SK_DIRECT_LAMBDA) &&
          rhs && LambdaExpr_cast(rhs, &lam)) {
        if (op_is_fn) {
          IpKey ok = ip_key(&s->intern, op_ft);
          // push op param types positionally at the clause's SK_PARAM nodes
          SyntaxNode *plist = LambdaExpr_params(&lam);
          if (plist) {
            uint32_t pc = syntax_node_num_children(plist), k = 0;
            for (uint32_t j = 0; j < pc; j++) {
              SyntaxElement pe = syntax_node_child_or_token(plist, j);
              if (pe.kind == SYNTAX_ELEM_NODE && pe.node) {
                if (syntax_node_kind(pe.node) == SK_PARAM &&
                    k < ok.fn_type.n_params)
                  node_type_builder_push(ctx, pe.node, ok.fn_type.params[k++]);
                syntax_node_release(pe.node);
              } else if (pe.kind == SYNTAX_ELEM_TOKEN && pe.token) {
                syntax_token_release(pe.token);
              }
            }
            syntax_node_release(plist);
          }
          // ctl ops resume — push `resume : fn(opresult) -> b` at the lambda
          // node (pass-through ⇒ b = opresult). final-ctl/direct never resume.
          if (rk == SK_CTL_LAMBDA) {
            IpIndex *rp = arena_alloc(&s->request_arena, sizeof(IpIndex));
            rp[0] = opresult;
            IpKey rkk = {.kind = IPK_FN_TYPE,
                         .fn_type = {.ret = ret_ty.v == IP_NONE.v ? opresult
                                                                  : ret_ty,
                                     .modifiers = 0,
                                     .params = rp,
                                     .n_params = 1,
                                     .effect_row = IP_EMPTY_EFFECT_ROW},
                         .src_arena = &s->request_arena,
                         .src_gen = s->request_arena.generation};
            node_type_builder_push(ctx, rhs, ip_get(&s->intern, rkk));
          }
        }
        SyntaxNode *body = LambdaExpr_body(&lam);
        if (body) {
          check_op_clause_body(ctx, body, expected);
          syntax_node_release(body);
        }
      } else if (rhs) {
        // `val` op-clause: the rhs IS the value expression (no params/resume),
        // checked against the op result. A stray `fn(...)` value types
        // signature-only (nested-lambda deferral) — harmless.
        check_op_clause_body(ctx, rhs, expected);
      }
      if (rhs)
        syntax_node_release(rhs);
      syntax_node_release(el.node);
    }
    // Coverage — any declared op with no clause is unhandled.
    if (eff_def.idx != DEF_ID_NONE.idx) {
      uint32_t n = db_effect_op_count(s, eff_def);
      for (uint32_t i = 0; i < n && i < 64; i++)
        if (!(seen & (1ull << i)))
          db_emit(s, DIAG_ERROR, span_of(ctx, node),
                  "operation '%S' is not handled",
                  db_effect_op_at(s, eff_def, i).name);
    }

    if (clause_list)
      syntax_node_release(clause_list);
    if (clause_block)
      syntax_node_release(clause_block);
    IpKey hk = {.kind = IPK_HANDLER_TYPE,
                .handler_type = {
                    .effect = eff_row, .action = action_ty, .ret = ret_ty}};
    return ip_get(&s->intern, hk);
  }

  case SK_LAMBDA_EXPR: {
    LambdaExpr lam;
    if (!LambdaExpr_cast(node, &lam))
      return IP_NONE;
    SyntaxNode *params = LambdaExpr_params(&lam);
    SyntaxNode *ret = LambdaExpr_return_type(&lam);
    SyntaxNode *er = LambdaExpr_effect_row(&lam);
    IpIndex t = build_fn_type(ctx, ret, params, er);
    if (params)
      syntax_node_release(params);
    if (ret)
      syntax_node_release(ret);
    if (er)
      syntax_node_release(er);
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
      return IP_ERROR_TYPE;
    }
    uint32_t n_args = 0;
    SyntaxNode **args = collect_arg_nodes(s, arg_list, &n_args);
    IpIndex result = IP_NONE;

    // J4: pre-type each arg before dispatch — centralized hover policy.
    //   - Value-position args (evaluates_args=true): type_of_expr
    //     records refs + pushes node-type.
    //   - Type-position args (evaluates_args=false: @sizeOf/@alignOf/
    //     @typeName/@import-name-as-type): resolve_type_expr records
    //     the resolved IpIndex on the arg node so hover on
    //     `MyStruct` in `@sizeOf(MyStruct)` returns the struct type.
    // @import is the one type-position builtin we still skip — its
    // arg is a string literal, not a type expression. @intCast and
    // @ptrCast handle their OWN arg-typing inside the handler (which
    // also doubles as the result-type computation).
    const BuiltinMeta *m = db_builtin_meta(k);
    if (m && m->evaluates_args) {
      for (uint32_t i = 0; i < n_args; i++)
        (void)type_of_expr(ctx, args[i]);
    } else if (k == BUILTIN_SIZEOF || k == BUILTIN_ALIGNOF ||
               k == BUILTIN_TYPENAME) {
      for (uint32_t i = 0; i < n_args; i++) {
        if (!args[i])
          continue;
        IpIndex t = resolve_type_expr(ctx, args[i]);
        if (ip_index_is_valid(t))
          node_type_builder_push(ctx, args[i], t);
      }
    }
    result = db_dispatch_builtin(ctx, k, args, n_args, anchor);
    release_arg_nodes(args, n_args);
    if (arg_list)
      syntax_node_release(arg_list);
    return result;
  }

  // SK_COMPTIME_EXPR — `comptime <inner>` marker. Subphase B0a wraps
  // every `comptime` prefix; subphase B6 replaces this pass-through
  // with sema_comptime_select for true Route A dispatch. For now we
  // type the inner expression transparently so existing comptime
  // fixtures (comptime_branch.ore etc.) don't regress.
  case SK_COMPTIME_EXPR: {
    ComptimeExpr ce;
    if (!ComptimeExpr_cast(node, &ce))
      return IP_NONE;
    SyntaxNode *inner = ComptimeExpr_inner(&ce);
    if (!inner)
      return IP_NONE;
    IpIndex t = sema_comptime_select(ctx, inner, IP_NONE);
    syntax_node_release(inner);
    return t;
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
    // walk_init_list still runs even on target=IP_ERROR_TYPE so init-list
    // members surface their own errors (DC8 sibling-masking discipline).
    // Field-check arms will see the sticky target via their check_expr
    // calls and absorb silently per the consumer-arm pattern.
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
    return IP_ERROR_TYPE;

  default:
    // The parser handed us a SyntaxKind the inference dispatcher
    // doesn't recognize as an expression. Either the kind shouldn't
    // appear in expression position (parser bug) or a grammar
    // addition forgot to add an arm above (developer bug). Either
    // way, this is an internal-compiler-error, not a "TODO":
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "internal: expression kind %d has no inference rule", (int)k);
    return IP_ERROR_TYPE;
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

  // Cascade-suppression: an upstream failure poisoned `expected` to
  // IP_NONE (e.g. SK_ASSIGN_EXPR's `lt = type_of_expr(lhs)` returned
  // IP_NONE because the LHS deref'd a IP_NONE'd pointer that came from
  // a failed @ptrCast that came from an unknown builtin upstream...).
  //
  // The bidirectional handlers below short-circuit on IP_NONE expected
  // anyway. The synthesis fall-through at the tail of this function
  // is what fires the cascade: type_of_expr(node) on an SK_PRODUCT_EXPR
  // with no type prefix emits "anonymous typed construction requires a
  // target type from context" — a fresh diag DOWNSTREAM of the real
  // problem. Same shape for other node kinds whose synthesis path
  // emits "I can't decide your type" diags.
  //
  // Caller already has an upstream diag pointing at the real failure;
  // we absorb (no fresh coerce diag) to keep the diagnostic stream
  // focused on root causes.
  //
  // Item #20 (revised): the two poisoned sentinels absorb ASYMMETRICALLY.
  // IP_NONE means "no expected type" (upstream lost it) — stay fully
  // silent AND skip synthesis: type_of_expr on e.g. an SK_PRODUCT_EXPR
  // with no type prefix would emit "requires a target type from context",
  // a fresh cascade diag (the block comment above). IP_ERROR_TYPE means
  // "upstream already DIAGNOSED the failure" — absorb the coercion but
  // FORCE-WALK the subtree first (the DC8 pattern, see the call-arg
  // force-type loop): errors INSIDE this node are real and independent
  // of the upstream failure, and the walk also populates hover types.
  // Without it, one bad param type used to mute every diag in every
  // argument/assignment/return-value subtree checked against it.
  if (expected.v == IP_NONE.v)
    return true;
  if (ip_is_error(expected)) {
    (void)type_of_expr(ctx, node);
    return true;
  }

  {

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
        if (ok && ip_index_is_valid(target) && target.v != expected.v) {
          // Pass NULL node so coerce_or_diag emits structural-only (target
          // is the natural type of the just-built product; not a comptime
          // narrow). Suppress the diag via custom emit so the span lands
          // on the product literal, not on a child of it.
          Coercion c = coerce(ctx, NULL, target, expected);
          if (c.kind != COERCE_OK) {
            db_emit(s, DIAG_ERROR, span_of(ctx, node),
                    "expected type '%T', found '%T'", expected, target);
            ok = false;
          }
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

    // Slice 5 cleanup: no SK_BLOCK_STMT case here. Under Zig-strict, a
    // block's type is purely STRUCTURAL — void by default, peer-unified
    // accumulator for labeled blocks with break-with-value sites.
    // `expected` doesn't propagate INTO a block (only `return` and
    // `break :label v` flow values out). The synth-then-coerce fallback
    // at the tail of this function handles blocks correctly: type_of_expr
    // on a block runs the SK_BLOCK_STMT case (pushes any label frame,
    // walks stmts, emits discardedness warnings, computes block_ty);
    // then coerce_or_diag against `expected` produces the right diag.

    if (k == SK_IF_EXPR) {
      IfExpr ie;
      if (IfExpr_cast(node, &ie)) {
        SyntaxNode *cond = IfExpr_condition(&ie);
        SyntaxNode *capture = IfExpr_capture(&ie);
        SyntaxNode *then_b = IfExpr_then_branch(&ie);
        SyntaxNode *else_b = IfExpr_else_branch(&ie);
        bool ok = true;
        handle_if_cond(ctx, cond, capture);
        // Zig-strict: an `if` with no `else` yields void on the untaken path,
        // so it cannot satisfy a non-void expected type. (The no-expected synth
        // path still degrades to void — that peer-typing hole rides with the
        // Slice-5 value/statement unification.)
        if (!else_b && expected.v != IP_NONE.v &&
            expected.v != IP_VOID_TYPE.v && expected.v != IP_ERROR_TYPE.v &&
            expected.v != IP_NORETURN_TYPE.v) {
          db_emit(s, DIAG_ERROR, span_of(ctx, node),
                  "`if` without `else` cannot yield a value of type %T; "
                  "add an `else` branch",
                  expected);
          ok = false;
        }
        if (then_b && !check_expr(ctx, then_b, expected))
          ok = false;
        if (else_b && !check_expr(ctx, else_b, expected))
          ok = false;
        if (cond)    syntax_node_release(cond);
        if (capture) syntax_node_release(capture);
        if (then_b)  syntax_node_release(then_b);
        if (else_b)  syntax_node_release(else_b);
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
      // H2: coerce_or_diag folds structural + range-check + Zig-parity
      // diag. Restamp only on success in the comptime→concrete narrow
      // case (H1 invariant: optional-lift / nil-lift / decay don't
      // restamp — they reach here only when actual_comptime is false).
      if (!coerce_or_diag(ctx, node, actual, expected))
        return false;
      if (actual_comptime && !expected_comptime)
        node_type_builder_push(ctx, node, expected);
      return true;
    }
  }

  // Synth-then-compare.
  IpIndex actual = type_of_expr(ctx, node);
  if (expected.v == IP_NONE.v)
    return actual.v != IP_NONE.v;
  // H2: single call folds structural + range-check + Zig-parity diag.
  return coerce_or_diag(ctx, node, actual, expected);
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

  // Producer-side: INFER_BODY is computing FOR `def`. Reading own
  // identity is self-data.
  StrId name = db_get_def_name_untracked(s, def);
  NamespaceId nsid = db_get_def_parent_module_untracked(s, def);

  const FnSignature *sig =
      db_query_fn_signature(ctx, def);  // dep: declared return
  (void)db_query_body_scopes(ctx, def); // dep: scope structure
  IpIndex sigty = sig ? sig->type : IP_NONE;

  TopLevelEntry e =
      db_query_top_level_entry(ctx, nsid, name); // CONTENT firewall
  SyntaxTree *tree = NULL;
  SyntaxNode *lambda_node = NULL;
  if (e.node_ptr.kind != SYNTAX_KIND_NONE) {
    // LINT_UNTRACKED_OK — TOP_LEVEL_ENTRY above is the body-stable
    // content firewall; tracked FILE_AST here would force every INFER_BODY
    // to recompute on any file edit.
    struct GreenNode *groot = db_read_file_ast_untracked(ctx, e.file);
    if (groot) {
      tree = syntax_tree_new(groot);
      SyntaxNode *rroot = syntax_tree_root(tree);
      SyntaxNode *wrapper = syntax_node_ptr_resolve(e.node_ptr, rroot);
      syntax_node_release(rroot);
      if (wrapper) {
        SyntaxNode *val = NULL;
        BindDef bd;
        if (BindDef_cast(wrapper, &bd))
          val = BindDef_value(&bd);
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

  // Phase P S6 — open the per-fn DiagBundle sink BEFORE the body walk,
  // and reset the bundle so this generation's emits start clean. The
  // decl_ast_id_map for this def was built by TYPE_OF_DECL (which
  // runs before INFER_BODY); we just read it.
  DiagBundle *body_bundle = infer_body_diags_slot(s, def);
  if (body_bundle)
    diag_bundle_reset(body_bundle);
  DiagSink body_sink = infer_body_sink_open(s, def);
  db_query_frame_set_sink(ctx, body_bundle ? &body_sink : NULL);

  // Cache the DeclAstIdMap pointer for span_of's hot path. Stable for
  // this INFER_BODY frame: PagedVec pages don't relocate (paged_get
  // returns a stable interior pointer). The map is OWNED by
  // TYPE_OF_DECL — which always runs before INFER_BODY for this def
  // via db_check_namespace's per-decl loop — so the row is populated
  // with the current-wrapper preorder.
  const DeclAstIdMap *decl_map = db_get_decl_ast_id_map_untracked(s, def);
  AstId decl_key_id = *(AstId *)vec_get(&s->defs.identity_keys, def.idx); // LINT_UNTRACKED_OK: producer self-data

  Fingerprint fp = FINGERPRINT_NONE;
  if (lambda_node) {
    LambdaExpr lam;
    if (LambdaExpr_cast(lambda_node, &lam)) {
      SyntaxNode *params = LambdaExpr_params(&lam);
      SyntaxNode *body = LambdaExpr_body(&lam);
      NodeTypeBuilder b;
      node_type_builder_begin(s, &b, e.file);
      // Effects-4 — per-INFER_BODY frame state. The accumulator starts
      // empty and grows via row_union at every effectful call site.
      // row_subst lives next to it so unify-time bindings (from effect
      // discharge or signature-check at the end) all live on the same
      // per-body frame.
      IpIndex body_row = IP_EMPTY_EFFECT_ROW;
      HashMap row_subst = {0};
      // Monomorphization — per-body type-var substitution. A call to a
      // polymorphic callee freshens its holes and binds them here (the
      // coerce hole rule); the callee's return is resolved against this
      // map. Lives on the same per-body frame as row_subst.
      HashMap type_subst = {0};
      SemaCtx walk = {.s = s,
                      .file_green_root = NULL,
                      .nsid = nsid,
                      .enclosing_fn = def,
                      .file_local = e.file,
                      .types = &b,
                      .decl_ast_map = decl_map,
                      .decl_key = decl_key_id.idx,
                      .row_subst = &row_subst,
                      .type_subst = &type_subst,
                      .body_effect_row = &body_row,
                      .expected_ret_override = IP_NONE};
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
      // Monomorphization — a GENERIC fn (its signature carries `anytype`
      // holes) is body-checked PER INSTANCE (db_query_infer_instance) against
      // concrete arg types, NEVER here against the raw holes: e.g. `p.x` with
      // p:hole would spuriously error ("field access on non-aggregate").
      // Zig-faithful — an uninstantiated generic body is not analyzed. Drop
      // the body so the walk + effect gate below are skipped.
      bool is_generic = sig_is_fn && sig_has_unbound_hole(&walk, sigty);
      if (body && is_generic) {
        syntax_node_release(body);
        body = NULL;
      }
      if (body) {
        OreSyntaxKind body_kind = (OreSyntaxKind)syntax_node_kind(body);
        if (body_kind == SK_BLOCK_STMT) {
          // Slice 5 — type the body as a statement block (Zig-strict). The
          // body's TYPE is always void; values flow out via explicit `return`
          // statements (which type-check their value against expected_ret at
          // the SK_RETURN_STMT case in type_of_expr) or `break :label v` to
          // labeled blocks. Using type_of_expr here (instead of the old
          // check_expr(body, expected_ret) call) avoids the spurious "block
          // yields void; expected <ret_ty>" diag from check_expr's block case.
          (void)type_of_expr(&walk, body);
          // Phase B terminator gate. Non-void fns must end every path in
          // an EXPLICIT terminator (return / noreturn callee / infinite-loop-
          // without-reachable-break). Slice 5: implicit-last-expression is
          // GONE — passing IP_NONE here so the prior "trailing expr counts
          // as termination" path doesn't fire even by accident.
          if (sig_is_fn && expected_ret.v != IP_VOID_TYPE.v &&
              expected_ret.v != IP_NORETURN_TYPE.v &&
              !ip_is_error(expected_ret) &&
              !block_always_terminates(&walk, body)) {
            db_emit(s, DIAG_ERROR, span_of(&walk, lambda_node),
                    "control reaches end of non-void function (returns %T) "
                    "without producing a value", expected_ret);
          }
        } else {
          // Slice 6 — bare-expression body (`fn(x) -> i32 x * x`). The
          // expression's value IS the implicit return. Type it bidirec-
          // tionally against `expected_ret` when one is declared; fall
          // back to plain synthesis otherwise (lambda used in a context
          // that doesn't provide an expected fn-type, e.g. an unannotated
          // const bind).
          //
          // No CFG missing-return gate: a bare-expression body always
          // "returns" its value by construction. The void-return case
          // (`expected_ret == IP_VOID_TYPE`) is acceptable here — sema
          // simply discards the value, mirroring `return EXPR;` against
          // a void fn (which is also tolerated — see SK_RETURN_STMT).
          if (sig_is_fn && expected_ret.v != IP_NONE.v &&
              expected_ret.v != IP_VOID_TYPE.v &&
              expected_ret.v != IP_NORETURN_TYPE.v) {
            check_expr(&walk, body, expected_ret);
          } else {
            (void)type_of_expr(&walk, body);
          }
        }
        syntax_node_release(body);
      }
      // Effects-4f — soundness gate. The body's accumulated row must
      // unify with the declared effect row. row_unify handles all
      // four shapes correctly:
      //   - declared closed: body row must match its labels exactly
      //   - declared open <l1..ln | μ>: μ absorbs the residual labels
      //   - body row has a residual row var: bound by unification
      //   - both empty: trivial
      // Failure (e.g. body performs <io> with declared <>) emits a
      // diag at the lambda site. This is the gate that lets Phase 5's
      // comptime purity check trust fn_type.effect_row.
      // (Skipped for a generic fn — its body wasn't walked, so body_row is
      // trivially empty; the per-instance check enforces effect soundness.
      // Skipped when the declared row slot is sticky-error — a bad effect
      // label was already diag'd at the row; unifying against a non-row
      // would always fail and emit cascade noise.)
      if (sig_is_fn && !is_generic &&
          !ip_is_error(ip_key(&s->intern, sigty).fn_type.effect_row)) {
        IpIndex declared = ip_key(&s->intern, sigty).fn_type.effect_row;
        // Effects-4.5c — defaulting pass. Any row var that the body
        // walk left unbound (e.g. an unused polymorphic argument's
        // fresh row var) is ground to IP_EMPTY_EFFECT_ROW here so the
        // 4f diag below renders <> instead of <..rv#N>. Run BEFORE
        // the unify so the resolved body row is already canonical.
        ground_unbound_row_vars(&walk, body_row);
        // row_flatten (not row_resolve) splices in bound EFFECT_ROW
        // tails so the IpIndex handed to the diag formatter reflects
        // post-substitution state — the formatter doesn't see ctx.
        IpIndex resolved_body = row_flatten(&walk, body_row);
        IpIndex resolved_decl = row_flatten(&walk, declared);
        // An error body row means an accumulator already diag'd a merge
        // failure at the offending call — don't re-fail the gate on it.
        if (!ip_is_error(resolved_body) &&
            !row_unify(&walk, resolved_body, resolved_decl, lambda_node)) {
          db_emit(s, DIAG_ERROR, span_of(&walk, lambda_node),
                  "function declares effects %T but body performs %T",
                  resolved_decl, resolved_body);
        }
      }
      if (hashmap_is_initialized(&row_subst))
        hashmap_free(&row_subst);
      if (hashmap_is_initialized(&type_subst))
        hashmap_free(&type_subst);
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

// ===========================================================================
// Monomorphization — db_query_infer_instance.
//
// A clone of db_query_infer_body keyed on an interned IPK_INSTANCE (callee
// def + concrete arg types) instead of a DefId. The ONE behavioural
// difference is the param-typing loop: where infer_body pushes each param's
// GENERIC signature type (which may be an `anytype` hole), this binds each
// hole to its concrete arg type in `type_subst` and pushes the CONCRETE type
// — so the body duck-types against the real types (`p.x` resolves against the
// concrete struct). Returns the instance's concrete return type.
//
// Invariant: name/nsid derive from the decoded callee `def` (NOT any caller),
// so private symbols resolve in the callee's own namespace.
// ===========================================================================
IpIndex db_query_infer_instance(db_query_ctx *ctx, IpIndex inst) {
  struct db *s = (struct db *)ctx;
  IpKey ik = ip_key(&s->intern, inst);
  if (ik.kind != IPK_INSTANCE)
    return IP_NONE;
  DefId def = ik.instance.def;
  const IpIndex *inst_args = ik.instance.args; // pool-stable (interned)
  size_t inst_n_args = ik.instance.n_args;
  if (db_def_kind(s, def) != KIND_FUNCTION)
    return IP_NONE;

  uint64_t key = (uint64_t)inst.v;
  db_query_slot_alloc(ctx, QUERY_INFER_INSTANCE, key); // HashMap-routed: mint row
  DB_QUERY_GUARD(ctx, QUERY_INFER_INSTANCE, key,
                 /* on_cached */ instance_read(s, key).ret_type,
                 /* on_cycle  */ IP_NONE,
                 /* on_error  */ IP_NONE);

  // Live recursion depth: only the compute path (not cached hits) re-runs
  // the body, so incrementing here tracks the true instantiation stack.
  // Balanced by the decrement just before the single return below.
  s->mono_depth++;

  StrId name = db_get_def_name_untracked(s, def);
  NamespaceId nsid = db_get_def_parent_module_untracked(s, def);

  const FnSignature *sig = db_query_fn_signature(ctx, def); // dep: signature
  (void)db_query_body_scopes(ctx, def);                     // dep: scopes
  IpIndex sigty = sig ? sig->type : IP_NONE;

  TopLevelEntry e = db_query_top_level_entry(ctx, nsid, name); // CONTENT firewall
  SyntaxTree *tree = NULL;
  SyntaxNode *lambda_node = NULL;
  if (e.node_ptr.kind != SYNTAX_KIND_NONE) {
    // LINT_UNTRACKED_OK — TOP_LEVEL_ENTRY above is the body-stable
    // content firewall; tracked FILE_AST here would force every INFER_INSTANCE
    // to recompute on any file edit.
    struct GreenNode *groot = db_read_file_ast_untracked(ctx, e.file);
    if (groot) {
      tree = syntax_tree_new(groot);
      SyntaxNode *rroot = syntax_tree_root(tree);
      SyntaxNode *wrapper = syntax_node_ptr_resolve(e.node_ptr, rroot);
      syntax_node_release(rroot);
      if (wrapper) {
        SyntaxNode *val = NULL;
        BindDef bd;
        if (BindDef_cast(wrapper, &bd))
          val = BindDef_value(&bd);
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

  // Open the per-INSTANCE DiagBundle sink (its own column, keyed by the
  // interned-instance key) so duck-typed body errors are attributed to THIS
  // monomorphization, not the generic callee.
  DiagBundle *inst_bundle = instance_diags_slot(s, key);
  if (inst_bundle)
    diag_bundle_reset(inst_bundle);
  DiagSink inst_sink = instance_sink_open(s, key);
  db_query_frame_set_sink(ctx, inst_bundle ? &inst_sink : NULL);

  const DeclAstIdMap *decl_map = db_get_decl_ast_id_map_untracked(s, def);
  AstId decl_key_id = *(AstId *)vec_get(&s->defs.identity_keys, def.idx); // LINT_UNTRACKED_OK

  IpIndex ret_type = IP_NONE;
  Fingerprint fp = FINGERPRINT_NONE;
  if (lambda_node) {
    LambdaExpr lam;
    if (LambdaExpr_cast(lambda_node, &lam)) {
      SyntaxNode *params = LambdaExpr_params(&lam);
      SyntaxNode *body = LambdaExpr_body(&lam);
      NodeTypeBuilder b;
      node_type_builder_begin(s, &b, e.file);
      IpIndex body_row = IP_EMPTY_EFFECT_ROW;
      HashMap row_subst = {0};
      HashMap type_subst = {0};
      SemaCtx walk = {.s = s,
                      .file_green_root = NULL,
                      .nsid = nsid,
                      .enclosing_fn = def,
                      .file_local = e.file,
                      .types = &b,
                      .decl_ast_map = decl_map,
                      .decl_key = decl_key_id.idx,
                      .row_subst = &row_subst,
                      .type_subst = &type_subst,
                      .body_effect_row = &body_row,
                      .expected_ret_override = IP_NONE};
      bool sig_is_fn =
          (sigty.v != IP_NONE.v && ip_tag(&s->intern, sigty) == IP_TAG_FN_TYPE);

      // THE DIVERGENCE from infer_body — bind each anytype-hole param to its
      // concrete arg type, then push the concrete (not the hole) so the body
      // duck-types against real types. Non-hole params push their declared
      // type unchanged.
      if (params && sig_is_fn) {
        IpKey fk = ip_key(&s->intern, sigty);
        uint32_t total = syntax_node_num_children(params);
        size_t pi = 0;
        for (uint32_t i = 0; i < total && pi < fk.fn_type.n_params; i++) {
          SyntaxElement el = syntax_node_child_or_token(params, i);
          if (el.kind == SYNTAX_ELEM_NODE && el.node) {
            if (syntax_node_kind(el.node) == SK_PARAM) {
              IpIndex pty = fk.fn_type.params[pi];
              if (ip_tag(&s->intern, pty) == IP_TAG_TYPE_VAR &&
                  pi < inst_n_args) {
                // Bind the hole ONCE: a `@TypeOf(<earlier anytype param>)`
                // param shares the earlier param's hole id, so a later
                // position must NOT rebind it — the first arg wins (matching
                // the call-site rank-1 unify). Push the hole's RESOLVED value
                // so a shared hole takes the first-bound concrete type, not
                // this position's arg.
                if (type_resolve(&walk, pty).v == pty.v)
                  type_subst_bind(&walk, ip_key(&s->intern, pty).type_var.id,
                                  inst_args[pi]);
                pty = type_resolve(&walk, pty); // concrete (first-bound)
              }
              node_type_builder_push(&walk, el.node, pty);
              pi++;
            }
            syntax_node_release(el.node);
          } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
            syntax_token_release(el.token);
          }
        }
      }
      if (params)
        syntax_node_release(params);

      IpIndex expected_ret =
          sig_is_fn ? apply_type_subst(&walk, ip_key(&s->intern, sigty).fn_type.ret)
                    : IP_NONE;
      // A block body's `return X` statements resolve their target via
      // expected_ret_override, else the ENCLOSING fn signature — which here is
      // the GENERIC sig carrying holes (`[]t`). Override with the per-instance
      // substituted return (`[]u32`) so `return [_]t{}` checks against the
      // concrete type, not the unbound hole. (Nested lambdas/op-clauses set
      // their own override locally, so this only governs the fn's own returns.)
      if (sig_is_fn && expected_ret.v != IP_NONE.v)
        walk.expected_ret_override = expected_ret;
      if (body) {
        OreSyntaxKind body_kind = (OreSyntaxKind)syntax_node_kind(body);
        if (body_kind == SK_BLOCK_STMT) {
          (void)type_of_expr(&walk, body);
          if (sig_is_fn && expected_ret.v != IP_VOID_TYPE.v &&
              expected_ret.v != IP_NORETURN_TYPE.v &&
              !ip_is_error(expected_ret) &&
              !block_always_terminates(&walk, body)) {
            db_emit(s, DIAG_ERROR, span_of(&walk, lambda_node),
                    "control reaches end of non-void function (returns %T) "
                    "without producing a value", expected_ret);
          }
        } else {
          if (sig_is_fn && expected_ret.v != IP_NONE.v &&
              expected_ret.v != IP_VOID_TYPE.v &&
              expected_ret.v != IP_NORETURN_TYPE.v) {
            check_expr(&walk, body, expected_ret);
          } else {
            (void)type_of_expr(&walk, body);
          }
        }
        syntax_node_release(body);
      }
      // Effect soundness gate (identical to infer_body, incl. the sticky-
      // error row + error-body-row skips — see the infer_body gate).
      if (sig_is_fn &&
          !ip_is_error(ip_key(&s->intern, sigty).fn_type.effect_row)) {
        IpIndex declared = ip_key(&s->intern, sigty).fn_type.effect_row;
        ground_unbound_row_vars(&walk, body_row);
        IpIndex resolved_body = row_flatten(&walk, body_row);
        IpIndex resolved_decl = row_flatten(&walk, declared);
        if (!ip_is_error(resolved_body) &&
            !row_unify(&walk, resolved_body, resolved_decl, lambda_node)) {
          db_emit(s, DIAG_ERROR, span_of(&walk, lambda_node),
                  "function declares effects %T but body performs %T",
                  resolved_decl, resolved_body);
        }
      }
      // Default any hole left unbound, then derive the concrete return.
      // If the ret-position hole is STILL unbound after the whole body
      // walk, grounding is about to bind it to IP_ERROR_TYPE — that used
      // to be a diag-less poison mint (the call site then suppressed
      // silently). Diag first (poison contract).
      if (sig_is_fn) {
        IpIndex sig_ret = ip_key(&s->intern, sigty).fn_type.ret;
        if (sig_has_unbound_hole(&walk, sig_ret)) {
          db_emit(s, DIAG_ERROR, span_of(&walk, lambda_node),
                  "cannot infer the generic return type for this "
                  "instantiation");
        }
        ground_unbound_type_vars(&walk, sig_ret);
        ret_type = apply_type_subst(&walk, sig_ret);
      }
      // Error-locality — if this instantiation produced any diags, append the
      // instantiation context so a duck-typed failure names the generic fn +
      // the concrete types it was instantiated with (e.g. `getx` for
      // [comptime_int]). The note is emitted AFTER the body errors (the
      // collector preserves emit order, no position sort), so it trails them.
      // (The originating call-site span is deferred — an instance is memoized
      // across call sites, so it has no single one.)
      if (inst_bundle && inst_bundle->items.count > 0) {
        char tybuf[256];
        size_t tw = 0;
        tybuf[0] = '\0';
        for (size_t i = 0; i < inst_n_args && tw + 2 < sizeof(tybuf); i++) {
          if (i > 0) {
            tybuf[tw++] = ',';
            tybuf[tw++] = ' ';
          }
          size_t w =
              ip_format(&s->intern, inst_args[i], tybuf + tw, sizeof(tybuf) - tw);
          if (w >= sizeof(tybuf) - tw) {
            tw = sizeof(tybuf) - 1;
            break;
          }
          tw += w;
        }
        tybuf[tw] = '\0';
        db_emit(s, DIAG_INFO, span_of(&walk, lambda_node),
                "while instantiating '%S' for [%s]", name, tybuf);
      }
      if (hashmap_is_initialized(&row_subst))
        hashmap_free(&row_subst);
      if (hashmap_is_initialized(&type_subst))
        hashmap_free(&type_subst);
      NodeTypesRange range = node_type_builder_end(&b, &fp);
      InstanceResult res = {.ret_type = ret_type, .node_types = range};
      instance_write(s, key, res); // frees prior node_types
    }
    syntax_node_release(lambda_node);
  } else {
    // No resolvable lambda (degenerate/drift). Store ret_type = IP_NONE
    // explicitly so a cached hit reads back the SAME value the compute path
    // returns. (Since the IP_NONE = index-0 flip, a zero-init {0} IS
    // IP_NONE — the explicit designator stays for intent, not necessity.)
    InstanceResult empty = {.ret_type = IP_NONE};
    instance_write(s, key, empty);
  }
  if (tree)
    syntax_tree_free(tree);

  s->mono_depth--; // balance the post-GUARD increment
  db_query_succeed(ctx, QUERY_INFER_INSTANCE, key, fp);
  return ret_type;
}
