#ifndef ORE_LSP_URI_H
#define ORE_LSP_URI_H

// `file://` URI → canonical filesystem path.
//
// LSP identifies documents by URI (e.g.
// `file:///Users/foo/bar.ore`). Internally we key everything on
// canonical absolute paths so that:
//   - symlinks resolve to the same identity as the real file
//   - path differences from URL encoding (`%20` etc.) don't split
//     identity
//   - `..` / `.` segments are normalized
//
// Returns a malloc'd null-terminated string on success; caller frees.
// Returns NULL if the URI isn't a `file://` scheme or canonicalization
// fails. The fallback (file exists in URI but not on disk yet — e.g.
// a fresh untitled document being saved as .ore for the first time)
// is to do a lexical decode + absolutize without realpath.
//
// Today we only handle `file://`. Non-`file` schemes (`untitled:`,
// `vscode-vfs:`, etc.) return NULL — the server logs and drops the
// event.

char *lsp_uri_to_path(const char *uri);

#endif
