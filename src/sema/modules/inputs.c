#include "inputs.h"

#include <stdio.h>
#include <string.h>

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../../common/stringpool.h"
#include "../../common/vec.h"
#include "../../diag/diag.h"
#include "../query/query_engine.h"
#include "../sema.h"

void sema_inputs_init(struct Sema *s) {
  if (s->inputs_table)
    return;
  s->inputs_table = vec_new_in(s->arena, sizeof(void *));
  void *placeholder = NULL;
  vec_push(s->inputs_table, &placeholder);
  hashmap_init_in(&s->inputs_by_path, s->arena);
}

struct InputInfo *input_info(struct Sema *s, InputId id) {
  if (!s->inputs_table || id.idx == 0 || id.idx >= s->inputs_table->count)
    return NULL;
  void **slot = (void **)vec_get(s->inputs_table, id.idx);
  return slot ? (struct InputInfo *)*slot : NULL;
}

InputId sema_register_input(struct Sema *s, const char *path) {
  if (!path)
    return INPUT_ID_INVALID;

  uint32_t path_id = pool_intern(s->pool, path, strlen(path));

  // Idempotent on path_id: same pool key returns same InputId.
  if (hashmap_contains(&s->inputs_by_path, (uint64_t)path_id)) {
    void *slot = hashmap_get(&s->inputs_by_path, (uint64_t)path_id);
    return (InputId){(uint32_t)(uintptr_t)slot};
  }

  struct InputInfo *info = arena_alloc(s->arena, sizeof(struct InputInfo));
  *info = (struct InputInfo){
      .path_id = path_id,
      .path = pool_get(s->pool, path_id, 0),
      .source = NULL,
      .source_len = 0,
      .source_fp = FINGERPRINT_NONE,
      .last_changed_rev = 0,
      .is_dirty = false,
  };
  sema_query_slot_init(&info->ast_query, QUERY_MODULE_AST);

  vec_push(s->inputs_table, &info);
  InputId id = (InputId){(uint32_t)(s->inputs_table->count - 1)};
  hashmap_put(&s->inputs_by_path, (uint64_t)path_id,
              (void *)(uintptr_t)id.idx);
  return id;
}

void sema_set_input_source(struct Sema *s, InputId id, const char *text,
                           size_t len) {
  struct InputInfo *info = input_info(s, id);
  if (!info)
    return;

  if (text == NULL) {
    sema_invalidate_input(s, id);
    info->source = NULL;
    info->source_len = 0;
    info->source_fp = FINGERPRINT_NONE;
    return;
  }

  // Copy into the arena (callers manage their own buffers).
  char *copy = arena_alloc(s->arena, len + 1);
  memcpy(copy, text, len);
  copy[len] = '\0';

  info->source = copy;
  info->source_len = len;
  info->source_fp = query_fingerprint_from_bytes(copy, len);
  info->is_dirty = true;
  info->last_changed_rev = ++s->current_revision;
  s->invalidation_enabled = true;

  // Slot transition: any cached AST is stale. Reset the slot so the
  // next query_module_ast re-runs. The invalidator (Layer 7.5)
  // would also catch this via fingerprint comparison on the
  // input — the eager reset short-circuits the walk for the common
  // case where the input itself is the changing leaf.
  info->ast_query.state = QUERY_EMPTY;
  info->ast_query.fingerprint = FINGERPRINT_NONE;
  info->ast_query.deps = NULL;
}

void sema_invalidate_input(struct Sema *s, InputId id) {
  struct InputInfo *info = input_info(s, id);
  if (!info)
    return;

  info->source = NULL;
  info->source_len = 0;
  info->source_fp = FINGERPRINT_NONE;
  info->is_dirty = true;
  info->last_changed_rev = ++s->current_revision;
  s->invalidation_enabled = true;
  info->ast_query.state = QUERY_EMPTY;
  info->ast_query.fingerprint = FINGERPRINT_NONE;
  info->ast_query.deps = NULL;
}

// Read a file's full contents into an arena-allocated buffer. Returns
// NULL on any failure; caller is expected to emit a diagnostic.
//
// We deliberately avoid mmap here — files are small (source code) and
// the simplicity of fread + arena is worth more than zero-copy.
static char *read_file_to_arena(Arena *arena, const char *path,
                                size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long sz = ftell(f);
  if (sz < 0) {
    fclose(f);
    return NULL;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }
  char *buf = arena_alloc(arena, (size_t)sz + 1);
  size_t read = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (read != (size_t)sz)
    return NULL;
  buf[sz] = '\0';
  if (out_len)
    *out_len = (size_t)sz;
  return buf;
}

const char *query_input_source(struct Sema *s, InputId id) {
  struct InputInfo *info = input_info(s, id);
  if (!info)
    return NULL;
  if (info->source)
    return info->source;

  size_t len = 0;
  char *buf = read_file_to_arena(s->arena, info->path, &len);
  if (!buf) {
    // E0001 — input read failed. Diagnostic plumbing for input-layer
    // errors is intentionally minimal; the LSP shell typically owns
    // file IO and we won't hit this path. CLI users get a one-line
    // stderr nudge; structured diagnostics will land alongside the
    // diag/codes.h work in a later PR.
    if (s->diags)
      diag_error(s->diags, (struct Span){0},
                 "could not read input file '%s'",
                 info->path ? info->path : "?");
    return NULL;
  }

  info->source = buf;
  info->source_len = len;
  info->source_fp = query_fingerprint_from_bytes(buf, len);
  return info->source;
}

size_t query_input_source_len(struct Sema *s, InputId id) {
  // Force the source to be loaded if it hasn't been; the byte length
  // isn't meaningful otherwise. Once loaded, the cached length is
  // valid until the next mutation.
  if (query_input_source(s, id) == NULL)
    return 0;
  struct InputInfo *info = input_info(s, id);
  return info ? info->source_len : 0;
}
