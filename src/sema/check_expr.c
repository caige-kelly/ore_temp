#include "../ast/ast_expr.h"
#include "../ast/ast_stmt.h"
#include "../db/db.h"
#include "../db/diag/diag.h"
#include "../db/intern_pool/intern_pool.h"
#include "../parser/syntax_kind.h"
#include "../support/data_structure/stringpool.h"
#include "../syntax/syntax.h"
#include "sema.h"

#include <stdbool.h>

// Bidirectional type checker — ported from the prior flat-AST version.
//
// v1 scope:
//
//   - Coercion: equal types pass; comptime_int → any concrete int/float;
//     comptime_float → f32/f64. Pointer / slice / many-ptr / optional
//     variance handled below. Full Zig-variance polish (error-union
//     wrapping, etc.) is the chunk-when-we-port-coerce.c follow-up.
//   - Bidirectional flow:
//       SK_BLOCK_STMT — propagate `expected` to the LAST statement;
//                       earlier statements synth (for deps + diags).
//       SK_IF_EXPR    — propagate to BOTH branches independently so
//                       a wrong branch is pinpointed in the diag
//                       rather than reported as "branches don't match".
//   - Everything else: synth-then-compare.
//
// Diag emission lands on the current query frame's slot (per the
// QuerySlot diag pipeline). Caller must be inside a query body.

// Full Zig-style coercion table.
//
//   ^T          → ^const T           (drop mut on single ptr)
//   []T         → []const T          (drop mut on slice)
//   [^]T        → [^]const T         (drop mut on many-ptr)
//   ^[N]T       → []T / []const T    (array-ptr decays to slice; const flows)
//   ^[N]T       → [^]T / [^]const T  (array-ptr decays to many-ptr; const
//   flows) T           → ?T                 (optional lift — speculative inner
//   check) nil         → ?T / ^T / [^]T / []T  (nil lifts to nullable-storage)
//   noreturn    → anything           (bottom type)
//   comptime_int → any concrete int / comptime_float / any concrete float
//   comptime_float → f32/f64
//   equal       → equal              (pointer-equal/interned)
//
// Range-check for comptime_int → concrete (does the value fit?) is
// deferred to chunk 6 (const_eval) — for now we accept structurally.
//
// IP_NONE on either side is silent (we don't poison cascading diags).
static bool can_coerce(struct db *s, IpIndex actual, IpIndex expected) {
  if (actual.v == IP_NONE.v || expected.v == IP_NONE.v)
    return true;
  if (actual.v == expected.v)
    return true;

  if (actual.v == IP_NORETURN_TYPE.v)
    return true;

  IpTag at = ip_tag(&s->intern, actual);
  IpTag et = ip_tag(&s->intern, expected);

  // === Pointer variance: ^T → ^const T (same elem, drop mut) ===
  if ((at == IP_TAG_PTR_TYPE || at == IP_TAG_PTR_CONST_TYPE) &&
      (et == IP_TAG_PTR_TYPE || et == IP_TAG_PTR_CONST_TYPE)) {
    IpKey ak = ip_key(&s->intern, actual);
    IpKey ek = ip_key(&s->intern, expected);
    if (ak.ptr_type.elem.v == ek.ptr_type.elem.v) {
      bool a_const = (at == IP_TAG_PTR_CONST_TYPE);
      bool e_const = (et == IP_TAG_PTR_CONST_TYPE);
      if (!a_const || e_const)
        return true;
    }
  }

  // === Slice variance: []T → []const T ===
  if ((at == IP_TAG_SLICE_TYPE || at == IP_TAG_SLICE_CONST_TYPE) &&
      (et == IP_TAG_SLICE_TYPE || et == IP_TAG_SLICE_CONST_TYPE)) {
    IpKey ak = ip_key(&s->intern, actual);
    IpKey ek = ip_key(&s->intern, expected);
    if (ak.slice_type.elem.v == ek.slice_type.elem.v) {
      bool a_const = (at == IP_TAG_SLICE_CONST_TYPE);
      bool e_const = (et == IP_TAG_SLICE_CONST_TYPE);
      if (!a_const || e_const)
        return true;
    }
  }

  // === Many-ptr variance: [^]T → [^]const T ===
  if ((at == IP_TAG_MANY_PTR_TYPE || at == IP_TAG_MANY_PTR_CONST_TYPE) &&
      (et == IP_TAG_MANY_PTR_TYPE || et == IP_TAG_MANY_PTR_CONST_TYPE)) {
    IpKey ak = ip_key(&s->intern, actual);
    IpKey ek = ip_key(&s->intern, expected);
    if (ak.many_ptr_type.elem.v == ek.many_ptr_type.elem.v) {
      bool a_const = (at == IP_TAG_MANY_PTR_CONST_TYPE);
      bool e_const = (et == IP_TAG_MANY_PTR_CONST_TYPE);
      if (!a_const || e_const)
        return true;
    }
  }

  // === Array-ptr decay: ^[N]T → []T / [^]T (const flows) ===
  if (at == IP_TAG_PTR_TYPE || at == IP_TAG_PTR_CONST_TYPE) {
    IpKey ak = ip_key(&s->intern, actual);
    IpTag pt = ip_tag(&s->intern, ak.ptr_type.elem);
    if (pt == IP_TAG_ARRAY_TYPE) {
      IpKey arrk = ip_key(&s->intern, ak.ptr_type.elem);
      bool a_const = (at == IP_TAG_PTR_CONST_TYPE);
      if (et == IP_TAG_SLICE_TYPE || et == IP_TAG_SLICE_CONST_TYPE) {
        IpKey ek = ip_key(&s->intern, expected);
        bool e_const = (et == IP_TAG_SLICE_CONST_TYPE);
        if (arrk.array_type.elem.v == ek.slice_type.elem.v &&
            (!a_const || e_const))
          return true;
      }
      if (et == IP_TAG_MANY_PTR_TYPE || et == IP_TAG_MANY_PTR_CONST_TYPE) {
        IpKey ek = ip_key(&s->intern, expected);
        bool e_const = (et == IP_TAG_MANY_PTR_CONST_TYPE);
        if (arrk.array_type.elem.v == ek.many_ptr_type.elem.v &&
            (!a_const || e_const))
          return true;
      }
    }
  }

  // === nil → ?T / ^T / [^]T / []T ===
  if (actual.v == IP_NIL_TYPE.v) {
    if (et == IP_TAG_OPTIONAL_TYPE || et == IP_TAG_PTR_TYPE ||
        et == IP_TAG_PTR_CONST_TYPE || et == IP_TAG_MANY_PTR_TYPE ||
        et == IP_TAG_MANY_PTR_CONST_TYPE || et == IP_TAG_SLICE_TYPE ||
        et == IP_TAG_SLICE_CONST_TYPE)
      return true;
  }

  // === Optional lift: T → ?T (speculative inner coerce) ===
  if (et == IP_TAG_OPTIONAL_TYPE) {
    IpKey ek = ip_key(&s->intern, expected);
    if (can_coerce(s, actual, ek.optional_type.elem))
      return true;
  }

  // === Comptime numeric coercions ===
  if (actual.v == IP_COMPTIME_INT_TYPE.v &&
      (expected.v == IP_U8_TYPE.v || expected.v == IP_U16_TYPE.v ||
       expected.v == IP_U32_TYPE.v || expected.v == IP_U64_TYPE.v ||
       expected.v == IP_I8_TYPE.v || expected.v == IP_I16_TYPE.v ||
       expected.v == IP_I32_TYPE.v || expected.v == IP_I64_TYPE.v ||
       expected.v == IP_USIZE_TYPE.v || expected.v == IP_ISIZE_TYPE.v ||
       expected.v == IP_F32_TYPE.v || expected.v == IP_F64_TYPE.v ||
       expected.v == IP_COMPTIME_FLOAT_TYPE.v))
    return true;
  if (actual.v == IP_COMPTIME_FLOAT_TYPE.v &&
      (expected.v == IP_F32_TYPE.v || expected.v == IP_F64_TYPE.v))
    return true;

  return false;
}

// === Helpers =================================================================

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

// Statement-kinds whose value is conventionally discarded (binding /
// control-flow / declaration). Used to suppress "unused value" warnings
// for non-tail statements that are themselves the discard construct.
static bool kind_is_discard_construct(SyntaxKind k) {
  return k == SK_CONST_DECL || k == SK_VAR_DECL ||
         k == SK_RETURN_STMT || k == SK_BREAK_STMT ||
         k == SK_CONTINUE_STMT || k == SK_DEFER_STMT ||
         k == SK_LOOP_EXPR || k == SK_BLOCK_STMT ||
         k == SK_IF_EXPR || k == SK_MATCH_EXPR;
}

// === Bidirectional check =====================================================

bool sema_check_expr(const SemaCtx *ctx, SyntaxNode *node, IpIndex expected) {
  if (!node)
    return true;
  struct db *s = ctx->s;
  SyntaxKind k = syntax_node_kind(node);

  // Bidirectional shapes: propagate `expected` into structural-control
  // forms so diags point at the offending leaf, not at the whole block /
  // if expression.
  if (expected.v != IP_NONE.v) {

    // === .Variant — needs the expected enum type to resolve ===
    if (k == SK_ENUM_REF_EXPR) {
      if (ip_tag(&s->intern, expected) == IP_TAG_ENUM_TYPE) {
        EnumRefExpr er;
        if (EnumRefExpr_cast(node, &er)) {
          SyntaxToken *vtok = EnumRefExpr_variant(&er);
          StrId vname = intern_tok(s, vtok);
          if (vtok) syntax_token_release(vtok);
          IpKey ek = ip_key(&s->intern, expected);
          for (size_t i = 0; i < ek.enum_type.n_variants; i++) {
            if (ek.enum_type.variant_names[i].idx == vname.idx)
              return true;
          }
          db_emit(s, DIAG_ERROR, span_of(ctx, node),
                  "no such variant in %T", expected);
          return false;
        }
      }
      // Expected isn't an enum (or cast failed) — fall through to
      // synth-then-compare, which will emit "expected T".
    }

    // === .{...} — anonymous product literal, needs expected struct ===
    if (k == SK_PRODUCT_EXPR) {
      ProductExpr pe;
      if (ProductExpr_cast(node, &pe)) {
        SyntaxNode *pty = ProductExpr_type(&pe);
        bool anon = (pty == NULL);
        if (pty) syntax_node_release(pty);
        if (anon && ip_tag(&s->intern, expected) == IP_TAG_STRUCT_TYPE) {
          SyntaxNode *init_list = ProductExpr_init(&pe);
          IpKey ek = ip_key(&s->intern, expected);
          bool ok = true;
          if (init_list) {
            uint32_t total = syntax_node_num_children(init_list);
            for (uint32_t i = 0; i < total; i++) {
              SyntaxElement el = syntax_node_child_or_token(init_list, i);
              if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
                if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
                  syntax_token_release(el.token);
                continue;
              }
              InitField ifld;
              if (!InitField_cast(el.node, &ifld)) {
                syntax_node_release(el.node);
                continue;
              }
              SyntaxToken *iname_tok = InitField_name(&ifld);
              StrId fname = intern_tok(s, iname_tok);
              if (iname_tok) syntax_token_release(iname_tok);
              SyntaxNode *fval = InitField_value(&ifld);
              if (fname.idx == 0) {
                // Positional initializer — deferred semantic.
                if (fval) syntax_node_release(fval);
                syntax_node_release(el.node);
                continue;
              }
              IpIndex ftype = IP_NONE;
              for (size_t j = 0; j < ek.struct_type.n_fields; j++) {
                if (ek.struct_type.field_names[j].idx == fname.idx) {
                  ftype = ek.struct_type.field_types[j];
                  break;
                }
              }
              if (ftype.v == IP_NONE.v) {
                db_emit(s, DIAG_ERROR, span_of(ctx, el.node),
                        "no such field in %T", expected);
                ok = false;
              } else if (fval) {
                if (!sema_check_expr(ctx, fval, ftype))
                  ok = false;
              }
              if (fval) syntax_node_release(fval);
              syntax_node_release(el.node);
            }
            syntax_node_release(init_list);
          }
          return ok;
        }
      }
      // Named `T{...}` or expected isn't a struct — fall through.
    }

    if (k == SK_BLOCK_STMT) {
      BlockStmt bs;
      if (BlockStmt_cast(node, &bs)) {
        SyntaxNode *stmts = BlockStmt_stmts(&bs);
        if (!stmts) {
          if (!can_coerce(s, IP_VOID_TYPE, expected)) {
            db_emit(s, DIAG_ERROR, span_of(ctx, node),
                    "empty block returns void; expected %T", expected);
            return false;
          }
          return true;
        }
        // First pass: count node children to locate the tail.
        uint32_t total = syntax_node_num_children(stmts);
        uint32_t node_count = 0;
        for (uint32_t i = 0; i < total; i++) {
          SyntaxElement el = syntax_node_child_or_token(stmts, i);
          if (el.kind == SYNTAX_ELEM_NODE && el.node) {
            node_count++;
            syntax_node_release(el.node);
          } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
            syntax_token_release(el.token);
          }
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
        // Second pass: synth non-tail (warn on unused values); check tail.
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
          bool is_tail = (seen == node_count - 1);
          if (is_tail) {
            if (!sema_check_expr(ctx, stmt, expected))
              ok = false;
          } else {
            IpIndex t = sema_type_of_expr(ctx, stmt);
            if (t.v != IP_NONE.v && t.v != IP_VOID_TYPE.v &&
                t.v != IP_NORETURN_TYPE.v) {
              SyntaxKind sk = syntax_node_kind(stmt);
              if (!kind_is_discard_construct(sk))
                db_emit(s, DIAG_WARNING, span_of(ctx, stmt),
                        "unused value of type %T", t);
            }
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
        if (cond) {
          if (!sema_check_expr(ctx, cond, IP_BOOL_TYPE)) ok = false;
          syntax_node_release(cond);
        }
        if (then_b) {
          if (!sema_check_expr(ctx, then_b, expected)) ok = false;
          syntax_node_release(then_b);
        }
        if (else_b) {
          if (!sema_check_expr(ctx, else_b, expected)) ok = false;
          syntax_node_release(else_b);
        }
        return ok;
      }
    }

    // === Arith / bitwise binops — propagate expected to operands ===
    //
    // For `a + b` in an i32 context, both operands must coerce to i32.
    // can_coerce already accepts comptime_int → i32; the leaf cases below
    // (LITERAL/REF) re-stamp the per-node cache with the contextual concrete
    // type so hover on a comptime literal or a ref to a comptime def
    // reports the expected type, not comptime_int.
    //
    // Comparison ops (==/!=/</.../>) and logical ops (&&/||) are NOT
    // propagated — their result is bool and operands unify independently.
    // Shifts (<</>>) are also excluded: the RHS has its own type rules
    // (unsigned, capped by log2(lhs_bits)).
    if (k == SK_BIN_EXPR) {
      BinExpr be;
      if (BinExpr_cast(node, &be)) {
        SyntaxKind opk = BinExpr_op_kind(&be);
        bool propagate =
            (opk == SK_PLUS || opk == SK_MINUS || opk == SK_STAR ||
             opk == SK_SLASH || opk == SK_PERCENT || opk == SK_STAR_STAR ||
             opk == SK_AMP || opk == SK_PIPE || opk == SK_CARET);
        if (propagate) {
          SyntaxNode *lhs = BinExpr_lhs(&be);
          SyntaxNode *rhs = BinExpr_rhs(&be);
          bool ok = true;
          if (lhs) {
            if (!sema_check_expr(ctx, lhs, expected)) ok = false;
            syntax_node_release(lhs);
          }
          if (rhs) {
            if (!sema_check_expr(ctx, rhs, expected)) ok = false;
            syntax_node_release(rhs);
          }
          sema_node_type_builder_push(ctx, node, expected);
          return ok;
        }
      }
      // Comparison / logical / shift — fall through to synth-then-compare.
    }

    // === Comptime-numeric leaves — stamp the contextual concrete type ===
    //
    // A literal `42` has natural type comptime_int. In a concrete-numeric
    // context (i32, u8, f32, ...) we synth via sema_type_of_expr (which
    // pushes comptime_int into the per-node cache as a side effect), then
    // OVERWRITE the cache slot with the expected type. sema_node_type_builder_push
    // unconditionally overwrites, so the second push wins.
    //
    // SK_REF_EXPR + SK_PATH_EXPR cover identifier-reference leaves
    // (single and multi-segment); SK_LITERAL_EXPR is the literal token
    // wrapper. Multi-context coercion works naturally — each use site
    // is a different SyntaxNode.
    if (k == SK_LITERAL_EXPR || k == SK_REF_EXPR || k == SK_PATH_EXPR) {
      IpIndex actual = sema_type_of_expr(ctx, node);
      bool actual_is_comptime = (actual.v == IP_COMPTIME_INT_TYPE.v ||
                                 actual.v == IP_COMPTIME_FLOAT_TYPE.v);
      bool expected_is_comptime = (expected.v == IP_COMPTIME_INT_TYPE.v ||
                                   expected.v == IP_COMPTIME_FLOAT_TYPE.v);
      if (actual_is_comptime && !expected_is_comptime &&
          can_coerce(s, actual, expected)) {
        sema_node_type_builder_push(ctx, node, expected);
        return true;
      }
      if (can_coerce(s, actual, expected))
        return true;
      db_emit(s, DIAG_ERROR, span_of(ctx, node), "expected %T", expected);
      return false;
    }
  }

  // Synth-then-compare for all other shapes.
  IpIndex actual = sema_type_of_expr(ctx, node);

  // expected==IP_NONE means "no expectation given — just type, don't
  // check." Caller uses this when the result type is recorded but no
  // coercion is required.
  if (expected.v == IP_NONE.v)
    return actual.v != IP_NONE.v;

  if (can_coerce(s, actual, expected))
    return true;

  db_emit(s, DIAG_ERROR, span_of(ctx, node), "expected %T", expected);
  return false;
}
