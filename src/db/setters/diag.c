// Diagnostic emit — the input boundary for adding diagnostics to the db.
// Every db_emit_* writes a Diag into the centralized db.diags table,
// keyed by the (QueryKind, key) analysis unit of the currently-active
// query. Caller MUST be inside a query body (asserted).
//
// Diagnostics are keyed by analysis unit, not stored on the query slot:
// a cached query keeps its diags (the unit is not cleared without a
// recompute); a recompute clears the unit via db_diags_clear, called
// from db_query_begin — so diags stay in sync with query state.

#include "../diag/diag.h"
#include "../db.h"

#include <assert.h>
#include <string.h>

// Default first-chunk size for a diag unit's arena. Most units emit zero
// diags; the DiagList (and this arena) is created only on first emit.
#define ORE_DIAG_ARENA_DEFAULT_CHUNK_CAP (1 * 1024)

// Pack the active query's (kind, key) into the uniform u64 db.diags key.
// Most query kinds key on a 4-byte id (DefId / FileId / ModuleId / StrId);
// QUERY_RESOLVE_REF and QUERY_DEF_IDENTITY key on an already-packed u64
// (see db_locate_slot in query/invalidate.c). The dereference
// canonicalizes — two call sites passing different key pointers for the
// same logical query land on the same unit.
static uint64_t diag_unit_key(QueryKind kind, const void *key) {
  uint64_t bits = (kind == QUERY_RESOLVE_REF || kind == QUERY_DEF_IDENTITY)
                      ? *(const uint64_t *)key
                      : (uint64_t) * (const uint32_t *)key;
  return ((uint64_t)kind << 56) | (bits & 0x00FFFFFFFFFFFFFFULL);
}

// Find-or-create the DiagList for the currently-active query unit.
// Asserts we're inside a query body — emission outside a query is a
// contract violation.
static DiagList *active_diag_list(struct db *s) {
  QueryFrame *top = db_query_stack_top(s);
  assert(top != NULL && "db_emit_* called outside a query body");
  uint64_t k = diag_unit_key(top->kind, top->key);
  DiagList *dl = (DiagList *)hashmap_get(&s->diags, k);
  if (!dl) {
    dl = (DiagList *)arena_alloc(&s->arena, sizeof(DiagList));
    arena_init(&dl->arena, ORE_DIAG_ARENA_DEFAULT_CHUNK_CAP);
    vec_init(&dl->items, sizeof(Diag));
    hashmap_put_or_die(&s->diags, k, dl, "db_emit: diag unit");
  }
  return dl;
}

// Copy n_args DiagArgs into the unit's arena. Returns a pointer into the
// arena (stable until the unit is cleared). For zero args returns NULL —
// Diag.args is NULL when n_args == 0.
static const DiagArg *copy_args(Arena *arena, const DiagArg *args,
                                size_t n_args) {
  if (n_args == 0)
    return NULL;
  DiagArg *dst = (DiagArg *)arena_alloc_raw(arena, sizeof(DiagArg) * n_args);
  memcpy(dst, args, sizeof(DiagArg) * n_args);
  return dst;
}

static void emit_internal(struct db *s, DiagSeverity severity, TinySpan span,
                          const char *tmpl, const DiagArg *args,
                          size_t n_args) {
  DiagList *dl = active_diag_list(s);

  StrId tid = pool_intern(&s->strings, tmpl, strlen(tmpl));
  const DiagArg *args_copied = copy_args(&dl->arena, args, n_args);

  Diag d = {
      .primary = span,
      .template_id = tid,
      .args = args_copied,
      .code = 0,
      .n_args = (uint8_t)n_args,
      .severity = severity,
  };
  vec_push(&dl->items, &d);
}

// Clear the diagnostics of one analysis unit. Called by db_query_begin
// when (kind, key)'s slot recomputes, and by input setters when they
// stale an input query's slot directly. No-op if the unit never emitted
// — the DiagList struct + its arena are retained for reuse on the next
// run; only the contents are dropped.
void db_diags_clear(struct db *s, QueryKind kind, const void *key) {
  if (!key)
    return;
  uint64_t k = diag_unit_key(kind, key);
  DiagList *dl = (DiagList *)hashmap_get(&s->diags, k);
  if (!dl)
    return;
  vec_clear(&dl->items);
  arena_reset(&dl->arena);
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
