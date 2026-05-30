#ifndef ORE_DB_QUERY_BUILTINS_H
#define ORE_DB_QUERY_BUILTINS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../db.h"
#include "../diag/diag.h"
#include "../intern_pool/intern_pool.h"
#include "../../syntax/syntax.h"

// Builtin dispatch — Zig-aligned sealed enum + switch. The enum and
// the per-kind name lookup are generated from the BUILTIN_LIST X-
// macro in names.inc (same row that pre-interns the name onto
// s->names), so the three never drift.
//
// Adding a builtin:
//   1. Add a row to BUILTIN_LIST in names.inc → enum entry +
//      pre-interned s->names.<ID> + db_builtin_kind_of arm all
//      generated.
//   2. Add a row to g_builtin_meta[] in builtins.c (min/max args,
//      evaluates_args).
//   3. Add a case to db_dispatch_builtin's switch. -Wswitch-enum
//      flags a missing arm at compile time.

typedef enum {
#define X(id, _name) BUILTIN_##id,
    BUILTIN_LIST(X)
#undef X
    BUILTIN_KIND_COUNT,
} BuiltinKind;

// Sentinel returned by db_builtin_kind_of on miss. Kept OUT of the
// enum so the dispatcher's -Wswitch-enum check remains exhaustive
// across BUILTIN_KIND_COUNT — the caller filters UNKNOWN with its
// own "unknown builtin @%S" diag before dispatching.
#define BUILTIN_KIND_UNKNOWN ((BuiltinKind)-1)

// Per-kind static metadata. Indexed by BuiltinKind; one row in
// g_builtin_meta[] per BUILTIN_LIST entry. max_args = UINT8_MAX
// means unbounded. evaluates_args distinguishes value-style
// (typecheck args first, future @TypeOf/@sizeOf) from macro-style
// (raw nodes, @import).
typedef struct {
  uint8_t min_args;
  uint8_t max_args;
  bool    evaluates_args;
} BuiltinMeta;

// Resolve the identifier after '@' (already interned through the
// workspace string pool) to its BuiltinKind. Returns
// BUILTIN_KIND_UNKNOWN on miss.
BuiltinKind db_builtin_kind_of(struct db *s, StrId name);

// Dispatch a builtin call. Caller has already resolved name → kind
// (filtering UNKNOWN) and collected the raw arg nodes from
// SK_ARG_LIST. The dispatcher checks arg-count against the kind's
// metadata (loud diag on mismatch), then sealed-switches to the
// handler. Returns the result type IpIndex, or IP_NONE on any
// failure (including unimplemented kinds, which emit a "not yet
// implemented" diag).
IpIndex db_dispatch_builtin(struct db *s, NamespaceId caller_nsid,
                            BuiltinKind k,
                            SyntaxNode *const *arg_nodes, size_t n_args,
                            DiagAnchor span);

#endif // ORE_DB_QUERY_BUILTINS_H
