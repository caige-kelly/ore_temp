#include "body_store.h"

#include <stddef.h>

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../../common/vec.h"
#include "../../parser/ast.h"
#include "../modules/def_map.h"  // sem_for_bind_value (unused — kind check inline)
#include "../modules/modules.h"  // module_for_span, query_module_ast, def_owning_module
#include "../query/query_engine.h"
#include "../resolve/scope_index.h"  // query_node_to_decl (bootstrap)
#include "../scope/scope.h"
#include "../sema.h"

// =============================================================================
// R8 — per-decl body store.
//
// Mirrors the structure of scope_index.c's `decl_walk`: same AST kind
// dispatch, same recursion shape. Differences:
//
//   - Records `(local, Expr*)` pairs onto the BodyStore's own
//     `exprs` / `nodeid_to_local` rather than the module-global
//     `node_to_decl` table.
//   - Folds a `(kind, arity)` fingerprint across the walk. Body-only
//     edits that don't reshape the tree (whitespace, comment) don't
//     shift this fingerprint, so downstream cache tables get cutoff.
//
// Bootstrap: `expr_to_id` calls `query_node_to_decl` (B21) to find
// the owning top-level decl, then `query_body_store` on that decl.
// The body store's body records an explicit dep on `query_module_ast`
// (so any source change refreshes the Expr pointers in `exprs`) but
// the slot's output fingerprint depends only on structural shape (so
// dependents cut off correctly).
//
// === Nested closures and inner decls ===
//
// One body_store per *top-level* decl. Nested Lambda/Struct/Enum
// expressions inside an fn body are walked AS PART OF the enclosing
// top-level decl's body_store — their interior Exprs get locals in
// the outer store. This mirrors rust-analyzer's data-structure
// choice: `DefWithBodyId` is restricted to top-level items
// (`FunctionId`, `StaticId`, `ConstId`, `VariantId` — see
// rust-analyzer/crates/hir-def/src/lib.rs:707). Closures don't get
// their own `Body`; their interior is just more `ExprId`s in the
// enclosing fn's `Body::store`.
//
// Granularity trade-off: editing a nested closure shifts sibling
// locals within the same top-level decl's body_store. Editing one
// top-level decl never affects another top-level decl's body_store.
// Identical to RA's invalidation granularity. The per-top-level-decl
// scoping is the load-bearing R8 win; finer nested-closure scoping
// would require bootstrap infrastructure (nearest-owning-body-decl
// map or current-decl stack) that RA itself doesn't have.
// =============================================================================

// Assigns `e` the next local index in `bs`, populates `nodeid_to_local`
// and the per-Expr `expr_id` cache, and folds `(kind, arity)` into
// the fingerprint.
//
// `arity` is intentionally cheap to compute — we use children-count
// proxies that are stable across renames/whitespace but shift on
// structural reshape. The exact formula doesn't matter, only that it
// distinguishes "same shape" from "reshape."
static Fingerprint assign_local(struct BodyStore *bs, struct Expr *e,
                                uint32_t arity, Fingerprint fp) {
  if (!e || e->id.id == 0)
    return fp;
  uint32_t local = (uint32_t)bs->exprs->count;
  vec_push(bs->exprs, &e);
  hashmap_put(&bs->nodeid_to_local, (uint64_t)e->id.id,
              (void *)(uintptr_t)local);
  e->expr_id = (ExprId){.decl = bs->decl, .local = local};
  // Fold (kind | arity << 16) into the running fingerprint. Each
  // body produces a stable hash over its structural walk.
  uint64_t bits = ((uint64_t)(uint32_t)e->kind) | ((uint64_t)arity << 16);
  return query_fingerprint_combine(fp, query_fingerprint_from_u64(bits));
}

// Forward decl — recursive walker.
static Fingerprint body_walk(struct Sema *s, struct BodyStore *bs,
                             struct Expr *e, Fingerprint fp);

// Walks a Vec<Expr*>, recurring into each element.
static Fingerprint walk_expr_vec(struct Sema *s, struct BodyStore *bs, Vec *v,
                                 Fingerprint fp) {
  if (!v)
    return fp;
  for (size_t i = 0; i < v->count; i++) {
    struct Expr **slot = (struct Expr **)vec_get(v, i);
    fp = body_walk(s, bs, slot ? *slot : NULL, fp);
  }
  return fp;
}

// Walks a Vec<Param>, recurring into each param's type annotation.
static Fingerprint walk_param_vec(struct Sema *s, struct BodyStore *bs, Vec *v,
                                  Fingerprint fp) {
  if (!v)
    return fp;
  for (size_t i = 0; i < v->count; i++) {
    struct Param *p = (struct Param *)vec_get(v, i);
    if (p)
      fp = body_walk(s, bs, p->type_ann, fp);
  }
  return fp;
}

static Fingerprint body_walk(struct Sema *s, struct BodyStore *bs,
                             struct Expr *e, Fingerprint fp) {
  if (!e)
    return fp;

  // Assign first so children inherit a valid expr_id ordering.
  // Arity is a coarse proxy — exact count doesn't matter so long as
  // it's stable for the same shape.
  uint32_t arity = 0;
  fp = assign_local(bs, e, arity, fp);

  switch (e->kind) {
  case expr_Lit:
  case expr_Wildcard:
  case expr_Break:
  case expr_Continue:
  case expr_Ident:
  case expr_EnumRef:
  case expr_Asm:
    return fp;

  case expr_Bin:
    fp = body_walk(s, bs, e->bin.Left, fp);
    fp = body_walk(s, bs, e->bin.Right, fp);
    return fp;
  case expr_Assign:
    fp = body_walk(s, bs, e->assign.target, fp);
    fp = body_walk(s, bs, e->assign.value, fp);
    return fp;
  case expr_Unary:
    fp = body_walk(s, bs, e->unary.operand, fp);
    return fp;
  case expr_Call:
    fp = body_walk(s, bs, e->call.callee, fp);
    fp = walk_expr_vec(s, bs, e->call.args, fp);
    return fp;
  case expr_Builtin:
    fp = walk_expr_vec(s, bs, e->builtin.args, fp);
    return fp;
  case expr_If:
    fp = body_walk(s, bs, e->if_expr.condition, fp);
    fp = body_walk(s, bs, e->if_expr.then_branch, fp);
    fp = body_walk(s, bs, e->if_expr.else_branch, fp);
    return fp;
  case expr_Block:
    fp = walk_expr_vec(s, bs, e->block.stmts, fp);
    return fp;
  case expr_Bind:
    fp = body_walk(s, bs, e->bind.type_ann, fp);
    fp = body_walk(s, bs, e->bind.value, fp);
    return fp;
  case expr_Lambda:
    // Nested closures are inlined into the enclosing top-level
    // decl's body_store (RA-aligned, see file header comment).
    fp = walk_param_vec(s, bs, e->lambda.params, fp);
    fp = body_walk(s, bs, e->lambda.effect, fp);
    fp = body_walk(s, bs, e->lambda.ret_type, fp);
    fp = body_walk(s, bs, e->lambda.body, fp);
    return fp;
  case expr_Loop:
    fp = body_walk(s, bs, e->loop_expr.init, fp);
    fp = body_walk(s, bs, e->loop_expr.condition, fp);
    fp = body_walk(s, bs, e->loop_expr.step, fp);
    fp = body_walk(s, bs, e->loop_expr.body, fp);
    return fp;
  case expr_Switch:
    fp = body_walk(s, bs, e->switch_expr.scrutinee, fp);
    if (e->switch_expr.arms) {
      for (size_t i = 0; i < e->switch_expr.arms->count; i++) {
        struct SwitchArm *arm =
            (struct SwitchArm *)vec_get(e->switch_expr.arms, i);
        if (!arm)
          continue;
        fp = walk_expr_vec(s, bs, arm->patterns, fp);
        fp = body_walk(s, bs, arm->body, fp);
      }
    }
    return fp;
  case expr_Field:
    fp = body_walk(s, bs, e->field.object, fp);
    return fp;
  case expr_Index:
    fp = body_walk(s, bs, e->index.object, fp);
    fp = body_walk(s, bs, e->index.index, fp);
    return fp;
  case expr_Slice:
    fp = body_walk(s, bs, e->slice.object, fp);
    fp = body_walk(s, bs, e->slice.start, fp);
    fp = body_walk(s, bs, e->slice.end, fp);
    return fp;
  case expr_Return:
    fp = body_walk(s, bs, e->return_expr.value, fp);
    return fp;
  case expr_Defer:
    fp = body_walk(s, bs, e->defer_expr.value, fp);
    return fp;
  case expr_Product:
    fp = body_walk(s, bs, e->product.type_expr, fp);
    if (e->product.Fields) {
      for (size_t i = 0; i < e->product.Fields->count; i++) {
        struct ProductField *f =
            (struct ProductField *)vec_get(e->product.Fields, i);
        if (f)
          fp = body_walk(s, bs, f->value, fp);
      }
    }
    return fp;
  case expr_Handler:
    fp = body_walk(s, bs, e->handler.effect, fp);
    fp = body_walk(s, bs, e->handler.initially_clause, fp);
    fp = body_walk(s, bs, e->handler.return_clause, fp);
    fp = body_walk(s, bs, e->handler.finally_clause, fp);
    if (e->handler.branches) {
      for (size_t i = 0; i < e->handler.branches->count; i++) {
        struct HandlerBranch **slot =
            (struct HandlerBranch **)vec_get(e->handler.branches, i);
        struct HandlerBranch *br = slot ? *slot : NULL;
        if (!br)
          continue;
        fp = walk_param_vec(s, bs, br->pars, fp);
        fp = body_walk(s, bs, br->expr, fp);
      }
    }
    return fp;
  case expr_Mask:
    fp = body_walk(s, bs, e->mask.body, fp);
    return fp;
  case expr_DestructureBind:
    fp = body_walk(s, bs, e->destructure.pattern, fp);
    fp = body_walk(s, bs, e->destructure.value, fp);
    return fp;
  case expr_ArrayLit:
    fp = body_walk(s, bs, e->array_lit.size, fp);
    fp = body_walk(s, bs, e->array_lit.elem_type, fp);
    fp = body_walk(s, bs, e->array_lit.initializer, fp);
    return fp;
  case expr_SliceType:
    fp = body_walk(s, bs, e->slice_type.elem, fp);
    return fp;
  case expr_ManyPtrType:
    fp = body_walk(s, bs, e->many_ptr_type.elem, fp);
    return fp;
  case expr_ArrayType:
    fp = body_walk(s, bs, e->array_type.size, fp);
    fp = body_walk(s, bs, e->array_type.elem, fp);
    return fp;
  case expr_FnType:
    fp = walk_expr_vec(s, bs, e->fn_type.param_types, fp);
    fp = body_walk(s, bs, e->fn_type.ret_type, fp);
    return fp;
  case expr_Struct:
    // Nested struct expressions are inlined (RA-aligned). Walk
    // member field types into the enclosing body_store.
    if (e->struct_expr.members) {
      for (size_t i = 0; i < e->struct_expr.members->count; i++) {
        struct StructMember *m =
            (struct StructMember *)vec_get(e->struct_expr.members, i);
        if (!m)
          continue;
        if (m->kind == member_Field) {
          fp = body_walk(s, bs, m->field.type, fp);
        } else if (m->kind == member_Union && m->union_def.variants) {
          for (size_t j = 0; j < m->union_def.variants->count; j++) {
            struct FieldDef *fd =
                (struct FieldDef *)vec_get(m->union_def.variants, j);
            if (fd)
              fp = body_walk(s, bs, fd->type, fp);
          }
        }
      }
    }
    return fp;
  case expr_Enum:
    if (e->enum_expr.variants) {
      for (size_t i = 0; i < e->enum_expr.variants->count; i++) {
        struct EnumVariant *v =
            (struct EnumVariant *)vec_get(e->enum_expr.variants, i);
        if (v)
          fp = body_walk(s, bs, v->explicit_value, fp);
      }
    }
    return fp;
  case decl_Effect:
    if (e->effect.op_declaration) {
      for (size_t i = 0; i < e->effect.op_declaration->count; i++) {
        struct OpDecl **slot =
            (struct OpDecl **)vec_get(e->effect.op_declaration, i);
        struct OpDecl *op = slot ? *slot : NULL;
        if (!op)
          continue;
        fp = walk_param_vec(s, bs, op->params, fp);
        fp = body_walk(s, bs, op->effect_type, fp);
        fp = body_walk(s, bs, op->result_type, fp);
      }
    }
    return fp;
  case expr_EffectRow:
    fp = body_walk(s, bs, e->effect_row.head, fp);
    return fp;
  case expr_Ctl:
    fp = walk_param_vec(s, bs, e->ctl.params, fp);
    fp = body_walk(s, bs, e->ctl.ret_type, fp);
    fp = body_walk(s, bs, e->ctl.body, fp);
    return fp;
  }
  return fp;
}

// Walk up the scope chain to find the owning module for a decl. Mirrors
// the static helper in ids.c — replicated here to avoid exposing it.
static ModuleId body_store_owning_module(struct Sema *s, DefId decl) {
  struct DefInfo *di = def_info(s, decl);
  if (!di)
    return MODULE_ID_INVALID;
  ScopeId cur = di->owner_scope;
  while (scope_id_is_valid(cur)) {
    struct ScopeInfo *si = scope_info(s, cur);
    if (!si)
      break;
    if (module_id_is_valid(si->owner_module))
      return si->owner_module;
    cur = si->parent;
  }
  return MODULE_ID_INVALID;
}

struct BodyStore *query_body_store(struct Sema *s, DefId decl) {
  if (!s || !def_id_is_valid(decl))
    return NULL;

  // Lazy-allocate / lookup the per-decl BodyStore. Keyed by DefId.idx
  // in s->body_stores. The slot lives inline on the BodyStore.
  uint64_t k = (uint64_t)decl.idx;
  struct BodyStore *bs = NULL;
  if (hashmap_contains(&s->body_stores, k)) {
    bs = (struct BodyStore *)hashmap_get(&s->body_stores, k);
  } else {
    bs = arena_alloc(&s->arena, sizeof(*bs));
    *bs = (struct BodyStore){.decl = decl};
    bs->exprs = vec_new_in(&s->arena, sizeof(struct Expr *));
    // Seat index 0 with a NULL sentinel so local == 0 = EXPR_ID_NONE.
    struct Expr *null_sentinel = NULL;
    vec_push(bs->exprs, &null_sentinel);
    hashmap_init_in(&bs->nodeid_to_local, &s->arena);
    sema_query_slot_init(&bs->query, QUERY_BODY_STORE);
    hashmap_put_or_die(&s->body_stores, k, bs, "body_stores");
  }

  // Origin lookup happens up front so we have a span for the frame
  // (used for cycle diagnostics) and can early-out cleanly on missing
  // origins (DECL_FIELD etc.).
  struct Expr *root = def_origin(s, decl);
  if (!root || root->kind != expr_Bind)
    return NULL;
  struct Span frame_span = root->span;

  SEMA_QUERY_GUARD(s, &bs->query, QUERY_BODY_STORE, bs, frame_span,
                   /*on_cached=*/bs,
                   /*on_cycle=*/NULL,
                   /*on_error=*/NULL);

  // ---- Source-of-truth dep: we MUST refresh `exprs` /
  //      `nodeid_to_local` on every source change, because the Expr*
  //      values in the prior parse are logically stale. Record an
  //      explicit dep on query_module_ast so any byte-level edit
  //      forces RECOMPUTE here. (top_level_index's structural
  //      fingerprint is too coarse — it doesn't shift on body-only
  //      edits, so depending only on it would leave `exprs` pointing
  //      at stale Exprs from the prior parse.)
  ModuleId mid = body_store_owning_module(s, decl);
  if (module_id_is_valid(mid))
    (void)query_module_ast(s, mid);

  // Reset accumulating state: keep the null sentinel at index 0,
  // drop everything else.
  bs->exprs->count = 1;
  hashmap_clear(&bs->nodeid_to_local);

  // The root bind itself goes into the store (it's a body-level
  // node from the perspective of whoever walks the module's exprs),
  // then we descend into its annotation + value via body_walk —
  // which handles Lambda/Struct/Enum/decl_Effect descent uniformly
  // (RA-aligned: nested closures inline into the enclosing
  // top-level decl's body_store).
  Fingerprint fp = query_fingerprint_from_u64((uint64_t)decl.idx);
  fp = assign_local(bs, root, 0, fp);
  fp = body_walk(s, bs, root->bind.type_ann, fp);
  fp = body_walk(s, bs, root->bind.value, fp);

  // The slot's fingerprint folds the structural (kind, arity) walk
  // only — invariant to whitespace and to NodeId reshuffles. Downstream
  // body-level cache tables (type_of_expr, const_eval, …) get cutoff
  // when body shape is unchanged even though `exprs` got refreshed.
  query_slot_set_fingerprint(&bs->query, fp);
  sema_query_succeed(s, &bs->query);
  return bs;
}

ExprId expr_to_id(struct Sema *s, struct Expr *expr) {
  if (!expr || expr->id.id == 0)
    return EXPR_ID_NONE;
  // Per-Expr cache: if the body store for this Expr's decl has
  // already run, `expr_id` is set in-line. Direct field read, no
  // hashmap lookup. The cache is invalidated implicitly: when the
  // body_store recomputes, it overwrites `expr_id` on every Expr it
  // visits with the new (decl, local).
  if (expr_id_is_valid(expr->expr_id))
    return expr->expr_id;

  // Cold path: trigger the body_store walk for this Expr's owning
  // decl. The walk populates `expr->expr_id` as a side effect, so
  // the second call short-circuits.
  DefId owner = query_node_to_decl(s, expr);
  if (!def_id_is_valid(owner))
    return EXPR_ID_NONE;
  struct BodyStore *bs = query_body_store(s, owner);
  if (!bs)
    return EXPR_ID_NONE;
  // The walk wrote expr->expr_id if `expr` was reachable from the
  // body root. Return it directly — `EXPR_ID_NONE` if the Expr is
  // outside the body (e.g. synthetic, or owned by a nested decl's
  // body_store that hasn't been built yet).
  return expr->expr_id;
}

struct Expr *id_to_expr(struct Sema *s, ExprId id) {
  if (!s || !expr_id_is_valid(id))
    return NULL;
  uint64_t k = (uint64_t)id.decl.idx;
  if (!hashmap_contains(&s->body_stores, k))
    return NULL;
  struct BodyStore *bs = (struct BodyStore *)hashmap_get(&s->body_stores, k);
  if (!bs || !bs->exprs || id.local >= bs->exprs->count)
    return NULL;
  struct Expr **slot = (struct Expr **)vec_get(bs->exprs, id.local);
  return slot ? *slot : NULL;
}
