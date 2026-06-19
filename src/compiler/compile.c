#include "compile.h"

#include "../db/db.h"
#include "../db/diag/diag.h"
#include "../db/workspace/workspace.h"
#include "../support/data_structure/vec.h"

// Query entry points (no per-query headers post-D1; db_query_ctx == struct db).
extern struct GreenNode *db_query_file_ast(db_query_ctx *ctx, FileId fid);
extern void db_check_namespace(db_query_ctx *ctx, NamespaceId nsid);
extern FileArray db_query_line_index(db_query_ctx *ctx, FileId fid);
extern FileArray db_query_file_imports(db_query_ctx *ctx, FileId fid);

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
  //   2. Re-runs db_query_file_ast inside a transient request.
  // FILE_AST's compute-entry hook resets its parse_diags bundle, so
  // prior-iteration diags don't accumulate. Sema runs ONCE after the
  // loop, in the main request below.
  for (int i = 0; i < profile_count; i++) {
    if (i > 0) {
      // Phase P cutover — FILE_AST's compute-entry hook resets the
      // parse_diags bundle on each rerun; no central clear needed.
      db_input_changed(db, (uint8_t)DUR_LOW);
    }
    db_request_begin(db, (uint32_t)(i + 1));
    (void)db_query_file_ast(db, fid);
    db_request_end(db);
  }

  // Main request — sema + diag collection in one boundary.
  // Sema's queries (namespace_scopes, type_of_def, fn_signature,
  // infer_body, ...) all run here against a pinned effective_revision.
  // Diag collection happens AFTER request_end because the per-query
  // DiagBundle columns are slot-owned: their contents persist as long
  // as the producing slot does (gated by db_slot_is_live in the
  // collector). Reclaim_slot frees the bundle, so collection never
  // sees orphan diags.
  // Whole-program diagnostics: check + collect the target's entire forward
  // @import closure, not just the target namespace. Imported files' errors —
  // including the cross-file generic INSTANCES they trigger, which anchor to
  // the callee (defining) file — are already computed and stored in the
  // per-query DiagBundle columns; they were simply never collected. This is a
  // set-driven executor: only the SET (here, the forward @import closure) is
  // policy. A future build.ore program manifest would feed a different file
  // set through the same check + line-index + collect loop, verbatim.
  Vec closure; // FileId worklist; doubles as the visited set (dedup on push)
  vec_init(&closure, sizeof(FileId));
  vec_push(&closure, &fid);

  db_request_begin(db, db_current_revision(db));
  // BFS the closure, checking each reachable namespace and populating each
  // file's line-start byte offsets so post-request diag rendering can resolve
  // anchor byte ranges to file:line:col (db_resolve_span runs at render time
  // and can't drive memoized queries itself, so LINE_INDEX must run here).
  for (size_t i = 0; i < closure.count; i++) {
    FileId f = *(FileId *)vec_get(&closure, i);
    NamespaceId fns = db_get_file_namespace(db, f);
    if (!namespace_id_valid(fns))
      continue;
    db_check_namespace(db, fns);
    (void)db_query_line_index(db, f);

    // Enqueue this file's @imports (resolved to their files), dedup'd.
    FileArray imports = db_query_file_imports(db, f);
    const FileImport *items = (const FileImport *)imports.data;
    for (uint32_t k = 0; k < imports.count; k++) {
      NamespaceId ins = workspace_resolve_import(db, fns, items[k].path);
      if (!namespace_id_valid(ins))
        continue;
      uint32_t nf = 0;
      const FileId *files = db_get_namespace_files(db, ins, &nf);
      for (uint32_t j = 0; files && j < nf; j++) {
        bool seen = false;
        for (size_t v = 0; v < closure.count && !seen; v++)
          if (((const FileId *)vec_get(&closure, v))->idx == files[j].idx)
            seen = true;
        if (!seen)
          vec_push(&closure, &files[j]);
      }
    }
  }
  db_request_end(db);

  // out_diags is caller-initialized. Shallow-copy: Diag.args still points into
  // the producing query's diag arena. Caller must consume before the next
  // recompute could reset that arena. Collect every reached file.
  for (size_t i = 0; i < closure.count; i++)
    db_collect_diags_for_file(db, *(FileId *)vec_get(&closure, i), out_diags);

  vec_free(&closure);

  return fid;
}
