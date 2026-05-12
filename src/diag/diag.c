#include "diag.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../sema/query/collect.h"
#include "../sema/query/query.h"
#include "../sema/sema.h"

struct DiagBag diag_bag_new(Arena *arena) {
  struct DiagBag bag = {0};
  bag.arena = arena;
  bag.diags = vec_new_in(arena, sizeof(struct Diag));
  return bag;
}

static const char *severity_str(DiagSeverity severity) {
  switch (severity) {
  case DIAG_ERROR:
    return "error";
  case DIAG_WARNING:
    return "warning";
  case DIAG_NOTE:
    return "note";
  }
  return "diagnostic";
}

static const char *severity_color(DiagSeverity severity) {
  switch (severity) {
  case DIAG_ERROR:
    return "\033[1;31m";
  case DIAG_WARNING:
    return "\033[1;33m";
  case DIAG_NOTE:
    return "\033[1;34m";
  }
  return "";
}

static void diag_vadd(struct DiagBag *bag, DiagSeverity severity,
                      struct Span span, const char *fmt, va_list ap) {
  if (!bag || !bag->diags)
    return;
  struct Diag diag = {0};
  diag.severity = severity;
  diag.span = span;
  diag.has_span = span.line > 0 && span.column > 0;
  vsnprintf(diag.msg, sizeof(diag.msg), fmt, ap);
  vec_push(bag->diags, &diag);
  if (severity == DIAG_ERROR)
    bag->error_count++;
  if (severity == DIAG_WARNING)
    bag->warning_count++;
}

static void diag_vadd_path(struct DiagBag *bag, DiagSeverity severity,
                           const char *path, const char *fmt, va_list ap) {
  if (!bag || !bag->diags)
    return;
  struct Diag diag = {0};
  diag.severity = severity;
  if (path)
    snprintf(diag.path, sizeof(diag.path), "%s", path);
  vsnprintf(diag.msg, sizeof(diag.msg), fmt, ap);
  vec_push(bag->diags, &diag);
  if (severity == DIAG_ERROR)
    bag->error_count++;
  if (severity == DIAG_WARNING)
    bag->warning_count++;
}

void diag_add(struct DiagBag *bag, DiagSeverity severity, struct Span span,
              const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  diag_vadd(bag, severity, span, fmt, ap);
  va_end(ap);
}

void diag_error(struct DiagBag *bag, struct Span span, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  diag_vadd(bag, DIAG_ERROR, span, fmt, ap);
  va_end(ap);
}

void diag_error_path(struct DiagBag *bag, const char *path, const char *fmt,
                     ...) {
  va_list ap;
  va_start(ap, fmt);
  diag_vadd_path(bag, DIAG_ERROR, path, fmt, ap);
  va_end(ap);
}

bool diag_has_errors(struct DiagBag *bag) {
  return bag && bag->error_count > 0;
}

void diag_bag_clear(struct DiagBag *bag) {
  if (!bag || !bag->diags)
    return;
  bag->diags->count = 0;
  bag->error_count = 0;
  bag->warning_count = 0;
}

// diag_emit is for emissions from inside a query body. The frame stack
// MUST be non-empty: if it isn't, the caller is outside the salsa-style
// query graph and should be using `diag_error(&bag, ...)` against an
// explicit bag (parser, LSP boundary, etc.). Falling back to the global
// bag would silently reintroduce the cache-staleness bug this refactor
// is fixing — emissions to that bag get cleared per pass and never
// re-fire on cache hits.
//
// Emissions attach to the currently executing query's slot. On cache
// hits, sema_query_begin skips the body and the slot's diags ride
// along with the memo; on RECOMPUTE, the `compute:` label in
// sema_query_begin resets the slot's diag arena before the body
// re-emits.
static void diag_emit_into_slot(struct QuerySlot *slot, struct Sema *s,
                                DiagSeverity severity, struct Span span,
                                const char *fmt, va_list ap) {
  if (!slot->diag_arena) {
    slot->diag_arena = arena_alloc(&s->arena, sizeof(Arena));
    arena_init(slot->diag_arena, 256);
  }
  if (!slot->diags) {
    slot->diags = vec_new_in(slot->diag_arena, sizeof(struct Diag));
  }
  struct Diag d = {0};
  d.severity = severity;
  d.span = span;
  d.has_span = span.line > 0 && span.column > 0;
  vsnprintf(d.msg, sizeof(d.msg), fmt, ap);
  vec_push(slot->diags, &d);
  if (severity == DIAG_ERROR)
    slot->diag_error_count++;
}

void diag_emit(struct Sema *s, struct Span span, const char *fmt, ...) {
  assert(s && "diag_emit called with NULL Sema");
  struct QueryFrame *top = query_stack_top(s);
  assert(top &&
         "diag_emit called outside any query frame — use diag_error(&bag, ...)");
  assert(top->slot && "QueryFrame with NULL slot");
  va_list ap;
  va_start(ap, fmt);
  diag_emit_into_slot(top->slot, s, DIAG_ERROR, span, fmt, ap);
  va_end(ap);
}

void diag_emit_severity(struct Sema *s, DiagSeverity severity, struct Span span,
                        const char *fmt, ...) {
  assert(s && "diag_emit_severity called with NULL Sema");
  struct QueryFrame *top = query_stack_top(s);
  assert(top && "diag_emit_severity called outside any query frame — use "
                "diag_add(&bag, ...)");
  assert(top->slot && "QueryFrame with NULL slot");
  va_list ap;
  va_start(ap, fmt);
  diag_emit_into_slot(top->slot, s, severity, span, fmt, ap);
  va_end(ap);
}

// === Collection ===
//
// Diagnostics now live in two places: per-slot accumulators (sema
// queries) and the sema-global bag (parse-time / IO errors that fire
// outside any query frame). The collector merges both into a single
// destination bag and sorts by location so the rendered order is
// stable run-to-run — HashMap iteration order is implementation-
// defined and slot tables aren't insertion-ordered.

struct DiagCollectCtx {
  struct DiagBag *dest;
  int file_id_filter; // < 0 means accept all
};

static void push_filtered(struct DiagBag *dest, struct Diag *d,
                          int file_id_filter) {
  if (file_id_filter >= 0) {
    if (!d->has_span || (int)d->span.file_id != file_id_filter)
      return;
  }
  vec_push(dest->diags, d);
  if (d->severity == DIAG_ERROR)
    dest->error_count++;
  else if (d->severity == DIAG_WARNING)
    dest->warning_count++;
}

static void collect_visit_slot(struct QuerySlot *slot, void *ud) {
  if (!slot || !slot->diags)
    return;
  struct DiagCollectCtx *ctx = (struct DiagCollectCtx *)ud;
  for (size_t i = 0; i < slot->diags->count; i++) {
    struct Diag *d = (struct Diag *)vec_get(slot->diags, i);
    if (d)
      push_filtered(ctx->dest, d, ctx->file_id_filter);
  }
}

static int compare_diags(const void *a, const void *b) {
  const struct Diag *da = (const struct Diag *)a;
  const struct Diag *db = (const struct Diag *)b;
  // Spanned diagnostics sort before path-only ones; within each group,
  // ordering is (file_id, line, column, span.start, severity).
  if (da->has_span != db->has_span)
    return da->has_span ? -1 : 1;
  if (da->has_span) {
    if (da->span.file_id != db->span.file_id)
      return da->span.file_id < db->span.file_id ? -1 : 1;
    if (da->span.line != db->span.line)
      return da->span.line < db->span.line ? -1 : 1;
    if (da->span.column != db->span.column)
      return da->span.column < db->span.column ? -1 : 1;
    if (da->span.start != db->span.start)
      return da->span.start < db->span.start ? -1 : 1;
  } else {
    int pc = strcmp(da->path, db->path);
    if (pc != 0)
      return pc;
  }
  if (da->severity != db->severity)
    return (int)da->severity - (int)db->severity;
  return 0;
}

void diag_collect_all(struct Sema *s, struct DiagBag *dest,
                      int file_id_filter) {
  if (!s || !dest || !dest->diags)
    return;
  struct DiagCollectCtx ctx = {.dest = dest, .file_id_filter = file_id_filter};

  // Slot accumulators.
  sema_for_each_slot(s, collect_visit_slot, &ctx);

  // Global bag: parse-time / IO emissions that fired outside any query
  // frame. (Today: parser errors and the input-read-failure path.)
  if (s->diags.diags) {
    for (size_t i = 0; i < s->diags.diags->count; i++) {
      struct Diag *d = (struct Diag *)vec_get(s->diags.diags, i);
      if (d)
        push_filtered(dest, d, file_id_filter);
    }
  }

  // Sort for deterministic render order.
  if (dest->diags->count > 1) {
    qsort(dest->diags->data, dest->diags->count, sizeof(struct Diag),
          compare_diags);
  }
}

static size_t underline_len_for_span(struct Span span, size_t line_len) {
  int raw_len = span.end - span.start;
  size_t len = raw_len > 0 ? (size_t)raw_len : 1;
  size_t col = span.column > 0 ? (size_t)span.column : 1;
  if (col > line_len)
    return 1;
  size_t remaining = line_len - col + 1;
  if (len > remaining)
    len = remaining;
  return len > 0 ? len : 1;
}

void diag_render(FILE *out, struct DiagBag *bag, struct SourceMap *source_map,
                 bool use_color) {
  if (!out || !bag || !bag->diags)
    return;

  for (size_t i = 0; i < bag->diags->count; i++) {
    struct Diag *diag = (struct Diag *)vec_get(bag->diags, i);
    if (!diag)
      continue;

    const char *path = diag->has_span
                           ? sourcemap_path(source_map, diag->span.file_id)
                           : (diag->path[0] ? diag->path : NULL);
    if (!path)
      path = "<unknown>";
    const char *sev = severity_str(diag->severity);
    const char *color = use_color ? severity_color(diag->severity) : "";
    const char *reset = use_color ? "\033[0m" : "";

    if (diag->has_span) {
      fprintf(out, "%s:%d:%d: %s%s%s: %s\n", path, diag->span.line,
              diag->span.column, color, sev, reset, diag->msg);
    } else {
      fprintf(out, "%s: %s%s%s: %s\n", path, color, sev, reset, diag->msg);
      continue;
    }

    size_t line_len = 0;
    const char *line = sourcemap_get_line(source_map, diag->span.file_id,
                                          diag->span.line, &line_len);
    if (!line)
      continue;

    fprintf(out, "  %4d | %.*s\n", diag->span.line, (int)line_len, line);
    fprintf(out, "       | ");
    int col = diag->span.column > 0 ? diag->span.column : 1;
    for (int c = 1; c < col; c++)
      fputc(' ', out);
    size_t underline_len = underline_len_for_span(diag->span, line_len);
    fputc('^', out);
    for (size_t u = 1; u < underline_len; u++)
      fputc('~', out);
    fputc('\n', out);
  }
}
