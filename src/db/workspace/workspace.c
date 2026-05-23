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
#include "../diag/diag.h"        // db_diags_clear
#include "../query/invalidate.h" // db_locate_slot
#include "../storage/stringpool.h"
#include "../../parser/ast.h" // ASTStore — for eviction free of malloc'd Vecs

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

  // 3. For each file backed by this source: free ASTStore's malloc'd
  //    Vecs (kinds/main_tokens/data/extra are NOT in the per-file
  //    arena — they're standalone Vecs malloc'd by the parser), then
  //    arena_free the per-file arena (reclaims ASTStore struct,
  //    FileNodeData arrays, line_starts/trivia/top_level/imports
  //    FileArray data, ast_id_map and its hashmap chunks), then NULL
  //    the per-file pointer columns + zero FileArray.count so
  //    iteration counts (Phase 8 gates) are consistent.
  for (size_t i = 1; i < s->files.source_id.count; i++) {
    SourceId *fsrc = (SourceId *)vec_get(&s->files.source_id, i);
    if (!source_id_eq(*fsrc, src))
      continue;

    struct ASTStore **ast_slot =
        (struct ASTStore **)vec_get(&s->files.asts, i);
    if (*ast_slot) {
      vec_free(&(*ast_slot)->kinds);
      vec_free(&(*ast_slot)->main_tokens);
      vec_free(&(*ast_slot)->data);
      vec_free(&(*ast_slot)->extra);
    }
    Arena *ma = (Arena *)vec_get(&s->files.arenas, i);
    arena_free(ma);
    // arena_free zeroes default_chunk_capacity, which would break a
    // future arena_alloc on this row. Re-init at a no-op capacity so
    // the Arena struct is in a valid empty state. Future code that
    // tries to allocate against an evicted file's arena hits the
    // arena_alloc growth path and produces a fresh chunk — but the
    // evicted-bit gates make this unreachable in practice.
    arena_init(ma, 0);

    *ast_slot = NULL;
    *(struct AstIdMap **)vec_get(&s->files.ast_id_maps, i) = NULL;

    FileNodeData *nd = (FileNodeData *)vec_get(&s->files.node_data, i);
    nd->spans = NULL;
    nd->parents = NULL;
    nd->defs = NULL;
    nd->types = NULL;
    *(uint32_t *)vec_get(&s->files.node_counts, i) = 0;

    *(FileArray *)vec_get(&s->files.line_starts, i) =
        (FileArray){.data = NULL, .count = 0};
    *(FileArray *)vec_get(&s->files.trivia_tokens, i) =
        (FileArray){.data = NULL, .count = 0};
    *(FileArray *)vec_get(&s->files.trivia_offsets, i) =
        (FileArray){.data = NULL, .count = 0};
    *(FileArray *)vec_get(&s->files.top_level_indices, i) =
        (FileArray){.data = NULL, .count = 0};
    *(FileArray *)vec_get(&s->files.imports, i) =
        (FileArray){.data = NULL, .count = 0};

    // Zero file-level query slots (FILE_AST, NODE_TO_DEF, FILE_IMPORTS).
    // Dep graph would invalidate them via DUR_MEDIUM bump below anyway;
    // direct zeroing avoids a wasted recompute attempt that would just
    // hit the evicted gates and return sentinels.
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
  NamespaceId target_nsid = db_create_namespace(s);
  (void)db_create_file(s, new_src, target_nsid);

  free(canonical);
  free(text);
  return target_nsid;
}

// Admit an in-memory source as a first-class file. See workspace.h
// for the identity-domain contract. Plan Phase 3b.
SourceId workspace_admit_virtual(struct db *s,
                                  const char *synthetic_name,
                                  size_t name_len,
                                  const char *text, size_t text_len) {
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

  SourceId src = db_admit_virtual_source(s, synthetic_name, name_len,
                                          text, text_len);
  NamespaceId nsid = db_create_namespace(s);
  (void)db_create_virtual_file(s, src, nsid);
  return src;
}
