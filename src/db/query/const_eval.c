// Comptime const-folding + layout — F1 port of sema_legacy.
//
// Pure (non-memoized) for now: each call recurses through SK_REF_EXPR
// chains and re-evaluates referenced consts. Chains in fixtures are
// short (depth ≤ ~10), so the cost is negligible. If profiling shows
// hotness, wire a DB_QUERY_GUARD slot keyed on SyntaxNodePtr.

#include "const_eval.h"
#include "capability.h" // db_read_file_ast — dep-recording FILE_AST read
#include "coerce.h"  // shared int_bits / is_signed_int / is_unsigned_int

#include "../diag/ast_id.h"  // DeclAstIdMap + decl_ast_id_lookup (Fix B anchor)
#include "../diag/diag.h"
#include "../../ast/ast_decl.h"
#include "../../ast/ast_expr.h"
#include "../../ast/ast_stmt.h"
#include "../../support/data_structure/stringpool.h"
#include "../../syntax/syntax.h"
#include "../../syntax/syntax_kind.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Externs (no per-query headers; type.c / scope.c style).
extern uint64_t      parse_int_literal(SyntaxToken *tok);
extern TopLevelEntry db_query_top_level_entry(db_query_ctx *ctx,
                                              NamespaceId nsid, StrId name);
extern DefId         db_query_def_identity(db_query_ctx *ctx,
                                           NamespaceId nsid, AstId id);
extern IpIndex       db_query_type_of_def(db_query_ctx *ctx, DefId def);

// ============================================================================
// J1 — cycle stack + ConstCtx (the per-top-call frame state).
//
// eval_ref recurses into the referenced binding's RHS via eval_inner.
// A self-referential bind (`MAX :: MAX`) or a mutual cycle (`A :: B;
// B :: A`) would otherwise infinitely recurse and overflow the stack.
//
// A tiny fixed-size stack of (FileId, node-ptr hash) for the bindings
// currently under evaluation. eval_ref consults it before recursing;
// a hit emits a diag once and returns CONST_NONE. The cap is generous
// (typical chains are < 10) but defends pathological inputs.
//
// Stack lives on the C stack of the public db_const_eval entrypoint —
// each top-level call gets a fresh stack. Cycles only form through
// eval_ref's binding-chain recursion; leaf evaluators (literal, bin,
// prefix) don't reach across bindings.
// ============================================================================

#define ORE_CONST_CYCLE_MAX 64

typedef struct {
  FileId   file;
  uint64_t hash; // syntax_node_ptr_hash(e.node_ptr)
} ConstCycleEntry;

typedef struct ConstCycle {
  ConstCycleEntry entries[ORE_CONST_CYCLE_MAX];
  uint32_t        count;
} ConstCycle;

// Per-top-call frame state. Bundles the immutable per-frame bits — the db,
// the caller-supplied diag-anchor handle — with the mutable cycle stack, so
// every eval_* helper takes a single `ConstCtx *ctx` instead of repeating
// (s, ..., stk) at every signature and call site. `fid` is NOT in here: it
// changes mid-recursion at the cross-file ref step (eval_ref / eval_field_expr
// pass `e.file` for the referenced binding's home file) and so stays a
// per-call param.
typedef struct ConstCtx {
  struct db          *s;
  ConstDiagAnchorCtx  anchor;
  ConstCycle          stk;
} ConstCtx;

static bool cycle_contains(const ConstCycle *stk, FileId f, uint64_t h) {
  for (uint32_t i = 0; i < stk->count; i++)
    if (stk->entries[i].file.idx == f.idx && stk->entries[i].hash == h)
      return true;
  return false;
}

// Forward decls — file-internal helpers used cross-cutting in eval_bin
// + eval_switch; defined further down. Keep these at the top so the
// existing one-pass layout (helpers before dispatch) still compiles.
static bool const_values_equal(ConstValue a, ConstValue b);
static ConstValue eval_inner(ConstCtx *ctx, FileId fid, SyntaxNode *node);

// #3: context-aware variant of eval_inner. Resolves a bare `.variant`
// SK_ENUM_REF_EXPR against `enum_ctx` (because const_eval has no
// expected-type plumbing — eval_inner alone can't fold a bare enum
// ref). For every other node kind, equivalent to eval_inner.
static ConstValue eval_with_enum_ctx(ConstCtx *ctx, FileId fid,
                                     SyntaxNode *node, DefId enum_ctx) {
  struct db *s = ctx->s;
  if (enum_ctx.idx != 0 && node &&
      syntax_node_kind(node) == SK_ENUM_REF_EXPR) {
    EnumRefExpr er;
    if (!EnumRefExpr_cast(node, &er))
      return (ConstValue){.kind = CONST_NONE};
    SyntaxToken *vt = EnumRefExpr_variant(&er);
    if (!vt)
      return (ConstValue){.kind = CONST_NONE};
    StrId vname =
        pool_intern(&s->strings, syntax_token_text(vt),
                    syntax_token_text_range(vt).length);
    syntax_token_release(vt);
    uint32_t nv = 0;
    const EnumVariantEntry *vs = db_enum_variants(s, enum_ctx, &nv);
    for (uint32_t k = 0; k < nv; k++) {
      if (vs[k].name.idx == vname.idx) {
        return (ConstValue){
            .kind = CONST_ENUM_VARIANT,
            .enum_variant = {enum_ctx, k}};
      }
    }
    return (ConstValue){.kind = CONST_NONE};
  }
  return eval_inner(ctx, fid, node);
}

// ============================================================================
// Literal extraction
// ============================================================================

static ConstValue none_value(void) {
  return (ConstValue){.kind = CONST_NONE};
}

// Build the diag anchor for a const_eval emit site. Prefers DIAG_ANCHOR_BODY
// (drift-stable across sibling-prepend edits — the bug from
// docs/diag-anchor-audit.md Fix B) when the caller threaded a wrapper map
// covering `node`; falls back to `diag_anchor_of_node` (byte-frozen but at
// least token-relative) when the map is absent or misses `node`. Same
// lookup-then-fallback shape as span_of() in type.c / infer.c and the inline
// block in coerce.c's coerce_or_diag. One marker on the fallback line covers
// the 5 emit sites in eval_ref / eval_call / eval_field_expr.
static DiagAnchor const_eval_anchor(const ConstCtx *ctx, FileId fid,
                                    SyntaxNode *node) {
  if (ctx && ctx->anchor.decl_ast_map && node) {
    uint32_t rel;
    if (decl_ast_id_lookup(ctx->anchor.decl_ast_map, node, &rel))
      return diag_anchor_body((uint16_t)fid.idx,
                              (DeclKey)ctx->anchor.decl_key, (RelAstId)rel);
  }
  return diag_anchor_of_node((uint16_t)fid.idx, node); // LINT_FILE_RAW_OK: const_eval fallback when decl_ast_map miss or caller had no SemaCtx
}

// Build the diag-anchor handle for a cross-file recursion target. Used by
// eval_ref / eval_field_expr at the `e.file` cross-file step: the active
// anchor's wrapper map belongs to the OUTER caller's decl and won't cover
// the recursed file's nodes (lookup miss → FILE_RAW fallback → drift on
// sibling-prepend edits to the referenced file). Swapping to the target
// decl's own (map, key) lets a diag emitted during the recursion stay
// BODY-anchored against the right wrapper.
//
// Calling db_query_type_of_def ensures the target's DeclAstIdMap is built
// (it's populated by TYPE_OF_DECL / FN_SIGNATURE compute bodies) and
// records a TYPE_OF_DECL dep edge from the active query frame on the
// target — same dep edge eval_call already registers when it consults the
// callee's fn type, so this is dedup-safe.
//
// Returns {NULL, 0} if the target can't be resolved or its map isn't built;
// the caller restores the outer anchor on return, and const_eval_anchor's
// own fallback handles the NULL-map case (same correctness as today, just
// not body-anchored).
static ConstDiagAnchorCtx const_eval_anchor_for_target(struct db *s,
                                                       DefId target,
                                                       uint32_t target_ast_id) {
  if (target.idx == 0)
    return (ConstDiagAnchorCtx){0};
  (void)db_query_type_of_def(s, target);
  const DeclAstIdMap *m = db_get_decl_ast_id_map_untracked(s, target);
  return (ConstDiagAnchorCtx){ .decl_ast_map = m,
                               .decl_key     = target_ast_id };
}

// Parse a float literal token's text — handles `_` separators and the
// `1.5e10` form via strtod. Returns false on parse failure.
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

static ConstValue eval_literal(SyntaxNode *node) {
  Literal lit;
  if (!Literal_cast(node, &lit))
    return none_value();
  SyntaxKind k = Literal_kind(&lit);
  if (k == SK_TRUE_KW)
    return (ConstValue){.kind = CONST_BOOL, .bool_val = true};
  if (k == SK_FALSE_KW)
    return (ConstValue){.kind = CONST_BOOL, .bool_val = false};
  SyntaxToken *tok = Literal_token(&lit);
  if (!tok)
    return none_value();
  ConstValue result = none_value();
  if (k == SK_INT_LIT) {
    // J2: parse via strtoull (uint64_t domain). Values > INT64_MAX get
    // stored as negative int64_t bit-patterns and marked is_unsigned
    // so fits_in / to_str / bin-ops can re-interpret correctly. Literal
    // overflow (value exceeds u64) is left as CONST_NONE — strtoull
    // saturates at ULLONG_MAX and sets errno; we report it by NOT
    // returning a foldable value, which falls through to the regular
    // "not a comptime expression" path.
    errno = 0;
    uint64_t u = parse_int_literal(tok);
    if (errno == ERANGE) {
      // Literal exceeds u64 — leave as CONST_NONE.
      syntax_token_release(tok);
      return none_value();
    }
    result.kind = CONST_INT;
    result.int_val = (int64_t)u;
    result.is_unsigned = (u > (uint64_t)INT64_MAX);
  } else if (k == SK_FLOAT_LIT) {
    double v = 0;
    if (parse_float_literal_text(tok, &v)) {
      result.kind = CONST_FLOAT;
      result.float_val = v;
    }
  }
  syntax_token_release(tok);
  return result;
}

// ============================================================================
// Bin-op arithmetic (port of sema_legacy/comptime/bin_ops/bin_ops.c)
// ============================================================================

// J2 propagation rule: result is_unsigned iff either operand is_unsigned.
// Bit-identical ops (add/sub/mul/shl) just propagate the flag; div/mod/shr
// re-select signed vs unsigned semantics on the flag.
static bool either_unsigned(ConstValue l, ConstValue r) {
  return (l.kind == CONST_INT && l.is_unsigned) ||
         (r.kind == CONST_INT && r.is_unsigned);
}

static ConstValue bin_add(ConstValue l, ConstValue r) {
  if (l.kind == CONST_INT && r.kind == CONST_INT) {
    bool u = either_unsigned(l, r);
    if (u) {
      uint64_t a = (uint64_t)l.int_val, b = (uint64_t)r.int_val, v;
      if (__builtin_add_overflow(a, b, &v))
        return none_value();
      return (ConstValue){.kind = CONST_INT, .is_unsigned = true,
                          .int_val = (int64_t)v};
    }
    int64_t v;
    if (__builtin_add_overflow(l.int_val, r.int_val, &v))
      return none_value();
    return (ConstValue){.kind = CONST_INT, .int_val = v};
  }
  if (l.kind == CONST_FLOAT && r.kind == CONST_FLOAT) {
    double v = l.float_val + r.float_val;
    if (!isfinite(v))
      return none_value();
    return (ConstValue){.kind = CONST_FLOAT, .float_val = v};
  }
  return none_value();
}

static ConstValue bin_sub(ConstValue l, ConstValue r) {
  if (l.kind == CONST_INT && r.kind == CONST_INT) {
    bool u = either_unsigned(l, r);
    if (u) {
      uint64_t a = (uint64_t)l.int_val, b = (uint64_t)r.int_val, v;
      if (__builtin_sub_overflow(a, b, &v))
        return none_value();
      return (ConstValue){.kind = CONST_INT, .is_unsigned = true,
                          .int_val = (int64_t)v};
    }
    int64_t v;
    if (__builtin_sub_overflow(l.int_val, r.int_val, &v))
      return none_value();
    return (ConstValue){.kind = CONST_INT, .int_val = v};
  }
  if (l.kind == CONST_FLOAT && r.kind == CONST_FLOAT) {
    double v = l.float_val - r.float_val;
    if (!isfinite(v))
      return none_value();
    return (ConstValue){.kind = CONST_FLOAT, .float_val = v};
  }
  return none_value();
}

static ConstValue bin_mul(ConstValue l, ConstValue r) {
  if (l.kind == CONST_INT && r.kind == CONST_INT) {
    bool u = either_unsigned(l, r);
    if (u) {
      uint64_t a = (uint64_t)l.int_val, b = (uint64_t)r.int_val, v;
      if (__builtin_mul_overflow(a, b, &v))
        return none_value();
      return (ConstValue){.kind = CONST_INT, .is_unsigned = true,
                          .int_val = (int64_t)v};
    }
    int64_t v;
    if (__builtin_mul_overflow(l.int_val, r.int_val, &v))
      return none_value();
    return (ConstValue){.kind = CONST_INT, .int_val = v};
  }
  if (l.kind == CONST_FLOAT && r.kind == CONST_FLOAT) {
    double v = l.float_val * r.float_val;
    if (!isfinite(v))
      return none_value();
    return (ConstValue){.kind = CONST_FLOAT, .float_val = v};
  }
  return none_value();
}

static ConstValue bin_div(ConstValue l, ConstValue r) {
  if (l.kind == CONST_INT && r.kind == CONST_INT) {
    bool u = either_unsigned(l, r);
    if (u) {
      uint64_t a = (uint64_t)l.int_val, b = (uint64_t)r.int_val;
      if (b == 0)
        return none_value();
      return (ConstValue){.kind = CONST_INT, .is_unsigned = true,
                          .int_val = (int64_t)(a / b)};
    }
    if (r.int_val == 0)
      return none_value();
    if (l.int_val == INT64_MIN && r.int_val == -1)
      return none_value(); // overflow
    return (ConstValue){.kind = CONST_INT, .int_val = l.int_val / r.int_val};
  }
  if (l.kind == CONST_FLOAT && r.kind == CONST_FLOAT) {
    double v = l.float_val / r.float_val;
    if (!isfinite(v))
      return none_value();
    return (ConstValue){.kind = CONST_FLOAT, .float_val = v};
  }
  return none_value();
}

static ConstValue bin_mod(ConstValue l, ConstValue r) {
  if (l.kind != CONST_INT || r.kind != CONST_INT)
    return none_value();
  bool u = either_unsigned(l, r);
  if (u) {
    uint64_t a = (uint64_t)l.int_val, b = (uint64_t)r.int_val;
    if (b == 0)
      return none_value();
    return (ConstValue){.kind = CONST_INT, .is_unsigned = true,
                        .int_val = (int64_t)(a % b)};
  }
  if (r.int_val == 0)
    return none_value();
  if (l.int_val == INT64_MIN && r.int_val == -1)
    return (ConstValue){.kind = CONST_INT, .int_val = 0};
  return (ConstValue){.kind = CONST_INT, .int_val = l.int_val % r.int_val};
}

static ConstValue bin_shl(ConstValue l, ConstValue r) {
  if (l.kind != CONST_INT || r.kind != CONST_INT)
    return none_value();
  if (r.int_val < 0 || r.int_val >= 64)
    return none_value();
  // Shift-left is bit-identical for signed/unsigned. Propagate unsigned-
  // ness so subsequent fits-in / div / shr keep the right view.
  return (ConstValue){.kind = CONST_INT, .is_unsigned = either_unsigned(l, r),
                      .int_val = l.int_val << r.int_val};
}

static ConstValue bin_shr(ConstValue l, ConstValue r) {
  if (l.kind != CONST_INT || r.kind != CONST_INT)
    return none_value();
  if (r.int_val < 0 || r.int_val >= 64)
    return none_value();
  // J2: arithmetic vs logical shift selected by l's signedness. Unsigned
  // source → zero-extend (the existing cast); signed → sign-extend via
  // signed shift.
  if (l.is_unsigned) {
    uint64_t a = (uint64_t)l.int_val;
    return (ConstValue){.kind = CONST_INT, .is_unsigned = true,
                        .int_val = (int64_t)(a >> r.int_val)};
  }
  return (ConstValue){.kind = CONST_INT,
                      .int_val = l.int_val >> r.int_val};
}

// ============================================================================
// Bin/Prefix dispatch
// ============================================================================

static ConstValue eval_bin(ConstCtx *ctx, FileId fid, SyntaxNode *node) {
  BinExpr be;
  if (!BinExpr_cast(node, &be))
    return none_value();
  SyntaxNode *lhs = BinExpr_lhs(&be);
  SyntaxNode *rhs = BinExpr_rhs(&be);
  SyntaxKind opk = BinExpr_op_kind(&be);
  ConstValue l = lhs ? eval_inner(ctx, fid, lhs) : none_value();
  ConstValue r = rhs ? eval_inner(ctx, fid, rhs) : none_value();

  // #3 equality on enum variants needs the OTHER side's enum_def to
  // resolve a bare `.variant`. If one side folded to CONST_ENUM_VARIANT
  // and the other to CONST_NONE (likely a bare SK_ENUM_REF_EXPR with no
  // standalone enum context), retry the bare side with the enum_def
  // from the typed side. This is what makes `@build.mode == .debug`
  // fold to CONST_BOOL.
  if (opk == SK_EQ_EQ || opk == SK_BANG_EQ) {
    if (l.kind == CONST_NONE && r.kind == CONST_ENUM_VARIANT && lhs)
      l = eval_with_enum_ctx(ctx, fid, lhs, r.enum_variant.enum_def);
    if (r.kind == CONST_NONE && l.kind == CONST_ENUM_VARIANT && rhs)
      r = eval_with_enum_ctx(ctx, fid, rhs, l.enum_variant.enum_def);
  }

  if (lhs)
    syntax_node_release(lhs);
  if (rhs)
    syntax_node_release(rhs);
  if (l.kind == CONST_NONE || r.kind == CONST_NONE)
    return none_value();
  switch (opk) {
  case SK_PLUS:    return bin_add(l, r);
  case SK_MINUS:   return bin_sub(l, r);
  case SK_STAR:    return bin_mul(l, r);
  case SK_SLASH:   return bin_div(l, r);
  case SK_PERCENT: return bin_mod(l, r);
  case SK_SHL:     return bin_shl(l, r);
  case SK_SHR:     return bin_shr(l, r);
  case SK_EQ_EQ:
    return (ConstValue){.kind = CONST_BOOL,
                        .bool_val = const_values_equal(l, r)};
  case SK_BANG_EQ:
    return (ConstValue){.kind = CONST_BOOL,
                        .bool_val = !const_values_equal(l, r)};
  default:         return none_value();
  }
}

static ConstValue eval_prefix(ConstCtx *ctx, FileId fid, SyntaxNode *node) {
  PrefixExpr pe;
  if (!PrefixExpr_cast(node, &pe))
    return none_value();
  SyntaxNode *operand = PrefixExpr_operand(&pe);
  SyntaxKind opk = PrefixExpr_op_kind(&pe);
  ConstValue v = operand ? eval_inner(ctx, fid, operand) : none_value();
  if (operand)
    syntax_node_release(operand);
  if (v.kind == CONST_NONE)
    return v;
  switch (opk) {
  case SK_MINUS:
    if (v.kind == CONST_INT) {
      // J2: an unsigned operand > INT64_MAX cannot be negated into a
      // valid int64_t. (-(2^63) wraps to itself; -(2^63+1) doesn't fit.)
      if (v.is_unsigned && (uint64_t)v.int_val > (uint64_t)INT64_MAX)
        return none_value();
      if (!v.is_unsigned && v.int_val == INT64_MIN)
        return none_value();
      // Negation produces signed semantics — clear is_unsigned.
      return (ConstValue){.kind = CONST_INT, .int_val = -v.int_val};
    }
    if (v.kind == CONST_FLOAT)
      return (ConstValue){.kind = CONST_FLOAT, .float_val = -v.float_val};
    return none_value();
  case SK_TILDE:
    if (v.kind == CONST_INT)
      return (ConstValue){.kind = CONST_INT, .is_unsigned = v.is_unsigned,
                          .int_val = ~v.int_val};
    return none_value();
  case SK_BANG:
    if (v.kind == CONST_BOOL)
      return (ConstValue){.kind = CONST_BOOL, .bool_val = !v.bool_val};
    return none_value();
  default:
    return none_value();
  }
}

// ============================================================================
// SK_REF_EXPR — chain folding via top-level resolution
// ============================================================================
//
// Resolves `name` in the namespace's items list (no scope-chain walk —
// const refs at top level only). If the found item is a `::` const-bind,
// recurse on its value expression. Forward refs / non-const binds /
// missing names all return CONST_NONE.

static ConstValue eval_ref(ConstCtx *ctx, FileId fid, SyntaxNode *node) {
  struct db *s = ctx->s;
  ConstCycle *stk = &ctx->stk;
  RefExpr r;
  if (!RefExpr_cast(node, &r))
    return none_value();
  SyntaxToken *nt = RefExpr_name(&r);
  if (!nt)
    return none_value();
  StrId name = pool_intern(&s->strings, syntax_token_text(nt),
                           syntax_token_text_range(nt).length);
  syntax_token_release(nt);
  if (name.idx == 0)
    return none_value();

  // top_level_entry is namespace-scoped — derive nsid from fid.
  NamespaceId nsid = db_get_file_namespace(s, fid);
  TopLevelEntry e = db_query_top_level_entry(s, nsid, name);
  if (e.node_ptr.kind == SYNTAX_KIND_NONE)
    return none_value();

  // J1: cycle gate. The binding is identified by (file, node-ptr-hash);
  // if it's already on the recursion stack we have a self-ref or mutual
  // cycle (`MAX :: MAX`, `A :: B; B :: A`). Emit one diag, return NONE
  // for this leaf — outer evaluators see CONST_NONE and stop folding.
  uint64_t entry_hash = syntax_node_ptr_hash(e.node_ptr);
  if (cycle_contains(stk, e.file, entry_hash)) {
    db_emit(s, DIAG_ERROR, const_eval_anchor(ctx, fid, node),
            "circular const dependency through '%S'", name);
    return none_value();
  }
  if (stk->count >= ORE_CONST_CYCLE_MAX) {
    db_emit(s, DIAG_ERROR, const_eval_anchor(ctx, fid, node),
            "const chain too deep (max %d)", ORE_CONST_CYCLE_MAX);
    return none_value();
  }

  // Resolve the bind wrapper from the green root + node_ptr, then
  // extract its value expression. SK_CONST_DECL = `::` bind; we
  // intentionally do NOT fold SK_VAR_DECL (`:=` is runtime-mutable).
  // db_read_file_ast records FILE_AST(e.file) dep on the active
  // query frame — caller is type-of-expr or its descendants, all in
  // a frame.
  struct GreenNode *groot = db_read_file_ast(s, e.file);
  if (!groot)
    return none_value();
  SyntaxTree *tree = syntax_tree_new(groot);
  SyntaxNode *rroot = syntax_tree_root(tree);
  SyntaxNode *wrapper = syntax_node_ptr_resolve(e.node_ptr, rroot);
  syntax_node_release(rroot);
  ConstValue result = none_value();
  if (wrapper && syntax_node_kind(wrapper) == SK_CONST_DECL) {
    ConstDef cd;
    if (ConstDef_cast(wrapper, &cd)) {
      SyntaxNode *val = ConstDef_value(&cd);
      if (val) {
        // J1: push the binding onto the cycle stack BEFORE recursing,
        // pop after. J3: recurse with e.file (the home file of the
        // referenced binding), NOT the caller's fid. Cross-file ref
        // chains now reach the right green root for nested type-
        // position lookups inside the referenced binding.
        // Fix B.2: swap the diag anchor to the referenced decl's own
        // (map, key) for the duration of the recursion so a diag
        // emitted inside e.file stays BODY-anchored against the right
        // wrapper instead of falling back to FILE_RAW (which drifts on
        // sibling-prepend edits to e.file).
        stk->entries[stk->count++] = (ConstCycleEntry){e.file, entry_hash};
        DefId t_def = db_query_def_identity(s, nsid, e.id);
        ConstDiagAnchorCtx saved_anchor = ctx->anchor;
        ctx->anchor = const_eval_anchor_for_target(s, t_def, e.id.idx);
        result = eval_inner(ctx, e.file, val);
        ctx->anchor = saved_anchor;
        stk->count--;
        syntax_node_release(val);
      }
    }
  }
  if (wrapper)
    syntax_node_release(wrapper);
  syntax_tree_free(tree);
  return result;
}

// ============================================================================
// @sizeOf / @alignOf via layout
// ============================================================================

extern IpIndex resolve_type_expr_from_const_eval(struct db *s, FileId fid,
                                                 SyntaxNode *node);
// ↑ Helper provided in infer.c so we can call resolve_type_expr without
// rebuilding a full SemaCtx here. See infer.c for the impl.

static ConstValue eval_builtin(ConstCtx *ctx, FileId fid, SyntaxNode *node) {
  struct db *s = ctx->s;
  // @sizeOf/@alignOf args are type-position only — no value-position
  // recursion through eval_inner, so the cycle stack is unused here.
  // Kept on the signature for shape uniformity with siblings.
  BuiltinExpr be;
  if (!BuiltinExpr_cast(node, &be))
    return none_value();
  SyntaxToken *nt = BuiltinExpr_name(&be);
  if (!nt)
    return none_value();
  StrId name = pool_intern(&s->strings, syntax_token_text(nt),
                           syntax_token_text_range(nt).length);
  syntax_token_release(nt);

  bool is_sizeof  = (name.idx == s->names.SIZEOF.idx);
  bool is_alignof = (name.idx == s->names.ALIGNOF.idx);
  bool is_import  = (name.idx == s->names.IMPORT.idx);
  if (!is_sizeof && !is_alignof && !is_import)
    return none_value();

  // @import("...") — fold to CONST_NAMESPACE if the path is a pre-
  // admitted virtual name (e.g. db_init's "builtin"). Disk imports
  // aren't comptime-foldable today; sema's BUILTIN_IMPORT handler
  // resolves those via workspace_resolve_import for type-side use.
  // Without this arm, `builtin :: @import("builtin")` followed by
  // `builtin.os` fails to fold because eval_ref → eval_inner on the
  // @import RHS returns CONST_NONE.
  if (is_import) {
    (void)fid;
    SyntaxNode *args = BuiltinExpr_args(&be);
    if (!args)
      return none_value();
    SyntaxToken *str_tok = NULL;
    uint32_t total = syntax_node_num_children(args);
    for (uint32_t i = 0; i < total; i++) {
      SyntaxElement el = syntax_node_child_or_token(args, i);
      if (el.kind == SYNTAX_ELEM_NODE && el.node) {
        // First arg's only child should be the SK_STRING_LIT token —
        // pull through one level.
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
    syntax_node_release(args);
    if (!str_tok)
      return none_value();
    // Strip the surrounding quotes for the interned name lookup.
    const char *txt = syntax_token_text(str_tok);
    uint32_t len = syntax_token_text_range(str_tok).length;
    syntax_token_release(str_tok);
    if (len < 2 || txt[0] != '"' || txt[len - 1] != '"')
      return none_value();
    StrId path = pool_intern(&s->strings, txt + 1, len - 2);
    if (path.idx == 0)
      return none_value();
    void *v = hashmap_get(&s->virtual_by_name, (uint64_t)path.idx);
    if (!v)
      return none_value();
    SourceId vsrc = {.idx = (uint32_t)(uintptr_t)v};
    FileId vfid = db_lookup_file_by_source(s, vsrc);
    if (vfid.idx == 0)
      return none_value();
    NamespaceId vns = db_get_file_namespace(s, vfid);
    if (vns.idx == 0)
      return none_value();
    return (ConstValue){.kind = CONST_NAMESPACE, .nsid = vns};
  }

  // Pull first NODE arg out of SK_ARG_LIST (sizeof/alignof type-arg path).
  SyntaxNode *arg_list = BuiltinExpr_args(&be);
  if (!arg_list)
    return none_value();
  SyntaxNode *type_arg = NULL;
  uint32_t total = syntax_node_num_children(arg_list);
  for (uint32_t i = 0; i < total; i++) {
    SyntaxElement el = syntax_node_child_or_token(arg_list, i);
    if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      if (!type_arg) {
        type_arg = el.node;
      } else {
        syntax_node_release(el.node);
      }
    } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      syntax_token_release(el.token);
    }
  }
  syntax_node_release(arg_list);
  if (!type_arg)
    return none_value();

  IpIndex t = resolve_type_expr_from_const_eval(s, fid, type_arg);
  syntax_node_release(type_arg);
  if (!ip_index_is_valid(t))
    return none_value();

  OreLayout L = db_layout_of_type(s, t);
  if (!L.is_known)
    return none_value();
  return (ConstValue){.kind = CONST_INT,
                      .int_val = (int64_t)(is_sizeof ? L.size : L.align)};
}

// ============================================================================
// F2: if / switch / block
// ============================================================================

// `if (cond) then else` with comptime cond → fold to taken branch.
//
// Tripwire: after the grammar pivot, the cond slot is pure expression —
// SK_VAR_DECL / SK_CONST_DECL cannot appear here. If one ever does, the
// `name :=` peek-ahead was reintroduced into parse_expr. Asserts loudly
// rather than silently folding to none_value via eval_inner's default.
static ConstValue eval_if(ConstCtx *ctx, FileId fid, SyntaxNode *node) {
  IfExpr ie;
  if (!IfExpr_cast(node, &ie))
    return none_value();
  SyntaxNode *cond = IfExpr_condition(&ie);
  SyntaxNode *then_b = IfExpr_then_branch(&ie);
  SyntaxNode *else_b = IfExpr_else_branch(&ie);
  assert(!cond || (syntax_node_kind(cond) != SK_VAR_DECL &&
                   syntax_node_kind(cond) != SK_CONST_DECL));
  ConstValue cv = cond ? eval_inner(ctx, fid, cond) : none_value();
  ConstValue result = none_value();
  if (cv.kind == CONST_BOOL) {
    SyntaxNode *taken = cv.bool_val ? then_b : else_b;
    if (taken)
      result = eval_inner(ctx, fid, taken);
  }
  if (cond)   syntax_node_release(cond);
  if (then_b) syntax_node_release(then_b);
  if (else_b) syntax_node_release(else_b);
  return result;
}

// `switch (scrut)` with comptime scrut → fold to matched arm. Walks
// arm children: every non-last node is a pattern, last is body.
// Pattern equality: literal patterns + bare `_` only (parity with
// sema_legacy's F1 surface; enum patterns deferred).
static bool pattern_is_underscore(SyntaxNode *p) {
  if (syntax_node_kind(p) != SK_LITERAL_EXPR)
    return false;
  uint32_t n = syntax_node_num_children(p);
  for (uint32_t i = 0; i < n; i++) {
    SyntaxElement el = syntax_node_child_or_token(p, i);
    if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      bool us = syntax_token_kind(el.token) == SK_UNDERSCORE;
      syntax_token_release(el.token);
      if (us) return true;
    } else if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      syntax_node_release(el.node);
    }
  }
  return false;
}

static bool const_values_equal(ConstValue a, ConstValue b) {
  if (a.kind != b.kind) return false;
  switch (a.kind) {
  case CONST_INT:   return a.int_val == b.int_val;
  case CONST_FLOAT: return a.float_val == b.float_val;
  case CONST_BOOL:  return a.bool_val == b.bool_val;
  // #3: namespace equality is identity on NamespaceId — in practice
  // namespaces never appear as switch patterns; kept for completeness.
  case CONST_NAMESPACE: return a.nsid.idx == b.nsid.idx;
  // #3: enum-variant equality is (enum_def, variant_idx) pairwise.
  // This is what powers `comptime switch (@target.os) { .macos => … }`:
  // the scrutinee folds to CONST_ENUM_VARIANT{os_enum, host_idx}; each
  // pattern `.macos` folds to CONST_ENUM_VARIANT{os_enum, macos_idx};
  // matching arm is picked when both DefId + variant_idx coincide.
  case CONST_ENUM_VARIANT:
    return a.enum_variant.enum_def.idx == b.enum_variant.enum_def.idx &&
           a.enum_variant.variant_idx == b.enum_variant.variant_idx;
  default:          return false;
  }
}

static ConstValue eval_switch(ConstCtx *ctx, FileId fid, SyntaxNode *node) {
  struct db *s = ctx->s;
  SwitchExpr se;
  if (!SwitchExpr_cast(node, &se))
    return none_value();
  SyntaxNode *scrut = SwitchExpr_scrutinee(&se);
  SyntaxNode *arms = SwitchExpr_arms(&se);
  ConstValue result = none_value();
  if (!scrut || !arms) {
    if (scrut) syntax_node_release(scrut);
    if (arms)  syntax_node_release(arms);
    return result;
  }
  ConstValue sv = eval_inner(ctx, fid, scrut);
  syntax_node_release(scrut);
  if (sv.kind == CONST_NONE) {
    syntax_node_release(arms);
    return result;
  }

  uint32_t n = syntax_node_num_children(arms);
  for (uint32_t i = 0; i < n && result.kind == CONST_NONE; i++) {
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
    // Walk arm children: each pattern is the node BEFORE the FATARROW
    // token, last node is the body. Same shape as the switch handler
    // in infer.c's infer_switch.
    uint32_t an = syntax_node_num_children(arm);
    SyntaxNode *prev = NULL;
    bool matched = false;
    SyntaxNode *body = NULL;
    for (uint32_t j = 0; j < an; j++) {
      SyntaxElement pel = syntax_node_child_or_token(arm, j);
      if (pel.kind == SYNTAX_ELEM_NODE && pel.node) {
        if (prev) {
          if (!matched) {
            if (pattern_is_underscore(prev)) {
              matched = true;
            } else {
              ConstValue pv;
              // #3: `.variant` patterns need the scrutinee's enum
              // context to resolve to a CONST_ENUM_VARIANT. eval_inner
              // alone can't fold them (no expected-type plumbing in
              // const_eval). When the scrutinee folded to an enum
              // variant, do an inline name match against its enum_def.
              if (sv.kind == CONST_ENUM_VARIANT &&
                  syntax_node_kind(prev) == SK_ENUM_REF_EXPR) {
                EnumRefExpr er;
                pv = none_value();
                if (EnumRefExpr_cast(prev, &er)) {
                  SyntaxToken *vt = EnumRefExpr_variant(&er);
                  if (vt) {
                    StrId vname = pool_intern(
                        &s->strings, syntax_token_text(vt),
                        syntax_token_text_range(vt).length);
                    syntax_token_release(vt);
                    uint32_t nv = 0;
                    const EnumVariantEntry *vs = db_enum_variants(
                        s, sv.enum_variant.enum_def, &nv);
                    for (uint32_t k = 0; k < nv; k++) {
                      if (vs[k].name.idx == vname.idx) {
                        pv = (ConstValue){
                            .kind = CONST_ENUM_VARIANT,
                            .enum_variant = {sv.enum_variant.enum_def, k}};
                        break;
                      }
                    }
                  }
                }
              } else {
                pv = eval_inner(ctx, fid, prev);
              }
              if (const_values_equal(pv, sv))
                matched = true;
            }
          }
          syntax_node_release(prev);
        }
        prev = pel.node;
      } else if (pel.kind == SYNTAX_ELEM_TOKEN && pel.token) {
        syntax_token_release(pel.token);
      }
    }
    body = prev; // last node = body
    if (matched && body)
      result = eval_inner(ctx, fid, body);
    if (body)
      syntax_node_release(body);
    syntax_node_release(arm);
  }
  syntax_node_release(arms);
  return result;
}

// Block tail: walk backwards through stmts, find the last non-binding
// expression, fold it. In-block `x :: 42` binds are reachable via the
// SK_REF_EXPR path if the tail references `x` (resolves via top_level_entry
// since block-local `::` binds aren't currently namespace-injected — F2
// keeps existing scope semantics).
static ConstValue eval_block(ConstCtx *ctx, FileId fid, SyntaxNode *node) {
  SyntaxNode *stmts = NULL;
  if (syntax_node_kind(node) == SK_BLOCK_STMT) {
    BlockStmt bs;
    if (BlockStmt_cast(node, &bs))
      stmts = BlockStmt_stmts(&bs);
  }
  if (!stmts)
    return none_value();
  ConstValue result = none_value();
  // Find tail = last non-binding node.
  uint32_t n = syntax_node_num_children(stmts);
  SyntaxNode *tail = NULL;
  // Collect nodes first; pick the last non-CONST/VAR decl.
  for (uint32_t i = 0; i < n; i++) {
    SyntaxElement el = syntax_node_child_or_token(stmts, i);
    if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      SyntaxKind k = syntax_node_kind(el.node);
      if (k != SK_CONST_DECL && k != SK_VAR_DECL) {
        if (tail) syntax_node_release(tail);
        tail = el.node;
      } else {
        syntax_node_release(el.node);
      }
    } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      syntax_token_release(el.token);
    }
  }
  if (tail) {
    result = eval_inner(ctx, fid, tail);
    syntax_node_release(tail);
  }
  syntax_node_release(stmts);
  return result;
}

// ============================================================================
// Phase 5 — comptime purity gate.
//
// Detect comptime calls to effectful functions. Resolves the callee via
// name lookup (top-level def references only — body-scope locals and
// nested field accesses aren't comptime-evaluable today), fetches the
// fn signature, and emits a clear diag when its effect row is non-empty.
//
// Pure callees return CONST_NONE — actual beta-reduction (bind params,
// evaluate the body) is a separate, larger feature. This arm exists so
// `[2 * fn_that_does_io()]T`-style misuse surfaces a real error message
// instead of a silent comptime-failure.
// ============================================================================

extern NamespaceScopes db_query_namespace_scopes(db_query_ctx *ctx,
                                                 NamespaceId nsid);
extern DefId db_query_resolve_ref(db_query_ctx *ctx, ScopeId scope,
                                  StrId name);

static ConstValue eval_call(ConstCtx *ctx, FileId fid, SyntaxNode *node) {
  struct db *s = ctx->s;
  CallExpr ce;
  if (!CallExpr_cast(node, &ce))
    return none_value();
  SyntaxNode *callee = CallExpr_callee(&ce);
  // Only SK_REF_EXPR callees are eligible for the purity check — for
  // anything else (lambda call, field expression, etc.) we silently
  // return CONST_NONE so the caller's comptime path bails as today.
  if (!callee || syntax_node_kind(callee) != SK_REF_EXPR) {
    if (callee) syntax_node_release(callee);
    return none_value();
  }
  SyntaxToken *nt = ast_first_token(callee, SK_IDENT);
  if (!nt) {
    syntax_node_release(callee);
    return none_value();
  }
  StrId name = pool_intern(&s->strings, syntax_token_text(nt),
                           syntax_token_text_range(nt).length);
  syntax_token_release(nt);
  if (name.idx == 0) {
    syntax_node_release(callee);
    return none_value();
  }

  NamespaceId nsid = db_get_file_namespace(s, fid);
  NamespaceScopes sc = db_query_namespace_scopes(s, nsid);
  if (sc.internal.idx == SCOPE_ID_NONE.idx) {
    syntax_node_release(callee);
    return none_value();
  }
  DefId target = db_query_resolve_ref(s, sc.internal, name);
  if (target.idx == DEF_ID_NONE.idx) {
    syntax_node_release(callee);
    return none_value();
  }

  // Fetch the callee's type. fn types carry effect_row in their key.
  IpIndex fn_ty = db_query_type_of_def(s, target);
  if (fn_ty.v == IP_NONE.v ||
      ip_tag(&s->intern, fn_ty) != IP_TAG_FN_TYPE) {
    syntax_node_release(callee);
    return none_value();
  }
  IpKey k = ip_key(&s->intern, fn_ty);
  if (k.fn_type.effect_row.v != IP_EMPTY_EFFECT_ROW.v &&
      k.fn_type.effect_row.v != IP_NONE.v) {
    db_emit(s, DIAG_ERROR, const_eval_anchor(ctx, fid, node),
            "comptime call to effectful '%S' (effects %T)",
            name, k.fn_type.effect_row);
  }
  syntax_node_release(callee);
  // Pure or effectful: no body-evaluation today — return CONST_NONE.
  // The diag above is the load-bearing output for effectful callees;
  // pure callees stay foldable-on-paper but not yet folded.
  return none_value();
}

// ============================================================================
// #3 — SK_FIELD_EXPR (namespace member lookup) + SK_COMPTIME_EXPR
// ============================================================================

// Fold `<base>.field` when `<base>` is comptime-foldable to a known
// namespace value. Today the only namespaces with comptime-known
// members are the synthetic @target and @build pre-baked at db_init.
// For user namespaces (file imports), members are runtime values and
// this returns CONST_NONE.
static ConstValue eval_field_expr(ConstCtx *ctx, FileId fid, SyntaxNode *node) {
  struct db *s = ctx->s;
  ConstCycle *stk = &ctx->stk;
  FieldExpr fe;
  if (!FieldExpr_cast(node, &fe))
    return none_value();
  SyntaxNode *base = FieldExpr_base(&fe);
  SyntaxToken *ft = FieldExpr_field(&fe);
  if (!base || !ft) {
    if (base) syntax_node_release(base);
    if (ft) syntax_token_release(ft);
    return none_value();
  }
  StrId fname = pool_intern(&s->strings, syntax_token_text(ft),
                            syntax_token_text_range(ft).length);
  syntax_token_release(ft);
  if (fname.idx == 0) {
    syntax_node_release(base);
    return none_value();
  }

  // Qualified enum-variant access: `Os.macos` parses as SK_FIELD_EXPR
  // with base = SK_REF_EXPR("Os"). "Os" is a TYPE name (KIND_ENUM
  // decl), not a const-bind, so eval_inner on the base returns
  // CONST_NONE. Detect this structurally BEFORE the recursive eval —
  // resolve the type name → enum DefId → variant lookup. Lets a const
  // like `os :: pub Os.macos` fold to CONST_ENUM_VARIANT directly,
  // and lets user code write `comptime switch (Color.Red) { ... }`
  // without needing bidirectional enum-ctx.
  if (syntax_node_kind(base) == SK_REF_EXPR) {
    RefExpr r;
    if (RefExpr_cast(base, &r)) {
      SyntaxToken *nt = RefExpr_name(&r);
      if (nt) {
        StrId tname = pool_intern(&s->strings, syntax_token_text(nt),
                                  syntax_token_text_range(nt).length);
        syntax_token_release(nt);
        if (tname.idx) {
          NamespaceId here = db_get_file_namespace(s, fid);
          TopLevelEntry te = db_query_top_level_entry(s, here, tname);
          if (te.node_ptr.kind != SYNTAX_KIND_NONE) {
            DefId tdef = db_query_def_identity(s, here, te.id);
            if (tdef.idx != 0 && db_def_kind(s, tdef) == KIND_ENUM) {
              uint32_t nv = 0;
              const EnumVariantEntry *vs = db_enum_variants(s, tdef, &nv);
              for (uint32_t k = 0; k < nv; k++) {
                if (vs[k].name.idx == fname.idx) {
                  syntax_node_release(base);
                  return (ConstValue){
                      .kind = CONST_ENUM_VARIANT,
                      .enum_variant = {tdef, k}};
                }
              }
            }
          }
        }
      }
    }
  }

  ConstValue bv = eval_inner(ctx, fid, base);
  syntax_node_release(base);
  if (bv.kind != CONST_NAMESPACE)
    return none_value();

  // B0-rework (generic CONST_NAMESPACE member walk — DoD-clean):
  // Look up the member directly via top_level_entry (members of a
  // namespace ARE its top-level entries — same path eval_ref takes for
  // a same-file bare reference, plus we get e.file for free so cross-
  // file member-of-virtual-file resolves the right green root).
  //
  // For a const-decl member, resolve its RHS expression and fold it
  // with the member's declared type as enum-context. This makes a bare
  // `.variant` in the RHS resolve against the member's declared enum
  // type (the only way to fold `os :: pub Os : .macos` to
  // CONST_ENUM_VARIANT, since const_eval has no expected-type plumbing
  // outside the eval_with_enum_ctx entry point).
  //
  // Works for ANY namespace's const fields — no @target/@build
  // special-case. Producer of CONST_NAMESPACE arrives in B2
  // (eval_builtin's TARGET / BUILD arms); this arm goes live then.
  TopLevelEntry e = db_query_top_level_entry(s, bv.nsid, fname);
  if (e.node_ptr.kind == SYNTAX_KIND_NONE)
    return none_value();

  // TopLevelEntry doesn't carry a DefId directly — derive via the
  // canonical (nsid, AstId) → DefId routing used everywhere else.
  DefId mdef = db_query_def_identity(s, bv.nsid, e.id);
  if (mdef.idx == 0)
    return none_value();

  // Only const decls fold; var decls (`:=`) are runtime-mutable.
  if (db_def_kind(s, mdef) != KIND_CONSTANT)
    return none_value();

  // Pull the member's declared type so a bare `.variant` RHS can
  // resolve via eval_with_enum_ctx.
  IpIndex member_ty = db_query_type_of_def(s, mdef);
  DefId enum_ctx = {0};
  if (ip_index_is_valid(member_ty) &&
      ip_tag(&s->intern, member_ty) == IP_TAG_ENUM_TYPE) {
    IpKey k = ip_key(&s->intern, member_ty);
    enum_ctx.idx = k.enum_type.zir_node_id;
  }

  // Cycle gate + depth guard — mirror eval_ref. The binding identity
  // is (e.file, hash(node_ptr)); a re-entry on the same binding means
  // a self-ref or mutual chain through namespace fields.
  uint64_t entry_hash = syntax_node_ptr_hash(e.node_ptr);
  if (cycle_contains(stk, e.file, entry_hash)) {
    db_emit(s, DIAG_ERROR, const_eval_anchor(ctx, fid, node),
            "circular const dependency through '%S'", fname);
    return none_value();
  }
  if (stk->count >= ORE_CONST_CYCLE_MAX) {
    db_emit(s, DIAG_ERROR, const_eval_anchor(ctx, fid, node),
            "const chain too deep (max %d)", ORE_CONST_CYCLE_MAX);
    return none_value();
  }

  // Resolve wrapper → ConstDef value, then recurse on the RHS with
  // the enum_ctx we computed above. db_read_file_ast records the
  // FILE_AST(e.file) dep on the active query frame.
  struct GreenNode *groot = db_read_file_ast(s, e.file);
  if (!groot)
    return none_value();
  SyntaxTree *tree = syntax_tree_new(groot);
  SyntaxNode *rroot = syntax_tree_root(tree);
  SyntaxNode *wrapper = syntax_node_ptr_resolve(e.node_ptr, rroot);
  syntax_node_release(rroot);
  ConstValue result = none_value();
  if (wrapper && syntax_node_kind(wrapper) == SK_CONST_DECL) {
    ConstDef cd;
    if (ConstDef_cast(wrapper, &cd)) {
      SyntaxNode *val = ConstDef_value(&cd);
      if (val) {
        // Fix B.2 — swap to the target member's own anchor for the
        // duration of the cross-file recursion (see eval_ref for the
        // rationale). Reuses `mdef` (the target member's DefId) already
        // resolved above for the type-context lookup.
        stk->entries[stk->count++] = (ConstCycleEntry){e.file, entry_hash};
        ConstDiagAnchorCtx saved_anchor = ctx->anchor;
        ctx->anchor = const_eval_anchor_for_target(s, mdef, e.id.idx);
        result = eval_with_enum_ctx(ctx, e.file, val, enum_ctx);
        ctx->anchor = saved_anchor;
        stk->count--;
        syntax_node_release(val);
      }
    }
  }
  if (wrapper)
    syntax_node_release(wrapper);
  syntax_tree_free(tree);
  return result;
}

// `comptime <inner>` — sema layer's sema_comptime_select handles
// branch selection; const_eval just folds the wrapped expression.
// Transparent for non-comptime-aware kinds (consts, arithmetic, etc.).
static ConstValue eval_comptime_expr(ConstCtx *ctx, FileId fid,
                                     SyntaxNode *node) {
  ComptimeExpr ce;
  if (!ComptimeExpr_cast(node, &ce))
    return none_value();
  SyntaxNode *inner = ComptimeExpr_inner(&ce);
  ConstValue v = inner ? eval_inner(ctx, fid, inner) : none_value();
  if (inner)
    syntax_node_release(inner);
  return v;
}

// ============================================================================
// Top-level dispatch
// ============================================================================

static ConstValue eval_inner(ConstCtx *ctx, FileId fid, SyntaxNode *node) {
  if (!ctx || !ctx->s || !node)
    return none_value();
  switch (syntax_node_kind(node)) {
  case SK_LITERAL_EXPR: return eval_literal(node);
  case SK_BIN_EXPR:     return eval_bin(ctx, fid, node);
  case SK_PREFIX_EXPR:  return eval_prefix(ctx, fid, node);
  case SK_REF_EXPR:     return eval_ref(ctx, fid, node);
  case SK_BUILTIN_EXPR: return eval_builtin(ctx, fid, node);
  case SK_CALL_EXPR:    return eval_call(ctx, fid, node);
  case SK_IF_EXPR:      return eval_if(ctx, fid, node);
  case SK_SWITCH_EXPR:  return eval_switch(ctx, fid, node);
  case SK_BLOCK_STMT:   return eval_block(ctx, fid, node);
  case SK_FIELD_EXPR:   return eval_field_expr(ctx, fid, node);
  case SK_COMPTIME_EXPR:return eval_comptime_expr(ctx, fid, node);
  default:              return none_value();
  }
}

ConstValue db_const_eval(struct db *s, FileId fid, SyntaxNode *node,
                         ConstDiagAnchorCtx anchor) {
  ConstCtx ctx = { .s = s, .anchor = anchor, .stk = {0} };
  return eval_inner(&ctx, fid, node);
}

ConstValue db_const_eval_with_enum_ctx(struct db *s, FileId fid,
                                       SyntaxNode *node, DefId enum_ctx,
                                       ConstDiagAnchorCtx anchor) {
  ConstCtx ctx = { .s = s, .anchor = anchor, .stk = {0} };
  return eval_with_enum_ctx(&ctx, fid, node, enum_ctx);
}

// ============================================================================
// fits_in — Port of sema_legacy/typechecker/fits.c
// ============================================================================

static bool int_fits_signed(int64_t v, int bits, const char **lo,
                            const char **hi) {
  switch (bits) {
  case 8:  *lo = "-128"; *hi = "127";
           return v >= INT8_MIN && v <= INT8_MAX;
  case 16: *lo = "-32768"; *hi = "32767";
           return v >= INT16_MIN && v <= INT16_MAX;
  case 32: *lo = "-2147483648"; *hi = "2147483647";
           return v >= INT32_MIN && v <= INT32_MAX;
  case 64: *lo = "-9223372036854775808"; *hi = "9223372036854775807";
           return true;
  }
  return false;
}

static bool int_fits_unsigned(int64_t v, int bits, const char **lo,
                              const char **hi) {
  *lo = "0";
  if (v < 0) {
    switch (bits) {
    case 8:  *hi = "255"; break;
    case 16: *hi = "65535"; break;
    case 32: *hi = "4294967295"; break;
    case 64: *hi = "18446744073709551615"; break;
    }
    return false;
  }
  uint64_t u = (uint64_t)v;
  switch (bits) {
  case 8:  *hi = "255";                  return u <= UINT8_MAX;
  case 16: *hi = "65535";                return u <= UINT16_MAX;
  case 32: *hi = "4294967295";           return u <= UINT32_MAX;
  case 64: *hi = "18446744073709551615"; return true;
  }
  return false;
}

// J2: caller already has the uint64_t view (from an is_unsigned source
// where the int64_t bit-pattern would mis-read as negative). Skips the
// v<0 reject. Same range strings.
static bool int_fits_unsigned_u64(uint64_t u, int bits, const char **lo,
                                  const char **hi) {
  *lo = "0";
  switch (bits) {
  case 8:  *hi = "255";                  return u <= UINT8_MAX;
  case 16: *hi = "65535";                return u <= UINT16_MAX;
  case 32: *hi = "4294967295";           return u <= UINT32_MAX;
  case 64: *hi = "18446744073709551615"; return true;
  }
  return false;
}

// int_bits / is_signed_int / is_unsigned_int now live in coerce.h
// (shared with infer.c). Kept here as a compile-time link only.

bool db_const_value_fits_in(struct db *s, ConstValue v, IpIndex t,
                            const char **out_lo, const char **out_hi) {
  (void)s;
  const char *lo_tmp = NULL, *hi_tmp = NULL;
  if (!out_lo) out_lo = &lo_tmp;
  if (!out_hi) out_hi = &hi_tmp;
  *out_lo = NULL;
  *out_hi = NULL;

  if (v.kind == CONST_INT) {
    if (t.v == IP_COMPTIME_INT_TYPE.v) return true;
    int bits = int_bits(t);
    if (bits == 0) return false;
    // J2: when the source ConstValue is_unsigned, route through the
    // unsigned path even if the target is signed (the signed-path
    // function would see a negative int64_t and reject a valid value).
    if (v.is_unsigned) {
      if (!is_unsigned_int(t)) {
        // Unsigned source into signed target: only fits if value ≤ that
        // target's signed-max. Re-check via the signed path with the
        // bit-pattern's u64 view bounded above first.
        uint64_t u = (uint64_t)v.int_val;
        if (is_signed_int(t)) {
          // SAFETY: bits ≤ 64; (1ULL << bits-1) - 1 fits in u64.
          uint64_t smax = (bits == 64)
              ? (uint64_t)INT64_MAX
              : ((uint64_t)1 << (bits - 1)) - 1;
          if (u > smax) {
            // Re-route through signed path so the diag carries proper
            // range bounds — int_fits_signed knows the strings.
            return int_fits_signed(v.int_val, bits, out_lo, out_hi);
          }
          return int_fits_signed((int64_t)u, bits, out_lo, out_hi);
        }
        return false;
      }
      return int_fits_unsigned_u64((uint64_t)v.int_val, bits, out_lo, out_hi);
    }
    if (is_signed_int(t))
      return int_fits_signed(v.int_val, bits, out_lo, out_hi);
    if (is_unsigned_int(t))
      return int_fits_unsigned(v.int_val, bits, out_lo, out_hi);
    return false;
  }
  if (v.kind == CONST_FLOAT) {
    if (t.v == IP_COMPTIME_FLOAT_TYPE.v || t.v == IP_F64_TYPE.v) return true;
    if (t.v == IP_F32_TYPE.v) {
      double a = v.float_val < 0 ? -v.float_val : v.float_val;
      // Magnitude check against f32 max.
      return a == 0.0 || a <= 3.4028234663852886e38;
    }
    return false;
  }
  if (v.kind == CONST_BOOL)
    return t.v == IP_BOOL_TYPE.v;
  return false;
}

const char *db_const_value_to_str(ConstValue v, char *buf, size_t buflen) {
  if (!buf || buflen < 2) return "?";
  switch (v.kind) {
  case CONST_INT:
    // J2: unsigned source ≥ 2^63 reads as a negative int64_t — render
    // via uint64_t format to display the correct value.
    if (v.is_unsigned)
      snprintf(buf, buflen, "%llu", (unsigned long long)(uint64_t)v.int_val);
    else
      snprintf(buf, buflen, "%lld", (long long)v.int_val);
    break;
  case CONST_FLOAT: snprintf(buf, buflen, "%g",   v.float_val);            break;
  case CONST_BOOL:  snprintf(buf, buflen, "%s",   v.bool_val ? "true" : "false"); break;
  default:          snprintf(buf, buflen, "?");                            break;
  }
  return buf;
}

// ============================================================================
// Layout
// ============================================================================
//
// Walks the IpIndex graph: primitives + ptr/slice/many_ptr (ptr-size)
// + array (N × elem) + optional (elem + 1-byte tag) + struct/enum
// (per-kind layout walk via db_aggregate_field_count / _at).
//
// Cycle detection: a small fixed-size stack of DefId values for the
// currently-walking nominal types. A struct field of its own type
// (transitively or directly) trips the cycle → is_known = false; the
// caller emits the diag (we don't because layout is also called for
// non-error introspection like hover).

#define LAYOUT_CYCLE_STACK_MAX 32

typedef struct {
  uint32_t depth;
  uint32_t ids[LAYOUT_CYCLE_STACK_MAX];
} LayoutCycle;

static OreLayout primitive_layout(IpIndex t) {
  OreLayout L = {0};
  L.is_known = true;
  // From ip_primitives.def — size and align columns.
  if      (t.v == IP_BOOL_TYPE.v) { L.size = 1;  L.align = 1; }
  else if (t.v == IP_U8_TYPE.v  || t.v == IP_I8_TYPE.v)  { L.size = 1; L.align = 1; }
  else if (t.v == IP_U16_TYPE.v || t.v == IP_I16_TYPE.v) { L.size = 2; L.align = 2; }
  else if (t.v == IP_U32_TYPE.v || t.v == IP_I32_TYPE.v) { L.size = 4; L.align = 4; }
  else if (t.v == IP_U64_TYPE.v || t.v == IP_I64_TYPE.v) { L.size = 8; L.align = 8; }
  else if (t.v == IP_F32_TYPE.v) { L.size = 4; L.align = 4; }
  else if (t.v == IP_F64_TYPE.v) { L.size = 8; L.align = 8; }
  else if (t.v == IP_USIZE_TYPE.v || t.v == IP_ISIZE_TYPE.v) {
    L.size = sizeof(void *); L.align = _Alignof(void *);
  } else {
    L.is_known = false;
  }
  return L;
}

static OreLayout layout_recurse(struct db *s, IpIndex t, LayoutCycle *cyc);

static OreLayout layout_struct_or_union(struct db *s, DefId def,
                                        LayoutCycle *cyc, bool is_union) {
  OreLayout L = {0};
  // Cycle check.
  for (uint32_t i = 0; i < cyc->depth; i++) {
    if (cyc->ids[i] == def.idx)
      return L; // is_known = false (cycle)
  }
  if (cyc->depth >= LAYOUT_CYCLE_STACK_MAX)
    return L; // depth exhausted — conservative miss
  cyc->ids[cyc->depth++] = def.idx;

  L.is_known = true;
  L.size = 0;
  L.align = 1;
  uint32_t n = db_aggregate_field_count(s, def);
  for (uint32_t i = 0; i < n; i++) {
    AggregateFieldEntry f = db_aggregate_field_at(s, def, i);
    OreLayout fl = layout_recurse(s, f.type, cyc);
    if (!fl.is_known) {
      L.is_known = false;
      cyc->depth--;
      return L;
    }
    if (fl.align > L.align) L.align = fl.align;
    if (is_union) {
      if (fl.size > L.size) L.size = fl.size;
    } else {
      // Pad to field alignment.
      if (fl.align > 0)
        L.size = (L.size + fl.align - 1) & ~(uint64_t)(fl.align - 1);
      L.size += fl.size;
    }
  }
  // Tail pad to overall align.
  if (L.align > 0)
    L.size = (L.size + L.align - 1) & ~(uint64_t)(L.align - 1);
  cyc->depth--;
  return L;
}

static OreLayout layout_recurse(struct db *s, IpIndex t, LayoutCycle *cyc) {
  OreLayout L = {0};
  if (!ip_index_is_valid(t))
    return L;

  IpTag tag = ip_tag(&s->intern, t);
  switch (tag) {
  case IP_TAG_PRIMITIVE_TYPE:
    return primitive_layout(t);

  case IP_TAG_PTR_TYPE:
  case IP_TAG_PTR_CONST_TYPE:
  case IP_TAG_MANY_PTR_TYPE:
  case IP_TAG_MANY_PTR_CONST_TYPE:
    L.is_known = true;
    L.size = sizeof(void *);
    L.align = _Alignof(void *);
    return L;

  case IP_TAG_SLICE_TYPE:
  case IP_TAG_SLICE_CONST_TYPE:
    // Slice = (ptr, len). 2 words.
    L.is_known = true;
    L.size = 2 * sizeof(void *);
    L.align = _Alignof(void *);
    return L;

  case IP_TAG_ARRAY_TYPE: {
    IpKey k = ip_key(&s->intern, t);
    OreLayout elem = layout_recurse(s, k.array_type.elem, cyc);
    if (!elem.is_known)
      return L; // cycle in element
    L.is_known = true;
    L.size = elem.size * k.array_type.size;
    L.align = elem.align;
    return L;
  }

  case IP_TAG_OPTIONAL_TYPE: {
    IpKey k = ip_key(&s->intern, t);
    OreLayout inner = layout_recurse(s, k.optional_type.elem, cyc);
    if (!inner.is_known)
      return L;
    L.is_known = true;
    // Conservative: tag byte + inner, aligned to inner.align.
    uint64_t size = inner.size + 1;
    if (inner.align > 0)
      size = (size + inner.align - 1) & ~(uint64_t)(inner.align - 1);
    L.size = size;
    L.align = inner.align > 0 ? inner.align : 1;
    return L;
  }

  case IP_TAG_FN_TYPE:
    // Function values are fn-pointer sized.
    L.is_known = true;
    L.size = sizeof(void *);
    L.align = _Alignof(void *);
    return L;

  case IP_TAG_STRUCT_TYPE: {
    IpKey k = ip_key(&s->intern, t);
    DefId def = (DefId){.idx = k.struct_type.zir_node_id};
    bool is_union = (db_def_kind(s, def) == KIND_UNION);
    return layout_struct_or_union(s, def, cyc, is_union);
  }

  case IP_TAG_ENUM_TYPE: {
    // Simple v0: enum tag is a u32 (4 bytes). Tightening to the
    // minimum-width-fitting-all-variants is sema_legacy behavior
    // worth porting later; for the F1 fixture this is sufficient.
    L.is_known = true;
    L.size = 4;
    L.align = 4;
    return L;
  }

  default:
    return L; // is_known = false
  }
}

OreLayout db_layout_of_type(struct db *s, IpIndex t) {
  LayoutCycle cyc = {0};
  return layout_recurse(s, t, &cyc);
}
