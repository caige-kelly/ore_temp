// Source readers — every read of a source-table column flows through
// here. Replaces ad-hoc `vec_get(&s->sources.X, sid.idx)` calls outside
// src/db/. Cheap (single load) accessors; no caching, no deps.

#include "../db.h"

#include <string.h>

const char *db_get_source_text(struct db *s, SourceId src) {
  if (!source_id_valid(src) || src.idx >= s->sources.texts.count)
    return NULL;
  return *(const char **)vec_get(&s->sources.texts, src.idx);
}

uint32_t db_get_source_len(struct db *s, SourceId src) {
  if (!source_id_valid(src) || src.idx >= s->sources.text_lens.count)
    return 0;
  return *(uint32_t *)vec_get(&s->sources.text_lens, src.idx);
}

StrId db_get_source_path(struct db *s, SourceId src) {
  if (!source_id_valid(src) || src.idx >= s->sources.paths.count)
    return (StrId){0};
  return *(StrId *)vec_get(&s->sources.paths, src.idx);
}

uint32_t db_get_source_version(struct db *s, SourceId src) {
  if (!source_id_valid(src) || src.idx >= s->sources.versions.count)
    return 0;
  return *(uint32_t *)vec_get(&s->sources.versions, src.idx);
}

Durability db_get_source_durability(struct db *s, SourceId src) {
  if (!source_id_valid(src) || src.idx >= s->sources.durability.count)
    return DUR_LOW;
  return *(Durability *)vec_get(&s->sources.durability, src.idx);
}

// Path → SourceId lookup. O(1) via the source_by_path HashMap.
// Uses pool_lookup so an unknown path doesn't leak a StrId into the
// pool (matters for LSP — clients can fire didChange for paths the
// server never opened).
SourceId db_lookup_source_by_path(struct db *s, const char *path,
                                  size_t path_len) {
  StrId target = pool_lookup(&s->strings, path, path_len);
  if (target.idx == 0)
    return SOURCE_ID_NONE;
  void *v = hashmap_get(&s->source_by_path, (uint64_t)target.idx);
  return (SourceId){.idx = (uint32_t)(uintptr_t)v};
}
