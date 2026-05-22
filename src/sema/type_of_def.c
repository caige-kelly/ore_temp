#include "../db/db.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/ast.h"
#include "../db/query/def_identity.h"
#include "../db/query/fn_signature.h"
#include "../db/storage/stringpool.h"
#include "../db/workspace/ast_id_map.h"
#include "../parser/ast.h"
#include "sema.h"

#include <stdlib.h>

// Build a struct (or union, treated as struct in chunk 3) type from an
// AST_DECL_STRUCT / AST_DECL_UNION node. Uses ip_wip_struct so the
// IpIndex is available before fields are resolved — enabling pointer
// cycles in principle, though self-referential `^Self` still bottoms
// out at IP_NONE today (the recursive type_of_def call doesn't
// trampoline back to the wip yet).
//
// zir_node_id = def.idx — nominal identity keyed by the declaring def,
// so two structurally-identical struct decls at different sites get
// distinct IpIndex values.
//
// Scratch arrays come from db.request_arena (reset at db_request_end),
// so they grow without a fixed cap and don't burn stack.
static IpIndex build_struct_type(struct db *s, ASTStore *ast,
                                 AstNodeId aggregate_node, DefId def,
                                 ModuleId mid) {
  AstNodeData ad = ((AstNodeData *)ast->data.data)[aggregate_node.idx];
  if (ad.extra_idx.idx == 0)
    return IP_NONE;
  const uint32_t *ex = &((uint32_t *)ast->extra.data)[ad.extra_idx.idx];
  uint32_t n_fields = ex[0];
  if (n_fields == 0) {
    IpKey key = {.kind = IPK_STRUCT_TYPE,
                 .struct_type = {.zir_node_id = def.idx,
                                 .field_names = NULL,
                                 .field_types = NULL,
                                 .n_fields = 0}};
    return ip_get(&s->intern, key);
  }

  StrId *names = arena_alloc(&s->request_arena, n_fields * sizeof(StrId));
  IpIndex *types = arena_alloc(&s->request_arena, n_fields * sizeof(IpIndex));
  if (!names || !types)
    return IP_NONE;
  WipContainerType wip = ip_wip_struct(&s->intern, def.idx, NULL, 0);

  for (uint32_t i = 0; i < n_fields; i++) {
    AstNodeId field_id = {.idx = ex[1 + i]};
    if (field_id.idx == AST_NODE_ID_NONE.idx) {
      ip_wip_struct_cancel(&s->intern, wip);
      return IP_NONE;
    }
    AstNodeKind fk = ((AstNodeKind *)ast->kinds.data)[field_id.idx];
    if (fk != AST_DECL_FIELD) {
      ip_wip_struct_cancel(&s->intern, wip);
      return IP_NONE;
    }
    // Field extras: [name_strid (0=anon), type, vis, fpos (0=auto)].
    AstNodeData fd = ((AstNodeData *)ast->data.data)[field_id.idx];
    const uint32_t *fex = &((uint32_t *)ast->extra.data)[fd.extra_idx.idx];
    StrId fname = {.idx = fex[0]};
    AstNodeId ftype = {.idx = fex[1]};
    IpIndex ftypei = sema_resolve_type_expr(s, ast, ftype, mid);
    if (ftypei.v == IP_NONE.v) {
      // Any unresolved field type fails the whole struct. Cleaner than
      // emitting a malformed nominal type; downstream treats IP_NONE
      // as "not yet known."
      ip_wip_struct_cancel(&s->intern, wip);
      return IP_NONE;
    }
    names[i] = fname;
    types[i] = ftypei;
  }

  ip_wip_struct_finish(&s->intern, wip, names, types, n_fields);
  return wip.index;
}

// Build an enum type from an AST_DECL_ENUM node. No wip API for enums —
// ip_get with IPK_ENUM_TYPE is sufficient since enums don't have the
// self-referential pattern that structs do. Variant values: auto-numbered
// (0..N-1) or bare-literal-int only; computed/expression values need
// chunk 6 const_eval.
static IpIndex build_enum_type(struct db *s, ASTStore *ast,
                               AstNodeId enum_node, DefId def) {
  AstNodeData ad = ((AstNodeData *)ast->data.data)[enum_node.idx];
  if (ad.extra_idx.idx == 0)
    return IP_NONE;
  const uint32_t *ex = &((uint32_t *)ast->extra.data)[ad.extra_idx.idx];
  uint32_t n_variants = ex[0];

  StrId *names = arena_alloc(&s->request_arena, n_variants * sizeof(StrId));
  int64_t *values =
      arena_alloc(&s->request_arena, n_variants * sizeof(int64_t));
  if (n_variants > 0 && (!names || !values))
    return IP_NONE;

  for (uint32_t i = 0; i < n_variants; i++) {
    AstNodeId v_id = {.idx = ex[1 + i]};
    if (v_id.idx == AST_NODE_ID_NONE.idx)
      return IP_NONE;
    AstNodeKind vk = ((AstNodeKind *)ast->kinds.data)[v_id.idx];
    if (vk != AST_DECL_VARIANT)
      return IP_NONE;
    // Variant extras: [name_strid, value_id].
    AstNodeData vd = ((AstNodeData *)ast->data.data)[v_id.idx];
    const uint32_t *vex = &((uint32_t *)ast->extra.data)[vd.extra_idx.idx];
    StrId vname = {.idx = vex[0]};
    AstNodeId v_val = {.idx = vex[1]};
    int64_t value;
    if (v_val.idx == 0) {
      value = (int64_t)i;
    } else {
      AstNodeKind v_val_k = ((AstNodeKind *)ast->kinds.data)[v_val.idx];
      if (v_val_k != AST_EXPR_LIT_INT)
        return IP_NONE;
      AstNodeData v_val_d = ((AstNodeData *)ast->data.data)[v_val.idx];
      const char *vs = pool_get(&s->strings, v_val_d.string_id);
      if (!vs)
        return IP_NONE;
      // Note: chunk 3 used strtoll. We do the same; full numeric-suffix
      // handling moves to const_eval (chunk 6).
      char *end;
      value = (int64_t)strtoll(vs, &end, 0);
    }
    names[i] = vname;
    values[i] = value;
  }

  IpKey key = {.kind = IPK_ENUM_TYPE,
               .enum_type = {.zir_node_id = def.idx,
                             .variant_names = names,
                             .variant_values = values,
                             .n_variants = n_variants}};
  return ip_get(&s->intern, key);
}

IpIndex sema_type_of_def(struct db *s, DefId def) {
  AstId ast_id = *(AstId *)vec_get(&s->defs.ast_ids, def.idx);
  ModuleId mid = *(ModuleId *)vec_get(&s->defs.parent_modules, def.idx);

  (void)db_query_def_identity(s, mid, ast_id);

  IpIndex result = IP_NONE;
  uint32_t fc = 0;
  const FileId *files = db_get_module_files(s, mid, &fc);
  for (uint32_t i = 0; i < fc; i++) {
    (void)db_query_file_ast(s, files[i]);

    struct AstIdMap *map = db_get_file_ast_id_map(s, files[i]);
    if (!map)
      continue;
    AstNodeId node = ast_id_map_get(map, ast_id);
    if (node.idx == AST_NODE_ID_NONE.idx)
      continue;

    ASTStore *ast = db_get_file_ast(s, files[i]);
    AstNodeKind dk = ((AstNodeKind *)ast->kinds.data)[node.idx];
    if (dk != AST_DECL_CONST && dk != AST_DECL_VAR)
      break;

    AstNodeData d = ((AstNodeData *)ast->data.data)[node.idx];
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    DefMeta meta = (DefMeta)ex[3];

    // Distinct binds need separate nominal-identity machinery (no
    // IPK_DISTINCT today) — chunk 8 alongside bit-packing.
    if (meta & META_DISTINCT)
      break;

    AstNodeId value_id = {.idx = ex[2]};

    // RHS-driven nominal types: an aggregate decl as the value side
    // determines the type, ignoring any `: type` annotation. Fn decls
    // delegate to db_query_fn_signature so the signature has its own
    // slot for call-site checking.
    if (value_id.idx != AST_NODE_ID_NONE.idx) {
      AstNodeKind vk = ((AstNodeKind *)ast->kinds.data)[value_id.idx];
      if (vk == AST_DECL_STRUCT || vk == AST_DECL_UNION) {
        result = build_struct_type(s, ast, value_id, def, mid);
        break;
      }
      if (vk == AST_DECL_ENUM) {
        result = build_enum_type(s, ast, value_id, def);
        break;
      }
      if (vk == AST_EXPR_LAMBDA) {
        result = db_query_fn_signature(s, def);
        break;
      }
    }

    if (ex[1] != 0) {
      // Typed bind: annotation wins over RHS inference. Coercion-
      // checking the RHS against the annotation is a chunk-5h concern.
      AstNodeId type_id = {.idx = ex[1]};
      result = sema_resolve_type_expr(s, ast, type_id, mid);
      break;
    }

    if (value_id.idx == AST_NODE_ID_NONE.idx)
      break;

    // Inferred bind: type comes from the value expression. db_type_of_expr
    // covers literals, identifier paths, and binops (chunks 5a/5c).
    // For top-level decls we're not inside a fn, so enclosing_fn is NONE.
    result = sema_type_of_expr(s, ast, value_id, mid, DEF_ID_NONE, files[i]);
    break;
  }

  return result;
}
