// Go-to-definition — resolve the cursor identifier to its definition's
// source location. Returns IdeLocation by reference; the LSP layer
// formats the result into a textDocument/definition response.
//
// Three resolution paths in priority order:
//   1. body-local — params + let-bound names walk the body's scope
//      chain via db_body_scope_lookup. Body-first so a local correctly
//      wins over a same-named top-level.
//   2. top-level — db_query_top_level_entry in the file's namespace.
//   3. cross-namespace — for `B.x` field access where B is an @import'd
//      namespace_type, resolve the member in the imported namespace.
//
// All three end at a SyntaxNodePtr in some file; the converter
// (locate_node_ptr) turns that into an IdeLocation via db_resolve_span.

#include "ide.h"

#include "../ast/ast_expr.h"
#include "../db/db.h"
#include "../db/diag/diag.h"
#include "../db/ids/ids.h"
#include "../db/intern_pool/intern_pool.h"
#include "../support/data_structure/stringpool.h"
#include "../syntax/syntax.h"
#include "../syntax/syntax_kind.h"

#include <stdbool.h>

// Externs for query entry points — db.h doesn't re-declare these per
// the no-per-query-header convention; matches hover.c's pattern.
extern DefId         db_node_enclosing_def(db_query_ctx *ctx, FileId fid,
                                           SyntaxNode *node);
extern IpIndex       db_query_node_type(db_query_ctx *ctx, FileId fid,
                                        SyntaxNode *node);
extern TopLevelEntry db_query_top_level_entry(db_query_ctx *ctx,
                                              NamespaceId nsid, StrId name);
extern SyntaxNodePtr db_body_scope_lookup(db_query_ctx *ctx, DefId fn_def,
                                          SyntaxNode *use_node, StrId name);
extern FileArray     db_query_line_index(db_query_ctx *ctx, FileId fid);

// Intern a SyntaxToken's text. {0} on NULL.
static StrId intern_tok(struct db *s, SyntaxToken *t) {
    if (!t)
        return (StrId){0};
    return pool_intern(&s->strings, syntax_token_text(t),
                       syntax_token_text_range(t).length);
}

// Convert (file, node_ptr) → IdeLocation by resolving the byte range
// to (line, col). Returns false if the span can't be resolved (file
// has no line_index yet, or the source was evicted).
static bool locate_node_ptr(struct db *db, FileId file, SyntaxNodePtr ptr,
                            IdeLocation *out) {
    if (!file_id_valid(file) || ptr.kind == SYNTAX_KIND_NONE)
        return false;
    TinySpan span = span_make((uint16_t)file_id_local(file), ptr.range.start,
                              ptr.range.length);
    ResolvedSpan rs;
    if (!db_resolve_span(db, span, &rs))
        return false;
    out->file = file;
    // db_resolve_span returns 1-indexed line + col; LSP wants 0-indexed.
    out->line_start = rs.line > 0 ? rs.line - 1 : 0;
    out->col_start = rs.col_start > 0 ? rs.col_start - 1 : 0;
    out->line_end = out->line_start; // single-line spans for top-level / bind sites
    out->col_end = rs.col_end > 0 ? rs.col_end - 1 : out->col_start;
    return true;
}

// Resolve a top-level name in a namespace to its (file, node_ptr) and
// fill out. Returns true on hit. Ensures the resolved file's line_index
// is populated before locating — compile_file's typecheck only builds
// line_index for the current namespace's files (D3.2C's tail loop), so
// a cross-namespace jump needs to populate the target file's index
// itself or db_resolve_span returns false on the bind-site span.
static bool resolve_top_level(db_query_ctx *ctx, struct db *db,
                              NamespaceId nsid, StrId name, IdeLocation *out) {
    TopLevelEntry e = db_query_top_level_entry(ctx, nsid, name);
    if (!file_id_valid(e.file))
        return false;
    (void)db_query_line_index(ctx, e.file);
    return locate_node_ptr(db, e.file, e.node_ptr, out);
}

bool ide_definition_at(struct db *db, FileId fid, uint32_t line0,
                       uint32_t char0, IdeLocation *out) {
    if (!out || !file_id_valid(fid))
        return false;

    uint32_t off = db_byte_offset_at(db, fid, line0, char0);
    if (off == UINT32_MAX)
        return false;
    SyntaxNode *node = db_node_at_offset(db, fid, off);
    if (!node)
        return false;

    bool ok = false;
    db_request_begin(db, db_current_revision(db));

    switch (syntax_node_kind(node)) {
    case SK_REF_EXPR: {
        RefExpr r;
        if (!RefExpr_cast(node, &r))
            break;
        SyntaxToken *nt = RefExpr_name(&r);
        StrId name = intern_tok(db, nt);
        if (nt)
            syntax_token_release(nt);
        if (name.idx == 0)
            break;

        // Body-first: if the cursor is inside a fn body, try the body
        // scope chain. db_body_scope_lookup returns the bind-site
        // SyntaxNodePtr in the SAME file as the use site (locals can't
        // be defined elsewhere). On miss, fall through to top-level.
        DefId enc = db_node_enclosing_def(db, fid, node);
        if (enc.idx != 0 && db_def_kind(db, enc) == KIND_FUNCTION) {
            SyntaxNodePtr bind =
                db_body_scope_lookup(db, enc, node, name);
            if (bind.kind != SYNTAX_KIND_NONE) {
                ok = locate_node_ptr(db, fid, bind, out);
                if (ok)
                    break;
            }
        }

        // Top-level — look up in the file's namespace.
        NamespaceId nsid = db_get_file_namespace(db, fid);
        if (namespace_id_valid(nsid))
            ok = resolve_top_level(db, db, nsid, name, out);
        break;
    }

    case SK_FIELD_EXPR: {
        FieldExpr fe;
        if (!FieldExpr_cast(node, &fe))
            break;
        SyntaxNode *base = FieldExpr_base(&fe);
        SyntaxToken *ft = FieldExpr_field(&fe);
        StrId member = intern_tok(db, ft);
        if (ft)
            syntax_token_release(ft);
        if (!base || member.idx == 0) {
            if (base)
                syntax_node_release(base);
            break;
        }

        IpIndex bt = db_query_node_type(db, fid, base);
        syntax_node_release(base);
        if (bt.v != IP_NONE.v &&
            ip_tag(&db->intern, bt) == IP_TAG_NAMESPACE_TYPE) {
            NamespaceId target = ip_key(&db->intern, bt).namespace_type.nsid;
            if (namespace_id_valid(target))
                ok = resolve_top_level(db, db, target, member, out);
        }
        // Aggregate field (struct/enum) location is deferred — needs
        // field-decl SyntaxNodePtr storage that the D2.1b/D2.2 layout
        // doesn't carry.
        break;
    }

    default:
        break;
    }

    db_request_end(db);
    syntax_node_release(node);
    return ok;
}
