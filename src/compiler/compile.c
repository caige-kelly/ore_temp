#include "compile.h"

#include "../db/db.h"
#include "../db/diag/diag.h"
#include "../db/query/ast.h"
#include "../db/query/invalidate.h"
#include "../db/query/query.h"
#include "../sema/sema.h"

FileId compile_file(struct db *db, SourceId src, const CompileFileOpts *opts,
                    Vec *out_diags) {
  if (!source_id_valid(src) || !out_diags)
    return FILE_ID_NONE;

  FileId fid = db_lookup_file_by_source(db, src);
  if (!file_id_valid(fid))
    return FILE_ID_NONE;
  NamespaceId nsid = db_get_file_namespace(db, fid);
  if (!namespace_id_valid(nsid))
    return FILE_ID_NONE;

  int profile_count =
      (opts && opts->profile_count > 0) ? opts->profile_count : 1;

  // Profile loop (debug-only; profile_count > 1). Re-parses the file
  // N times so a perf sampler can capture multiple iterations of the
  // parse work. Each iteration:
  //   1. After the first iter, bumps DUR_LOW so QUERY_FILE_AST's verify
  //      path observes the input tier moved and walks deps. The slot's
  //      source-text fingerprint is then re-checked; if the bytes
  //      haven't changed the engine early-cuts (byte-identical hot
  //      path), so for a true reparse benchmark the loop must actually
  //      mutate the source between iterations.
  //   2. CRITICAL: clears the slot's diags via db_diags_clear. Without
  //      this, prior-iteration parse diags accumulate in db.diag_lists
  //      and the final collect-pass returns 2N or 3N copies of every
  //      parse diag. This was a latent bug in the pre-2026-05-23
  //      driver — its open-coded loop did the slot reset but skipped
  //      db_diags_clear. Closed here for both consumers.
  //   3. Re-runs db_query_file_ast inside a transient request.
  // Sema runs ONCE after the loop, in the main request below.
  // TODO: if profile_count measurements show flat compute_count, the
  // engine needs an explicit per-slot invalidation hook (a public
  // db_engine_invalidate(kind, key) wrapper around the engine's
  // routing-key reset). Phase C+.
  for (int i = 0; i < profile_count; i++) {
    if (i > 0) {
      db_diags_clear(db, QUERY_FILE_AST, (uint64_t)fid.idx);
      db_input_changed(db, (uint8_t)DUR_LOW);
    }
    db_request_begin(db, (uint32_t)(i + 1));
    (void)db_query_file_ast(db, fid);
    db_request_end(db);
  }

  // Main request — sema + diag collection in one boundary.
  // Sema's queries (namespace_scopes, type_of_def, fn_signature,
  // infer_body, ...) all run here against a pinned effective_revision.
  // Diag collection happens AFTER request_end is fine because diags
  // live in db.diag_lists (centralized, not slot-scoped) and persist
  // until explicitly cleared.
  db_request_begin(db, db_current_revision(db));
  sema_check_module(db, nsid);
  db_request_end(db);

  // out_diags is caller-initialized. Shallow-copy: Diag.args still
  // points into the producing query's diag arena. Caller must consume
  // before the next recompute could reset that arena.
  db_collect_diags_for_file(db, fid, out_diags);

  return fid;
}
