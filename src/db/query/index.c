#include "../../db/db.h"
#include "ast.h"
#include "invalidate.h"
#include "query.h"
#include "query_engine.h"

Fingerprint db_query_top_level_index(struct db *s, ModuleId mod) {
  ModuleId *stable_mod = (ModuleId *)vec_get(&s->modules.ids, mod.idx);

  // Re-locate inside the on_cached branch — never cache a QuerySlot*
  // across DB_QUERY_GUARD (Vec column reallocs can invalidate it).
  DB_QUERY_GUARD(
      s, QUERY_TOP_LEVEL_INDEX, stable_mod,
      db_locate_slot(s, QUERY_TOP_LEVEL_INDEX, stable_mod)->fingerprint,
      FINGERPRINT_NONE, FINGERPRINT_NONE);

  // The module index is a DERIVED aggregation over the module's files.
  // For each file: parse it (db_query_file_ast records a dep on that
  // file's QUERY_FILE_AST onto this frame, so editing one file
  // recomputes only its parse + this aggregation; sibling files'
  // QUERY_FILE_AST deps verify unchanged and are early-cut) and fold
  // its top-level entries into the index fingerprint. Names/vis/ast_ids
  // only — node IDs and spans are excluded so body-only edits / line
  // shifts don't invalidate the index result.
  uint32_t file_count = 0;
  const FileId *files = db_module_files(s, mod, &file_count);

  Fingerprint fp = db_fp_u64(file_count);
  for (uint32_t fi = 0; fi < file_count; fi++) {
    FileId fid = files[fi];
    db_query_file_ast(s, fid);

    Vec *idx =
        (Vec *)vec_get(&s->files.top_level_indices, file_id_local(fid));
    fp = db_fp_combine(fp, db_fp_u64(idx->count));
    for (size_t i = 0; i < idx->count; i++) {
      TopLevelEntry *e = (TopLevelEntry *)vec_get(idx, i);
      fp = db_fp_combine(fp, db_fp_u64(e->name.idx));
      fp = db_fp_combine(fp, db_fp_u64((uint64_t)e->meta));
      fp = db_fp_combine(fp, db_fp_u64(e->ast_id.idx));
    }
  }

  db_query_succeed(s, QUERY_TOP_LEVEL_INDEX, stable_mod, fp);
  return fp;
}
