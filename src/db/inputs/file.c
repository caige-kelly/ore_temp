// File mutators — the input boundary for the file table.
//
// A "file" is the parse unit: one source → one file → one module.
// db_create_file stamps the file's back-refs (source_id, module_id),
// prepares the per-file columns the parse query (QUERY_FILE_AST) will
// write into, and bumps the workspace-tier revision so dependent
// queries re-verify. Per-entry top-level enumeration is owned by
// QUERY_TOP_LEVEL_ENTRY now — no aggregating per-file index to stale.

#include "../db.h"

// First-chunk capacity for a per-file arena (db.files.arenas[fid]).
// Modest: most files are small; large ones grow via chunk doubling.
#define ORE_FILE_ARENA_DEFAULT_CAP (16 * 1024)

// Fold a newly-admitted file into its namespace's FILE_SET input
// fingerprint (O(1) per add). db_fp_combine is order-SENSITIVE, but files
// are admitted in deterministic id order, so the same membership always
// yields the same fp. Queries that read the file set (namespace_items,
// namespace_scopes) record a dep on FILE_SET, so this fp change
// invalidates exactly them when membership grows — the per-namespace
// precision a coarse tier bump can't give. Removal is symmetric:
// db_namespace_remove_file recomputes the fp from the surviving members
// (combine can't subtract, but it's an incremental fold, so a recompute
// over member_files is consistent with subsequent combine-based adds).
static void file_set_add(struct db *s, NamespaceId owner, FileId fid) {
  if (!namespace_id_valid(owner))
    return;
  // Append to the per-namespace reverse index (db_get_namespace_files reads
  // it instead of scanning every file). Input-side, append-only — same
  // class as the file_by_source map. The row was vec_init'd in
  // db_create_namespace, so push is amortized O(1).
  vec_push((Vec *)vec_get(&s->namespaces.member_files, (uint32_t)owner.idx),
           &fid);
  Fingerprint old = db_slot_fingerprint(s, QUERY_FILE_SET, (uint64_t)owner.idx);
  db_input_set(s, QUERY_FILE_SET, (uint64_t)owner.idx,
               db_fp_combine(old, db_fp_u64((uint64_t)fid.idx)), DUR_MEDIUM);
}

// L1 — readmit a previously-evicted source. Pre-L1, workspace_did_open
// would reuse an existing SourceId via source_by_path and only call
// db_set_source_text — never touching the `evicted` bit or the
// namespace's member_files. The result: the source's text updated, but
// downstream queries reading member_files saw the post-evict empty
// list (FILE_SET stamped at the shrunk membership), so the namespace's
// exports remained permanently lost until LSP restart.
//
// Safe to call unconditionally on every reopen: the evicted-check
// no-ops for sources that aren't evicted.
void db_readmit_source(struct db *s, SourceId src) {
  if (!source_id_valid(src) || src.idx >= s->sources.evicted.count)
    return;
  uint8_t *ev = (uint8_t *)vec_get(&s->sources.evicted, src.idx);
  if (*ev == 0)
    return;
  *ev = 0;
  // Re-join the namespace via file_set_add: appends to member_files +
  // folds FILE_SET fp via db_input_set (DUR_MEDIUM bump). The same path
  // db_create_file uses; symmetric with db_namespace_remove_file's
  // recompute, so downstream queries re-verify and see the restored
  // membership.
  FileId fid = db_lookup_file_by_source(s, src);
  if (!file_id_valid(fid))
    return;
  NamespaceId owner = db_get_file_namespace(s, fid);
  if (!namespace_id_valid(owner))
    return;
  file_set_add(s, owner, fid);
}

// Remove a file from its namespace's membership (on eviction). Drops `fid`
// from member_files (order-preserving, so files[0] stays the importer's
// first file for the multi-file future) and recomputes the FILE_SET fp
// from the survivors — db_fp_combine can't subtract, but folding
// member_files from the empty-set base (db_fp_u64(0), matching
// db_create_namespace's seed) reproduces exactly what the add-path combine
// would yield for that membership. Cost O(files-in-namespace), trivial.
// The caller (workspace_did_evict_source) provides the revision bump.
void db_namespace_remove_file(struct db *s, NamespaceId owner, FileId fid) {
  if (!namespace_id_valid(owner) ||
      owner.idx >= s->namespaces.member_files.count)
    return;
  Vec *m = (Vec *)vec_get(&s->namespaces.member_files, (uint32_t)owner.idx);
  FileId *fids = (FileId *)m->data;

  // Compact out the first matching fid in place; shrink the live length.
  size_t w = 0;
  bool removed = false;
  for (size_t r = 0; r < m->count; r++) {
    if (!removed && fids[r].idx == fid.idx) {
      removed = true;
      continue;
    }
    fids[w++] = fids[r];
  }
  m->count = w;

  // Recompute the FILE_SET fp from the surviving membership.
  Fingerprint fp = db_fp_u64(0); // empty-set base == db_create_namespace seed
  for (size_t i = 0; i < m->count; i++)
    fp = db_fp_combine(fp, db_fp_u64((uint64_t)fids[i].idx));
  db_input_set(s, QUERY_FILE_SET, (uint64_t)owner.idx, fp, DUR_MEDIUM);
}

// Shared body for db_create_file (bump=true) and db_create_file_lazy
// (bump=false). The bump is the only difference: see each wrapper's
// docstring for which path needs which.
static FileId db_create_file_impl(struct db *s, SourceId src,
                                  NamespaceId owner, bool bump) {
  uint32_t idx = (uint32_t)s->files.ids.count;
  FileId fid = file_id_make_physical(idx);

  // Grow every files column by one zero row in lockstep — X-macro
  // driven so a new (or split) column can't be forgotten here. Vec
  // columns + PagedVec slot columns step together so row N exists in
  // every column at once.
#define X(name, type, _evict) vec_push_zero(&s->files.name);
  ORE_FILES_COLUMNS(X)
#undef X
#define X(name, type, _evict) paged_push_zero(&s->files.name);
  ORE_FILES_PAGED_DIAG_COLUMNS(X)
#undef X
#define X(name, type) paged_push_zero(&s->files.name);
  ORE_FILES_SLOT_COLUMNS(X)
#undef X
  // Stamp the identity / back-ref columns over their zero rows. The
  // owner write IS what makes this file a member of `owner`'s module —
  // db_get_namespace_files filters this column.
  *(FileId *)vec_get(&s->files.ids, idx) = fid;
  *(SourceId *)vec_get(&s->files.source_id, idx) = src;
  *(NamespaceId *)vec_get(&s->files.module_id, idx) = owner;
  arena_init((Arena *)vec_get(&s->files.arenas, idx),
             ORE_FILE_ARENA_DEFAULT_CAP);

  // O(1) source → file reverse index. Value is the file_local idx
  // (file_id_local of fid); callers reconstruct the FileId via
  // file_id_make_physical() in the lookup.
  hashmap_put(&s->file_by_source, (uint64_t)src.idx, (void *)(uintptr_t)idx);

  // Bump DUR_MEDIUM: the namespace's file set just grew, so any
  // namespace-scoped query (NAMESPACE_SCOPES, NAMESPACE_TYPE,
  // TOP_LEVEL_ENTRY) recorded against the old file set must re-verify
  // on its next pull. Per-entry verification then drives the actual
  // recompute decision — slots that were never computed pay nothing.
  if (namespace_id_valid(owner)) {
    if (bump)
      db_input_changed(s, (uint8_t)DUR_MEDIUM);
    file_set_add(s, owner, fid);
  }

  return fid;
}

// Workspace-editor entrypoint: a user added a file to an existing
// namespace tree (LSP did_open, multi-file module composition). Bumps
// the revision so dependent queries re-verify on next pull.
// MUST be called outside an open request — db_input_changed asserts.
FileId db_create_file(struct db *s, SourceId src, NamespaceId owner) {
  return db_create_file_impl(s, src, owner, /*bump=*/true);
}

// Lazy-load entrypoint: an @import resolution is populating a brand-new
// namespace that was just created by the same call chain. Skips the
// revision bump because nothing was watching the empty FILE_SET — the
// bump would be a structural no-op (no slot recorded a dep on the
// just-created namespace) but globally noisy, AND would trip
// db_input_changed's "no open request" assert (workspace_resolve_import
// runs inside infer_body's request frame). Safe ONLY because the owner
// namespace was created by the same call site immediately before. If
// future callers want to add a file to an EXISTING namespace inside a
// request, they need a different mechanism — bumping is correctness
// (verify the watchers) for any non-fresh namespace.
FileId db_create_file_lazy(struct db *s, SourceId src, NamespaceId owner) {
  return db_create_file_impl(s, src, owner, /*bump=*/false);
}

// Virtual-file row: same shape as db_create_file but the FileId's
// virtual bit is set (file_id_make_virtual) and the gate-bump for
// TOP_LEVEL_INDEX is skipped — virtual files are admitted into a
// FRESH owner namespace (caller allocates via db_create_namespace
// right before), so the gate would never fire anyway.
//
// db_get_source_is_virtual(source(fid)) is the canonical "this file
// is synthetic" check; the FileId bit is the same information at the
// file layer.
FileId db_create_virtual_file(struct db *s, SourceId src, NamespaceId owner) {
  uint32_t idx = (uint32_t)s->files.ids.count;
  FileId fid = file_id_make_virtual(idx);

#define X(name, type, _evict) vec_push_zero(&s->files.name);
  ORE_FILES_COLUMNS(X)
#undef X
#define X(name, type, _evict) paged_push_zero(&s->files.name);
  ORE_FILES_PAGED_DIAG_COLUMNS(X)
#undef X
#define X(name, type) paged_push_zero(&s->files.name);
  ORE_FILES_SLOT_COLUMNS(X)
#undef X
  *(FileId *)vec_get(&s->files.ids, idx) = fid;
  *(SourceId *)vec_get(&s->files.source_id, idx) = src;
  *(NamespaceId *)vec_get(&s->files.module_id, idx) = owner;
  arena_init((Arena *)vec_get(&s->files.arenas, idx),
             ORE_FILE_ARENA_DEFAULT_CAP);

  hashmap_put(&s->file_by_source, (uint64_t)src.idx, (void *)(uintptr_t)idx);
  // No revision bump: virtual file's owner is always a fresh namespace
  // (no prior queries to invalidate). Still fold into FILE_SET for
  // consistency / future multi-file virtual namespaces.
  file_set_add(s, owner, fid);
  return fid;
}
