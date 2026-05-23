#include "../db/db.h"
#include "../db/diag/diag.h"
#include "../db/intern_pool/intern_pool.h"
#include "../parser/ast.h"
#include "sema.h"

// Bidirectional type checker — ported from
// sema_legacy/typechecker/expr_check.c::check_expr. v1 scope:
//
//   - Coercion: equal types pass; comptime_int → any concrete int/float;
//     comptime_float → f32/f64. Full Zig-variance (^T → ^const T,
//     []T → []const T, optional wrapping, error-union wrapping) is the
//     chunk-when-we-port-coerce.c follow-up.
//   - Bidirectional flow:
//       AST_STMT_BLOCK   — propagate `expected` to the LAST statement;
//                          earlier statements synth (for deps + diags).
//       AST_STMT_IF      — propagate to BOTH branches independently so
//                          a wrong branch is pinpointed in the diag
//                          rather than reported as "branches don't match".
//   - Everything else: synth-then-compare.
//
// Diag emission lands on the current query frame's slot (per the
// QuerySlot diag pipeline). Caller must be inside a query body.

// Full Zig-style coercion — ported from
// sema_legacy/typechecker/coerce.c. The semantics:
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

  // noreturn → anything (bottom).
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
      // Const flows from actual → expected: mut→const ok, const→mut not.
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
  // Recognized when actual is a single-ptr to an array. Useful so
  // address-of-array (`&arr` → `^[N]T`) feeds slice / many-ptr params.
  if (at == IP_TAG_PTR_TYPE || at == IP_TAG_PTR_CONST_TYPE) {
    IpKey ak = ip_key(&s->intern, actual);
    IpTag pt = ip_tag(&s->intern, ak.ptr_type.elem);
    if (pt == IP_TAG_ARRAY_TYPE) {
      IpKey arrk = ip_key(&s->intern, ak.ptr_type.elem);
      bool a_const = (at == IP_TAG_PTR_CONST_TYPE);
      // ^[N]T → []T / []const T
      if (et == IP_TAG_SLICE_TYPE || et == IP_TAG_SLICE_CONST_TYPE) {
        IpKey ek = ip_key(&s->intern, expected);
        bool e_const = (et == IP_TAG_SLICE_CONST_TYPE);
        if (arrk.array_type.elem.v == ek.slice_type.elem.v &&
            (!a_const || e_const))
          return true;
      }
      // ^[N]T → [^]T / [^]const T
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
  // comptime_int → any concrete int / comptime_float / concrete float
  if (actual.v == IP_COMPTIME_INT_TYPE.v &&
      (expected.v == IP_U8_TYPE.v || expected.v == IP_U16_TYPE.v ||
       expected.v == IP_U32_TYPE.v || expected.v == IP_U64_TYPE.v ||
       expected.v == IP_I8_TYPE.v || expected.v == IP_I16_TYPE.v ||
       expected.v == IP_I32_TYPE.v || expected.v == IP_I64_TYPE.v ||
       expected.v == IP_USIZE_TYPE.v || expected.v == IP_ISIZE_TYPE.v ||
       expected.v == IP_F32_TYPE.v || expected.v == IP_F64_TYPE.v ||
       expected.v == IP_COMPTIME_FLOAT_TYPE.v))
    return true;
  // comptime_float → f32/f64
  if (actual.v == IP_COMPTIME_FLOAT_TYPE.v &&
      (expected.v == IP_F32_TYPE.v || expected.v == IP_F64_TYPE.v))
    return true;

  return false;
}

bool sema_check_expr(struct db *s, ASTStore *ast, AstNodeId node,
                     IpIndex expected, NamespaceId nsid, DefId enclosing_fn,
                     FileId file_local) {
  if (node.idx == AST_NODE_ID_NONE.idx)
    return true;

  AstNodeKind k = ((AstNodeKind *)ast->kinds.data)[node.idx];
  AstNodeData d = ((AstNodeData *)ast->data.data)[node.idx];

  // Bidirectional shapes: propagate `expected` into structural-control
  // statements so diags point at the offending leaf, not at the block
  // or if expression as a whole.
  if (expected.v != IP_NONE.v) {

    // === .Variant — needs the expected enum type to resolve ===
    //
    // Ported from sema_legacy/typechecker/expr_check.c (the EnumRef
    // case in check_expr). `.Red` has no type without context; with
    // an expected enum type we look up the variant by name. Synth
    // (no expected) still returns IP_NONE — there's no way to type
    // `.Variant` without context.
    if (k == AST_EXPR_ENUM_REF) {
      if (ip_tag(&s->intern, expected) == IP_TAG_ENUM_TYPE) {
        IpKey ek = ip_key(&s->intern, expected);
        StrId vname = d.string_id;
        for (size_t i = 0; i < ek.enum_type.n_variants; i++) {
          if (ek.enum_type.variant_names[i].idx == vname.idx)
            return true;
        }
        // Variant not in expected enum.
        TinySpan span = db_get_node_span(s, file_local, node);
        if (span != TINYSPAN_NONE)
          db_emit_error_t(s, span, "no such variant in {0}", expected);
        return false;
      }
      // Expected isn't an enum — fall through to synth-then-compare,
      // which will emit the right "expected T" diag.
    }

    // === .{...} — anonymous product literal, needs expected struct ===
    //
    // Ported from sema_legacy::type_of_product. Each .field = value
    // is matched against the expected struct's fields by name, then
    // recursively check_expr'd against the field's declared type.
    // Missing fields are not yet flagged (Zig allows defaults via
    // const_eval — chunk 6 follow-up); extra fields are flagged.
    //
    // Named products `T{...}` are skipped here — they have an explicit
    // type expr and synth-type via that. Anonymous `.{...}` is the
    // form that needs bidirectional dispatch.
    if (k == AST_EXPR_PRODUCT) {
      const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
      AstNodeId type_expr = {.idx = ex[0]};
      if (type_expr.idx == AST_NODE_ID_NONE.idx &&
          ip_tag(&s->intern, expected) == IP_TAG_STRUCT_TYPE) {
        IpKey ek = ip_key(&s->intern, expected);
        uint32_t fcount = ex[1];
        bool ok = true;
        for (uint32_t i = 0; i < fcount; i++) {
          AstNodeId init_id = {.idx = ex[2 + i]};
          if (init_id.idx == AST_NODE_ID_NONE.idx)
            continue;
          AstNodeKind ik = ((AstNodeKind *)ast->kinds.data)[init_id.idx];
          if (ik != AST_INIT_FIELD)
            continue;
          // AST_INIT_FIELD extras: [name_strid, value_id].
          // (Positional initializers — name_strid = 0 — are deferred
          //  until we have a clear semantic for them.)
          AstNodeData id_d = ((AstNodeData *)ast->data.data)[init_id.idx];
          const uint32_t *iex =
              &((uint32_t *)ast->extra.data)[id_d.extra_idx.idx];
          StrId fname = {.idx = iex[0]};
          AstNodeId fval = {.idx = iex[1]};
          if (fname.idx == 0)
            continue; // positional — chunk-6+ work

          // Find field in expected struct.
          IpIndex ftype = IP_NONE;
          for (size_t j = 0; j < ek.struct_type.n_fields; j++) {
            if (ek.struct_type.field_names[j].idx == fname.idx) {
              ftype = ek.struct_type.field_types[j];
              break;
            }
          }
          if (ftype.v == IP_NONE.v) {
            // Extra field: name doesn't exist in the expected struct.
            TinySpan span = db_get_node_span(s, file_local, init_id);
            if (span != TINYSPAN_NONE)
              db_emit_error_t(s, span, "no such field in {0}", expected);
            ok = false;
            continue;
          }
          // Recursive check on the field value.
          if (!sema_check_expr(s, ast, fval, ftype, nsid, enclosing_fn,
                               file_local))
            ok = false;
        }
        return ok;
      }
      // Named T{...} or expected isn't a struct — fall through.
    }

    if (k == AST_STMT_BLOCK) {
      const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
      uint32_t count = ex[0];
      if (count == 0) {
        // Empty block ≡ void; check against expected.
        if (!can_coerce(s, IP_VOID_TYPE, expected)) {
          TinySpan span = db_get_node_span(s, file_local, node);
          if (span != TINYSPAN_NONE)
            db_emit_error_t(s, span, "empty block returns void; expected {0}",
                            expected);
          return false;
        }
        return true;
      }
      // All but the last stmt: synth (records deps + intrinsic diags from
      // type_of_expr). Emit a warning when a non-tail statement produces
      // a non-void, non-noreturn value that the user is discarding.
      // Rust-style `#[must_use]` policy — explicit discard via `_ := ...`
      // (a let binding) bypasses the warning by going through the
      // binding-RHS path, not this loop.
      for (uint32_t i = 0; i + 1 < count; i++) {
        AstNodeId stmt = {.idx = ex[1 + i]};
        IpIndex t =
            sema_type_of_expr(s, ast, stmt, nsid, enclosing_fn, file_local);
        if (t.v == IP_NONE.v || t.v == IP_VOID_TYPE.v ||
            t.v == IP_NORETURN_TYPE.v)
          continue;
        // Statement-form constructs that legally produce values which
        // are conventionally discarded — assignments, increments,
        // bindings — go through dedicated AST kinds; let / := lands as
        // AST_STMT_LET, not as a top-level expression. The remaining
        // expression kinds at non-tail position are genuine
        // value-producing exprs whose result is being thrown away.
        AstNodeKind sk = ((AstNodeKind *)ast->kinds.data)[stmt.idx];
        // Suppress for kinds that ARE the discard / control / binding
        // constructs themselves (no value to discard).
        if (sk == AST_DECL_CONST || sk == AST_DECL_VAR ||
            sk == AST_DECL_DESTRUCTURE || sk == AST_STMT_RETURN ||
            sk == AST_STMT_BREAK || sk == AST_STMT_CONTINUE ||
            sk == AST_STMT_DEFER || sk == AST_STMT_LOOP ||
            sk == AST_STMT_BLOCK || sk == AST_STMT_IF ||
            sk == AST_STMT_SWITCH)
          continue;
        TinySpan span = db_get_node_span(s, file_local, stmt);
        if (span != TINYSPAN_NONE)
          db_emit_warning_t(s, span, "unused value of type {0}", t);
      }
      // Tail: check with the outer expectation.
      AstNodeId tail = {.idx = ex[count]};
      return sema_check_expr(s, ast, tail, expected, nsid, enclosing_fn,
                             file_local);
    }

    if (k == AST_STMT_IF) {
      // Extras: [cond, then, else]. Condition is always bool — check
      // independently against bool. Branches check against the outer
      // expectation independently so a wrong branch is pinpointed.
      const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
      AstNodeId cond = {.idx = ex[0]};
      AstNodeId then_b = {.idx = ex[1]};
      AstNodeId else_b = {.idx = ex[2]};
      bool ok = true;
      if (cond.idx != AST_NODE_ID_NONE.idx)
        ok &= sema_check_expr(s, ast, cond, IP_BOOL_TYPE, nsid, enclosing_fn,
                              file_local);
      if (then_b.idx != AST_NODE_ID_NONE.idx)
        ok &= sema_check_expr(s, ast, then_b, expected, nsid, enclosing_fn,
                              file_local);
      if (else_b.idx != AST_NODE_ID_NONE.idx)
        ok &= sema_check_expr(s, ast, else_b, expected, nsid, enclosing_fn,
                              file_local);
      return ok;
    }
  }

  // Synth-then-compare for all other shapes.
  IpIndex actual =
      sema_type_of_expr(s, ast, node, nsid, enclosing_fn, file_local);

  // expected==IP_NONE means "no expectation given — just type, don't
  // check." Caller uses this when the result type is recorded but no
  // coercion is required (rare; mostly defensive).
  if (expected.v == IP_NONE.v)
    return actual.v != IP_NONE.v;

  if (can_coerce(s, actual, expected))
    return true;

  // Mismatch — emit diag pointing at this node's span.
  TinySpan span = db_get_node_span(s, file_local, node);
  if (span != TINYSPAN_NONE)
    db_emit_error_t(s, span, "expected {0}", expected);
  return false;
}
