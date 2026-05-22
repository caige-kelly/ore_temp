#include "../db/db.h"
#include "../db/diag/diag.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/fn_signature.h"
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

// Body-scope chain (params + let-binds) first via sema_body_scope_lookup;
// fall through to the module's internal scope on miss. resolve_ref +
// type_of_def calls register their salsa deps on the outer query's frame.
// `use_node` is the path-use AST node — needed to find its enclosing
// body scope. sema_body_scope_lookup reads the body-scope pools raw (no
// dep), since the outer query has already declared a dep on
// body_scopes(enclosing_fn) (infer_body does this at entry; the
// body_scopes builder does it implicitly via the same slot).
static IpIndex resolve_value_path(struct db *s, ModuleId mid,
                                  DefId enclosing_fn, AstNodeId use_node,
                                  StrId name) {
  if (name.idx == 0)
    return IP_NONE;
  IpIndex local = sema_body_scope_lookup(s, enclosing_fn, use_node, name);
  if (local.v != IP_NONE.v)
    return local;
  if (mid.idx >= s->modules.internal_scopes.count)
    return IP_NONE;
  ScopeId internal = *(ScopeId *)vec_get(&s->modules.internal_scopes, mid.idx);
  if (internal.idx == SCOPE_ID_NONE.idx)
    return IP_NONE;
  DefId target = db_query_resolve_ref(s, internal, name);
  if (target.idx == DEF_ID_NONE.idx)
    return IP_NONE;
  return db_query_type_of_def(s, target);
}

// === Main entry =============================================================

IpIndex sema_type_of_expr(struct db *s, ASTStore *ast, AstNodeId node,
                          ModuleId mid, DefId enclosing_fn, FileId file_local) {
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
    return resolve_value_path(s, mid, enclosing_fn, node, d.string_id);

  // === Arith binops — unify operand types ===
  case AST_EXPR_BIN_ADD:
  case AST_EXPR_BIN_SUB:
  case AST_EXPR_BIN_MUL:
  case AST_EXPR_BIN_DIV:
  case AST_EXPR_BIN_MOD:
  case AST_EXPR_BIN_POW: {
    IpIndex lt =
        sema_type_of_expr(s, ast, d.bin.lhs, mid, enclosing_fn, file_local);
    IpIndex rt =
        sema_type_of_expr(s, ast, d.bin.rhs, mid, enclosing_fn, file_local);
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
    IpIndex lt =
        sema_type_of_expr(s, ast, d.bin.lhs, mid, enclosing_fn, file_local);
    IpIndex rt =
        sema_type_of_expr(s, ast, d.bin.rhs, mid, enclosing_fn, file_local);
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
    IpIndex lt =
        sema_type_of_expr(s, ast, d.bin.lhs, mid, enclosing_fn, file_local);
    IpIndex rt =
        sema_type_of_expr(s, ast, d.bin.rhs, mid, enclosing_fn, file_local);
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
    IpIndex lt =
        sema_type_of_expr(s, ast, d.bin.lhs, mid, enclosing_fn, file_local);
    IpIndex rt =
        sema_type_of_expr(s, ast, d.bin.rhs, mid, enclosing_fn, file_local);
    return unify_arith(lt, rt);
  }

  // === Call: f(arg1, arg2, ...) ===
  //
  // Ported from sema_legacy/typechecker/expr_check.c::type_of_call.
  // Chunk 5e v1: structural checks only — verify callee resolves to a
  // fn type and arg count matches. Arg-vs-param coercion checking is
  // chunk 5h (lands with check_expr).
  //
  // Ore's polymorphism story is Zig-style `comptime T: type` params,
  // not separate generics syntax. Today a call site like `Vec(i32)`
  // looks like an ordinary call — but its result type depends on the
  // comptime-arg's value, so the fn's IPK_FN_TYPE alone can't give
  // us the return type. That resolution lands with chunk 7 (comptime
  // call evaluation + per-instantiation type interning via the wip
  // API's captures parameter). For chunk 5e: if the callee's return
  // type references a comptime param, we'll see IP_NONE here.
  //
  // AST_EXPR_CALL extras: [callee, arg_count, arg0, arg1, ...].
  case AST_EXPR_CALL: {
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    AstNodeId callee = {.idx = ex[0]};
    uint32_t arg_count = ex[1];

    IpIndex callee_ty =
        sema_type_of_expr(s, ast, callee, mid, enclosing_fn, file_local);
    if (callee_ty.v == IP_NONE.v)
      return IP_NONE;

    if (ip_tag(&s->intern, callee_ty) != IP_TAG_FN_TYPE)
      return IP_NONE; // not callable; proper diag is chunk 5h

    IpKey key = ip_key(&s->intern, callee_ty);
    if (key.fn_type.n_params != arg_count)
      return IP_NONE; // arity mismatch; proper diag is chunk 5h

    // Check each arg against its declared param type via the
    // bidirectional checker. sema_check_expr emits "expected T" diags
    // pointing at the offending arg expression on mismatch. The call's
    // result type is the fn's return regardless of per-arg pass/fail
    // — we don't poison the surrounding type to keep cascading-diag
    // noise down.
    for (uint32_t i = 0; i < arg_count; i++) {
      AstNodeId arg = {.idx = ex[2 + i]};
      (void)sema_check_expr(s, ast, arg, key.fn_type.params[i], mid,
                            enclosing_fn, file_local);
    }

    return key.fn_type.ret;
  }

  // === Field access: obj.field ===
  //
  // Ported from sema_legacy/typechecker/expr_check.c::type_of_field.
  // Chunk 5f v1: handles struct fields, enum variants, slice .len/.ptr,
  // array .len, plus single-pointer auto-deref (Zig-style — `ptr.field`
  // implicitly derefs `^T` to access T's fields).
  //
  // AST: data.bin{lhs=receiver, rhs=name_path}. The field name lives
  // on the rhs AST_EXPR_PATH node's data.string_id.
  //
  // Deferred: visibility checks + diagnostics (chunk 5h), module-member
  // access (`import.foo` — chunk 7), tagged-union active-variant
  // narrowing (chunk 8).
  case AST_EXPR_FIELD: {
    IpIndex recv =
        sema_type_of_expr(s, ast, d.bin.lhs, mid, enclosing_fn, file_local);
    if (recv.v == IP_NONE.v)
      return IP_NONE;

    // Extract field name from the rhs path node.
    if (d.bin.rhs.idx == AST_NODE_ID_NONE.idx)
      return IP_NONE;
    AstNodeKind rk = ((AstNodeKind *)ast->kinds.data)[d.bin.rhs.idx];
    if (rk != AST_EXPR_PATH)
      return IP_NONE;
    StrId fname = ((AstNodeData *)ast->data.data)[d.bin.rhs.idx].string_id;
    if (fname.idx == 0)
      return IP_NONE;

    // Auto-deref single pointers — `ptr.field` reads through the ptr
    // (Zig semantics). Many-pointers DON'T auto-deref (they have no
    // single-element semantics; you index them instead).
    IpTag tag = ip_tag(&s->intern, recv);
    if (tag == IP_TAG_PTR_TYPE || tag == IP_TAG_PTR_CONST_TYPE) {
      IpKey pk = ip_key(&s->intern, recv);
      recv = pk.ptr_type.elem;
      tag = ip_tag(&s->intern, recv);
    }

    switch (tag) {
    case IP_TAG_STRUCT_TYPE: {
      IpKey k = ip_key(&s->intern, recv);
      for (size_t i = 0; i < k.struct_type.n_fields; i++) {
        if (k.struct_type.field_names[i].idx == fname.idx)
          return k.struct_type.field_types[i];
      }
      return IP_NONE; // no such field; proper diag is chunk 5h
    }

    case IP_TAG_ENUM_TYPE: {
      // EnumName.Variant — verify the variant exists, then the
      // access yields a value of the enum type. (Constructing enum
      // values via `.Variant` shorthand is AST_EXPR_ENUM_REF, not
      // field access.)
      IpKey k = ip_key(&s->intern, recv);
      for (size_t i = 0; i < k.enum_type.n_variants; i++) {
        if (k.enum_type.variant_names[i].idx == fname.idx)
          return recv;
      }
      return IP_NONE;
    }

    case IP_TAG_SLICE_TYPE:
    case IP_TAG_SLICE_CONST_TYPE: {
      if (fname.idx == s->names.LEN.idx)
        return IP_USIZE_TYPE;
      if (fname.idx == s->names.PTR.idx) {
        // Zig ABI: `slice.ptr` is `[^]T`, with the slice's const-ness
        // preserved on the resulting many-pointer.
        IpKey sk = ip_key(&s->intern, recv);
        bool is_const = (tag == IP_TAG_SLICE_CONST_TYPE);
        IpKey mp = {.kind = IPK_MANY_PTR_TYPE,
                    .many_ptr_type = {.elem = sk.slice_type.elem,
                                      .is_const = is_const}};
        return ip_get(&s->intern, mp);
      }
      return IP_NONE;
    }

    case IP_TAG_ARRAY_TYPE:
      if (fname.idx == s->names.LEN.idx)
        return IP_USIZE_TYPE;
      return IP_NONE;

    default:
      // Field access on a non-aggregate type. proper diag is chunk 5h.
      return IP_NONE;
    }
  }

  // === Index: obj[i] ===
  //
  // Ported from sema_legacy/typechecker/expr_check.c::type_of_index.
  // Zig-strict indexable kinds: array, slice, many-pointer. Single
  // pointers `^T` are NOT indexable — they point at exactly one
  // element; to read it you deref (`p^`), and to index a multi-element
  // run you use `[^]T`. Index must be int (diag deferred to chunk 5h).
  //
  // AST: data.bin{lhs=receiver, rhs=index_expr}.
  case AST_EXPR_INDEX: {
    IpIndex obj =
        sema_type_of_expr(s, ast, d.bin.lhs, mid, enclosing_fn, file_local);
    if (obj.v == IP_NONE.v)
      return IP_NONE;
    // Type the index for dep recording even if we can't validate it
    // here — chunk 5h's check_expr emits the int-required diag.
    (void)sema_type_of_expr(s, ast, d.bin.rhs, mid, enclosing_fn, file_local);

    IpTag tag = ip_tag(&s->intern, obj);
    IpKey k = ip_key(&s->intern, obj);
    switch (tag) {
    case IP_TAG_ARRAY_TYPE:
      return k.array_type.elem;
    case IP_TAG_SLICE_TYPE:
    case IP_TAG_SLICE_CONST_TYPE:
      return k.slice_type.elem;
    case IP_TAG_MANY_PTR_TYPE:
    case IP_TAG_MANY_PTR_CONST_TYPE:
      return k.many_ptr_type.elem;
    default:
      return IP_NONE; // not indexable
    }
  }

  // === Slice: obj[lo..hi] / obj[lo..] / obj[..hi] ===
  //
  // Ported from sema_legacy/typechecker/expr_check.c::type_of_slice.
  // v1: result is always `[]T` (slice) with const-ness preserved from
  // the receiver. The Zig optimization where comptime-known bounds
  // produce `^[N]T` (single-pointer-to-array of known length) is a
  // chunk-6 follow-up — it needs const_eval of the bounds first.
  //
  // AST_EXPR_SLICE extras: [recv, lo, hi]. Either bound may be 0
  // (AST_NODE_ID_NONE) for the open forms `obj[..hi]` / `obj[lo..]`.
  case AST_EXPR_SLICE: {
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    AstNodeId recv = {.idx = ex[0]};
    AstNodeId lo = {.idx = ex[1]};
    AstNodeId hi = {.idx = ex[2]};

    IpIndex obj =
        sema_type_of_expr(s, ast, recv, mid, enclosing_fn, file_local);
    if (obj.v == IP_NONE.v)
      return IP_NONE;
    // Type bounds for dep recording. Int-coercion check is chunk 5h.
    if (lo.idx != AST_NODE_ID_NONE.idx)
      (void)sema_type_of_expr(s, ast, lo, mid, enclosing_fn, file_local);
    if (hi.idx != AST_NODE_ID_NONE.idx)
      (void)sema_type_of_expr(s, ast, hi, mid, enclosing_fn, file_local);

    // Unpack elem + const-ness from the receiver.
    IpTag tag = ip_tag(&s->intern, obj);
    IpKey k = ip_key(&s->intern, obj);
    IpIndex elem = IP_NONE;
    bool is_const = false;
    switch (tag) {
    case IP_TAG_ARRAY_TYPE:
      elem = k.array_type.elem;
      break;
    case IP_TAG_SLICE_TYPE:
      elem = k.slice_type.elem;
      break;
    case IP_TAG_SLICE_CONST_TYPE:
      elem = k.slice_type.elem;
      is_const = true;
      break;
    case IP_TAG_MANY_PTR_TYPE:
      elem = k.many_ptr_type.elem;
      break;
    case IP_TAG_MANY_PTR_CONST_TYPE:
      elem = k.many_ptr_type.elem;
      is_const = true;
      break;
    default:
      return IP_NONE; // not sliceable
    }

    IpKey out = {.kind = IPK_SLICE_TYPE,
                 .slice_type = {.elem = elem, .is_const = is_const}};
    return ip_get(&s->intern, out);
  }

  // === Unary prefix ops ===
  //
  // Ported from sema_legacy/typechecker/expr_check.c::type_of_unary.
  // Diags deferred — sema_type_of_expr doesn't currently know the
  // file_local for the operand's span (would need plumbing). The
  // return-type-mismatch path in sema_infer_body emits a diag as the
  // chunk-5h proof-of-pipeline; per-op unary diags follow when we
  // thread file_local through.
  case AST_EXPR_UNARY_NEG: {
    // Arithmetic negate — numeric operand, returns same type.
    IpIndex t = sema_type_of_expr(s, ast, d.single_child, mid, enclosing_fn,
                                  file_local);
    if (t.v == IP_NONE.v || !is_numeric(t))
      return IP_NONE;
    return t;
  }
  case AST_EXPR_UNARY_BIT_NOT: {
    // Bitwise not — int operand, returns same type.
    IpIndex t = sema_type_of_expr(s, ast, d.single_child, mid, enclosing_fn,
                                  file_local);
    if (t.v == IP_NONE.v)
      return IP_NONE;
    if (t.v != IP_COMPTIME_INT_TYPE.v && !is_concrete_int(t))
      return IP_NONE;
    return t;
  }
  case AST_EXPR_UNARY_NOT: {
    // Logical not — bool operand, returns bool.
    IpIndex t = sema_type_of_expr(s, ast, d.single_child, mid, enclosing_fn,
                                  file_local);
    if (t.v != IP_BOOL_TYPE.v)
      return IP_NONE;
    return IP_BOOL_TYPE;
  }
  case AST_EXPR_UNARY_REF: {
    // &x — address-of. Result is `^T` (mutable single-pointer).
    //
    // L-value check (ported from sema_legacy/typechecker/expr_check.c::
    // target_is_assignable). Address-of only makes sense on storage
    // locations: identifier (a let-bind or param), field access,
    // index expression, or a pointer deref. Literals and call results
    // are temporaries with no stable address — reject them.
    if (d.single_child.idx != AST_NODE_ID_NONE.idx) {
      AstNodeKind ck = ((AstNodeKind *)ast->kinds.data)[d.single_child.idx];
      bool is_lvalue = (ck == AST_EXPR_PATH || ck == AST_EXPR_FIELD ||
                        ck == AST_EXPR_INDEX || ck == AST_EXPR_UNARY_DEREF);
      if (!is_lvalue) {
        TinySpan span = db_get_node_span(s, file_local, node);
        if (span != TINYSPAN_NONE)
          db_emit_error(s, span,
                        "address-of '&' requires an l-value "
                        "(variable, field, index, or deref)");
        return IP_NONE;
      }
    }
    IpIndex t = sema_type_of_expr(s, ast, d.single_child, mid, enclosing_fn,
                                  file_local);
    if (t.v == IP_NONE.v)
      return IP_NONE;
    IpKey k = {.kind = IPK_PTR_TYPE,
               .ptr_type = {.elem = t, .is_const = false}};
    return ip_get(&s->intern, k);
  }
  case AST_EXPR_UNARY_DEREF: {
    // x^ — postfix deref. Reads through a single pointer (^T or
    // ^const T). Many-pointers don't auto-deref — they're indexed
    // with `p[0]` since they have no inherent length-of-1 semantics.
    IpIndex t = sema_type_of_expr(s, ast, d.single_child, mid, enclosing_fn,
                                  file_local);
    if (t.v == IP_NONE.v)
      return IP_NONE;
    IpTag tag = ip_tag(&s->intern, t);
    if (tag != IP_TAG_PTR_TYPE && tag != IP_TAG_PTR_CONST_TYPE)
      return IP_NONE;
    return ip_key(&s->intern, t).ptr_type.elem;
  }
  case AST_EXPR_UNARY_DENIL: {
    // x? — postfix optional unwrap. Operand must be ?T; result is T.
    IpIndex t = sema_type_of_expr(s, ast, d.single_child, mid, enclosing_fn,
                                  file_local);
    if (t.v == IP_NONE.v)
      return IP_NONE;
    if (ip_tag(&s->intern, t) != IP_TAG_OPTIONAL_TYPE)
      return IP_NONE;
    return ip_key(&s->intern, t).optional_type.elem;
  }

  // === Return statement: type as noreturn, check value vs declared ===
  //
  // Ported from sema_legacy/typechecker/expr_check.c::type_of_return.
  // `return X` doesn't produce a value to the surrounding expression —
  // control transfers away. Type as IP_NORETURN_TYPE which coerces to
  // anything (per can_coerce), so a block ending in `return X` doesn't
  // double-diagnose at the block boundary.
  //
  // The actual checking: we look up the enclosing fn's declared return
  // type via fn_signature and check X against it via sema_check_expr —
  // diag fires at X's span on mismatch.
  case AST_STMT_RETURN: {
    if (enclosing_fn.idx != DEF_ID_NONE.idx &&
        d.single_child.idx != AST_NODE_ID_NONE.idx) {
      IpIndex sig = db_query_fn_signature(s, enclosing_fn);
      if (sig.v != IP_NONE.v && ip_tag(&s->intern, sig) == IP_TAG_FN_TYPE) {
        IpKey k = ip_key(&s->intern, sig);
        (void)sema_check_expr(s, ast, d.single_child, k.fn_type.ret, mid,
                              enclosing_fn, file_local);
      }
    }
    return IP_NORETURN_TYPE;
  }

  // === Block expression: { stmt; stmt; tail } returns tail's type ===
  //
  // Ported from sema_legacy/typechecker/expr_check.c::type_of_block.
  // All statements are typed (records deps + lets sub-expression diags
  // fire). The block's type is the LAST statement's type, or IP_VOID
  // for an empty block. (Statement-position blocks always have a
  // last-stmt type even if it's void; expression-position blocks
  // produce that type as the block's value.)
  case AST_STMT_BLOCK: {
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    uint32_t count = ex[0];
    if (count == 0)
      return IP_VOID_TYPE;
    IpIndex last = IP_VOID_TYPE;
    for (uint32_t i = 0; i < count; i++) {
      AstNodeId stmt = {.idx = ex[1 + i]};
      last = sema_type_of_expr(s, ast, stmt, mid, enclosing_fn, file_local);
    }
    return last;
  }

  default:
    // ORELSE / CATCH, assignments, product, builtin (@sizeOf etc.),
    // pattern matching, INC/DEERR (need lvalue + error story),
    // if/loop/switch as expressions, etc. — later sub-chunks.
    return IP_NONE;
  }
}
