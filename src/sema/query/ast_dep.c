#include "ast_dep.h"

#include "../../lexer/token.h"  // struct Span
#include "../ids/ids.h"
#include "../modules/modules.h"  // query_module_ast, module_for_span
#include "../scope/scope.h"
#include "../sema.h"

void record_ast_dep_for_def(struct Sema *s, DefId def) {
  if (!s) return;
  struct DefInfo *di = def_info(s, def);
  if (!di) return;
  if (!scope_id_is_valid(di->owner_scope)) return;
  struct ScopeInfo *si = scope_info(s, di->owner_scope);
  if (!si) return;
  if (!module_id_is_valid(si->owner_module)) return;
  (void)query_module_ast(s, si->owner_module);
}

void record_ast_dep_for_span(struct Sema *s, struct Span span) {
  if (!s) return;
  ModuleId mid = module_for_span(s, span);
  if (module_id_is_valid(mid))
    (void)query_module_ast(s, mid);
}
