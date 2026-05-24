#include "builtins.h"

#include "../db/db.h"
#include "../db/diag/diag.h"
#include "../db/query/namespace_type.h"
#include "../db/storage/stringpool.h"
#include "../db/workspace/workspace.h"

#include <string.h>

// === @import handler ===========================================
//
// Macro-style: takes a single string-literal arg ("./b.ore"),
// resolves it relative to the caller's file via the workspace
// coordinator, returns the target file's namespace struct type
// (built lazily by db_query_namespace_type). Returns IP_NONE on
// resolution failure — the caller emits the diag.
static IpIndex builtin_import(struct db *s, NamespaceId caller_nsid,
                              ASTStore *ast, const AstNodeId *arg_nodes,
                              size_t n_args, TinySpan span) {
  (void)span; // diag emission deferred to the caller (see plan 2e)
  if (n_args < 1)
    return IP_NONE;

  AstNodeId arg0 = arg_nodes[0];
  if (arg0.idx == AST_NODE_ID_NONE.idx)
    return IP_NONE;

  AstNodeKind ak = ((AstNodeKind *)ast->kinds.data)[arg0.idx];
  if (ak != AST_EXPR_LIT_STRING)
    return IP_NONE;

  StrId path = ((AstNodeData *)ast->data.data)[arg0.idx].string_id;

  NamespaceId target = workspace_resolve_import(s, caller_nsid, path);
  if (!namespace_id_valid(target))
    return IP_NONE;

  // The namespace IS a struct type whose fields are the target file's
  // public top-level decls (Zig's Namespace.owner_type). Field types
  // are resolved lazily via db_query_type_of_def at field access —
  // see the IP_TAG_NAMESPACE_TYPE branch in type_of_expr.c.
  return db_query_namespace_type(s, target);
}

// === The table ==================================================
//
// One row per builtin. cached_name is filled on first dispatch from
// name_literal — no explicit init pass needed. The table is mutable
// across calls (static lifetime) so the cache persists.
static BuiltinEntry g_builtins[] = {
    {
        .name_literal = "import",
        .cached_name = {0}, // STR_ID_NONE; lazy-init on first dispatch
        .evaluates_args = false,
        .handler.m = builtin_import,
        .min_args = 1,
        .max_args = 1,
    },
    // Future rows (comptime work adds these):
    //   { "sizeOf",       evaluates_args = true,  handler.v = builtin_sizeOf,
    //   1, 1 }, { "TypeOf",       evaluates_args = true,  handler.v =
    //   builtin_typeOf,     1, 1 }, { "compileError", evaluates_args = false,
    //   handler.m = builtin_compile_error, 1, 1 }, { "embedFile",
    //   evaluates_args = false, handler.m = builtin_embed_file, 1, 1 }, {
    //   "cImport",      evaluates_args = false, handler.m = builtin_c_import,
    //   1, 1 }, { "field",        evaluates_args = true,  handler.v =
    //   builtin_field,      2, 2 }, { "hasField",     evaluates_args = true,
    //   handler.v = builtin_has_field,  2, 2 },
};

static const size_t g_builtins_count =
    sizeof(g_builtins) / sizeof(g_builtins[0]);

IpIndex sema_dispatch_builtin(struct db *s, NamespaceId caller_nsid,
                              ASTStore *ast, StrId name,
                              const AstNodeId *arg_nodes, size_t n_args,
                              TinySpan span) {
  for (size_t i = 0; i < g_builtins_count; i++) {
    BuiltinEntry *e = &g_builtins[i];
    // Lazy-cache the entry's name as a StrId — only paid on first
    // dispatch per builtin. Subsequent lookups are O(table size)
    // u32 comparisons.
    if (e->cached_name.idx == 0) {
      e->cached_name =
          pool_intern(&s->strings, e->name_literal, strlen(e->name_literal));
    }
    if (e->cached_name.idx != name.idx)
      continue;

    if (n_args < e->min_args || n_args > e->max_args) {
      if (span != TINYSPAN_NONE) {
        db_emit(s, DIAG_ERROR, span,
                "builtin @%s expects %d..%d arguments, got %d", e->name_literal,
                (int32_t)e->min_args, (int32_t)e->max_args, (int32_t)n_args);
      }
      return IP_NONE;
    }

    if (e->evaluates_args) {
      // The dispatcher would evaluate each arg AST node to its
      // IpIndex type, then call handler.v. None of today's builtins
      // are value-style, so this path is unreached until comptime
      // work adds @sizeOf et al. Loud diag so the gap is visible:
      // silent IP_NONE here used to cascade into every let-bind that
      // uses @sizeOf / @ptrCast and surface much later as confusing
      // hover-`?`s. See diagnostic-completeness pass.
      if (span != TINYSPAN_NONE) {
        db_emit(s, DIAG_ERROR, span, "builtin @%s is not yet implemented",
                e->name_literal);
      }
      return IP_NONE;
    }
    return e->handler.m(s, caller_nsid, ast, arg_nodes, n_args, span);
  }

  // Unknown builtin name — emit so the user sees "unknown builtin
  // @foo" at the call site instead of an "undefined identifier" or
  // silent ? downstream. The name resolves through the same StrId we
  // already have; format it via %S.
  if (span != TINYSPAN_NONE)
    db_emit(s, DIAG_ERROR, span, "unknown builtin @%S", name);
  return IP_NONE;
}
