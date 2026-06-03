// Module mutators — input boundary for the module table.
//
// Each file owns its own NamespaceId (file-as-namespace, matching Zig).
// Workspace allocates one per registered source; no shared
// directory-as-module identity. The set of files belonging to a module
// is the back-ref `files.module_id` filtered to that NamespaceId
// (db_get_namespace_files).

#include "../db.h"

NamespaceId db_create_namespace(struct db *s) {
  uint32_t idx = (uint32_t)s->namespaces.ids.count;
  NamespaceId nsid = {.idx = idx};
  // Grow every rowed modules column by one zero row in lockstep —
  // X-macro driven so a new (or split) column can't be forgotten.
  // Vec metadata + PagedVec slot columns step together so row N
  // exists everywhere at once.
#define X(name, type) vec_push_zero(&s->namespaces.name);
  ORE_NAMESPACES_COLUMNS(X)
#undef X
#define X(name, type) paged_push_zero(&s->namespaces.name);
  ORE_NAMESPACES_PAGED_DIAG_COLUMNS(X)
  ORE_NAMESPACES_SLOT_COLUMNS(X)
#undef X
  *(NamespaceId *)vec_get(&s->namespaces.ids, idx) = nsid;

  // The member_files row is a Vec<FileId> (per-namespace reverse index for
  // db_get_namespace_files). The lockstep vec_push_zero above left it a
  // zeroed Vec (element_size == 0, can't be pushed into); give it its real
  // element size now. file_set_add appends each admitted file's id.
  vec_init((Vec *)vec_get(&s->namespaces.member_files, idx), sizeof(FileId));

  // Seed the FILE_SET input slot for the empty file set. db_create_file
  // folds each added file's id into this fingerprint, so a query that
  // reads the file set (top_level_entry, namespace_scopes) is invalidated
  // when membership changes — the per-namespace mechanism that a coarse
  // tier bump can't provide (see db_create_file).
  db_input_set(s, QUERY_FILE_SET, (uint64_t)idx, db_fp_u64(0), DUR_MEDIUM);
  return nsid;
}
