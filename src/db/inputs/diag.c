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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Default first-chunk size for a diag unit's arena. Most units emit zero
// diags; the DiagList (and this arena) is created only on first emit.
#define ORE_DIAG_ARENA_DEFAULT_CHUNK_CAP (1 * 1024)

// Pack the active query's (kind, key) into the uniform u64 db.diags key.
// Query keys are by-value u64 (see db_query_begin) — just tag with kind.
static uint64_t diag_unit_key(QueryKind kind, uint64_t key) {
  return ((uint64_t)kind << 56) | (key & 0x00FFFFFFFFFFFFFFULL);
}

// Find-or-create the DiagList for the given analysis unit. The
// DiagList lives by value in the dense db.diag_lists Vec; db.diags
// routes the unit key to its row. A DiagList is safely relocatable —
// its Arena and Vec are relocatable structs and the heap they own
// (chunks / backing buffer) does not move — so a diag_lists realloc is
// harmless.
//
// Frame-independent: callers pass (kind, key) explicitly. Post-typecheck
// orchestration code (e.g. sema_emit_unused_diagnostics) uses this via
// db_emit_to; in-query callers use db_emit which derives the unit from
// the active frame.
static DiagList *diag_list_for_unit(struct db *s, QueryKind kind,
                                    uint64_t key) {
  uint64_t k = diag_unit_key(kind, key);

  void *rowp = hashmap_get(&s->diags, k);
  if (rowp)
    return (DiagList *)vec_get(&s->diag_lists, (uint32_t)(uintptr_t)rowp);

  // First emit for this unit — append a fresh DiagList row.
  uint32_t row = (uint32_t)s->diag_lists.count;
  vec_push_zero(&s->diag_lists);
  DiagList *dl = (DiagList *)vec_get(&s->diag_lists, row);
  arena_init(&dl->arena, ORE_DIAG_ARENA_DEFAULT_CHUNK_CAP);
  vec_init(&dl->items, sizeof(Diag));
  // Record the owning analysis unit so collection can gate on the
  // slot's liveness (db_collect_diags_for_file → db_slot_is_live).
  dl->owner_kind = kind;
  dl->owner_key = key;
  hashmap_put_or_die(&s->diags, k, (void *)(uintptr_t)row,
                     "db_emit: diag unit");
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

// M3 — per-file index maintenance. Maintains:
//   - s->diag_lists_by_file: FileId.idx → malloc'd Vec<uint32_t> rows
//   - s->diag_lists_multi_file: Vec<uint32_t> for MULTI_FILE rows
// On collect_file transitions (0→F, F→G, F→MULTI_FILE, MULTI_FILE→…)
// we remove the row from its old bucket and add it to the new one.
// O(1) for append, O(n) for remove via swap-with-last (n = lists per
// file, typically tiny).
static Vec *diag_index_bucket(struct db *s, uint32_t file_idx) {
  if (file_idx == DIAG_LIST_MULTI_FILE)
    return &s->diag_lists_multi_file;
  void *p = hashmap_get(&s->diag_lists_by_file, (uint64_t)file_idx);
  if (p)
    return (Vec *)p;
  Vec *v = (Vec *)malloc(sizeof(Vec));
  vec_init(v, sizeof(uint32_t));
  hashmap_put_or_die(&s->diag_lists_by_file, (uint64_t)file_idx, v,
                     "diag_index: per-file bucket");
  return v;
}

static void diag_index_remove(Vec *bucket, uint32_t row) {
  for (size_t i = 0; i < bucket->count; i++) {
    if (*(uint32_t *)vec_get(bucket, i) == row) {
      // Swap-with-last; cheaper than memmove.
      uint32_t last = *(uint32_t *)vec_get(bucket, bucket->count - 1);
      *(uint32_t *)vec_get(bucket, i) = last;
      bucket->count--;
      return;
    }
  }
}

// Apply a collect_file transition. file_idx == 0 means "unindexed."
// MULTI_FILE goes into the dedicated vec; other values into the per-
// file map. Removes from the old bucket only if old != 0.
static void diag_list_retag(struct db *s, uint32_t row, uint32_t old_file,
                            uint32_t new_file) {
  if (old_file == new_file)
    return;
  if (old_file != 0)
    diag_index_remove(diag_index_bucket(s, old_file), row);
  if (new_file != 0) {
    Vec *b = diag_index_bucket(s, new_file);
    vec_push(b, &row);
  }
}

static void emit_internal(struct db *s, QueryKind unit_kind, uint64_t unit_key,
                          DiagSeverity severity, DiagTag tag, DiagAnchor anchor,
                          const char *tmpl, const DiagArg *args,
                          size_t n_args) {
  DiagList *dl = diag_list_for_unit(s, unit_kind, unit_key);
  // Recover the row index for the M3 index updates. Cheaper than
  // re-routing through the diags map: vec_get + pointer arithmetic.
  uint32_t row = (uint32_t)((char *)dl - (char *)s->diag_lists.data) /
                 (uint32_t)sizeof(DiagList);

  StrId tid = pool_intern(&s->strings, tmpl, strlen(tmpl));
  const DiagArg *args_copied = copy_args(&dl->arena, args, n_args);

  Diag d = {
      .anchor = anchor,
      .template_id = tid,
      .args = args_copied,
      .code = 0,
      .n_args = (uint8_t)n_args,
      .severity = severity,
      .tag = (uint8_t)tag,
  };

  // Maintain the per-file collection fast-path: first diag pins the unit's
  // anchor file; a later diag in a different file demotes it to MULTI_FILE
  // (then db_collect_diags_for_file always scans it + filters per-diag).
  // M3: every transition also updates s->diag_lists_by_file /
  // diag_lists_multi_file so per-file collection walks O(file's lists)
  // instead of O(all lists).
  uint32_t old_file = dl->collect_file;
  uint32_t new_file = old_file;
  if (dl->items.count == 0)
    new_file = (uint32_t)anchor.file_id;
  else if (dl->collect_file != (uint32_t)anchor.file_id)
    new_file = DIAG_LIST_MULTI_FILE;
  dl->collect_file = new_file;
  diag_list_retag(s, row, old_file, new_file);

  vec_push(&dl->items, &d);
}

// Clear the diagnostics of one analysis unit. Called by db_query_begin
// when (kind, key)'s slot recomputes, and by input setters when they
// stale an input query's slot directly. No-op if the unit never emitted
// — the DiagList struct + its arena are retained for reuse on the next
// run; only the contents are dropped.
void db_diags_clear(struct db *s, QueryKind kind, uint64_t key) {
  if (!key)
    return;
  uint64_t k = diag_unit_key(kind, key);
  void *rowp = hashmap_get(&s->diags, k);
  if (!rowp)
    return;
  DiagList *dl = (DiagList *)vec_get(&s->diag_lists, (uint32_t)(uintptr_t)rowp);
  vec_clear(&dl->items);
  arena_reset(&dl->arena);
}

// Shared printf-style template + args builder. Returns the arg count.
// Writes the translated {N}-placeholder template into `tmpl_out`
// (NUL-terminated, capped at tmpl_cap-1 bytes) and the parsed DiagArgs
// into `args_out` (capped at args_cap entries).
static size_t build_template_and_args(struct db *s, const char *fmt, va_list ap,
                                      char *tmpl_out, size_t tmpl_cap,
                                      DiagArg *args_out, size_t args_cap) {
  size_t t = 0;
  size_t n = 0;
  size_t cap = tmpl_cap - 1;

  for (size_t i = 0; fmt[i] && t < cap; i++) {
    char c = fmt[i];
    if (c != '%') {
      tmpl_out[t++] = c;
      continue;
    }
    c = fmt[++i];
    if (c == '\0') {
      if (t < cap)
        tmpl_out[t++] = '%';
      break;
    }
    if (c == '%') {
      if (t < cap)
        tmpl_out[t++] = '%';
      continue;
    }

    if (n >= args_cap)
      break;
    switch (c) {
    case 'S':
      args_out[n].kind = DIAG_ARG_STR_ID;
      args_out[n].str = va_arg(ap, StrId);
      break;
    case 's': {
      const char *cs = va_arg(ap, const char *);
      args_out[n].kind = DIAG_ARG_STR_ID;
      args_out[n].str =
          pool_intern(&s->strings, cs ? cs : "(null)", cs ? strlen(cs) : 6);
      break;
    }
    case 'T':
      args_out[n].kind = DIAG_ARG_TYPE;
      args_out[n].type = va_arg(ap, IpIndex);
      break;
    case 'd':
      args_out[n].kind = DIAG_ARG_INT;
      args_out[n].i = va_arg(ap, int);
      break;
    case 'c':
      args_out[n].kind = DIAG_ARG_CHAR;
      args_out[n].ch = va_arg(ap, unsigned int);
      break;
    case 'P':
      args_out[n].kind = DIAG_ARG_SPAN;
      args_out[n].span = va_arg(ap, DiagAnchor);
      break;
    default:
      if (t + 1 < cap) {
        tmpl_out[t++] = '%';
        tmpl_out[t++] = c;
      }
      continue;
    }
    int wrote = snprintf(tmpl_out + t, cap - t, "{%zu}", n);
    if (wrote > 0)
      t += (size_t)wrote > cap - t ? cap - t : (size_t)wrote;
    n++;
  }
  tmpl_out[t] = '\0';
  return n;
}

// db_emit — printf-style variadic emit, frame-routed.
//
// Reads the active query frame via db_query_stack_top and derives the
// analysis-unit key from (frame->kind, frame->key). Asserts a frame is
// on the stack — for post-typecheck orchestration code that emits
// outside any query, use db_emit_to instead.
//
// Format specifiers: see diag.h.
void db_emit(struct db *s, DiagSeverity severity, DiagAnchor anchor,
             const char *fmt, ...) {
  QueryFrame *top = db_query_stack_top(s);
  assert(top != NULL &&
         "db_emit called outside a query body — use db_emit_to with explicit "
         "(kind, key) routing");

  va_list ap;
  va_start(ap, fmt);
  char tmpl[512];
  DiagArg args[8];
  size_t n = build_template_and_args(s, fmt, ap, tmpl, sizeof tmpl, args, 8);
  va_end(ap);

  emit_internal(s, db_frame_kind(top), db_frame_key(top), severity,
                DIAG_TAG_NONE, anchor, tmpl, n ? args : NULL, n);
}

// db_emit_to — printf-style variadic emit with explicit unit routing.
//
// Used by post-typecheck orchestration code that walks results outside
// any salsa frame (e.g. sema_emit_unused_diagnostics). The caller
// provides the (kind, key) pair the diag should be routed to —
// typically the namespace_scopes or similar parent query whose
// re-runs invalidate this diag.
void db_emit_to(struct db *s, QueryKind unit_kind, uint64_t unit_key,
                DiagSeverity severity, DiagAnchor anchor, const char *fmt,
                ...) {
  va_list ap;
  va_start(ap, fmt);
  char tmpl[512];
  DiagArg args[8];
  size_t n = build_template_and_args(s, fmt, ap, tmpl, sizeof tmpl, args, 8);
  va_end(ap);

  emit_internal(s, unit_kind, unit_key, severity, DIAG_TAG_NONE, anchor, tmpl,
                n ? args : NULL, n);
}

// N2 — db_emit_tagged_to. Like db_emit_to plus an LSP DiagnosticTag.
// Use for diagnostics where the editor should render the identifier
// faded (UNNECESSARY: unused decls, unreachable code) or strikethrough
// (DEPRECATED). Tag == NONE behaves identically to db_emit_to.
void db_emit_tagged_to(struct db *s, QueryKind unit_kind, uint64_t unit_key,
                       DiagSeverity severity, DiagTag tag, DiagAnchor anchor,
                       const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char tmpl[512];
  DiagArg args[8];
  size_t n = build_template_and_args(s, fmt, ap, tmpl, sizeof tmpl, args, 8);
  va_end(ap);

  emit_internal(s, unit_kind, unit_key, severity, tag, anchor, tmpl,
                n ? args : NULL, n);
}
