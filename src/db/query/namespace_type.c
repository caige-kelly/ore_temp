#include "namespace_type.h"

#include "../db.h"
#include "../intern_pool/intern_pool.h"
#include "../storage/arena.h"
#include "def_identity.h"
#include "index.h"
#include "query.h"
#include "query_engine.h"

#include <string.h>

// db_query_namespace_type — build (and intern) the IpIndex for the
// namespace's struct type. The struct's fields are the file's public
// top-level decls. Field types are NOT resolved here — the struct
// stores (StrId name, DefId def) pairs, and sema's dot-access calls
// db_query_type_of_def(field_def) on demand. Matches Zig's
// Namespace.owner_type model.
//
// Deps:
//   - db_query_top_level_index(nsid) — pub-decl set
//   - db_query_def_identity(nsid, ast_id) per pub decl — stable DefIds
IpIndex db_query_namespace_type(struct db *s, NamespaceId nsid) {
  if (!namespace_id_valid(nsid))
    return IP_NONE;

  DB_QUERY_GUARD(s, QUERY_NAMESPACE_TYPE, (uint64_t)nsid.idx,
                 ((NamespaceScopes *)vec_get(&s->namespaces.exports, nsid.idx))
                     ->struct_type,
                 IP_NONE, IP_NONE);

  // Record the dep that fingerprints the pub-decl set. Any edit that
  // changes the index re-runs this query.
  (void)db_query_top_level_index(s, nsid);

  // Collect (name, def_id) pairs for every public decl across the
  // namespace's backing files. Stored in request_arena so the borrowed
  // pointers in the IpKey survive until ip_get finishes (which copies
  // them into the pool's extra_arena).
  uint32_t file_count = 0;
  const FileId *files = db_get_namespace_files(s, nsid, &file_count);

  // Two passes so we know the exact n_fields before allocating the
  // tight arrays. Counting first avoids growing buffers nsid-walk.
  size_t n_pub = 0;
  for (uint32_t fi = 0; fi < file_count; fi++) {
    uint32_t local = file_id_local(files[fi]);
    FileArray *idx = (FileArray *)vec_get(&s->files.top_level_indices, local);
    for (size_t i = 0; i < idx->count; i++) {
      TopLevelEntry *e = &((TopLevelEntry *)idx->data)[i];
      if ((e->meta & META_VIS_MASK) == VIS_PUBLIC)
        n_pub++;
    }
  }

  StrId *names = NULL;
  DefId *defs = NULL;
  if (n_pub > 0) {
    names = (StrId *)arena_alloc_raw(&s->request_arena, n_pub * sizeof(StrId));
    defs = (DefId *)arena_alloc_raw(&s->request_arena, n_pub * sizeof(DefId));
    size_t out = 0;
    for (uint32_t fi = 0; fi < file_count; fi++) {
      uint32_t local = file_id_local(files[fi]);
      FileArray *idx = (FileArray *)vec_get(&s->files.top_level_indices, local);
      for (size_t i = 0; i < idx->count; i++) {
        TopLevelEntry *e = &((TopLevelEntry *)idx->data)[i];
        if ((e->meta & META_VIS_MASK) != VIS_PUBLIC)
          continue;
        names[out] = e->name;
        // Record dep on the def's identity slot — rename / removal
        // invalidates this query's slot via the standard dep walk.
        defs[out] = db_query_def_identity(s, nsid, e->ast_id);
        out++;
      }
    }
  }

  IpKey k = {.kind = IPK_NAMESPACE_TYPE};
  k.namespace_type.nsid = nsid;
  k.namespace_type.field_names = names;
  k.namespace_type.field_defs = defs;
  k.namespace_type.n_fields = n_pub;
  IpIndex result = ip_get(&s->intern, k);

  // Cache in the per-namespace record so cached-hit branch returns it
  // without re-interning.
  ((NamespaceScopes *)vec_get(&s->namespaces.exports, nsid.idx))->struct_type =
      result;

  // Fingerprint covers nsid + each (name, def_id) pair.
  Fingerprint fp = db_fp_u64((uint64_t)nsid.idx);
  fp = db_fp_combine(fp, db_fp_u64((uint64_t)n_pub));
  for (size_t i = 0; i < n_pub; i++) {
    fp = db_fp_combine(fp, db_fp_u64((uint64_t)names[i].idx));
    fp = db_fp_combine(fp, db_fp_u64((uint64_t)defs[i].idx));
  }
  db_query_succeed(s, QUERY_NAMESPACE_TYPE, (uint64_t)nsid.idx, fp);
  return result;
}
