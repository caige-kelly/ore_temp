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
  ORE_NAMESPACES_SLOT_COLUMNS(X)
#undef X
  *(NamespaceId *)vec_get(&s->namespaces.ids, idx) = nsid;
  return nsid;
}
