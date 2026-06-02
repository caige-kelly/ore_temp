#include "builtins.h"

#include "../../ast/ast_expr.h"
#include "../../support/data_structure/stringpool.h"
#include "../../syntax/syntax.h"
#include "../db.h"
#include "../diag/diag.h"
#include "../workspace/workspace.h"

#include <stdint.h>

// db_query_namespace_type lives in the type layer (no per-query header).
extern IpIndex db_query_namespace_type(db_query_ctx *ctx, NamespaceId nsid);

// === Per-kind metadata ===========================================
// Indexed by BuiltinKind — one row per BUILTIN_LIST entry in
// names.inc. Designated initializers so reordering BUILTIN_LIST
// can't silently misalign rows.
static const BuiltinMeta g_builtin_meta[BUILTIN_KIND_COUNT] = {
    [BUILTIN_SIZEOF]   = {.min_args = 1, .max_args = 1, .evaluates_args = false},
    [BUILTIN_ALIGNOF]  = {.min_args = 1, .max_args = 1, .evaluates_args = false},
    [BUILTIN_TYPEOF]   = {.min_args = 1, .max_args = 1, .evaluates_args = true},
    [BUILTIN_TYPENAME] = {.min_args = 1, .max_args = 1, .evaluates_args = false},
    // @intCast(T, v) / @ptrCast(T, v) — arg-0 is type-position, arg-1 is
    // value-position. infer.c's SK_BUILTIN_EXPR handler walks them per-arg
    // before dispatch and returns arg-0's resolved type.
    [BUILTIN_INTCAST]  = {.min_args = 2, .max_args = 2, .evaluates_args = false},
    [BUILTIN_PTRCAST]  = {.min_args = 2, .max_args = 2, .evaluates_args = false},
    [BUILTIN_IMPORT]   = {.min_args = 1, .max_args = 1, .evaluates_args = false},
};

// === Name → kind ================================================
// Linear scan over the pre-interned StrId.idx set. For N ≤ ~30
// this is faster than a hash map (no hash compute, the whole table
// fits in one cache line).
const BuiltinMeta *db_builtin_meta(BuiltinKind k) {
  if ((int)k < 0 || (int)k >= (int)BUILTIN_KIND_COUNT)
    return NULL;
  return &g_builtin_meta[k];
}

BuiltinKind db_builtin_kind_of(struct db *s, StrId name) {
#define X(id, _name)                                                           \
  if (name.idx == s->names.id.idx)                                             \
    return BUILTIN_##id;
  BUILTIN_LIST(X)
#undef X
  return BUILTIN_KIND_UNKNOWN;
}

// === Handlers ===================================================
//
// @import — macro-style: takes a single string-literal arg, resolves
// it relative to the caller's file via the workspace coordinator,
// returns the target file's namespace struct type (built lazily by
// db_query_namespace_type). The dep edge on NAMESPACE_TYPE(target)
// lands automatically because we're running inside the importer's
// infer_body frame.
static IpIndex builtin_import(struct db *s, NamespaceId caller_nsid,
                              SyntaxNode *const *arg_nodes, size_t n_args,
                              DiagAnchor span) {
  (void)span;
  if (n_args < 1 || !arg_nodes[0])
    return IP_NONE;

  const char *txt;
  uint32_t len;
  if (!ast_string_literal_text(arg_nodes[0], &txt, &len))
    return IP_NONE;
  StrId path = pool_intern(&s->strings, txt, len);

  NamespaceId target = workspace_resolve_import(s, caller_nsid, path);
  if (!namespace_id_valid(target))
    return IP_NONE;

  return db_query_namespace_type(s, target);
}

// @sizeOf / @alignOf — type-only: return IP_COMPTIME_INT_TYPE for any
// arg. Value computation (actual bytes / alignment) and arg-as-type
// validation both block on Phase-6's comptime evaluator. Returning
// comptime_int now is enough to unblock `c :: @sizeOf(T)` and
// `c : u32 :: @sizeOf(T)` — the surrounding bind site coerces the value
// to the target int width.
static IpIndex builtin_sizeof_alignof(struct db *s, NamespaceId caller_nsid,
                                      SyntaxNode *const *args, size_t n,
                                      DiagAnchor span) {
  (void)s;
  (void)caller_nsid;
  (void)args;
  (void)n;
  (void)span;
  return IP_COMPTIME_INT_TYPE;
}

// @TypeOf(x) — returns the type-of-types. Zig: returns the resolved
// type of its argument. The infer.c pre-dispatch type_of_expr call
// records the arg's type in the node-types map for hover; this handler
// returns IP_TYPE_TYPE (the type whose value IS a type). Whether the
// returned type is "i32" or just "type" matters at value level; for
// hover/compose we hand back `type` and let the consumer use the
// node-types map for the concrete type.
static IpIndex builtin_typeof(struct db *s, NamespaceId caller_nsid,
                              SyntaxNode *const *args, size_t n,
                              DiagAnchor span) {
  (void)s; (void)caller_nsid; (void)args; (void)n; (void)span;
  return IP_TYPE_TYPE;
}

// @typeName(T) — returns []const u8 (Zig: [:0]const u8; ore drops the
// sentinel). Like @sizeOf, the value is deferred to Phase-6; the type
// alone is enough for binding sites and call chains.
static IpIndex builtin_typename(struct db *s, NamespaceId caller_nsid,
                                SyntaxNode *const *args, size_t n,
                                DiagAnchor span) {
  (void)s; (void)caller_nsid; (void)args; (void)n; (void)span;
  return IP_STRING_SLICE_TYPE;
}

// @intCast / @ptrCast — result types computed by infer.c BEFORE dispatch
// (these handlers can't see the resolved target — would need SemaCtx
// access). Returning IP_NONE; infer.c short-circuits before reaching
// here. If the stub fires, infer.c lost its special-case.
static IpIndex builtin_cast_stub(struct db *s, NamespaceId caller_nsid,
                                 SyntaxNode *const *args, size_t n,
                                 DiagAnchor span) {
  (void)s; (void)caller_nsid; (void)args; (void)n; (void)span;
  return IP_NONE; // infer.c short-circuits before reaching here.
}

// Stub for kinds whose handlers haven't landed yet. Emits a loud
// diag so the gap is visible at every call site instead of silently
// typing to IP_NONE.
static IpIndex emit_unimplemented(struct db *s, const char *name,
                                  DiagAnchor span) {
  if (!diag_anchor_is_none(span)) {
    db_emit(s, DIAG_ERROR, span, "builtin @%s is not yet implemented", name);
  }
  return IP_NONE;
}

IpIndex db_dispatch_builtin(struct db *s, NamespaceId caller_nsid,
                            BuiltinKind k, SyntaxNode *const *arg_nodes,
                            size_t n_args, DiagAnchor span) {
  const BuiltinMeta *m = &g_builtin_meta[k];
  if (n_args < m->min_args || n_args > m->max_args) {
    if (!diag_anchor_is_none(span)) {
      db_emit(s, DIAG_ERROR, span, "builtin expects %d..%d arguments, got %d",
              (int32_t)m->min_args, (int32_t)m->max_args, (int32_t)n_args);
    }
    return IP_NONE;
  }

  // Sealed switch — -Wswitch-enum flags missing arms when BUILTIN_LIST
  // grows. BUILTIN_KIND_COUNT has its own case so the warning doesn't
  // fire on the sentinel, but it can never be passed in (dispatcher
  // contract).
  switch (k) {
  case BUILTIN_IMPORT:
    return builtin_import(s, caller_nsid, arg_nodes, n_args, span);
  case BUILTIN_SIZEOF:
    return builtin_sizeof_alignof(s, caller_nsid, arg_nodes, n_args, span);
  case BUILTIN_ALIGNOF:
    return builtin_sizeof_alignof(s, caller_nsid, arg_nodes, n_args, span);
  case BUILTIN_TYPEOF:
    return builtin_typeof(s, caller_nsid, arg_nodes, n_args, span);
  case BUILTIN_TYPENAME:
    return builtin_typename(s, caller_nsid, arg_nodes, n_args, span);
  case BUILTIN_INTCAST:
  case BUILTIN_PTRCAST:
    return builtin_cast_stub(s, caller_nsid, arg_nodes, n_args, span);
  case BUILTIN_KIND_COUNT:
    break;
  }
  return IP_NONE;
}
