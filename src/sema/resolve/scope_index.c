#include "scope_index.h"

#include <stddef.h>

#include "../../common/hashmap.h"
#include "../../parser/ast.h"
#include "../modules/modules.h"
#include "../scope/scope.h"
#include "../sema.h"

// Implementation note on the scope-index walk:
//
// The walker is a single recursive function `walk(s, expr, scope)`
// that does two things at every node:
//   1. Records `node->id.id -> scope.idx` in s->node_to_scope so
//      query_scope_for_node can read it back.
//   2. Recurses into children with the appropriate scope. Most
//      kinds keep the current scope; scope-introducers (Lambda,
//      Block, Handler, Loop, Switch arm) create a child scope and
//      recurse into their body with it.
//
// Local definitions (function params, block-scoped binds, handler
// op params) are allocated as DefIds and inserted into the
// containing scope at the moment the walker encounters them. This
// is the local mirror of what def_map.c does for top-level decls.
//
// We deliberately do NOT recurse into struct/enum/effect bodies
// here — their members are handled by def_map's type-shape pass.
// This keeps scope_index focused on executable code paths.

static void walk(struct Sema *s, struct Expr *e, ScopeId scope);

static void record_node(struct Sema *s, struct NodeId node, ScopeId scope) {
  if (node.id == 0)
    return;
  hashmap_put(&s->node_to_scope, (uint64_t)node.id,
              (void *)(uintptr_t)scope.idx);
}

static ModuleId scope_owner_module(struct Sema *s, ScopeId scope) {
  struct ScopeInfo *si = scope_info(s, scope);
  return si ? si->owner_module : MODULE_ID_INVALID;
}

static ScopeId child_scope(struct Sema *s, ScopeKind kind, ScopeId parent) {
  return scope_create(s, kind, parent, scope_owner_module(s, parent));
}

static void walk_vec(struct Sema *s, Vec *exprs, ScopeId scope) {
  if (!exprs)
    return;
  for (size_t i = 0; i < exprs->count; i++) {
    struct Expr **slot = (struct Expr **)vec_get(exprs, i);
    walk(s, slot ? *slot : NULL, scope);
  }
}

static void define_param(struct Sema *s, struct Param *p, ScopeId scope) {
  if (!p || p->name.string_id == 0)
    return;
  bool is_comptime = p->kind == PARAM_COMPTIME ||
                     p->kind == PARAM_INFERRED_COMPTIME;
  struct DefInfo proto = {
      .kind = DECL_PARAM,
      .semantic_kind = SEM_VALUE,
      .name_id = p->name.string_id,
      .span = p->name.span,
      .origin_id = (struct NodeId){0},
      .origin = NULL,
      .owner_scope = scope,
      .child_scope = SCOPE_ID_INVALID,
      .imported_module = MODULE_ID_INVALID,
      .vis = Visibility_public,
      .scope_token_id = 0,
      .is_comptime = is_comptime,
      .has_effects = false,
  };
  DefId def = def_create(s, proto);
  scope_insert_def(s, scope, def);
  walk(s, p->type_ann, scope);
}

static void define_local_bind(struct Sema *s, struct Expr *e, ScopeId scope) {
  struct BindExpr *b = &e->bind;
  if (b->name.string_id == 0)
    return;
  struct DefInfo proto = {
      .kind = DECL_USER,
      .semantic_kind = SEM_VALUE,
      .name_id = b->name.string_id,
      .span = b->name.span,
      .origin_id = e->id,
      .origin = e,
      .owner_scope = scope,
      .child_scope = SCOPE_ID_INVALID,
      .imported_module = MODULE_ID_INVALID,
      .vis = b->visibility,
      .scope_token_id = 0,
      .is_comptime = e->is_comptime,
      .has_effects = false,
  };
  DefId def = def_create(s, proto);
  scope_insert_def(s, scope, def);
}

static void walk_handler_branch(struct Sema *s, struct HandlerBranch *br,
                                ScopeId handler_scope) {
  if (!br)
    return;
  ScopeId op_scope = child_scope(s, SCOPE_FUNCTION, handler_scope);
  if (br->pars) {
    for (size_t i = 0; i < br->pars->count; i++) {
      struct Param *p = (struct Param *)vec_get(br->pars, i);
      define_param(s, p, op_scope);
    }
  }
  walk(s, br->expr, op_scope);
}

static void walk(struct Sema *s, struct Expr *e, ScopeId scope) {
  if (!e)
    return;
  record_node(s, e->id, scope);

  switch (e->kind) {
  // Leaves — nothing to recurse into.
  case expr_Lit:
  case expr_Wildcard:
  case expr_Break:
  case expr_Continue:
  case expr_Ident:
  case expr_EnumRef:
  case expr_Asm:
    return;

  case expr_Bin:
    walk(s, e->bin.Left, scope);
    walk(s, e->bin.Right, scope);
    return;

  case expr_Assign:
    walk(s, e->assign.target, scope);
    walk(s, e->assign.value, scope);
    return;

  case expr_Unary:
    walk(s, e->unary.operand, scope);
    return;

  case expr_Call:
    walk(s, e->call.callee, scope);
    walk_vec(s, e->call.args, scope);
    return;

  case expr_Builtin:
    walk_vec(s, e->builtin.args, scope);
    return;

  case expr_If:
    walk(s, e->if_expr.condition, scope);
    walk(s, e->if_expr.then_branch, scope);
    walk(s, e->if_expr.else_branch, scope);
    return;

  case expr_Block: {
    ScopeId block = child_scope(s, SCOPE_BLOCK, scope);
    if (!e->block.stmts)
      return;
    // First pass: register all top-of-block bindings so they're
    // visible to peer statements (mutual let binding within a
    // block is rare but the existing resolver allowed it).
    // Actually no — block scope semantics in this language are
    // post-declaration. Single-pass walk it is.
    for (size_t i = 0; i < e->block.stmts->count; i++) {
      struct Expr **slot = (struct Expr **)vec_get(e->block.stmts, i);
      struct Expr *stmt = slot ? *slot : NULL;
      if (!stmt)
        continue;
      if (stmt->kind == expr_Bind)
        define_local_bind(s, stmt, block);
      walk(s, stmt, block);
    }
    return;
  }

  case expr_Bind:
    // Reached via direct walk (caller didn't pre-define). Define
    // it here too — covers the rare case where a Bind shows up
    // outside an expr_Block (e.g., as the body of a single-
    // expression lambda).
    define_local_bind(s, e, scope);
    walk(s, e->bind.type_ann, scope);
    walk(s, e->bind.value, scope);
    return;

  case expr_Lambda: {
    ScopeId fn = child_scope(s, SCOPE_FUNCTION, scope);
    if (e->lambda.params) {
      for (size_t i = 0; i < e->lambda.params->count; i++) {
        struct Param *p = (struct Param *)vec_get(e->lambda.params, i);
        define_param(s, p, fn);
      }
    }
    walk(s, e->lambda.effect, fn);
    walk(s, e->lambda.ret_type, fn);
    walk(s, e->lambda.body, fn);
    return;
  }

  case expr_Loop: {
    ScopeId loop = child_scope(s, SCOPE_LOOP, scope);
    walk(s, e->loop_expr.init, loop);
    walk(s, e->loop_expr.condition, loop);
    walk(s, e->loop_expr.step, loop);
    walk(s, e->loop_expr.body, loop);
    return;
  }

  case expr_Switch: {
    walk(s, e->switch_expr.scrutinee, scope);
    if (!e->switch_expr.arms)
      return;
    for (size_t i = 0; i < e->switch_expr.arms->count; i++) {
      struct SwitchArm *arm =
          (struct SwitchArm *)vec_get(e->switch_expr.arms, i);
      if (!arm)
        continue;
      // Each arm gets its own scope so pattern bindings don't
      // leak across arms.
      ScopeId arm_scope = child_scope(s, SCOPE_BLOCK, scope);
      walk_vec(s, arm->patterns, arm_scope);
      walk(s, arm->body, arm_scope);
    }
    return;
  }

  case expr_Field:
    walk(s, e->field.object, scope);
    return;

  case expr_Index:
    walk(s, e->index.object, scope);
    walk(s, e->index.index, scope);
    return;

  case expr_Return:
    walk(s, e->return_expr.value, scope);
    return;

  case expr_Defer:
    walk(s, e->defer_expr.value, scope);
    return;

  case expr_Product:
    walk(s, e->product.type_expr, scope);
    if (e->product.Fields) {
      for (size_t i = 0; i < e->product.Fields->count; i++) {
        struct ProductField *f =
            (struct ProductField *)vec_get(e->product.Fields, i);
        if (f)
          walk(s, f->value, scope);
      }
    }
    return;

  case expr_Handler: {
    ScopeId handler = child_scope(s, SCOPE_HANDLER, scope);
    walk(s, e->handler.effect, handler);
    walk(s, e->handler.initially_clause, handler);
    walk(s, e->handler.return_clause, handler);
    walk(s, e->handler.finally_clause, handler);
    if (e->handler.branches) {
      for (size_t i = 0; i < e->handler.branches->count; i++) {
        struct HandlerBranch **slot =
            (struct HandlerBranch **)vec_get(e->handler.branches, i);
        walk_handler_branch(s, slot ? *slot : NULL, handler);
      }
    }
    return;
  }

  case expr_Mask:
    // MaskExpr.body is the masked subtree; the effect annotation
    // is parsed into mask.effect (kept here as future-proof,
    // though current MaskExpr fields may differ — adjust when we
    // exercise mask in tests).
    walk(s, e->mask.body, scope);
    return;

  case expr_DestructureBind:
    walk(s, e->destructure.pattern, scope);
    walk(s, e->destructure.value, scope);
    return;

  case expr_ArrayLit:
    walk(s, e->array_lit.size, scope);
    walk(s, e->array_lit.elem_type, scope);
    walk(s, e->array_lit.initializer, scope);
    return;

  case expr_SliceType:
    walk(s, e->slice_type.elem, scope);
    return;

  case expr_ManyPtrType:
    walk(s, e->many_ptr_type.elem, scope);
    return;

  case expr_ArrayType:
    walk(s, e->array_type.size, scope);
    walk(s, e->array_type.elem, scope);
    return;

  // Type/effect/ctl bodies: members are pre-seeded by def_map's
  // type-shape pass; deeper indexing here will land when tests
  // exercise member-resolution inside these forms.
  case expr_Struct:
  case expr_Enum:
  case decl_Effect:
  case expr_EffectRow:
  case expr_Ctl:
    return;
  }
}

void scope_index_build_module(struct Sema *s, ModuleId mid) {
  struct ModuleInfo *m = module_info(s, mid);
  if (!m || !m->ast || !scope_id_is_valid(m->internal_scope))
    return;

  for (size_t i = 0; i < m->ast->count; i++) {
    struct Expr **slot = (struct Expr **)vec_get(m->ast, i);
    struct Expr *e = slot ? *slot : NULL;
    if (!e)
      continue;
    record_node(s, e->id, m->internal_scope);
    // For top-level binds, def_map already registered the name;
    // walk only the children. For other top-level expressions,
    // walk normally so they're indexed but no duplicate insert
    // happens.
    if (e->kind == expr_Bind) {
      walk(s, e->bind.type_ann, m->internal_scope);
      walk(s, e->bind.value, m->internal_scope);
    } else {
      walk(s, e, m->internal_scope);
    }
  }
}

ScopeId query_scope_for_node(struct Sema *s, struct NodeId node) {
  if (node.id == 0)
    return SCOPE_ID_INVALID;
  if (!hashmap_contains(&s->node_to_scope, (uint64_t)node.id))
    return SCOPE_ID_INVALID;
  void *slot = hashmap_get(&s->node_to_scope, (uint64_t)node.id);
  return (ScopeId){(uint32_t)(uintptr_t)slot};
}
