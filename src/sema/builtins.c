#include "builtins.h"

#include "../ast/ast_expr.h"
#include "../db/db.h"
#include "../db/diag/diag.h"
#include "../db/query/namespace_type.h"
#include "../db/workspace/workspace.h"
#include "../parser/syntax_kind.h"
#include "../support/data_structure/stringpool.h"
#include "../syntax/syntax.h"

#include <string.h>

// === @import handler ===========================================
//
// Macro-style: takes a single string-literal arg ("./b.ore"),
// resolves it relative to the caller's file via the workspace
// coordinator, returns the target file's namespace struct type
// (built lazily by db_query_namespace_type). Returns IP_NONE on
// resolution failure — the caller emits the diag.
static IpIndex builtin_import(struct db *s, NamespaceId caller_nsid,
                              SyntaxNode *const *arg_nodes,
                              size_t n_args, TinySpan span) {
  (void)span; // diag emission deferred to the caller
  if (n_args < 1 || !arg_nodes[0])
    return IP_NONE;

  // The arg must be a string literal (SK_LITERAL_EXPR wrapping SK_STRING_LIT).
  Literal lit;
  if (!Literal_cast(arg_nodes[0], &lit) ||
      Literal_kind(&lit) != SK_STRING_LIT)
    return IP_NONE;

  SyntaxToken *tok = Literal_token(&lit);
  if (!tok)
    return IP_NONE;
  const char *txt = syntax_token_text(tok);
  uint32_t len = syntax_token_text_range(tok).length;
  // Strip surrounding quotes if present.
  StrId path;
  if (len >= 2 && txt[0] == '"' && txt[len - 1] == '"')
    path = pool_intern(&s->strings, txt + 1, len - 2);
  else
    path = pool_intern(&s->strings, txt, len);
  syntax_token_release(tok);

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

IpIndex sema_dispatch_builtin(struct db *s, NamespaceId caller_nsid,
                              StrId name,
                              SyntaxNode *const *arg_nodes, size_t n_args,
                              TinySpan span) {
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
      if (span != TINYSPAN_NONE) {
        db_emit(s, DIAG_ERROR, span,
                "builtin @%s expects %d..%d arguments, got %d", e->name_literal,
                (int32_t)e->min_args, (int32_t)e->max_args, (int32_t)n_args);
      }
      return IP_NONE;
    }

    if (e->evaluates_args) {
      // No value-style builtins implemented yet; loud diag so the gap
      // is visible.
      if (span != TINYSPAN_NONE) {
        db_emit(s, DIAG_ERROR, span, "builtin @%s is not yet implemented",
                e->name_literal);
      }
      return IP_NONE;
    }
    return e->handler.m(s, caller_nsid, arg_nodes, n_args, span);
  }

  // Unknown builtin name.
  if (span != TINYSPAN_NONE)
    db_emit(s, DIAG_ERROR, span, "unknown builtin @%S", name);
  return IP_NONE;
}
