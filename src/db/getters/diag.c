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

// db_diags_clear wipes a unit when its query recomputes, so a DiagList
// holds diagnostics only from its owning slot's latest run. BUT an
// edit that shifts a decl's byte range supersedes its DefId: the old
// slot is never recomputed (a fresh DefId took its place) and never
// cleared, so its stale diags linger in db.diag_lists. Gate collection
// on the owning slot's liveness — db_slot_is_live is true only when the
// slot is DONE and verified/stamped at the current revision. Orphaned
// slots are excluded; their diags vanish on the next collect. (Phase 0
// Bug 3 / G10.)
void db_collect_diags_for_file(struct db *s, FileId file, Vec *out_diags) {
  for (size_t r = 1; r < s->diag_lists.count; r++) {
    DiagList *dl = (DiagList *)vec_get(&s->diag_lists, r);
    if (dl->items.count == 0)
      continue;
    if (!db_slot_is_live(s, dl->owner_kind, dl->owner_key))
      continue; // orphaned unit — its diags are stale
    for (size_t i = 0; i < dl->items.count; i++) {
      Diag *d = (Diag *)vec_get(&dl->items, i);
      if (!file_id_eq((FileId){.idx = d->anchor.file_id}, file))
        continue;
      vec_push(out_diags, d);
    }
  }
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
    // DiagAnchor carries (file_id, kind, start, length). For the
    // secondary-location "file#N:start-end" preview, render the
    // captured byte range directly; reparse-stable re-resolution is
    // only worth the cost on the primary anchor.
    DiagAnchor a = arg->span;
    int n = snprintf(scratch, sizeof(scratch), "file#%u:%u-%u",
                     (unsigned)a.file_id, (unsigned)a.start,
                     (unsigned)(a.start + a.length));
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

  // Phase 8 — eviction gate. After workspace_did_evict_source the
  // per-file arena (which owns line_starts->data) and the source
  // text buffer are freed; downstream reads here would UAF without
  // this check. Diag rendering for an evicted file simply can't
  // resolve the span — callers get false and print without line
  // context.
  if (db_get_source_evicted(s, sid))
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
    size_t nsid = lo + (hi - lo) / 2;
    if (starts[nsid] <= byte_start)
      lo = nsid + 1;
    else
      hi = nsid;
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
   Anchor resolution — DiagAnchor → ResolvedSpan with reparse-stable
   rebind. Tries syntax_node_ptr_resolve against the file's current
   GreenNode root; if a matching node is still present, uses its
   CURRENT byte range (so a diag emitted before an edit that only
   shifted offsets still squiggles the right node). Falls back to the
   captured byte range when no matching node is found.
   ============================================================================
 */

void diag_resolver_init(DiagResolver *r, struct db *db) {
  r->db = db;
  r->cached_file_id = 0;
  r->cached_tree = NULL;
  r->cached_root = NULL;
}

void diag_resolver_free(DiagResolver *r) {
  if (r->cached_root)
    syntax_node_release(r->cached_root);
  if (r->cached_tree)
    syntax_tree_free(r->cached_tree);
  r->cached_root = NULL;
  r->cached_tree = NULL;
  r->cached_file_id = 0;
}

// Ensure the resolver's cache holds the red root for `file_id`.
// Returns the root or NULL if the file has no parsed tree (caller
// falls through to the byte-range path).
//
// Slot-of-one LRU: on file change, releases prior root + frees prior
// tree, then builds new. file_id == 0 is the sentinel "nothing
// cached" and always returns NULL.
static SyntaxNode *resolver_root_for(DiagResolver *r, uint16_t file_id) {
  if (file_id == 0)
    return NULL;
  if (file_id == r->cached_file_id)
    return r->cached_root;

  if (r->cached_root)
    syntax_node_release(r->cached_root);
  if (r->cached_tree)
    syntax_tree_free(r->cached_tree);
  r->cached_file_id = 0;
  r->cached_tree = NULL;
  r->cached_root = NULL;

  FileId fid = file_id_make_physical((uint32_t)file_id);
  uint32_t local = file_id_local(fid);
  if (local == 0 || local >= r->db->files.green_roots.count)
    return NULL;
  GreenNode *groot =
      *(GreenNode **)vec_get(&r->db->files.green_roots, local);
  if (!groot)
    return NULL;

  r->cached_tree = syntax_tree_new(groot);
  r->cached_root = syntax_tree_root(r->cached_tree);
  r->cached_file_id = file_id;
  return r->cached_root;
}

bool diag_resolver_resolve(DiagResolver *r, DiagAnchor anchor,
                           ResolvedSpan *out) {
  *out = (ResolvedSpan){0};
  out->path = "<unknown>";

  if (anchor.file_id == 0)
    return false;

  uint32_t start = anchor.start;
  uint32_t length = anchor.length;

  // Reparse-stable rebind via the cached red root. NULL root means
  // the file has no parsed tree (unparsed, evicted, or never opened)
  // — we fall through to the captured byte range.
  SyntaxNode *root_red = resolver_root_for(r, anchor.file_id);
  if (root_red && anchor.kind != SYNTAX_KIND_NONE) {
    SyntaxNodePtr ptr = {.kind = anchor.kind,
                         .range = {.start = start, .length = length}};
    SyntaxNode *match = syntax_node_ptr_resolve(ptr, root_red);
    if (match) {
      TextRange tr = syntax_node_text_range(match);
      start = tr.start;
      length = tr.length;
      syntax_node_release(match);
    }
  }

  TinySpan span = span_make(anchor.file_id, start, length);
  return db_resolve_span(r->db, span, out);
}

/* ============================================================================
   Rust-style rendering — used by the CLI driver. LSP renders separately
   (via db_format_diag + diag_resolver_resolve directly into LSP Range/
   Position JSON; see src/consumers/lsp/server.c).
   ============================================================================
 */

size_t diag_resolver_print(DiagResolver *r, const Diag *d, FILE *out) {
  char stack_buf[256];
  char *buf = stack_buf;
  size_t needed = db_format_diag(r->db, d, stack_buf, sizeof(stack_buf));
  if (needed >= sizeof(stack_buf)) {
    buf = (char *)malloc(needed + 1);
    if (!buf)
      return (size_t)fwrite(stack_buf, 1, strlen(stack_buf), out);
    db_format_diag(r->db, d, buf, needed + 1);
  }

  const char *sev = d->severity == DIAG_ERROR     ? "error"
                    : d->severity == DIAG_WARNING ? "warning"
                    : d->severity == DIAG_INFO    ? "info"
                                                  : "note";

  ResolvedSpan rs;
  bool resolved = diag_resolver_resolve(r, d->anchor, &rs);

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
