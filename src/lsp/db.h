#ifndef ORE_LSP_DB_H
#define ORE_LSP_DB_H

// LSP-server-owned database wrapper around `struct Sema`. Adds
// per-document editor state (`Draft`) on top of sema's input
// table; sema itself already interns paths and dedups InputIds.
//
// Lifecycle: one OreDb per server process. Created on `initialize`,
// torn down on `exit`. Holds a long-lived `struct Sema` whose
// inputs accumulate as the editor opens documents.
//
// Threading: today the server is single-threaded — request handlers
// run on the stdio loop's thread. The `Draft` table doesn't need a
// mutex yet. Adding worker threads (e.g. for diagnostics computation
// off the main loop) will need synchronization here; clangd's
// DraftStore guards equivalent state with a single std::mutex.

#include "../common/vec.h"
#include "../sema/ids/ids.h"
#include "../sema/modules/inputs.h"
#include "../sema/sema.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Per-InputId editor state. `lsp_synced=false` means the input has
// either never been opened or was closed (didClose); reads should
// fall back to disk. `version` mirrors the LSP protocol's
// document version — monotonically increasing per the spec.
// `mid` caches the ModuleId allocated for this input on first
// typecheck; MODULE_ID_INVALID until then.
struct Draft {
  bool lsp_synced;
  int32_t version;
  ModuleId mid;
};

struct OreDb {
  struct Sema sema;
  Vec drafts; // Vec<struct Draft>, indexed by InputId.idx
};

void oredb_init(struct OreDb *db);
void oredb_free(struct OreDb *db);

// Wire an LSP didOpen event. Canonicalizes the URI, allocates or
// reuses the matching InputId, copies `text` into sema, and flips
// the draft to lsp_synced. Returns the InputId on success;
// INPUT_ID_INVALID if the URI couldn't be parsed (non-file://
// scheme, malformed). Failures are silently dropped per spec.
InputId oredb_did_open(struct OreDb *db, const char *uri, int32_t version,
                       const char *text, size_t text_len);

// Wire an LSP didChange event. For TextDocumentSyncKind.Full,
// `text` is the entire new document body. Stale events (older
// version than what we have) are dropped with a stderr note and
// return INPUT_ID_INVALID so the caller skips re-typechecking.
InputId oredb_did_change(struct OreDb *db, const char *uri, int32_t version,
                         const char *text, size_t text_len);

// Wire an LSP didClose event. Flips lsp_synced=false; we don't
// evict the source bytes from sema since downstream queries may
// still want them (the editor closing a tab doesn't unload disk
// content). Returns true if the URI was previously known.
bool oredb_did_close(struct OreDb *db, const char *uri);

// Run the build pipeline (def_map → scope_index → typecheck) for
// `iid`'s module, clearing sema's diag bag first so the resulting
// diagnostics describe only this revision. Allocates the ModuleId
// on first call and caches it in the Draft. Returns the ModuleId
// (which the caller passes to oredb_module_for_input for filtering
// diagnostics). Returns MODULE_ID_INVALID if `iid` is unknown.
ModuleId oredb_typecheck(struct OreDb *db, InputId iid);

#endif
