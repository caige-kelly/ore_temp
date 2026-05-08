#include "scope_index.h"

#include <stddef.h>

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../../parser/ast.h"
#include "../modules/def_map.h"
#include "../modules/modules.h"
#include "../scope/scope.h"
#include "../sema.h"

// Implementation note:
//
// Two recursive walkers share AST traversal shape but record
// different things:
//
//   decl_walk:
//     For each NodeId in the subtree, record node_to_decl[node] =
//     enclosing top-level DefId. No scope creation, no DefId
//     allocation. Cheap; runs once per module.
//
//   scope_walk:
//     For each NodeId, record node_to_scope[node] = current scope
//     (in the per-fn map). At scope-introducing AST forms (Lambda,
//     Block, Handler, Loop), create child scopes inline. Allocate
//     local DefIds for params, block-binds, handler-op params.
//     Runs once per top-level fn.
//
// Both walkers iterate the same AST kinds. The dispatch logic lives
// in scope_walk since it's the more elaborate one; decl_walk has its
// own minimal recursion.
//
// Local DefId allocation (params, lets) happens only in scope_walk.
// The earlier non-lazy version recorded everything to a global
// node_to_scope map and walked all module bodies eagerly; the new
// per-fn split lets a body-only edit invalidate exactly one fn's
// cache.

// === Recording helpers ===

static void record_in_decl_index(struct Sema *s, struct NodeId node,
                                 DefId fn_def) {
  if (node.id == 0)
    return;
  hashmap_put(&s->node_to_decl, (uint64_t)node.id,
              (void *)(uintptr_t)fn_def.idx);
}

// Populate the global NodeId -> Expr* index. Same lifetime as the
// AST (which lives in the Sema arena). Keyed by NodeId so the
// position queries can convert their NodeId hits back to AST
// nodes for resolution.
static void record_node_expr(struct Sema *s, struct Expr *e) {
  if (!e || e->id.id == 0)
    return;
  if (s->node_to_expr.entries == NULL)
    hashmap_init_in(&s->node_to_expr, s->arena);
  hashmap_put(&s->node_to_expr, (uint64_t)e->id.id, e);
}

static void record_in_fn_scope(struct ScopeIndexResult *res,
                               struct NodeId node, ScopeId scope) {
  if (node.id == 0)
    return;
  hashmap_put(&res->node_to_scope, (uint64_t)node.id,
              (void *)(uintptr_t)scope.idx);
}

// === decl_walk: shallow node -> top-level DefId index ===

static void decl_walk(struct Sema *s, struct Expr *e, DefId fn_def) {
  if (!e)
    return;
  record_in_decl_index(s, e->id, fn_def);
  record_node_expr(s, e);

  switch (e->kind) {
  case expr_Lit:
  case expr_Wildcard:
  case expr_Break:
  case expr_Continue:
  case expr_Ident:
  case expr_EnumRef:
  case expr_Asm:
    return;

  case expr_Bin:
    decl_walk(s, e->bin.Left, fn_def);
    decl_walk(s, e->bin.Right, fn_def);
    return;
  case expr_Assign:
    decl_walk(s, e->assign.target, fn_def);
    decl_walk(s, e->assign.value, fn_def);
    return;
  case expr_Unary:
    decl_walk(s, e->unary.operand, fn_def);
    return;
  case expr_Call:
    decl_walk(s, e->call.callee, fn_def);
    if (e->call.args) {
      for (size_t i = 0; i < e->call.args->count; i++) {
        struct Expr **slot = (struct Expr **)vec_get(e->call.args, i);
        decl_walk(s, slot ? *slot : NULL, fn_def);
      }
    }
    return;
  case expr_Builtin:
    if (e->builtin.args) {
      for (size_t i = 0; i < e->builtin.args->count; i++) {
        struct Expr **slot = (struct Expr **)vec_get(e->builtin.args, i);
        decl_walk(s, slot ? *slot : NULL, fn_def);
      }
    }
    return;
  case expr_If:
    decl_walk(s, e->if_expr.condition, fn_def);
    decl_walk(s, e->if_expr.then_branch, fn_def);
    decl_walk(s, e->if_expr.else_branch, fn_def);
    return;
  case expr_Block:
    if (e->block.stmts) {
      for (size_t i = 0; i < e->block.stmts->count; i++) {
        struct Expr **slot = (struct Expr **)vec_get(e->block.stmts, i);
        decl_walk(s, slot ? *slot : NULL, fn_def);
      }
    }
    return;
  case expr_Bind:
    decl_walk(s, e->bind.type_ann, fn_def);
    decl_walk(s, e->bind.value, fn_def);
    return;
  case expr_Lambda:
    if (e->lambda.params) {
      for (size_t i = 0; i < e->lambda.params->count; i++) {
        struct Param *p = (struct Param *)vec_get(e->lambda.params, i);
        if (p)
          decl_walk(s, p->type_ann, fn_def);
      }
    }
    decl_walk(s, e->lambda.effect, fn_def);
    decl_walk(s, e->lambda.ret_type, fn_def);
    decl_walk(s, e->lambda.body, fn_def);
    return;
  case expr_Loop:
    decl_walk(s, e->loop_expr.init, fn_def);
    decl_walk(s, e->loop_expr.condition, fn_def);
    decl_walk(s, e->loop_expr.step, fn_def);
    decl_walk(s, e->loop_expr.body, fn_def);
    return;
  case expr_Switch:
    decl_walk(s, e->switch_expr.scrutinee, fn_def);
    if (e->switch_expr.arms) {
      for (size_t i = 0; i < e->switch_expr.arms->count; i++) {
        struct SwitchArm *arm =
            (struct SwitchArm *)vec_get(e->switch_expr.arms, i);
        if (!arm)
          continue;
        if (arm->patterns) {
          for (size_t j = 0; j < arm->patterns->count; j++) {
            struct Expr **slot =
                (struct Expr **)vec_get(arm->patterns, j);
            decl_walk(s, slot ? *slot : NULL, fn_def);
          }
        }
        decl_walk(s, arm->body, fn_def);
      }
    }
    return;
  case expr_Field:
    decl_walk(s, e->field.object, fn_def);
    return;
  case expr_Index:
    decl_walk(s, e->index.object, fn_def);
    decl_walk(s, e->index.index, fn_def);
    return;
  case expr_Return:
    decl_walk(s, e->return_expr.value, fn_def);
    return;
  case expr_Defer:
    decl_walk(s, e->defer_expr.value, fn_def);
    return;
  case expr_Product:
    decl_walk(s, e->product.type_expr, fn_def);
    if (e->product.Fields) {
      for (size_t i = 0; i < e->product.Fields->count; i++) {
        struct ProductField *f =
            (struct ProductField *)vec_get(e->product.Fields, i);
        if (f)
          decl_walk(s, f->value, fn_def);
      }
    }
    return;
  case expr_Handler:
    decl_walk(s, e->handler.effect, fn_def);
    decl_walk(s, e->handler.initially_clause, fn_def);
    decl_walk(s, e->handler.return_clause, fn_def);
    decl_walk(s, e->handler.finally_clause, fn_def);
    if (e->handler.branches) {
      for (size_t i = 0; i < e->handler.branches->count; i++) {
        struct HandlerBranch **slot =
            (struct HandlerBranch **)vec_get(e->handler.branches, i);
        struct HandlerBranch *br = slot ? *slot : NULL;
        if (!br)
          continue;
        if (br->pars) {
          for (size_t j = 0; j < br->pars->count; j++) {
            struct Param *p = (struct Param *)vec_get(br->pars, j);
            if (p)
              decl_walk(s, p->type_ann, fn_def);
          }
        }
        decl_walk(s, br->expr, fn_def);
      }
    }
    return;
  case expr_Mask:
    decl_walk(s, e->mask.body, fn_def);
    return;
  case expr_DestructureBind:
    decl_walk(s, e->destructure.pattern, fn_def);
    decl_walk(s, e->destructure.value, fn_def);
    return;
  case expr_ArrayLit:
    decl_walk(s, e->array_lit.size, fn_def);
    decl_walk(s, e->array_lit.elem_type, fn_def);
    decl_walk(s, e->array_lit.initializer, fn_def);
    return;
  case expr_SliceType:
    decl_walk(s, e->slice_type.elem, fn_def);
    return;
  case expr_ManyPtrType:
    decl_walk(s, e->many_ptr_type.elem, fn_def);
    return;
  case expr_ArrayType:
    decl_walk(s, e->array_type.size, fn_def);
    decl_walk(s, e->array_type.elem, fn_def);
    return;
  case expr_Struct:
    if (e->struct_expr.members) {
      for (size_t i = 0; i < e->struct_expr.members->count; i++) {
        struct StructMember *m =
            (struct StructMember *)vec_get(e->struct_expr.members, i);
        if (!m) continue;
        if (m->kind == member_Field) {
          decl_walk(s, m->field.type, fn_def);
          decl_walk(s, m->field.default_value, fn_def);
        } else if (m->kind == member_Union && m->union_def.variants) {
          for (size_t j = 0; j < m->union_def.variants->count; j++) {
            struct FieldDef *fd =
                (struct FieldDef *)vec_get(m->union_def.variants, j);
            if (fd) decl_walk(s, fd->type, fn_def);
          }
        }
      }
    }
    return;
  case expr_Enum:
    if (e->enum_expr.variants) {
      for (size_t i = 0; i < e->enum_expr.variants->count; i++) {
        struct EnumVariant *v =
            (struct EnumVariant *)vec_get(e->enum_expr.variants, i);
        if (v) decl_walk(s, v->explicit_value, fn_def);
      }
    }
    return;
  case decl_Effect:
    if (e->effect.op_declaration) {
      for (size_t i = 0; i < e->effect.op_declaration->count; i++) {
        struct OpDecl **slot =
            (struct OpDecl **)vec_get(e->effect.op_declaration, i);
        struct OpDecl *op = slot ? *slot : NULL;
        if (!op) continue;
        if (op->params) {
          for (size_t j = 0; j < op->params->count; j++) {
            struct Param *p = (struct Param *)vec_get(op->params, j);
            if (p) decl_walk(s, p->type_ann, fn_def);
          }
        }
        decl_walk(s, op->effect_type, fn_def);
        decl_walk(s, op->result_type, fn_def);
      }
    }
    return;
  case expr_EffectRow:
    decl_walk(s, e->effect_row.head, fn_def);
    return;
  case expr_Ctl:
    if (e->ctl.params) {
      for (size_t i = 0; i < e->ctl.params->count; i++) {
        struct Param *p = (struct Param *)vec_get(e->ctl.params, i);
        if (p) decl_walk(s, p->type_ann, fn_def);
      }
    }
    decl_walk(s, e->ctl.ret_type, fn_def);
    decl_walk(s, e->ctl.body, fn_def);
    return;
  }
}

// === query_node_to_decl_index ===

void query_node_to_decl_index(struct Sema *s, ModuleId mid) {
  struct ModuleInfo *m = module_info(s, mid);
  if (!m)
    return;
  Vec *ast = m->ast ? m->ast : query_module_ast(s, mid);
  if (!ast)
    return;
  // Ensure top-level def_map ran so we have DefIds to record.
  Vec *idx = query_top_level_index(s, mid);
  if (!idx)
    return;

  for (size_t i = 0; i < idx->count; i++) {
    struct TopLevelEntry *entry =
        (struct TopLevelEntry *)vec_get(idx, i);
    if (!entry || entry->name_id == 0)
      continue;
    DefId fn_def = query_def_for_name(s, mid, entry->name_id);
    if (!def_id_is_valid(fn_def))
      continue;
    decl_walk(s, entry->node, fn_def);
  }
}

DefId query_node_to_decl(struct Sema *s, struct NodeId node) {
  if (node.id == 0)
    return DEF_ID_INVALID;
  if (!hashmap_contains(&s->node_to_decl, (uint64_t)node.id))
    return DEF_ID_INVALID;
  void *slot = hashmap_get(&s->node_to_decl, (uint64_t)node.id);
  return (DefId){(uint32_t)(uintptr_t)slot};
}

// === scope_walk: per-fn deep walk creating scopes + locals ===

static void scope_walk(struct Sema *s, struct ScopeIndexResult *res,
                       struct Expr *e, ScopeId scope);

static ModuleId scope_owner_module(struct Sema *s, ScopeId scope) {
  struct ScopeInfo *si = scope_info(s, scope);
  return si ? si->owner_module : MODULE_ID_INVALID;
}

static ScopeId child_scope(struct Sema *s, struct ScopeIndexResult *res,
                           ScopeKind kind, ScopeId parent) {
  ScopeId child =
      scope_create(s, kind, parent, scope_owner_module(s, parent));
  if (res && res->local_scopes)
    vec_push(res->local_scopes, &child);
  return child;
}

static void define_param(struct Sema *s, struct ScopeIndexResult *res,
                         struct Param *p, ScopeId scope) {
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
  scope_walk(s, res, p->type_ann, scope);
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

static void scope_walk_handler_branch(struct Sema *s,
                                      struct ScopeIndexResult *res,
                                      struct HandlerBranch *br,
                                      ScopeId handler_scope) {
  if (!br)
    return;
  ScopeId op_scope = child_scope(s, res, SCOPE_FUNCTION, handler_scope);
  if (br->pars) {
    for (size_t i = 0; i < br->pars->count; i++) {
      struct Param *p = (struct Param *)vec_get(br->pars, i);
      define_param(s, res, p, op_scope);
    }
  }
  scope_walk(s, res, br->expr, op_scope);
}

static void scope_walk(struct Sema *s, struct ScopeIndexResult *res,
                       struct Expr *e, ScopeId scope) {
  if (!e)
    return;
  record_in_fn_scope(res, e->id, scope);

  switch (e->kind) {
  case expr_Lit:
  case expr_Wildcard:
  case expr_Break:
  case expr_Continue:
  case expr_Ident:
  case expr_EnumRef:
  case expr_Asm:
    return;

  case expr_Bin:
    scope_walk(s, res, e->bin.Left, scope);
    scope_walk(s, res, e->bin.Right, scope);
    return;

  case expr_Assign:
    scope_walk(s, res, e->assign.target, scope);
    scope_walk(s, res, e->assign.value, scope);
    return;

  case expr_Unary:
    scope_walk(s, res, e->unary.operand, scope);
    return;

  case expr_Call:
    scope_walk(s, res, e->call.callee, scope);
    if (e->call.args) {
      for (size_t i = 0; i < e->call.args->count; i++) {
        struct Expr **slot = (struct Expr **)vec_get(e->call.args, i);
        scope_walk(s, res, slot ? *slot : NULL, scope);
      }
    }
    return;

  case expr_Builtin:
    if (e->builtin.args) {
      for (size_t i = 0; i < e->builtin.args->count; i++) {
        struct Expr **slot = (struct Expr **)vec_get(e->builtin.args, i);
        scope_walk(s, res, slot ? *slot : NULL, scope);
      }
    }
    return;

  case expr_If:
    scope_walk(s, res, e->if_expr.condition, scope);
    scope_walk(s, res, e->if_expr.then_branch, scope);
    scope_walk(s, res, e->if_expr.else_branch, scope);
    return;

  case expr_Block: {
    ScopeId block = child_scope(s, res, SCOPE_BLOCK, scope);
    if (!e->block.stmts)
      return;
    for (size_t i = 0; i < e->block.stmts->count; i++) {
      struct Expr **slot = (struct Expr **)vec_get(e->block.stmts, i);
      struct Expr *stmt = slot ? *slot : NULL;
      if (!stmt)
        continue;
      if (stmt->kind == expr_Bind)
        define_local_bind(s, stmt, block);
      scope_walk(s, res, stmt, block);
    }
    return;
  }

  case expr_Bind:
    define_local_bind(s, e, scope);
    scope_walk(s, res, e->bind.type_ann, scope);
    scope_walk(s, res, e->bind.value, scope);
    return;

  case expr_Lambda: {
    ScopeId fn = child_scope(s, res, SCOPE_FUNCTION, scope);
    if (e->lambda.params) {
      for (size_t i = 0; i < e->lambda.params->count; i++) {
        struct Param *p = (struct Param *)vec_get(e->lambda.params, i);
        define_param(s, res, p, fn);
      }
    }
    scope_walk(s, res, e->lambda.effect, fn);
    scope_walk(s, res, e->lambda.ret_type, fn);
    scope_walk(s, res, e->lambda.body, fn);
    return;
  }

  case expr_Loop: {
    ScopeId loop = child_scope(s, res, SCOPE_LOOP, scope);
    scope_walk(s, res, e->loop_expr.init, loop);
    scope_walk(s, res, e->loop_expr.condition, loop);
    scope_walk(s, res, e->loop_expr.step, loop);
    scope_walk(s, res, e->loop_expr.body, loop);
    return;
  }

  case expr_Switch: {
    scope_walk(s, res, e->switch_expr.scrutinee, scope);
    if (!e->switch_expr.arms)
      return;
    for (size_t i = 0; i < e->switch_expr.arms->count; i++) {
      struct SwitchArm *arm =
          (struct SwitchArm *)vec_get(e->switch_expr.arms, i);
      if (!arm)
        continue;
      ScopeId arm_scope = child_scope(s, res, SCOPE_BLOCK, scope);
      if (arm->patterns) {
        for (size_t j = 0; j < arm->patterns->count; j++) {
          struct Expr **slot =
              (struct Expr **)vec_get(arm->patterns, j);
          scope_walk(s, res, slot ? *slot : NULL, arm_scope);
        }
      }
      scope_walk(s, res, arm->body, arm_scope);
    }
    return;
  }

  case expr_Field:
    scope_walk(s, res, e->field.object, scope);
    return;

  case expr_Index:
    scope_walk(s, res, e->index.object, scope);
    scope_walk(s, res, e->index.index, scope);
    return;

  case expr_Return:
    scope_walk(s, res, e->return_expr.value, scope);
    return;

  case expr_Defer:
    scope_walk(s, res, e->defer_expr.value, scope);
    return;

  case expr_Product:
    scope_walk(s, res, e->product.type_expr, scope);
    if (e->product.Fields) {
      for (size_t i = 0; i < e->product.Fields->count; i++) {
        struct ProductField *f =
            (struct ProductField *)vec_get(e->product.Fields, i);
        if (f)
          scope_walk(s, res, f->value, scope);
      }
    }
    return;

  case expr_Handler: {
    ScopeId handler = child_scope(s, res, SCOPE_HANDLER, scope);
    scope_walk(s, res, e->handler.effect, handler);
    scope_walk(s, res, e->handler.initially_clause, handler);
    scope_walk(s, res, e->handler.return_clause, handler);
    scope_walk(s, res, e->handler.finally_clause, handler);
    if (e->handler.branches) {
      for (size_t i = 0; i < e->handler.branches->count; i++) {
        struct HandlerBranch **slot =
            (struct HandlerBranch **)vec_get(e->handler.branches, i);
        scope_walk_handler_branch(s, res, slot ? *slot : NULL, handler);
      }
    }
    return;
  }

  case expr_Mask:
    scope_walk(s, res, e->mask.body, scope);
    return;

  case expr_DestructureBind:
    scope_walk(s, res, e->destructure.pattern, scope);
    scope_walk(s, res, e->destructure.value, scope);
    return;

  case expr_ArrayLit:
    scope_walk(s, res, e->array_lit.size, scope);
    scope_walk(s, res, e->array_lit.elem_type, scope);
    scope_walk(s, res, e->array_lit.initializer, scope);
    return;

  case expr_SliceType:
    scope_walk(s, res, e->slice_type.elem, scope);
    return;

  case expr_ManyPtrType:
    scope_walk(s, res, e->many_ptr_type.elem, scope);
    return;

  case expr_ArrayType:
    scope_walk(s, res, e->array_type.size, scope);
    scope_walk(s, res, e->array_type.elem, scope);
    return;

  case expr_Struct:
    if (e->struct_expr.members) {
      for (size_t i = 0; i < e->struct_expr.members->count; i++) {
        struct StructMember *m =
            (struct StructMember *)vec_get(e->struct_expr.members, i);
        if (!m) continue;
        if (m->kind == member_Field) {
          scope_walk(s, res, m->field.type, scope);
          scope_walk(s, res, m->field.default_value, scope);
        } else if (m->kind == member_Union && m->union_def.variants) {
          for (size_t j = 0; j < m->union_def.variants->count; j++) {
            struct FieldDef *fd =
                (struct FieldDef *)vec_get(m->union_def.variants, j);
            if (fd) scope_walk(s, res, fd->type, scope);
          }
        }
      }
    }
    return;
  case expr_Enum:
    if (e->enum_expr.variants) {
      for (size_t i = 0; i < e->enum_expr.variants->count; i++) {
        struct EnumVariant *v =
            (struct EnumVariant *)vec_get(e->enum_expr.variants, i);
        if (v) scope_walk(s, res, v->explicit_value, scope);
      }
    }
    return;
  case decl_Effect:
    if (e->effect.op_declaration) {
      for (size_t i = 0; i < e->effect.op_declaration->count; i++) {
        struct OpDecl **slot =
            (struct OpDecl **)vec_get(e->effect.op_declaration, i);
        struct OpDecl *op = slot ? *slot : NULL;
        if (!op) continue;
        if (op->params) {
          for (size_t j = 0; j < op->params->count; j++) {
            struct Param *p = (struct Param *)vec_get(op->params, j);
            if (p) scope_walk(s, res, p->type_ann, scope);
          }
        }
        scope_walk(s, res, op->effect_type, scope);
        scope_walk(s, res, op->result_type, scope);
      }
    }
    return;
  case expr_EffectRow:
    scope_walk(s, res, e->effect_row.head, scope);
    return;
  case expr_Ctl:
    if (e->ctl.params) {
      for (size_t i = 0; i < e->ctl.params->count; i++) {
        struct Param *p = (struct Param *)vec_get(e->ctl.params, i);
        if (p) scope_walk(s, res, p->type_ann, scope);
      }
    }
    scope_walk(s, res, e->ctl.ret_type, scope);
    scope_walk(s, res, e->ctl.body, scope);
    return;
  }
}

// === query_fn_scope_index ===

struct ScopeIndexResult *query_fn_scope_index(struct Sema *s, DefId fn_def) {
  if (!def_id_is_valid(fn_def))
    return NULL;
  struct DefInfo *di = def_info(s, fn_def);
  if (!di || !di->origin)
    return NULL;

  uint64_t key = (uint64_t)fn_def.idx;
  if (s->fn_scope_index_cache.entries == NULL)
    hashmap_init_in(&s->fn_scope_index_cache, s->arena);

  struct ScopeIndexResult *res = NULL;
  if (hashmap_contains(&s->fn_scope_index_cache, key)) {
    res = (struct ScopeIndexResult *)hashmap_get(&s->fn_scope_index_cache,
                                                 key);
  } else {
    res = arena_alloc(s->arena, sizeof(struct ScopeIndexResult));
    *res = (struct ScopeIndexResult){
        .fn_def = fn_def,
        .local_scopes = vec_new_in(s->arena, sizeof(ScopeId)),
    };
    hashmap_init_in(&res->node_to_scope, s->arena);
    sema_query_slot_init(&res->query, QUERY_FN_SCOPE_INDEX);
    hashmap_put(&s->fn_scope_index_cache, key, res);
  }

  struct Span frame_span = di->span;
  SEMA_QUERY_GUARD(s, &res->query, QUERY_FN_SCOPE_INDEX, res, frame_span,
                   /*on_cached=*/res, /*on_cycle=*/NULL,
                   /*on_error=*/NULL);

  // The fn's "origin" is the top-level expr_Bind. Walk it under
  // module scope — the Bind sits in module scope; the Lambda
  // inside introduces SCOPE_FUNCTION and the rest cascades.
  ScopeId module_scope = di->owner_scope;
  scope_walk(s, res, di->origin, module_scope);

  sema_query_succeed(s, &res->query);
  return res;
}

// === query_scope_for_node ===

ScopeId query_scope_for_node(struct Sema *s, struct NodeId node) {
  if (node.id == 0)
    return SCOPE_ID_INVALID;

  DefId fn_def = query_node_to_decl(s, node);
  if (!def_id_is_valid(fn_def))
    return SCOPE_ID_INVALID;
  struct ScopeIndexResult *res = query_fn_scope_index(s, fn_def);
  if (!res || !hashmap_contains(&res->node_to_scope, (uint64_t)node.id))
    return SCOPE_ID_INVALID;
  void *slot = hashmap_get(&res->node_to_scope, (uint64_t)node.id);
  return (ScopeId){(uint32_t)(uintptr_t)slot};
}

// === scope_index_build_module ===

void scope_index_build_module(struct Sema *s, ModuleId mid) {
  struct ModuleInfo *m = module_info(s, mid);
  if (!m)
    return;

  // Step 1: ensure top-level def_map is run and node_to_decl is
  // populated for every node in the module.
  query_node_to_decl_index(s, mid);

  // Step 2: build per-fn scope_index for every top-level decl
  // (so the cache is fully populated for the batch driver).
  Vec *idx = query_top_level_index(s, mid);
  if (!idx)
    return;
  for (size_t i = 0; i < idx->count; i++) {
    struct TopLevelEntry *entry =
        (struct TopLevelEntry *)vec_get(idx, i);
    if (!entry || entry->name_id == 0)
      continue;
    DefId def = query_def_for_name(s, mid, entry->name_id);
    if (def_id_is_valid(def))
      query_fn_scope_index(s, def);
  }
}
