#include "builtins.h"

#include "../db.h"
#include "../diag/diag.h"
#include "../workspace/workspace.h"
#include "../../ast/ast_expr.h"
#include "../../syntax/syntax_kind.h"
#include "../../syntax/syntax.h"
#include "../../support/data_structure/stringpool.h"

#include <string.h>

// db_query_namespace_type lives in the type layer (no per-query header; the
// import handler returns the imported file's namespace export type).
extern IpIndex db_query_namespace_type(db_query_ctx *ctx, NamespaceId nsid);

// === @import handler ===========================================
//
// Macro-style: takes a single string-literal arg ("./b.ore"),
// resolves it relative to the caller's file via the workspace
// coordinator, returns the target file's namespace struct type
// (built lazily by db_query_namespace_type). Returns IP_NONE on
// resolution failure — the caller emits the diag.
static IpIndex builtin_import(struct db *s, NamespaceId caller_nsid,
                              SyntaxNode *const *arg_nodes,
                              size_t n_args, DiagAnchor span) {
  (void)span; // diag emission deferred to the caller
  if (n_args < 1 || !arg_nodes[0])
    return IP_NONE;

  // The arg must be a string literal; ast_string_literal_text does the
  // SK_STRING_LIT check + quote-stripping (shared with file_imports'
  // @import extraction so the spelling lives in one place).
  const char *txt;
  uint32_t len;
  if (!ast_string_literal_text(arg_nodes[0], &txt, &len))
    return IP_NONE;
  StrId path = pool_intern(&s->strings, txt, len);

  NamespaceId target = workspace_resolve_import(s, caller_nsid, path);
  if (!namespace_id_valid(target))
    return IP_NONE;

  // The namespace IS a struct type whose fields are the target file's
  // public top-level decls. Field types resolve lazily via
  // db_query_type_of_def at field access.
  return db_query_namespace_type(s, target);
}

// === The table ==================================================
static BuiltinEntry g_builtins[] = {
    {
        .name_literal = "import",
        .cached_name = {0},
        .evaluates_args = false,
        .handler.m = builtin_import,
        .min_args = 1,
        .max_args = 1,
    },
};

static const size_t g_builtins_count =
    sizeof(g_builtins) / sizeof(g_builtins[0]);

IpIndex db_dispatch_builtin(struct db *s, NamespaceId caller_nsid,
                            StrId name,
                            SyntaxNode *const *arg_nodes, size_t n_args,
                            DiagAnchor span) {
  for (size_t i = 0; i < g_builtins_count; i++) {
    BuiltinEntry *e = &g_builtins[i];
    // Lazy-cache the entry's name as a StrId.
    if (e->cached_name.idx == 0) {
      e->cached_name =
          pool_intern(&s->strings, e->name_literal, strlen(e->name_literal));
    }
    if (e->cached_name.idx != name.idx)
      continue;

    if (n_args < e->min_args || n_args > e->max_args) {
      if (!diag_anchor_is_none(span)) {
        db_emit(s, DIAG_ERROR, span,
                "builtin @%s expects %d..%d arguments, got %d", e->name_literal,
                (int32_t)e->min_args, (int32_t)e->max_args, (int32_t)n_args);
      }
      return IP_NONE;
    }

    if (e->evaluates_args) {
      // No value-style builtins implemented yet; loud diag so the gap
      // is visible.
      if (!diag_anchor_is_none(span)) {
        db_emit(s, DIAG_ERROR, span, "builtin @%s is not yet implemented",
                e->name_literal);
      }
      return IP_NONE;
    }
    return e->handler.m(s, caller_nsid, arg_nodes, n_args, span);
  }

  // Unknown builtin name.
  if (!diag_anchor_is_none(span))
    db_emit(s, DIAG_ERROR, span, "unknown builtin @%S", name);
  return IP_NONE;
}
