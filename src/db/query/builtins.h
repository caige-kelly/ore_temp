#ifndef ORE_DB_QUERY_BUILTINS_H
#define ORE_DB_QUERY_BUILTINS_H

#include <stdbool.h>
#include <stddef.h>

#include "../db.h"
#include "../diag/diag.h"            // DiagAnchor
#include "../intern_pool/intern_pool.h"
#include "../../syntax/syntax.h"

// Builtin dispatch table — plan Phase 3c.
//
// Today: one entry (@import). The table replaces the hardcoded
// if (name.idx == s->names.IMPORT.idx) chain in type_of_expr.c so
// future comptime work can add @sizeOf, @TypeOf, @compileError,
// @embedFile, @cImport, @field, @hasField, etc. without touching
// type_of_expr.c. Each new builtin is a single table row + one
// handler function.
//
// V1 CAVEAT: this is a dispatch centralization mechanism, NOT a
// finalized semantic builtin model. The `evaluates_args` boolean
// will likely grow into a richer enum (lazy args, partially
// evaluated args, type-position vs value-position, AST rewrites)
// as the comptime engine matures.

// Dual handler shape — picked by BuiltinEntry.evaluates_args.

// Value-style (evaluates_args = true): args are already typechecked
// by the dispatcher and their IpIndex types passed in. For "normal"
// builtins where eager arg eval is correct (@sizeOf, @TypeOf,
// @compileError-when-given-an-expr, @field, @hasField).
typedef IpIndex (*BuiltinValueHandler)(struct db *s,
                                        NamespaceId caller_nsid,
                                        const IpIndex *arg_types,
                                        size_t n_args,
                                        DiagAnchor span);

// Macro-style (evaluates_args = false): raw SyntaxNode args passed in;
// handler does its own arg interpretation. For builtins that consume
// string LITERALS or syntactic structure that shouldn't be reduced
// through the normal type-eval pipeline — @import, @embedFile,
// @cImport. caller_nsid is essential for path-relative resolution.
typedef IpIndex (*BuiltinMacroHandler)(struct db *s,
                                        NamespaceId caller_nsid,
                                        SyntaxNode *const *arg_nodes,
                                        size_t n_args,
                                        DiagAnchor span);

typedef struct {
  const char *name_literal;  // e.g. "import" (NO @ prefix; matches StrId)
  StrId       cached_name;   // populated lazily on first dispatch
  bool        evaluates_args;
  union {
    BuiltinValueHandler  v;
    BuiltinMacroHandler  m;
  } handler;
  uint8_t min_args;
  uint8_t max_args;          // UINT8_MAX = unbounded
} BuiltinEntry;

// Resolve a builtin call. Returns the result IpIndex, or IP_NONE if
// the name isn't a known builtin (caller emits the unknown-builtin
// diag).
IpIndex db_dispatch_builtin(struct db *s, NamespaceId caller_nsid,
                            StrId name,
                            SyntaxNode *const *arg_nodes,
                            size_t n_args, DiagAnchor span);

#endif // ORE_DB_QUERY_BUILTINS_H
