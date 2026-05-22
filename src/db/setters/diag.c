// Diagnostic emit — the input boundary for adding diagnostics to the
// db. Every db_emit_* writes a Diag into the currently-active query
// slot's diag_arena. Caller MUST be inside a query body (asserted).
//
// Diagnostics live with the query that produced them: cached queries
// replay their diags on subsequent calls without re-running the body,
// and a recompute resets the slot's diag storage (db_query_begin's
// compute path), so diags stay in sync with the query state.

#include "../db.h"
#include "../diag/diag.h"
#include "../query/invalidate.h"

#include <assert.h>
#include <string.h>

// Default first-chunk size for a slot's diag_arena. Most slots emit zero
// diags; this only allocates when the first emit fires. 1 KB covers a
// handful of diags' text/args without immediate chunk growth.
#define ORE_DIAG_ARENA_DEFAULT_CHUNK_CAP (1 * 1024)

// Find the active query body's slot. Asserts that we're inside a query
// body — diag emission outside a query is a contract violation.
static QuerySlot *active_slot(struct db *s) {
  QueryFrame *top = db_query_stack_top(s);
  assert(top != NULL && "db_emit_* called outside a query body");
  QuerySlot *slot = db_locate_slot(s, top->kind, top->key);
  assert(slot != NULL && "active query's slot kind has no db_locate_slot home");
  return slot;
}

// Lazy-init the slot's diag_arena and diags Vec on first emit.
static void ensure_diag_storage(struct db *s, QuerySlot *slot) {
  if (!slot->diag_arena) {
    slot->diag_arena = (Arena *)arena_alloc(&s->arena, sizeof(Arena));
    arena_init(slot->diag_arena, ORE_DIAG_ARENA_DEFAULT_CHUNK_CAP);
  }
  if (!slot->diags) {
    slot->diags = (Vec *)arena_alloc(slot->diag_arena, sizeof(Vec));
    vec_init(slot->diags, sizeof(Diag));
  }
}

// Copy n_args DiagArgs into the slot's diag_arena. Returns a pointer
// into the arena (stable for the arena's lifetime). For zero args,
// returns NULL — Diag.args is NULL when n_args == 0.
static const DiagArg *copy_args(Arena *diag_arena, const DiagArg *args,
                                size_t n_args) {
  if (n_args == 0)
    return NULL;
  DiagArg *dst =
      (DiagArg *)arena_alloc_raw(diag_arena, sizeof(DiagArg) * n_args);
  memcpy(dst, args, sizeof(DiagArg) * n_args);
  return dst;
}

static void emit_internal(struct db *s, DiagSeverity severity, TinySpan span,
                          const char *tmpl, const DiagArg *args,
                          size_t n_args) {
  QuerySlot *slot = active_slot(s);
  ensure_diag_storage(s, slot);

  StrId tid = pool_intern(&s->strings, tmpl, strlen(tmpl));
  const DiagArg *args_copied = copy_args(slot->diag_arena, args, n_args);

  Diag d = {
      .primary = span,
      .template_id = tid,
      .args = args_copied,
      .code = 0,
      .n_args = (uint8_t)n_args,
      .severity = severity,
  };
  vec_push(slot->diags, &d);

  if (severity == DIAG_ERROR) {
    slot->diag_error_count++;
  }
}

void db_emit_error(struct db *s, TinySpan span, const char *tmpl) {
  emit_internal(s, DIAG_ERROR, span, tmpl, NULL, 0);
}
void db_emit_warning(struct db *s, TinySpan span, const char *tmpl) {
  emit_internal(s, DIAG_WARNING, span, tmpl, NULL, 0);
}
void db_emit_info(struct db *s, TinySpan span, const char *tmpl) {
  emit_internal(s, DIAG_INFO, span, tmpl, NULL, 0);
}
void db_emit_hint(struct db *s, TinySpan span, const char *tmpl) {
  emit_internal(s, DIAG_HINT, span, tmpl, NULL, 0);
}

void db_emit_error_c(struct db *s, TinySpan span, const char *tmpl,
                     uint32_t ch) {
  DiagArg arg = {.kind = DIAG_ARG_CHAR, ._pad = {0}, .ch = ch};
  emit_internal(s, DIAG_ERROR, span, tmpl, &arg, 1);
}

void db_emit_error_s(struct db *s, TinySpan span, const char *tmpl, StrId str) {
  DiagArg arg = {.kind = DIAG_ARG_STR_ID, ._pad = {0}, .str = str};
  emit_internal(s, DIAG_ERROR, span, tmpl, &arg, 1);
}

void db_emit_error_n(struct db *s, TinySpan span, const char *tmpl, int32_t n) {
  DiagArg arg = {.kind = DIAG_ARG_INT, ._pad = {0}, .i = n};
  emit_internal(s, DIAG_ERROR, span, tmpl, &arg, 1);
}

void db_emit_error_t(struct db *s, TinySpan span, const char *tmpl,
                     IpIndex type) {
  DiagArg arg = {.kind = DIAG_ARG_TYPE, ._pad = {0}, .type = type};
  emit_internal(s, DIAG_ERROR, span, tmpl, &arg, 1);
}

void db_emit_error_ss(struct db *s, TinySpan span, const char *tmpl, StrId a,
                      StrId b) {
  DiagArg args[2] = {
      {.kind = DIAG_ARG_STR_ID, ._pad = {0}, .str = a},
      {.kind = DIAG_ARG_STR_ID, ._pad = {0}, .str = b},
  };
  emit_internal(s, DIAG_ERROR, span, tmpl, args, 2);
}

void db_emit_error_va(struct db *s, TinySpan span, const char *tmpl,
                      const DiagArg *args, size_t n_args) {
  emit_internal(s, DIAG_ERROR, span, tmpl, args, n_args);
}
