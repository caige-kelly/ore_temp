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
#include <string.h>

// Default first-chunk size for a diag unit's arena. Most units emit zero
// diags; the DiagList (and this arena) is created only on first emit.
#define ORE_DIAG_ARENA_DEFAULT_CHUNK_CAP (1 * 1024)

// Pack the active query's (kind, key) into the uniform u64 db.diags key.
// Query keys are by-value u64 (see db_query_begin) — just tag with kind.
static uint64_t diag_unit_key(QueryKind kind, uint64_t key) {
  return ((uint64_t)kind << 56) | (key & 0x00FFFFFFFFFFFFFFULL);
}

// Find-or-create the DiagList for the currently-active query unit.
// The DiagList lives by value in the dense db.diag_lists Vec; db.diags
// routes the unit key to its row. A DiagList is safely relocatable — its
// Arena and Vec are relocatable structs and the heap they own (chunks /
// backing buffer) does not move — so a diag_lists realloc is harmless.
// Asserts we're inside a query body — emission outside a query is a
// contract violation. (Post-typecheck orchestration code that used to
// emit from outside a frame was demolished in the Option-C migration;
// every diag now belongs to a real query unit.)
static DiagList *active_diag_list(struct db *s) {
  QueryFrame *top = db_query_stack_top(s);
  assert(top != NULL && "db_emit_* called outside a query body");
  uint64_t k = diag_unit_key(top->kind, top->key);

  void *rowp = hashmap_get(&s->diags, k);
  if (rowp)
    return (DiagList *)vec_get(&s->diag_lists, (uint32_t)(uintptr_t)rowp);

  // First emit for this unit — append a fresh DiagList row.
  uint32_t row = (uint32_t)s->diag_lists.count;
  vec_push_zero(&s->diag_lists);
  DiagList *dl = (DiagList *)vec_get(&s->diag_lists, row);
  arena_init(&dl->arena, ORE_DIAG_ARENA_DEFAULT_CHUNK_CAP);
  vec_init(&dl->items, sizeof(Diag));
  hashmap_put_or_die(&s->diags, k, (void *)(uintptr_t)row, "db_emit: diag unit");
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

// db_emit — printf-style variadic emit. The single canonical entry
// point for everything diag-shaped. Replaces 7+ shape-specific helpers
// (_n, _s, _t, _tt, _st, _nn, _warning_t, ...) with one API; adding a
// new arg type means one new specifier case below, not a new helper.
//
// Format specifiers:
//   %S       StrId      — interned string, looked up via db.strings
//   %T       IpIndex    — type formatted via db_format_type
//   %d       int32_t    — decimal integer
//   %c       uint32_t   — character (escaped if non-printable)
//   %P       TinySpan   — secondary location ("file:line:col")
//   %%       literal '%'
//
// The format string IS the template that gets interned + stored in the
// Diag (after a one-shot translation here to the legacy {N} placeholder
// syntax so the render path stays unchanged). Walking %X twice — once
// here to build args, once at render — would split the source of truth
// for which slot maps to which arg.
//
// Max args per call: 8 (raise the array size if a future diag needs
// more; printf-style scales naturally vs. helper-explosion).
//
// Example:
//   db_emit(s, DIAG_ERROR, span, "no field '%S' in %T", fname, recv_ty);
//
// The translated stored template would be "no field '{0}' in {1}".
void db_emit(struct db *s, DiagSeverity severity, TinySpan span,
             const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);

  // Stack-bounded scratch. Diag templates are short in practice
  // (<200 chars typical); 512 leaves comfortable headroom.
  char tmpl[512];
  DiagArg args[8];
  size_t t = 0;     // bytes written to tmpl
  size_t n = 0;     // arg count
  size_t cap = sizeof tmpl - 1;

  for (size_t i = 0; fmt[i] && t < cap; i++) {
    char c = fmt[i];
    if (c != '%') {
      tmpl[t++] = c;
      continue;
    }
    c = fmt[++i];
    if (c == '\0') {
      // Trailing `%` — emit verbatim and stop.
      if (t < cap) tmpl[t++] = '%';
      break;
    }
    if (c == '%') {
      if (t < cap) tmpl[t++] = '%';
      continue;
    }

    // Specifier — pull va_arg by spec letter, push DiagArg, emit `{N}`.
    if (n >= sizeof args / sizeof args[0])
      break; // ran out of arg slots; drop remainder.
    switch (c) {
    case 'S':
      args[n].kind = DIAG_ARG_STR_ID;
      args[n].str = va_arg(ap, StrId);
      break;
    case 's': {
      // Raw `const char *` — intern on the fly so it becomes a
      // first-class StrId arg (stable across pool resizes, no
      // borrowed-pointer lifetime concerns). Used by p_error to
      // forward a parser-side static message into a diag.
      const char *cs = va_arg(ap, const char *);
      args[n].kind = DIAG_ARG_STR_ID;
      args[n].str = pool_intern(&s->strings, cs ? cs : "(null)",
                                cs ? strlen(cs) : 6);
      break;
    }
    case 'T':
      args[n].kind = DIAG_ARG_TYPE;
      args[n].type = va_arg(ap, IpIndex);
      break;
    case 'd':
      args[n].kind = DIAG_ARG_INT;
      args[n].i = va_arg(ap, int);
      break;
    case 'c':
      args[n].kind = DIAG_ARG_CHAR;
      args[n].ch = va_arg(ap, unsigned int);
      break;
    case 'P':
      args[n].kind = DIAG_ARG_SPAN;
      args[n].span = va_arg(ap, TinySpan);
      break;
    default:
      // Unknown spec — emit verbatim, don't consume a va_arg.
      if (t + 1 < cap) {
        tmpl[t++] = '%';
        tmpl[t++] = c;
      }
      continue;
    }
    int wrote = snprintf(tmpl + t, cap - t, "{%zu}", n);
    if (wrote > 0)
      t += (size_t)wrote > cap - t ? cap - t : (size_t)wrote;
    n++;
  }
  tmpl[t] = '\0';
  va_end(ap);

  emit_internal(s, severity, span, tmpl, n ? args : NULL, n);
}
