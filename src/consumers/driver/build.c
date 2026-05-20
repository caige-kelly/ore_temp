#include "../../db/db.h"
#include "../../db/diag/diag.h"
#include "../../db/intern_pool/intern_pool.h"
#include "../../db/query/ast.h"
#include "../../db/query/def_identity.h"
#include "../../db/query/invalidate.h"
#include "../../db/query/module_exports.h"
#include "../../db/query/query.h"
#include "../../db/query/resolve_ref.h"
#include "../../db/query/type_of_def.h"
#include "../../db/storage/stringpool.h"
#include "../../db/storage/vec.h"
#include "options.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast_dump_inc.h"

// Recursive type printer for the driver dump. Reserved primitives map
// to their canonical spelling; compound types (^T, ?T, []T, [N]T, …)
// recover their shape via ip_key and recurse. Nominal types (struct/
// enum) use their declaring DefId — stored as `zir_node_id` in the
// key — to recover the decl name from db.defs.names. Anything we can't
// render falls back to "IP[<idx>]".
static int format_ip_type(char *buf, size_t cap, struct db *db, IpIndex t) {
  if (cap == 0)
    return 0;
  if (t.v == IP_NONE.v)
    return snprintf(buf, cap, "?");
  InternPool *pool = &db->intern;

  // Reserved primitive types — keyword spelling.
  if (t.v == IP_BOOL_TYPE.v)
    return snprintf(buf, cap, "bool");
  if (t.v == IP_U8_TYPE.v)
    return snprintf(buf, cap, "u8");
  if (t.v == IP_I8_TYPE.v)
    return snprintf(buf, cap, "i8");
  if (t.v == IP_U16_TYPE.v)
    return snprintf(buf, cap, "u16");
  if (t.v == IP_I16_TYPE.v)
    return snprintf(buf, cap, "i16");
  if (t.v == IP_U32_TYPE.v)
    return snprintf(buf, cap, "u32");
  if (t.v == IP_I32_TYPE.v)
    return snprintf(buf, cap, "i32");
  if (t.v == IP_U64_TYPE.v)
    return snprintf(buf, cap, "u64");
  if (t.v == IP_I64_TYPE.v)
    return snprintf(buf, cap, "i64");
  if (t.v == IP_F32_TYPE.v)
    return snprintf(buf, cap, "f32");
  if (t.v == IP_F64_TYPE.v)
    return snprintf(buf, cap, "f64");
  if (t.v == IP_USIZE_TYPE.v)
    return snprintf(buf, cap, "usize");
  if (t.v == IP_ISIZE_TYPE.v)
    return snprintf(buf, cap, "isize");
  if (t.v == IP_VOID_TYPE.v)
    return snprintf(buf, cap, "void");
  if (t.v == IP_NORETURN_TYPE.v)
    return snprintf(buf, cap, "noreturn");
  if (t.v == IP_TYPE_TYPE.v)
    return snprintf(buf, cap, "type");
  if (t.v == IP_COMPTIME_INT_TYPE.v)
    return snprintf(buf, cap, "comptime_int");
  if (t.v == IP_COMPTIME_FLOAT_TYPE.v)
    return snprintf(buf, cap, "comptime_float");
  if (t.v == IP_NIL_TYPE.v)
    return snprintf(buf, cap, "nil");
  if (t.v == IP_ERROR_TYPE.v)
    return snprintf(buf, cap, "error");

  // Compound types — reconstruct the key and recurse on the element.
  IpTag tag = ip_tag(pool, t);
  IpKey k = ip_key(pool, t);
  char inner[256];
  switch (tag) {
  case IP_TAG_PTR_TYPE:
    format_ip_type(inner, sizeof inner, db, k.ptr_type.elem);
    return snprintf(buf, cap, "^%s", inner);
  case IP_TAG_PTR_CONST_TYPE:
    format_ip_type(inner, sizeof inner, db, k.ptr_type.elem);
    return snprintf(buf, cap, "^const %s", inner);
  case IP_TAG_SLICE_TYPE:
    format_ip_type(inner, sizeof inner, db, k.slice_type.elem);
    return snprintf(buf, cap, "[]%s", inner);
  case IP_TAG_SLICE_CONST_TYPE:
    format_ip_type(inner, sizeof inner, db, k.slice_type.elem);
    return snprintf(buf, cap, "[]const %s", inner);
  case IP_TAG_MANY_PTR_TYPE:
    format_ip_type(inner, sizeof inner, db, k.many_ptr_type.elem);
    return snprintf(buf, cap, "[^]%s", inner);
  case IP_TAG_MANY_PTR_CONST_TYPE:
    format_ip_type(inner, sizeof inner, db, k.many_ptr_type.elem);
    return snprintf(buf, cap, "[^]const %s", inner);
  case IP_TAG_OPTIONAL_TYPE:
    format_ip_type(inner, sizeof inner, db, k.optional_type.elem);
    return snprintf(buf, cap, "?%s", inner);
  case IP_TAG_ARRAY_TYPE:
    format_ip_type(inner, sizeof inner, db, k.array_type.elem);
    return snprintf(buf, cap, "[%llu]%s",
                    (unsigned long long)k.array_type.size, inner);
  case IP_TAG_FN_TYPE: {
    // fn(P1, P2, ...) -> R. Each leaf rendered into a small inner
    // buffer; if the joined result overflows `cap`, snprintf truncates
    // cleanly per the printf contract.
    size_t off = 0;
    int n = snprintf(buf, cap, "fn(");
    if (n > 0)
      off += (size_t)n;
    for (size_t i = 0; i < k.fn_type.n_params; i++) {
      if (i > 0 && off < cap) {
        n = snprintf(buf + off, cap - off, ", ");
        if (n > 0)
          off += (size_t)n;
      }
      char inner_p[128];
      format_ip_type(inner_p, sizeof inner_p, db, k.fn_type.params[i]);
      if (off < cap) {
        n = snprintf(buf + off, cap - off, "%s", inner_p);
        if (n > 0)
          off += (size_t)n;
      }
    }
    char inner_r[128];
    format_ip_type(inner_r, sizeof inner_r, db, k.fn_type.ret);
    if (off < cap) {
      n = snprintf(buf + off, cap - off, ") -> %s", inner_r);
      if (n > 0)
        off += (size_t)n;
    }
    return (int)off;
  }
  case IP_TAG_STRUCT_TYPE:
  case IP_TAG_ENUM_TYPE: {
    // zir_node_id IS the declaring DefId.idx — look up the decl's name
    // in db.defs.names (which db_query_def_identity populated).
    uint32_t def_idx = (tag == IP_TAG_STRUCT_TYPE)
                           ? k.struct_type.zir_node_id
                           : k.enum_type.zir_node_id;
    const char *kw = (tag == IP_TAG_STRUCT_TYPE) ? "struct" : "enum";
    if (def_idx < db->defs.names.count) {
      StrId nm = *(StrId *)vec_get(&db->defs.names, def_idx);
      const char *nm_str = pool_get(&db->strings, nm);
      if (nm_str && nm_str[0])
        return snprintf(buf, cap, "%s %s", kw, nm_str);
    }
    return snprintf(buf, cap, "%s@%u", kw, def_idx);
  }
  default:
    return snprintf(buf, cap, "IP[%u]", t.v);
  }
}

static char *slurp_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "could not open %s\n", path);
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long sz = ftell(f);
  if (sz < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  char *buf = (char *)malloc((size_t)sz + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t n = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (n != (size_t)sz) {
    free(buf);
    return NULL;
  }
  buf[sz] = '\0';
  if (out_len)
    *out_len = (size_t)sz;
  return buf;
}

int driver_build_run(const struct CompilerOptions *opts) {
  size_t src_len = 0;
  char *src = slurp_file(opts->input_path, &src_len);
  if (!src)
    return EXIT_FAILURE;

  struct db db;
  db_init(&db);

  SourceId sid = db_alloc_source(&db, opts->input_path,
                                 strlen(opts->input_path), src, src_len);

  // source -> file -> module: distinct id spaces. 1:1 today (one file
  // per module); db_alloc_file stamps the file's source/module
  // back-refs and the module records this file in its file list.
  ModuleId mid = db_alloc_module(&db);
  FileId fid = db_alloc_file(&db, sid, mid);
  db_module_add_file(&db, mid, fid);

  // ORE_PROFILE_LOOP=N — re-run the parse query N times for sampling
  // (each iteration after the first stales the slot so the work is
  // actually redone). Temporary; not for production use.
  int loops = 1;
  const char *lp = getenv("ORE_PROFILE_LOOP");
  if (lp) {
    int n = atoi(lp);
    if (n > 1)
      loops = n;
  }

  Fingerprint fp = 0;
  for (int li = 0; li < loops; li++) {
    if (li > 0) {
      // Stale the slot so the next iteration recomputes from scratch.
      QuerySlot *sl = db_locate_slot(&db, QUERY_FILE_AST,
                                     vec_get(&db.files.ids, file_id_local(fid)));
      if (sl) {
        sl->state = QUERY_EMPTY;
        sl->fingerprint = FINGERPRINT_NONE;
      }
    }
    db_request_begin(&db, (uint32_t)(li + 1));
    fp = db_query_file_ast(&db, fid);
    db_request_end(&db);
  }

  // Honest oracle: success is "zero error diagnostics", not "the query
  // returned". Collect this file's slot-keyed diagnostics and render.
  Vec diags;
  vec_init(&diags, sizeof(Diag));
  db_diag_collect_for_file(&db, fid, &diags);

  size_t errors = 0;
  for (size_t i = 0; i < diags.count; i++) {
    Diag *d = (Diag *)vec_get(&diags, i);
    char buf[1024];
    db_diag_format(&db, d, buf, sizeof buf);
    fprintf(stderr, "%s: %s\n",
            d->severity == DIAG_ERROR     ? "error"
            : d->severity == DIAG_WARNING ? "warning"
                                          : "note",
            buf);
    if (d->severity == DIAG_ERROR)
      errors++;
  }

  if (errors == 0)
    printf("OK: %s parsed (fingerprint %llu)\n", opts->input_path,
           (unsigned long long)fp);
  else
    printf("FAIL: %s — %zu parse error(s)\n", opts->input_path, errors);

  // Sema entry point — build the module's scopes, then materialize
  // each top-level def via db_query_def_identity(mid, ast_id). The
  // identity query owns DefId allocation (canonical (mid, ast_id) →
  // DefId map in db.def_by_identity), so DeclEntries don't need to
  // cache the DefId.
  db_request_begin(&db, 1);
  ScopeId export_scope = db_query_module_exports(&db, mid);
  ScopeId internal_scope =
      *(ScopeId *)vec_get(&db.modules.internal_scopes, mid.idx);
  if (internal_scope.idx != SCOPE_ID_NONE.idx) {
    uint32_t s0 =
        *(uint32_t *)vec_get(&db.scopes.decl_offsets, internal_scope.idx);
    uint32_t s1 = *(uint32_t *)vec_get(&db.scopes.decl_offsets,
                                       internal_scope.idx + 1);
    for (uint32_t i = s0; i < s1; i++) {
      DeclEntry *de = (DeclEntry *)vec_get(&db.scopes.decl_pool, i);
      db_query_def_identity(&db, mid, de->ast_id);
    }
  }
  db_request_end(&db);

  if (!getenv("ORE_NO_DUMP") && export_scope.idx != SCOPE_ID_NONE.idx) {
    printf("\nTop-Level Defs (export scope):\n");
    uint32_t off_start =
        *(uint32_t *)vec_get(&db.scopes.decl_offsets, export_scope.idx);
    uint32_t off_end = *(uint32_t *)vec_get(&db.scopes.decl_offsets,
                                            export_scope.idx + 1);
    for (uint32_t i = off_start; i < off_end; i++) {
      DeclEntry *de = (DeclEntry *)vec_get(&db.scopes.decl_pool, i);
      // Force-materialize (idempotent — CACHED after first call) so
      // the dump can recover the DefId without depending on the
      // module_exports loop above.
      DefId def = db_query_def_identity(&db, mid, de->ast_id);
      IpIndex t = db_query_type_of_def(&db, def);
      char tbuf[256];
      format_ip_type(tbuf, sizeof tbuf, &db, t);
      printf("  def=%u  name=%-20s ast_id=%08x  type=%s\n", def.idx,
             pool_get(&db.strings, de->name), de->ast_id.idx, tbuf);
    }
    uint32_t int_start =
        *(uint32_t *)vec_get(&db.scopes.decl_offsets, internal_scope.idx);
    uint32_t int_end =
        *(uint32_t *)vec_get(&db.scopes.decl_offsets, internal_scope.idx + 1);
    printf("  (internal scope: %u defs, export scope: %u defs)\n\n",
           int_end - int_start, off_end - off_start);

    // Name-resolution sanity check: each internal DeclEntry's name
    // should resolve back to its own DefId through db_query_resolve_ref.
    // Reports any mismatches; silent on a clean module.
    db_request_begin(&db, 1);
    uint32_t mismatches = 0;
    for (uint32_t i = int_start; i < int_end; i++) {
      DeclEntry *de = (DeclEntry *)vec_get(&db.scopes.decl_pool, i);
      DefId expected = db_query_def_identity(&db, mid, de->ast_id);
      DefId resolved =
          db_query_resolve_ref(&db, internal_scope, de->name);
      if (resolved.idx != expected.idx) {
        printf("  [resolve mismatch] name=%s resolved=%u expected=%u\n",
               pool_get(&db.strings, de->name), resolved.idx, expected.idx);
        mismatches++;
      }
    }
    db_request_end(&db);
    if (mismatches == 0)
      printf("Name resolution: %u/%u round-trip ok\n\n",
             int_end - int_start, int_end - int_start);

    // QUERY_TYPE_OF_DECL dump (chunk 1): literal-RHS const/var binds
    // map to a concrete IP_* type; everything else prints `?` and is
    // filled in by later chunks.
    printf("Top-Level Types (internal scope):\n");
    db_request_begin(&db, 1);
    for (uint32_t i = int_start; i < int_end; i++) {
      DeclEntry *de = (DeclEntry *)vec_get(&db.scopes.decl_pool, i);
      DefId def = db_query_def_identity(&db, mid, de->ast_id);
      IpIndex t = db_query_type_of_def(&db, def);
      char tbuf[256];
      format_ip_type(tbuf, sizeof tbuf, &db, t);
      printf("  def=%u  name=%-20s type=%s\n", def.idx,
             pool_get(&db.strings, de->name), tbuf);
    }
    db_request_end(&db);
    printf("\n");
  }

  uint32_t f = file_id_local(fid);
  ASTStore *ast = *(ASTStore **)vec_get(&db.files.asts, f);
  Vec *top_level_index = (Vec *)vec_get(&db.files.top_level_indices, f);
  if (!getenv("ORE_NO_DUMP"))
    ast_dump_module(ast, top_level_index, &db.strings);

  vec_free(&diags);
  db_free(&db);
  free(src);
  return errors == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
