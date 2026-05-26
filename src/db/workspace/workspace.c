// Workspace coordinator — the single layer that calls db input
// setters AND owns disk I/O. See workspace.h for the layering
// rationale.
//
// File-as-namespace model (Zig-aligned):
//   - Each registered .ore file gets its own NamespaceId (1:1).
//   - Sibling files do NOT share scope. To access a sibling's
//     decls, you must @import("./sibling.ore").
//   - workspace_resolve_import canonicalizes via realpath, looks up
//     in source_by_path, and lazy-loads from disk if absent.
//   - Lazy loads do NOT bump revisions (Roslyn/rust-analyzer model).

#include "workspace.h"

#include "path.h"

#include "../../support/data_structure/stringpool.h"
#include "../../syntax/syntax.h" // GreenNode + green_node_release for eviction
#include "../db.h"
#include "../diag/diag.h"        // db_diags_clear
#include "../query/invalidate.h" // db_locate_slot

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Read a whole file into a malloc'd, NUL-terminated buffer. Returns
// NULL on any failure (open, seek, alloc, short read). out_len gets
// the byte length excluding the NUL.
static char *slurp_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long sz = ftell(f);
  if (sz < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  char *buf = (char *)malloc((size_t)sz + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t n = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (n != (size_t)sz) {
    free(buf);
    return NULL;
  }
  buf[sz] = '\0';
  if (out_len)
    *out_len = (size_t)sz;
  return buf;
}

// Canonical absolute path. Tries realpath() (resolves symlinks +
// case-quirks on HFS+/APFS); falls back to a malloc'd copy of the
// input if realpath fails (file doesn't exist yet — common for LSP
// did_open on a freshly typed but unsaved buffer). Returns malloc'd
// buffer the caller must free.
static char *canonicalize_path(const char *path, size_t path_len) {
  char *real = realpath(path, NULL);
  if (real)
    return real;
  char *copy = (char *)malloc(path_len + 1);
  if (!copy)
    return NULL;
  memcpy(copy, path, path_len);
  copy[path_len] = '\0';
  return copy;
}

// Resolve `rel` against `importer_path`'s directory, then realpath
// the result. Returns malloc'd absolute canonical path, or NULL if
// the target file doesn't exist on disk.
static char *canonicalize_relative(const char *importer_path,
                                   size_t importer_dir_len, const char *rel) {
  char joined[ORE_PATH_MAX];
  size_t n = path_normalize(importer_path, importer_dir_len, rel, strlen(rel),
                            joined, sizeof(joined));
  if (n == 0)
    return NULL;
  // realpath REQUIRES the file to exist. For @import, that's correct —
  // a missing target is a resolution failure (caller emits diag).
  return realpath(joined, NULL);
}

SourceId workspace_did_open(struct db *s, const char *path, size_t path_len,
                            const char *text, size_t text_len) {
  // Canonicalize for stable identity (e.g. /tmp/a vs /private/tmp/a on
  // macOS; case-insensitive FS; symlinks).
  char *canonical = canonicalize_path(path, path_len);
  if (!canonical)
    return SOURCE_ID_NONE;
  size_t canonical_len = strlen(canonical);

  SourceId src = db_lookup_source_by_path(s, canonical, canonical_len);
  if (source_id_valid(src)) {
    // Already registered — just update the text. Source's module is
    // already pinned; db_set_source_text handles the FILE_AST stale
    // + revision bump.
    db_set_source_text(s, src, text, text_len);
    free(canonical);
    return src;
  }

  // First-time registration. File-as-namespace: each file gets its
  // own fresh NamespaceId. dir_path is STR_ID_NONE — the module's
  // identity is the file (back-ref via files.module_id), not a
  // directory string.
  src = db_create_source(s, canonical, canonical_len, text, text_len);
  NamespaceId nsid = db_create_namespace(s);
  (void)db_create_file(s, src, nsid);

  free(canonical);
  return src;
}

void workspace_did_change(struct db *s, const char *path, size_t path_len,
                          const char *text, size_t text_len) {
  char *canonical = canonicalize_path(path, path_len);
  if (!canonical)
    return;
  SourceId src = db_lookup_source_by_path(s, canonical, strlen(canonical));
  free(canonical);
  if (!source_id_valid(src))
    return; // unknown source: silent no-op (LSP convention)
  db_set_source_text(s, src, text, text_len);
}

void workspace_did_close(struct db *s, const char *path, size_t path_len) {
  (void)s;
  (void)path;
  (void)path_len;
  // Source stays in the db: other files may still import it via the
  // resolved module. A future LSP could refcount editor-opens and
  // evict orphaned sources; not needed for Gap B.
}

// External-tool-driven source update (FS watcher: file modified on
// disk by `git checkout`, a non-LSP editor, codegen tool, etc.).
//
// Silently no-ops on unknown paths — the watcher fires for every
// matching file in the glob, not just ones we've lazy-loaded. If a
// stranger file is changed, sema will lazy-load fresh content on its
// next @import; nothing to do here.
//
// If `text` is NULL, slurps disk content (LSP watcher events don't
// carry content). Returns false on slurp failure.
bool workspace_did_change_external(struct db *s, const char *path,
                                   size_t path_len, const char *text,
                                   size_t text_len) {
  char *canonical = canonicalize_path(path, path_len);
  if (!canonical)
    return false;
  SourceId src = db_lookup_source_by_path(s, canonical, strlen(canonical));
  if (!source_id_valid(src)) {
    free(canonical);
    return true; // unknown source — fine, sema will lazy-load fresh
  }

  if (text == NULL) {
    char *disk_text = slurp_file(canonical, &text_len);
    free(canonical);
    if (!disk_text)
      return false; // file vanished between event and read; caller
                    // may follow up with workspace_did_evict_source
    db_set_source_text(s, src, disk_text, text_len);
    free(disk_text);
    return true;
  }

  free(canonical);
  db_set_source_text(s, src, text, text_len);
  return true;
}

// External-tool-driven source removal (FS watcher: file deleted on
// disk). Marks the source row evicted, frees the source text buffer
// and every per-file arena backed by the source, NULLs the per-file
// pointer columns, clears file-level query slots, and bumps DUR_MEDIUM.
//
// SAFETY: stable-IDs invariant holds — SourceId/FileId/NamespaceId
// rows stay allocated; only their content is reclaimed. Readers that
// might race a freed pointer are gated on db_get_source_evicted
// (db_resolve_span, db_get_file_ast, db_get_node_span,
// db_byte_offset_at, db_node_at_offset, db_get_file_ast_id_map,
// db_get_def_for_node).
// ============================================================================
// Eviction actions for the ORE_FILES_COLUMNS X-macro.
//
// Each macro takes (column_name, element_type, fid_local) and emits
// the per-row cleanup code for that column. The X-macro driver in
// workspace_did_evict_source expands once per column, dispatching to
// the action named in ORE_FILES_COLUMNS' third arg.
//
// Adding a new per-file column to ORE_FILES_COLUMNS requires naming
// one of these actions (or adding a new one). Forgetting to specify
// = compile error (unknown EVICT_*). This is the single source of
// truth that closes the silent-leak hole the open-coded eviction had.
// ============================================================================

// No-op: the column survives eviction. IDs and back-references (FileId,
// SourceId, NamespaceId) must not change — stable-IDs invariant. Query
// slots use EVICT_NOOP because they're reset explicitly below (the
// reset is a partial clear: state + fingerprint, NOT a full struct
// zero — preserving recorded deps + verified_rev for the next
// compute).
#define EVICT_NOOP(name, type, idx) ((void)0)

// Set a single-pointer column to NULL. For pointer types whose pointee
// lives in the per-file arena (freed via EVICT_ARENA_FREE on the
// arenas column). Generic because not all pointer columns share a
// type — `type` parameter binds the cast.
#define EVICT_NULL_PTR_GENERIC(name, type, idx)                                \
  *(type *)vec_get(&s->files.name, (idx)) = NULL

// GreenNode *: lives outside the per-file arena (malloc'd via
// green_node_alloc and refcounted). Release our +1 and NULL the slot.
// The NodeCache may still hold a refcount; it'll release that on
// node_cache_destroy at db_free.
#define EVICT_RELEASE_GREEN(name, type, idx)                                   \
  do {                                                                         \
    struct GreenNode **_slot =                                                 \
        (struct GreenNode **)vec_get(&s->files.name, (idx));                   \
    if (*_slot) {                                                              \
      green_node_release(*_slot);                                              \
    }                                                                          \
    *_slot = NULL;                                                             \
  } while (0)

// HashMap lives by-value in the Vec. hashmap_clear empties the table
// but keeps the bucket allocation reusable for the next parse.
#define EVICT_HASHMAP_CLEAR(name, type, idx)                                   \
  do {                                                                         \
    HashMap *_m = (HashMap *)vec_get(&s->files.name, (idx));                   \
    hashmap_clear(_m);                                                         \
  } while (0)

// Arena: free chunks, then arena_init(0) to leave the struct in a
// valid empty state. arena_free alone zeroes default_chunk_capacity
// which would break a future arena_alloc (the evicted-bit gates make
// that unreachable in practice, but defense-in-depth).
#define EVICT_ARENA_FREE(name, type, idx)                                      \
  do {                                                                         \
    Arena *_a = (Arena *)vec_get(&s->files.name, (idx));                       \
    arena_free(_a);                                                            \
    arena_init(_a, 0);                                                         \
  } while (0)

// Plain uint32_t column — write 0.
#define EVICT_ZERO_U32(name, type, idx)                                        \
  *(uint32_t *)vec_get(&s->files.name, (idx)) = 0

// FileArray { void *data, uint32_t count }: data lived in the per-file
// arena (now freed); zero both fields so iteration filters see empty.
#define EVICT_ZERO_FILEARRAY(name, type, idx)                                  \
  *(FileArray *)vec_get(&s->files.name, (idx)) =                               \
      (FileArray){.data = NULL, .count = 0}

void workspace_did_evict_source(struct db *s, const char *path,
                                size_t path_len) {
  char *canonical = canonicalize_path(path, path_len);
  if (!canonical)
    return;
  SourceId src = db_lookup_source_by_path(s, canonical, strlen(canonical));
  free(canonical);
  if (!source_id_valid(src))
    return; // unknown source: nothing to evict

  // 1. Set evicted bit FIRST. Any reader that races (e.g. an
  //    in-flight diag rendering) sees the bit and bails before
  //    dereferencing the about-to-be-freed buffer.
  *(uint8_t *)vec_get(&s->sources.evicted, src.idx) = 1;

  // 2. Free + NULL the source text. db_resolve_span gates on evicted
  //    so this is safe.
  char **text_slot = (char **)vec_get(&s->sources.texts, src.idx);
  if (*text_slot) {
    free(*text_slot);
    *text_slot = NULL;
  }
  *(uint32_t *)vec_get(&s->sources.text_lens, src.idx) = 0;

  // 3. For each file backed by this source: drive eviction through the
  //    ORE_FILES_COLUMNS X-macro. Each column's third arg names the
  //    EVICT_* action to run on this row. Centralizes the policy with
  //    the column declaration — adding a new column = updating the
  //    macro line = eviction stays correct automatically.
  for (size_t i = 1; i < s->files.source_id.count; i++) {
    SourceId *fsrc = (SourceId *)vec_get(&s->files.source_id, i);
    if (!source_id_eq(*fsrc, src))
      continue;

    size_t fid_local = i;
#define X(name, type, evict) evict(name, type, fid_local);
    ORE_FILES_COLUMNS(X)
#undef X

    // Query slots survive the X-macro pass (EVICT_NOOP) because their
    // reset is a PARTIAL clear: state + fingerprint, NOT a full struct
    // zero — recorded deps and verified_rev stay intact so the next
    // recompute can early-cut correctly. Going through db_locate_slot
    // also handles the dispatch (which Vec the slot lives in) in one
    // place, matching how the rest of the engine accesses slots.
    FileId *fkey = (FileId *)vec_get(&s->files.ids, i);
    QuerySlotHot *sl;
    if ((sl = db_locate_slot(s, QUERY_FILE_AST, (uint64_t)fkey->idx))) {
      sl->state = QUERY_EMPTY;
      sl->fingerprint = FINGERPRINT_NONE;
    }
    if ((sl = db_locate_slot(s, QUERY_NODE_TO_DECL, (uint64_t)fkey->idx))) {
      sl->state = QUERY_EMPTY;
      sl->fingerprint = FINGERPRINT_NONE;
    }
    if ((sl = db_locate_slot(s, QUERY_FILE_IMPORTS, (uint64_t)fkey->idx))) {
      sl->state = QUERY_EMPTY;
      sl->fingerprint = FINGERPRINT_NONE;
    }
    db_diags_clear(s, QUERY_FILE_AST, (uint64_t)fkey->idx);
    db_diags_clear(s, QUERY_NODE_TO_DECL, (uint64_t)fkey->idx);
    db_diags_clear(s, QUERY_FILE_IMPORTS, (uint64_t)fkey->idx);
  }

  // 4. Bump DUR_MEDIUM — the file-set has structurally changed
  // (analogous to db_create_file's bump on file addition). Downstream
  // queries (top_level_index, namespace_scopes, namespace_type, ...)
  // re-verify and propagate the eviction through the dep graph.
  db_input_changed(s, (uint8_t)DUR_MEDIUM);
}

// Resolve a string like "./b.ore" (relative to importer's file's
// directory) to the imported file's NamespaceId. Lazy-loads from disk
// on miss. NO revision bump on lazy load — matches Roslyn/rust-
// analyzer "lazy inputs" model where disk reads populate a memoized
// view of an immutable external truth.
//
// File-as-module invariant: every registered file owns its own
// NamespaceId. Sema turns this into the file's struct type via
// db_query_namespace_type (IPK_NAMESPACE_TYPE).
NamespaceId workspace_resolve_import(struct db *s, NamespaceId importer_module,
                                     StrId path_str) {
  if (!namespace_id_valid(importer_module) || path_str.idx == STR_ID_NONE.idx)
    return NAMESPACE_ID_NONE;

  // 1. Find the importer's file (file-as-module: exactly one file per
  //    module). Then the importer's source's path, which is the basis
  //    for relative-path resolution.
  uint32_t fcount = 0;
  const FileId *files = db_get_namespace_files(s, importer_module, &fcount);
  if (!files || fcount == 0)
    return NAMESPACE_ID_NONE;
  FileId importer_fid = files[0];

  SourceId importer_src = db_get_file_source(s, importer_fid);
  StrId importer_path_id = db_get_source_path(s, importer_src);
  if (importer_path_id.idx == STR_ID_NONE.idx)
    return NAMESPACE_ID_NONE;
  const char *importer_path = pool_get(&s->strings, importer_path_id);
  size_t importer_path_len = strlen(importer_path);
  size_t importer_dir_len = path_dirname_len(importer_path, importer_path_len);

  // 2. Canonicalize the @import argument. realpath() requires the
  //    target to exist on disk — a missing file returns NULL → import
  //    fails → caller emits "file not found" diag.
  const char *rel = pool_get(&s->strings, path_str);
  char *canonical = canonicalize_relative(importer_path, importer_dir_len, rel);
  if (!canonical)
    return NAMESPACE_ID_NONE;

  // 3. Already in registry? Reuse its NamespaceId.
  size_t canonical_len = strlen(canonical);
  SourceId existing = db_lookup_source_by_path(s, canonical, canonical_len);
  if (source_id_valid(existing)) {
    free(canonical);
    FileId fid = db_lookup_file_by_source(s, existing);
    return db_get_file_namespace(s, fid);
  }

  // 4. Lazy load — slurp + register. Fresh module-per-file (file-as-
  //    namespace). db_create_file's DUR_MEDIUM-bump gate doesn't fire
  //    because the new module's TOP_LEVEL_INDEX slot is QUERY_EMPTY.
  size_t text_len = 0;
  char *text = slurp_file(canonical, &text_len);
  if (!text) {
    free(canonical);
    return NAMESPACE_ID_NONE;
  }

  SourceId new_src =
      db_create_source(s, canonical, canonical_len, text, text_len);
  NamespaceId target_nsid = db_create_namespace(s);
  (void)db_create_file(s, new_src, target_nsid);

  free(canonical);
  free(text);
  return target_nsid;
}

// Admit an in-memory source as a first-class file. See workspace.h
// for the identity-domain contract. Plan Phase 3b.
SourceId workspace_admit_virtual(struct db *s, const char *synthetic_name,
                                 size_t name_len, const char *text,
                                 size_t text_len) {
  if (!synthetic_name || name_len == 0)
    return SOURCE_ID_NONE;

  // Collision check against existing disk-registered paths AND
  // already-admitted virtual names — both intern the name as a StrId,
  // so an existing entry under the same name fails the admit.
  // Note: source_by_path holds DISK paths; for virtual collision we
  // rely on the caller to use unique synthetic names. A future
  // virtual_by_name HashMap could enforce this if collisions become
  // a real concern.
  if (db_lookup_source_by_path(s, synthetic_name, name_len).idx != 0)
    return SOURCE_ID_NONE;

  SourceId src =
      db_admit_virtual_source(s, synthetic_name, name_len, text, text_len);
  NamespaceId nsid = db_create_namespace(s);
  (void)db_create_virtual_file(s, src, nsid);
  return src;
}
