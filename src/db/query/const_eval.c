// Comptime const-folding + layout — F1 port of sema_legacy.
//
// Pure (non-memoized) for now: each call recurses through SK_REF_EXPR
// chains and re-evaluates referenced consts. Chains in fixtures are
// short (depth ≤ ~10), so the cost is negligible. If profiling shows
// hotness, wire a DB_QUERY_GUARD slot keyed on SyntaxNodePtr.

#include "const_eval.h"

#include "../diag/diag.h"
#include "../../ast/ast_decl.h"
#include "../../ast/ast_expr.h"
#include "../../ast/ast_stmt.h"
#include "../../support/data_structure/stringpool.h"
#include "../../syntax/syntax.h"
#include "../../syntax/syntax_kind.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Externs (no per-query headers; type.c / scope.c style).
extern uint64_t      parse_int_literal(SyntaxToken *tok);
extern TopLevelEntry db_query_top_level_entry(db_query_ctx *ctx,
                                              NamespaceId nsid, StrId name);

// ============================================================================
// Literal extraction
// ============================================================================

static ConstValue none_value(void) {
  return (ConstValue){.kind = CONST_NONE};
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
    uint64_t u = parse_int_literal(tok);
    result.kind = CONST_INT;
    result.int_val = (int64_t)u;
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

static ConstValue bin_add(ConstValue l, ConstValue r) {
  if (l.kind == CONST_INT && r.kind == CONST_INT) {
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
  return (ConstValue){.kind = CONST_INT, .int_val = l.int_val << r.int_val};
}

static ConstValue bin_shr(ConstValue l, ConstValue r) {
  if (l.kind != CONST_INT || r.kind != CONST_INT)
    return none_value();
  if (r.int_val < 0 || r.int_val >= 64)
    return none_value();
  return (ConstValue){.kind = CONST_INT,
                      .int_val = (int64_t)((uint64_t)l.int_val >> r.int_val)};
}

// ============================================================================
// Bin/Prefix dispatch
// ============================================================================

static ConstValue eval_bin(struct db *s, NamespaceId nsid, SyntaxNode *node) {
  BinExpr be;
  if (!BinExpr_cast(node, &be))
    return none_value();
  SyntaxNode *lhs = BinExpr_lhs(&be);
  SyntaxNode *rhs = BinExpr_rhs(&be);
  SyntaxKind opk = BinExpr_op_kind(&be);
  ConstValue l = lhs ? db_const_eval(s, nsid, lhs) : none_value();
  ConstValue r = rhs ? db_const_eval(s, nsid, rhs) : none_value();
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
  default:         return none_value();
  }
}

static ConstValue eval_prefix(struct db *s, NamespaceId nsid,
                              SyntaxNode *node) {
  PrefixExpr pe;
  if (!PrefixExpr_cast(node, &pe))
    return none_value();
  SyntaxNode *operand = PrefixExpr_operand(&pe);
  SyntaxKind opk = PrefixExpr_op_kind(&pe);
  ConstValue v = operand ? db_const_eval(s, nsid, operand) : none_value();
  if (operand)
    syntax_node_release(operand);
  if (v.kind == CONST_NONE)
    return v;
  switch (opk) {
  case SK_MINUS:
    if (v.kind == CONST_INT) {
      if (v.int_val == INT64_MIN)
        return none_value();
      return (ConstValue){.kind = CONST_INT, .int_val = -v.int_val};
    }
    if (v.kind == CONST_FLOAT)
      return (ConstValue){.kind = CONST_FLOAT, .float_val = -v.float_val};
    return none_value();
  case SK_TILDE:
    if (v.kind == CONST_INT)
      return (ConstValue){.kind = CONST_INT, .int_val = ~v.int_val};
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

static ConstValue eval_ref(struct db *s, NamespaceId nsid, SyntaxNode *node) {
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

  TopLevelEntry e = db_query_top_level_entry(s, nsid, name);
  if (e.node_ptr.kind == SYNTAX_KIND_NONE)
    return none_value();

  // Resolve the bind wrapper from the green root + node_ptr, then
  // extract its value expression. SK_CONST_DECL = `::` bind; we
  // intentionally do NOT fold SK_VAR_DECL (`:=` is runtime-mutable).
  struct GreenNode *groot = NULL;
  uint32_t local = file_id_local(e.file);
  if (local < s->files.green_roots.count)
    groot = *(struct GreenNode **)vec_get(&s->files.green_roots, local);
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
        result = db_const_eval(s, nsid, val);
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

extern IpIndex resolve_type_expr_from_const_eval(struct db *s, NamespaceId nsid,
                                                 SyntaxNode *node);
// ↑ Helper provided in infer.c so we can call resolve_type_expr without
// rebuilding a full SemaCtx here. See infer.c for the impl.

static ConstValue eval_builtin(struct db *s, NamespaceId nsid,
                               SyntaxNode *node) {
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
  if (!is_sizeof && !is_alignof)
    return none_value();

  // Pull first NODE arg out of SK_ARG_LIST.
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

  IpIndex t = resolve_type_expr_from_const_eval(s, nsid, type_arg);
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
static ConstValue eval_if(struct db *s, NamespaceId nsid, SyntaxNode *node) {
  IfExpr ie;
  if (!IfExpr_cast(node, &ie))
    return none_value();
  SyntaxNode *cond = IfExpr_condition(&ie);
  SyntaxNode *then_b = IfExpr_then_branch(&ie);
  SyntaxNode *else_b = IfExpr_else_branch(&ie);
  ConstValue cv = cond ? db_const_eval(s, nsid, cond) : none_value();
  ConstValue result = none_value();
  if (cv.kind == CONST_BOOL) {
    SyntaxNode *taken = cv.bool_val ? then_b : else_b;
    if (taken)
      result = db_const_eval(s, nsid, taken);
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
  default:          return false;
  }
}

static ConstValue eval_switch(struct db *s, NamespaceId nsid,
                              SyntaxNode *node) {
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
  ConstValue sv = db_const_eval(s, nsid, scrut);
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
              ConstValue pv = db_const_eval(s, nsid, prev);
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
      result = db_const_eval(s, nsid, body);
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
static ConstValue eval_block(struct db *s, NamespaceId nsid, SyntaxNode *node) {
  SyntaxNode *stmts = NULL;
  if (syntax_node_kind(node) == SK_BLOCK_STMT) {
    BlockStmt bs;
    if (BlockStmt_cast(node, &bs))
      stmts = BlockStmt_stmts(&bs);
  } else if (syntax_node_kind(node) == SK_BLOCK_EXPR) {
    BlockExpr be;
    if (BlockExpr_cast(node, &be))
      stmts = BlockExpr_stmts(&be);
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
    result = db_const_eval(s, nsid, tail);
    syntax_node_release(tail);
  }
  syntax_node_release(stmts);
  return result;
}

// ============================================================================
// Top-level dispatch
// ============================================================================

ConstValue db_const_eval(struct db *s, NamespaceId nsid, SyntaxNode *node) {
  if (!s || !node)
    return none_value();
  switch (syntax_node_kind(node)) {
  case SK_LITERAL_EXPR: return eval_literal(node);
  case SK_BIN_EXPR:     return eval_bin(s, nsid, node);
  case SK_PREFIX_EXPR:  return eval_prefix(s, nsid, node);
  case SK_REF_EXPR:     return eval_ref(s, nsid, node);
  case SK_BUILTIN_EXPR: return eval_builtin(s, nsid, node);
  case SK_IF_EXPR:      return eval_if(s, nsid, node);
  case SK_SWITCH_EXPR:  return eval_switch(s, nsid, node);
  case SK_BLOCK_STMT:
  case SK_BLOCK_EXPR:   return eval_block(s, nsid, node);
  default:              return none_value();
  }
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

// Bits per concrete int primitive. usize/isize are host-pointer-sized
// per ip_primitives.def's TARGET_PTR_SIZE convention.
static int int_bits(IpIndex t) {
  if (t.v == IP_U8_TYPE.v || t.v == IP_I8_TYPE.v)   return 8;
  if (t.v == IP_U16_TYPE.v || t.v == IP_I16_TYPE.v) return 16;
  if (t.v == IP_U32_TYPE.v || t.v == IP_I32_TYPE.v) return 32;
  if (t.v == IP_U64_TYPE.v || t.v == IP_I64_TYPE.v) return 64;
  if (t.v == IP_USIZE_TYPE.v || t.v == IP_ISIZE_TYPE.v)
    return (int)(sizeof(void *) * 8);
  return 0;
}

static bool int_is_signed(IpIndex t) {
  return t.v == IP_I8_TYPE.v || t.v == IP_I16_TYPE.v ||
         t.v == IP_I32_TYPE.v || t.v == IP_I64_TYPE.v ||
         t.v == IP_ISIZE_TYPE.v;
}

static bool int_is_unsigned(IpIndex t) {
  return t.v == IP_U8_TYPE.v || t.v == IP_U16_TYPE.v ||
         t.v == IP_U32_TYPE.v || t.v == IP_U64_TYPE.v ||
         t.v == IP_USIZE_TYPE.v;
}

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
    if (int_is_signed(t))
      return int_fits_signed(v.int_val, bits, out_lo, out_hi);
    if (int_is_unsigned(t))
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
  case CONST_INT:   snprintf(buf, buflen, "%lld", (long long)v.int_val);  break;
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
