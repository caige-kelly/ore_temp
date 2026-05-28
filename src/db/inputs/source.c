// Source mutators — the input boundary for raw text in the database.
// Every other piece of the compiler reads source bytes via getters;
// this is the only place that writes them.
//
// Setters here:
//   db_create_source            allocate a new SourceId for (path, text)
//   db_set_source_text          replace text; hash-compares to no-op on
//                               byte-identical edits, otherwise bumps the
//                               source's revision and stales the file_ast
//                               slot of every file backed by this source.
//   db_set_source_durability    pin tier (workspace vs library)

#include "../db.h"
#include "../diag/diag.h"        // db_diags_clear — drop superseded parse diags
#include "../query/invalidate.h" // db_locate_slot — for source-edit invalidation

#include <assert.h>
#include <stdlib.h>
#include <string.h>

// Source text is malloc-owned (NOT arena): db_set_source_text frees the
// prior buffer and installs a new one on every edit, so the storage is
// bounded across an editing session. (An arena would accumulate every
// superseded revision until db_free.)
static char *dup_source_text(const char *text, size_t len) {
  char *copy = (char *)malloc(len + 1);
  if (len)
    memcpy(copy, text, len);
  copy[len] = '\0';
  return copy;
}

// FNV-1a 64-bit over a byte buffer. Used for source content hashing so
// the LSP can detect "nothing actually changed" without re-parsing.
static uint64_t source_fnv1a(const char *data, size_t len) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < len; i++) {
    h ^= (uint8_t)data[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

// Shared row-allocation body for db_create_source + db_admit_virtual_source.
// `register_in_path_map` distinguishes disk-backed sources (yes, real
// paths get inserted into source_by_path so @import() can find them)
// from virtual sources (no — virtual files are addressable only by
// SourceId held by the creator; synthetic names live in a separate
// identity domain).
static SourceId create_source_row(struct db *s, const char *name_or_path,
                                  size_t name_len, const char *text,
                                  size_t text_len, bool is_virtual,
                                  bool register_in_path_map) {
  assert(text_len < (1u << 24) && "source > 16MB exceeds TinySpan range");

  uint32_t idx = (uint32_t)s->sources.hashes.count;
  pool_reserve_slots(&s->strings, text_len / 8);

  StrId path_id = pool_intern(&s->strings, name_or_path, name_len);
  char *text_copy = dup_source_text(text, text_len);
  uint64_t hash = source_fnv1a(text, text_len);
  uint32_t version = 1;

  // Grow every sources column in lockstep — X-macro covers the new
  // evicted / is_virtual flags too.
#define X(col, type) vec_push_zero(&s->sources.col);
  ORE_SOURCES_COLUMNS(X)
#undef X
  *(uint64_t *)vec_get(&s->sources.hashes, idx) = hash;
  *(uint32_t *)vec_get(&s->sources.versions, idx) = version;
  *(StrId *)vec_get(&s->sources.paths, idx) = path_id;
  *(char **)vec_get(&s->sources.texts, idx) = text_copy;
  *(uint32_t *)vec_get(&s->sources.text_lens, idx) = (uint32_t)text_len;
  *(Durability *)vec_get(&s->sources.durability, idx) = DUR_LOW;
  *(uint8_t *)vec_get(&s->sources.is_virtual, idx) = is_virtual ? 1 : 0;
  // evicted defaults to 0 via push_zero.

  if (register_in_path_map) {
    hashmap_put(&s->source_by_path, (uint64_t)path_id.idx,
                (void *)(uintptr_t)idx);
  }
  return (SourceId){.idx = idx};
}

SourceId db_create_source(struct db *s, const char *path, size_t path_len,
                          const char *text, size_t text_len) {
  return create_source_row(s, path, path_len, text, text_len,
                           /*is_virtual=*/false, /*register_in_path_map=*/true);
}

// Virtual-source row: same shape, NOT inserted into source_by_path.
// Synthetic names live in their own identity domain — addressable
// only by SourceId / FileId / NamespaceId held by the creator.
SourceId db_admit_virtual_source(struct db *s, const char *synthetic_name,
                                 size_t name_len, const char *text,
                                 size_t text_len) {
  return create_source_row(s, synthetic_name, name_len, text, text_len,
                           /*is_virtual=*/true, /*register_in_path_map=*/false);
}

void db_set_source_durability(struct db *s, SourceId src, uint8_t dur) {
  if (!source_id_valid(src) || src.idx >= s->sources.durability.count)
    return;
  *(Durability *)vec_get(&s->sources.durability, src.idx) = (Durability)dur;
}

bool db_set_source_text(struct db *s, SourceId src, const char *text,
                        size_t text_len) {
  if (!source_id_valid(src) || src.idx >= s->sources.texts.count)
    return false;
  assert(text_len < (1u << 24) && "source > 16MB exceeds TinySpan range");

  uint64_t new_hash = source_fnv1a(text, text_len);
  uint64_t *old_hash = (uint64_t *)vec_get(&s->sources.hashes, src.idx);
  uint32_t *old_len = (uint32_t *)vec_get(&s->sources.text_lens, src.idx);

  // "Nothing actually changed" fast path — a byte-identical edit must
  // not bump the revision or stale any memo.
  if (*old_hash == new_hash && *old_len == (uint32_t)text_len)
    return false;

  char **slot = (char **)vec_get(&s->sources.texts, src.idx);
  free(*slot);
  *slot = dup_source_text(text, text_len);
  *old_hash = new_hash;
  *old_len = (uint32_t)text_len;
  (*(uint32_t *)vec_get(&s->sources.versions, src.idx))++;

  // Invalidate the parse memo of every file backed by this source.
  // QUERY_FILE_AST is an INPUT query: a mere revision bump won't
  // recompute it (no deps), so staling the slot is the correct
  // input-set mechanism (Salsa: setting an input stamps its memo
  // changed).
  for (size_t i = 1; i < s->files.source_id.count; i++) {
    SourceId *fsrc = (SourceId *)vec_get(&s->files.source_id, i);
    if (!source_id_eq(*fsrc, src))
      continue;
    FileId *fkey = (FileId *)vec_get(&s->files.ids, i);
    QuerySlotHot *sl = db_locate_slot(s, QUERY_FILE_AST, (uint64_t)fkey->idx);
    if (sl) {
      sl->state = QUERY_EMPTY;
      sl->fingerprint = FINGERPRINT_NONE;
    }
    // Drop the prior parse's diagnostics. QUERY_FILE_AST is an input
    // query; staling its slot won't run db_query_begin's recompute
    // clear until something re-queries it, so clear the unit now to
    // keep diag collection free of superseded parse errors.
    db_diags_clear(s, QUERY_FILE_AST, (uint64_t)fkey->idx);
  }

  // Revision + durability bookkeeping at the source's own tier, so
  // db_revalidate early-cuts slots that don't depend on this tier.
  Durability dur = *(Durability *)vec_get(&s->sources.durability, src.idx);
  db_input_changed(s, (uint8_t)dur);
  return true;
}

// Internal helper used by db_ids_free (in src/db/ids/ids.c) to free
// the malloc-owned text buffer for each source.
void db_source_free_texts(struct db *s) {
  for (size_t i = 0; i < s->sources.texts.count; i++)
    free(*(char **)vec_get(&s->sources.texts, i));
}
