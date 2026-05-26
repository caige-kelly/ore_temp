#include "../db/db.h"
#include "../db/diag/diag.h"
#include "../db/query/decl_ast.h"
#include "../db/query/def_identity.h"
#include "../db/query/query.h"
#include "../support/data_structure/stringpool.h"
#include "../syntax/syntax.h"
#include "sema.h"

// Walk a namespace's internal scope after typecheck and emit a WARNING
// for each top-level decl with zero references that isn't exported or
// `main`.
//
// Architecture:
//   - Not a salsa query. The previous QUERY_UNUSED_WARNINGS wrapper had
//     a cache-staleness pathology (zero deps, durability-LOW slot, empty
//     dep walk returns CACHED on whitespace edits → no re-emit → stale
//     diags retained). Now this is a plain post-typecheck function.
//   - Diags route via db_emit_to(QUERY_NAMESPACE_SCOPES, nsid.idx). That
//     unit re-runs naturally when the module's exported set changes;
//     otherwise we explicitly clear-and-re-emit at the top of every
//     invocation so unused warnings always reflect the current ref_count.
//   - Anchors are TinySpan(file, byte range); sema re-emits unused
//     diags every cycle, so re-emission tracks edits via fresh
//     spans rather than via reparse-stable identity.
void sema_emit_unused_diagnostics(struct db *s, NamespaceId nsid) {
  ScopeId internal = db_get_namespace_internal_scope(s, nsid);
  if (internal.idx == SCOPE_ID_NONE.idx)
    return;

  // Clear any prior unused-diags so this cycle's emissions are fresh.
  // QUERY_NAMESPACE_SCOPES is the routing unit; module_exports currently
  // doesn't db_emit (verified by audit), so this clear has no collateral.
  // If that changes, route post-typecheck warnings to a dedicated unit.
  db_diags_clear(s, QUERY_NAMESPACE_SCOPES, (uint64_t)nsid.idx);

  uint32_t lo = *(uint32_t *)vec_get(&s->scopes.decl_lo, internal.idx);
  uint32_t len = *(uint32_t *)vec_get(&s->scopes.decl_len, internal.idx);

  // Lazy-lookup the StrId for "main" — exempted from unused warnings.
  // pool_lookup returns the sentinel (idx=0) if "main" was never
  // interned in this db, which means there's no `main` decl to exempt
  // anyway, so the comparison stays correct.
  StrId main_name = pool_lookup(&s->strings, "main", 4);

  uint32_t fc = 0;
  const FileId *files = db_get_namespace_files(s, nsid, &fc);
  if (fc == 0)
    return;

  for (uint32_t i = 0; i < len; i++) {
    DeclEntry *de = (DeclEntry *)vec_get(&s->scopes.decl_pool, lo + i);
    if (de->name.idx == 0)
      continue;
    if (main_name.idx != 0 && de->name.idx == main_name.idx)
      continue;

    DefId def = db_query_def_identity(s, nsid, de->node_ptr);
    if (!def_id_valid(def))
      continue;

    DefMeta meta = *(DefMeta *)vec_get(&s->defs.meta, def.idx);
    if ((meta & META_VIS_MASK) == VIS_PUBLIC)
      continue;

    uint32_t refs = *(uint32_t *)vec_get(&s->defs.ref_count, def.idx);
    if (refs != 0)
      continue;

    // Find the decl's wrapper in any of its backing files. Anchor is
    // a TinySpan derived directly from the SyntaxNodePtr's byte range.
    TinySpan anchor = TINYSPAN_NONE;
    for (uint32_t f = 0; f < fc && anchor == TINYSPAN_NONE; f++) {
      SyntaxNodePtr ptr = db_query_decl_ast(s, files[f], de->node_ptr);
      if (ptr.kind != SYNTAX_KIND_NONE) {
        anchor = span_make((uint16_t)file_id_local(files[f]),
                           ptr.range.start, ptr.range.length);
      }
    }
    if (anchor == TINYSPAN_NONE)
      continue;

    db_emit_to(s, QUERY_NAMESPACE_SCOPES, (uint64_t)nsid.idx, DIAG_WARNING,
               anchor, "%S is declared but never used", de->name);
  }
}
