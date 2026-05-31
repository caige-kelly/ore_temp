#ifndef ORE_LSP_DB_H
#define ORE_LSP_DB_H

// LSP-server-owned wrapper around `struct db`. Adds per-source editor
// state (`Draft`) on top of the salsa db's input tables; the db itself
// already interns paths, dedups SourceIds, and tracks file/module
// associations. The Draft table only holds what the db doesn't care
// about: whether the source is currently open in the editor and the
// LSP-protocol-version number.
//
// Lifecycle: one OreDb per server process. Created on `initialize`,
// torn down on `exit`. Holds a long-lived `struct db` whose sources
// accumulate as the editor opens documents.
//
// Threading: today the server is single-threaded — request handlers
// run on the stdio loop's thread. The `Draft` table doesn't need a
// mutex yet. Adding worker threads (e.g. for diagnostics computation
// off the main loop) will need synchronization here; clangd's
// DraftStore guards equivalent state with a single std::mutex.

#include "../../db/db.h"
#include "../../db/ids/ids.h"
#include "../../support/data_structure/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Per-SourceId editor state, indexed by SourceId.idx. `lsp_synced=false`
// means the source either has never been opened or was closed
// (didClose); reads should fall back to disk. `version` mirrors the
// LSP protocol's document version — monotonically increasing per the
// spec. We don't cache (NamespaceId, FileId) here — db_lookup_file_by_source
// already gives us O(n_files) lookup, and that's tiny in practice.
//
// L2: `last_published_diag_hash` is a content hash of the most recent
// publishDiagnostics payload. publish_diagnostics compares against it
// before sending; identical → skip the stdout write. Closes the
// per-keystroke flood where republish_all_open writes N JSON payloads
// for N open files even when only one file's diags actually changed.
// Zero (HASH_NONE) means "never published" — first publish always sends.
//
// M1: `last_published_revision` is the db revision at which we last
// either pushed OR confirmed an L2 hash hit. republish_all_open
// short-circuits the entire typecheck/collect/hash trio for files
// whose last_published_revision equals the current revision — safe
// because salsa guarantees no slot's value can change without a
// revision bump. Catches repeat-republish triggers within one
// revision (didOpen + didChange races, did_change_watched_files
// fan-out) without paying the per-file work twice.
struct Draft {
  bool     lsp_synced;
  int32_t  version;
  uint64_t last_published_diag_hash;
  uint64_t last_published_revision;
};

struct OreDb {
  struct db db;
  Vec drafts; // Vec<struct Draft>, indexed by SourceId.idx
  // M2: mirror of LspState.client_uses_pull. Lives here (not on
  // LspState) so handlers reached via the OreDb pointer can suppress
  // push when the client opts into pull. Set by handle_initialize.
  bool client_uses_pull;
};

void oredb_init(struct OreDb *lsp_db);
void oredb_free(struct OreDb *lsp_db);

// Wire an LSP didOpen event. Canonicalizes the URI, allocates or
// reuses the matching SourceId, sets the text, and flips the draft to
// lsp_synced. Returns the SourceId on success; SOURCE_ID_NONE if the
// URI couldn't be parsed (non-file:// scheme, malformed). Failures
// are silently dropped per spec.
SourceId oredb_did_open(struct OreDb *lsp_db, const char *uri,
                        int32_t version, const char *text, size_t text_len);

// Wire an LSP didChange event. For TextDocumentSyncKind.Full, `text`
// is the entire new document body. Stale events (older version than
// what we have) are dropped with a stderr note and return
// SOURCE_ID_NONE so the caller skips re-typechecking.
SourceId oredb_did_change(struct OreDb *lsp_db, const char *uri,
                          int32_t version, const char *text, size_t text_len);

// Wire an LSP didClose event. Flips lsp_synced=false; we don't evict
// the source bytes since downstream queries may still want them (the
// editor closing a tab doesn't unload disk content). Returns true if
// the URI was previously known.
bool oredb_did_close(struct OreDb *lsp_db, const char *uri);

// Run the full type-check pipeline for the module that owns `src`,
// allocating a fresh (Module, File) pair if this is the first time
// we've seen this source. Returns the FileId so the caller can pull
// per-file diagnostics. Returns FILE_ID_NONE if `src` is invalid.
//
// Idempotent: salsa-style query slots short-circuit on cached calls;
// re-invoking after a text edit revalidates only the affected slots.
//
// L3: out_diags may be NULL (caller doesn't need diags — hover /
// completion / definition flows). When non-NULL, the caller-owned Vec
// is populated with the file's diagnostics; the publish flow consumes
// it directly instead of re-walking db.diag_lists. Diag.args memory is
// owned by the producing query's arena and stays valid until the next
// recompute on that slot — caller MUST consume the Vec before any
// subsequent edit-triggered recompute.
FileId oredb_typecheck(struct OreDb *lsp_db, SourceId src, Vec *out_diags);

// Hover + completion live in src/ide/ — typed-state pure reads, no
// protocol concerns. LSP server handlers do URI→SourceId→FileId
// resolution then call ide_hover_at / ide_completions_at directly.

#endif
