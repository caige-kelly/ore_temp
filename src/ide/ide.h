#ifndef ORE_IDE_H
#define ORE_IDE_H

// IDE service layer — typed queries for editor / language-server
// features (hover, completion, go-to-def, signature help, etc).
//
// Design contract:
//
//   - Every ide_* function operates on (struct db *, FileId, ...).
//     No protocol concerns (no URIs, no JSON, no LSP types). The LSP
//     server is the protocol translator.
//
//   - Every ide_* function opens its own salsa request boundary so
//     downstream db_query_* calls run against a pinned
//     effective_revision. Callers must NOT pre-open a request.
//
//   - Every ide_* function reads the CACHED state populated by
//     compile_file (oredb_typecheck). They do NOT re-run synthesis —
//     calling sema_type_of_expr from here would emit diagnostics
//     through the centralized diag pipeline, which asserts on an
//     active query frame; the IDE service doesn't have one (and
//     shouldn't emit diags on every hover anyway).
//
//   - Return values are typed (IpIndex, DefId, AstNodeId) — NOT
//     strings, NOT JSON. The LSP server formats them.
//
// The layering:
//
//   LSP server  →  IDE service  →  db queries (cached)
//                     ↑
//          compile_file (populates cache during did_open/did_change)

#include "../db/db.h"
#include "../db/ids/ids.h"
#include "../db/intern_pool/intern_pool.h"
#include "../support/data_structure/arena.h"
#include "../support/data_structure/vec.h"

#include <stddef.h>
#include <stdint.h>

struct db;

// Format a hover description for the cursor position into `buf`.
// Returns bytes written (>=0); 0 means "nothing to show".
// Truncates to buflen and always NUL-terminates if buflen > 0.
//
// Output shape today: "name: T" for resolvable names, just "T" for
// raw expressions. Markdown decoration / docs / signature help are
// layered on top later.
size_t ide_hover_at(struct db *db, FileId fid,
                    uint32_t line0, uint32_t char0,
                    char *buf, size_t buflen);

// One completion candidate. Strings point into the caller-provided
// arena (lifetime owned by the LSP server / test driver — reset after
// serialization).
//
// `kind` mirrors LSP's CompletionItemKind enum (server.c maps these
// directly into the JSON-RPC payload):
//   2  = Method        (fn-typed field)
//   3  = Function      (namespace fn)
//   5  = Field         (struct field, namespace type)
//   6  = Variable      (namespace value/var)
//   10 = Property      (slice .len/.ptr, array .len)
//   20 = EnumMember    (enum variant)
//   21 = Constant      (const decl in a namespace)
typedef struct {
    const char *label;
    const char *detail; // type string ("i32", "fn(i32) i32", ...) or NULL
    int32_t     kind;
} IdeCompletion;

// Build the completion list for a `.` trigger at `(line0, char0)`.
// Walks back from the cursor to find the dot, identifies the receiver
// node, reads its cached IpIndex, enumerates fields/variants/etc by
// type tag.
//
// Returns the number of completions pushed into `out` (already
// initialized by caller to sizeof(IdeCompletion)). 0 means no useful
// context at the cursor.
size_t ide_completions_at(struct db *db, FileId fid,
                          uint32_t line0, uint32_t char0,
                          Arena *arena, Vec *out);

#endif // ORE_IDE_H
