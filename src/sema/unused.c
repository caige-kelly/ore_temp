#include "../db/db.h"
#include "../db/diag/diag.h"
#include "../db/query/decl_ast.h"
#include "../db/query/def_identity.h"
#include "../db/query/query.h"
#include "../db/storage/stringpool.h"
#include "sema.h"

// Walk a namespace's internal scope after typecheck and emit a WARNING
// for each top-level decl with zero references that isn't exported or
// `main`. The ref_count column on db.defs is maintained inside
// db_query_resolve_ref — every successful name resolution increments
// the resolved def's count; re-runs decrement-old + increment-new in
// lockstep.
//
// Why post-typecheck and not a salsa query: the resolution graph isn't
// fully settled until every per-decl query (type_of_def, infer_body)
// has had a chance to drive db_query_resolve_ref via type_of_expr.
// We emit fresh diags each typecheck and let the diag pipeline clear
// stale ones via the QUERY_NAMESPACE_SCOPES unit key.
void sema_emit_unused_diagnostics(struct db *s, NamespaceId nsid) {
  // Open a synthetic salsa frame so db_emit can route diags to this
  // analysis unit (QUERY_UNUSED_WARNINGS, nsid). Cleared on every
  // re-run via db_query_begin's slot-clear path.
  QueryBeginResult __qbr =
      db_query_begin(s, QUERY_UNUSED_WARNINGS, (uint64_t)nsid.idx);
  if (__qbr == QUERY_BEGIN_CACHED || __qbr == QUERY_BEGIN_CYCLE ||
      __qbr == QUERY_BEGIN_ERROR)
    return;

  ScopeId internal = db_get_namespace_internal_scope(s, nsid);
  if (internal.idx == SCOPE_ID_NONE.idx) {
    db_query_succeed(s, QUERY_UNUSED_WARNINGS, (uint64_t)nsid.idx,
                     FINGERPRINT_NONE);
    return;
  }
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

    DefId def = db_query_def_identity(s, nsid, de->ast_id);
    if (!def_id_valid(def))
      continue;

    DefMeta meta = *(DefMeta *)vec_get(&s->defs.meta, def.idx);
    if ((meta & META_VIS_MASK) == VIS_PUBLIC)
      continue;

    uint32_t refs = *(uint32_t *)vec_get(&s->defs.ref_count, def.idx);
    if (refs != 0)
      continue;

    // Anchor the warning on the decl's AST node — db_get_node_span
    // returns the node's source range, which spans the whole decl. The
    // LSP client will draw the squiggly under it.
    TinySpan span = TINYSPAN_NONE;
    for (uint32_t f = 0; f < fc && span == TINYSPAN_NONE; f++) {
      AstNodeId node = db_query_decl_ast(s, files[f], de->ast_id);
      if (node.idx != AST_NODE_ID_NONE.idx)
        span = db_get_node_span(s, files[f], node);
    }
    if (span == TINYSPAN_NONE)
      continue;

    db_emit(s, DIAG_WARNING, span, "%S is declared but never used", de->name);
  }

  db_query_succeed(s, QUERY_UNUSED_WARNINGS, (uint64_t)nsid.idx,
                   FINGERPRINT_NONE);
}
