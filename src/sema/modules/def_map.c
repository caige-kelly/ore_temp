#include "def_map.h"

#include <stddef.h>

#include "../../parser/ast.h"
#include "../scope/scope.h"
#include "../sema.h"
#include "modules.h"

// SemanticKind classification from the Bind's RHS shape. Defaults to
// SEM_VALUE — functions, constants, and "we don't know yet" all map
// to value-namespace lookups. Type/effect classification is purely
// syntactic at the def-map level; deeper checks (e.g. that an
// expr_Lambda actually returns a value) happen at typecheck.
static SemanticKind sem_for_bind_value(struct Expr *value) {
  if (!value)
    return SEM_VALUE;
  switch (value->kind) {
  case expr_Struct:
  case expr_Enum:
    return SEM_TYPE;
  case decl_Effect:
    return SEM_EFFECT;
  default:
    return SEM_VALUE;
  }
}

// Register one top-level expression into the module's scopes.
// Currently handles `expr_Bind` and `expr_DestructureBind`. Other
// top-level forms (loose calls, comptime blocks) don't introduce
// names; they're ignored by def-map and surface in the module body
// at sema_check time.
static bool collect_one(struct Sema *s, struct ModuleInfo *m,
                        struct Expr *e) {
  if (!e)
    return true;

  switch (e->kind) {
  case expr_Bind: {
    struct BindExpr *b = &e->bind;
    struct DefInfo proto = {
        .kind = DECL_USER,
        .semantic_kind = sem_for_bind_value(b->value),
        .name_id = b->name.string_id,
        .span = b->name.span,
        .origin_id = e->id,
        .origin = e,
        .owner_scope = m->internal_scope,
        .child_scope = SCOPE_ID_INVALID,
        .imported_module = MODULE_ID_INVALID,
        .vis = b->visibility,
        .scope_token_id = 0,
        .is_comptime = e->is_comptime,
        .has_effects = false,
    };
    DefId def = def_create(s, proto);
    if (!scope_insert_def(s, m->internal_scope, def)) {
      // Duplicate top-level name. Diagnostic emission lives in
      // diag/codes.h (E-code TBD when that header lands); for now
      // we register the failure by returning false.
      return false;
    }
    if (b->visibility == Visibility_public)
      scope_insert_def(s, m->export_scope, def);
    return true;
  }

  case expr_DestructureBind:
    // Top-level destructure-binds (`(a, b) := pair()`) are rare but
    // legal. Each pattern leaf becomes its own DefId; for now we
    // don't recurse into the pattern. Plumbing lands when we have
    // tests covering this case.
    return true;

  case decl_Effect:
    // Bare top-level `effect X { ... }` — the parser also wraps
    // these in a Bind for naming, so the bind path above handles
    // the named case. The naked decl_Effect case is unusual and
    // gets a follow-up when we exercise it in tests.
    return true;

  default:
    // Loose top-level expression. Doesn't introduce a name; module
    // body will hold it at sema_check.
    return true;
  }
}

bool def_map_collect_top_level(struct Sema *s, ModuleId mid) {
  struct ModuleInfo *m = module_info(s, mid);
  if (!m || !m->ast)
    return true;

  bool ok = true;
  for (size_t i = 0; i < m->ast->count; i++) {
    struct Expr **slot = (struct Expr **)vec_get(m->ast, i);
    if (!slot)
      continue;
    if (!collect_one(s, m, *slot))
      ok = false;
  }
  return ok;
}
