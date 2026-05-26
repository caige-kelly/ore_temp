#include "def_identity.h"
#include "../db.h"
#include "../../parser/syntax_kind.h"
#include "../../syntax/syntax.h"
#include "index.h"
#include "invalidate.h"
#include "query.h"
#include "query_engine.h"

#include <string.h>

// Find the TopLevelEntry in the module's files matching `node_ptr`.
// Sets *out_local to the file_local index of the file that owns the
// entry, and *out_entry to a pointer into that file's
// top_level_indices array. Returns false if no file in the module
// contains the matching node_ptr.
static bool find_top_level_entry(struct db *s, NamespaceId nsid,
                                 SyntaxNodePtr node_ptr,
                                 uint32_t *out_local,
                                 TopLevelEntry **out_entry) {
  uint32_t fc = 0;
  const FileId *files = db_get_namespace_files(s, nsid, &fc);
  for (uint32_t i = 0; i < fc; i++) {
    uint32_t local = file_id_local(files[i]);
    FileArray *idx =
        (FileArray *)vec_get(&s->files.top_level_indices, local);
    if (!idx)
      continue;
    TopLevelEntry *entries = (TopLevelEntry *)idx->data;
    for (size_t e = 0; e < idx->count; e++) {
      if (syntax_node_ptr_eq(entries[e].node_ptr, node_ptr)) {
        if (out_local)
          *out_local = local;
        if (out_entry)
          *out_entry = &entries[e];
        return true;
      }
    }
  }
  return false;
}

// Classify the def's kind from the bind's RHS payload kind in the
// green tree. The wrapper is SK_CONST_DECL / SK_VAR_DECL; the RHS
// is its last node-child (after the LHS and the bind operator
// token). KIND_NONE means "fall back to KIND_CONSTANT / VARIABLE
// based on the wrapper kind itself."
static DefKind classify_kind_from_rhs(SyntaxNode *wrapper, SyntaxKind wrap_kind) {
  if (!wrapper)
    return KIND_NONE;
  uint32_t count = syntax_node_num_children(wrapper);
  // Find the last node-child — that's the RHS.
  SyntaxNode *rhs = NULL;
  for (uint32_t i = count; i > 0; i--) {
    GreenElement g = green_node_child(syntax_node_green(wrapper), i - 1);
    if (g.kind == GREEN_ELEM_NODE) {
      rhs = syntax_node_child(wrapper, i - 1);
      break;
    }
  }
  DefKind kind = KIND_NONE;
  if (rhs) {
    switch (syntax_node_kind(rhs)) {
    case SK_STRUCT_DECL:    kind = KIND_STRUCT; break;
    case SK_UNION_DECL:     kind = KIND_UNION; break;
    case SK_ENUM_DECL:      kind = KIND_ENUM; break;
    case SK_EFFECT_DECL:    kind = KIND_EFFECT; break;
    case SK_LAMBDA_EXPR:    kind = KIND_FUNCTION; break;
    case SK_HANDLER_EXPR:   kind = KIND_HANDLER; break;
    default:                break; // plain value RHS
    }
    syntax_node_release(rhs);
  }
  if (kind == KIND_NONE)
    kind = (wrap_kind == SK_VAR_DECL) ? KIND_VARIABLE : KIND_CONSTANT;
  return kind;
}

// db_query_def_identity — canonical (nsid, node_ptr) → DefId materialization.
//
// Stable identity: the db.def_identity row persists across
// module_exports re-runs, so the same DefId is returned for the same
// (nsid, node_ptr) for the life of the db. On re-run, the slot's
// fingerprint is recomputed against the (possibly refreshed) green-
// tree shape; if it matches the prior stamp, downstream queries
// early-cut on dep-walk.
//
// On first call, allocates the DefId via db_create_def, locates the
// matching TopLevelEntry in the module's files (carries the
// precomputed name + meta), classifies the def's kind from the green
// tree's RHS payload, and fills the db.defs.* identity columns.
DefId db_query_def_identity(struct db *s, NamespaceId nsid,
                            SyntaxNodePtr node_ptr) {
  uint64_t k = ((uint64_t)nsid.idx << 32) | syntax_node_ptr_hash(node_ptr);

  // Route the (nsid, ptr-hash) key to a dense row in db.def_identity.
  // The row is allocated once and never moves, so the canonical DefId
  // is stable across re-runs. Row 0 is reserved — a real row is
  // non-NULL in the map.
  void *rowp = hashmap_get(&s->def_by_identity, k);
  uint32_t row;
  if (!rowp) {
    row = (uint32_t)s->def_identity.slots_hot.count;
    vec_push_zero(&s->def_identity.results);
    vec_push_zero(&s->def_identity.keys);
    vec_push_zero(&s->def_identity.slots_hot);
    vec_push_zero(&s->def_identity.slots_cold);
    // Stash the original SyntaxNodePtr so recompute_def_identity can
    // recover it from the routing key.
    *(SyntaxNodePtr *)vec_get(&s->def_identity.keys, row) = node_ptr;
    hashmap_put_or_die(&s->def_by_identity, k, (void *)(uintptr_t)row,
                       "def_by_identity");
  } else {
    row = (uint32_t)(uintptr_t)rowp;
  }

  DB_QUERY_GUARD(s, QUERY_DEF_IDENTITY, k,
                 *(DefId *)vec_get(&s->def_identity.results, row), DEF_ID_NONE,
                 DEF_ID_NONE);

  // Depend on the module's top-level index — find_top_level_entry
  // scans the module's file list; a file-set change must re-run this.
  (void)db_query_top_level_index(s, nsid);

  // First-allocation path: assign the canonical DefId.
  DefId cur = *(DefId *)vec_get(&s->def_identity.results, row);
  if (cur.idx == DEF_ID_NONE.idx) {
    cur = db_create_def(s);
    *(DefId *)vec_get(&s->def_identity.results, row) = cur;
    *(NamespaceId *)vec_get(&s->defs.parent_modules, cur.idx) = nsid;
    *(SyntaxNodePtr *)vec_get(&s->defs.syntax_ptrs, cur.idx) = node_ptr;
  }

  // Resolve (nsid, node_ptr) → matching TopLevelEntry.
  uint32_t local = 0;
  TopLevelEntry *entry = NULL;
  if (!find_top_level_entry(s, nsid, node_ptr, &local, &entry)) {
    db_query_fail(s, QUERY_DEF_IDENTITY, k);
    return DEF_ID_NONE;
  }

  StrId name = entry->name;
  DefMeta meta = entry->meta;
  DefKind kind = KIND_NONE;

  // Classify by walking the wrapper's RHS in the green tree.
  GreenNode *groot = *(GreenNode **)vec_get(&s->files.green_roots, local);
  if (groot) {
    SyntaxTree *tree = syntax_tree_new(groot);
    SyntaxNode *root_red = syntax_tree_root(tree);
    SyntaxNode *wrapper = syntax_node_ptr_resolve(node_ptr, root_red);
    if (wrapper) {
      kind = classify_kind_from_rhs(wrapper, node_ptr.kind);
      syntax_node_release(wrapper);
    }
    syntax_node_release(root_red);
    syntax_tree_free(tree);
  }
  if (kind == KIND_NONE)
    kind = (node_ptr.kind == SK_VAR_DECL) ? KIND_VARIABLE : KIND_CONSTANT;

  // Fill identity columns. parent_modules + syntax_ptrs were stamped
  // at alloc time above; the rest get filled (idempotently — same
  // values on every re-run, so the fingerprint below stays stable).
  *(StrId *)vec_get(&s->defs.names, cur.idx) = name;
  *(DefMeta *)vec_get(&s->defs.meta, cur.idx) = meta;
  // Classify (idempotent — db_def_set_kind early-returns once a def's
  // kind is fixed, so re-runs of this query are no-ops here).
  if (kind != KIND_NONE)
    db_def_set_kind(s, cur, kind);

  // Fingerprint folds kind too: db_locate_slot routes every def-keyed
  // query through kinds[def], so a decl changing kind (e.g. struct{} →
  // lambda) with name/meta unchanged must NOT early-cut its dependents.
  Fingerprint fp = db_fp_u64((uint64_t)name.idx);
  fp = db_fp_combine(fp, syntax_node_ptr_hash(node_ptr));
  fp = db_fp_combine(fp, db_fp_u64((uint64_t)meta));
  fp = db_fp_combine(fp, db_fp_u64((uint64_t)kind));

  db_query_succeed(s, QUERY_DEF_IDENTITY, k, fp);
  return cur;
}
