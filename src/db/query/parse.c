// Parse layer (Phase C) — file_ast and the per-decl queries that read it.
//
// Model B (pull early-cutoff firewall): file_ast is the ONLY query that
// parses. It produces the file's green tree, persists line_starts for
// position/diag rendering, and drains lex+parse errors into the
// FILE_AST diagnostic unit. The per-decl queries (decl_ast,
// top_level_entry) and file_imports DEPEND on file_ast and derive their
// results from the green tree, each emitting a position-independent
// content-hash fingerprint so a sibling-decl edit re-runs only the cheap
// derivation, not the downstream that cache-hits on the stable fp.
//
// Pure-query contract (see engine.h): write the result column BEFORE
// db_query_succeed; never write outside the query's own column.

#define ORE_ENGINE_PRIVATE
#include "engine.h"
#include "engine_internal.h"
#include "result_columns.h"   // db.h, ids.h, syntax.h, intern_pool.h

#include "../diag/diag.h"     // db_emit, diag_anchor_make, DIAG_ERROR

#include "../../lexer/lexer.h"
#include "../../lexer/layout.h"
#include "../../lexer/token.h"
#include "../../parser_new/parser.h"
#include "../../syntax/node_cache.h"  // green_node_hash_of, green_structural_hash
#include "../../ast/ast_decl.h"       // FnDef_name / StructDef_name / … (+ SK_*)
#include "../../ast/ast_expr.h"       // BuiltinExpr / Literal (@import sites)

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
    DiagAnchor a = diag_anchor_make((uint16_t)file_local, t->kind, t->start,
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
    const char *src = db_query_file_text(ctx, sid);  // records SOURCE_TEXT dep
    uint32_t len = db_get_source_len(s, sid);

    if (!src) {
        line_index_write(s, fid, empty);
        db_query_succeed(ctx, QUERY_LINE_INDEX, (uint64_t)fid.idx,
                         FINGERPRINT_NONE);
        return empty;
    }

    // Scan into a growable scratch vec (one pass, correct \n / \r\n / \r
    // handling), then copy into an exact arena-backed array.
    Vec tmp;
    vec_init(&tmp, sizeof(uint32_t));
    uint32_t zero = 0;
    vec_push(&tmp, &zero);  // line 0 begins at offset 0
    for (uint32_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < len && src[i + 1] == '\n')
                i++;            // CRLF: the break is the pair
            uint32_t start = i + 1;  // next line begins past the break
            vec_push(&tmp, &start);
        }
    }

    Arena *fa = (Arena *)vec_get(&s->files.arenas, local);
    arena_reset(fa);
    size_t bytes = (size_t)tmp.count * sizeof(uint32_t);
    uint32_t *arr = (uint32_t *)arena_alloc_raw(fa, bytes);
    memcpy(arr, tmp.data, bytes);
    vec_free(&tmp);

    FileArray result = {.data = arr, .count = (uint32_t)(bytes / sizeof(uint32_t))};
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

    uint32_t local = file_id_local(fid);
    SourceId sid = db_get_file_source(s, fid);
    const char *src = db_query_file_text(ctx, sid);  // reads text + records input dep
    uint32_t len = db_get_source_len(s, sid);

    if (!src) {
        // Evicted / missing source — no tree this revision.
        struct GreenNode *old = file_ast_read(s, fid);
        file_ast_write(s, fid, NULL);
        if (old) green_node_release(old);
        db_query_succeed(ctx, QUERY_FILE_AST, (uint64_t)fid.idx,
                         FINGERPRINT_NONE);
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
    struct GreenNode *root = parse_file_green(&toks, src, s->node_cache, &errors);

    // Publish the green root, releasing the prior parse's root. parse
    // returns +1 ref (RETURNS_OWNED); we donate it to the column and drop
    // the column's previous ref. Hash-consed shared subtrees survive via
    // their own refcounts; releasing is correct even if root == old.
    struct GreenNode *old = file_ast_read(s, fid);
    file_ast_write(s, fid, root);
    if (old) green_node_release(old);

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

// DECL_AST — dep-tracked thin handle for one decl. Depends on file_ast,
// resolves the (caller-supplied, current-revision) ptr against the fresh
// green tree, and returns its current SyntaxNodePtr. The fingerprint is
// the decl subtree's POSITION-INDEPENDENT content hash, so a downstream
// query that depends on this slot cache-hits whenever the decl's content
// is unchanged — even if a sibling edit shifted its byte range (the
// early-cutoff firewall). Callers pass a current ptr (from the file walk
// / top_level_entry); a stale/deleted ptr resolves to NONE.
SyntaxNodePtr db_query_decl_ast(db_query_ctx *ctx, FileId fid,
                                SyntaxNodePtr ptr) {
    struct db *s = (struct db *)ctx;
    SyntaxNodePtr none = {0};
    uint64_t key = ((uint64_t)fid.idx << 32) |
                   (uint32_t)syntax_node_ptr_hash(ptr);
    db_query_slot_alloc(ctx, QUERY_DECL_AST, key);
    DB_QUERY_GUARD(ctx, QUERY_DECL_AST, key,
                   /* on_cached */ decl_ast_read(s, key),
                   /* on_cycle  */ none,
                   /* on_error  */ none);

    struct GreenNode *root = db_query_file_ast(ctx, fid);  // records dep
    SyntaxNodePtr result = none;
    Fingerprint fp = FINGERPRINT_NONE;
    if (root) {
        SyntaxTree *tree = syntax_tree_new(root);          // BORROWS root
        SyntaxNode *rroot = syntax_tree_root(tree);        // +1
        SyntaxNode *node = syntax_node_ptr_resolve(ptr, rroot);  // +1 or NULL
        if (node) {
            result = syntax_node_ptr_new(node);
            // Trivia-EXCLUDING structural hash: a whitespace/comment edit
            // reparses file_ast (so we re-run + refresh the position-
            // dependent ptr above), but this fp stays stable, so our
            // downstream (type_of_def, ...) cache-hits — contract C3.
            fp = db_fp_u64(green_structural_hash(syntax_node_green(node)));
            syntax_node_release(node);
        }
        syntax_node_release(rroot);
        syntax_tree_free(tree);
    }

    decl_ast_write(s, key, result);
    db_query_succeed(ctx, QUERY_DECL_AST, key, fp);
    return result;
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
                   /* on_cached */ db_slot_fingerprint(ctx, QUERY_FILE_SET,
                                                       (uint64_t)nsid.idx),
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
static StrId decl_name_of(struct db *s, SyntaxNode *node) {
    SyntaxToken *tok = NULL;
    switch ((OreSyntaxKind)syntax_node_kind(node)) {
        case SK_FN_DECL:     { FnDef d;     if (FnDef_cast(node, &d))     tok = FnDef_name(&d);     break; }
        case SK_STRUCT_DECL: { StructDef d; if (StructDef_cast(node, &d)) tok = StructDef_name(&d); break; }
        case SK_ENUM_DECL:   { EnumDef d;   if (EnumDef_cast(node, &d))   tok = EnumDef_name(&d);   break; }
        case SK_UNION_DECL:  { UnionDef d;  if (UnionDef_cast(node, &d))  tok = UnionDef_name(&d);  break; }
        case SK_EFFECT_DECL: { EffectDef d; if (EffectDef_cast(node, &d)) tok = EffectDef_name(&d); break; }
        case SK_CONST_DECL:  { ConstDef d;  if (ConstDef_cast(node, &d))  tok = ConstDef_name(&d);  break; }
        case SK_VAR_DECL:    { VarDef d;    if (VarDef_cast(node, &d))    tok = VarDef_name(&d);    break; }
        default: return STR_ID_NONE;
    }
    if (!tok)
        return STR_ID_NONE;
    TextRange r = syntax_token_text_range(tok);
    StrId id = pool_intern(&s->strings, syntax_token_text(tok), r.length);
    syntax_token_release(tok);
    return id;
}

// TOP_LEVEL_ENTRY — the per-name firewall. Resolves `name` to the
// top-level decl that defines it within namespace `nsid`, returning a
// reparse-stable handle (node_ptr) plus a POSITION-INDEPENDENT content
// fingerprint. Downstream name/type queries depend on THIS slot, so a
// sibling-decl edit (which reparses file_ast and shifts byte ranges) does
// not invalidate them: the structural-hash fp is unchanged, so they
// cache-hit. Deps recorded: FILE_SET(nsid) — membership change re-runs
// the lookup — and file_ast(file) for every file scanned, so an edit to a
// scanned file re-runs the lookup against the fresh tree.
//
// First match wins; a duplicate top-level name is a sema error reported in
// Phase D, not resolved here. NOT_FOUND yields an empty entry +
// FINGERPRINT_NONE (still a cached result — re-verified when FILE_SET or a
// scanned file's ast changes, which is exactly when a name could appear).
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

    // Record the file-set dep BEFORE the scan: if a file defining `name`
    // is added later, the slot re-verifies and re-runs this scan (the
    // case a coarse tier bump alone would miss — it would cache-hit the
    // old file set's file_ast deps and never see the new file).
    (void)db_query_namespace_file_set(ctx, nsid);

    uint32_t nfiles = 0;
    const FileId *files = db_get_namespace_files(s, nsid, &nfiles);

    TopLevelEntry result = empty;
    Fingerprint fp = FINGERPRINT_NONE;
    bool found = false;

    for (uint32_t fi = 0; fi < nfiles && !found; fi++) {
        struct GreenNode *root = db_query_file_ast(ctx, files[fi]); // records dep
        if (!root)
            continue;
        SyntaxTree *tree = syntax_tree_new(root);     // BORROWS root
        SyntaxNode *rroot = syntax_tree_root(tree);   // +1
        SyntaxChildren it;
        syntax_children_init(&it, rroot, SYNTAX_DIR_NEXT);
        for (SyntaxNode *child; (child = syntax_children_next(&it)); ) {
            StrId dname = decl_name_of(s, child);
            bool match = dname.idx != 0 && dname.idx == name.idx;
            if (match) {
                result.name     = name;
                result.node_ptr = syntax_node_ptr_new(child);
                result.meta     = 0;  // TODO(phase-D): populate visibility
                                      // (pub modifier) when module_exports
                                      // / unused analysis consumes it.
                fp = db_fp_u64(green_structural_hash(syntax_node_green(child)));
                found = true;
            }
            syntax_node_release(child);
            if (match)
                break;
        }
        syntax_children_free(&it);
        syntax_node_release(rroot);
        syntax_tree_free(tree);
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

    SyntaxNode *args = BuiltinExpr_args(&be);          // SK_ARG_LIST
    if (!args)
        return false;
    SyntaxNode *arg0 = syntax_node_first_child(args);  // first arg expr
    syntax_node_release(args);
    if (!arg0)
        return false;

    bool ok = false;
    Literal lit;
    if (Literal_cast(arg0, &lit) && Literal_kind(&lit) == SK_STRING_LIT) {
        SyntaxToken *tok = Literal_token(&lit);
        if (tok) {
            const char *txt = syntax_token_text(tok);
            uint32_t len = syntax_token_text_range(tok).length;
            StrId path =
                (len >= 2 && txt[0] == '"' && txt[len - 1] == '"')
                    ? pool_intern(&s->strings, txt + 1, len - 2)
                    : pool_intern(&s->strings, txt, len);
            syntax_token_release(tok);
            out->path = path;
            out->site = syntax_node_ptr_new(node);
            ok = true;
        }
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

    struct GreenNode *root = db_query_file_ast(ctx, fid);  // records dep

    Vec found;
    vec_init(&found, sizeof(FileImport));
    Fingerprint fp = FINGERPRINT_NONE;

    if (root) {
        SyntaxTree *tree = syntax_tree_new(root);     // BORROWS root
        SyntaxNode *rroot = syntax_tree_root(tree);   // +1
        SyntaxDescendants it;
        syntax_descendants_init(&it, rroot);
        for (SyntaxNode *n; (n = syntax_descendants_next(&it)); ) {
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
