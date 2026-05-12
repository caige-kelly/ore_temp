#ifndef ORE_LSP_MARSHAL_H
#define ORE_LSP_MARSHAL_H

// LSP JSON-RPC message framing.
//
// Every message is preceded by a header block terminated by an
// empty line (\r\n\r\n). The only header we use today is
// `Content-Length: N`, where N is the byte length of the body
// that follows. `Content-Type` is permitted by the spec but
// optional; we ignore it on read and never emit it on write.
//
// Body format is JSON-RPC 2.0. This module only handles the
// framing layer — JSON parsing lives in src/lsp/server.c with
// cJSON.

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

// Read one framed message from `in`. On success, returns a
// malloc'd buffer holding the message body (null-terminated for
// convenience; the actual byte length is written to `*out_len`).
// Caller frees with free().
//
// Returns NULL on EOF or framing error. On framing error, prints
// a diagnostic to stderr; on clean EOF (no bytes available), sets
// `*out_eof` to true and returns NULL with no diagnostic.
char *lsp_read_message(FILE *in, size_t *out_len, bool *out_eof);

// Write `body` (length `len`) to `out` framed with a Content-Length
// header. Flushes so the client doesn't stall on stdio buffering.
// Returns true on success.
bool lsp_write_message(FILE *out, const char *body, size_t len);

#endif
