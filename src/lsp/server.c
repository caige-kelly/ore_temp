#include "server.h"

#include "marshal.h"

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
// `initialize`. Today we advertise nothing — Step 1 is just the
// handshake. As features land, fields like textDocumentSync,
// definitionProvider, hoverProvider, foldingRangeProvider, and
// semanticTokensProvider get added here.
static cJSON *build_server_capabilities(void) {
  return cJSON_CreateObject();
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

// Process one JSON-RPC message. Returns:
//   0  → continue the loop
//   1  → clean exit (the `exit` notification, with prior shutdown)
//   -1 → fatal protocol error (signal to outer loop to abort)
static int dispatch(cJSON *msg, LspState *state) {
  const cJSON *method_item = cJSON_GetObjectItemCaseSensitive(msg, "method");
  const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
  bool is_request = id != NULL;

  if (!cJSON_IsString(method_item)) {
    // Could be a response to a request *we* sent (server→client
    // requests aren't implemented yet, so just drop it).
    if (cJSON_GetObjectItem(msg, "result") ||
        cJSON_GetObjectItem(msg, "error"))
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
      send_error(id, LSP_ERR_INVALID_REQUEST,
                 "server has been shut down");
    return 0;
  }

  if (strcmp(method, "initialize") == 0)
    return handle_initialize(id, state) ? 0 : -1;
  if (strcmp(method, "initialized") == 0)
    return 0; // notification, no reply, no state change
  if (strcmp(method, "shutdown") == 0)
    return handle_shutdown(id, state) ? 0 : -1;

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

  for (;;) {
    size_t body_len = 0;
    bool eof = false;
    char *body = lsp_read_message(stdin, &body_len, &eof);
    if (!body) {
      if (eof) {
        // Client closed without sending exit. Treat as 1
        // (abnormal) unless we already saw shutdown.
        return state == LSP_STATE_SHUTDOWN ? 0 : 1;
      }
      return 1;
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

    int rc = dispatch(msg, &state);
    cJSON_Delete(msg);

    if (rc == 1)
      return 0; // clean exit
    if (rc == -1)
      return 1; // protocol fatal
  }
}
