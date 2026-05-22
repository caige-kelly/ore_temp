// Diagnostic readers — collection (walk db.diags), template
// formatting ({N} substitution), source-position resolution
// (byte_offset → file/line/col + the source-line text), and rust-style
// rendering.

#include "../diag/diag.h"
#include "../db.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
   Collection — walk the dense db.diag_lists Vec, copy diag entries.
   O(emitted diags). Row 0 is the reserved sentinel.
   ============================================================================
 */

// Each DiagList's diags are current by construction: db_diags_clear wipes
// a unit when its query recomputes (or when an input setter stales it),
// so a DiagList only ever holds diagnostics from the latest run. No
// slot-state check is needed.
static void collect_into(struct db *s, Vec *out, bool filter_by_file,
                         FileId target) {
  for (size_t r = 1; r < s->diag_lists.count; r++) {
    DiagList *dl = (DiagList *)vec_get(&s->diag_lists, r);
    for (size_t i = 0; i < dl->items.count; i++) {
      Diag *d = (Diag *)vec_get(&dl->items, i);
      if (filter_by_file &&
          !file_id_eq(file_id_make_physical(span_file(d->primary)), target))
        continue;
      vec_push(out, d);
    }
  }
}

void db_collect_diags_all(struct db *s, Vec *out_diags) {
  collect_into(s, out_diags, false, FILE_ID_NONE);
}

void db_collect_diags_for_file(struct db *s, FileId file, Vec *out_diags) {
  collect_into(s, out_diags, true, file);
}

/* ============================================================================
   Render — resolve template + args into formatted text.
   ============================================================================
 */

static void append(char *buf, size_t buflen, size_t *written, const char *s,
                   size_t len) {
  if (!len)
    return;
  size_t cap = buflen ? buflen - 1 : 0;
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

static void format_arg(struct db *s, const DiagArg *arg, char *buf,
                       size_t buflen, size_t *written) {
  char scratch[64];
  switch (arg->kind) {
  case DIAG_ARG_NONE:
    append_cstr(buf, buflen, written, "<none>");
    return;

  case DIAG_ARG_CHAR: {
    uint32_t ch = arg->ch;
    int n;
    if (ch < 128 && isprint((int)ch))
      n = snprintf(scratch, sizeof(scratch), "'%c'", (char)ch);
    else
      n = snprintf(scratch, sizeof(scratch), "'\\x%02X'", ch & 0xFF);
    if (n < 0)
      n = 0;
    append(buf, buflen, written, scratch, (size_t)n);
    return;
  }

  case DIAG_ARG_STR_ID: {
    const char *text = pool_get(&s->strings, arg->str);
    append_cstr(buf, buflen, written, text ? text : "<bad str>");
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
    char wide[256];
    size_t needed = db_format_type(s, arg->type, wide, sizeof(wide));
    size_t to_copy = needed < sizeof(wide) ? needed : sizeof(wide) - 1;
    append(buf, buflen, written, wide, to_copy);
    return;
  }

  case DIAG_ARG_SPAN: {
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

size_t db_format_diag(struct db *s, const Diag *d, char *buf, size_t buflen) {
  if (!s || !d) {
    if (buf && buflen)
      buf[0] = '\0';
    return 0;
  }

  size_t written = 0;
  const char *tpl = pool_get(&s->strings, d->template_id);
  if (!tpl) {
    append_cstr(buf, buflen, &written, "<bad template>");
    if (buf && buflen)
      buf[written < buflen ? written : buflen - 1] = '\0';
    return written;
  }
  size_t tlen = strlen(tpl);

  // Walk template, substitute {N} placeholders. {{ }} are literal-brace
  // escapes.
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
      size_t j = i + 1;
      uint32_t idx = 0;
      bool ok = false;
      while (j < tlen && tpl[j] >= '0' && tpl[j] <= '9') {
        idx = idx * 10 + (uint32_t)(tpl[j] - '0');
        j++;
        ok = true;
      }
      if (ok && j < tlen && tpl[j] == '}') {
        if (idx < d->n_args && d->args)
          format_arg(s, &d->args[idx], buf, buflen, &written);
        else
          append(buf, buflen, &written, &tpl[i], j - i + 1);
        i = j + 1;
        continue;
      }
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

/* ============================================================================
   Span resolution — TinySpan → (path, line, col_start, col_end, line_text).
   ============================================================================
 */

bool db_resolve_span(struct db *s, TinySpan span, ResolvedSpan *out) {
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

  const char *text = *(const char **)vec_get(&s->sources.texts, sid.idx);
  uint32_t text_len = *(uint32_t *)vec_get(&s->sources.text_lens, sid.idx);
  if (!text)
    return false;

  FileArray *line_starts = (FileArray *)vec_get(&s->files.line_starts, local);
  if (!line_starts || line_starts->count == 0)
    return false;

  uint32_t byte_start = span_start(span);
  uint32_t byte_end = span_end(span);
  if (byte_start > text_len)
    return false;

  // Binary search for line containing byte_start.
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

  uint32_t line_start_byte = starts[line_idx];
  uint32_t line_end_byte =
      (line_idx + 1 < line_starts->count) ? starts[line_idx + 1] : text_len;
  while (line_end_byte > line_start_byte &&
         (text[line_end_byte - 1] == '\n' || text[line_end_byte - 1] == '\r'))
    line_end_byte--;

  out->line_text = text + line_start_byte;
  out->line_text_len = line_end_byte - line_start_byte;

  uint32_t end_clamped = byte_end > line_end_byte ? line_end_byte : byte_end;
  out->col_end = end_clamped > line_start_byte
                     ? end_clamped - line_start_byte + 1
                     : out->col_start + 1;
  if (out->col_end <= out->col_start)
    out->col_end = out->col_start + 1;

  return true;
}

/* ============================================================================
   Rust-style rendering — used by the CLI driver. LSP renders separately
   (via db_format_diag + db_resolve_span directly into LSP Range/Position
   JSON; see src/consumers/lsp/server.c).
   ============================================================================
 */

size_t db_print_diag(struct db *s, const Diag *d, FILE *out) {
  char stack_buf[256];
  char *buf = stack_buf;
  size_t needed = db_format_diag(s, d, stack_buf, sizeof(stack_buf));
  if (needed >= sizeof(stack_buf)) {
    buf = (char *)malloc(needed + 1);
    if (!buf)
      return (size_t)fwrite(stack_buf, 1, strlen(stack_buf), out);
    db_format_diag(s, d, buf, needed + 1);
  }

  const char *sev = d->severity == DIAG_ERROR     ? "error"
                    : d->severity == DIAG_WARNING ? "warning"
                    : d->severity == DIAG_INFO    ? "info"
                                                  : "note";

  ResolvedSpan rs;
  bool resolved = db_resolve_span(s, d->primary, &rs);

  size_t written = 0;
  if (resolved) {
    written += (size_t)fprintf(out, "%s:%u:%u: %s: %.*s\n", rs.path, rs.line,
                               rs.col_start, sev, (int)needed, buf);

    if (rs.line_text_len > 0) {
      written += (size_t)fprintf(out, "    %.*s\n", (int)rs.line_text_len,
                                 rs.line_text);
      written += (size_t)fputs("    ", out);
      for (uint32_t i = 1; i < rs.col_start; i++) {
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
    written += (size_t)fprintf(out, "%s: %.*s\n", sev, (int)needed, buf);
  }

  if (buf != stack_buf)
    free(buf);
  return written;
}
