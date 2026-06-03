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
// F1 (Phase P audit) — defined in body_scopes.c. INFER_BODY calls it
// at compute entry so the BodyAstIdMap is fresh against the current
// tree even when BODY_SCOPES salsa-cut off but INFER_BODY re-ran.
extern void body_ast_id_map_refresh(struct db *s, DefId def,
                                    SyntaxNode *lambda_node);
extern SyntaxNodePtr db_body_scope_lookup(db_query_ctx *ctx, DefId fn_def,
                                          SyntaxNode *use_node, StrId name);

// --- Forward decls (mutually recursive) --------------------------------------
IpIndex type_of_expr(const SemaCtx *ctx, SyntaxNode *node);
bool check_expr(const SemaCtx *ctx, SyntaxNode *node, IpIndex expected);

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
  // Phase P S6 — prefer a structural body anchor when sema is inside
  // a body-owning frame AND the node was visited by the body's
  // preorder walker (the rev map sees it). Body anchors resolve via
  // body_ast_id_resolve's preorder walk in the publish path, which
  // survives sibling reparse byte-shifts. On miss (e.g. a sub-query
  // walked outside the body), fall back to the legacy FILE_RAW anchor
  // — still correct, just position-fragile.
  if (ctx->body_ast_map && node) {
    uint32_t rel;
    if (body_ast_id_lookup(ctx->body_ast_map, node, &rel)) {
      // F5 (Phase P audit) — file_id == 0 would silently drop this
      // diag at collect time (file_id_eq filter). INFER_BODY always
      // sets ctx->file_local from a TopLevelEntry's e.file, so a zero
      // here is a structural bug, not a runtime condition.
      assert(ctx->file_local.idx != 0 &&
             "span_of: BODY anchor with file_local.idx == 0");
      return diag_anchor_body((uint16_t)ctx->file_local.idx,
                              (DeclKey)ctx->body_decl_key, (RelAstId)rel);
    }
  }
  return diag_anchor_of_node((uint16_t)ctx->file_local.idx, node);
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
  return k == SK_CONST_DECL || k == SK_VAR_DECL || k == SK_RETURN_STMT ||
         k == SK_BREAK_STMT || k == SK_CONTINUE_STMT || k == SK_DEFER_STMT ||
         k == SK_LOOP_EXPR || k == SK_BLOCK_STMT || k == SK_IF_EXPR ||
         k == SK_SWITCH_EXPR || k == SK_ASSIGN_EXPR || k == SK_EXPR_STMT;
}

// ============================================================================
// Phase-B terminator pass: "control reaches end of non-void function".
//
// `block_always_terminates(ctx, node, expected)` returns true iff every
// execution path through `node` ends in a value-producing terminator —
// `return`, a `noreturn` callee (panic / exit / non-resuming effect op),
// an infinite loop with no reachable `break`, or an implicit-last
// expression whose type coerces to `expected`.
//
// Conservative everywhere: unknown / unhandled node kinds default to
// false. False negatives produce harmless diagnostics; false positives
// would silence the safety gate (and re-introduce the "ret with garbage"
// class of bugs the whole pass is here to prevent).
//
// IP_NORETURN_TYPE callees are detected via db_lookup_node_type — a
// side-effect-free cache-peek into the in-flight NodeTypeBuilder that
// check_expr populated. Re-typing the callee would re-accumulate its
// effect row into ctx->body_effect_row (the original Phase B deferral
// reason); the cache-peek skips compute entirely.
// ============================================================================

static bool block_always_terminates(const SemaCtx *ctx, SyntaxNode *node,
                                    IpIndex expected);
static bool pattern_is_wildcard(SyntaxNode *p); // defined further down

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

// `expected` is the enclosing fn's declared return type — used for the
// implicit-return slot at a block's last statement. Pass IP_NONE to
// disable the implicit-return check (e.g. when recursing into an
// expression that isn't a block tail).
static bool block_always_terminates(const SemaCtx *ctx, SyntaxNode *node,
                                    IpIndex expected) {
  if (!node)
    return false;
  struct db *s = ctx->s;
  SyntaxKind k = syntax_node_kind(node);

  // `return` exits the function unconditionally.
  if (k == SK_RETURN_STMT)
    return true;

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
    SyntaxNode *callee = CallExpr_callee(&ce);
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
              block_always_terminates(ctx, then_b, expected) &&
              block_always_terminates(ctx, else_b, expected);
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
            if (!block_always_terminates(ctx, body, expected))
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
    // Without a wildcard we trust the exhaustiveness check elsewhere
    // ONLY when the scrutinee is an enum that the type-checker has
    // verified all-variants-covered. Conservative default: require a
    // wildcard arm to declare the switch terminating from this pass's
    // view.
    return all_term && has_wildcard;
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

  // Block: iterate statements in source order. If ANY statement is a
  // terminator, the rest is dead code and the block terminates. The
  // last statement also gets the implicit-return check — a value-
  // producing expression whose type coerces to `expected` is treated
  // as the block's terminator (Rust-style implicit return).
  if (k == SK_BLOCK_STMT || k == SK_BLOCK_EXPR) {
    BlockStmt bs = {.syntax = node};
    SyntaxNode *stmts = BlockStmt_stmts(&bs);
    if (!stmts)
      return false;
    uint32_t total = syntax_node_num_children(stmts);
    // Find the LAST node-child index (skipping interleaved tokens) so
    // the implicit-return check fires only on the actual trailing stmt.
    int32_t last_node_idx = -1;
    for (int32_t i = (int32_t)total - 1; i >= 0; i--) {
      SyntaxElement pe = syntax_node_child_or_token(stmts, (uint32_t)i);
      if (pe.kind == SYNTAX_ELEM_NODE && pe.node) {
        syntax_node_release(pe.node);
        last_node_idx = i;
        break;
      } else if (pe.kind == SYNTAX_ELEM_TOKEN && pe.token) {
        syntax_token_release(pe.token);
      }
    }
    bool ok = false;
    for (uint32_t i = 0; i < total && !ok; i++) {
      SyntaxElement el = syntax_node_child_or_token(stmts, i);
      if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
        if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
          syntax_token_release(el.token);
        continue;
      }
      SyntaxNode *stmt = el.node;
      if (block_always_terminates(ctx, stmt, expected)) {
        ok = true; // rest is dead code; whole block terminates.
      } else if ((int32_t)i == last_node_idx && expected.v != IP_NONE.v) {
        // Implicit-return slot: a STRUCTURALLY value-producing
        // trailing expression counts as the block's terminator. We
        // classify by syntax kind only (no type_of_expr) because the
        // check_expr pass at infer.c:2185 already verified the actual
        // coercion against `expected` — re-typing here would (a)
        // re-accumulate effects from any call in the expression and
        // (b) hit SYNTHESIS-only nodes like SK_ENUM_REF_EXPR `.Var`
        // which only have a check_expr rule.
        //
        // Statements (SK_*_STMT, SK_*_DECL) are not value-producing
        // in this slot; only expression-kind nodes are. SK_ASSIGN_EXPR
        // technically yields void but stays in the expression range —
        // an `x = 5` in a non-void fn's tail will surface as
        // check_expr's "expected T, found void" diag; the redundant
        // "control reaches end" gate doesn't fire because we accept
        // any expr-kind here.
        OreSyntaxKind sk = (OreSyntaxKind)syntax_node_kind(stmt);
        if (ore_kind_is_expr_node(sk))
          ok = true;
      }
      syntax_node_release(stmt);
    }
    syntax_node_release(stmts);
    return ok;
  }

  // Defer / expr-statement / paren wrappers: recurse into the inner
  // expression. A naked terminator wrapped in one of these still
  // terminates.
  if (k == SK_EXPR_STMT || k == SK_PAREN_EXPR || k == SK_DEFER_STMT) {
    bool ok = false;
    uint32_t n = syntax_node_num_children(node);
    for (uint32_t i = 0; i < n && !ok; i++) {
      SyntaxElement el = syntax_node_child_or_token(node, i);
      if (el.kind == SYNTAX_ELEM_NODE && el.node) {
        if (block_always_terminates(ctx, el.node, expected))
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
    IpIndex lt = visit_child(ctx, BinExpr_lhs(&be));
    IpIndex rt = visit_child(ctx, BinExpr_rhs(&be));
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
    case SK_DOT_DOT:
      // Range expressions only carry a type INLINE inside a loop header
      // today (the loop handler routes around BIN_EXPR for this case).
      // Outside that position there is no Range value type yet —
      // surfaces as a clean error rather than a silent fold.
      db_emit(s, DIAG_ERROR, span_of(ctx, node),
              "range expressions are only allowed inline in a loop header "
              "(`loop (lo..hi) <i>`); stored Range values are not "
              "supported yet");
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
    SyntaxNode *callee = CallExpr_callee(&ce);
    SyntaxNode *arg_list = CallExpr_args(&ce);
    IpIndex callee_ty = callee ? type_of_expr(ctx, callee) : IP_NONE;
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
          if (bind.kind != SYNTAX_KIND_NONE)
            instantiate_callee = false;
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
    for (uint32_t i = 0; i < n_args; i++)
      (void)check_expr(ctx, args[i], key.fn_type.params[i]);
    release_arg_nodes(args, n_args);
    // Effects-4c — accumulate the callee's effect row.
    if (ctx->body_effect_row &&
        key.fn_type.effect_row.v != IP_NONE.v) {
      *ctx->body_effect_row =
          row_union(ctx, *ctx->body_effect_row, key.fn_type.effect_row);
    }
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
    if (enclosing_fn.idx != DEF_ID_NONE.idx) {
      const FnSignature *sig = db_query_fn_signature(s, enclosing_fn);
      IpIndex sigty = sig ? sig->type : IP_NONE;
      if (sigty.v != IP_NONE.v && ip_tag(&s->intern, sigty) == IP_TAG_FN_TYPE) {
        IpIndex ret_ty = ip_key(&s->intern, sigty).fn_type.ret;
        if (val) {
          // `return X;` — X must coerce to declared return. `return X`
          // in a void fn is rejected by check_expr's coerce diag
          // ("expected type 'void', found '%T'") without a special case.
          (void)check_expr(ctx, val, ret_ty);
        } else if (ret_ty.v != IP_VOID_TYPE.v) {
          // `return;` (no payload) in a non-void fn — hard error.
          // Naked `return;` is a valid guard-clause idiom for void fns
          // but never legal when a value is required.
          db_emit(s, DIAG_ERROR, span_of(ctx, node),
                  "non-void function (returns %T) requires a value in "
                  "`return`", ret_ty);
        }
      }
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
    SyntaxNode *capture = IfExpr_capture(&ie);
    SyntaxNode *then_b = IfExpr_then_branch(&ie);
    SyntaxNode *else_b = IfExpr_else_branch(&ie);
    bool had_else = (else_b != NULL);
    handle_if_cond(ctx, cond, capture);
    if (cond)    syntax_node_release(cond);
    if (capture) syntax_node_release(capture);
    IpIndex tt = then_b ? visit_child(ctx, then_b) : IP_VOID_TYPE;
    IpIndex et = had_else ? visit_child(ctx, else_b) : IP_VOID_TYPE;
    // Join the branches like switch arms do: unify_arith folds comptime↔
    // concrete and same-type (so `if c then 1 else x:i32` yields i32, not
    // void). A real mismatch or a missing else → void (if-as-statement).
    IpIndex u = had_else ? unify_arith(tt, et) : IP_NONE;
    return (u.v != IP_NONE.v) ? u : IP_VOID_TYPE;
  }

  // --- loop: cond (optional) + capture (optional) + body. Yields void.
  //
  // Capture handling depends on the cond's shape:
  //   - cond is a range expression (SK_BIN_EXPR with SK_DOT_DOT op):
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
        if (BinExpr_cast(cond, &be) && BinExpr_op_kind(&be) == SK_DOT_DOT) {
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
    // Static type of the loop expression:
    //   - infinite loop (no cond) with no reachable `break` → noreturn.
    //     The body either runs forever, returns, or panics. Block-tail
    //     check_expr accepts IP_NORETURN_TYPE against any expected
    //     type (coerce.c:615, the bottom-type rule), so non-void fns
    //     whose body ends in `loop { return X }` typecheck cleanly.
    //   - everything else (while-cond, range, while-let, or infinite
    //     with a reachable break) → void: control may fall through.
    bool noreturn_loop = !cond_present && body &&
                          !loop_has_reachable_break(body);
    if (capture) syntax_node_release(capture);
    if (body) {
      (void)type_of_expr(ctx, body);
      syntax_node_release(body);
    }
    return noreturn_loop ? IP_NORETURN_TYPE : IP_VOID_TYPE;
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
  // Effects-4d — `handle action with handler { ... }`.
  //
  // Discharge is done by UNIFICATION, not subtraction — so a row-
  // polymorphic action like `f : fn() <..e> T` works: we unify
  // `action_row ≡ ⟨targeted | μ_residual⟩`, which binds μ to absorb
  // the targeted effect even when the action's labels are unknown
  // up front.
  //
  // The accumulator dance:
  //   1. Save the outer body row.
  //   2. Swap in an empty sub-accumulator; type the action.
  //   3. Unify action_row ≡ ⟨targeted | fresh μ_residual⟩.
  //   4. Restore the OUTER accumulator before typing clause bodies —
  //      a clause that itself calls a `<log>` op must bubble that
  //      effect up to the surrounding fn, not the action's row
  //      (Gemini Trap 4).
  //   5. After clauses, union the resolved μ_residual into outer.
  //
  // Trap 3 (early-return safety): the accumulator restore MUST run
  // on every exit path. We use a single `goto restore` label.
  case SK_HANDLE_EXPR: {
    HandleExpr he;
    if (!HandleExpr_cast(node, &he))
      return IP_NONE;
    SyntaxNode *targeted_node = HandleExpr_effect(&he);
    SyntaxNode *action = HandleExpr_body(&he);
    SyntaxNode *handler = HandleExpr_handler(&he);

    IpIndex action_ty = IP_NONE;
    // No body accumulator → fall back to plain typing (no discharge
    // bookkeeping). This path also runs in standalone type_of_expr
    // tests; production fn bodies always supply ctx->body_effect_row.
    if (!ctx->body_effect_row) {
      if (action) action_ty = type_of_expr(ctx, action);
      if (handler) (void)type_of_expr(ctx, handler);
      if (targeted_node) syntax_node_release(targeted_node);
      if (action) syntax_node_release(action);
      if (handler) syntax_node_release(handler);
      return action_ty;
    }

    IpIndex outer = *ctx->body_effect_row;
    IpIndex residual_var = IP_EMPTY_EFFECT_ROW;
    bool restored = false;

    // (1)(2) Swap to a sub-accumulator and type the action.
    *ctx->body_effect_row = IP_EMPTY_EFFECT_ROW;
    if (action)
      action_ty = type_of_expr(ctx, action);
    IpIndex action_row = *ctx->body_effect_row;

    // (3) Discharge by unification. Build ⟨targeted | μ_residual⟩
    // and unify against action_row. row_unify binds row vars; the
    // outcome of μ_residual is read via row_resolve after the
    // clauses run (the clauses may bind it further if the polymorphic
    // action gets further instantiated).
    IpIndex targeted = build_effect_row(ctx, targeted_node);
    if (targeted_node) syntax_node_release(targeted_node);
    if (targeted.v == IP_NONE.v) {
      // Malformed targeted row already diag'd by build_effect_row.
      goto restore;
    }
    residual_var = ip_fresh_row_var(&s->intern);
    IpKey tk = ip_key(&s->intern, targeted);
    // Build ⟨ targeted's labels | μ_residual ⟩.
    IpIndex expected = row_intern(s, tk.effect_row.labels,
                                  tk.effect_row.n_labels, residual_var);
    if (!row_unify(ctx, action_row, expected)) {
      // Closed action row that doesn't contain the targeted effect.
      // This is the ONLY correct site for a "useless handle" diag —
      // the polymorphic case succeeds via row_unify binding the
      // action's row var.
      db_emit(s, DIAG_ERROR, span_of(ctx, node),
              "handle discharges effect %T but action's row %T cannot "
              "satisfy it",
              targeted, action_row);
      // Continue — still type the handler so clause diags are useful.
    }

  restore:
    // (4) Restore the outer accumulator BEFORE typing clauses.
    *ctx->body_effect_row = outer;
    restored = true;
    (void)restored; // explicit marker that the unconditional restore ran

    // Type the handler in the outer frame so clause-body effects
    // accumulate to the surrounding fn (Gemini Trap 4).
    if (handler)
      (void)type_of_expr(ctx, handler);
    if (action) syntax_node_release(action);
    if (handler) syntax_node_release(handler);

    // (5) Union the resolved residual into the outer accumulator.
    // If residual_var was never minted (early-return path), this is
    // IP_EMPTY_EFFECT_ROW and row_union is a no-op.
    IpIndex resolved_residual = row_resolve(ctx, residual_var);
    if (resolved_residual.v != IP_EMPTY_EFFECT_ROW.v &&
        resolved_residual.v != IP_NONE.v) {
      *ctx->body_effect_row =
          row_union(ctx, *ctx->body_effect_row, resolved_residual);
    }
    // TODO(Phase 4d follow-up): inject a synthetic `resume` binding
    // into each SK_OP_CLAUSE's body scope with type
    // `fn(op_ret_type) <μ_residual> action_ty`. Mechanism is the
    // synthetic-DefId path the virtual_collision keep-zone test
    // exercises. Not yet needed by the Phase 4 audit gates (they
    // don't call resume); will be required by Phase 6 fixtures that
    // do.
    return action_ty;
  }

  // Effects-4e — bare `handler { ... }` value. Types as
  // IPK_HANDLER_TYPE. Clause bodies type in the current (outer) ctx
  // so their effects bubble up the same way an SK_HANDLE_EXPR's
  // clauses do; the discharge happens at the eventual `handle`
  // site, not here.
  case SK_HANDLER_EXPR: {
    HandlerExpr hr;
    if (!HandlerExpr_cast(node, &hr))
      return IP_NONE;
    IpIndex ret_ty = IP_VOID_TYPE;
    IpIndex eff_row = IP_EMPTY_EFFECT_ROW;
    // Read the optional handler-level effect row annotation.
    SyntaxNode *eff_node = HandlerExpr_effect(&hr);
    if (eff_node) {
      eff_row = build_effect_row(ctx, eff_node);
      if (eff_row.v == IP_NONE.v)
        eff_row = IP_EMPTY_EFFECT_ROW;
      syntax_node_release(eff_node);
    }
    // Walk clauses and type their bodies. Each clause body's
    // effects accumulate into ctx->body_effect_row (the outer
    // frame), matching the [H] rule's residual-row context.
    uint32_t nch = syntax_node_num_children(node);
    for (uint32_t i = 0; i < nch; i++) {
      SyntaxElement el = syntax_node_child_or_token(node, i);
      if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
        if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
          syntax_token_release(el.token);
        continue;
      }
      SyntaxKind ck = syntax_node_kind(el.node);
      if (ck == SK_OP_CLAUSE || ck == SK_RETURN_CLAUSE) {
        uint32_t cn = syntax_node_num_children(el.node);
        // The clause body is the last non-token expression child.
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
          // The return clause's body type IS the handler's ret type.
          if (ck == SK_RETURN_CLAUSE && body_ty.v != IP_NONE.v)
            ret_ty = body_ty;
          syntax_node_release(body);
        }
      }
      syntax_node_release(el.node);
    }
    IpKey hk = {.kind = IPK_HANDLER_TYPE,
                .handler_type = {.effect = eff_row, .ret = ret_ty}};
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
  // we silently absorb to keep the diagnostic stream focused on root
  // causes. Inner errors specific to THIS node will surface naturally
  // on the next compile after the user fixes the upstream issue.
  //
  // Item #20: both poisoned sentinels absorb silently. IP_NONE means
  // "no expected type" (upstream lost it); IP_ERROR_TYPE means
  // "upstream already diagnosed the failure" — neither warrants a
  // fresh cascade diag from this synth-then-coerce frame.
  if (expected.v == IP_NONE.v || ip_is_error(expected))
    return true;

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

    if (k == SK_BLOCK_STMT || k == SK_BLOCK_EXPR) {
      BlockStmt bs = {.syntax = node}; // kind validated above
      {
        SyntaxNode *stmts = BlockStmt_stmts(&bs);
        if (!stmts) {
          if (coerce(ctx, NULL, IP_VOID_TYPE, expected).kind != COERCE_OK) {
            // Custom diag — "empty block returns void" is more specific
            // than coerce_or_diag's "expected type '%T', found 'void'".
            db_emit(s, DIAG_ERROR, span_of(ctx, node),
                    "empty block returns void; expected type '%T'", expected);
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
          if (coerce(ctx, NULL, IP_VOID_TYPE, expected).kind != COERCE_OK) {
            // Custom diag — "empty block returns void" is more specific
            // than coerce_or_diag's "expected type '%T', found 'void'".
            db_emit(s, DIAG_ERROR, span_of(ctx, node),
                    "empty block returns void; expected type '%T'", expected);
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
        SyntaxNode *capture = IfExpr_capture(&ie);
        SyntaxNode *then_b = IfExpr_then_branch(&ie);
        SyntaxNode *else_b = IfExpr_else_branch(&ie);
        bool ok = true;
        handle_if_cond(ctx, cond, capture);
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
    struct GreenNode *groot = db_read_file_ast(ctx, e.file);
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

  // F1 (Phase P audit) — refresh the BodyAstIdMap against the CURRENT
  // tree before sema walks the body. Without this, an FN_SIGNATURE-only
  // edit (e.g. signature whitespace) would let BODY_SCOPES salsa-cut
  // off while INFER_BODY re-runs, leaving rev populated with stale
  // byte-range hashes. Every body_ast_id_lookup would miss → every
  // emit falls back to FILE_RAW → byte ranges go stale on the next
  // sibling edit (the bug Gemini's audit caught). Cost: one body
  // preorder walk; amortized into the body walk this query already does.
  if (lambda_node)
    body_ast_id_map_refresh(s, def, lambda_node);

  // Phase P S6 — open the per-fn DiagBundle sink BEFORE the body walk,
  // and reset the bundle so this generation's emits start clean. Cache
  // the BodyAstIdMap + DeclKey on the SemaCtx so span_of() can build
  // structural DIAG_ANCHOR_BODY anchors that survive sibling reparse.
  // (Cache is keyed by the active fn — for nested fns we'd need a
  // walk-time push/pop, but ore doesn't have nested fns today.)
  DiagBundle *body_bundle = infer_body_diags_slot(s, def);
  if (body_bundle)
    diag_bundle_reset(body_bundle);
  DiagSink body_sink = infer_body_sink_open(s, def);
  db_query_frame_set_sink(ctx, body_bundle ? &body_sink : NULL);

  // F9 (Phase P audit) — cache the BodyAstIdMap pointer for span_of's
  // hot path. Lifetime: stable for this INFER_BODY frame because
  // (1) body_ast_id_maps is only grown by db_def_set_kind, never
  // during the body walk, and (2) PagedVec pages don't relocate once
  // a row exists (paged_get returns a stable interior pointer). If a
  // future refactor allows nested queries to push new fn rows during
  // the walk, re-fetch the pointer or hold a row index instead.
  // Producer-side self-data — INFER_BODY owns `def`'s body_ast_id_map
  // row. The untracked accessor in capability.c encodes the
  // KIND_FUNCTION + row-in-range gate (returns NULL on either miss);
  // safe because we're computing the slot ourselves in this frame.
  const BodyAstIdMap *body_map = db_get_fn_body_ast_id_map_untracked(s, def);
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
      // row_subst lives next to it so unify-time bindings (from
      // discharge in SK_HANDLE_EXPR or signature-check at the end) all
      // live on the same per-body frame.
      IpIndex body_row = IP_EMPTY_EFFECT_ROW;
      HashMap row_subst = {0};
      SemaCtx walk = {.s = s,
                      .file_green_root = NULL,
                      .nsid = nsid,
                      .enclosing_fn = def,
                      .file_local = e.file,
                      .types = &b,
                      .body_ast_map = body_map,
                      .body_decl_key = decl_key_id.idx,
                      .row_subst = &row_subst,
                      .body_effect_row = &body_row};
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
        // Phase B terminator gate. Non-void fns must end every path in
        // a value-producing terminator (return / noreturn callee / an
        // implicit-last expression). Drop-off-end with non-void
        // declared return is UB at codegen time — surface here. The
        // implicit-last case is recognized by block_always_terminates'
        // last-stmt branch: it's the same trailing slot check_expr at
        // infer.c:2185 already verified the type of.
        if (sig_is_fn && expected_ret.v != IP_VOID_TYPE.v &&
            expected_ret.v != IP_NORETURN_TYPE.v &&
            !block_always_terminates(&walk, body, expected_ret)) {
          db_emit(s, DIAG_ERROR, span_of(&walk, lambda_node),
                  "control reaches end of non-void function (returns %T) "
                  "without producing a value", expected_ret);
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
      if (sig_is_fn) {
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
        if (!row_unify(&walk, resolved_body, resolved_decl)) {
          db_emit(s, DIAG_ERROR, span_of(&walk, lambda_node),
                  "function declares effects %T but body performs %T",
                  resolved_decl, resolved_body);
        }
      }
      if (hashmap_is_initialized(&row_subst))
        hashmap_free(&row_subst);
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
