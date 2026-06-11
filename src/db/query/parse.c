// Parse layer (Phase C) — file_ast and the per-decl queries that read it.
//
// Model B (pull early-cutoff firewall): file_ast is the ONLY query that
// parses. It produces the file's green tree, persists line_starts for
// position/diag rendering, and drains lex+parse errors into the
// FILE_AST diagnostic unit. The downstream parse queries (namespace_items,
// top_level_entry, file_imports) DEPEND on file_ast and derive their
// results from the green tree, each emitting a position-independent
// content-hash fingerprint so a sibling-decl edit re-runs only the cheap
// derivation, not the downstream that cache-hits on the stable fp.
//
// Pure-query contract (see engine.h): write the result column BEFORE
// db_query_succeed; never write outside the query's own column.

#define ORE_ENGINE_PRIVATE
#include "engine.h"
#include "engine_internal.h"
#include "result_columns.h" // db.h, ids.h, syntax.h, intern_pool.h

#include "../diag/diag.h"   // db_emit, diag_anchor_make, DIAG_ERROR

#include "../../ast/ast_decl.h" // FnDef_name / StructDef_name / … (+ SK_*)
#include "../../ast/ast_expr.h" // BuiltinExpr / Literal (@import sites)
#include "../../lexer/layout.h"
#include "../../lexer/lexer.h"
#include "../../lexer/token.h"
#include "../../parser_new/parser.h"
#include "../../syntax/node_cache.h" // green_node_hash_of, green_structural_hash

#include "../../support/data_structure/arena.h"
#include "../../support/data_structure/vec.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Emit one diagnostic per token byte range. Anchors to the token's
// (kind, start, length) so render-time resolution rebinds via the green
// tree. Called only inside file_ast's frame, so db_emit routes to the
// active FILE_AST unit.
static void emit_at_token(struct db *s, uint32_t file_local, const Token *t,
                          const char *msg) {
  DiagAnchor a = diag_anchor_make((uint16_t)file_local, t->kind, t->start, // LINT_FILE_RAW_OK: parser emits inside FILE_AST's frame; bundle resets on every recompute, so byte offsets are always fresh
                                  t->byte_end - t->start);
  db_emit(s, DIAG_ERROR, a, "%s", msg);
}

// SOURCE_TEXT input reader. Returns the source text AND records a dep on
// the (SOURCE_TEXT, sid) input slot via the begin→CACHED path, so the
// caller is invalidated precisely when THIS source's content changes.
// The input slot is ALWAYS pre-set DONE by the source setters
// (create_source_row / db_set_source_text via db_input_set), so begin
// returns CACHED; COMPUTE is unreachable for any sid with a slot row.
// (An invalid sid asserts in routing, not here.) An input slot must
// never be db_query_succeed'd — that's the derived path — so a reached
// COMPUTE is a contract violation, not a value to fabricate.
const char *db_query_file_text(db_query_ctx *ctx, SourceId sid) {
  struct db *s = (struct db *)ctx;
  DB_QUERY_GUARD(ctx, QUERY_SOURCE_TEXT, (uint64_t)sid.idx,
                 /* on_cached */ db_get_source_text(s, sid),
                 /* on_cycle  */ NULL,
                 /* on_error  */ NULL);
  assert(0 && "db_query_file_text: SOURCE_TEXT slot must be pre-set via "
              "db_input_set — COMPUTE is unreachable");
  return NULL;
}

// LINE_INDEX — per-file line-start byte offsets, for line/col + span
// rendering. A PURE query parallel to (not downstream of) file_ast: line
// starts are a byte scan of the source — no lexer needed — so it depends
// only on SOURCE_TEXT. Result lives in files.line_starts (its result
// column), hosted in the per-file arena so it survives the request for
// render-time reads and is reclaimed by arena_free on eviction.
//
// Matches the lexer's line-break semantics (lex_newline): \n, \r\n, and
// a lone \r each begin a new line. line_starts[0]=0; each subsequent
// entry is the offset just past a break. NOTE: line_index is currently
// the sole writer of files.arenas[fid]; when file_imports (C.1) needs
// per-file-arena storage, the two must coordinate (separate arenas or a
// shared reset owner).
FileArray db_query_line_index(db_query_ctx *ctx, FileId fid) {
  struct db *s = (struct db *)ctx;
  FileArray empty = {0};
  DB_QUERY_GUARD(ctx, QUERY_LINE_INDEX, (uint64_t)fid.idx,
                 /* on_cached */ line_index_read(s, fid),
                 /* on_cycle  */ empty,
                 /* on_error  */ empty);

  uint32_t local = file_id_local(fid);
  SourceId sid = db_get_file_source(s, fid);
  const char *src = db_query_file_text(ctx, sid); // records SOURCE_TEXT dep
  uint32_t len = db_get_source_len(s, sid);

  if (!src) {
    line_index_write(s, fid, empty);
    db_query_succeed(ctx, QUERY_LINE_INDEX, (uint64_t)fid.idx,
                     FINGERPRINT_NONE);
    return empty;
  }

  // Two-pass: count breaks (so the arena alloc is exact-sized), then
  // fill. Source text is already hot in cache from the lexer/parser, so
  // the second scan is essentially free. Avoids the prior malloc-then-
  // memcpy-into-arena dance (G3).
  uint32_t lines = 1; // line 0 begins at offset 0
  for (uint32_t i = 0; i < len; i++) {
    char c = src[i];
    if (c == '\n' || c == '\r') {
      if (c == '\r' && i + 1 < len && src[i + 1] == '\n')
        i++;
      lines++;
    }
  }

  Arena *fa = (Arena *)paged_get(&s->files.arenas, local); // LINT_UNTRACKED_OK: producer (LINE_INDEX) owns its arena
  arena_reset(fa);
  size_t bytes = (size_t)lines * sizeof(uint32_t);
  uint32_t *arr = (uint32_t *)arena_alloc_raw(fa, bytes);
  uint32_t w = 0;
  arr[w++] = 0;
  for (uint32_t i = 0; i < len; i++) {
    char c = src[i];
    if (c == '\n' || c == '\r') {
      if (c == '\r' && i + 1 < len && src[i + 1] == '\n')
        i++;
      arr[w++] = i + 1;
    }
  }

  FileArray result = {.data = arr, .count = lines};
  line_index_write(s, fid, result);
  db_query_succeed(ctx, QUERY_LINE_INDEX, (uint64_t)fid.idx,
                   db_fp_bytes(result.data, bytes));
  return result;
}

struct GreenNode *db_query_file_ast(db_query_ctx *ctx, FileId fid) {
  struct db *s = (struct db *)ctx;
  DB_QUERY_GUARD(ctx, QUERY_FILE_AST, (uint64_t)fid.idx,
                 /* on_cached */ file_ast_read(s, fid),
                 /* on_cycle  */ NULL,
                 /* on_error  */ NULL);

  // Phase P cutover — install the file's parse_diags sink so every
  // emit during this compute lands in the per-file DiagBundle. Reset
  // first so this generation starts clean.
  DiagBundle *parse_bundle = parse_diags_slot(s, fid);
  if (parse_bundle)
    diag_bundle_reset(parse_bundle);
  DiagSink parse_sink = parse_diags_sink_open(s, fid);
  db_query_frame_set_sink(ctx, parse_bundle ? &parse_sink : NULL);

  uint32_t local = file_id_local(fid);
  SourceId sid = db_get_file_source(s, fid);
  const char *src =
      db_query_file_text(ctx, sid); // reads text + records input dep
  uint32_t len = db_get_source_len(s, sid);

  if (!src) {
    // Evicted / missing source — no tree this revision.
    struct GreenNode *old = file_ast_read(s, fid);
    file_ast_write(s, fid, NULL);
    if (old)
      green_node_release(old);
    db_query_succeed(ctx, QUERY_FILE_AST, (uint64_t)fid.idx, FINGERPRINT_NONE);
    return NULL;
  }

  // Token stream + the lexer's line-start vec are SCRATCH in the
  // request arena (the designated request-scoped scratch — bump-
  // allocated, reclaimed in bulk at db_request_end, no per-parse
  // malloc churn on the reparse hot path). file_ast is PURE: its only
  // output is the green root (file_ast_write). The persistent line
  // index is QUERY_LINE_INDEX; the lexer needs a line_starts vec for
  // column tracking during layout, but we discard it here.
  //
  // Safe fixed capacities (arena vecs can't grow): line starts ≤ line
  // breaks + 1 ≤ len+1; the unified stream is trivia+real (partition
  // the source ≤ len) + layout virtuals (≤ a few per line ≤ ~3*len),
  // so 4*len+64 is a conservative upper bound.
  Vec ls_scratch;
  vec_init_in_arena(&ls_scratch, &s->request_arena, (size_t)len + 2,
                    sizeof(uint32_t));
  Vec toks;
  vec_init_in_arena(&toks, &s->request_arena, 4 * (size_t)len + 64,
                    sizeof(Token));

  LexCursor cur;
  lex_begin(&cur, src, len, &s->strings, &ls_scratch);
  layout_stream(&cur, &ls_scratch, &toks);

  Vec errors;
  vec_init(&errors, sizeof(ParseError));
  struct GreenNode *root =
      parse_file_green(&toks, src, &s->strings, s->node_cache, &errors);

  // Publish the green root, releasing the prior parse's root. parse
  // returns +1 ref (RETURNS_OWNED); we donate it to the column and drop
  // the column's previous ref. Hash-consed shared subtrees survive via
  // their own refcounts; releasing is correct even if root == old.
  struct GreenNode *old = file_ast_read(s, fid);
  file_ast_write(s, fid, root);
  if (old)
    green_node_release(old);

  // Drain diagnostics into the FILE_AST unit (the active frame). Parse
  // errors carry a token index; map it to that token's byte range.
  const Token *tv = (const Token *)toks.data;
  for (size_t i = 0; i < errors.count; i++) {
    ParseError *e = (ParseError *)vec_get(&errors, i);
    if (e->tok_pos < toks.count)
      emit_at_token(s, local, &tv[e->tok_pos], e->msg);
    else
      db_emit(s, DIAG_ERROR, DIAG_ANCHOR_NONE, "%s", e->msg);
  }
  vec_free(&errors);

  // Lex errors ride as SK_LEX_ERROR tokens in the stream.
  for (size_t i = 0; i < toks.count; i++)
    if (tv[i].kind == SK_LEX_ERROR)
      emit_at_token(s, local, &tv[i], "lexical error");

  // No vec_free: toks/ls_scratch are request-arena-backed; the arena
  // reclaims them at db_request_end (free() on arena memory is invalid).

  Fingerprint fp =
      root ? db_fp_u64(green_node_hash_of(root)) : FINGERPRINT_NONE;
  db_query_succeed(ctx, QUERY_FILE_AST, (uint64_t)fid.idx, fp);
  return root;
}

// FILE_SET input reader. Records a dep on the (FILE_SET, nsid) input slot
// via begin→CACHED, so the caller is invalidated precisely when THIS
// namespace's file-set membership changes — the per-namespace edge a
// coarse DUR_MEDIUM bump can't give (db_create_file folds each new file's
// id into this slot's fingerprint). The slot is ALWAYS pre-set DONE by
// db_create_namespace / db_create_file (via db_input_set), so begin
// returns CACHED; COMPUTE is unreachable for any namespace that exists.
// An input slot must never be db_query_succeed'd, so a reached COMPUTE is
// a contract violation, not a value to fabricate.
static Fingerprint db_query_namespace_file_set(db_query_ctx *ctx,
                                               NamespaceId nsid) {
  DB_QUERY_GUARD(ctx, QUERY_FILE_SET, (uint64_t)nsid.idx,
                 /* on_cached */
                 db_slot_fingerprint(ctx, QUERY_FILE_SET, (uint64_t)nsid.idx),
                 /* on_cycle  */ FINGERPRINT_NONE,
                 /* on_error  */ FINGERPRINT_NONE);
  assert(0 && "db_query_namespace_file_set: FILE_SET slot must be pre-set "
              "via db_input_set — COMPUTE is unreachable");
  return FINGERPRINT_NONE;
}

// Intern a top-level decl's name. Returns STR_ID_NONE for a node that
// isn't a named top-level decl (or a decl missing its name token, e.g. a
// parse error). The per-kind switch is the single place that knows which
// wrapper owns the SK_IDENT — adding a new top-level decl kind means
// adding a case here. The interned StrId is content-addressed in
// s->strings, so it compares equal to a query `name` interned the same
// way (idempotent; the lexer already populates this pool during file_ast).
// Classify a top-level decl child into its (name, DefKind) in ONE cast —
// the single source of truth for "what kind of def is this." Ore is
// expression-oriented (parse_decl.c): the dedicated forms (SK_FN_DECL,
// SK_STRUCT_DECL, … — e.g. a top-level `effect E {}`) carry their own
// name, while `::`/`:=` binds (SK_BIND_DECL) carry the name
// on the wrapper and the SEMANTIC kind on the RHS value. Modifiers
// (`comptime`/`pub`/…) are sibling tokens, not wrappers, so the value
// node's own kind is already semantic — peek it and map straight to
// DefKind (a lambda value is KIND_FUNCTION). Feeds NamespaceItem.kind and
// the AstId, so a struct→enum retype is a clean new-identity change rather
// than a db_def_set_kind "kind is fixed" assert. Returns KIND_NONE with
// *name_out = STR_ID_NONE for a child that isn't a named decl.
// Classify a top-level child in ONE pass: name + semantic DefKind + DefMeta.
// The wrapper is cast once (name; and for `::`/`:=` binds the RHS value node
// gives the SEMANTIC kind — `Foo :: struct{}` is KIND_STRUCT, not a constant).
// Then the modifier tokens (`pub`/`pvt`/`comptime`/`distinct`/`abstract`/
// `scoped`/`linear`/`named`) are read off the green child sequence: they are
// plain tokens emitted AFTER the bind op (emit_bind_decl_tail), so scan tokens
// that follow the first bind-op token (SK_COLON_COLON / SK_COLON_EQ /
// SK_COLON). Scanning after the bind op — not before — means a decl literally
// named `pub` (its name IDENT precedes the bind op) is not read as a visibility
// modifier. Default visibility is VIS_PRIVATE (0). One call → (name, kind,
// meta).
static DefKind decl_classify(struct db *s, SyntaxNode *child, StrId *name_out,
                             DefMeta *meta_out) {
  *name_out = STR_ID_NONE;
  *meta_out = 0; // VIS_PRIVATE
  SyntaxToken *tok = NULL;
  DefKind kind = KIND_NONE;
  SyntaxNode *val = NULL; // borrowed RHS value to release (binds only)

  switch ((OreSyntaxKind)syntax_node_kind(child)) {
  // Top-level decls are ALWAYS SK_BIND_DECL (`name :: …`) / SK_DESTRUCTURE_DECL;
  // the parser never emits a bare top-level fn/struct/enum/union/effect — those
  // appear only as a bind's RHS value (promoted below). SK_DESTRUCTURE_DECL is
  // handled upstream in the walk (push_destructure_items), so SK_BIND_DECL is
  // the only arm here (7.0b).
  case SK_BIND_DECL: {
    BindDef d;
    if (BindDef_cast(child, &d)) {
      tok = BindDef_name(&d);
      val = BindDef_value(&d);
      // Mutability is a property of the bind-op token, not the node kind:
      // `::` → KIND_CONSTANT, `:=`/`:` → KIND_VARIABLE (7.0a).
      kind = BindDef_is_const(&d) ? KIND_CONSTANT : KIND_VARIABLE;
    }
    break;
  }
  default:
    return KIND_NONE;
  }
  if (!tok) {
    if (val)
      syntax_node_release(val);
    return KIND_NONE;
  }
  TextRange r = syntax_token_text_range(tok);
  *name_out = pool_intern(&s->strings, syntax_token_text(tok), r.length);
  syntax_token_release(tok);

  // For a bind, the SEMANTIC kind is the RHS value node's kind.
  if (val) {
    switch ((OreSyntaxKind)syntax_node_kind(val)) {
    case SK_STRUCT_DECL:
      kind = KIND_STRUCT;
      break;
    case SK_UNION_DECL:
      kind = KIND_UNION;
      break;
    case SK_ENUM_DECL:
      kind = KIND_ENUM;
      break;
    case SK_LAMBDA_EXPR:
      kind = KIND_FUNCTION;
      break;
    case SK_EFFECT_DECL:
      kind = KIND_EFFECT;
      break;
    case SK_DISTINCT_TYPE:
      kind = KIND_DISTINCT; // `MyT :: distinct u8` — Slice 6.19
      break;
    default:
      break; // keep KIND_CONSTANT / KIND_VARIABLE
    }
    syntax_node_release(val);
  }

  // Modifiers — scan green tokens after the bind op (same node, no re-cast).
  const struct GreenNode *g = syntax_node_green(child);
  if (g) {
    DefMeta meta = 0; // VIS_PRIVATE
    uint32_t n = green_node_num_children(g);
    bool past_bind = false;
    for (uint32_t i = 0; i < n; i++) {
      GreenElement e = green_node_child(g, i);
      if (e.kind != GREEN_ELEM_TOKEN)
        continue;
      SyntaxKind tk = green_token_kind(e.token);
      if (!past_bind) {
        if (tk == SK_COLON_COLON || tk == SK_COLON_EQ || tk == SK_COLON)
          past_bind = true;
        continue; // skip the name (and the bind op itself)
      }
      if (tk == SK_COMPTIME_KW) {
        meta |= META_COMPTIME;
        continue;
      }
      if (tk != SK_IDENT)
        continue;
      const char *t = green_token_text(e.token);
      uint32_t len = green_token_text_len(e.token);
      if (len == 3 && memcmp(t, "pub", 3) == 0)
        meta = (meta & ~META_VIS_MASK) | VIS_PUBLIC;
      else if (len == 3 && memcmp(t, "pvt", 3) == 0)
        meta = (meta & ~META_VIS_MASK) | VIS_PRIVATE;
      else if (len == 8 && memcmp(t, "abstract", 8) == 0)
        meta = (meta & ~META_VIS_MASK) | VIS_ABSTRACT;
      else if (len == 6 && memcmp(t, "scoped", 6) == 0)
        meta |= META_SCOPED;
      else if (len == 6 && memcmp(t, "linear", 6) == 0)
        meta |= META_LINEAR;
      else if (len == 5 && memcmp(t, "named", 5) == 0)
        meta |= META_NAMED;
    }
    *meta_out = meta;
  }
  return kind;
}

// A top-level destructure `a, b :: rhs` / `a, b := rhs` binds N names at once,
// so decl_classify (one name per node) can't describe it. The walk routes the
// SK_DESTRUCTURE_DECL node here instead: push ONE NamespaceItem per LHS target
// (each a bare SK_REF_EXPR under the SK_PRODUCT_EXPR). Const vs var is the
// bind-op token (`::` → const); the shared `ptr` is the destructure node, so
// typecheck can recover a target's type from the RHS by its product position.
static void push_destructure_items(struct db *s, SyntaxNode *dnode, FileId file,
                                   Vec *found) {
  DefKind kind = KIND_VARIABLE; // `:=`; promoted to const on `::`
  SyntaxNode *prod = NULL;      // borrowed LHS SK_PRODUCT_EXPR
  uint32_t n = syntax_node_num_children(dnode);
  for (uint32_t i = 0; i < n; i++) {
    SyntaxElement el = syntax_node_child_or_token(dnode, i);
    if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      if (syntax_token_kind(el.token) == SK_COLON_COLON)
        kind = KIND_CONSTANT;
      syntax_token_release(el.token);
    } else if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      if (!prod && syntax_node_kind(el.node) == SK_PRODUCT_EXPR)
        prod = el.node; // borrow for the target walk; released below
      else
        syntax_node_release(el.node);
    }
  }
  if (!prod)
    return;

  uint32_t tn = syntax_node_num_children(prod);
  for (uint32_t i = 0; i < tn; i++) {
    SyntaxElement el = syntax_node_child_or_token(prod, i);
    if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      syntax_token_release(el.token);
      continue;
    }
    if (el.kind != SYNTAX_ELEM_NODE || !el.node)
      continue;
    RefExpr ref;
    SyntaxToken *nt = RefExpr_cast(el.node, &ref) ? RefExpr_name(&ref) : NULL;
    syntax_node_release(el.node);
    if (!nt)
      continue; // a non-name target (malformed) — skip, not register
    TextRange r = syntax_token_text_range(nt);
    StrId name = pool_intern(&s->strings, syntax_token_text(nt), r.length);
    syntax_token_release(nt);
    if (name.idx == 0)
      continue;
    NamespaceItem item = {
        .id = ast_id_compute(name),
        .name = name,
        .file = file,
        .ptr = syntax_node_ptr_new(dnode), // shared by all targets
        .meta = 0, // destructure targets carry no modifiers
        .kind = kind,
    };
    vec_push(found, &item);
  }
  syntax_node_release(prod);
}

// Order items by AstId — canonical (reorder-stable) order for the
// membership fingerprint + binary search by def_identity.
static int cmp_item_by_astid(const void *a, const void *b) {
  uint32_t x = ((const NamespaceItem *)a)->id.idx;
  uint32_t y = ((const NamespaceItem *)b)->id.idx;
  return (x < y) ? -1 : (x > y) ? 1 : 0;
}

// NAMESPACE_ITEMS — the per-namespace top-level items index: the single
// memoized, dep-tracked answer to "what are the top-level items of
// namespace N?" Walks the namespace's file-set ONCE per recompute (not
// once per name), producing an enumerable NamespaceItem[] (SORTED by AstId)
// that top_level_entry reads and that Phase-D enumeration consumers
// (namespace_type, completion) build on. Deps: FILE_SET(nsid) — membership
// change re-runs the walk — and file_ast(file) for each file.
//
// fp is a MEMBERSHIP signal: it folds the items' AstIds (kind+name) in
// sorted order — NOT content, NOT byte ranges. So a content edit (or a
// pure shift, or a reorder) recomputes (refreshing every item's ptr in the
// result column) but BACKDATES (fp unchanged) — firewalling the name layer
// (def_identity, namespace_scopes) from body edits; only add/remove/rename
// flips it. Per-decl CONTENT is top_level_entry's structural-hash fp, not
// stored here. Result body is a standalone malloc (NamespaceItem[]),
// replaced wholesale here and freed at teardown (db_ids_free).
FileArray db_query_namespace_items(db_query_ctx *ctx, NamespaceId nsid) {
  struct db *s = (struct db *)ctx;
  FileArray empty = {0};
  DB_QUERY_GUARD(ctx, QUERY_NAMESPACE_ITEMS, (uint64_t)nsid.idx,
                 /* on_cached */ namespace_items_read(s, nsid),
                 /* on_cycle  */ empty,
                 /* on_error  */ empty);

  (void)db_query_namespace_file_set(ctx, nsid); // records FILE_SET dep

  uint32_t nfiles = 0;
  const FileId *files = db_get_namespace_files(s, nsid, &nfiles);

  Vec found;
  vec_init(&found, sizeof(NamespaceItem));

  for (uint32_t fi = 0; fi < nfiles; fi++) {
    struct GreenNode *root = db_query_file_ast(ctx, files[fi]); // records dep
    if (!root)
      continue;
    SyntaxTree *tree = syntax_tree_new(root);   // BORROWS root
    SyntaxNode *rroot = syntax_tree_root(tree); // +1
    SyntaxChildren it;
    syntax_children_init(&it, rroot, SYNTAX_DIR_NEXT);
    for (SyntaxNode *child; (child = syntax_children_next(&it));) {
      // A destructure binds N names from one node — decl_classify yields only
      // one, so route it to the per-target walk (pushes one item per name).
      if (syntax_node_kind(child) == SK_DESTRUCTURE_DECL) {
        push_destructure_items(s, child, files[fi], &found);
        syntax_node_release(child);
        continue;
      }
      StrId name;
      DefMeta meta;
      // One call → (name, DefKind, DefMeta). DefKind is the semantic kind
      // (peeks the RHS value for `::`/`:=` binds) so nominals/fns classify
      // correctly and the AstId tracks semantic identity (a kind change →
      // a new id); DefMeta carries pub/comptime/distinct/… modifiers.
      DefKind kind = decl_classify(s, child, &name, &meta);
      if (name.idx != 0) {
        NamespaceItem item = {
            .id = ast_id_compute(name),
            .name = name,
            .file = files[fi],
            .ptr = syntax_node_ptr_new(child),
            .meta = meta,
            .kind = kind,
        };
        vec_push(&found, &item);
      }
      syntax_node_release(child);
    }
    syntax_children_free(&it);
    syntax_node_release(rroot);
    syntax_tree_free(tree);
  }

  // Sort by AstId → canonical order, so the membership fingerprint is
  // reorder-stable and def_identity can binary-search.
  if (found.count > 1)
    qsort(found.data, found.count, sizeof(NamespaceItem), cmp_item_by_astid);

  // Membership fingerprint: fold (AstId, meta, kind) per item — NOT content.
  // Body edits leave them stable so the name layer cuts off, but add/remove/
  // rename (AstId) AND a modifier toggle (`pub`/`distinct`/… → meta) AND
  // a kind change (kind) flip it, so def_identity re-runs to refresh defs.meta
  // and defs.kinds, and namespace_type re-filters its pub member set.
  // (meta and kind are folded here, NOT into the AstId: a kind/pub change
  // must NOT change the DefId — it's the same decl, just reclassified.)
  Fingerprint fp = FINGERPRINT_NONE;
  const NamespaceItem *sorted = (const NamespaceItem *)found.data;
  for (uint32_t i = 0; i < found.count; i++) {
    Fingerprint item_fp = db_fp_combine(db_fp_u64((uint64_t)sorted[i].id.idx),
                                        db_fp_combine(db_fp_u64((uint64_t)sorted[i].meta),
                                                      db_fp_u64((uint64_t)sorted[i].kind)));
    fp = db_fp_combine(fp, item_fp);
  }

  // Replace the previous body wholesale: free old malloc, install new.
  FileArray old = namespace_items_read(s, nsid);
  free(old.data);

  FileArray result = {.data = NULL, .count = (uint32_t)found.count};
  if (found.count) {
    size_t bytes = (size_t)found.count * sizeof(NamespaceItem);
    result.data = malloc(bytes);
    memcpy(result.data, found.data, bytes);
  }
  vec_free(&found);

  namespace_items_write(s, nsid, result);
  db_query_succeed(ctx, QUERY_NAMESPACE_ITEMS, (uint64_t)nsid.idx, fp);
  return result;
}

// TOP_LEVEL_ENTRY — the per-name CONTENT firewall, a reader over
// NAMESPACE_ITEMS. Resolves `name` within `nsid` to its stable AstId
// identity + a CURRENT node_ptr, and emits a POSITION-INDEPENDENT content
// fingerprint — the decl's trivia-excluding structural hash, computed HERE
// (NAMESPACE_ITEMS no longer carries it; its fp is membership-only).
// Downstream name/type queries depend on THIS slot, so a sibling-decl edit
// leaves their cache valid: this fp is unchanged.
//
// Deps: NAMESPACE_ITEMS(nsid) (the lookup + transitively FILE_SET) and, on
// a match, file_ast(item.file). The file_ast dep is LOAD-BEARING twice over:
// NAMESPACE_ITEMS backdates on a body edit (membership fp), so without the
// file_ast dep this slot wouldn't re-verify — yet we must recompute to (a)
// re-read the refreshed item.ptr and (b) recompute the structural hash. The
// file_ast fp changes on any edit to the file, forcing exactly that; our own
// structural-hash fp then stays stable for unchanged decls so downstream
// cuts off.
//
// First match wins; a duplicate top-level name is reported separately by the
// CHECK driver (emit_redefinition_errors, 7.1a), not here.
// NOT_FOUND → empty / FINGERPRINT_NONE, re-verified via the index when a
// defining file/decl appears.
TopLevelEntry db_query_top_level_entry(db_query_ctx *ctx, NamespaceId nsid,
                                       StrId name) {
  struct db *s = (struct db *)ctx;
  TopLevelEntry empty = {0};
  uint64_t key = ((uint64_t)nsid.idx << 32) | (uint32_t)name.idx;
  db_query_slot_alloc(ctx, QUERY_TOP_LEVEL_ENTRY, key);
  DB_QUERY_GUARD(ctx, QUERY_TOP_LEVEL_ENTRY, key,
                 /* on_cached */ top_level_entry_read(s, key),
                 /* on_cycle  */ empty,
                 /* on_error  */ empty);

  FileArray items = db_query_namespace_items(ctx, nsid); // records dep
  const NamespaceItem *arr = (const NamespaceItem *)items.data;

  TopLevelEntry result = empty;
  Fingerprint fp = FINGERPRINT_NONE;
  for (uint32_t i = 0; i < items.count; i++) {
    if (arr[i].name.idx != name.idx)
      continue;
    // Resolve the item's ptr against the current tree (file_ast dep)
    // to get the CURRENT node_ptr + the trivia-excluding structural
    // hash — the per-name content firewall. (NAMESPACE_ITEMS no longer
    // carries struct_hash: its fp is membership-only, so the name layer
    // cuts off on body edits; the content firewall lives here.)
    struct GreenNode *root = db_query_file_ast(ctx, arr[i].file); // dep
    SyntaxNodePtr cur = arr[i].ptr;
    uint64_t sh = 0;
    if (root) {
      SyntaxTree *tree = syntax_tree_new(root);   // BORROWS root
      SyntaxNode *rroot = syntax_tree_root(tree); // +1
      SyntaxNode *node = syntax_node_ptr_resolve(arr[i].ptr, rroot);
      if (node) {
        cur = syntax_node_ptr_new(node);
        sh = green_structural_hash(syntax_node_green(node));
        syntax_node_release(node);
      }
      syntax_node_release(rroot);
      syntax_tree_free(tree);
    }
    result.id = arr[i].id;
    result.name = name;
    result.file = arr[i].file;
    result.node_ptr = cur;
    result.meta = arr[i].meta;
    fp = sh ? db_fp_u64(sh) : FINGERPRINT_NONE;
    break;
  }

  top_level_entry_write(s, key, result);
  db_query_succeed(ctx, QUERY_TOP_LEVEL_ENTRY, key, fp);
  return result;
}

// If `node` is an `@import("path")` builtin, intern its path (surrounding
// quotes stripped, mirroring the @import handler in sema/builtins.c) and
// anchor `out->site` to the builtin expr; return true. Any other node — a
// different builtin (@sizeOf, …), or @import without a string-literal arg
// (a malformed import; sema reports it) — returns false. Imports are
// EXCLUSIVELY `@import` builtins in the current grammar; the vestigial
// SK_IMPORT_DECL kind was removed, so there is no decl form to handle.
static bool import_site_of(struct db *s, SyntaxNode *node, FileImport *out) {
  if (syntax_node_kind(node) != SK_BUILTIN_EXPR)
    return false;
  BuiltinExpr be;
  if (!BuiltinExpr_cast(node, &be))
    return false;

  SyntaxToken *name_tok = BuiltinExpr_name(&be);
  bool is_import =
      name_tok && strcmp(syntax_token_text(name_tok), "import") == 0;
  if (name_tok)
    syntax_token_release(name_tok);
  if (!is_import)
    return false;

  SyntaxNode *args = BuiltinExpr_args(&be); // SK_ARG_LIST
  if (!args)
    return false;
  SyntaxNode *arg0 = syntax_node_first_child(args); // first arg expr
  syntax_node_release(args);
  if (!arg0)
    return false;

  bool ok = false;
  const char *txt;
  uint32_t len;
  if (ast_string_literal_text(arg0, &txt, &len)) { // quote-stripping shared
    out->path = pool_intern(&s->strings, txt, len);
    out->site = syntax_node_ptr_new(node);
    ok = true;
  }
  syntax_node_release(arg0);
  return ok;
}

// FILE_IMPORTS — every `@import("path")` site in the file. Depends on
// file_ast; walks the whole tree (imports can nest in any expression
// position) for the builtin sites. The result body is a STANDALONE malloc
// (FileImport[]) stored in files.imports — replaced wholesale here (free
// old → malloc new) and freed on evict (EVICT_FREE_FILEARRAY). NOT the
// per-file arena: line_index owns that arena and resets it on reparse,
// which would clobber a body sharing it.
//
// The fingerprint folds the path StrIds in DOCUMENT ORDER, so adding,
// removing, or reordering an import changes it (the import graph shifts),
// while an unrelated edit that leaves the import set intact keeps it
// stable — the early-cutoff firewall for a future module-graph consumer.
FileArray db_query_file_imports(db_query_ctx *ctx, FileId fid) {
  struct db *s = (struct db *)ctx;
  FileArray empty = {0};
  DB_QUERY_GUARD(ctx, QUERY_FILE_IMPORTS, (uint64_t)fid.idx,
                 /* on_cached */ file_imports_read(s, fid),
                 /* on_cycle  */ empty,
                 /* on_error  */ empty);

  struct GreenNode *root = db_query_file_ast(ctx, fid); // records dep

  Vec found;
  vec_init(&found, sizeof(FileImport));
  Fingerprint fp = FINGERPRINT_NONE;

  if (root) {
    SyntaxTree *tree = syntax_tree_new(root);   // BORROWS root
    SyntaxNode *rroot = syntax_tree_root(tree); // +1
    SyntaxDescendants it;
    syntax_descendants_init(&it, rroot);
    for (SyntaxNode *n; (n = syntax_descendants_next(&it));) {
      FileImport imp;
      if (import_site_of(s, n, &imp)) {
        vec_push(&found, &imp);
        fp = db_fp_combine(fp, db_fp_u64((uint64_t)imp.path.idx));
      }
      syntax_node_release(n);
    }
    syntax_descendants_free(&it);
    syntax_node_release(rroot);
    syntax_tree_free(tree);
  }

  // Replace the previous body wholesale: free the old malloc, install a
  // fresh exact-sized one (or {NULL,0} for a file with no imports).
  FileArray old = file_imports_read(s, fid);
  free(old.data);

  FileArray result = {.data = NULL, .count = (uint32_t)found.count};
  if (found.count) {
    size_t bytes = (size_t)found.count * sizeof(FileImport);
    result.data = malloc(bytes);
    memcpy(result.data, found.data, bytes);
  }
  vec_free(&found);

  file_imports_write(s, fid, result);
  db_query_succeed(ctx, QUERY_FILE_IMPORTS, (uint64_t)fid.idx, fp);
  return result;
}
