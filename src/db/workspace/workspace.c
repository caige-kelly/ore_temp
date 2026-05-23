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

#include "../db.h"
#include "../storage/stringpool.h"

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
                                    size_t importer_dir_len,
                                    const char *rel) {
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
  char *canonical = canonicalize_relative(importer_path, importer_dir_len,
                                          rel);
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

  SourceId new_src = db_create_source(s, canonical, canonical_len, text,
                                       text_len);
  NamespaceId target_mid = db_create_namespace(s);
  (void)db_create_file(s, new_src, target_mid);

  free(canonical);
  free(text);
  return target_mid;
}
