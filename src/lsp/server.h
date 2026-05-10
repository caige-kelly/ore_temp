#ifndef ORE_LSP_SERVER_H
#define ORE_LSP_SERVER_H

// LSP server entry point. Reads framed JSON-RPC messages from
// stdin, writes responses to stdout. Diagnostics and tracing go
// to stderr so they don't corrupt the protocol stream.
//
// Today this is a minimal handshake: `initialize` → reply with
// capabilities → wait for `initialized` notification → handle
// `shutdown` + `exit`. Document sync, diagnostics, definition,
// hover, folding ranges, and semantic tokens get layered on in
// subsequent PRs.
//
// Returns 0 on clean shutdown (saw shutdown then exit), non-zero
// on protocol error or unexpected EOF.

int lsp_server_run(void);

#endif
