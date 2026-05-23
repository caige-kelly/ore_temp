#include "position.h"

#include <stddef.h>

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
static struct Expr *find_innermost(struct Expr *e, uint32_t line, uint32_t col);

static struct Expr *try_child(struct Expr *e, uint32_t line, uint32_t col) {
  return find_innermost(e, line, col);
}

#define TRY(child)                                                             \
  do {                                                                         \
    struct Expr *__r = try_child((child), line, col);                          \
    if (__r)                                                                   \
      return __r;                                                              \
  } while (0)

static struct Expr *find_innermost(struct Expr *e, uint32_t line,
                                   uint32_t col) {
  if (!e)
    return NULL;
  if (!span_contains(e->span, line, col))
    return NULL;

  switch (e->kind) {
  case expr_Lit:
  case expr_Wildcard:
  case expr_Break:
  case expr_Continue:
  case expr_Ident:
  case expr_EnumRef:
  case expr_Asm:
    return e;

  case expr_Bin:
    TRY(e->bin.Left);
    TRY(e->bin.Right);
    return e;

  case expr_Assign:
    TRY(e->assign.target);
    TRY(e->assign.value);
    return e;

  case expr_Unary:
    TRY(e->unary.operand);
    return e;

  case expr_Call:
    TRY(e->call.callee);
    if (e->call.args) {
      for (size_t i = 0; i < e->call.args->count; i++) {
        struct Expr **slot = (struct Expr **)vec_get(e->call.args, i);
        if (slot)
          TRY(*slot);
      }
    }
    return e;

  case expr_Builtin:
    if (e->builtin.args) {
      for (size_t i = 0; i < e->builtin.args->count; i++) {
        struct Expr **slot = (struct Expr **)vec_get(e->builtin.args, i);
        if (slot)
          TRY(*slot);
      }
    }
    return e;

  case expr_If:
    TRY(e->if_expr.condition);
    TRY(e->if_expr.then_branch);
    TRY(e->if_expr.else_branch);
    return e;

  case expr_Block:
    if (e->block.stmts) {
      for (size_t i = 0; i < e->block.stmts->count; i++) {
        struct Expr **slot = (struct Expr **)vec_get(e->block.stmts, i);
        if (slot)
          TRY(*slot);
      }
    }
    return e;

  case expr_Bind:
    TRY(e->bind.type_ann);
    TRY(e->bind.value);
    return e;

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
    return e;

  case expr_Loop:
    TRY(e->loop_expr.init);
    TRY(e->loop_expr.condition);
    TRY(e->loop_expr.step);
    TRY(e->loop_expr.body);
    return e;

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
            struct Expr **slot = (struct Expr **)vec_get(arm->patterns, j);
            if (slot)
              TRY(*slot);
          }
        }
        TRY(arm->body);
      }
    }
    return e;

  case expr_Field:
    TRY(e->field.object);
    return e;

  case expr_Index:
    TRY(e->index.object);
    TRY(e->index.index);
    return e;

  case expr_Slice:
    TRY(e->slice.object);
    TRY(e->slice.start);
    TRY(e->slice.end);
    return e;

  case expr_Return:
    TRY(e->return_expr.value);
    return e;

  case expr_Defer:
    TRY(e->defer_expr.value);
    return e;

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
    return e;

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
    return e;

  case expr_Mask:
    TRY(e->mask.body);
    return e;

  case expr_DestructureBind:
    TRY(e->destructure.pattern);
    TRY(e->destructure.value);
    return e;

  case expr_ArrayLit:
    TRY(e->array_lit.size);
    TRY(e->array_lit.elem_type);
    TRY(e->array_lit.initializer);
    return e;

  case expr_SliceType:
    TRY(e->slice_type.elem);
    return e;

  case expr_ManyPtrType:
    TRY(e->many_ptr_type.elem);
    return e;

  case expr_ArrayType:
    TRY(e->array_type.size);
    TRY(e->array_type.elem);
    return e;

  case expr_FnType:
    if (e->fn_type.param_types) {
      for (size_t i = 0; i < e->fn_type.param_types->count; i++) {
        struct Expr **slot = (struct Expr **)vec_get(e->fn_type.param_types, i);
        TRY(slot ? *slot : NULL);
      }
    }
    TRY(e->fn_type.ret_type);
    return e;

  case expr_Struct:
  case expr_Enum:
  case decl_Effect:
  case expr_EffectRow:
  case expr_Ctl:
    return e;
  }
  return e;
}

#undef TRY

// Internal helper: find the innermost AST Expr at (line, col). Walks
// the top-level vec, dispatching to find_innermost on each. Returns
// NULL when nothing matches.
static struct Expr *find_expr_at_position(struct Sema *s, NamespaceId nsid,
                                          uint32_t line, uint32_t col) {
  Vec *ast = query_module_ast(s, nsid);
  if (!ast)
    return NULL;

  // Linear scan over top-level expressions; the recursive descent
  // inside each handles nesting. For thousands-of-decls modules,
  // swap this for the binary-search-on-sorted-spans approach noted
  // in position.h's docs.
  for (size_t i = 0; i < ast->count; i++) {
    struct Expr **slot = (struct Expr **)vec_get(ast, i);
    struct Expr *e = slot ? *slot : NULL;
    if (!e)
      continue;
    struct Expr *hit = find_innermost(e, line, col);
    if (hit)
      return hit;
  }
  return NULL;
}

struct NodeId query_node_at_position(struct Sema *s, NamespaceId nsid,
                                     uint32_t line, uint32_t col) {
  struct Expr *hit = find_expr_at_position(s, nsid, line, col);
  return hit ? hit->id : (struct NodeId){0};
}

DefId query_def_at_position(struct Sema *s, NamespaceId nsid, uint32_t line,
                            uint32_t col) {
  // Post-R8: walk the AST directly to find the Expr — no
  // node_to_expr indirection. The walker has the Expr in hand at
  // every step; returning it from find_innermost cuts out an
  // O(N) HashMap lookup.
  struct Expr *e = find_expr_at_position(s, nsid, line, col);
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
