// Diagnostic readers — collection (walk db.diags), template
// formatting ({N} substitution), source-position resolution
// (byte_offset → file/line/col + the source-line text), and rust-style
// rendering.

#include "../diag/diag.h"
#include "../db.h"

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Phase P S8 — needed by the dual-path collector to recover a fn's
// owning file from its DefId (BODY-anchored diag routing).
extern TopLevelEntry db_query_top_level_entry(db_query_ctx *ctx,
                                              NamespaceId nsid, StrId name);
// Phase P S9 — BODY-anchor resolver. Walks the current body subtree
// preorder and returns the rel-th node. Defined in body_scopes.c.
extern SyntaxNode   *body_ast_id_resolve(db_query_ctx *ctx, DefId def,
                                         uint32_t rel);
// F3 (Phase P audit) — preorder array collector. Lets DiagResolver
// cache the entire body's preorder Vec for a single def, so K diags
// in one fn share one tree+walk instead of paying K builds.
extern void          body_ast_id_preorder_collect(db_query_ctx *ctx,
                                                  DefId def, Vec *out_nodes);
// Phase P cutover — shared bundle walker. For every Diag in `bundle`
// whose anchor.file_id matches `file`, append to out_diags. Skips
// NONE_KIND anchors; asserts non-zero file_id for live non-NONE
// (collector-side complement of span_of's emit-time assert in
// infer.c — catches a silent-drop wiring bug loud and early).
static void append_bundle_diags_for_file(struct db *s, FileId file,
                                         Vec *out_diags,
                                         DiagBundle *bundle) {
  (void)s;
  if (!bundle || bundle->items.count == 0)
    return;
  for (size_t j = 0; j < bundle->items.count; j++) {
    Diag *d = (Diag *)vec_get(&bundle->items, j);
    if (d->anchor.kind == DIAG_ANCHOR_NONE_KIND)
      continue;
    assert(d->anchor.file_id != 0 &&
           "append_bundle_diags_for_file: live non-NONE anchor with file_id == 0");
    if (file_id_eq((FileId){.idx = d->anchor.file_id}, file))
      vec_push(out_diags, d);
  }
}

/* ============================================================================
   Collection — walk each per-query DiagBundle column, append items whose
   anchor.file_id matches the target file. O(n_total_defs + n_namespaces)
   per call; cheap because most bundles are empty (early-skipped).
   ============================================================================
 */

// Phase P cutover — diagnostics live in per-query-result DiagBundle
// columns owned by their producing slot. Orphan-DefId staleness is
// structurally impossible now: reclaim_slot frees the bundle, and the
// liveness gate on each column's owning slot skips reads against a
// reclaimed row.
void db_collect_diags_for_file(struct db *s, FileId file, Vec *out_diags) {
  // Phase P cutover — bundle walks. Each column gets the same
  // shape: for every populated bundle (gated by liveness where the
  // owning query has a slot), append items whose anchor.file_id
  // matches. NONE_KIND skipped; file_id == 0 on a live non-NONE
  // anchor is a structural bug (assert, don't silently drop).
  {
    uint32_t fid_local = file_id_local(file);
    if (fid_local < s->files.parse_diags.count) {
      append_bundle_diags_for_file(
          s, file, out_diags,
          (DiagBundle *)vec_get(&s->files.parse_diags, fid_local));
    }
  }

  for (size_t def_idx = 1; def_idx < s->defs.kinds.count; def_idx++) {
    DefKind k = *(DefKind *)vec_get(&s->defs.kinds, def_idx);
    if (k == KIND_NONE)
      continue;
    // TYPE_OF_DECL — per-def, all kinds. Liveness on the per-kind
    // TYPE_OF_DECL slot.
    if (def_idx < s->defs.type_of_decl_diags.count &&
        db_slot_is_live(s, QUERY_TYPE_OF_DECL, (uint64_t)def_idx)) {
      append_bundle_diags_for_file(
          s, file, out_diags,
          (DiagBundle *)vec_get(&s->defs.type_of_decl_diags, def_idx));
    }
    if (k != KIND_FUNCTION)
      continue;
    uint32_t row = *(uint32_t *)vec_get(&s->defs.kind_row, def_idx);
    if (row == 0)
      continue;
    // INFER_BODY — per-fn body diags.
    if (row < paged_count(&s->fns.fn_body_diags) &&
        db_slot_is_live(s, QUERY_INFER_BODY, (uint64_t)def_idx)) {
      append_bundle_diags_for_file(
          s, file, out_diags,
          (DiagBundle *)paged_get(&s->fns.fn_body_diags, row));
    }
    // FN_SIGNATURE — per-fn signature diags.
    if (row < paged_count(&s->fns.signature_diags) &&
        db_slot_is_live(s, QUERY_FN_SIGNATURE, (uint64_t)def_idx)) {
      append_bundle_diags_for_file(
          s, file, out_diags,
          (DiagBundle *)paged_get(&s->fns.signature_diags, row));
    }
  }

  // CHECK — per-namespace driver bundle. No salsa liveness gate;
  // the driver is the sole owner and resets on every pass.
  for (size_t nsi = 1; nsi < s->namespaces.check_diags.count; nsi++) {
    append_bundle_diags_for_file(
        s, file, out_diags,
        (DiagBundle *)vec_get(&s->namespaces.check_diags, nsi));
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
    // Phase P: DiagAnchor is a tagged union. FILE_RAW renders the raw
    // byte range; BODY renders the abstract (decl, rel) pair (the
    // resolver does line/col lookup at publish time — this is just
    // for the formatter's %P preview).
    DiagAnchor a = arg->span;
    int n;
    if (a.kind == DIAG_ANCHOR_FILE_RAW) {
      n = snprintf(scratch, sizeof(scratch), "file#%u:%u-%u",
                   (unsigned)a.file_id, (unsigned)a.u.raw.start,
                   (unsigned)(a.u.raw.start + a.u.raw.length));
    } else if (a.kind == DIAG_ANCHOR_BODY) {
      n = snprintf(scratch, sizeof(scratch), "decl#%u:rel#%u",
                   (unsigned)a.u.body.decl, (unsigned)a.u.body.rel);
    } else {
      n = snprintf(scratch, sizeof(scratch), "<none>");
    }
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

// F3 (Phase P audit) — drop the cached body preorder array (release
// every red ref it holds, then vec_clear). Idempotent. Called on file
// switch, on resolver_free, and when a different DefId is requested.
static void resolver_body_cache_clear(DiagResolver *r) {
  for (size_t i = 0; i < r->cached_body_preorder.count; i++) {
    SyntaxNode *n = *(SyntaxNode **)vec_get(&r->cached_body_preorder, i);
    if (n)
      syntax_node_release(n);
  }
  if (r->cached_body_preorder.element_size != 0)
    vec_clear(&r->cached_body_preorder);
  r->cached_body_def_idx = 0;
}

void diag_resolver_init(DiagResolver *r, struct db *db) {
  r->db = db;
  r->cached_file_id = 0;
  r->cached_tree = NULL;
  r->cached_root = NULL;
  r->cached_body_def_idx = 0;
  vec_init(&r->cached_body_preorder, sizeof(SyntaxNode *));
}

void diag_resolver_free(DiagResolver *r) {
  resolver_body_cache_clear(r);
  vec_free(&r->cached_body_preorder);
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

  // F3 (Phase P audit) — file switch invalidates the body cache too;
  // the cached SyntaxNode refs live inside the old red tree.
  resolver_body_cache_clear(r);
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
  GreenNode *groot = *(GreenNode **)vec_get(&r->db->files.green_roots, local);
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

  // Phase P: dispatch on the new tagged-union anchor kind.
  // - FILE_RAW: today's byte-range path verbatim.
  // - BODY: S9 — resolve via body_ast_id_resolve (preorder walk
  //   through the current body subtree). RelAstId is structurally
  //   invariant under salsa cutoff, so this survives sibling reparse
  //   byte-shifts that would defeat any byte-range anchor.
  // - FILE: still TODO; the FILE-anchored emit migration is P7.2.
  if (anchor.kind == DIAG_ANCHOR_BODY) {
    if (anchor.file_id == 0)
      return false;
    FileId fid = {.idx = anchor.file_id};
    NamespaceId nsid = db_get_file_namespace(r->db, fid);
    // DeclKey IS the AstId we stashed at def_identity time. Combine
    // with nsid to hit def_by_identity (same key shape as scope.c:41).
    uint64_t key = ((uint64_t)nsid.idx << 32) | (uint32_t)anchor.u.body.decl;
    void *rp = hashmap_get(&r->db->def_by_identity, key);
    if (!rp) {
      TinySpan span = span_make(anchor.file_id, 0, 1);
      return db_resolve_span(r->db, span, out);
    }
    uint32_t row = (uint32_t)(uintptr_t)rp;
    DefId def = *(DefId *)paged_get(&r->db->def_identity.results, row);

    // F3 — slot-of-one body preorder cache. On a def switch (or first
    // resolve), drop any prior cached preorder and rebuild for THIS
    // def. Subsequent diags for the same def are O(1) array reads.
    if (r->cached_body_def_idx != def.idx) {
      resolver_body_cache_clear(r);
      body_ast_id_preorder_collect(r->db, def, &r->cached_body_preorder);
      r->cached_body_def_idx = def.idx;
    }
    if (anchor.u.body.rel >= r->cached_body_preorder.count) {
      TinySpan span = span_make(anchor.file_id, 0, 1);
      return db_resolve_span(r->db, span, out);
    }
    SyntaxNode *node =
        *(SyntaxNode **)vec_get(&r->cached_body_preorder, anchor.u.body.rel);
    if (!node) {
      TinySpan span = span_make(anchor.file_id, 0, 1);
      return db_resolve_span(r->db, span, out);
    }
    TextRange rng = syntax_node_text_range(node);
    // No release — node is owned by cached_body_preorder until file
    // switch / cache clear / resolver_free.
    TinySpan span = span_make(anchor.file_id, rng.start, rng.length);
    return db_resolve_span(r->db, span, out);
  }
  if (anchor.file_id == 0)
    return false;

  uint32_t start = anchor.u.raw.start;
  uint32_t length = anchor.u.raw.length;

  // Reparse-stable rebind via the cached red root. NULL root means
  // the file has no parsed tree (unparsed, evicted, or never opened)
  // — we fall through to the captured byte range. Note: the legacy
  // anchor used to carry a SyntaxKind hint for ptr_resolve; FILE_RAW
  // doesn't preserve that hint (the new FILE/BODY variants will use
  // their AstIdMaps instead), so we skip the ptr_resolve fast path
  // here and rely on the captured byte range alone.
  SyntaxNode *root_red = resolver_root_for(r, anchor.file_id);
  (void)root_red;

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
