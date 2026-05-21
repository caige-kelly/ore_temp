#include "diag.h"

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../db.h"
#include "../query/collect.h"
#include "../query/invalidate.h"
#include "../query/query.h"

// Default first-chunk size for a slot's diag_arena. Most slots emit zero
// diags; this only allocates when the first emit fires. 1 KB covers a
// handful of diags' text/args without immediate chunk growth.
#define ORE_DIAG_ARENA_DEFAULT_CHUNK_CAP (1 * 1024)

/* ============================================================================
   Internal: locate the active slot and lazy-init its diag state.
   ============================================================================
 */

// Find the active query body's slot. Asserts that we're inside a query
// body — diag emission outside a query is a contract violation.
static QuerySlot *active_slot(struct db *s) {
  QueryFrame *top = db_query_stack_top(s);
  assert(top != NULL && "db_diag_* called outside a query body");
  QuerySlot *slot = db_locate_slot(s, top->kind, top->key);
  assert(slot != NULL && "active query's slot kind has no db_locate_slot home");
  return slot;
}

// Lazy-init the slot's diag_arena and diags Vec on first emit.
static void ensure_diag_storage(struct db *s, QuerySlot *slot) {
  (void)s;
  if (!slot->diag_arena) {
    // Arena STRUCT lives in db.arena (small, pointer-stable). Its
    // chunks malloc independently.
    slot->diag_arena = (Arena *)arena_alloc(&s->arena, sizeof(Arena));
    arena_init(slot->diag_arena, ORE_DIAG_ARENA_DEFAULT_CHUNK_CAP);
  }
  if (!slot->diags) {
    // The Vec OBJECT lives in the slot's diag_arena. Its backing
    // buffer is malloc-owned by vec_init/vec_push and freed via
    // vec_free on slot_release. (See lifecycle.c.)
    slot->diags = (Vec *)arena_alloc(slot->diag_arena, sizeof(Vec));
    vec_init(slot->diags, sizeof(Diag));
  }
}

/* ============================================================================
   Internal: build a Diag in the active slot's storage.
   ============================================================================
 */

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

  // Auto-intern the template. Cheap on repeats — pool_intern dedupes.
  StrId tid = pool_intern(&s->strings, tmpl, strlen(tmpl));

  // Args copied into the slot's diag_arena. Caller can pass stack
  // locals safely.
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

/* ============================================================================
   Emit — public API.
   ============================================================================
 */

void db_diag_error(struct db *s, TinySpan span, const char *tmpl) {
  emit_internal(s, DIAG_ERROR, span, tmpl, NULL, 0);
}
void db_diag_warning(struct db *s, TinySpan span, const char *tmpl) {
  emit_internal(s, DIAG_WARNING, span, tmpl, NULL, 0);
}
void db_diag_info(struct db *s, TinySpan span, const char *tmpl) {
  emit_internal(s, DIAG_INFO, span, tmpl, NULL, 0);
}
void db_diag_hint(struct db *s, TinySpan span, const char *tmpl) {
  emit_internal(s, DIAG_HINT, span, tmpl, NULL, 0);
}

void db_diag_error_c(struct db *s, TinySpan span, const char *tmpl,
                     uint32_t ch) {
  DiagArg arg = {.kind = DIAG_ARG_CHAR, ._pad = {0}, .ch = ch};
  emit_internal(s, DIAG_ERROR, span, tmpl, &arg, 1);
}

void db_diag_error_s(struct db *s, TinySpan span, const char *tmpl, StrId str) {
  DiagArg arg = {.kind = DIAG_ARG_STR_ID, ._pad = {0}, .str = str};
  emit_internal(s, DIAG_ERROR, span, tmpl, &arg, 1);
}

void db_diag_error_n(struct db *s, TinySpan span, const char *tmpl, int32_t n) {
  DiagArg arg = {.kind = DIAG_ARG_INT, ._pad = {0}, .i = n};
  emit_internal(s, DIAG_ERROR, span, tmpl, &arg, 1);
}

void db_diag_error_t(struct db *s, TinySpan span, const char *tmpl,
                     IpIndex type) {
  DiagArg arg = {.kind = DIAG_ARG_TYPE, ._pad = {0}, .type = type};
  emit_internal(s, DIAG_ERROR, span, tmpl, &arg, 1);
}

void db_diag_error_ss(struct db *s, TinySpan span, const char *tmpl, StrId a,
                      StrId b) {
  DiagArg args[2] = {
      {.kind = DIAG_ARG_STR_ID, ._pad = {0}, .str = a},
      {.kind = DIAG_ARG_STR_ID, ._pad = {0}, .str = b},
  };
  emit_internal(s, DIAG_ERROR, span, tmpl, args, 2);
}

void db_diag_error_va(struct db *s, TinySpan span, const char *tmpl,
                      const DiagArg *args, size_t n_args) {
  emit_internal(s, DIAG_ERROR, span, tmpl, args, n_args);
}

/* ============================================================================
   Collection — walk slots via db_for_each_slot, copy diag entries.
   ============================================================================
 */

struct collect_ctx {
  Vec *out;
  bool filter_by_file;
  FileId target_file;
};

static void collect_visitor(QuerySlot *slot, QueryKind kind, const void *key,
                            void *user_data) {
  (void)kind;
  (void)key;
  struct collect_ctx *ctx = (struct collect_ctx *)user_data;
  if (!slot->diags)
    return;
  for (size_t i = 0; i < slot->diags->count; i++) {
    Diag *d = (Diag *)vec_get(slot->diags, i);
    if (ctx->filter_by_file &&
        !file_id_eq(file_id_make_physical(span_file(d->primary)),
                    ctx->target_file)) {
      continue;
    }
    vec_push(ctx->out, d);
  }
}

void db_diag_collect_all(struct db *s, Vec *out_diags) {
  struct collect_ctx ctx = {.out = out_diags, .filter_by_file = false};
  db_for_each_slot(s, collect_visitor, &ctx);
}

void db_diag_collect_for_file(struct db *s, FileId file, Vec *out_diags) {
  struct collect_ctx ctx = {
      .out = out_diags,
      .filter_by_file = true,
      .target_file = file,
  };
  db_for_each_slot(s, collect_visitor, &ctx);
}

/* ============================================================================
   Render — resolve template + args into formatted text.
   ============================================================================
 */

// Append `s` (len bytes) to buf. Always tracks `written` against `buflen`
// so the snprintf-style "would-have-written" semantics work for sizing.
// NUL-termination is the caller's job after the last append.
static void append(char *buf, size_t buflen, size_t *written, const char *s,
                   size_t len) {
  if (!len)
    return;
  size_t cap = buflen ? buflen - 1 : 0; // reserve 1 for NUL
  size_t pos = *written < cap ? *written : cap;
  size_t copy = (cap > pos) ? (cap - pos) : 0;
  if (copy > len)
    copy = len;
  if (copy && buf)
    memcpy(buf + pos, s, copy);
  *written += len;
}

static void append_cstr(char *buf, size_t buflen, size_t *written,
                        const char *s) {
  append(buf, buflen, written, s, strlen(s));
}

// Format one arg into buf. Mutates *written by the bytes it would have
// emitted (truncates to buflen as needed).
static void format_arg(struct db *s, const DiagArg *arg, char *buf,
                       size_t buflen, size_t *written) {
  char scratch[64];
  switch (arg->kind) {
  case DIAG_ARG_NONE:
    // Shouldn't happen for a valid placeholder; print sentinel for
    // debug visibility.
    append_cstr(buf, buflen, written, "<none>");
    return;

  case DIAG_ARG_CHAR: {
    // Printable: 'x'. Non-printable: \xNN escape.
    uint32_t ch = arg->ch;
    int n;
    if (ch < 128 && isprint((int)ch)) {
      n = snprintf(scratch, sizeof(scratch), "'%c'", (char)ch);
    } else {
      n = snprintf(scratch, sizeof(scratch), "'\\x%02X'", ch & 0xFF);
    }
    if (n < 0)
      n = 0;
    append(buf, buflen, written, scratch, (size_t)n);
    return;
  }

  case DIAG_ARG_STR_ID: {
    const char *text = pool_get(&s->strings, arg->str);
    if (text)
      append_cstr(buf, buflen, written, text);
    else
      append_cstr(buf, buflen, written, "<bad str>");
    return;
  }

  case DIAG_ARG_INT: {
    int n = snprintf(scratch, sizeof(scratch), "%" PRId32, arg->i);
    if (n < 0)
      n = 0;
    append(buf, buflen, written, scratch, (size_t)n);
    return;
  }

  case DIAG_ARG_TYPE: {
    // db_format_type resolves nominal types (struct/enum) by name via
    // db.defs.names. Snprintf-style; truncate to scratch for the rare
    // wide types.
    char wide[256];
    size_t needed = db_format_type(s, arg->type, wide, sizeof(wide));
    size_t to_copy = needed < sizeof(wide) ? needed : sizeof(wide) - 1;
    append(buf, buflen, written, wide, to_copy);
    return;
  }

  case DIAG_ARG_SPAN: {
    // Compact form: "file#<file_id>:<byte_start>-<byte_end>".
    // The render layer that actually shows diagnostics to humans
    // typically resolves file_id → path via db.sources and
    // byte_start → line/col via mod->line_starts. We keep this
    // simple in the lower layer.
    int n = snprintf(scratch, sizeof(scratch), "file#%u:%u-%u",
                     span_file(arg->span), span_start(arg->span),
                     span_end(arg->span));
    if (n < 0)
      n = 0;
    append(buf, buflen, written, scratch, (size_t)n);
    return;
  }
  }
}

size_t db_diag_format(struct db *s, const Diag *d, char *buf, size_t buflen) {
  if (!s || !d) {
    if (buf && buflen)
      buf[0] = '\0';
    return 0;
  }

  size_t written = 0;

  // Resolve template text via the StringPool. pool_get returns a
  // NUL-terminated borrowed pointer; we walk by length explicitly so
  // {{/}}/{N} parsing isn't dependent on terminator placement.
  const char *tpl = pool_get(&s->strings, d->template_id);
  if (!tpl) {
    append_cstr(buf, buflen, &written, "<bad template>");
    if (buf && buflen)
      buf[written < buflen ? written : buflen - 1] = '\0';
    return written;
  }
  size_t tlen = strlen(tpl);

  // Walk the template, substituting {N} placeholders. Anything else
  // copies verbatim. `{{` and `}}` are escapes for literal `{`/`}`.
  for (size_t i = 0; i < tlen;) {
    char c = tpl[i];

    if (c == '{' && i + 1 < tlen && tpl[i + 1] == '{') {
      append(buf, buflen, &written, "{", 1);
      i += 2;
      continue;
    }
    if (c == '}' && i + 1 < tlen && tpl[i + 1] == '}') {
      append(buf, buflen, &written, "}", 1);
      i += 2;
      continue;
    }

    if (c == '{') {
      // Parse a decimal index up to the closing brace.
      size_t j = i + 1;
      uint32_t idx = 0;
      bool ok = false;
      while (j < tlen && tpl[j] >= '0' && tpl[j] <= '9') {
        idx = idx * 10 + (uint32_t)(tpl[j] - '0');
        j++;
        ok = true;
      }
      if (ok && j < tlen && tpl[j] == '}') {
        if (idx < d->n_args && d->args) {
          format_arg(s, &d->args[idx], buf, buflen, &written);
        } else {
          // Out-of-range arg index — emit the placeholder
          // verbatim so the bug is visible.
          append(buf, buflen, &written, &tpl[i], j - i + 1);
        }
        i = j + 1;
        continue;
      }
      // Unclosed or malformed brace — copy as-is.
      append(buf, buflen, &written, &c, 1);
      i++;
      continue;
    }

    append(buf, buflen, &written, &c, 1);
    i++;
  }

  if (buf && buflen) {
    size_t terminator_pos = written < buflen ? written : buflen - 1;
    buf[terminator_pos] = '\0';
  }
  return written;
}

// Resolve a TinySpan to its source-position context (path, 1-indexed
// line+col, and the literal source-line text). Used by db_diag_fprint
// for rust-style diagnostic rendering. Returns false when the span
// can't be resolved (e.g., virtual files with no on-disk path, or a
// span that points into an unparsed file); the caller then prints the
// diag without source context.
typedef struct {
  const char *path;
  uint32_t    line;          // 1-indexed
  uint32_t    col_start;     // 1-indexed
  uint32_t    col_end;       // 1-indexed, exclusive; clamped to the line
  const char *line_text;     // not NUL-terminated
  size_t      line_text_len;
} ResolvedSpan;

static bool resolve_span(struct db *s, TinySpan span, ResolvedSpan *out) {
  *out = (ResolvedSpan){0};
  out->path = "<unknown>";

  uint16_t file_id_raw = span_file(span);
  if (file_id_raw == 0)
    return false;

  FileId fid = file_id_make_physical((uint32_t)file_id_raw);
  uint32_t local = file_id_local(fid);
  if (local == 0 || local >= s->files.source_id.count)
    return false;

  SourceId sid = *(SourceId *)vec_get(&s->files.source_id, local);
  if (sid.idx == 0 || sid.idx >= s->sources.paths.count)
    return false;

  StrId path_id = *(StrId *)vec_get(&s->sources.paths, sid.idx);
  const char *path = pool_get(&s->strings, path_id);
  if (path && path[0])
    out->path = path;

  // Source text + per-file line starts. line_starts is a Vec<uint32_t>
  // built by the lexer; each entry is the byte offset of a line start.
  const char *text = *(const char **)vec_get(&s->sources.texts, sid.idx);
  uint32_t text_len = *(uint32_t *)vec_get(&s->sources.text_lens, sid.idx);
  if (!text)
    return false;

  Vec *line_starts = (Vec *)vec_get(&s->files.line_starts, local);
  if (!line_starts || line_starts->count == 0)
    return false;

  uint32_t byte_start = span_start(span);
  uint32_t byte_end = span_end(span);
  if (byte_start > text_len)
    return false;

  // Binary search: find the largest line_starts[i] such that
  // line_starts[i] <= byte_start. line index is i, col is the offset
  // into that line (both 1-indexed for output).
  const uint32_t *starts = (const uint32_t *)line_starts->data;
  size_t lo = 0, hi = line_starts->count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (starts[mid] <= byte_start)
      lo = mid + 1;
    else
      hi = mid;
  }
  size_t line_idx = (lo == 0) ? 0 : lo - 1;

  out->line = (uint32_t)line_idx + 1;
  out->col_start = byte_start - starts[line_idx] + 1;

  // Line text spans [line_start_byte, line_end_byte), trimming the
  // trailing newline if present.
  uint32_t line_start_byte = starts[line_idx];
  uint32_t line_end_byte = (line_idx + 1 < line_starts->count)
                               ? starts[line_idx + 1]
                               : text_len;
  while (line_end_byte > line_start_byte &&
         (text[line_end_byte - 1] == '\n' ||
          text[line_end_byte - 1] == '\r'))
    line_end_byte--;

  out->line_text = text + line_start_byte;
  out->line_text_len = line_end_byte - line_start_byte;

  // col_end clamps to this line so multi-line spans don't bleed.
  uint32_t end_clamped = byte_end > line_end_byte ? line_end_byte : byte_end;
  out->col_end = end_clamped > line_start_byte
                     ? end_clamped - line_start_byte + 1
                     : out->col_start + 1;
  if (out->col_end <= out->col_start)
    out->col_end = out->col_start + 1;

  return true;
}

// Rust-style diagnostic rendering:
//
//   path:line:col: severity: message
//       <source line>
//       <spaces><carets>
//
// When source context can't be resolved (virtual / no line_starts) we
// fall back to the bare path-line-col header plus message, no body.
size_t db_diag_fprint(struct db *s, const Diag *d, FILE *out) {
  // Format the message body first into a sized buffer (small messages
  // stack-allocate; long ones heap-allocate).
  char stack_buf[256];
  char *buf = stack_buf;
  size_t needed = db_diag_format(s, d, stack_buf, sizeof(stack_buf));
  if (needed >= sizeof(stack_buf)) {
    buf = (char *)malloc(needed + 1);
    if (!buf) {
      return (size_t)fwrite(stack_buf, 1, strlen(stack_buf), out);
    }
    db_diag_format(s, d, buf, needed + 1);
  }

  const char *sev = d->severity == DIAG_ERROR     ? "error"
                    : d->severity == DIAG_WARNING ? "warning"
                    : d->severity == DIAG_INFO    ? "info"
                                                  : "note";

  ResolvedSpan rs;
  bool resolved = resolve_span(s, d->primary, &rs);

  size_t written = 0;
  if (resolved) {
    written += (size_t)fprintf(out, "%s:%u:%u: %s: %.*s\n", rs.path,
                               rs.line, rs.col_start, sev,
                               (int)needed, buf);

    // Source line + caret. Indent both consistently so the carets
    // line up under the offending text.
    if (rs.line_text_len > 0) {
      written += (size_t)fprintf(out, "    %.*s\n",
                                 (int)rs.line_text_len, rs.line_text);
      written += (size_t)fputs("    ", out);
      for (uint32_t i = 1; i < rs.col_start; i++) {
        // Preserve tabs in the source line as tabs in the caret line
        // so the carets stay column-aligned.
        char c = rs.line_text[i - 1];
        fputc(c == '\t' ? '\t' : ' ', out);
        written++;
      }
      for (uint32_t i = rs.col_start; i < rs.col_end; i++) {
        fputc('^', out);
        written++;
      }
      fputc('\n', out);
      written++;
    }
  } else {
    // No source context — bare header.
    written += (size_t)fprintf(out, "%s: %.*s\n", sev, (int)needed, buf);
  }

  if (buf != stack_buf)
    free(buf);
  return written;
}
