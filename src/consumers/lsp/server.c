#include "server.h"

#include "db.h"
#include "marshal.h"
#include "uri.h"

#include "../../db/db.h"
#include "../../db/diag/diag.h"
#include "../../db/workspace/workspace.h"
#include "../../ide/ide.h"
#include "../../support/data_structure/arena.h"
#include "../../support/data_structure/vec.h"

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// JSON-RPC 2.0 error codes used by LSP. Subset; we only emit the
// ones the handshake actually needs today.
// https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#errorCodes
enum {
  LSP_ERR_PARSE = -32700,
  LSP_ERR_INVALID_REQUEST = -32600,
  LSP_ERR_METHOD_NOT_FOUND = -32601,
  LSP_ERR_INVALID_PARAMS = -32602,
  LSP_ERR_SERVER_NOT_INITIALIZED = -32002,
};

// Protocol state machine. Most servers fold PRE_INIT and
// INIT_REQUESTED into a single "not ready" state; we follow that
// convention since the `initialized` notification carries no
// information we need before READY.
typedef enum {
  LSP_PROTO_PRE_INIT, // before `initialize` request
  LSP_PROTO_READY,    // initialize replied; normal request flow
  LSP_PROTO_SHUTDOWN, // `shutdown` replied; only `exit` honored
} LspProtoState;

// Per-session server state. M2: tracks `client_uses_pull` so we can
// suppress server-pushed publishDiagnostics for clients that advertised
// the LSP 3.17 pull-diagnostics capability (they'll fetch via
// textDocument/diagnostic + workspace/diagnostic instead).
typedef struct {
  LspProtoState proto;
  bool          client_uses_pull;
} LspState;

#define LSP_STATE_INIT { .proto = LSP_PROTO_PRE_INIT, .client_uses_pull = false }

// N1: monotonic id space for server-initiated requests. Replies arrive
// via the same stdin pipe; the dispatcher drops messages carrying
// `result`/`error` (they have no method to route on), so fire-and-
// forget works for requests whose reply we don't need
// (workspace/diagnostic/refresh's reply is null).
static int64_t s_server_request_id = 1;

// Serialize `obj` and frame-write to stdout. Takes ownership of
// `obj` (deletes it after writing). Returns true on success.
static bool send_message(cJSON *obj) {
  char *body = cJSON_PrintUnformatted(obj);
  cJSON_Delete(obj);
  if (!body) {
    fprintf(stderr, "lsp: cJSON_PrintUnformatted failed\n");
    return false;
  }
  size_t len = strlen(body);
  bool ok = lsp_write_message(stdout, body, len);
  free(body);
  return ok;
}

// Build a JSON-RPC response envelope `{jsonrpc, id, result|error}`.
// `id` is copied from the request (number, string, or null);
// callers pass NULL to mean "the request had no id field" but in
// practice that only happens for notifications and notifications
// never get responses.
static cJSON *make_response(const cJSON *id) {
  cJSON *resp = cJSON_CreateObject();
  cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
  if (id)
    cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, /*recurse=*/true));
  else
    cJSON_AddNullToObject(resp, "id");
  return resp;
}

static bool send_error(const cJSON *id, int code, const char *message) {
  cJSON *resp = make_response(id);
  cJSON *err = cJSON_AddObjectToObject(resp, "error");
  cJSON_AddNumberToObject(err, "code", code);
  cJSON_AddStringToObject(err, "message", message);
  return send_message(resp);
}

// N1 — fire-and-forget server-initiated request. Caller transfers
// ownership of `params` (or NULL for no params). The dispatcher
// silently drops the eventual response since we don't track it.
static void send_server_request(const char *method, cJSON *params) {
  cJSON *msg = cJSON_CreateObject();
  cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
  cJSON_AddNumberToObject(msg, "id", (double)s_server_request_id++);
  cJSON_AddStringToObject(msg, "method", method);
  if (params)
    cJSON_AddItemToObject(msg, "params", params);
  send_message(msg);
}

// N3 — `$/progress` notification (LSP §3.16). Streams per-file pieces
// of a workspace-diagnostic report under the client-supplied
// partialResultToken. Token is opaque (number or string); we duplicate
// it into the message. `value` is the payload (a
// WorkspaceDiagnosticReportPartialResult here, but the helper is
// payload-agnostic). Caller transfers ownership of `value`.
static void send_progress(const cJSON *token, cJSON *value) {
  if (!token) {
    cJSON_Delete(value);
    return;
  }
  cJSON *msg = cJSON_CreateObject();
  cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
  cJSON_AddStringToObject(msg, "method", "$/progress");
  cJSON *p = cJSON_AddObjectToObject(msg, "params");
  cJSON_AddItemToObject(p, "token", cJSON_Duplicate(token, true));
  cJSON_AddItemToObject(p, "value", value);
  send_message(msg);
}

static bool send_result(const cJSON *id, cJSON *result) {
  cJSON *resp = make_response(id);
  // cJSON_AddItemToObject transfers ownership; passing `result`
  // directly avoids a copy.
  cJSON_AddItemToObject(resp, "result", result);
  return send_message(resp);
}

// Build the ServerCapabilities object advertised in our reply to
// `initialize`. Today we advertise document sync only — the rest
// (definitionProvider, hoverProvider, foldingRangeProvider,
// semanticTokensProvider, …) land in subsequent steps.
//
// `textDocumentSync.change = 1` is TextDocumentSyncKind.Full. The
// client sends the entire document text on every didChange. We'll
// upgrade to Incremental (=2) when file sizes start to make full
// resends costly; the offset-conversion machinery for incremental
// edits is the only Step-2 thing we're deliberately deferring.
static cJSON *build_server_capabilities(void) {
  cJSON *caps = cJSON_CreateObject();
  cJSON *sync = cJSON_AddObjectToObject(caps, "textDocumentSync");
  cJSON_AddBoolToObject(sync, "openClose", true);
  cJSON_AddNumberToObject(sync, "change", 1);
  cJSON_AddBoolToObject(caps, "hoverProvider", true);
  // Completion: today only `.` triggers (member access). Bare-identifier
  // prefix completion is a future addition — the same handler can serve
  // both once we know the AST context the cursor is in.
  cJSON *comp = cJSON_AddObjectToObject(caps, "completionProvider");
  cJSON *trig = cJSON_AddArrayToObject(comp, "triggerCharacters");
  cJSON_AddItemToArray(trig, cJSON_CreateString("."));
  cJSON_AddBoolToObject(comp, "resolveProvider", false);
  cJSON_AddBoolToObject(caps, "definitionProvider", true);
  // M2 — pull diagnostics (LSP 3.17). interFileDependencies=true because
  // ore's @import means an edit in one file can change another's diags;
  // workspaceDiagnostics=true enables the workspace/diagnostic handler.
  // identifier="ore" lets clients distinguish our diags from other LSP
  // servers' if they multiplex.
  cJSON *diag = cJSON_AddObjectToObject(caps, "diagnosticProvider");
  cJSON_AddStringToObject(diag, "identifier", "ore");
  cJSON_AddBoolToObject(diag, "interFileDependencies", true);
  cJSON_AddBoolToObject(diag, "workspaceDiagnostics", true);
  return caps;
}

static cJSON *build_server_info(void) {
  cJSON *info = cJSON_CreateObject();
  cJSON_AddStringToObject(info, "name", "ore-lsp");
  cJSON_AddStringToObject(info, "version", "0.0.1");
  return info;
}

// Extract the client's diagnostic capability from initialize params.
// Per LSP 3.17 §3.17.18.1, presence of textDocument.diagnostic (any
// non-null object) signals support for pull diagnostics. Spec doesn't
// require dynamicRegistration to be true for the capability to apply.
static bool client_advertises_pull(const cJSON *params) {
  if (!cJSON_IsObject(params)) return false;
  const cJSON *caps = cJSON_GetObjectItemCaseSensitive(params, "capabilities");
  if (!cJSON_IsObject(caps)) return false;
  const cJSON *td = cJSON_GetObjectItemCaseSensitive(caps, "textDocument");
  if (!cJSON_IsObject(td)) return false;
  const cJSON *diag = cJSON_GetObjectItemCaseSensitive(td, "diagnostic");
  return cJSON_IsObject(diag);
}

// N1: workspace.diagnostics.refreshSupport — true if the client will
// accept server-initiated `workspace/diagnostic/refresh` requests. Per
// LSP 3.17 §3.17.18.4, this lives under capabilities.workspace.
// diagnostics, separate from the per-document capability above.
static bool client_advertises_refresh(const cJSON *params) {
  if (!cJSON_IsObject(params)) return false;
  const cJSON *caps = cJSON_GetObjectItemCaseSensitive(params, "capabilities");
  if (!cJSON_IsObject(caps)) return false;
  const cJSON *ws = cJSON_GetObjectItemCaseSensitive(caps, "workspace");
  if (!cJSON_IsObject(ws)) return false;
  const cJSON *diags = cJSON_GetObjectItemCaseSensitive(ws, "diagnostics");
  if (!cJSON_IsObject(diags)) return false;
  const cJSON *rs = cJSON_GetObjectItemCaseSensitive(diags, "refreshSupport");
  return cJSON_IsBool(rs) && cJSON_IsTrue(rs);
}

static bool handle_initialize(const cJSON *id, const cJSON *params,
                              LspState *state, struct OreDb *lsp_db) {
  if (state->proto != LSP_PROTO_PRE_INIT) {
    return send_error(id, LSP_ERR_INVALID_REQUEST,
                      "initialize requested twice");
  }
  state->client_uses_pull = client_advertises_pull(params);
  lsp_db->client_uses_pull = state->client_uses_pull;
  lsp_db->client_supports_refresh = client_advertises_refresh(params);
  cJSON *result = cJSON_CreateObject();
  cJSON_AddItemToObject(result, "capabilities", build_server_capabilities());
  cJSON_AddItemToObject(result, "serverInfo", build_server_info());
  state->proto = LSP_PROTO_READY;
  return send_result(id, result);
}

static bool handle_shutdown(const cJSON *id, LspState *state) {
  state->proto = LSP_PROTO_SHUTDOWN;
  // Per spec, shutdown's result is null. cJSON_CreateNull() gives
  // a JSON null literal in the response body.
  return send_result(id, cJSON_CreateNull());
}

// === Diagnostic publishing ===
//
// LSP Positions are 0-indexed (both line and character). db_resolve_span
// returns 1-indexed line/col (rust-style display convention), so we
// subtract 1 when building the Position. Character offsets are UTF-16
// code units by default in LSP; we emit byte offsets today since Ore
// source is ASCII in every fixture we have. When non-ASCII source
// lands, advertise `positionEncoding: "utf-8"` in initialize
// capabilities and add a UTF-16 conversion path for clients that
// don't accept it.

static int clamp0(int v) { return v > 0 ? v - 1 : 0; }

static cJSON *position_json(uint32_t line_1, uint32_t col_1) {
  cJSON *p = cJSON_CreateObject();
  cJSON_AddNumberToObject(p, "line", clamp0((int)line_1));
  cJSON_AddNumberToObject(p, "character", clamp0((int)col_1));
  return p;
}

// Position from already-0-indexed line/col (IdeLocation's convention).
static cJSON *position_json_0(uint32_t line0, uint32_t col0) {
  cJSON *p = cJSON_CreateObject();
  cJSON_AddNumberToObject(p, "line", (int)line0);
  cJSON_AddNumberToObject(p, "character", (int)col0);
  return p;
}

// Build an LSP Location: { uri, range:{start,end} } from a definition
// resolver's IdeLocation. Returns NULL on path-lookup failure (caller
// should send `result: null` instead).
static cJSON *location_json(struct OreDb *lsp_db, const IdeLocation *loc) {
  SourceId src = db_get_file_source(&lsp_db->db, loc->file);
  if (!source_id_valid(src))
    return NULL;
  StrId path_id = db_get_source_path(&lsp_db->db, src);
  if (path_id.idx == 0)
    return NULL;
  const char *path = pool_get(&lsp_db->db.strings, path_id);
  char *uri = lsp_path_to_uri(path);
  if (!uri)
    return NULL;
  cJSON *out = cJSON_CreateObject();
  cJSON_AddStringToObject(out, "uri", uri);
  free(uri);
  cJSON *range = cJSON_AddObjectToObject(out, "range");
  cJSON_AddItemToObject(range, "start",
                        position_json_0(loc->line_start, loc->col_start));
  cJSON_AddItemToObject(range, "end",
                        position_json_0(loc->line_end, loc->col_end));
  return out;
}

// Build an LSP Range from a DiagAnchor. Uses the caller's DiagResolver
// for sticky-squiggle rebinding via syntax_node_ptr_resolve; the
// resolver's slot-of-one LRU keeps successive resolves for the same
// file at zero allocation cost. Falls back to a zero-length range at
// (1,1) when resolution fails (virtual file, unparsed source, or the
// anchored node was deleted by the user).
static cJSON *range_for_anchor(DiagResolver *r, DiagAnchor anchor) {
  cJSON *out = cJSON_CreateObject();
  ResolvedSpan rs;
  if (diag_resolver_resolve(r, anchor, &rs)) {
    cJSON_AddItemToObject(out, "start", position_json(rs.line, rs.col_start));
    cJSON_AddItemToObject(out, "end", position_json(rs.line, rs.col_end));
  } else {
    cJSON_AddItemToObject(out, "start", position_json(1, 1));
    cJSON_AddItemToObject(out, "end", position_json(1, 1));
  }
  return out;
}

// Map our DiagSeverity to LSP DiagnosticSeverity. LSP values:
//   1 = Error, 2 = Warning, 3 = Information, 4 = Hint.
static int lsp_severity(DiagSeverity s) {
  switch (s) {
  case DIAG_ERROR:
    return 1;
  case DIAG_WARNING:
    return 2;
  case DIAG_INFO:
    return 3;
  case DIAG_HINT:
    return 4;
  }
  return 1;
}

// Build a JSON array of `Diagnostic` items per LSP §5.7 (Diagnostic).
// Shared by push (publishDiagnostics.params.diagnostics) and pull
// (DocumentDiagnosticReport.items / WorkspaceDocumentDiagnosticReport.items).
// L3 / M2: caller passes a pre-collected Vec<Diag> from
// db_collect_diags_for_file — the per-query DiagBundle columns are
// already walked there; this function just renders.
static cJSON *build_diag_items_array(struct OreDb *lsp_db,
                                     const Vec *collected) {
  cJSON *diags = cJSON_CreateArray();
  // Slot-of-one resolver caches this file's red root for the duration
  // of the publish loop; all anchors in `collected` share the same
  // file_id so resolution stays at one tree build per publish.
  DiagResolver dr;
  diag_resolver_init(&dr, &lsp_db->db);
  char msg_buf[512];
  for (size_t i = 0; i < collected->count; i++) {
    Diag *d = (Diag *)vec_get((Vec *)collected, i);
    db_format_diag(&lsp_db->db, d, msg_buf, sizeof(msg_buf));
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddItemToObject(entry, "range", range_for_anchor(&dr, d->anchor));
    cJSON_AddNumberToObject(entry, "severity", lsp_severity(d->severity));
    cJSON_AddStringToObject(entry, "source", "ore");
    cJSON_AddStringToObject(entry, "message", msg_buf);
    // N2 — LSP DiagnosticTag (LSP §5.7). Omit the field when no tag is
    // set; non-empty array signals which clients should treat specially.
    if (d->tag != DIAG_TAG_NONE) {
      cJSON *tags = cJSON_AddArrayToObject(entry, "tags");
      cJSON_AddItemToArray(tags, cJSON_CreateNumber((double)d->tag));
    }
    cJSON_AddItemToArray(diags, entry);
  }
  diag_resolver_free(&dr);
  return diags;
}

// Build the `params` payload for a publishDiagnostics notification:
//   { uri, version, diagnostics: [{range, severity, source, message}] }
// L3: takes the pre-collected Vec from oredb_typecheck —
// db_collect_diags_for_file walked the per-query DiagBundles once;
// this function just renders.
static cJSON *build_publish_params(struct OreDb *lsp_db, const char *uri,
                                   int32_t version, const Vec *collected) {
  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "uri", uri);
  cJSON_AddNumberToObject(params, "version", version);
  cJSON_AddItemToObject(params, "diagnostics",
                        build_diag_items_array(lsp_db, collected));
  return params;
}

// Format a 64-bit hash as a 16-char lowercase hex string (NUL-terminated).
// Used as the opaque LSP `resultId` for pull diagnostics. Caller-owned buf
// must be ≥ 17 bytes.
static void format_result_id(uint64_t h, char *buf) {
  static const char hex[] = "0123456789abcdef";
  for (int i = 15; i >= 0; i--) {
    buf[i] = hex[h & 0xF];
    h >>= 4;
  }
  buf[16] = '\0';
}

// O3 — parse-error presence detector. A diag whose owner_kind is
// QUERY_FILE_AST is a parse-time emission (error recovery, lexer
// gripe). When any such diag is present in the current pull, the
// file is mid-edit-partial-parse: semantic diags from cached slots
// carry stale (pre-edit) anchor byte ranges that ptr_resolve can't
// rebind, so publishing them would overwrite the editor's locally-
// tracked positions with wrong ones ("sticky squiggles").
static bool has_parse_errors(const Vec *diags) {
  for (size_t i = 0; i < diags->count; i++) {
    const Diag *d = (const Diag *)vec_get((Vec *)diags, i);
    if (d->owner_kind == QUERY_FILE_AST)
      return true;
  }
  return false;
}

// L2 — content hash over the publishDiagnostics payload's STABLE
// fields. Skips cJSON serialization (no need to render-then-hash);
// folds each Diag's POD identity (anchor + template + severity + args)
// directly. Two publishes with identical hashes mean the editor would
// see identical diag arrays — safe to skip the write.
//
// The args pointer itself is excluded — it moves across recomputes
// (different arena allocations) — but the args' VALUES are folded by
// walking each DiagArg.
static uint64_t hash_diag_list(const Vec *diags) {
  Fingerprint fp = db_fp_u64((uint64_t)diags->count);
  for (size_t i = 0; i < diags->count; i++) {
    const Diag *d = (const Diag *)vec_get((Vec *)diags, i);
    fp = db_fp_combine(fp, db_fp_bytes(&d->anchor, sizeof(d->anchor)));
    fp = db_fp_combine(fp, db_fp_u64((uint64_t)d->template_id.idx));
    fp = db_fp_combine(
        fp, db_fp_u64(((uint64_t)d->code << 16) |
                      ((uint64_t)d->severity << 8) | (uint64_t)d->n_args));
    for (uint8_t a = 0; a < d->n_args; a++)
      fp = db_fp_combine(fp, db_fp_bytes(&d->args[a], sizeof(d->args[a])));
  }
  return (uint64_t)fp;
}

// Send a textDocument/publishDiagnostics notification for `src`.
// The version is read from the Draft so the client can correlate
// against the document state it's currently displaying.
// L3: caller passes the diag list (populated by oredb_typecheck).
// L2: skips the write when the diag list is byte-for-byte the same as
// the previous publish (cheap content hash compared to the cached
// per-Draft value). Closes the per-keystroke flood: typing in one
// file no longer re-sends N JSON payloads for the other N-1 open
// files whose diags didn't change.
static void publish_diagnostics(struct OreDb *lsp_db, SourceId src,
                                const char *uri, const Vec *diags) {
  if (!source_id_valid(src) || src.idx >= lsp_db->drafts.count)
    return;
  struct Draft *d = (struct Draft *)vec_get(&lsp_db->drafts, src.idx);

  // M1: stamp the publish revision regardless of whether we end up
  // sending — the L2 hash hit case also counts as "we've reconciled
  // this file at this revision," and republish_all_open should skip
  // it on the next call within the same revision.
  d->last_published_revision =
      db_current_revision((db_query_ctx *)&lsp_db->db);

  // O3 — sticky-squiggle gate. If the file's current parse contains
  // errors AND we've already published a state for this Draft, hold
  // the line: the editor's local anchor tracking shifts our last-
  // published squiggles as the user types. Republishing now with
  // potentially-stale anchors would snap them back to wrong byte
  // positions. The first publish (last_published_diag_hash == 0)
  // always sends so the editor gets initial state.
  if (d->last_published_diag_hash != 0 && has_parse_errors(diags))
    return;

  uint64_t h = hash_diag_list(diags);
  if (h == d->last_published_diag_hash && d->last_published_diag_hash != 0)
    return; // unchanged — skip the stdout write entirely
  d->last_published_diag_hash = h ? h : 1; // 0 reserved for "never published"

  cJSON *msg = cJSON_CreateObject();
  cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
  cJSON_AddStringToObject(msg, "method", "textDocument/publishDiagnostics");
  cJSON_AddItemToObject(msg, "params",
                        build_publish_params(lsp_db, uri, d->version, diags));
  send_message(msg);
}

// Copy the subset of `all` anchored to file `file_idx` into `out` (caller-
// initialized). compile_file now returns a file's whole @import closure, so the
// editor-presentation layers route each diag to its own file via this filter.
static void collect_file_diags(const Vec *all, uint32_t file_idx, Vec *out) {
  for (size_t i = 0; i < all->count; i++) {
    Diag *d = (Diag *)vec_get((Vec *)all, i);
    if (d->anchor.file_id == file_idx)
      vec_push(out, d);
  }
}

// Whole-program PUSH diagnostics. compile_file now returns a MULTI-file diag
// list (the target's @import closure, incl. cross-file generic INSTANCES which
// anchor to their defining file). Publishing all of them under the edited
// file's URI would mis-attribute imported errors, so group by anchor file and
// publish each group under ITS OWN URI. Only files the editor currently has
// open (a synced Draft) are published — the PRIMARY (edited) file always, even
// with an empty group, so its squiggles clear when it goes error-free.
//
// TODO(whole-program): also publish to UNOPENED imported files (rust-analyzer
// surfaces whole-crate errors in Problems for files you haven't opened). That
// needs a tracked set of externally-published URIs to clear when they go clean
// — a focused follow-up. Today an import's errors appear once that file is open
// (its own typecheck + the cross-file instances triggered by open importers
// both land under its URI).
static void publish_grouped(struct OreDb *lsp_db, SourceId primary_src,
                            FileId primary_fid, Vec *diags) {
  Vec files; // distinct anchor files, primary first (always publish/clear it)
  vec_init(&files, sizeof(FileId));
  vec_push(&files, &primary_fid);
  for (size_t i = 0; i < diags->count; i++) {
    FileId f = {.idx = ((Diag *)vec_get(diags, i))->anchor.file_id};
    bool seen = false;
    for (size_t v = 0; v < files.count && !seen; v++)
      if (((FileId *)vec_get(&files, v))->idx == f.idx)
        seen = true;
    if (!seen)
      vec_push(&files, &f);
  }
  for (size_t fi = 0; fi < files.count; fi++) {
    FileId f = *(FileId *)vec_get(&files, fi);
    SourceId src = (f.idx == primary_fid.idx)
                       ? primary_src
                       : db_get_file_source(&lsp_db->db, f);
    // Publish only to files with an open (synced) Draft; the primary is open.
    if (!source_id_valid(src) || src.idx >= lsp_db->drafts.count ||
        !((struct Draft *)vec_get(&lsp_db->drafts, src.idx))->lsp_synced)
      continue;
    StrId path_id = db_get_source_path(&lsp_db->db, src);
    const char *path =
        path_id.idx ? pool_get(&lsp_db->db.strings, path_id) : NULL;
    char *uri = (path && path[0]) ? lsp_path_to_uri(path) : NULL;
    if (!uri)
      continue;
    Vec group;
    vec_init(&group, sizeof(Diag));
    collect_file_diags(diags, f.idx, &group);
    publish_diagnostics(lsp_db, src, uri, &group);
    vec_free(&group);
    free(uri);
  }
  vec_free(&files);
}

// Re-typecheck and re-publish diagnostics for EVERY currently-open
// file. Called after any event that mutates db inputs (didOpen,
// didChange, didChangeWatchedFiles) so cross-file consumers see
// fresh errors without typing in their own buffer.
//
// Cost: O(open_files × cached_typecheck_cost). Salsa's caching
// short-circuits each call when nothing changed (~100µs per file
// in typical sessions). Files whose transitive deps changed
// recompute as needed — that's the whole point.
//
// `except_src` skips one source the caller already published (avoids
// double publish on the just-edited file). Pass SOURCE_ID_NONE to
// republish all open files (e.g. for the workspace-watcher path).
static void republish_all_open(struct OreDb *lsp_db, SourceId except_src) {
  // M2.4 + N1: client pulls on its own schedule. With refreshSupport we
  // nudge the client via workspace/diagnostic/refresh so it knows to
  // re-poll. Without it, client falls back to its own polling cadence
  // (idle / focus / timer); stale-until-then is spec-compliant but UX-
  // worse. Refresh is namespace-wide, so we send once per republish_
  // all_open call regardless of how many files changed.
  if (lsp_db->client_uses_pull) {
    if (lsp_db->client_supports_refresh)
      send_server_request("workspace/diagnostic/refresh", NULL);
    return;
  }
  uint64_t cur_rev = db_current_revision((db_query_ctx *)&lsp_db->db);
  for (size_t i = 0; i < lsp_db->drafts.count; i++) {
    struct Draft *d = (struct Draft *)vec_get(&lsp_db->drafts, i);
    if (!d->lsp_synced)
      continue;
    SourceId src = {.idx = (uint32_t)i};
    if (source_id_valid(except_src) && src.idx == except_src.idx)
      continue;
    // M1 — skip the typecheck/collect/hash trio when we already
    // published at this revision. Salsa guarantees no slot's value
    // can change without a revision bump, so the diags can't differ
    // from what we last published.
    if (d->last_published_revision == cur_rev && cur_rev != 0)
      continue;
    Vec diags;
    vec_init(&diags, sizeof(Diag));
    FileId fid = oredb_typecheck(lsp_db, src, &diags);
    if (!file_id_valid(fid)) {
      vec_free(&diags);
      continue;
    }
    publish_grouped(lsp_db, src, fid, &diags);
    vec_free(&diags);
  }
}

// === textDocument/* notifications ===
//
// These are notifications (no id, no reply). We route them into
// OreDb which keeps the editor-side view of each document in sync
// with sema. Parse failures (missing fields, wrong types) are
// logged and dropped — the client doesn't get a response either
// way since these aren't requests.

static const char *json_string_or_null(const cJSON *obj, const char *field) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, field);
  return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int json_int_or_default(const cJSON *obj, const char *field,
                               int fallback) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, field);
  return cJSON_IsNumber(v) ? (int)v->valuedouble : fallback;
}

static void handle_did_open(const cJSON *params, struct OreDb *lsp_db) {
  const cJSON *td = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
  if (!cJSON_IsObject(td)) {
    fprintf(stderr, "lsp: didOpen missing textDocument\n");
    return;
  }
  const char *uri = json_string_or_null(td, "uri");
  const cJSON *text_item = cJSON_GetObjectItemCaseSensitive(td, "text");
  if (!uri || !cJSON_IsString(text_item)) {
    fprintf(stderr, "lsp: didOpen missing uri or text\n");
    return;
  }
  int32_t version = (int32_t)json_int_or_default(td, "version", 0);
  const char *text = text_item->valuestring;

  SourceId src = oredb_did_open(lsp_db, uri, version, text, strlen(text));
  if (!source_id_valid(src))
    return;

  Vec diags;
  vec_init(&diags, sizeof(Diag));
  FileId fid = oredb_typecheck(lsp_db, src, &diags);
  if (!file_id_valid(fid)) {
    vec_free(&diags);
    return;
  }
  // M2.4: skip push for pull-capable clients.
  if (!lsp_db->client_uses_pull)
    publish_grouped(lsp_db, src, fid, &diags);
  vec_free(&diags);
  // Newly-opened file may invalidate exports of other open files
  // (lazy load discovered new content). Republish for the rest.
  republish_all_open(lsp_db, src);
}

static void handle_did_change(const cJSON *params, struct OreDb *lsp_db) {
  const cJSON *td = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
  if (!cJSON_IsObject(td))
    return;

  const char *uri = json_string_or_null(td, "uri");
  int32_t version = (int32_t)json_int_or_default(td, "version", 0);
  if (!uri)
    return;

  // TextDocumentSyncKind.Full: each contentChange's `text` is the full
  // document body. The spec allows multiple changes per notification —
  // the last one is authoritative under Full sync.
  const cJSON *changes =
      cJSON_GetObjectItemCaseSensitive(params, "contentChanges");
  if (!cJSON_IsArray(changes) || cJSON_GetArraySize(changes) == 0)
    return;
  const cJSON *last =
      cJSON_GetArrayItem(changes, cJSON_GetArraySize(changes) - 1);
  const cJSON *text_item = cJSON_GetObjectItemCaseSensitive(last, "text");
  if (!cJSON_IsString(text_item))
    return;
  const char *text = text_item->valuestring;

  SourceId src = oredb_did_change(lsp_db, uri, version, text, strlen(text));
  if (!source_id_valid(src))
    return;

  Vec diags;
  vec_init(&diags, sizeof(Diag));
  FileId fid = oredb_typecheck(lsp_db, src, &diags);
  if (!file_id_valid(fid)) {
    vec_free(&diags);
    return;
  }
  // M2.4: skip push for pull-capable clients.
  if (!lsp_db->client_uses_pull)
    publish_grouped(lsp_db, src, fid, &diags);
  vec_free(&diags);
  // Cross-file invalidation: editing this file may break (or fix)
  // diagnostics in other open files that @import it. Salsa's
  // forward-dep tracking does the right thing per file; we just
  // need to ask it to re-run for each open file.
  republish_all_open(lsp_db, src);
}

// === textDocument/hover (request) ===
//
// Request, not notification — has an `id` and expects a Hover result
// (or null when there's nothing to show). LSP Hover shape:
//   { contents: { kind: "markdown", value: "<text>" } }
// We don't emit a range yet; clients render a tooltip at the cursor.
static void handle_hover(const cJSON *id, const cJSON *params,
                         struct OreDb *lsp_db) {
  const cJSON *td = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
  const cJSON *pos = cJSON_GetObjectItemCaseSensitive(params, "position");
  const char *uri = json_string_or_null(td, "uri");
  if (!uri || !cJSON_IsObject(pos)) {
    send_result(id, cJSON_CreateNull());
    return;
  }
  int line0 = json_int_or_default(pos, "line", -1);
  int char0 = json_int_or_default(pos, "character", -1);
  if (line0 < 0 || char0 < 0) {
    send_result(id, cJSON_CreateNull());
    return;
  }

  char *path = lsp_uri_to_path(uri);
  if (!path) {
    send_result(id, cJSON_CreateNull());
    return;
  }
  SourceId src = db_lookup_source_by_path(&lsp_db->db, path, strlen(path));
  free(path);
  if (!source_id_valid(src)) {
    send_result(id, cJSON_CreateNull());
    return;
  }

  // Ensure typecheck has run for this source so body_scopes and the
  // type queries are populated. Cheap on cached calls.
  FileId fid = oredb_typecheck(lsp_db, src, NULL);

  char hover[512];
  size_t n = ide_hover_at(&lsp_db->db, fid, (uint32_t)line0, (uint32_t)char0,
                          hover, sizeof hover);
  if (n == 0) {
    send_result(id, cJSON_CreateNull());
    return;
  }

  cJSON *result = cJSON_CreateObject();
  cJSON *contents = cJSON_AddObjectToObject(result, "contents");
  cJSON_AddStringToObject(contents, "kind", "markdown");
  cJSON_AddStringToObject(contents, "value", hover);
  send_result(id, result);
}

// === textDocument/completion (request) ===
//
// Cursor in `foo.<here>` style — return the receiver type's fields /
// variants / properties as CompletionItem entries. Trigger char is `.`
// (set in build_server_capabilities). Bare-identifier prefix completion
// is future work; for now this only fires on `.` and returns null
// otherwise — `oredb_completion` returns 0 when context doesn't match.
static void handle_completion(const cJSON *id, const cJSON *params,
                              struct OreDb *lsp_db) {
  const cJSON *td = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
  const cJSON *pos = cJSON_GetObjectItemCaseSensitive(params, "position");
  const char *uri = json_string_or_null(td, "uri");
  if (!uri || !cJSON_IsObject(pos)) {
    send_result(id, cJSON_CreateNull());
    return;
  }
  int line0 = json_int_or_default(pos, "line", -1);
  int char0 = json_int_or_default(pos, "character", -1);
  if (line0 < 0 || char0 < 0) {
    send_result(id, cJSON_CreateNull());
    return;
  }

  char *path = lsp_uri_to_path(uri);
  if (!path) {
    send_result(id, cJSON_CreateNull());
    return;
  }
  SourceId src = db_lookup_source_by_path(&lsp_db->db, path, strlen(path));
  free(path);
  if (!source_id_valid(src)) {
    send_result(id, cJSON_CreateNull());
    return;
  }

  // Make sure typecheck has run + per-node type cache is populated.
  FileId fid = oredb_typecheck(lsp_db, src, NULL);

  // Per-request scratch arena — completion-item strings live here.
  // Reset (free chunks) after we've serialized the response.
  Arena arena;
  arena_init(&arena, 4 * 1024);

  Vec items;
  vec_init(&items, sizeof(IdeCompletion));

  size_t n = ide_completions_at(&lsp_db->db, fid, (uint32_t)line0,
                                (uint32_t)char0, &arena, &items);
  if (n == 0) {
    vec_free(&items);
    arena_free(&arena);
    send_result(id, cJSON_CreateNull());
    return;
  }

  // CompletionList shape — sticking with the array variant for
  // simplicity (LSP allows either CompletionItem[] or CompletionList).
  cJSON *result = cJSON_CreateArray();
  for (size_t i = 0; i < items.count; i++) {
    IdeCompletion *c = (IdeCompletion *)vec_get(&items, i);
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "label", c->label ? c->label : "");
    cJSON_AddNumberToObject(item, "kind", c->kind);
    if (c->detail && c->detail[0])
      cJSON_AddStringToObject(item, "detail", c->detail);
    cJSON_AddItemToArray(result, item);
  }
  vec_free(&items);
  arena_free(&arena);
  send_result(id, result);
}

// === textDocument/definition (request) ===
//
// Resolve the cursor identifier to its definition's source location.
// Covers body-local refs, top-level refs, and cross-namespace member
// access through @import'd namespaces (see ide_definition_at for the
// resolution rules). Returns a single Location on hit, null on miss.
static void handle_definition(const cJSON *id, const cJSON *params,
                              struct OreDb *lsp_db) {
  const cJSON *td = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
  const cJSON *pos = cJSON_GetObjectItemCaseSensitive(params, "position");
  const char *uri = json_string_or_null(td, "uri");
  if (!uri || !cJSON_IsObject(pos)) {
    send_result(id, cJSON_CreateNull());
    return;
  }
  int line0 = json_int_or_default(pos, "line", -1);
  int char0 = json_int_or_default(pos, "character", -1);
  if (line0 < 0 || char0 < 0) {
    send_result(id, cJSON_CreateNull());
    return;
  }

  char *path = lsp_uri_to_path(uri);
  if (!path) {
    send_result(id, cJSON_CreateNull());
    return;
  }
  SourceId src = db_lookup_source_by_path(&lsp_db->db, path, strlen(path));
  free(path);
  if (!source_id_valid(src)) {
    send_result(id, cJSON_CreateNull());
    return;
  }

  FileId fid = oredb_typecheck(lsp_db, src, NULL);

  IdeLocation loc;
  if (!ide_definition_at(&lsp_db->db, fid, (uint32_t)line0, (uint32_t)char0,
                         &loc)) {
    send_result(id, cJSON_CreateNull());
    return;
  }

  cJSON *result = location_json(lsp_db, &loc);
  send_result(id, result ? result : cJSON_CreateNull());
}

// M2.2 — textDocument/diagnostic (LSP 3.17 §3.17.18).
// Request: { textDocument: { uri }, identifier?, previousResultId? }
// Response:
//   { kind: "full", resultId, items: Diagnostic[] }
//   { kind: "unchanged", resultId }
// previousResultId is opaque server state echoed back; if equal to the
// current resultId, we return `unchanged` (saves the wire bytes for
// the diag array). Reuses L2's content hash directly as the resultId
// (hex-formatted) — same skip semantics as the push path.
static void handle_text_document_diagnostic(const cJSON *id,
                                            const cJSON *params,
                                            struct OreDb *lsp_db) {
  const cJSON *td = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
  const char *uri = json_string_or_null(td, "uri");
  const cJSON *prev = cJSON_GetObjectItemCaseSensitive(params, "previousResultId");
  const char *prev_id = cJSON_IsString(prev) ? prev->valuestring : NULL;
  if (!uri) {
    send_error(id, LSP_ERR_INVALID_PARAMS, "missing textDocument.uri");
    return;
  }
  char *path = lsp_uri_to_path(uri);
  if (!path) {
    send_error(id, LSP_ERR_INVALID_PARAMS, "unparseable uri");
    return;
  }
  SourceId src = db_lookup_source_by_path(&lsp_db->db, path, strlen(path));
  free(path);
  if (!source_id_valid(src)) {
    // No record of this file — return an empty full report so the client
    // clears any stale diagnostics for it. resultId = "0".
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "kind", "full");
    cJSON_AddStringToObject(result, "resultId", "0");
    cJSON_AddItemToObject(result, "items", cJSON_CreateArray());
    send_result(id, result);
    return;
  }

  Vec all;
  vec_init(&all, sizeof(Diag));
  FileId fid = oredb_typecheck(lsp_db, src, &all);
  Vec diags; // whole-program: route this pull to only the requested file's diags
  vec_init(&diags, sizeof(Diag));
  if (file_id_valid(fid))
    collect_file_diags(&all, fid.idx, &diags);
  vec_free(&all);
  uint64_t h = hash_diag_list(&diags);
  char rid[17];
  format_result_id(h ? h : 1, rid);

  struct Draft *d = (struct Draft *)vec_get(&lsp_db->drafts, src.idx);
  d->last_published_revision =
      db_current_revision((db_query_ctx *)&lsp_db->db);

  cJSON *result = cJSON_CreateObject();
  if (prev_id && strcmp(prev_id, rid) == 0) {
    // Unchanged — client already has these.
    cJSON_AddStringToObject(result, "kind", "unchanged");
    cJSON_AddStringToObject(result, "resultId", rid);
    vec_free(&diags);
    send_result(id, result);
    return;
  }
  d->last_published_diag_hash = h ? h : 1;
  cJSON_AddStringToObject(result, "kind", "full");
  cJSON_AddStringToObject(result, "resultId", rid);
  cJSON_AddItemToObject(result, "items",
                        build_diag_items_array(lsp_db, &diags));
  vec_free(&diags);
  send_result(id, result);
}

// M2.3 — workspace/diagnostic (LSP 3.17 §3.17.18.4).
// Request: { identifier?, previousResultIds: { uri, value }[] }
// Response: WorkspaceDiagnosticReport { items: WorkspaceDocumentDiagnosticReport[] }
//   Each item: { kind: "full"|"unchanged", uri, version, resultId, items? }
// One-shot (not partial-result streaming): defer until workspace pull
// latency becomes user-visible.
static const char *workspace_pull_prev_id(const cJSON *prev_ids,
                                          const char *uri) {
  if (!cJSON_IsArray(prev_ids))
    return NULL;
  int n = cJSON_GetArraySize(prev_ids);
  for (int i = 0; i < n; i++) {
    const cJSON *e = cJSON_GetArrayItem(prev_ids, i);
    if (!cJSON_IsObject(e))
      continue;
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(e, "uri");
    if (!cJSON_IsString(u) || strcmp(u->valuestring, uri) != 0)
      continue;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(e, "value");
    return cJSON_IsString(v) ? v->valuestring : NULL;
  }
  return NULL;
}

static void handle_workspace_diagnostic(const cJSON *id, const cJSON *params,
                                        struct OreDb *lsp_db) {
  const cJSON *prev_ids =
      params ? cJSON_GetObjectItemCaseSensitive(params, "previousResultIds")
             : NULL;
  // N3 — if the client supplied a progress token, stream per-file
  // reports via $/progress notifications and reply with an empty
  // final report. Otherwise one-shot with the full items array.
  const cJSON *prog_tok =
      params ? cJSON_GetObjectItemCaseSensitive(params, "partialResultToken")
             : NULL;
  bool streaming = (prog_tok != NULL);
  uint64_t cur_rev = db_current_revision((db_query_ctx *)&lsp_db->db);

  cJSON *report = cJSON_CreateObject();
  cJSON *items = cJSON_AddArrayToObject(report, "items");

  for (size_t i = 0; i < lsp_db->drafts.count; i++) {
    struct Draft *d = (struct Draft *)vec_get(&lsp_db->drafts, i);
    if (!d->lsp_synced)
      continue;
    SourceId src = {.idx = (uint32_t)i};
    StrId path_id = db_get_source_path(&lsp_db->db, src);
    const char *path = path_id.idx ? pool_get(&lsp_db->db.strings, path_id)
                                   : NULL;
    if (!path || !path[0])
      continue;
    char *uri = lsp_path_to_uri(path);
    if (!uri)
      continue;

    Vec all;
    vec_init(&all, sizeof(Diag));
    FileId fid = oredb_typecheck(lsp_db, src, &all);
    Vec diags; // whole-program: route each report to only that file's own diags
    vec_init(&diags, sizeof(Diag));
    if (file_id_valid(fid))
      collect_file_diags(&all, fid.idx, &diags);
    vec_free(&all);
    uint64_t h = hash_diag_list(&diags);
    char rid[17];
    format_result_id(h ? h : 1, rid);
    d->last_published_revision = cur_rev;

    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "uri", uri);
    cJSON_AddNumberToObject(entry, "version", d->version);
    cJSON_AddStringToObject(entry, "resultId", rid);

    const char *prev = workspace_pull_prev_id(prev_ids, uri);
    if (prev && strcmp(prev, rid) == 0) {
      cJSON_AddStringToObject(entry, "kind", "unchanged");
    } else {
      cJSON_AddStringToObject(entry, "kind", "full");
      cJSON_AddItemToObject(entry, "items",
                            build_diag_items_array(lsp_db, &diags));
      d->last_published_diag_hash = h ? h : 1;
    }
    if (streaming) {
      // Wrap this per-file report in a
      // WorkspaceDiagnosticReportPartialResult { items: [entry] } and
      // ship as a $/progress notification.
      cJSON *partial = cJSON_CreateObject();
      cJSON *partial_items = cJSON_AddArrayToObject(partial, "items");
      cJSON_AddItemToArray(partial_items, entry);
      send_progress(prog_tok, partial);
    } else {
      cJSON_AddItemToArray(items, entry);
    }
    vec_free(&diags);
    free(uri);
  }
  // Streaming mode: final response carries an empty items array; all
  // per-file reports went via $/progress. One-shot mode: items has
  // everything.
  send_result(id, report);
}

static void handle_did_close(const cJSON *params, struct OreDb *lsp_db) {
  const cJSON *td = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
  if (!cJSON_IsObject(td)) {
    fprintf(stderr, "lsp: didClose missing textDocument\n");
    return;
  }
  const char *uri = json_string_or_null(td, "uri");
  if (!uri) {
    fprintf(stderr, "lsp: didClose missing uri\n");
    return;
  }
  oredb_did_close(lsp_db, uri);
}

// workspace/didChangeWatchedFiles — fires when files in the watched
// glob change on disk (git checkout, external editor, codegen, etc.).
// params.changes is an array of { uri, type } where type is:
//   1 = Created  — sema will lazy-load on next @import; we drop these
//   2 = Changed  — re-slurp from disk + invalidate cached parse
//   3 = Deleted  — evict the source row (mark + bump revision)
// Unknown paths (sources we've never lazy-loaded) silently no-op.
static void handle_did_change_watched_files(const cJSON *params,
                                            struct OreDb *lsp_db) {
  const cJSON *changes = cJSON_GetObjectItemCaseSensitive(params, "changes");
  if (!cJSON_IsArray(changes))
    return;

  const cJSON *change = NULL;
  cJSON_ArrayForEach(change, changes) {
    const char *uri = json_string_or_null(change, "uri");
    const cJSON *type_item = cJSON_GetObjectItemCaseSensitive(change, "type");
    if (!uri || !cJSON_IsNumber(type_item))
      continue;
    int type = (int)type_item->valuedouble;

    char *path = lsp_uri_to_path(uri);
    if (!path)
      continue;
    size_t path_len = strlen(path);

    switch (type) {
    case 1: // Created — sema will lazy-load on next @import
      break;
    case 2: // Changed — re-slurp + invalidate
      if (!workspace_did_change_external(&lsp_db->db, path, path_len, NULL,
                                         0)) {
        // Slurp failed (file deleted between event and read);
        // fall through to evict.
        workspace_did_evict_source(&lsp_db->db, path, path_len);
      }
      break;
    case 3: // Deleted
      workspace_did_evict_source(&lsp_db->db, path, path_len);
      break;
    default:
      break;
    }
    free(path);
  }

  // External changes can invalidate any open file's typecheck. Once
  // all events are processed, re-typecheck and re-publish every open
  // document. No "primary" file to skip — pass SOURCE_ID_NONE.
  republish_all_open(lsp_db, SOURCE_ID_NONE);
}

// Process one JSON-RPC message. Returns:
//   0  → continue the loop
//   1  → clean exit (the `exit` notification, with prior shutdown)
//   -1 → fatal protocol error (signal to outer loop to abort)
static int dispatch(cJSON *msg, LspState *state, struct OreDb *db) {
  const cJSON *method_item = cJSON_GetObjectItemCaseSensitive(msg, "method");
  const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
  bool is_request = id != NULL;

  if (!cJSON_IsString(method_item)) {
    // Could be a response to a request *we* sent (server→client
    // requests aren't implemented yet, so just drop it).
    if (cJSON_GetObjectItem(msg, "result") || cJSON_GetObjectItem(msg, "error"))
      return 0;
    if (is_request)
      send_error(id, LSP_ERR_INVALID_REQUEST, "missing method");
    return 0;
  }
  const char *method = method_item->valuestring;

  // `exit` is special — always honored, regardless of state.
  if (strcmp(method, "exit") == 0)
    return state->proto == LSP_PROTO_SHUTDOWN ? 1 : -1;

  // Before initialize, only `initialize` is accepted. Other
  // requests get ServerNotInitialized; notifications get dropped.
  if (state->proto == LSP_PROTO_PRE_INIT && strcmp(method, "initialize") != 0) {
    if (is_request)
      send_error(id, LSP_ERR_SERVER_NOT_INITIALIZED,
                 "server has not been initialized");
    return 0;
  }

  // After shutdown, requests get InvalidRequest; notifications
  // are dropped. Only `exit` (handled above) gets us out.
  if (state->proto == LSP_PROTO_SHUTDOWN) {
    if (is_request)
      send_error(id, LSP_ERR_INVALID_REQUEST, "server has been shut down");
    return 0;
  }

  if (strcmp(method, "initialize") == 0) {
    const cJSON *init_params = cJSON_GetObjectItemCaseSensitive(msg, "params");
    return handle_initialize(id, init_params, state, db) ? 0 : -1;
  }
  if (strcmp(method, "initialized") == 0)
    return 0; // notification, no reply, no state change
  if (strcmp(method, "shutdown") == 0)
    return handle_shutdown(id, state) ? 0 : -1;

  // Document-sync notifications. These have no `id` and produce
  // no response; the client doesn't expect a reply.
  const cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
  if (strcmp(method, "textDocument/didOpen") == 0) {
    handle_did_open(params, db);
    return 0;
  }
  if (strcmp(method, "textDocument/didChange") == 0) {
    handle_did_change(params, db);
    return 0;
  }
  if (strcmp(method, "textDocument/didClose") == 0) {
    handle_did_close(params, db);
    return 0;
  }
  if (strcmp(method, "workspace/didChangeWatchedFiles") == 0) {
    handle_did_change_watched_files(params, db);
    return 0;
  }
  if (strcmp(method, "textDocument/hover") == 0) {
    if (is_request)
      handle_hover(id, params, db);
    return 0;
  }
  if (strcmp(method, "textDocument/completion") == 0) {
    if (is_request)
      handle_completion(id, params, db);
    return 0;
  }
  if (strcmp(method, "textDocument/definition") == 0) {
    if (is_request)
      handle_definition(id, params, db);
    return 0;
  }
  // M2.2 — LSP 3.17 pull diagnostics (per-file).
  if (strcmp(method, "textDocument/diagnostic") == 0) {
    if (is_request)
      handle_text_document_diagnostic(id, params, db);
    return 0;
  }
  // M2.3 — LSP 3.17 pull diagnostics (whole-workspace).
  if (strcmp(method, "workspace/diagnostic") == 0) {
    if (is_request)
      handle_workspace_diagnostic(id, params, db);
    return 0;
  }

  // Unknown method. Requests get MethodNotFound; notifications
  // are dropped silently per spec ("$" prefix or otherwise).
  if (is_request)
    send_error(id, LSP_ERR_METHOD_NOT_FOUND, method);
  return 0;
}

int lsp_server_run(void) {
  // Make stdio binary-safe. LSP uses \r\n in headers; on Windows
  // (and some Unix tools) implicit CRLF translation would
  // corrupt the framing. Body bytes are UTF-8 JSON and must pass
  // through untouched.
  setvbuf(stdout, NULL, _IONBF, 0);

  LspState state = LSP_STATE_INIT;
  struct OreDb db;
  oredb_init(&db);

  int rc = 0;
  for (;;) {
    size_t body_len = 0;
    bool eof = false;
    char *body = lsp_read_message(stdin, &body_len, &eof);
    if (!body) {
      if (eof) {
        // Client closed without sending exit. Treat as 1
        // (abnormal) unless we already saw shutdown.
        rc = state.proto == LSP_PROTO_SHUTDOWN ? 0 : 1;
      } else {
        rc = 1;
      }
      break;
    }

    cJSON *msg = cJSON_ParseWithLength(body, body_len);
    free(body);
    if (!msg) {
      // Parse error — reply only if we can dig out an id, which
      // we can't from unparseable JSON. Best-effort error reply
      // with null id per JSON-RPC spec.
      cJSON *resp = make_response(NULL);
      cJSON *err = cJSON_AddObjectToObject(resp, "error");
      cJSON_AddNumberToObject(err, "code", LSP_ERR_PARSE);
      cJSON_AddStringToObject(err, "message", "parse error");
      send_message(resp);
      continue;
    }

    int step = dispatch(msg, &state, &db);
    cJSON_Delete(msg);

    if (step == 1) {
      rc = 0; // clean exit
      break;
    }
    if (step == -1) {
      rc = 1; // protocol fatal
      break;
    }
  }

  oredb_free(&db);
  return rc;
}
