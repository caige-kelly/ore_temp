#include "const_eval.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../../common/stringpool.h"
#include "../../diag/diag.h"
#include "../../parser/ast.h"
#include "../ids/ids.h"         // def_origin
#include "../modules/modules.h" // module_for_span / query_module_ast
#include "../query/ast_dep.h"   // record_ast_dep_for_span
#include "../query/query_engine.h"
#include "../resolve/resolve.h" // query_resolve_ref
#include "../scope/scope.h"
#include "../sema.h"
#include "../type/checker.h" // resolve_type_expr
#include "../type/layout.h"  // query_layout_of_type
#include "../type/type.h"    // struct Type, TY_ERROR
#include "bin_ops/bin_ops.h"
#include "literals/literals.h"

// === Entry lookup / lazy creation ===
//
// One ConstEvalEntry per Expr we ever try to const-eval, keyed by
// NodeId so the table survives re-parses cleanly (NodeIds are unique
// per parse; raw Expr* pointers aren't). The entry holds the slot so
// SEMA_QUERY_GUARD can do cycle detection and (eventually) early
// cutoff via fingerprints.
static struct ConstEvalEntry *const_eval_entry_for(struct Sema *s,
                                                   struct Expr *expr) {
  if (s->const_eval_entries.entries == NULL)
    hashmap_init_in(&s->const_eval_entries, &s->arena);

  uint64_t key = (uint64_t)expr->id.id;
  if (hashmap_contains(&s->const_eval_entries, key))
    return (struct ConstEvalEntry *)hashmap_get(&s->const_eval_entries, key);

  struct ConstEvalEntry *e = arena_alloc(&s->arena, sizeof(*e));
  *e = (struct ConstEvalEntry){0};
  sema_query_slot_init(&e->query, QUERY_CONST_EVAL);
  hashmap_put_or_die(&s->const_eval_entries, key, e, "const_eval_entries");
  return e;
}

// === Compute helpers ===

static struct ConstValue eval_lit(struct Sema *s, struct Expr *expr) {
  struct ConstValue r = {.kind = CONST_NONE};
  // lit_True / lit_False have no string payload; map directly.
  if (expr->lit.kind == lit_True) {
    r.kind = CONST_BOOL;
    r.bool_val = true;
    return r;
  }
  if (expr->lit.kind == lit_False) {
    r.kind = CONST_BOOL;
    r.bool_val = false;
    return r;
  }
  const char *text = pool_get(&s->pool, expr->lit.string_id, 0);
  if (!text)
    return r;
  if (expr->lit.kind == lit_Int) {
    int64_t v;
    if (parse_int_literal(text, &v)) {
      r.kind = CONST_INT;
      r.int_val = v;
    }
  } else if (expr->lit.kind == lit_Float) {
    double v;
    if (parse_float_literal(text, &v)) {
      r.kind = CONST_FLOAT;
      r.float_val = v;
    }
  }
  return r;
}

static struct ConstValue eval_bin(struct Sema *s, struct Expr *expr) {
  struct ConstValue l = query_const_eval(s, expr->bin.Left);
  struct ConstValue r = query_const_eval(s, expr->bin.Right);
  if (l.kind == CONST_NONE || r.kind == CONST_NONE)
    return (struct ConstValue){.kind = CONST_NONE};

  switch (expr->bin.op) {
  case Plus:
    return bin_add(s, expr, l, r);
  case Minus:
    return bin_sub(s, expr, l, r);
  case Star:
    return bin_mul(s, expr, l, r);
  case ForwardSlash:
    return bin_div(s, expr, l, r);
  case Percent:
    return bin_mod(s, expr, l, r);
  default:
    return (struct ConstValue){.kind = CONST_NONE};
  }
}

static struct ConstValue eval_unary(struct Sema *s, struct Expr *expr) {
  // Only prefix arithmetic-shaped operators are constant-evaluable.
  // Postfix `^` (deref), `?` (de-nil), `++`, `--` and address-of `&`
  // are runtime-shaped; they don't apply to comptime-known scalars.
  if (expr->unary.postfix)
    return (struct ConstValue){.kind = CONST_NONE};

  struct ConstValue v = query_const_eval(s, expr->unary.operand);
  if (v.kind == CONST_NONE)
    return v;

  switch (expr->unary.op) {
  case unary_Neg:
    if (v.kind == CONST_INT) {
      // Detect INT64_MIN negation (UB on signed overflow).
      if (v.int_val == INT64_MIN) {
        diag_error(&s->diags, expr->span, "int overflow during unary negation");
        return (struct ConstValue){.kind = CONST_NONE};
      }
      return (struct ConstValue){.kind = CONST_INT, .int_val = -v.int_val};
    }
    if (v.kind == CONST_FLOAT) {
      return (struct ConstValue){.kind = CONST_FLOAT,
                                 .float_val = -v.float_val};
    }
    return (struct ConstValue){.kind = CONST_NONE};
  case unary_BitNot:
    if (v.kind == CONST_INT)
      return (struct ConstValue){.kind = CONST_INT, .int_val = ~v.int_val};
    return (struct ConstValue){.kind = CONST_NONE};
  case unary_Not:
    if (v.kind == CONST_BOOL)
      return (struct ConstValue){.kind = CONST_BOOL, .bool_val = !v.bool_val};
    return (struct ConstValue){.kind = CONST_NONE};
  default:
    // Type-shaped unaries (Const, Optional, Ptr, ManyPtr) and
    // address/deref/inc/dec aren't const-evaluable in the int/float
    // lattice. Stay non-constant.
    return (struct ConstValue){.kind = CONST_NONE};
  }
}

// === query_is_comptime ===
//
// Per-Expr predicate, slot-cached. Replaces the recursive walker that
// used to live in checker.c. Every recursion goes through
// query_is_comptime so the dep graph stays honest: editing a const-bind
// referenced transitively by D's RHS will invalidate D's slot via the
// cached child fingerprint changing.

static struct IsComptimeEntry *is_comptime_entry_for(struct Sema *s,
                                                     struct Expr *expr) {
  if (s->is_comptime_entries.entries == NULL)
    hashmap_init_in(&s->is_comptime_entries, &s->arena);
  uint64_t key = (uint64_t)expr->id.id;
  if (hashmap_contains(&s->is_comptime_entries, key))
    return (struct IsComptimeEntry *)hashmap_get(&s->is_comptime_entries, key);
  struct IsComptimeEntry *e = arena_alloc(&s->arena, sizeof(*e));
  *e = (struct IsComptimeEntry){0};
  sema_query_slot_init(&e->query, QUERY_IS_COMPTIME);
  hashmap_put_or_die(&s->is_comptime_entries, key, e, "is_comptime_entries");
  return e;
}

static bool args_all_comptime_q(struct Sema *s, Vec *args) {
  if (!args)
    return true;
  for (size_t i = 0; i < args->count; i++) {
    struct Expr **slot = (struct Expr **)vec_get(args, i);
    struct Expr *arg = slot ? *slot : NULL;
    if (arg && !query_is_comptime(s, arg))
      return false;
  }
  return true;
}

static bool fields_all_comptime_q(struct Sema *s, Vec *fields) {
  if (!fields)
    return true;
  for (size_t i = 0; i < fields->count; i++) {
    struct ProductField *pf = (struct ProductField *)vec_get(fields, i);
    if (pf && pf->value && !query_is_comptime(s, pf->value))
      return false;
  }
  return true;
}

bool query_is_comptime(struct Sema *s, struct Expr *expr) {
  if (!s || !expr || expr->id.id == 0)
    return false;

  struct IsComptimeEntry *entry = is_comptime_entry_for(s, expr);

  SEMA_QUERY_GUARD(s, &entry->query, QUERY_IS_COMPTIME, entry, expr->span,
                   /*on_cached=*/entry->result,
                   /*on_cycle=*/false,
                   /*on_error=*/false);

  // Record AST dep so a re-parse forces revalidate. Recursive calls
  // record their own; the redundancy is cheap.
  record_ast_dep_for_span(s, expr->span);

  bool result = false;

  switch (expr->kind) {
  // Comptime by shape — always true.
  case expr_Lit:
  case expr_Lambda:
  case expr_Struct:
  case expr_Enum:
  case expr_ArrayType:
  case expr_SliceType:
  case expr_ManyPtrType:
  case expr_EnumRef:
    result = true;
    break;

  // Comptime by composition — recurse.
  case expr_Bin:
    result = query_is_comptime(s, expr->bin.Left) &&
             query_is_comptime(s, expr->bin.Right);
    break;

  case expr_Unary:
    if (expr->unary.op == unary_Ref || expr->unary.op == unary_Deref ||
        expr->unary.op == unary_Inc || expr->unary.op == unary_Dec)
      result = false;
    else
      result = query_is_comptime(s, expr->unary.operand);
    break;

  case expr_Builtin:
    result = args_all_comptime_q(s, expr->builtin.args);
    break;

  case expr_Product:
    result = fields_all_comptime_q(s, expr->product.Fields);
    break;

  case expr_ArrayLit: {
    if (!expr->array_lit.initializer ||
        expr->array_lit.initializer->kind != expr_Product)
      result = false;
    else
      result =
          fields_all_comptime_q(s, expr->array_lit.initializer->product.Fields);
    break;
  }

  case expr_If:
    result = query_is_comptime(s, expr->if_expr.condition) &&
             query_is_comptime(s, expr->if_expr.then_branch) &&
             (!expr->if_expr.else_branch ||
              query_is_comptime(s, expr->if_expr.else_branch));
    break;

  case expr_Switch: {
    result = query_is_comptime(s, expr->switch_expr.scrutinee);
    if (result && expr->switch_expr.arms) {
      for (size_t i = 0; i < expr->switch_expr.arms->count && result; i++) {
        struct SwitchArm *arm =
            (struct SwitchArm *)vec_get(expr->switch_expr.arms, i);
        if (arm && arm->body && !query_is_comptime(s, arm->body))
          result = false;
      }
    }
    break;
  }

  case expr_Block: {
    result = true;
    if (expr->block.stmts) {
      for (size_t i = 0; i < expr->block.stmts->count && result; i++) {
        struct Expr **slot = (struct Expr **)vec_get(expr->block.stmts, i);
        if (slot && *slot && !query_is_comptime(s, *slot))
          result = false;
      }
    }
    break;
  }

  case expr_Bind:
    result = expr->bind.value ? query_is_comptime(s, expr->bind.value) : true;
    break;

  // Comptime by reference (resolution-driven).
  case expr_Ident: {
    // B6: single slot per Ident covering both namespaces.
    DefId def = query_resolve_ref(s, expr, NS_VALUE_OR_TYPE);
    if (!def_id_is_valid(def)) {
      result = false;
      break;
    }
    struct DefInfo *di = def_info(s, def);
    if (!di) {
      result = false;
      break;
    }
    switch (di->kind) {
    case DECL_PRIMITIVE:
    case DECL_VARIANT:
    case DECL_IMPORT:
      result = true;
      break;
    case DECL_USER: {
      struct Expr *origin = def_origin(s, def);
      if (!origin || origin->kind != expr_Bind) {
        result = false;
        break;
      }
      // Const-bound (`::`) DECL_USER is comptime iff its RHS is.
      // Recurse via the query so the dep is recorded.
      if (origin->bind.kind != bind_Const) {
        result = false;
        break;
      }
      result =
          origin->bind.value ? query_is_comptime(s, origin->bind.value) : true;
      break;
    }
    case DECL_PARAM:
    case DECL_FIELD:
    case DECL_SCOPE_PARAM:
    case DECL_EFFECT_ROW:
    case DECL_LOOP_LABEL:
      result = false;
      break;
    }
    break;
  }

  case expr_Field:
    result = query_is_comptime(s, expr->field.object);
    break;

  // Always runtime.
  default:
    result = false;
    break;
  }

  entry->result = result;
  // Fingerprint over the bool (1 or 0). Composition propagates via
  // the recorded dep on each child query — the parent's slot deps
  // include child fingerprints, so a flip in any subtree shifts the
  // recorded dep_fp on revalidation.
  query_slot_set_fingerprint(&entry->query,
                             query_fingerprint_from_u64(result ? 1 : 0));
  sema_query_succeed(s, &entry->query);
  return result;
}

// === Query body ===

struct ConstValue query_const_eval(struct Sema *s, struct Expr *expr) {
  struct ConstValue none = {.kind = CONST_NONE};
  if (!s || !expr || expr->id.id == 0)
    return none;

  struct ConstEvalEntry *entry = const_eval_entry_for(s, expr);

  SEMA_QUERY_GUARD(s, &entry->query, QUERY_CONST_EVAL, entry, expr->span,
                   /*on_cached=*/entry->value,
                   /*on_cycle=*/none,
                   /*on_error=*/none);

  // AST-dep so a re-parse invalidates this slot for any expr kind.
  // The Lit/Bin/Unary cases were correct without it because their
  // operands are recursed via query_const_eval — but kinds that read
  // structural data (Builtin args, If branches, Switch arms, Block
  // tail, Ident → def_origin) need the dep at the parent's frame.
  record_ast_dep_for_span(s, expr->span);

  struct ConstValue result = none;
  switch (expr->kind) {
  case expr_Lit:
    result = eval_lit(s, expr);
    break;
  case expr_Bin:
    result = eval_bin(s, expr);
    break;
  case expr_Unary:
    result = eval_unary(s, expr);
    break;

  // Ident → const-bind chain folding. Resolve to a DefId, read the
  // bind shape via def_origin (re-parse-safe), recurse into the
  // bound value. The recursive query_const_eval call is what records
  // the dep on the child entry's slot, so editing MAX in
  // `MAX :: 1024 ; HALF :: MAX / 2` invalidates HALF's cached value.
  // Cycles (`A :: A + 1`) are caught by QUERY_RUNNING — on_cycle is
  // CONST_NONE.
  case expr_Ident: {
    // B6: NS_VALUE_OR_TYPE so this slot lookup is shared with the
    // is_comptime + expr_check sites above. Type-resolved idents fall
    // through to CONST_NONE at the bind_Const / non-foldable-value
    // checks below — same end behavior as NS_VALUE-only would give us.
    DefId def = query_resolve_ref(s, expr, NS_VALUE_OR_TYPE);
    if (!def_id_is_valid(def))
      break;
    struct DefInfo *di = def_info(s, def);
    if (!di || di->kind != DECL_USER)
      break;
    struct Expr *origin = def_origin(s, def);
    if (!origin || origin->kind != expr_Bind)
      break;
    if (origin->bind.kind != bind_Const)
      break;
    if (!origin->bind.value)
      break;
    result = query_const_eval(s, origin->bind.value);
    break;
  }

  case expr_Builtin: {
    StrId nm = expr->builtin.name_id;
    if (nm.v == s->name_sizeOf.v || nm.v == s->name_alignOf.v) {
      if (!expr->builtin.args || expr->builtin.args->count == 0)
        break;
      struct Expr **slot = (struct Expr **)vec_get(expr->builtin.args, 0);
      struct Expr *arg = slot ? *slot : NULL;
      if (!arg)
        break;
      struct Type *t = resolve_type_expr(s, arg);
      if (!t || t->kind == TY_ERROR)
        break;
      // Delegate to query_layout_of_type — handles primitives,
      // pointers, slices, arrays, optionals, structs (with C-style
      // alignment), enums (variant-value-range-fitted width), fn
      // pointers. Replaces the pre-PR-3.5 primitive-only table that
      // returned CONST_NONE for everything else (a known partial
      // documented as B7 / cleanup.md #4).
      struct Layout layout = query_layout_of_type(s, t);
      if (!layout.is_known)
        break; // cycle / unsupported → CONST_NONE
      result.kind = CONST_INT;
      result.int_val =
          (int64_t)((nm.v == s->name_sizeOf.v) ? layout.size : layout.align);
    }
    // @typeName, @TypeOf, @intCast, @returnType: deferred (need
    // ConstValue string variant / type-as-value support).
    break;
  }

  // Comptime-condition `if` collapses to its taken branch.
  case expr_If: {
    if (!expr->if_expr.condition)
      break;
    struct ConstValue cond = query_const_eval(s, expr->if_expr.condition);
    if (cond.kind != CONST_BOOL)
      break;
    struct Expr *taken =
        cond.bool_val ? expr->if_expr.then_branch : expr->if_expr.else_branch;
    if (taken)
      result = query_const_eval(s, taken);
    break;
  }

  // Switch with a comptime scrutinee: walk arms, match against
  // pattern, recurse into the matched arm's body. Pattern equality
  // is shallow today — literal patterns and bare `_` only. Enum-ref
  // patterns are deferred (would need the enum's variant value
  // looked up against scrutinee's TY_ENUM ConstValue, which is a
  // PR 2.5 ConstValue extension).
  case expr_Switch: {
    if (!expr->switch_expr.scrutinee || !expr->switch_expr.arms)
      break;
    struct ConstValue scrut = query_const_eval(s, expr->switch_expr.scrutinee);
    if (scrut.kind == CONST_NONE)
      break;

    for (size_t i = 0; i < expr->switch_expr.arms->count; i++) {
      struct SwitchArm *arm =
          (struct SwitchArm *)vec_get(expr->switch_expr.arms, i);
      if (!arm)
        continue;
      bool matched = false;
      if (arm->patterns) {
        for (size_t pi = 0; pi < arm->patterns->count && !matched; pi++) {
          struct Expr **pslot = (struct Expr **)vec_get(arm->patterns, pi);
          struct Expr *pat = pslot ? *pslot : NULL;
          if (!pat)
            continue;
          if (pat->kind == expr_Wildcard) {
            matched = true;
            break;
          }
          struct ConstValue pv = query_const_eval(s, pat);
          if (pv.kind != scrut.kind)
            continue;
          switch (scrut.kind) {
          case CONST_INT:
            matched = (pv.int_val == scrut.int_val);
            break;
          case CONST_FLOAT:
            matched = (pv.float_val == scrut.float_val);
            break;
          case CONST_BOOL:
            matched = (pv.bool_val == scrut.bool_val);
            break;
          default:
            break;
          }
        }
      }
      if (matched && arm->body) {
        result = query_const_eval(s, arm->body);
        break;
      }
    }
    break;
  }

  // Block tail. Bindings inside the block contribute via the Ident
  // path: a tail expression that references an in-block `x :: 42`
  // resolves through query_resolve_ref to the local DefId and folds
  // recursively. No explicit environment threading needed.
  case expr_Block: {
    if (!expr->block.stmts || expr->block.stmts->count == 0)
      break;
    // Tail = last non-binding statement. A trailing const-bind block
    // has no value (or we treat it as void), so we walk backwards
    // and pick the first non-Bind expression.
    struct Expr *tail = NULL;
    for (size_t i = expr->block.stmts->count; i > 0; i--) {
      struct Expr **slot = (struct Expr **)vec_get(expr->block.stmts, i - 1);
      struct Expr *stmt = slot ? *slot : NULL;
      if (!stmt)
        continue;
      if (stmt->kind == expr_Bind)
        continue;
      tail = stmt;
      break;
    }
    if (tail)
      result = query_const_eval(s, tail);
    break;
  }

  default:
    break;
  }

  entry->value = result;

  // Fingerprint the result so the invalidator can do early cutoff.
  // CONST_NONE stays at FINGERPRINT_NONE so misses aren't confused
  // with "valid zero" downstream.
  //
  // Hash (kind, active-variant-bytes) — *not* the whole struct.
  // sizeof(struct ConstValue) reads the inactive union bytes which
  // are uninitialized for any kind != the active one (B7 in
  // bug_of_bugs.md). Switch on the tag and feed only the live
  // payload so the fingerprint is a function of the value, not of
  // whatever happened to be in the surrounding stack/heap memory.
  if (result.kind != CONST_NONE) {
    Fingerprint fp = query_fingerprint_from_u64((uint64_t)result.kind);
    switch (result.kind) {
    case CONST_INT:
      fp = query_fingerprint_combine(
          fp, query_fingerprint_from_u64((uint64_t)result.int_val));
      break;
    case CONST_FLOAT: {
      // Bit-cast the double to a stable 64-bit representation. Same
      // bits → same fingerprint. memcpy avoids the strict-aliasing
      // pitfall a union or pointer-cast would have.
      uint64_t bits = 0;
      memcpy(&bits, &result.float_val, sizeof(bits));
      fp = query_fingerprint_combine(fp, query_fingerprint_from_u64(bits));
      break;
    }
    case CONST_BOOL:
      fp = query_fingerprint_combine(
          fp, query_fingerprint_from_u64(result.bool_val ? 1u : 0u));
      break;
    case CONST_NONE:
      // Unreachable — guarded above.
      break;
    }
    query_slot_set_fingerprint(&entry->query, fp);
  }

  sema_query_succeed(s, &entry->query);
  return result;
}

// === R4 Step 4: ConstValue ↔ IpIndex bridge ===
//
// One-way-on-demand: callers convert a ConstValue to its interned
// IpIndex when they need pool-managed identity (introspection
// builtins, future @typeInfo of a comptime value, etc.). Today no
// production path calls these — they're groundwork for Step 5+.
//
// Bool / void / undef map to reserved indices (cheap u32 compare,
// no hashmap touch). Int / float go through ip_get with their
// type-as-IpIndex companion. CONST_NONE returns IP_NONE.

IpIndex const_value_to_ip(struct Sema *s, struct ConstValue v) {
  if (!s) return IP_NONE;
  switch (v.kind) {
  case CONST_NONE:
    return IP_NONE;
  case CONST_BOOL:
    return v.bool_val ? IP_BOOL_TRUE : IP_BOOL_FALSE;
  case CONST_INT: {
    // Use comptime_int as the carrier type — values produced by
    // const_eval haven't been narrowed to a concrete int type yet
    // (coerce does that later). When Step 5+ surfaces int values
    // post-coercion, the caller will pass the resolved type.
    IpKey k = {.kind = IPK_INT_VALUE};
    k.int_value.type = IP_COMPTIME_INT_TYPE;
    k.int_value.value = v.int_val;
    return ip_get(&s->intern_pool, k);
  }
  case CONST_FLOAT: {
    IpKey k = {.kind = IPK_FLOAT_VALUE};
    k.float_value.type = IP_COMPTIME_FLOAT_TYPE;
    k.float_value.value = v.float_val;
    return ip_get(&s->intern_pool, k);
  }
  }
  return IP_NONE;  // unreachable; switch is exhaustive
}

struct ConstValue const_value_from_ip(struct Sema *s, IpIndex idx) {
  struct ConstValue none = {.kind = CONST_NONE};
  if (!s || !ip_index_is_valid(idx)) return none;

  // Reserved values short-circuit without an ip_key call.
  if (idx.v == IP_BOOL_TRUE.v)
    return (struct ConstValue){.kind = CONST_BOOL, .bool_val = true};
  if (idx.v == IP_BOOL_FALSE.v)
    return (struct ConstValue){.kind = CONST_BOOL, .bool_val = false};

  IpKey k = ip_key(&s->intern_pool, idx);
  switch (k.kind) {
  case IPK_INT_VALUE:
    return (struct ConstValue){.kind = CONST_INT, .int_val = k.int_value.value};
  case IPK_FLOAT_VALUE:
    return (struct ConstValue){.kind = CONST_FLOAT,
                               .float_val = k.float_value.value};
  default:
    // Either a type IpIndex (caller error) or a non-bool reserved
    // value we don't model in ConstValue. Return CONST_NONE rather
    // than asserting; consumers can probe via ip_tag if they need
    // discrimination.
    return none;
  }
}
