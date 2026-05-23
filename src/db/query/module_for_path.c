// QUERY_MODULE_FOR_PATH — pure resolution from (importer_module, path)
// to a ModuleId via source_by_path. The workspace coordinator's
// resolve_import wrapper lazy-loads on miss before sema sees the
// MODULE_ID_NONE.

#include "module_for_path.h"

#include "../db.h"
#include "../storage/hashmap.h"
#include "../storage/stringpool.h"
#include "query.h"
#include "query_engine.h"

#include <string.h>

#define ORE_PATH_MAX 4096

// Lexical path normalization — collapse ./ and ../ components, drop
// trailing slashes, no I/O. If `rel` is absolute (leading '/'), `dir`
// is ignored.
//
// Writes the result into `out` (up to out_cap-1 bytes plus NUL).
// Returns the number of bytes written excluding NUL, or 0 on failure
// (overflow or invalid input).
static size_t path_normalize(const char *dir, size_t dir_len,
                              const char *rel, size_t rel_len,
                              char *out, size_t out_cap) {
  if (out_cap < 2)
    return 0;

  // Build the initial absolute buffer: dir + '/' + rel  (or just rel if
  // rel is absolute).
  char buf[ORE_PATH_MAX];
  size_t bn = 0;
  if (rel_len > 0 && rel[0] == '/') {
    if (rel_len + 1 > sizeof(buf))
      return 0;
    memcpy(buf, rel, rel_len);
    bn = rel_len;
  } else {
    if (dir_len + 1 + rel_len + 1 > sizeof(buf))
      return 0;
    memcpy(buf, dir, dir_len);
    bn = dir_len;
    if (bn == 0 || buf[bn - 1] != '/') {
      buf[bn++] = '/';
    }
    memcpy(buf + bn, rel, rel_len);
    bn += rel_len;
  }
  buf[bn] = '\0';

  // Walk components, building out by pushing components and popping on
  // '..'. The output retains the leading '/' if buf was absolute.
  size_t out_n = 0;
  if (buf[0] == '/') {
    out[out_n++] = '/';
  }

  size_t i = 0;
  while (i < bn) {
    // Skip leading slashes.
    while (i < bn && buf[i] == '/')
      i++;
    if (i >= bn)
      break;
    // Find end of component.
    size_t start = i;
    while (i < bn && buf[i] != '/')
      i++;
    size_t comp_len = i - start;
    if (comp_len == 0)
      continue;
    if (comp_len == 1 && buf[start] == '.')
      continue; // skip "."
    if (comp_len == 2 && buf[start] == '.' && buf[start + 1] == '.') {
      // Pop the previous component (if any beyond the leading '/').
      if (out_n > 1) {
        // Strip trailing '/'.
        if (out[out_n - 1] == '/')
          out_n--;
        while (out_n > 0 && out[out_n - 1] != '/')
          out_n--;
      }
      continue;
    }
    // Push component.
    if (out_n > 0 && out[out_n - 1] != '/') {
      if (out_n + 1 >= out_cap)
        return 0;
      out[out_n++] = '/';
    }
    if (out_n + comp_len >= out_cap)
      return 0;
    memcpy(out + out_n, buf + start, comp_len);
    out_n += comp_len;
  }

  // Strip trailing '/' unless that's the only character.
  while (out_n > 1 && out[out_n - 1] == '/')
    out_n--;
  if (out_n < out_cap)
    out[out_n] = '\0';
  else
    out[out_cap - 1] = '\0';
  return out_n;
}

ModuleId db_query_module_for_path(struct db *s, ModuleId importer_module,
                                   StrId path_str) {
  if (!module_id_valid(importer_module) ||
      importer_module.idx >= s->modules.dirs.count ||
      path_str.idx == STR_ID_NONE.idx) {
    return MODULE_ID_NONE;
  }

  uint64_t k = ((uint64_t)importer_module.idx << 32) | (uint64_t)path_str.idx;

  // Routed-SoA: route (importer, path) → row in db.module_for_path,
  // allocating on first sight (mirrors def_identity / decl_ast).
  void *rowp = hashmap_get(&s->module_for_path_cache, k);
  uint32_t row;
  if (!rowp) {
    row = (uint32_t)s->module_for_path.slots_hot.count;
    vec_push_zero(&s->module_for_path.results);
    vec_push_zero(&s->module_for_path.slots_hot);
    vec_push_zero(&s->module_for_path.slots_cold);
    hashmap_put_or_die(&s->module_for_path_cache, k, (void *)(uintptr_t)row,
                       "module_for_path_cache");
  } else {
    row = (uint32_t)(uintptr_t)rowp;
  }

  DB_QUERY_GUARD(s, QUERY_MODULE_FOR_PATH, k,
                 *(ModuleId *)vec_get(&s->module_for_path.results, row),
                 MODULE_ID_NONE, MODULE_ID_NONE);

  // Resolve the path lexically against the importer's directory. No
  // disk I/O — purity requirement.
  StrId dir_id = *(StrId *)vec_get(&s->modules.dirs, importer_module.idx);
  ModuleId result = MODULE_ID_NONE;
  if (dir_id.idx != STR_ID_NONE.idx) {
    const char *dir = pool_get(&s->strings, dir_id);
    const char *rel = pool_get(&s->strings, path_str);
    char canonical[ORE_PATH_MAX];
    size_t n = path_normalize(dir, strlen(dir), rel, strlen(rel),
                               canonical, sizeof(canonical));
    if (n > 0) {
      StrId canon_id = pool_lookup(&s->strings, canonical, n);
      if (canon_id.idx != STR_ID_NONE.idx) {
        void *src_slot = hashmap_get(&s->source_by_path, (uint64_t)canon_id.idx);
        if (src_slot) {
          SourceId src = {.idx = (uint32_t)(uintptr_t)src_slot};
          FileId fid = db_lookup_file_by_source(s, src);
          if (file_id_valid(fid)) {
            result = *(ModuleId *)vec_get(&s->files.module_id,
                                          file_id_local(fid));
          }
        }
      }
    }
  }

  *(ModuleId *)vec_get(&s->module_for_path.results, row) = result;
  Fingerprint fp = db_fp_u64((uint64_t)result.idx);
  db_query_succeed(s, QUERY_MODULE_FOR_PATH, k, fp);
  return result;
}
