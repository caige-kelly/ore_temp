#include "db.h"

#include "uri.h"

#include "../../compiler/compile.h"
#include "../../db/diag/diag.h"
#include "../../db/intern_pool/intern_pool.h"
#include "../../db/query/resolve_ref.h"
#include "../../db/query/type_of_def.h"
#include "../../db/workspace/workspace.h"
#include "../../sema/sema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void oredb_init(struct OreDb *lsp_db) {
  db_init(&lsp_db->db);
  vec_init(&lsp_db->drafts, sizeof(struct Draft));
}

void oredb_free(struct OreDb *lsp_db) {
  vec_free(&lsp_db->drafts);
  db_free(&lsp_db->db);
}

// Grow the drafts Vec so `drafts[src.idx]` is accessible. New slots
// are zero-initialized — lsp_synced=false, version=0 — matching the
// "never opened" semantics.
static struct Draft *ensure_draft_slot(struct OreDb *lsp_db, SourceId src) {
  while (lsp_db->drafts.count <= src.idx) {
    *(struct Draft *)vec_push_slot(&lsp_db->drafts) = (struct Draft){0};
  }
  return (struct Draft *)vec_get(&lsp_db->drafts, src.idx);
}

SourceId oredb_did_open(struct OreDb *lsp_db, const char *uri, int32_t version,
                        const char *text, size_t text_len) {
  if (!uri)
    return SOURCE_ID_NONE;

  char *path = lsp_uri_to_path(uri);
  if (!path)
    return SOURCE_ID_NONE;

  size_t path_len = strlen(path);

  // Route through the workspace coordinator. workspace_did_open
  // canonicalizes the path via realpath and returns the registered
  // SourceId — we use that directly since the original `path` may
  // not be canonical. File-as-namespace: each file owns its own
  // fresh NamespaceId; sibling files do NOT share scope (must @import).
  SourceId src =
      workspace_did_open(&lsp_db->db, path, path_len, text, text_len);
  free(path);

  if (source_id_valid(src)) {
    struct Draft *d = ensure_draft_slot(lsp_db, src);
    d->lsp_synced = true;
    d->version = version;
  }

  return src;
}

SourceId oredb_did_change(struct OreDb *lsp_db, const char *uri,
                          int32_t version, const char *text, size_t text_len) {
  if (!uri)
    return SOURCE_ID_NONE;

  char *path = lsp_uri_to_path(uri);
  if (!path)
    return SOURCE_ID_NONE;

  size_t path_len = strlen(path);
  SourceId src = db_lookup_source_by_path(&lsp_db->db, path, path_len);

  if (!source_id_valid(src)) {
    // Editor sent didChange for a file we've never seen. Spec says
    // didOpen always precedes didChange — log and drop.
    fprintf(stderr, "lsp: dropped didChange for unknown file %s\n", uri);
    free(path);
    return SOURCE_ID_NONE;
  }

  // Stale-packet check: out-of-order didChange (older version than
  // what we already applied). LSP doesn't guarantee strict ordering
  // across reconnects.
  if (src.idx < lsp_db->drafts.count) {
    struct Draft *prev = (struct Draft *)vec_get(&lsp_db->drafts, src.idx);
    if (prev->lsp_synced && version < prev->version) {
      fprintf(stderr,
              "lsp: dropping stale didChange (version %d < %d) for %s\n",
              version, prev->version, uri);
      free(path);
      return SOURCE_ID_NONE;
    }
  }

  // Route through workspace so any future per-edit bookkeeping
  // (file watcher, import-graph refresh, etc.) gathers in one place.
  workspace_did_change(&lsp_db->db, path, path_len, text, text_len);
  free(path);

  struct Draft *d = ensure_draft_slot(lsp_db, src);
  d->lsp_synced = true;
  d->version = version;

  return src;
}

bool oredb_did_close(struct OreDb *lsp_db, const char *uri) {
  if (!uri)
    return false;
  char *path = lsp_uri_to_path(uri);
  if (!path)
    return false;
  SourceId src = db_lookup_source_by_path(&lsp_db->db, path, strlen(path));
  free(path);

  if (!source_id_valid(src) || src.idx >= lsp_db->drafts.count)
    return false;

  struct Draft *d = (struct Draft *)vec_get(&lsp_db->drafts, src.idx);
  if (!d->lsp_synced)
    return false;

  d->lsp_synced = false;
  return true;
}

FileId oredb_typecheck(struct OreDb *lsp_db, SourceId src) {
  // Thin wrapper over the canonical compile_file pipeline. We don't
  // need the collected diags ourselves — build_publish_params re-walks
  // db.diag_lists when constructing the publishDiagnostics JSON. But
  // compile_file's contract requires a diags Vec, so allocate a
  // throwaway one and free immediately.
  //
  // Future optimization: compile_file already populates out_diags
  // (shallow copy from db.diag_lists). publish_diagnostics could
  // consume from this directly instead of re-walking. Defer.
  CompileFileOpts co = {.profile_count = 1};
  Vec diags;
  vec_init(&diags, sizeof(Diag));
  FileId fid = compile_file(&lsp_db->db, src, &co, &diags);
  vec_free(&diags);
  return fid;
}

// === IDE features ============================================================
//
// Hover + completion implementations live in src/ide/. The LSP server
// handlers (handle_hover, handle_completion) call ide_hover_at /
// ide_completions_at directly after looking up the file from the URI.
// This file (lsp/db.c) is now LSP-specific bookkeeping only: drafts,
// did_open/did_change/did_close, the oredb_typecheck wrapper over
// compile_file.
