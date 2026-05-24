// Hover — type description for the cursor position.
//
// Architecture: pure read of typecheck state. Reads `node_data.types[]`
// populated by sema_type_of_expr's wrapper during the immediately-
// preceding compile_file run (oredb_typecheck). Does NOT call
// sema_type_of_expr — that would emit diagnostics through the
// centralized diag pipeline, which asserts on an active query frame.
//
// Wraps the body in db_request_begin/end because db_query_resolve_ref
// and db_query_type_of_def (called via resolve_path_for_hover) need a
// pinned effective_revision. The dependent slots were already
// populated by the prior typecheck so the queries cache-hit; the
// request boundary is for soundness, not work.

#include "ide.h"

#include "../db/db.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/resolve_ref.h"
#include "../db/query/type_of_def.h"
#include "../parser/ast.h"
#include "../sema/sema.h"

#include <stdio.h>
#include <string.h>

// Enclosing top-level DefId via the parent-chain walk stamped by
// module_exports. O(parent_depth).
static DefId enclosing_fn_for_node(struct db *s, FileId fid, AstNodeId node) {
    return db_get_def_for_node(s, fid, node);
}

// Resolve a name in path position. Body scope first (when we know the
// enclosing fn), then namespace internal scope. Returns IP_NONE on
// unresolved.
static IpIndex resolve_path_for_hover(struct db *s, NamespaceId nsid,
                                      DefId enclosing_fn, AstNodeId use_node,
                                      StrId name) {
    if (name.idx == 0)
        return IP_NONE;
    if (def_id_valid(enclosing_fn)) {
        IpIndex local = sema_body_scope_lookup(s, enclosing_fn, use_node, name);
        if (local.v != IP_NONE.v)
            return local;
    }
    ScopeId internal = db_get_namespace_internal_scope(s, nsid);
    if (internal.idx == SCOPE_ID_NONE.idx)
        return IP_NONE;
    DefId target = db_query_resolve_ref(s, internal, name);
    if (!def_id_valid(target))
        return IP_NONE;
    return db_query_type_of_def(s, target);
}

size_t ide_hover_at(struct db *db, FileId fid,
                    uint32_t line0, uint32_t char0,
                    char *buf, size_t buflen) {
    if (!buf || buflen == 0)
        return 0;
    buf[0] = '\0';

    if (!file_id_valid(fid))
        return 0;

    uint32_t off = db_byte_offset_at(db, fid, line0, char0);
    if (off == UINT32_MAX)
        return 0;
    AstNodeId node = db_node_at_offset(db, fid, off);
    if (node.idx == AST_NODE_ID_NONE.idx)
        return 0;

    NamespaceId nsid = db_get_file_namespace(db, fid);
    if (!namespace_id_valid(nsid))
        return 0;

    ASTStore *ast = db_get_file_ast(db, fid);
    if (!ast)
        return 0;

    AstNodeKind k = ((AstNodeKind *)ast->kinds.data)[node.idx];
    AstNodeData d = ((AstNodeData *)ast->data.data)[node.idx];

    db_request_begin(db, db_current_revision(db));
    DefId enclosing_fn = enclosing_fn_for_node(db, fid, node);

    IpIndex type = IP_NONE;
    const char *name_str = NULL;

    switch (k) {
    case AST_EXPR_PATH: {
        StrId name = d.string_id;
        type = resolve_path_for_hover(db, nsid, enclosing_fn, node, name);
        name_str = pool_get(&db->strings, name);
        break;
    }
    // Top-level decl names hover as the decl's type. The "cursor on
    // the name token" case lands on the AST_DECL_* node because the
    // name's span is part of the decl's span (innermost-containing
    // wins).
    case AST_DECL_CONST:
    case AST_DECL_VAR: {
        const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
        StrId name = {.idx = ex[0]};
        name_str = pool_get(&db->strings, name);
        if (def_id_valid(enclosing_fn))
            type = sema_body_scope_lookup(db, enclosing_fn, node, name);
        if (type.v == IP_NONE.v)
            type = resolve_path_for_hover(db, nsid, DEF_ID_NONE, node, name);
        break;
    }
    case AST_DECL_PARAM: {
        const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
        StrId name = {.idx = ex[0]};
        name_str = pool_get(&db->strings, name);
        if (def_id_valid(enclosing_fn))
            type = sema_body_scope_lookup(db, enclosing_fn, node, name);
        break;
    }
    default: {
        // Per-node type cache lookup. sema_type_of_expr's wrapper
        // populates types[node.idx] for every visited node during
        // typecheck.
        uint32_t fl = file_id_local(fid);
        if (fl < db->files.node_data.count &&
            fl < db->files.node_counts.count) {
            FileNodeData *nd = (FileNodeData *)vec_get(&db->files.node_data, fl);
            uint32_t nc = *(uint32_t *)vec_get(&db->files.node_counts, fl);
            if (nd && nd->types && node.idx < nc)
                type = nd->types[node.idx];
        }
        break;
    }
    }

    db_request_end(db);

    if (type.v == IP_NONE.v && (!name_str || !name_str[0]))
        return 0;

    char tbuf[256];
    db_format_type(db, type, tbuf, sizeof tbuf);

    int n;
    if (name_str && name_str[0])
        n = snprintf(buf, buflen, "%s: %s", name_str, tbuf);
    else
        n = snprintf(buf, buflen, "%s", tbuf);
    return n < 0 ? 0 : (size_t)n;
}
