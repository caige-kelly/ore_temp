#include "server.h"

#include "db.h"
#include "marshal.h"

#include "../diag/diag.h"

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
  LSP_STATE_PRE_INIT, // before `initialize` request
  LSP_STATE_READY,    // initialize replied; normal request flow
  LSP_STATE_SHUTDOWN, // `shutdown` replied; only `exit` honored
} LspState;

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
  return caps;
}

static cJSON *build_server_info(void) {
  cJSON *info = cJSON_CreateObject();
  cJSON_AddStringToObject(info, "name", "ore-lsp");
  cJSON_AddStringToObject(info, "version", "0.0.1");
  return info;
}

static bool handle_initialize(const cJSON *id, LspState *state) {
  if (*state != LSP_STATE_PRE_INIT) {
    return send_error(id, LSP_ERR_INVALID_REQUEST,
                      "initialize requested twice");
  }
  cJSON *result = cJSON_CreateObject();
  cJSON_AddItemToObject(result, "capabilities", build_server_capabilities());
  cJSON_AddItemToObject(result, "serverInfo", build_server_info());
  *state = LSP_STATE_READY;
  return send_result(id, result);
}

static bool handle_shutdown(const cJSON *id, LspState *state) {
  *state = LSP_STATE_SHUTDOWN;
  // Per spec, shutdown's result is null. cJSON_CreateNull() gives
  // a JSON null literal in the response body.
  return send_result(id, cJSON_CreateNull());
}

// === Diagnostic publishing ===
//
// LSP convention: lines and characters in a Position are
// 0-indexed. Ore's Span uses 1-indexed line/column, so we
// subtract 1 (clamping to 0 for missing positions). Character
// offsets are UTF-16 code units by default; we emit byte offsets
// today since Ore identifiers are ASCII in every fixture we
// have. When non-ASCII source becomes a thing, advertise
// `positionEncoding: "utf-8"` in initialize capabilities to
// negotiate UTF-8 with capable clients, and add a UTF-16
// conversion path for the rest.

static int clamp_pos(int v) { return v > 0 ? v - 1 : 0; }

static cJSON *position_json(int line, int column) {
  cJSON *p = cJSON_CreateObject();
  cJSON_AddNumberToObject(p, "line", clamp_pos(line));
  cJSON_AddNumberToObject(p, "character", clamp_pos(column));
  return p;
}

static cJSON *range_for_span(const struct Span *span) {
  cJSON *r = cJSON_CreateObject();
  cJSON_AddItemToObject(r, "start", position_json(span->line, span->column));
  // Some sites set only `line`/`column`; in that case fall back
  // to a zero-length range at the start position rather than
  // shipping a negative range that breaks clients.
  int line_end = span->line_end > 0 ? span->line_end : span->line;
  int col_end = span->column_end > 0 ? span->column_end : span->column;
  cJSON_AddItemToObject(r, "end", position_json(line_end, col_end));
  return r;
}

// Map our DiagSeverity to LSP DiagnosticSeverity. LSP values:
//   1 = Error, 2 = Warning, 3 = Information, 4 = Hint.
// We don't emit Hint today; DIAG_NOTE maps to Information.
static int lsp_severity(DiagSeverity s) {
  switch (s) {
  case DIAG_ERROR:
    return 1;
  case DIAG_WARNING:
    return 2;
  case DIAG_NOTE:
    return 3;
  }
  return 1;
}

// Build the `params` payload for a publishDiagnostics notification:
//   { uri, version, diagnostics: [{range, severity, source, message}] }
// `iid` filters which diagnostics belong to this document; today
// almost all diagnostics carry a span tied to the input we just
// typechecked, but filtering is cheap defense against future
// cross-input diagnostics leaking into the wrong file.
static cJSON *build_publish_params(struct OreDb *db, InputId iid,
                                   const char *uri, int32_t version) {
  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "uri", uri);
  cJSON_AddNumberToObject(params, "version", version);
  cJSON *diags = cJSON_AddArrayToObject(params, "diagnostics");

  // Diagnostics live on per-slot accumulators (sema queries) plus the
  // sema-global bag (parse-time / IO). Collect both, filtered by the
  // current file's id. Sort is built into diag_collect_all so the
  // publish payload is deterministic. pass_arena backs the temp bag —
  // it gets reset between requests so this allocation is transient.
  struct DiagBag collected = diag_bag_new(&db->sema.pass_arena);
  diag_collect_all(&db->sema, &collected, /*file_id_filter=*/(int)iid.idx);
  if (!collected.diags)
    return params;
  for (size_t i = 0; i < collected.diags->count; i++) {
    struct Diag *d = (struct Diag *)vec_get(collected.diags, i);
    if (!d->has_span)
      continue;
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddItemToObject(entry, "range", range_for_span(&d->span));
    cJSON_AddNumberToObject(entry, "severity", lsp_severity(d->severity));
    cJSON_AddStringToObject(entry, "source", "ore");
    cJSON_AddStringToObject(entry, "message", d->msg);
    cJSON_AddItemToArray(diags, entry);
  }
  return params;
}

// Send a textDocument/publishDiagnostics notification for `iid`.
// The version is read from the Draft so the client can correlate
// against the document state it's currently displaying.
static void publish_diagnostics(struct OreDb *db, InputId iid,
                                const char *uri) {
  if (!input_id_is_valid(iid) || iid.idx >= db->drafts.count)
    return;
  struct Draft *d = (struct Draft *)vec_get(&db->drafts, iid.idx);

  cJSON *msg = cJSON_CreateObject();
  cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
  cJSON_AddStringToObject(msg, "method", "textDocument/publishDiagnostics");
  cJSON_AddItemToObject(msg, "params",
                        build_publish_params(db, iid, uri, d->version));
  send_message(msg);
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
  // Grab json object
  const cJSON *td = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
  if (!cJSON_IsObject(td)) {
    fprintf(stderr, "lsp: didOpen missing textDocument\n");
    return;
  }

  // Grab uri
  const char *uri = json_string_or_null(td, "uri");

  // Grab text
  const cJSON *text_item = cJSON_GetObjectItemCaseSensitive(td, "text");
  if (!uri || !cJSON_IsString(text_item)) {
    fprintf(stderr, "lsp: didOpen missing uri or text\n");
    return;
  }

  // Grab version
  int32_t version = (int32_t)json_int_or_default(td, "version", 0);
  const char *text = text_item->valuestring;

  oredb_did_open(lsp_db, uri, text, strlen(text));

  // SourceId sid = = db_source_set_text(&db->db, uri, text);
  // InputId iid = oredb_did_open(db, uri, version, text, strlen(text));
  // if (!input_id_is_valid(iid))
  //   return;
  // oredb_typecheck(db, iid);
  // publish_diagnostics(db, iid, uri);
}

static void handle_did_change(const cJSON *params, struct OreDb *lsp_db) {
  // --- LSP JSON ---
  const cJSON *td = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
  if (!cJSON_IsObject(td)) return;
  
  const char *uri = json_string_or_null(td, "uri");
  int32_t version = (int32_t)json_int_or_default(td, "version", 0);
  if (!uri) return;

  const cJSON *changes = cJSON_GetObjectItemCaseSensitive(params, "contentChanges");
  if (!cJSON_IsArray(changes) || cJSON_GetArraySize(changes) == 0) return;

  const cJSON *last = cJSON_GetArrayItem(changes, cJSON_GetArraySize(changes) - 1);
  const cJSON *text_item = cJSON_GetObjectItemCaseSensitive(last, "text");
  if (!cJSON_IsString(text_item)) return;
  
  const char *text = text_item->valuestring;

  // --- LOGIC ---
  
  // Push the state into the database
  SourceId src = oredb_did_change(lsp_db, uri, version, text, strlen(text));
  if (!source_id_valid(src)) {
    return; // Stale network packet or unknown file, do nothing
  }

  // Look up the semantic FileId for this raw Source bytes
  FileId fid = db_file_for_source(&lsp_db->db, src);
  if (!file_id_valid(fid)) {
      return; 
  }

  // Trigger the top-level query. 
  DiagnosticBag *diags = query_file_diagnostics(&lsp_db->db, fid);
  
  // Send the results back to the editor
  publish_diagnostics(lsp_db, fid, uri, diags);
}

static void handle_did_close(const cJSON *params, struct OreDb *db) {
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
  oredb_did_close(db, uri);
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
    return *state == LSP_STATE_SHUTDOWN ? 1 : -1;

  // Before initialize, only `initialize` is accepted. Other
  // requests get ServerNotInitialized; notifications get dropped.
  if (*state == LSP_STATE_PRE_INIT && strcmp(method, "initialize") != 0) {
    if (is_request)
      send_error(id, LSP_ERR_SERVER_NOT_INITIALIZED,
                 "server has not been initialized");
    return 0;
  }

  // After shutdown, requests get InvalidRequest; notifications
  // are dropped. Only `exit` (handled above) gets us out.
  if (*state == LSP_STATE_SHUTDOWN) {
    if (is_request)
      send_error(id, LSP_ERR_INVALID_REQUEST, "server has been shut down");
    return 0;
  }

  if (strcmp(method, "initialize") == 0)
    return handle_initialize(id, state) ? 0 : -1;
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

  LspState state = LSP_STATE_PRE_INIT;
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
        rc = state == LSP_STATE_SHUTDOWN ? 0 : 1;
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
