#include "def_identity.h"
#include "../../parser/ast.h"
#include "../db.h"
#include "../workspace/ast_id_map.h"
#include "invalidate.h"
#include "query.h"
#include "query_engine.h"

// Resolve (mid, ast_id) to the file + AstNodeId of the binding by
// walking the module's files' AstIdMaps. Returns file_id_local index
// in *out_local and the AstNodeId in *out_node; returns false if the
// AstId doesn't appear in any of the module's files.
static bool resolve_ast_id_in_module(struct db *s, ModuleId mid, AstId ast_id,
                                     uint32_t *out_local, AstNodeId *out_node) {
  uint32_t fc = 0;
  const FileId *files = db_get_module_files(s, mid, &fc);
  for (uint32_t i = 0; i < fc; i++) {
    uint32_t local = file_id_local(files[i]);
    struct AstIdMap *map =
        *(struct AstIdMap **)vec_get(&s->files.ast_id_maps, local);
    if (!map)
      continue;
    AstNodeId node = ast_id_map_get(map, ast_id);
    if (node.idx != 0) {
      if (out_local)
        *out_local = local;
      if (out_node)
        *out_node = node;
      return true;
    }
  }
  return false;
}

// db_query_def_identity — canonical (mid, ast_id) → DefId materialization.
//
// Stable identity: the DefIdentityEntry persists across module_exports
// re-runs, so the same DefId is returned for the same (mid, ast_id) for
// the life of the db. On re-run, the slot's fingerprint is recomputed
// against the (possibly refreshed) AST shape; if it matches the prior
// stamp, downstream queries early-cut on dep-walk.
//
// On first call, allocates the DefId via db_create_def, resolves the
// owning file via the module's AstIdMaps, reads the binding's extras to
// recover (name, meta), and fills the db.defs.* identity columns.
DefId db_query_def_identity(struct db *s, ModuleId mid, AstId ast_id) {
  // Pack the (mid, ast_id) key into a stable u64. Stack address is
  // fine — db_query_begin reads the value through the pointer at call
  // time; the slot itself lives in the HashMap entry (pointer-stable).
  uint64_t k = ((uint64_t)mid.idx << 32) | (uint64_t)ast_id.idx;

  // Get-or-create the HashMap entry BEFORE calling db_query_begin so
  // db_locate_slot has something to return. arena-backed so the slot's
  // address is stable for the db's lifetime.
  DefIdentityEntry *entry =
      (DefIdentityEntry *)hashmap_get(&s->def_by_identity, k);
  if (!entry) {
    entry =
        (DefIdentityEntry *)arena_alloc(&s->arena, sizeof(DefIdentityEntry));
    *entry = (DefIdentityEntry){.key = k, .def = DEF_ID_NONE, .slot = {0}};
    hashmap_put_or_die(&s->def_by_identity, k, entry, "def_by_identity");
  }

  DB_QUERY_GUARD(s, QUERY_DEF_IDENTITY, &k, entry->def, DEF_ID_NONE,
                 DEF_ID_NONE);

  // First-allocation path: assign the canonical DefId.
  if (entry->def.idx == DEF_ID_NONE.idx) {
    entry->def = db_create_def(s);
    *(ModuleId *)vec_get(&s->defs.parent_modules, entry->def.idx) = mid;
    *(AstId *)vec_get(&s->defs.ast_ids, entry->def.idx) = ast_id;
  }

  // Resolve (mid, ast_id) → AST node. The node IS a top-level bind
  // (AST_DECL_CONST or AST_DECL_VAR; destructures aren't in
  // TopLevelEntry so they shouldn't reach here).
  uint32_t local = 0;
  AstNodeId node = AST_NODE_ID_NONE;
  if (!resolve_ast_id_in_module(s, mid, ast_id, &local, &node)) {
    db_query_fail(s, QUERY_DEF_IDENTITY, &k);
    return DEF_ID_NONE;
  }

  ASTStore *ast = *(ASTStore **)vec_get(&s->files.asts, local);
  AstNodeKind k_node = ((AstNodeKind *)ast->kinds.data)[node.idx];
  AstNodeData d_node = ((AstNodeData *)ast->data.data)[node.idx];

  StrId name = {0};
  DefMeta meta = 0;
  DefKind kind = KIND_NONE;
  if (k_node == AST_DECL_CONST || k_node == AST_DECL_VAR) {
    // Extras layout: [name_strid, type_id, value_id, meta].
    uint32_t *ex = &((uint32_t *)ast->extra.data)[d_node.extra_idx.idx];
    name = (StrId){ex[0]};
    meta = (DefMeta)ex[3];

    // Classify the def syntactically from the RHS expression's node
    // kind — no type inference needed. This must happen here, before
    // any per-kind query runs: db_locate_slot routes on kinds[def].
    AstNodeId value_id = {.idx = ex[2]};
    if (value_id.idx != AST_NODE_ID_NONE.idx) {
      switch (((AstNodeKind *)ast->kinds.data)[value_id.idx]) {
      case AST_DECL_STRUCT:
        kind = KIND_STRUCT;
        break;
      case AST_DECL_UNION:
        kind = KIND_UNION;
        break;
      case AST_DECL_ENUM:
        kind = KIND_ENUM;
        break;
      case AST_DECL_EFFECT:
        kind = KIND_EFFECT;
        break;
      case AST_EXPR_LAMBDA:
        kind = KIND_FUNCTION;
        break;
      case AST_EXPR_HANDLER:
        kind = KIND_HANDLER;
        break;
      default:
        break; // a plain value RHS → variable / constant
      }
    }
    if (kind == KIND_NONE)
      kind = (k_node == AST_DECL_VAR) ? KIND_VARIABLE : KIND_CONSTANT;
  }

  // Fill identity columns. parent_modules + ast_ids were stamped at
  // alloc time above; the rest get filled (idempotently — same values
  // on every re-run, so the fingerprint below stays stable).
  *(StrId *)vec_get(&s->defs.names, entry->def.idx) = name;
  *(DefMeta *)vec_get(&s->defs.meta, entry->def.idx) = meta;
  // Classify (idempotent — db_def_set_kind early-returns once a def's
  // kind is fixed, so re-runs of this query are no-ops here).
  if (kind != KIND_NONE)
    db_def_set_kind(s, entry->def, kind);

  Fingerprint fp = db_fp_u64((uint64_t)name.idx);
  fp = db_fp_combine(fp, db_fp_u64((uint64_t)ast_id.idx));
  fp = db_fp_combine(fp, db_fp_u64((uint64_t)meta));

  db_query_succeed(s, QUERY_DEF_IDENTITY, &k, fp);
  return entry->def;
}
