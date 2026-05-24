#include "../db/db.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/decl_ast.h"
#include "../db/query/def_identity.h"
#include "../db/query/fn_signature.h"
#include "../db/query/index.h"
#include "../db/query/type_of_def.h"
#include "../db/storage/stringpool.h"
#include "../parser/ast.h"
#include "sema.h"

#include <stdlib.h>

// Build a struct (or union, treated as struct in chunk 3) type from an
// AST_DECL_STRUCT / AST_DECL_UNION node. Uses ip_wip_struct to allocate
// the IpIndex up-front; pointer-cycle support is wired by publishing
// wip.index into the def's per-kind type cell BEFORE the field loop
// runs. The salsa cycle-return path in db_query_type_of_def reads that
// cell, so a recursive type_of_def(self) call during the field loop
// returns the wip's IpIndex instead of the IP_NONE cycle-default. This
// is what makes `Node :: struct { next : ?^Node }` and mutual-
// reference cases like Header/Page type correctly.
//
// zir_node_id = def.idx — nominal identity keyed by the declaring def,
// so two structurally-identical struct decls at different sites get
// distinct IpIndex values.
//
// Scratch arrays come from db.request_arena (reset at db_request_end),
// so they grow without a fixed cap and don't burn stack.
static IpIndex build_struct_type(struct db *s, ASTStore *ast,
                                 AstNodeId aggregate_node, DefId def,
                                 NamespaceId nsid, FileId file_local) {
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

  // Publish the wip's IpIndex into the def's type cell BEFORE the field
  // loop runs. This is the load-bearing change that unblocks self-
  // referential and mutually-referential struct types: when a field's
  // type-expr like `^Self` recurses back into db_query_type_of_def(def),
  // the salsa cycle path now reads this cell (via the inline cycle
  // handler in db_query_type_of_def) and returns wip.index instead of
  // the IP_NONE cycle-default. db_def_set_kind is idempotent — if
  // def_identity already classified the def (the common path), this is
  // a no-op; if not (only when build_struct_type is reached without a
  // def_identity prefix, which shouldn't happen but defend anyway), it
  // sets KIND_STRUCT so db_def_type_cell is routable.
  if (db_def_kind(s, def) == KIND_NONE)
    db_def_set_kind(s, def, KIND_STRUCT);
  *db_def_type_cell(s, def) = wip.index;

  for (uint32_t i = 0; i < n_fields; i++) {
    AstNodeId field_id = {.idx = ex[1 + i]};
    if (field_id.idx == AST_NODE_ID_NONE.idx) {
      ip_wip_struct_cancel(&s->intern, wip);
      *db_def_type_cell(s, def) = IP_NONE;
      return IP_NONE;
    }
    AstNodeKind fk = ((AstNodeKind *)ast->kinds.data)[field_id.idx];
    if (fk != AST_DECL_FIELD) {
      ip_wip_struct_cancel(&s->intern, wip);
      *db_def_type_cell(s, def) = IP_NONE;
      return IP_NONE;
    }
    // Field extras: [name_strid (0=anon), type, vis, fpos (0=auto)].
    AstNodeData fd = ((AstNodeData *)ast->data.data)[field_id.idx];
    const uint32_t *fex = &((uint32_t *)ast->extra.data)[fd.extra_idx.idx];
    StrId fname = {.idx = fex[0]};
    AstNodeId ftype = {.idx = fex[1]};
    IpIndex ftypei = sema_resolve_type_expr(s, ast, ftype, nsid, file_local);
    if (ftypei.v == IP_NONE.v) {
      // Any unresolved field type fails the whole struct. Cleaner than
      // emitting a malformed nominal type; downstream treats IP_NONE
      // as "not yet known."
      ip_wip_struct_cancel(&s->intern, wip);
      *db_def_type_cell(s, def) = IP_NONE;
      return IP_NONE;
    }
    names[i] = fname;
    types[i] = ftypei;
    // Stamp the AST_DECL_FIELD node itself with the field's type so
    // hover on the field-name token reads it from the cache without
    // re-running sema_resolve_type_expr (the L2 universal-cache pillar).
    sema_cache_node_type(s, file_local, field_id, ftypei);
  }

  ip_wip_struct_finish(&s->intern, wip, names, types, n_fields);
  // wip.index is now backed by real field data; cell already points at
  // it, no second write needed (the wrapper in db_query_type_of_def
  // will write the same value when sema_type_of_def returns — harmless).
  return wip.index;
}

// Build an enum type from an AST_DECL_ENUM node. No wip API for enums —
// ip_get with IPK_ENUM_TYPE is sufficient since enums don't have the
// self-referential pattern that structs do. Variant values: auto-numbered
// (0..N-1) or bare-literal-int only; computed/expression values need
// chunk 6 const_eval.
static IpIndex build_enum_type(struct db *s, ASTStore *ast, AstNodeId enum_node,
                               DefId def) {
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
                             .n_variants = n_variants},
               // names/values are borrowed from request_arena — stamp it
               // so ip_get can assert consumption before the next reset.
               .src_arena = n_variants ? &s->request_arena : NULL,
               .src_gen = s->request_arena.generation};
  return ip_get(&s->intern, key);
}

// Post-typecheck file walker — re-stamps FileNodeData.types[] for every
// AST node that has a known IpIndex, even when the per-decl salsa
// queries early-cut and skip their impl (and thus the cache writes that
// happen inside those impls).
//
// Why this exists: parser.c re-allocates FileNodeData on every reparse,
// zero-initialising types[] to IP_NONE. The cache writes that populate
// non-trivial entries live INSIDE salsa-cached query impls
// (sema_type_of_def → build_struct_type, sema_build_fn_type, etc.) — so
// when sibling-decl edits leave THIS decl unchanged, the query early-
// cuts, the impl doesn't run, and the cache stays zero. Reading hover
// then shows `?` for an otherwise valid struct field.
//
// This walker re-runs unconditionally from sema_check_module after the
// per-decl loop completes. Its work is salsa-cache-hit-bounded: each
// db_query_type_of_def / sema_resolve_type_expr call short-cuts on a
// stale-but-valid slot. No new salsa state; pure side-effect on
// FileNodeData.types[].
void sema_stamp_file_types(struct db *s, FileId fid) {
  if (!file_id_valid(fid))
    return;
  uint32_t local = file_id_local(fid);
  if (local >= s->files.top_level_indices.count)
    return;

  ASTStore *ast = db_get_file_ast(s, fid);
  if (!ast)
    return;
  NamespaceId nsid = db_get_file_namespace(s, fid);
  if (!namespace_id_valid(nsid))
    return;

  FileArray *idx = (FileArray *)vec_get(&s->files.top_level_indices, local);
  for (size_t i = 0; i < idx->count; i++) {
    TopLevelEntry *e = &((TopLevelEntry *)idx->data)[i];
    AstNodeId decl_node = e->node;

    // 1. Top-level decl's type (cheap salsa cache hit).
    DefId def = db_query_def_identity(s, nsid, e->ast_id);
    IpIndex dt = db_query_type_of_def(s, def);
    sema_cache_node_type(s, fid, decl_node, dt);

    // 2. Drill into sub-shapes whose nodes the per-decl impl would have
    //    stamped — but only IF the impl ran. Calling sema_resolve_type_expr
    //    here re-walks the type-exprs; the recursive cache writes inside
    //    the resolver re-populate types[] for the WHOLE sub-tree (struct
    //    field type-exprs, fn-param type-exprs, AST_EXPR_PATH targets,
    //    AST_TYPE_* constructors).
    AstNodeData dd = ((AstNodeData *)ast->data.data)[decl_node.idx];
    AstNodeKind dk = ((AstNodeKind *)ast->kinds.data)[decl_node.idx];
    if (dk != AST_DECL_CONST && dk != AST_DECL_VAR)
      continue;
    const uint32_t *dex = &((uint32_t *)ast->extra.data)[dd.extra_idx.idx];
    AstNodeId value_id = {.idx = dex[2]};
    if (value_id.idx == AST_NODE_ID_NONE.idx)
      continue;
    AstNodeKind vk = ((AstNodeKind *)ast->kinds.data)[value_id.idx];

    if (vk == AST_DECL_STRUCT || vk == AST_DECL_UNION) {
      // Struct fields: extras [n_fields, f0, f1, ...]. For each field,
      // resolve its type-expr (recursive cache writes) AND stamp the
      // field node itself with that resolved type.
      AstNodeData vd = ((AstNodeData *)ast->data.data)[value_id.idx];
      if (vd.extra_idx.idx == 0)
        continue;
      const uint32_t *vex = &((uint32_t *)ast->extra.data)[vd.extra_idx.idx];
      uint32_t n_fields = vex[0];
      for (uint32_t fi = 0; fi < n_fields; fi++) {
        AstNodeId field_id = {.idx = vex[1 + fi]};
        if (field_id.idx == AST_NODE_ID_NONE.idx)
          continue;
        AstNodeKind fk = ((AstNodeKind *)ast->kinds.data)[field_id.idx];
        if (fk != AST_DECL_FIELD)
          continue;
        AstNodeData fd = ((AstNodeData *)ast->data.data)[field_id.idx];
        const uint32_t *fex = &((uint32_t *)ast->extra.data)[fd.extra_idx.idx];
        AstNodeId ftype = {.idx = fex[1]};
        IpIndex ftypei = sema_resolve_type_expr(s, ast, ftype, nsid, fid);
        sema_cache_node_type(s, fid, field_id, ftypei);
      }
    } else if (vk == AST_EXPR_LAMBDA) {
      // Lambda params: extras [ret_id, effect_id, n_params, p0, p1, ...].
      // Mirror sema_build_fn_type's param loop so we re-stamp every
      // param node + its type-expr sub-tree.
      AstNodeData ld = ((AstNodeData *)ast->data.data)[value_id.idx];
      const uint32_t *lex = &((uint32_t *)ast->extra.data)[ld.extra_idx.idx];
      AstNodeId ret_node = {.idx = lex[0]};
      uint32_t n_params = lex[3];
      for (uint32_t pi = 0; pi < n_params; pi++) {
        AstNodeId param_id = {.idx = lex[4 + pi]};
        if (param_id.idx == AST_NODE_ID_NONE.idx)
          continue;
        AstNodeKind pk = ((AstNodeKind *)ast->kinds.data)[param_id.idx];
        if (pk != AST_DECL_PARAM)
          continue;
        AstNodeData pd = ((AstNodeData *)ast->data.data)[param_id.idx];
        const uint32_t *pex = &((uint32_t *)ast->extra.data)[pd.extra_idx.idx];
        AstNodeId ptype = {.idx = pex[1]};
        IpIndex pti = sema_resolve_type_expr(s, ast, ptype, nsid, fid);
        sema_cache_node_type(s, fid, param_id, pti);
      }
      // Return-type-expr too.
      if (ret_node.idx != AST_NODE_ID_NONE.idx)
        (void)sema_resolve_type_expr(s, ast, ret_node, nsid, fid);
    }
  }
}

IpIndex sema_type_of_def(struct db *s, DefId def) {
  AstId ast_id = *(AstId *)vec_get(&s->defs.ast_ids, def.idx);
  NamespaceId nsid = *(NamespaceId *)vec_get(&s->defs.parent_modules, def.idx);

  // Depend on the module's top-level index: this body reads the module's
  // file list (db_get_namespace_files) below, so a file-set change must
  // re-run it. db_query_def_identity's own dep does not cover this — its
  // fingerprint is membership-insensitive by construction.
  (void)db_query_top_level_index(s, nsid);
  (void)db_query_def_identity(s, nsid, ast_id);

  IpIndex result = IP_NONE;
  uint32_t fc = 0;
  const FileId *files = db_get_namespace_files(s, nsid, &fc);
  for (uint32_t i = 0; i < fc; i++) {
    // Per-decl AST dep: a sibling decl's edit reproduces this query's
    // fingerprint, so this query early-cuts instead of recomputing.
    AstNodeId node = db_query_decl_ast(s, files[i], ast_id);
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
        result = build_struct_type(s, ast, value_id, def, nsid, files[i]);
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
      result = sema_resolve_type_expr(s, ast, type_id, nsid, files[i]);
      break;
    }

    if (value_id.idx == AST_NODE_ID_NONE.idx)
      break;

    // Inferred bind: type comes from the value expression. db_type_of_expr
    // covers literals, identifier paths, and binops (chunks 5a/5c).
    // For top-level decls we're not inside a fn, so enclosing_fn is NONE.
    result = sema_type_of_expr(s, ast, value_id, nsid, DEF_ID_NONE, files[i]);
    break;
  }

  return result;
}
