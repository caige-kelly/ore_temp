#include "position.h"

#include <stddef.h>

#include "../../common/hashmap.h"
#include "../../parser/ast.h"
#include "../modules/modules.h"
#include "../resolve/resolve.h"
#include "../resolve/scope_index.h"
#include "../scope/scope.h"
#include "../sema.h"

// Span containment: does (line, col) sit inside [span.line:column,
// span.line_end:column_end]? Spans use 1-indexed lines/cols.
//
// Edge cases:
//   - Line outside [start, end]: no.
//   - On start line but column < start col: no.
//   - On end line but column >= end col: no (end col is exclusive).
//   - Otherwise: yes.
static bool span_contains(struct Span span, uint32_t line, uint32_t col) {
  if ((int)line < span.line || (int)line > span.line_end)
    return false;
  if ((int)line == span.line && (int)col < span.column)
    return false;
  if ((int)line == span.line_end && (int)col >= span.column_end)
    return false;
  return true;
}

// Recursive walk: descend into children whose span contains the
// position; return the innermost (deepest) NodeId. Symmetric with
// scope_index's walker — covers the same set of AST kinds. Less
// elaborate because we only descend, never create scopes.
//
// Returns NodeId{0} when no descendant matches; the caller handles
// propagation. If `e`'s own span contains the position but no
// child does, we return `e->id` — the most-specific node we
// could find.
static struct NodeId find_innermost(struct Expr *e, uint32_t line,
                                    uint32_t col);

static struct NodeId try_child(struct Expr *e, uint32_t line, uint32_t col) {
  return find_innermost(e, line, col);
}

#define TRY(child)                                                       \
  do {                                                                   \
    struct NodeId __r = try_child((child), line, col);                   \
    if (__r.id != 0)                                                     \
      return __r;                                                        \
  } while (0)

static struct NodeId find_innermost(struct Expr *e, uint32_t line,
                                    uint32_t col) {
  if (!e)
    return (struct NodeId){0};
  if (!span_contains(e->span, line, col))
    return (struct NodeId){0};

  switch (e->kind) {
  case expr_Lit:
  case expr_Wildcard:
  case expr_Break:
  case expr_Continue:
  case expr_Ident:
  case expr_EnumRef:
  case expr_Asm:
    return e->id;

  case expr_Bin:
    TRY(e->bin.Left);
    TRY(e->bin.Right);
    return e->id;

  case expr_Assign:
    TRY(e->assign.target);
    TRY(e->assign.value);
    return e->id;

  case expr_Unary:
    TRY(e->unary.operand);
    return e->id;

  case expr_Call:
    TRY(e->call.callee);
    if (e->call.args) {
      for (size_t i = 0; i < e->call.args->count; i++) {
        struct Expr **slot = (struct Expr **)vec_get(e->call.args, i);
        if (slot)
          TRY(*slot);
      }
    }
    return e->id;

  case expr_Builtin:
    if (e->builtin.args) {
      for (size_t i = 0; i < e->builtin.args->count; i++) {
        struct Expr **slot = (struct Expr **)vec_get(e->builtin.args, i);
        if (slot)
          TRY(*slot);
      }
    }
    return e->id;

  case expr_If:
    TRY(e->if_expr.condition);
    TRY(e->if_expr.then_branch);
    TRY(e->if_expr.else_branch);
    return e->id;

  case expr_Block:
    if (e->block.stmts) {
      for (size_t i = 0; i < e->block.stmts->count; i++) {
        struct Expr **slot = (struct Expr **)vec_get(e->block.stmts, i);
        if (slot)
          TRY(*slot);
      }
    }
    return e->id;

  case expr_Bind:
    TRY(e->bind.type_ann);
    TRY(e->bind.value);
    return e->id;

  case expr_Lambda:
    if (e->lambda.params) {
      for (size_t i = 0; i < e->lambda.params->count; i++) {
        struct Param *p = (struct Param *)vec_get(e->lambda.params, i);
        if (p)
          TRY(p->type_ann);
      }
    }
    TRY(e->lambda.effect);
    TRY(e->lambda.ret_type);
    TRY(e->lambda.body);
    return e->id;

  case expr_Loop:
    TRY(e->loop_expr.init);
    TRY(e->loop_expr.condition);
    TRY(e->loop_expr.step);
    TRY(e->loop_expr.body);
    return e->id;

  case expr_Switch:
    TRY(e->switch_expr.scrutinee);
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
            if (slot)
              TRY(*slot);
          }
        }
        TRY(arm->body);
      }
    }
    return e->id;

  case expr_Field:
    TRY(e->field.object);
    return e->id;

  case expr_Index:
    TRY(e->index.object);
    TRY(e->index.index);
    return e->id;

  case expr_Return:
    TRY(e->return_expr.value);
    return e->id;

  case expr_Defer:
    TRY(e->defer_expr.value);
    return e->id;

  case expr_Product:
    TRY(e->product.type_expr);
    if (e->product.Fields) {
      for (size_t i = 0; i < e->product.Fields->count; i++) {
        struct ProductField *f =
            (struct ProductField *)vec_get(e->product.Fields, i);
        if (f)
          TRY(f->value);
      }
    }
    return e->id;

  case expr_Handler:
    TRY(e->handler.effect);
    TRY(e->handler.initially_clause);
    TRY(e->handler.return_clause);
    TRY(e->handler.finally_clause);
    if (e->handler.branches) {
      for (size_t i = 0; i < e->handler.branches->count; i++) {
        struct HandlerBranch **slot =
            (struct HandlerBranch **)vec_get(e->handler.branches, i);
        struct HandlerBranch *br = slot ? *slot : NULL;
        if (!br)
          continue;
        TRY(br->expr);
      }
    }
    return e->id;

  case expr_Mask:
    TRY(e->mask.body);
    return e->id;

  case expr_DestructureBind:
    TRY(e->destructure.pattern);
    TRY(e->destructure.value);
    return e->id;

  case expr_ArrayLit:
    TRY(e->array_lit.size);
    TRY(e->array_lit.elem_type);
    TRY(e->array_lit.initializer);
    return e->id;

  case expr_SliceType:
    TRY(e->slice_type.elem);
    return e->id;

  case expr_ManyPtrType:
    TRY(e->many_ptr_type.elem);
    return e->id;

  case expr_ArrayType:
    TRY(e->array_type.size);
    TRY(e->array_type.elem);
    return e->id;

  case expr_Struct:
  case expr_Enum:
  case decl_Effect:
  case expr_EffectRow:
  case expr_Ctl:
    return e->id;
  }
  return e->id;
}

#undef TRY

struct NodeId query_node_at_position(struct Sema *s, ModuleId mid,
                                     uint32_t line, uint32_t col) {
  Vec *ast = query_module_ast(s, mid);
  if (!ast)
    return (struct NodeId){0};

  // Linear scan over top-level expressions; the recursive
  // descent inside each handles nesting. For thousands-of-decls
  // modules, swap this for the binary-search-on-sorted-spans
  // approach noted in position.h's docs.
  for (size_t i = 0; i < ast->count; i++) {
    struct Expr **slot = (struct Expr **)vec_get(ast, i);
    struct Expr *e = slot ? *slot : NULL;
    if (!e)
      continue;
    struct NodeId hit = find_innermost(e, line, col);
    if (hit.id != 0)
      return hit;
  }
  return (struct NodeId){0};
}

DefId query_def_at_position(struct Sema *s, ModuleId mid, uint32_t line,
                            uint32_t col) {
  struct NodeId node = query_node_at_position(s, mid, line, col);
  if (node.id == 0)
    return DEF_ID_INVALID;

  // Look up the Expr* for this NodeId via the index populated by
  // scope_index's decl_walk. The index is built lazily per-module
  // — trigger it for `mid` if it hasn't run yet.
  if (s->node_to_expr.entries == NULL ||
      !hashmap_contains(&s->node_to_expr, (uint64_t)node.id))
    query_node_to_decl_index(s, mid);

  if (!hashmap_contains(&s->node_to_expr, (uint64_t)node.id))
    return DEF_ID_INVALID;

  struct Expr *e =
      (struct Expr *)hashmap_get(&s->node_to_expr, (uint64_t)node.id);
  if (!e)
    return DEF_ID_INVALID;

  // Resolution today only handles bare Idents directly. Field
  // chains (a.b.c) need query_resolve_path with a constructed
  // segment list — straightforward extension when LSP exercises
  // these.
  if (e->kind == expr_Ident)
    return query_resolve_ref(s, e, NS_VALUE);
  return DEF_ID_INVALID;
}
