#include "../ast/ast_expr.h"
#include "../ast/ast_stmt.h"
#include "../db/db.h"
#include "../db/diag/diag.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/fn_signature.h"
#include "../db/query/query_engine.h"
#include "../db/query/resolve_ref.h"
#include "../db/query/type_of_def.h"
#include "../db/workspace/workspace.h"
#include "../syntax/syntax_kind.h"
#include "../support/data_structure/arena.h"
#include "../support/data_structure/hashmap.h"
#include "../support/data_structure/stringpool.h"
#include "../syntax/syntax.h"
#include "builtins.h"
#include "sema.h"

#include <stdbool.h>

// === Op-name helper for diag templates =====================================

static const char *opkind_name(SyntaxKind k) {
  switch (k) {
  case SK_PLUS:      return "+";
  case SK_MINUS:     return "-";
  case SK_STAR:      return "*";
  case SK_SLASH:     return "/";
  case SK_PERCENT:   return "%";
  case SK_STAR_STAR: return "**";
  case SK_EQ_EQ:     return "==";
  case SK_BANG_EQ:   return "!=";
  case SK_LT:        return "<";
  case SK_LE:        return "<=";
  case SK_GT:        return ">";
  case SK_GE:        return ">=";
  case SK_AMP_AMP:   return "&&";
  case SK_PIPE_PIPE: return "||";
  case SK_AMP:       return "&";
  case SK_PIPE:      return "|";
  case SK_CARET:     return "^";
  case SK_SHL:       return "<<";
  case SK_SHR:       return ">>";
  case SK_BANG:      return "!";
  case SK_TILDE:     return "~";
  case SK_QUESTION:  return "?";
  default:           return "?";
  }
}

// === Numeric predicates =====================================================
//
// Bitmasks over IpReservedIndex values — all reserved primitives have
// IpIndex.v < 32 today (extend to u64 if we ever blow that), so each
// predicate is a single shift+and. The masks are built from the enum
// constants directly, so reordering ip_primitives.def keeps them correct.

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

// Arith unification, Zig-style. comptime_int / comptime_float coerce up
// to matching concretes (or to each other in mixed numeric ops); concrete
// + different concrete returns IP_NONE.
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

// === Literal token-kind → built-in type ====================================

static IpIndex type_from_lit_token(SyntaxKind tk) {
  switch (tk) {
  case SK_INT_LIT:    return IP_COMPTIME_INT_TYPE;
  case SK_FLOAT_LIT:  return IP_COMPTIME_FLOAT_TYPE;
  case SK_TRUE_KW:
  case SK_FALSE_KW:   return IP_BOOL_TYPE;
  case SK_BYTE_LIT:   return IP_U8_TYPE;
  case SK_STRING_LIT: return IP_STRING_SLICE_TYPE;
  case SK_NIL_KW:     return IP_NIL_TYPE;
  default:            return IP_NONE;
  }
}

// === Helpers ================================================================

static TinySpan span_of(const SemaCtx *ctx, SyntaxNode *node) {
  TextRange r = syntax_node_text_range(node);
  return span_make((uint16_t)ctx->file_local.idx, r.start, r.length);
}

static StrId intern_tok(struct db *s, SyntaxToken *t) {
  if (!t)
    return (StrId){0};
  const char *txt = syntax_token_text(t);
  uint32_t len = syntax_token_text_range(t).length;
  return pool_intern(&s->strings, txt, len);
}

// === Value-position identifier resolution ===================================
//
// Body-scope chain (params + let-binds) first via sema_body_scope_lookup;
// fall through to the module's internal scope on miss. resolve_ref +
// type_of_def calls register their salsa deps on the outer query's frame.
// `use_node` is the path-use SyntaxNode — needed to locate its enclosing
// body scope. sema_body_scope_lookup reads body-scope pools raw (no dep),
// since the outer query has already declared a dep on body_scopes(enclosing_fn).
static IpIndex resolve_value_path(const SemaCtx *ctx, SyntaxNode *use_node,
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
  db_emit(ctx->s, DIAG_ERROR, span_of(ctx, use_node),
          "undefined identifier '%S'", name);
  return IP_NONE;
}

// === NodeTypeBuilder — HashMap<SyntaxNodePtr, IpIndex> ======================
//
// rust-analyzer's InferenceResult pattern: each per-decl query that types
// a sub-tree (infer_body / fn_signature / build_struct_type) constructs
// a builder, stamps a pointer onto its SemaCtx, and lets recursive
// type-resolving sema calls write into the builder via ctx->types.
// builder_end transfers ownership of the HashMap into a NodeTypesRange
// that lands on a db column.

void sema_node_type_builder_begin(struct db *s, NodeTypeBuilder *b,
                                  FileId file_local) {
  (void)s;
  b->file_local = file_local;
  hashmap_init(&b->types);
  b->fp = db_fp_u64(0);
}

void sema_node_type_builder_push(const SemaCtx *ctx, SyntaxNode *node,
                                 IpIndex type) {
  if (!ctx || !ctx->types || !node)
    return;
  NodeTypeBuilder *b = ctx->types;
  uint64_t key = syntax_node_ptr_hash(syntax_node_ptr_new(node));
  // Store IpIndex.v in the HashMap's void* value slot. Idempotent —
  // latest write wins (matches old dense-pool overwrite semantics).
  hashmap_put(&b->types, key, (void *)(uintptr_t)type.v);
  // Fingerprint accumulator over (key, type.v) pairs. Position-
  // insensitive: edits that change a node's type change the fp; pool /
  // address shifts (a reparse producing different in-memory addresses)
  // don't, because syntax_node_ptr_hash is over (kind, byte-range).
  b->fp = db_fp_combine(b->fp, db_fp_u64(key));
  b->fp = db_fp_combine(b->fp, db_fp_u64((uint64_t)type.v));
}

NodeTypesRange sema_node_type_builder_end(NodeTypeBuilder *b,
                                          Fingerprint *out_fp) {
  if (out_fp)
    *out_fp = b->fp;
  // Move the map into the range; the builder is stack-local and goes
  // out of scope. Zero the builder's slot so callers calling end twice
  // would see an empty range, not a double-free.
  NodeTypesRange out = {.types = b->types};
  b->types = (HashMap){0};
  return out;
}

IpIndex sema_node_types_range_lookup(struct db *s, NodeTypesRange range,
                                     SyntaxNode *node) {
  (void)s;
  if (!node || !hashmap_is_initialized(&range.types))
    return IP_NONE;
  uint64_t key = syntax_node_ptr_hash(syntax_node_ptr_new(node));
  void *v = hashmap_get(&range.types, key);
  if (!v)
    return IP_NONE;
  return (IpIndex){.v = (uint32_t)(uintptr_t)v};
}

// === Main entry =============================================================
//
// sema_type_of_expr is split into a thin public WRAPPER + a static IMPL.
// The wrapper pushes the computed type into the ctx's active NodeTypeBuilder
// after the impl returns. Recursive calls go through the wrapper, so every
// visited sub-expression's type lands in the builder for downstream
// consumers (LSP hover, codegen).

static IpIndex sema_type_of_expr_impl(const SemaCtx *ctx, SyntaxNode *node);

IpIndex sema_type_of_expr(const SemaCtx *ctx, SyntaxNode *node) {
  if (!node)
    return IP_NONE;
  IpIndex result = sema_type_of_expr_impl(ctx, node);
  sema_node_type_builder_push(ctx, node, result);
  return result;
}

// === Argument-list collection ==============================================
//
// Walks SK_ARG_LIST and returns its direct node children as a heap-allocated
// SyntaxNode* array (request-arena, freed at request end). Caller releases
// each entry — these are RETURNS_OWNED.
static SyntaxNode **collect_arg_nodes(struct db *s, SyntaxNode *arg_list,
                                      uint32_t *out_count) {
  *out_count = 0;
  if (!arg_list)
    return NULL;
  uint32_t total = syntax_node_num_children(arg_list);
  // First pass: count node children.
  uint32_t n = 0;
  for (uint32_t i = 0; i < total; i++) {
    GreenElement g = green_node_child(syntax_node_green(arg_list), i);
    if (g.kind == GREEN_ELEM_NODE)
      n++;
  }
  if (n == 0)
    return NULL;
  SyntaxNode **out =
      arena_alloc(&s->request_arena, n * sizeof(SyntaxNode *));
  if (!out)
    return NULL;
  uint32_t k = 0;
  for (uint32_t i = 0; i < total && k < n; i++) {
    SyntaxElement el = syntax_node_child_or_token(arg_list, i);
    if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      out[k++] = el.node;
    } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      syntax_token_release(el.token);
    }
  }
  *out_count = k;
  return out;
}

// Release `count` SyntaxNode* entries from an arg-node array.
static void release_arg_nodes(SyntaxNode **args, uint32_t count) {
  for (uint32_t i = 0; i < count; i++)
    if (args[i])
      syntax_node_release(args[i]);
}

// === The main switch ========================================================

static IpIndex sema_type_of_expr_impl(const SemaCtx *ctx, SyntaxNode *node) {
  if (!node)
    return IP_NONE;
  struct db *s = ctx->s;
  NamespaceId nsid = ctx->nsid;
  DefId enclosing_fn = ctx->enclosing_fn;
  SyntaxKind k = syntax_node_kind(node);

  switch (k) {

  // === Literal — token-kind disambiguates the actual type ===
  case SK_LITERAL_EXPR: {
    Literal lit;
    if (!Literal_cast(node, &lit))
      return IP_NONE;
    SyntaxKind tk = Literal_kind(&lit);
    return type_from_lit_token(tk);
  }

  // === Single-ident reference: name → type ===
  case SK_REF_EXPR: {
    RefExpr r;
    if (!RefExpr_cast(node, &r))
      return IP_NONE;
    SyntaxToken *nt = RefExpr_name(&r);
    StrId name = intern_tok(s, nt);
    if (nt) syntax_token_release(nt);
    return resolve_value_path(ctx, node, name);
  }

  // === Multi-segment path: foo.bar.baz ===
  //
  // For value-position single-leaf resolution we use the LAST IDENT
  // segment (matches the type-position handling in type_resolve.c).
  // Full dotted-segment resolution lives in a future port; the current
  // pipeline relies on FieldExpr for module/struct access.
  case SK_PATH_EXPR: {
    StrId last = {0};
    uint32_t count = syntax_node_num_children(node);
    for (uint32_t i = 0; i < count; i++) {
      GreenElement g = green_node_child(syntax_node_green(node), i);
      if (g.kind == GREEN_ELEM_TOKEN && green_token_kind(g.token) == SK_IDENT) {
        const char *txt = green_token_text(g.token);
        uint32_t len = green_token_text_len(g.token);
        last = pool_intern(&s->strings, txt, len);
      }
    }
    return resolve_value_path(ctx, node, last);
  }

  // === Paren — transparent =====================================
  case SK_PAREN_EXPR: {
    ParenExpr pe;
    if (!ParenExpr_cast(node, &pe))
      return IP_NONE;
    SyntaxNode *inner = ParenExpr_inner(&pe);
    IpIndex t = inner ? sema_type_of_expr(ctx, inner) : IP_NONE;
    if (inner) syntax_node_release(inner);
    return t;
  }

  // === Binary operators (op-collapsed) ========================
  case SK_BIN_EXPR: {
    BinExpr be;
    if (!BinExpr_cast(node, &be))
      return IP_NONE;
    SyntaxKind opk = BinExpr_op_kind(&be);
    SyntaxNode *lhs = BinExpr_lhs(&be);
    SyntaxNode *rhs = BinExpr_rhs(&be);
    IpIndex lt = lhs ? sema_type_of_expr(ctx, lhs) : IP_NONE;
    IpIndex rt = rhs ? sema_type_of_expr(ctx, rhs) : IP_NONE;
    if (lhs) syntax_node_release(lhs);
    if (rhs) syntax_node_release(rhs);

    // Suppress cascading diags: an inner failure already emitted.
    if (lt.v == IP_NONE.v || rt.v == IP_NONE.v)
      return IP_NONE;

    // Classify the operator group from the op-token kind.
    bool is_arith = (opk == SK_PLUS || opk == SK_MINUS || opk == SK_STAR ||
                     opk == SK_SLASH || opk == SK_PERCENT ||
                     opk == SK_STAR_STAR);
    bool is_compare = (opk == SK_EQ_EQ || opk == SK_BANG_EQ || opk == SK_LT ||
                       opk == SK_LE || opk == SK_GT || opk == SK_GE);
    bool is_logical = (opk == SK_AMP_AMP || opk == SK_PIPE_PIPE);
    bool is_bitop_value = (opk == SK_AMP || opk == SK_PIPE || opk == SK_CARET);
    bool is_shift = (opk == SK_SHL || opk == SK_SHR);

    if (is_arith) {
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
      if (lt.v != rt.v) {
        IpIndex u = unify_arith(lt, rt);
        if (u.v == IP_NONE.v) {
          db_emit(s, DIAG_ERROR, span_of(ctx, node),
                  "cannot apply '%s' to operands of type %T and %T",
                  opkind_name(opk), lt, rt);
          return IP_NONE;
        }
      }
      return IP_BOOL_TYPE;
    }

    if (is_logical) {
      if (lt.v != IP_BOOL_TYPE.v || rt.v != IP_BOOL_TYPE.v) {
        IpIndex bad = (lt.v != IP_BOOL_TYPE.v) ? lt : rt;
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "logical '%s' requires bool operands, got %T",
                opkind_name(opk), bad);
        return IP_NONE;
      }
      return IP_BOOL_TYPE;
    }

    if (is_bitop_value || is_shift) {
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

    // Unrecognized op token (range-typing op like `..`, assign op which
    // is the assign-expr's domain, etc.). Loud diag.
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "binary operator '%s' not yet supported in type inference",
            opkind_name(opk));
    return IP_NONE;
  }

  // === Prefix unary ops: - ! ~ & ============================================
  //
  // Operator kind is the SK_* token in PrefixExpr_op.
  case SK_PREFIX_EXPR: {
    PrefixExpr pe;
    if (!PrefixExpr_cast(node, &pe))
      return IP_NONE;
    SyntaxKind opk = PrefixExpr_op_kind(&pe);
    SyntaxNode *operand = PrefixExpr_operand(&pe);
    if (!operand)
      return IP_NONE;

    if (opk == SK_MINUS) {
      IpIndex t = sema_type_of_expr(ctx, operand);
      syntax_node_release(operand);
      if (t.v == IP_NONE.v)
        return IP_NONE;
      if (!is_numeric(t)) {
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "unary '-' requires numeric operand, got %T", t);
        return IP_NONE;
      }
      return t;
    }
    if (opk == SK_TILDE) {
      IpIndex t = sema_type_of_expr(ctx, operand);
      syntax_node_release(operand);
      if (t.v == IP_NONE.v)
        return IP_NONE;
      if (t.v != IP_COMPTIME_INT_TYPE.v && !is_concrete_int(t)) {
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "unary '~' requires integer operand, got %T", t);
        return IP_NONE;
      }
      return t;
    }
    if (opk == SK_BANG) {
      IpIndex t = sema_type_of_expr(ctx, operand);
      syntax_node_release(operand);
      if (t.v == IP_NONE.v)
        return IP_NONE;
      if (t.v != IP_BOOL_TYPE.v) {
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "unary '!' requires bool, got %T", t);
        return IP_NONE;
      }
      return IP_BOOL_TYPE;
    }
    if (opk == SK_AMP) {
      // L-value check — address-of only makes sense on storage locations:
      // identifier / field / index / pointer-deref. Postfix `x^` (deref)
      // counts; postfix `x?` (denil) does not.
      SyntaxKind ck = syntax_node_kind(operand);
      bool is_lvalue = (ck == SK_REF_EXPR || ck == SK_PATH_EXPR ||
                        ck == SK_FIELD_EXPR || ck == SK_INDEX_EXPR);
      if (!is_lvalue && ck == SK_POSTFIX_EXPR) {
        PostfixExpr inner;
        if (PostfixExpr_cast(operand, &inner) &&
            PostfixExpr_op_kind(&inner) == SK_CARET)
          is_lvalue = true;
      }
      if (!is_lvalue) {
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "address-of '&' requires an l-value "
                "(variable, field, index, or deref)");
        syntax_node_release(operand);
        return IP_NONE;
      }
      IpIndex t = sema_type_of_expr(ctx, operand);
      syntax_node_release(operand);
      if (t.v == IP_NONE.v)
        return IP_NONE;
      IpKey pk = {.kind = IPK_PTR_TYPE,
                  .ptr_type = {.elem = t, .is_const = false}};
      return ip_get(&s->intern, pk);
    }
    syntax_node_release(operand);
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "prefix operator '%s' not yet supported in type inference",
            opkind_name(opk));
    return IP_NONE;
  }

  // === Postfix unary ops: ^ ? ++ -- =========================================
  case SK_POSTFIX_EXPR: {
    PostfixExpr po;
    if (!PostfixExpr_cast(node, &po))
      return IP_NONE;
    SyntaxKind opk = PostfixExpr_op_kind(&po);
    SyntaxNode *operand = PostfixExpr_operand(&po);
    if (!operand)
      return IP_NONE;

    if (opk == SK_CARET) {
      // Deref — reads through ^T or ^const T.
      IpIndex t = sema_type_of_expr(ctx, operand);
      syntax_node_release(operand);
      if (t.v == IP_NONE.v)
        return IP_NONE;
      IpTag tag = ip_tag(&s->intern, t);
      if (tag != IP_TAG_PTR_TYPE && tag != IP_TAG_PTR_CONST_TYPE) {
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "cannot dereference non-pointer type %T", t);
        return IP_NONE;
      }
      return ip_key(&s->intern, t).ptr_type.elem;
    }
    if (opk == SK_QUESTION) {
      // Optional unwrap — `x?` requires `?T`, yields `T`.
      IpIndex t = sema_type_of_expr(ctx, operand);
      syntax_node_release(operand);
      if (t.v == IP_NONE.v)
        return IP_NONE;
      if (ip_tag(&s->intern, t) != IP_TAG_OPTIONAL_TYPE) {
        db_emit(s, DIAG_ERROR, span_of(ctx, node),
                "'.?' requires optional type, got %T", t);
        return IP_NONE;
      }
      return ip_key(&s->intern, t).optional_type.elem;
    }
    // `++` / `--` produce no value (statement-like). Type as the operand
    // for now — proper "void / lvalue increment" semantics is a follow-up.
    IpIndex t = sema_type_of_expr(ctx, operand);
    syntax_node_release(operand);
    return t;
  }

  // === Call: f(arg1, arg2, ...) ===========================================
  //
  // Structural checks only — verify callee resolves to a fn type and arg
  // count matches; check each arg via bidirectional sema_check_expr against
  // its declared param type. Polymorphic / comptime-arg call resolution
  // (`Vec(i32)` etc.) lands with the comptime engine.
  case SK_CALL_EXPR: {
    CallExpr ce;
    if (!CallExpr_cast(node, &ce))
      return IP_NONE;
    SyntaxNode *callee = CallExpr_callee(&ce);
    SyntaxNode *arg_list = CallExpr_args(&ce);

    IpIndex callee_ty = callee ? sema_type_of_expr(ctx, callee) : IP_NONE;
    if (callee_ty.v == IP_NONE.v) {
      if (callee) syntax_node_release(callee);
      if (arg_list) syntax_node_release(arg_list);
      return IP_NONE;
    }

    if (ip_tag(&s->intern, callee_ty) != IP_TAG_FN_TYPE) {
      db_emit(s, DIAG_ERROR, span_of(ctx, callee ? callee : node),
              "value of type %T is not callable", callee_ty);
      syntax_node_release(callee);
      if (arg_list) syntax_node_release(arg_list);
      return IP_NONE;
    }
    syntax_node_release(callee);

    IpKey key = ip_key(&s->intern, callee_ty);
    uint32_t n_args = 0;
    SyntaxNode **args = collect_arg_nodes(s, arg_list, &n_args);
    if (arg_list) syntax_node_release(arg_list);

    if (key.fn_type.n_params != n_args) {
      db_emit(s, DIAG_ERROR, span_of(ctx, node),
              "call expects %d args, got %d", (int32_t)key.fn_type.n_params,
              (int32_t)n_args);
      release_arg_nodes(args, n_args);
      return IP_NONE;
    }

    // Check each arg against its declared param type. The call's result
    // is the fn's return regardless of per-arg pass/fail — we don't
    // poison the surrounding type to keep cascading-diag noise down.
    for (uint32_t i = 0; i < n_args; i++) {
      (void)sema_check_expr(ctx, args[i], key.fn_type.params[i]);
    }
    release_arg_nodes(args, n_args);

    return key.fn_type.ret;
  }

  // === Field access: obj.field ==========================================
  //
  // Handles struct fields, enum variants, slice .len/.ptr, array .len,
  // plus single-pointer auto-deref (Zig-style). Visibility checks, module
  // member resolution beyond the namespace-type lookup below, and tagged-
  // union active-variant narrowing are deferred.
  case SK_FIELD_EXPR: {
    FieldExpr fe;
    if (!FieldExpr_cast(node, &fe))
      return IP_NONE;
    SyntaxNode *base = FieldExpr_base(&fe);
    SyntaxToken *fname_tok = FieldExpr_field(&fe);
    StrId fname = intern_tok(s, fname_tok);
    if (fname_tok) syntax_token_release(fname_tok);

    IpIndex recv = base ? sema_type_of_expr(ctx, base) : IP_NONE;
    TinySpan field_span =
        span_of(ctx, base ? base : node); // best-effort anchor
    if (base) syntax_node_release(base);

    if (recv.v == IP_NONE.v || fname.idx == 0)
      return IP_NONE;

    // Auto-deref single pointers — `ptr.field` reads through the ptr.
    // Many-pointers DON'T auto-deref (no single-element semantics; you
    // index them).
    IpTag tag = ip_tag(&s->intern, recv);
    if (tag == IP_TAG_PTR_TYPE || tag == IP_TAG_PTR_CONST_TYPE) {
      IpKey pk = ip_key(&s->intern, recv);
      recv = pk.ptr_type.elem;
      tag = ip_tag(&s->intern, recv);
    }

    switch (tag) {
    case IP_TAG_STRUCT_TYPE: {
      IpKey sk = ip_key(&s->intern, recv);
      for (size_t i = 0; i < sk.struct_type.n_fields; i++) {
        if (sk.struct_type.field_names[i].idx == fname.idx)
          return sk.struct_type.field_types[i];
      }
      db_emit(s, DIAG_ERROR, field_span, "no field '%S' in %T", fname, recv);
      return IP_NONE;
    }
    case IP_TAG_ENUM_TYPE: {
      IpKey ek = ip_key(&s->intern, recv);
      for (size_t i = 0; i < ek.enum_type.n_variants; i++) {
        if (ek.enum_type.variant_names[i].idx == fname.idx)
          return recv;
      }
      db_emit(s, DIAG_ERROR, field_span, "no variant '%S' in %T", fname, recv);
      return IP_NONE;
    }
    case IP_TAG_SLICE_TYPE:
    case IP_TAG_SLICE_CONST_TYPE: {
      if (fname.idx == s->names.LEN.idx)
        return IP_USIZE_TYPE;
      if (fname.idx == s->names.PTR.idx) {
        // Zig ABI: `slice.ptr` is `[^]T` preserving const-ness.
        IpKey sk = ip_key(&s->intern, recv);
        bool is_const = (tag == IP_TAG_SLICE_CONST_TYPE);
        IpKey mp = {.kind = IPK_MANY_PTR_TYPE,
                    .many_ptr_type = {.elem = sk.slice_type.elem,
                                      .is_const = is_const}};
        return ip_get(&s->intern, mp);
      }
      db_emit(s, DIAG_ERROR, field_span,
              "no field '%S' on slice (only '.len' and '.ptr')", fname);
      return IP_NONE;
    }
    case IP_TAG_ARRAY_TYPE:
      if (fname.idx == s->names.LEN.idx)
        return IP_USIZE_TYPE;
      db_emit(s, DIAG_ERROR, field_span,
              "no field '%S' on array (only '.len')", fname);
      return IP_NONE;
    case IP_TAG_NAMESPACE_TYPE: {
      // Namespace member access — `b.foo` where the receiver's type is a
      // file's anonymous struct (built lazily by db_query_namespace_type
      // at @import time). Field value is a DefId (lazy); type via
      // type_of_def.
      IpKey nk = ip_key(&s->intern, recv);
      for (size_t i = 0; i < nk.namespace_type.n_fields; i++) {
        if (nk.namespace_type.field_names[i].idx == fname.idx) {
          DefId d = nk.namespace_type.field_defs[i];
          return db_query_type_of_def(s, d);
        }
      }
      db_emit(s, DIAG_ERROR, field_span, "no member '%S' in %T", fname, recv);
      return IP_NONE;
    }
    default:
      db_emit(s, DIAG_ERROR, field_span,
              "field access on non-aggregate type %T", recv);
      return IP_NONE;
    }
  }

  // === Index: obj[i] ====================================================
  case SK_INDEX_EXPR: {
    IndexExpr ie;
    if (!IndexExpr_cast(node, &ie))
      return IP_NONE;
    SyntaxNode *base = IndexExpr_base(&ie);
    SyntaxNode *idx = IndexExpr_index(&ie);
    IpIndex obj = base ? sema_type_of_expr(ctx, base) : IP_NONE;
    // Type the index for dep recording; coerce-to-int is enforced by
    // check_expr in a future port.
    if (idx) (void)sema_type_of_expr(ctx, idx);
    TinySpan base_span = span_of(ctx, base ? base : node);
    if (base) syntax_node_release(base);
    if (idx) syntax_node_release(idx);

    if (obj.v == IP_NONE.v)
      return IP_NONE;

    IpTag tag = ip_tag(&s->intern, obj);
    IpKey key = ip_key(&s->intern, obj);
    switch (tag) {
    case IP_TAG_ARRAY_TYPE:
      return key.array_type.elem;
    case IP_TAG_SLICE_TYPE:
    case IP_TAG_SLICE_CONST_TYPE:
      return key.slice_type.elem;
    case IP_TAG_MANY_PTR_TYPE:
    case IP_TAG_MANY_PTR_CONST_TYPE:
      return key.many_ptr_type.elem;
    default:
      db_emit(s, DIAG_ERROR, base_span,
              "value of type %T is not indexable", obj);
      return IP_NONE;
    }
  }

  // === Slice: obj[lo..hi] / obj[lo..] / obj[..hi] ========================
  case SK_SLICE_EXPR: {
    SliceExpr se;
    if (!SliceExpr_cast(node, &se))
      return IP_NONE;
    SyntaxNode *base = SliceExpr_base(&se);
    SyntaxNode *lo = SliceExpr_lo(&se);
    SyntaxNode *hi = SliceExpr_hi(&se);

    IpIndex obj = base ? sema_type_of_expr(ctx, base) : IP_NONE;
    if (lo) (void)sema_type_of_expr(ctx, lo);
    if (hi) (void)sema_type_of_expr(ctx, hi);
    TinySpan base_span = span_of(ctx, base ? base : node);
    if (base) syntax_node_release(base);
    if (lo) syntax_node_release(lo);
    if (hi) syntax_node_release(hi);

    if (obj.v == IP_NONE.v)
      return IP_NONE;

    IpTag tag = ip_tag(&s->intern, obj);
    IpKey key = ip_key(&s->intern, obj);
    IpIndex elem = IP_NONE;
    bool is_const = false;
    switch (tag) {
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
      db_emit(s, DIAG_ERROR, base_span,
              "value of type %T is not sliceable", obj);
      return IP_NONE;
    }
    IpKey out = {.kind = IPK_SLICE_TYPE,
                 .slice_type = {.elem = elem, .is_const = is_const}};
    return ip_get(&s->intern, out);
  }

  // === Return statement: type as noreturn, check value vs declared =====
  case SK_RETURN_STMT: {
    ReturnStmt rs;
    if (!ReturnStmt_cast(node, &rs))
      return IP_NORETURN_TYPE;
    SyntaxNode *val = ReturnStmt_value(&rs);
    if (enclosing_fn.idx != DEF_ID_NONE.idx && val) {
      IpIndex sig = db_query_fn_signature(s, enclosing_fn);
      if (sig.v != IP_NONE.v && ip_tag(&s->intern, sig) == IP_TAG_FN_TYPE) {
        IpKey k_sig = ip_key(&s->intern, sig);
        (void)sema_check_expr(ctx, val, k_sig.fn_type.ret);
      }
    }
    if (val) syntax_node_release(val);
    return IP_NORETURN_TYPE;
  }

  // === Block expression: { stmt; stmt; tail } returns tail's type =====
  //
  // All statements are typed (records deps + lets sub-expression diags
  // fire). The block's type is the LAST node-child's type, or IP_VOID
  // for an empty block.
  case SK_BLOCK_STMT: {
    BlockStmt bs;
    if (!BlockStmt_cast(node, &bs))
      return IP_NONE;
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
      last = sema_type_of_expr(ctx, el.node);
      saw = true;
      syntax_node_release(el.node);
    }
    syntax_node_release(stmts);
    return saw ? last : IP_VOID_TYPE;
  }

  // === @builtin(...) ====================================================
  //
  // Routes through src/sema/builtins.c (Phase 3c dispatch table). Future
  // builtins (@sizeOf, @TypeOf, @compileError, @embedFile, @cImport, ...)
  // add a single table row each — zero diff to this file per builtin.
  case SK_BUILTIN_EXPR: {
    BuiltinExpr be;
    if (!BuiltinExpr_cast(node, &be))
      return IP_NONE;
    SyntaxToken *name_tok = BuiltinExpr_name(&be);
    StrId name = intern_tok(s, name_tok);
    if (name_tok) syntax_token_release(name_tok);
    SyntaxNode *arg_list = BuiltinExpr_args(&be);
    uint32_t n_args = 0;
    SyntaxNode **args = collect_arg_nodes(s, arg_list, &n_args);
    if (arg_list) syntax_node_release(arg_list);
    TinySpan anchor = span_of(ctx, node);
    IpIndex r = sema_dispatch_builtin(s, nsid, name,
                                      (SyntaxNode *const *)args,
                                      (size_t)n_args, anchor);
    release_arg_nodes(args, n_args);
    return r;
  }

  default:
    // Expression kinds the inference pipeline doesn't handle yet
    // (ORELSE / CATCH, assignments, full pattern matching, handle /
    // handler bodies as values, if/loop/switch as expressions, ...).
    // Emit a loud diag so the failure lands at the actual source
    // location rather than silently propagating IP_NONE.
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "expression kind %d not yet implemented in type inference",
            (int)k);
    return IP_NONE;
  }
}
