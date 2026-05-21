#include "../db/db.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/resolve_ref.h"
#include "../db/query/type_of_def.h"
#include "../parser/ast.h"
#include "sema.h"

#include <stdbool.h>

// === Numeric predicates =====================================================
//
// Bitmasks over IpReservedIndex values — all reserved primitives have
// IpIndex.v < 32 today (extend to u64 if we ever blow that), so each
// predicate is a single shift+and. The masks are built from the enum
// constants directly, so reordering ip_primitives.def keeps them
// correct without manual edits.

#define IP_BIT(name) (1u << IP_INDEX_##name##_TYPE)

static const uint32_t CONCRETE_INT_MASK =
    IP_BIT(U8)  | IP_BIT(I8)  | IP_BIT(U16) | IP_BIT(I16) |
    IP_BIT(U32) | IP_BIT(I32) | IP_BIT(U64) | IP_BIT(I64) |
    IP_BIT(USIZE) | IP_BIT(ISIZE);

static const uint32_t CONCRETE_FLOAT_MASK =
    IP_BIT(F32) | IP_BIT(F64);

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

// Arith unification, Zig-style. comptime_int and comptime_float coerce
// up to matching concretes (or to each other in mixed numeric ops);
// concrete + different concrete returns IP_NONE.
//
// This is a placeholder for the much more thorough coerce.c we'll port
// from sema_legacy/typechecker/coerce.c — that one handles variance,
// pointer-to-pointer, slice-to-slice, optional unwrapping, etc.
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

// === Literal mapping ========================================================

static IpIndex type_from_literal_kind(AstNodeKind k) {
  switch (k) {
  case AST_EXPR_LIT_INT:
    return IP_COMPTIME_INT_TYPE;
  case AST_EXPR_LIT_FLOAT:
    return IP_COMPTIME_FLOAT_TYPE;
  case AST_EXPR_LIT_BOOL:
    return IP_BOOL_TYPE;
  case AST_EXPR_LIT_BYTE:
    return IP_U8_TYPE;
  case AST_EXPR_LIT_STRING:
    return IP_STRING_SLICE_TYPE;
  case AST_EXPR_LIT_NIL:
    return IP_NIL_TYPE;
  default:
    return IP_NONE;
  }
}

// === Value-position identifier resolution ===================================

// Local-scope (params + future let-binds) first via sema_local_scope_lookup;
// fall through to the module's internal scope on miss. resolve_ref +
// type_of_def calls both register their salsa deps on the outer query's
// frame.
static IpIndex resolve_value_path(struct db *s, ModuleId mid,
                                  DefId enclosing_fn, StrId name) {
  if (name.idx == 0)
    return IP_NONE;
  IpIndex local = sema_local_scope_lookup(s, enclosing_fn, name);
  if (local.v != IP_NONE.v)
    return local;
  if (mid.idx >= s->modules.internal_scopes.count)
    return IP_NONE;
  ScopeId internal =
      *(ScopeId *)vec_get(&s->modules.internal_scopes, mid.idx);
  if (internal.idx == SCOPE_ID_NONE.idx)
    return IP_NONE;
  DefId target = db_query_resolve_ref(s, internal, name);
  if (target.idx == DEF_ID_NONE.idx)
    return IP_NONE;
  return db_query_type_of_def(s, target);
}

// === Main entry =============================================================

IpIndex sema_type_of_expr(struct db *s, ASTStore *ast, AstNodeId node,
                          ModuleId mid, DefId enclosing_fn) {
  if (node.idx == AST_NODE_ID_NONE.idx)
    return IP_NONE;
  AstNodeKind k = ((AstNodeKind *)ast->kinds.data)[node.idx];
  AstNodeData d = ((AstNodeData *)ast->data.data)[node.idx];

  switch (k) {
  case AST_EXPR_LIT_INT:
  case AST_EXPR_LIT_FLOAT:
  case AST_EXPR_LIT_BOOL:
  case AST_EXPR_LIT_BYTE:
  case AST_EXPR_LIT_STRING:
  case AST_EXPR_LIT_NIL:
    return type_from_literal_kind(k);

  case AST_EXPR_PATH:
    return resolve_value_path(s, mid, enclosing_fn, d.string_id);

  // === Arith binops — unify operand types ===
  case AST_EXPR_BIN_ADD:
  case AST_EXPR_BIN_SUB:
  case AST_EXPR_BIN_MUL:
  case AST_EXPR_BIN_DIV:
  case AST_EXPR_BIN_MOD:
  case AST_EXPR_BIN_POW: {
    IpIndex lt = sema_type_of_expr(s, ast, d.bin.lhs, mid, enclosing_fn);
    IpIndex rt = sema_type_of_expr(s, ast, d.bin.rhs, mid, enclosing_fn);
    IpIndex u = unify_arith(lt, rt);
    if (u.v == IP_NONE.v)
      return IP_NONE;
    if (!is_numeric(u))
      return IP_NONE;
    return u;
  }

  // === Comparison — operands unify, result is bool ===
  case AST_EXPR_BIN_EQ:
  case AST_EXPR_BIN_NEQ:
  case AST_EXPR_BIN_LT:
  case AST_EXPR_BIN_LE:
  case AST_EXPR_BIN_GT:
  case AST_EXPR_BIN_GE: {
    IpIndex lt = sema_type_of_expr(s, ast, d.bin.lhs, mid, enclosing_fn);
    IpIndex rt = sema_type_of_expr(s, ast, d.bin.rhs, mid, enclosing_fn);
    if (lt.v == IP_NONE.v || rt.v == IP_NONE.v)
      return IP_NONE;
    if (lt.v != rt.v) {
      IpIndex u = unify_arith(lt, rt);
      if (u.v == IP_NONE.v)
        return IP_NONE;
    }
    return IP_BOOL_TYPE;
  }

  // === Logical — bool inputs, bool result ===
  case AST_EXPR_BIN_AND:
  case AST_EXPR_BIN_OR: {
    IpIndex lt = sema_type_of_expr(s, ast, d.bin.lhs, mid, enclosing_fn);
    IpIndex rt = sema_type_of_expr(s, ast, d.bin.rhs, mid, enclosing_fn);
    if (lt.v != IP_BOOL_TYPE.v || rt.v != IP_BOOL_TYPE.v)
      return IP_NONE;
    return IP_BOOL_TYPE;
  }

  // === Bit ops — unify (int restriction is a chunk-5h diag concern) ===
  case AST_EXPR_BIN_BIT_AND:
  case AST_EXPR_BIN_BIT_OR:
  case AST_EXPR_BIN_BIT_XOR:
  case AST_EXPR_BIN_SHL:
  case AST_EXPR_BIN_SHR: {
    IpIndex lt = sema_type_of_expr(s, ast, d.bin.lhs, mid, enclosing_fn);
    IpIndex rt = sema_type_of_expr(s, ast, d.bin.rhs, mid, enclosing_fn);
    return unify_arith(lt, rt);
  }

  default:
    // ORELSE / CATCH, assignments, calls, field, index, unary,
    // product, etc. — later sub-chunks.
    return IP_NONE;
  }
}
