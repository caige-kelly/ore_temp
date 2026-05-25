#include "../db/db.h"
#include "../db/diag/diag.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/fn_signature.h"
#include "../db/query/namespace_type.h"
#include "../db/query/query_engine.h"
#include "../db/query/resolve_ref.h"
#include "../db/query/type_of_def.h"
#include "../db/workspace/workspace.h"
#include "../parser/ast.h"
#include "builtins.h"
#include "sema.h"

#include <stdbool.h>

// Operator-name lookup for diag templates. The template gets interned
// with the operator baked in (one StrId per unique op+message pair).
static const char *binop_name(AstNodeKind k) {
  switch (k) {
  case AST_EXPR_BIN_ADD:
    return "+";
  case AST_EXPR_BIN_SUB:
    return "-";
  case AST_EXPR_BIN_MUL:
    return "*";
  case AST_EXPR_BIN_DIV:
    return "/";
  case AST_EXPR_BIN_MOD:
    return "%";
  case AST_EXPR_BIN_POW:
    return "**";
  case AST_EXPR_BIN_EQ:
    return "==";
  case AST_EXPR_BIN_NEQ:
    return "!=";
  case AST_EXPR_BIN_LT:
    return "<";
  case AST_EXPR_BIN_LE:
    return "<=";
  case AST_EXPR_BIN_GT:
    return ">";
  case AST_EXPR_BIN_GE:
    return ">=";
  case AST_EXPR_BIN_AND:
    return "and";
  case AST_EXPR_BIN_OR:
    return "or";
  case AST_EXPR_BIN_BIT_AND:
    return "&";
  case AST_EXPR_BIN_BIT_OR:
    return "|";
  case AST_EXPR_BIN_BIT_XOR:
    return "^";
  case AST_EXPR_BIN_SHL:
    return "<<";
  case AST_EXPR_BIN_SHR:
    return ">>";
  default:
    return "?";
  }
}

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
static IpIndex resolve_value_path(const SemaCtx *ctx, AstNodeId use_node,
                                  StrId name) {
  if (name.idx == 0)
    return IP_NONE;
  bool found_local = false;
  IpIndex local = sema_body_scope_lookup(ctx->s, ctx->enclosing_fn, use_node,
                                         name, &found_local);
  if (found_local)
    return local;
  ScopeId internal = db_get_namespace_internal_scope(ctx->s, ctx->nsid);
  if (internal.idx != SCOPE_ID_NONE.idx) {
    DefId target = db_query_resolve_ref(ctx->s, internal, name);
    if (target.idx != DEF_ID_NONE.idx)
      return db_query_type_of_def(ctx->s, target);
  }
  AstSpan span = astspan_make(ctx->file_local, use_node);
  if (!astspan_is_none(span))
    db_emit(ctx->s, DIAG_ERROR, span, "undefined identifier '%S'", name);
  return IP_NONE;
}

// === Main entry =============================================================
//
// sema_type_of_expr is split into a thin public WRAPPER + a static
// IMPL. The wrapper caches the computed type into the per-file
// node_data.types[] array after the impl returns. This populates the
// per-node type cache used by:
//   - Phase 7's typed-body fingerprint in sema_infer_body
//   - (future) LSP hover for arbitrary expression types
//   - (future) codegen reading per-node typed bodies
//
// Recursive calls inside the impl go through the public wrapper, so
// every visited sub-expression's type is written to the cache.

static IpIndex sema_type_of_expr_impl(const SemaCtx *ctx, AstNodeId node);

// === NodeTypeBuilder — per-decl resolved-types builder =====================
//
// See sema.h for the architectural notes. The builder is a stack-
// allocated struct on the calling query body; sema_node_type_builder_begin
// allocates a contiguous IP_NONE region in db.node_types_pool. The
// caller wires the builder onto its SemaCtx (ctx->types = &b); recursive
// sema_type_of_expr / sema_resolve_type_expr calls write into the
// builder via ctx->types. No hidden global — each query body owns its
// builder, and the SemaCtx makes that ownership explicit.

void sema_node_type_builder_begin(struct db *s, NodeTypeBuilder *b,
                                  FileId file_local, uint32_t node_min,
                                  uint32_t node_max) {
  b->file_local = file_local;
  b->node_min = node_min;
  b->node_max = node_max;
  b->fp = db_fp_u64(0);
  if (node_max < node_min) {
    // Empty / degenerate range — no pool allocation.
    b->types_off = 0;
    b->types_len = 0;
  } else {
    b->types_len = node_max - node_min + 1;
    b->types_off = (uint32_t)s->node_types_pool.count;
    IpIndex none = IP_NONE;
    for (uint32_t i = 0; i < b->types_len; i++)
      vec_push(&s->node_types_pool, &none);
  }
}

void sema_node_type_builder_push(const SemaCtx *ctx, AstNodeId node,
                                 IpIndex type) {
  if (!ctx || !ctx->types)
    return;
  NodeTypeBuilder *b = ctx->types;
  if (b->types_len == 0)
    return;
  if (node.idx == AST_NODE_ID_NONE.idx)
    return;
  if (node.idx < b->node_min || node.idx > b->node_max)
    return;
  uint32_t off = b->types_off + (node.idx - b->node_min);
  IpIndex *slot = (IpIndex *)vec_get(&ctx->s->node_types_pool, off);
  *slot = type;
  // Fingerprint accumulator over (node.idx, type.v) pairs. Position-
  // insensitive within the range: edits that change a node's type
  // change the fp; pure pool-offset shifts (a reparse moving the
  // range earlier/later in the pool) do not, so sibling-decl re-runs
  // early-cut correctly.
  b->fp = db_fp_combine(b->fp, db_fp_u64((uint64_t)node.idx));
  b->fp = db_fp_combine(b->fp, db_fp_u64((uint64_t)type.v));
}

NodeTypesRange sema_node_type_builder_end(NodeTypeBuilder *b,
                                          Fingerprint *out_fp) {
  if (out_fp)
    *out_fp = b->fp;
  return (NodeTypesRange){.types_off = b->types_off,
                          .types_len = b->types_len,
                          .node_min = b->node_min};
}

IpIndex sema_node_types_range_lookup(struct db *s, NodeTypesRange range,
                                     AstNodeId node) {
  if (range.types_len == 0 || node.idx == AST_NODE_ID_NONE.idx)
    return IP_NONE;
  if (node.idx < range.node_min)
    return IP_NONE;
  uint32_t local = node.idx - range.node_min;
  if (local >= range.types_len)
    return IP_NONE;
  return *(IpIndex *)vec_get(&s->node_types_pool, range.types_off + local);
}

// (sema_cache_node_type removed 2026-05-24 — Option-C migration.
//  Per-node cache writes go through sema_node_type_builder_push into
//  the active per-decl query's pool range; the FileNodeData.types[]
//  field is gone.)

IpIndex sema_type_of_expr(const SemaCtx *ctx, AstNodeId node) {
  if (node.idx == AST_NODE_ID_NONE.idx)
    return IP_NONE;
  IpIndex result = sema_type_of_expr_impl(ctx, node);
  // Push the visited node's resolved type into the builder owned by
  // this ctx (set by the enclosing infer_body / fn_signature /
  // build_struct_type query). No-op if ctx->types is NULL or the node
  // falls outside its range — the rightful-owner ctx will pick it up
  // when its own walk reaches the node.
  sema_node_type_builder_push(ctx, node, result);
  return result;
}

static IpIndex sema_type_of_expr_impl(const SemaCtx *ctx, AstNodeId node) {
  if (node.idx == AST_NODE_ID_NONE.idx)
    return IP_NONE;
  // Locals named to match pre-refactor body code (still referenced
  // throughout the switch below). Avoids a 600-line s/X/ctx->X/g pass.
  struct db *s = ctx->s;
  ASTStore *ast = ctx->ast;
  NamespaceId nsid = ctx->nsid;
  DefId enclosing_fn = ctx->enclosing_fn;
  FileId file_local = ctx->file_local;
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
    return resolve_value_path(ctx, node, d.string_id);

  // === Arith binops — unify operand types ===
  case AST_EXPR_BIN_ADD:
  case AST_EXPR_BIN_SUB:
  case AST_EXPR_BIN_MUL:
  case AST_EXPR_BIN_DIV:
  case AST_EXPR_BIN_MOD:
  case AST_EXPR_BIN_POW: {
    IpIndex lt = sema_type_of_expr(ctx, d.bin.lhs);
    IpIndex rt = sema_type_of_expr(ctx, d.bin.rhs);
    // Suppress cascading diags: an inner failure already emitted.
    if (lt.v == IP_NONE.v || rt.v == IP_NONE.v)
      return IP_NONE;
    IpIndex u = unify_arith(lt, rt);
    if (u.v == IP_NONE.v || !is_numeric(u)) {
      AstSpan span = astspan_make(file_local, node);
      if (!astspan_is_none(span))
        db_emit(s, DIAG_ERROR, span,
                "cannot apply '%s' to operands of type %T and %T",
                binop_name(k), lt, rt);
      return IP_NONE;
    }
    return u;
  }

  // === Comparison — operands unify, result is bool ===
  case AST_EXPR_BIN_EQ:
  case AST_EXPR_BIN_NEQ:
  case AST_EXPR_BIN_LT:
  case AST_EXPR_BIN_LE:
  case AST_EXPR_BIN_GT:
  case AST_EXPR_BIN_GE: {
    IpIndex lt = sema_type_of_expr(ctx, d.bin.lhs);
    IpIndex rt = sema_type_of_expr(ctx, d.bin.rhs);
    if (lt.v == IP_NONE.v || rt.v == IP_NONE.v)
      return IP_NONE;
    if (lt.v != rt.v) {
      IpIndex u = unify_arith(lt, rt);
      if (u.v == IP_NONE.v) {
        AstSpan span = astspan_make(file_local, node);
        if (!astspan_is_none(span))
          db_emit(s, DIAG_ERROR, span,
                  "cannot apply '%s' to operands of type %T and %T",
                  binop_name(k), lt, rt);
        return IP_NONE;
      }
    }
    return IP_BOOL_TYPE;
  }

  // === Logical — bool inputs, bool result ===
  case AST_EXPR_BIN_AND:
  case AST_EXPR_BIN_OR: {
    IpIndex lt = sema_type_of_expr(ctx, d.bin.lhs);
    IpIndex rt = sema_type_of_expr(ctx, d.bin.rhs);
    if (lt.v == IP_NONE.v || rt.v == IP_NONE.v)
      return IP_NONE;
    if (lt.v != IP_BOOL_TYPE.v || rt.v != IP_BOOL_TYPE.v) {
      AstSpan span = astspan_make(file_local, node);
      if (!astspan_is_none(span)) {
        IpIndex bad = (lt.v != IP_BOOL_TYPE.v) ? lt : rt;
        db_emit(s, DIAG_ERROR, span,
                "logical '%s' requires bool operands, got %T", binop_name(k),
                bad);
      }
      return IP_NONE;
    }
    return IP_BOOL_TYPE;
  }

  // === Bit ops — unify (int restriction is a chunk-5h diag concern) ===
  case AST_EXPR_BIN_BIT_AND:
  case AST_EXPR_BIN_BIT_OR:
  case AST_EXPR_BIN_BIT_XOR:
  case AST_EXPR_BIN_SHL:
  case AST_EXPR_BIN_SHR: {
    IpIndex lt = sema_type_of_expr(ctx, d.bin.lhs);
    IpIndex rt = sema_type_of_expr(ctx, d.bin.rhs);
    if (lt.v == IP_NONE.v || rt.v == IP_NONE.v)
      return IP_NONE;
    IpIndex u = unify_arith(lt, rt);
    bool lt_ok = (lt.v == IP_COMPTIME_INT_TYPE.v) || is_concrete_int(lt);
    bool rt_ok = (rt.v == IP_COMPTIME_INT_TYPE.v) || is_concrete_int(rt);
    if (u.v == IP_NONE.v || !lt_ok || !rt_ok) {
      AstSpan span = astspan_make(file_local, node);
      if (!astspan_is_none(span))
        db_emit(s, DIAG_ERROR, span,
                "bitwise '%s' requires integer operands, got %T and %T",
                binop_name(k), lt, rt);
      return IP_NONE;
    }
    return u;
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

    IpIndex callee_ty = sema_type_of_expr(ctx, callee);
    if (callee_ty.v == IP_NONE.v)
      return IP_NONE;

    if (ip_tag(&s->intern, callee_ty) != IP_TAG_FN_TYPE) {
      AstSpan span = astspan_make(file_local, callee);
      if (!astspan_is_none(span))
        db_emit(s, DIAG_ERROR, span, "value of type %T is not callable",
                callee_ty);
      return IP_NONE;
    }

    IpKey key = ip_key(&s->intern, callee_ty);
    if (key.fn_type.n_params != arg_count) {
      AstSpan span = astspan_make(file_local, node);
      if (!astspan_is_none(span))
        db_emit(s, DIAG_ERROR, span, "call expects %d args, got %d",
                (int)key.fn_type.n_params, (int)arg_count);
      return IP_NONE;
    }

    // Check each arg against its declared param type via the
    // bidirectional checker. sema_check_expr emits "expected T" diags
    // pointing at the offending arg expression on mismatch. The call's
    // result type is the fn's return regardless of per-arg pass/fail
    // — we don't poison the surrounding type to keep cascading-diag
    // noise down.
    for (uint32_t i = 0; i < arg_count; i++) {
      AstNodeId arg = {.idx = ex[2 + i]};
      (void)sema_check_expr(ctx, arg, key.fn_type.params[i]);
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
    IpIndex recv = sema_type_of_expr(ctx, d.bin.lhs);
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

    AstSpan field_span = astspan_make(file_local, d.bin.rhs);
    switch (tag) {
    case IP_TAG_STRUCT_TYPE: {
      IpKey k = ip_key(&s->intern, recv);
      for (size_t i = 0; i < k.struct_type.n_fields; i++) {
        if (k.struct_type.field_names[i].idx == fname.idx)
          return k.struct_type.field_types[i];
      }
      if (!astspan_is_none(field_span))
        db_emit(s, DIAG_ERROR, field_span, "no field '%S' in %T", fname, recv);
      return IP_NONE;
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
      if (!astspan_is_none(field_span))
        db_emit(s, DIAG_ERROR, field_span, "no variant '%S' in %T", fname,
                recv);
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
      if (!astspan_is_none(field_span))
        db_emit(s, DIAG_ERROR, field_span,
                "no field '%S' on slice (only '.len' and '.ptr')", fname);
      return IP_NONE;
    }

    case IP_TAG_ARRAY_TYPE:
      if (fname.idx == s->names.LEN.idx)
        return IP_USIZE_TYPE;
      if (!astspan_is_none(field_span))
        db_emit(s, DIAG_ERROR, field_span,
                "no field '%S' on array (only '.len')", fname);
      return IP_NONE;

    case IP_TAG_NAMESPACE_TYPE: {
      // Namespace member access — `b.foo` where the receiver's type
      // is the file's anonymous struct (built by db_query_namespace_type
      // at @import time). Field set is the file's public top-level
      // decls; field TYPE is resolved here, lazily, via type_of_def.
      // Matches struct-field-lookup shape — the only difference is
      // the field "value" is a DefId (lazy) instead of an IpIndex
      // (eager).
      IpKey k = ip_key(&s->intern, recv);
      for (size_t i = 0; i < k.namespace_type.n_fields; i++) {
        if (k.namespace_type.field_names[i].idx == fname.idx) {
          DefId d = k.namespace_type.field_defs[i];
          return db_query_type_of_def(s, d);
        }
      }
      if (!astspan_is_none(field_span))
        db_emit(s, DIAG_ERROR, field_span, "no member '%S' in %T", fname, recv);
      return IP_NONE;
    }

    default:
      // Field access on a non-aggregate type.
      if (!astspan_is_none(field_span))
        db_emit(s, DIAG_ERROR, field_span,
                "field access on non-aggregate type %T", recv);
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
    IpIndex obj = sema_type_of_expr(ctx, d.bin.lhs);
    if (obj.v == IP_NONE.v)
      return IP_NONE;
    // Type the index for dep recording even if we can't validate it
    // here — chunk 5h's check_expr emits the int-required diag.
    (void)sema_type_of_expr(ctx, d.bin.rhs);

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
    default: {
      AstSpan span = astspan_make(file_local, d.bin.lhs);
      if (!astspan_is_none(span))
        db_emit(s, DIAG_ERROR, span, "value of type %T is not indexable", obj);
      return IP_NONE;
    }
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

    IpIndex obj = sema_type_of_expr(ctx, recv);
    if (obj.v == IP_NONE.v)
      return IP_NONE;
    // Type bounds for dep recording. Int-coercion check is chunk 5h.
    if (lo.idx != AST_NODE_ID_NONE.idx)
      (void)sema_type_of_expr(ctx, lo);
    if (hi.idx != AST_NODE_ID_NONE.idx)
      (void)sema_type_of_expr(ctx, hi);

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
    default: {
      AstSpan span = astspan_make(file_local, recv);
      if (!astspan_is_none(span))
        db_emit(s, DIAG_ERROR, span, "value of type %T is not sliceable", obj);
      return IP_NONE;
    }
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
    IpIndex t = sema_type_of_expr(ctx, d.single_child);
    if (t.v == IP_NONE.v)
      return IP_NONE;
    if (!is_numeric(t)) {
      AstSpan span = astspan_make(file_local, node);
      if (!astspan_is_none(span))
        db_emit(s, DIAG_ERROR, span,
                "unary '-' requires numeric operand, got %T", t);
      return IP_NONE;
    }
    return t;
  }
  case AST_EXPR_UNARY_BIT_NOT: {
    // Bitwise not — int operand, returns same type.
    IpIndex t = sema_type_of_expr(ctx, d.single_child);
    if (t.v == IP_NONE.v)
      return IP_NONE;
    if (t.v != IP_COMPTIME_INT_TYPE.v && !is_concrete_int(t)) {
      AstSpan span = astspan_make(file_local, node);
      if (!astspan_is_none(span))
        db_emit(s, DIAG_ERROR, span,
                "unary '~' requires integer operand, got %T", t);
      return IP_NONE;
    }
    return t;
  }
  case AST_EXPR_UNARY_NOT: {
    // Logical not — bool operand, returns bool.
    IpIndex t = sema_type_of_expr(ctx, d.single_child);
    if (t.v == IP_NONE.v)
      return IP_NONE;
    if (t.v != IP_BOOL_TYPE.v) {
      AstSpan span = astspan_make(file_local, node);
      if (!astspan_is_none(span))
        db_emit(s, DIAG_ERROR, span, "unary '!' requires bool, got %T", t);
      return IP_NONE;
    }
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
        AstSpan span = astspan_make(file_local, node);
        if (!astspan_is_none(span))
          db_emit(s, DIAG_ERROR, span,
                  "address-of '&' requires an l-value "
                  "(variable, field, index, or deref)");
        return IP_NONE;
      }
    }
    IpIndex t = sema_type_of_expr(ctx, d.single_child);
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
    IpIndex t = sema_type_of_expr(ctx, d.single_child);
    if (t.v == IP_NONE.v)
      return IP_NONE;
    IpTag tag = ip_tag(&s->intern, t);
    if (tag != IP_TAG_PTR_TYPE && tag != IP_TAG_PTR_CONST_TYPE) {
      AstSpan span = astspan_make(file_local, node);
      if (!astspan_is_none(span))
        db_emit(s, DIAG_ERROR, span, "cannot dereference non-pointer type %T",
                t);
      return IP_NONE;
    }
    return ip_key(&s->intern, t).ptr_type.elem;
  }
  case AST_EXPR_UNARY_DENIL: {
    // x? — postfix optional unwrap. Operand must be ?T; result is T.
    IpIndex t = sema_type_of_expr(ctx, d.single_child);
    if (t.v == IP_NONE.v)
      return IP_NONE;
    if (ip_tag(&s->intern, t) != IP_TAG_OPTIONAL_TYPE) {
      AstSpan span = astspan_make(file_local, node);
      if (!astspan_is_none(span))
        db_emit(s, DIAG_ERROR, span, "'.?' requires optional type, got %T", t);
      return IP_NONE;
    }
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
        (void)sema_check_expr(ctx, d.single_child, k.fn_type.ret);
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
      last = sema_type_of_expr(ctx, stmt);
    }
    return last;
  }

  // === @builtin(...) ===
  //
  // Extras layout: [name_strid, arg_count, arg0_node_id, ...].
  // Dispatch routes through src/sema/builtins.c (Phase 3c table).
  // Comptime work later adds rows for @sizeOf, @TypeOf, @compileError,
  // @embedFile, @cImport, etc. — zero diff to this file per builtin.
  case AST_EXPR_BUILTIN: {
    const uint32_t *extras = (uint32_t *)ast->extra.data;
    uint32_t base = d.extra_idx.idx;
    StrId name = {.idx = extras[base + 0]};
    uint32_t arg_count = extras[base + 1];
    const AstNodeId *arg_nodes = (const AstNodeId *)&extras[base + 2];

    // AstSpan anchor for the call site — passed to sema_dispatch_builtin
    // for diag emission.
    AstSpan span = astspan_make(file_local, node);

    return sema_dispatch_builtin(s, nsid, ast, name, arg_nodes,
                                 (size_t)arg_count, span);
  }

  default: {
    // ORELSE / CATCH, assignments, product, pattern matching, INC/DEERR
    // (need lvalue + error story), if/loop/switch as expressions, etc.
    // — later sub-chunks. Emit a diagnostic so the failure is loud
    // rather than silently propagating IP_NONE up the cascade; the
    // user sees "expression kind X not yet supported" at the actual
    // source location instead of "?" leaking into every downstream
    // hover / type display.
    AstNodeKind k_node = ((AstNodeKind *)ast->kinds.data)[node.idx];
    AstSpan span = astspan_make(file_local, node);
    if (!astspan_is_none(span)) {
      db_emit(s, DIAG_ERROR, span,
              "expression kind %s not yet implemented in type inference",
              ast_kind_name(k_node));
    }
    return IP_NONE;
  }
  }
}
