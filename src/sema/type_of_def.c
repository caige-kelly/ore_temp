#include "../db/query/type_of_def.h"
#include "../db/db.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/decl_ast.h"
#include "../db/query/def_identity.h"
#include "../db/query/fn_signature.h"
#include "../db/query/index.h"
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

  // Open a NodeTypeBuilder over the struct's field sub-tree (every
  // AST_DECL_FIELD + its type-expr descendants). sema_resolve_type_expr
  // recursive calls push every visited node's resolved type into the
  // active builder; the assembled NodeTypesRange lands on
  // db.structs.field_node_types[struct_row] for the unified node_type
  // router to read.
  uint32_t f_min = UINT32_MAX, f_max = 0;
  for (uint32_t i = 0; i < n_fields; i++) {
    AstNodeId fid = {.idx = ex[1 + i]};
    if (fid.idx == AST_NODE_ID_NONE.idx)
      continue;
    uint32_t fmin = 0, fmax = 0;
    sema_ast_subtree_range(ast, fid, &fmin, &fmax);
    if (fmin < f_min)
      f_min = fmin;
    if (fmax > f_max)
      f_max = fmax;
  }
  if (f_min == UINT32_MAX) {
    f_min = 0;
    f_max = 0;
  }

  NodeTypeBuilder fields_b;
  sema_node_type_builder_begin(s, &fields_b, file_local, f_min, f_max);
  SemaCtx fields_ctx = {
      .s = s,
      .ast = ast,
      .nsid = nsid,
      .enclosing_fn = DEF_ID_NONE, // struct fields are outside any fn body
      .file_local = file_local,
      .types = &fields_b,
  };

  IpIndex final_result = IP_NONE;
  bool cancelled = false;
  for (uint32_t i = 0; i < n_fields; i++) {
    AstNodeId field_id = {.idx = ex[1 + i]};
    if (field_id.idx == AST_NODE_ID_NONE.idx) {
      ip_wip_struct_cancel(&s->intern, wip);
      *db_def_type_cell(s, def) = IP_NONE;
      cancelled = true;
      break;
    }
    AstNodeKind fk = ((AstNodeKind *)ast->kinds.data)[field_id.idx];
    if (fk != AST_DECL_FIELD) {
      ip_wip_struct_cancel(&s->intern, wip);
      *db_def_type_cell(s, def) = IP_NONE;
      cancelled = true;
      break;
    }
    // Field extras: [name_strid (0=anon), type, vis, fpos (0=auto)].
    AstNodeData fd = ((AstNodeData *)ast->data.data)[field_id.idx];
    const uint32_t *fex = &((uint32_t *)ast->extra.data)[fd.extra_idx.idx];
    StrId fname = {.idx = fex[0]};
    AstNodeId ftype = {.idx = fex[1]};
    IpIndex ftypei = sema_resolve_type_expr(&fields_ctx, ftype);
    if (ftypei.v == IP_NONE.v) {
      // Any unresolved field type fails the whole struct. Cleaner than
      // emitting a malformed nominal type; downstream treats IP_NONE
      // as "not yet known."
      ip_wip_struct_cancel(&s->intern, wip);
      *db_def_type_cell(s, def) = IP_NONE;
      cancelled = true;
      break;
    }
    names[i] = fname;
    types[i] = ftypei;
    // Stamp the AST_DECL_FIELD node itself with the field's type
    // into the fields_ctx builder, so hover on the field-name token
    // reads from db.node_types_pool via the unified router.
    sema_node_type_builder_push(&fields_ctx, field_id, ftypei);
  }

  if (!cancelled) {
    ip_wip_struct_finish(&s->intern, wip, names, types, n_fields);
    // wip.index is now backed by real field data; cell already points at
    // it, no second write needed (the wrapper in db_query_type_of_def
    // will write the same value when sema_type_of_def returns — harmless).
    final_result = wip.index;
  }

  // Close the field-types builder regardless of cancel/success and
  // stash the assembled range on the struct's per-kind column. Even
  // on cancel we store the range — partial writes for fields that DID
  // resolve are still valid lookups for those nodes.
  NodeTypesRange field_range = sema_node_type_builder_end(&fields_b, NULL);
  if (db_def_kind(s, def) == KIND_STRUCT) {
    uint32_t row = db_def_row(s, def, KIND_STRUCT);
    *(NodeTypesRange *)vec_get(&s->structs.field_node_types, row) = field_range;
  }

  return final_result;
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

// (sema_stamp_file_types removed 2026-05-24 — Option-C migration.
//  Per-decl salsa queries now own their own NodeTypesRange in
//  db.node_types_pool; the unified node_type router reads from those
//  ranges directly. No post-typecheck walker needed.)

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

    AstNodeId type_id = {.idx = ex[1]};
    bool has_annotation = (ex[1] != 0);
    bool has_value = (value_id.idx != AST_NODE_ID_NONE.idx);
    if (!has_annotation && !has_value)
      break;

    // Compute the builder's subtree bound — covers the annotation node
    // (if present) and/or the value node (if present), so a single
    // builder can serve both sema_* calls below.
    uint32_t v_min = UINT32_MAX, v_max = 0;
    if (has_annotation) {
      uint32_t a_min, a_max;
      sema_ast_subtree_range(ast, type_id, &a_min, &a_max);
      if (a_min < v_min)
        v_min = a_min;
      if (a_max > v_max)
        v_max = a_max;
    }
    if (has_value) {
      uint32_t vv_min, vv_max;
      sema_ast_subtree_range(ast, value_id, &vv_min, &vv_max);
      if (vv_min < v_min)
        v_min = vv_min;
      if (vv_max > v_max)
        v_max = vv_max;
    }

    // Open a NodeTypeBuilder so per-node types in the annotation and/or
    // value subtree get stamped into db.node_types_pool. The unified
    // node_type router reads from db.{constants,variables}.value_node_types
    // for hover on sub-nodes of the bind.
    NodeTypeBuilder val_b;
    sema_node_type_builder_begin(s, &val_b, files[i], v_min, v_max);
    SemaCtx val_ctx = {.s = s,
                       .ast = ast,
                       .nsid = nsid,
                       .enclosing_fn = DEF_ID_NONE,
                       .file_local = files[i],
                       .types = &val_b};

    if (has_annotation) {
      // Typed bind: annotation wins over RHS inference.
      result = sema_resolve_type_expr(&val_ctx, type_id);
      // Drive the RHS through sema_check_expr with the annotation as
      // the expected type. This is the bidirectional path: comptime_int
      // literals (and refs to comptime defs) inside the RHS get
      // re-stamped in the per-node cache as the annotation's concrete
      // type, so hover on `100` in `foo : i32 : 100` returns i32, not
      // comptime_int. Multi-context coercion works because each use
      // site writes its own cache entry — the def's stored type is
      // not touched.
      if (has_value && result.v != IP_NONE.v)
        (void)sema_check_expr(&val_ctx, value_id, result);
    } else {
      // Inferred bind: type comes from the value expression with no
      // expected context. Literals stay comptime_int — they're the
      // def's truthful, polymorphic identity.
      result = sema_type_of_expr(&val_ctx, value_id);
    }

    NodeTypesRange v_range = sema_node_type_builder_end(&val_b, NULL);
    DefKind k = db_def_kind(s, def);
    if (k == KIND_CONSTANT) {
      uint32_t row = db_def_row(s, def, KIND_CONSTANT);
      *(NodeTypesRange *)vec_get(&s->constants.value_node_types, row) = v_range;
    } else if (k == KIND_VARIABLE) {
      uint32_t row = db_def_row(s, def, KIND_VARIABLE);
      *(NodeTypesRange *)vec_get(&s->variables.value_node_types, row) = v_range;
    }
    break;
  }

  return result;
}
