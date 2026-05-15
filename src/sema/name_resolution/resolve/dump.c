#include "dump.h"

#include <stdio.h>

#include "db/storage/stringpool.h"
#include "db/storage/vec.h"
#include "parser/ast.h"
#include "../modules/def_map.h"
#include "../modules/modules.h"
#include "../scope/scope.h"
#include "../sema.h"
#include "resolve.h"

// Recursive Ident walker — for every expr_Ident in the tree, runs
// query_resolve_ref and prints the outcome. Type-position children get
// NS_TYPE; everything else NS_VALUE. Effect-position resolution is left
// for a follow-up — the layer doesn't yet know which Idents sit there.
static void walk(struct Sema *s, struct Expr *e, Namespace ns, int depth);

static void walk_vec(struct Sema *s, Vec *v, Namespace ns, int depth) {
  if (!v)
    return;
  for (size_t i = 0; i < v->count; i++) {
    struct Expr **slot = (struct Expr **)vec_get(v, i);
    if (slot)
      walk(s, *slot, ns, depth);
  }
}

static const char *ns_name(Namespace ns) {
  switch (ns) {
  case NS_VALUE:
    return "val";
  case NS_TYPE:
    return "type";
  case NS_EFFECT:
    return "eff";
  case NS_OP:
    return "op";
  case NS_VALUE_OR_TYPE:
    return "val|type";
  }
  return "?";
}

static void walk(struct Sema *s, struct Expr *e, Namespace ns, int depth) {
  if (!e)
    return;

  switch (e->kind) {
  case expr_Ident: {
    DefId def = query_resolve_ref(s, e, ns);
    const char *name = pool_get(&s->pool, e->ident.string_id, 0);
    printf("  %*s%s/%s -> def=%u\n", depth * 2, "", name ? name : "?",
           ns_name(ns), def.idx);
    return;
  }
  case expr_Bind:
    if (e->bind.type_ann)
      walk(s, e->bind.type_ann, NS_TYPE, depth);
    if (e->bind.value)
      walk(s, e->bind.value, NS_VALUE, depth);
    return;
  case expr_Bin:
    walk(s, e->bin.Left, ns, depth);
    walk(s, e->bin.Right, ns, depth);
    return;
  case expr_Assign:
    walk(s, e->assign.target, NS_VALUE, depth);
    walk(s, e->assign.value, NS_VALUE, depth);
    return;
  case expr_Unary:
    walk(s, e->unary.operand, ns, depth);
    return;
  case expr_Call:
    walk(s, e->call.callee, NS_VALUE, depth);
    walk_vec(s, e->call.args, NS_VALUE, depth);
    return;
  case expr_Builtin:
    walk_vec(s, e->builtin.args, NS_VALUE, depth);
    return;
  case expr_If:
    walk(s, e->if_expr.condition, NS_VALUE, depth);
    walk(s, e->if_expr.then_branch, NS_VALUE, depth);
    walk(s, e->if_expr.else_branch, NS_VALUE, depth);
    return;
  case expr_Block:
    walk_vec(s, e->block.stmts, NS_VALUE, depth);
    return;
  case expr_Field:
    // Only the root object is resolved; the field name is a member
    // selector, not a free identifier — that's a path-resolve concern.
    walk(s, e->field.object, ns, depth);
    return;
  case expr_Index:
    walk(s, e->index.object, NS_VALUE, depth);
    walk(s, e->index.index, NS_VALUE, depth);
    return;
  case expr_Slice:
    walk(s, e->slice.object, NS_VALUE, depth);
    walk(s, e->slice.start, NS_VALUE, depth);
    walk(s, e->slice.end, NS_VALUE, depth);
    return;
  case expr_Lambda:
    if (e->lambda.params) {
      for (size_t i = 0; i < e->lambda.params->count; i++) {
        struct Param *p = (struct Param *)vec_get(e->lambda.params, i);
        if (p && p->type_ann)
          walk(s, p->type_ann, NS_TYPE, depth);
      }
    }
    if (e->lambda.ret_type)
      walk(s, e->lambda.ret_type, NS_TYPE, depth);
    if (e->lambda.body)
      walk(s, e->lambda.body, NS_VALUE, depth);
    return;
  case expr_Switch:
    walk(s, e->switch_expr.scrutinee, NS_VALUE, depth);
    if (e->switch_expr.arms) {
      for (size_t i = 0; i < e->switch_expr.arms->count; i++) {
        struct SwitchArm *a =
            (struct SwitchArm *)vec_get(e->switch_expr.arms, i);
        if (!a)
          continue;
        walk_vec(s, a->patterns, NS_VALUE, depth);
        walk(s, a->body, NS_VALUE, depth);
      }
    }
    return;
  case expr_Loop:
    walk(s, e->loop_expr.init, NS_VALUE, depth);
    walk(s, e->loop_expr.condition, NS_VALUE, depth);
    walk(s, e->loop_expr.step, NS_VALUE, depth);
    walk(s, e->loop_expr.body, NS_VALUE, depth);
    return;
  case expr_ArrayType:
    walk(s, e->array_type.size, NS_VALUE, depth);
    walk(s, e->array_type.elem, NS_TYPE, depth);
    return;
  case expr_SliceType:
    walk(s, e->slice_type.elem, NS_TYPE, depth);
    return;
  case expr_ManyPtrType:
    walk(s, e->many_ptr_type.elem, NS_TYPE, depth);
    return;
  case expr_ArrayLit:
    walk(s, e->array_lit.size, NS_VALUE, depth);
    walk(s, e->array_lit.elem_type, NS_TYPE, depth);
    walk(s, e->array_lit.initializer, NS_VALUE, depth);
    return;
  case expr_Struct:
    if (e->struct_expr.members) {
      for (size_t i = 0; i < e->struct_expr.members->count; i++) {
        struct StructMember *m =
            (struct StructMember *)vec_get(e->struct_expr.members, i);
        if (!m)
          continue;
        if (m->kind == member_Field && m->field.type)
          walk(s, m->field.type, NS_TYPE, depth);
      }
    }
    return;
  case expr_Product:
    if (e->product.type_expr)
      walk(s, e->product.type_expr, NS_TYPE, depth);
    if (e->product.Fields) {
      for (size_t i = 0; i < e->product.Fields->count; i++) {
        struct ProductField *f =
            (struct ProductField *)vec_get(e->product.Fields, i);
        if (f && f->value)
          walk(s, f->value, NS_VALUE, depth);
      }
    }
    return;
  default:
    return;
  }
}

void dump_resolve(struct Sema *s, ModuleId mid) {
  struct ModuleInfo *m = module_info(s, mid);
  if (!m) {
    printf("=== resolve === <invalid module>\n");
    return;
  }
  Vec *idx = query_top_level_index(s, mid);
  // Surface the structural fingerprints alongside the module info.
  // These are the hashes downstream queries (resolve_ref, future
  // path_resolve, importers) consume for early cutoff. Stable across
  // body-only edits; shifts only when top-level shape changes.
  printf("=== resolve === module=%u top_level=%zu "
         "def_map_fp=0x%016llx exports_fp=0x%016llx\n",
         mid.idx, idx ? idx->count : 0,
         (unsigned long long)m->def_map_query.fingerprint,
         (unsigned long long)m->exports_query.fingerprint);
  if (!idx)
    return;
  for (size_t i = 0; i < idx->count; i++) {
    struct TopLevelEntry *e = (struct TopLevelEntry *)vec_get(idx, i);
    if (!e)
      continue;
    DefId def = query_def_for_name(s, mid, e->name_id);
    const char *name = pool_get(&s->pool, e->name_id, 0);
    printf("[%zu] %-20s vis=%d def=%u\n", i, name ? name : "?", (int)e->vis,
           def.idx);
    if (e->node)
      walk(s, e->node, NS_VALUE, 1);
  }
}
