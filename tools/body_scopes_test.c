// D2.3 gate — body_scopes (structural, RA ExprScopes-aligned).
//   1. scope tree + bindings: params in the root scope; a let nested under an
//      `if` lands in a descendant scope; binds carry a bind_site, NOT a type.
//   2. db_body_scope_lookup resolves a use → the nearest binding's bind_site.
//   3. fp is POSITION-INDEPENDENT structural: stable across a pure value edit
//      (`x := a`→`x := 7`), flips on renaming a local or adding one, and a
//      sibling top-level edit cuts off (firewall via top_level_entry).
// KEEP_ZONE, ASan.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/engine.h"
#include "../src/db/workspace/workspace.h"
#include "../src/syntax/syntax.h"
#include "../src/syntax/syntax_kind.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern FileArray     db_query_namespace_items(db_query_ctx *, NamespaceId);
extern DefId         db_query_def_identity(db_query_ctx *, NamespaceId, AstId);
extern const FnBody *db_query_body_scopes(db_query_ctx *, DefId);
extern SyntaxNodePtr db_body_scope_lookup(db_query_ctx *, DefId, SyntaxNode *, StrId);
extern struct GreenNode *db_query_file_ast(db_query_ctx *, FileId);

static FileId open_file(struct db *s, const char *path, const char *text) {
    SourceId src = workspace_did_open(s, path, strlen(path), text, strlen(text));
    return db_lookup_file_by_source(s, src);
}
static StrId intern(struct db *s, const char *str) {
    return pool_intern(&s->strings, str, strlen(str));
}
static DefId def_of(struct db *s, NamespaceId ns, const char *nm) {
    StrId name = intern(s, nm);
    FileArray items = db_query_namespace_items(s, ns);
    const NamespaceItem *a = (const NamespaceItem *)items.data;
    for (uint32_t i = 0; i < items.count; i++)
        if (a[i].name.idx == name.idx) return db_query_def_identity(s, ns, a[i].id);
    return DEF_ID_NONE;
}
// First scope holding a bind named `nm`, BODY_SCOPE_NONE if absent.
static uint32_t bind_scope(struct db *s, FnBody fb, const char *nm) {
    StrId name = intern(s, nm);
    const ScopedBind *binds = (const ScopedBind *)s->body_scope_binds.data;
    for (uint32_t i = 0; i < fb.bind_len; i++)
        if (binds[fb.bind_off + i].name.idx == name.idx)
            return binds[fb.bind_off + i].scope_id;
    return BODY_SCOPE_NONE;
}
static bool is_ancestor(struct db *s, FnBody fb, uint32_t anc, uint32_t desc) {
    const ScopeRow *rows = (const ScopeRow *)s->body_scope_rows.data;
    for (uint32_t sc = desc; sc != BODY_SCOPE_NONE; ) {
        if (sc == anc) return true;
        if (sc >= fb.scope_len) return false;
        sc = rows[fb.scope_off + sc].parent;
    }
    return false;
}
static SyntaxNode *find_first_kind(SyntaxNode *node, SyntaxKind kind) {
    uint32_t n = syntax_node_num_children(node);
    for (uint32_t i = 0; i < n; i++) {
        SyntaxElement el = syntax_node_child_or_token(node, i);
        if (el.kind == SYNTAX_ELEM_NODE && el.node) {
            if (syntax_node_kind(el.node) == kind) return el.node;  // +1; caller frees
            SyntaxNode *f = find_first_kind(el.node, kind);
            syntax_node_release(el.node);
            if (f) return f;
        } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
            syntax_token_release(el.token);
        }
    }
    return NULL;
}

static const char *BASE =
    "f :: fn(a: i32) i32\n"
    "    x := a\n"
    "    if (a)\n"
    "        y := x\n"
    "    return x\n";

int main(void) {
    struct db s;
    db_init(&s);
    FileId fid = open_file(&s, "/f.ore", BASE);
    NamespaceId ns = db_get_file_namespace(&s, fid);
    SourceId sid = db_get_file_source(&s, fid);

    // (1) Structure.
    db_request_begin(&s, db_current_revision(&s));
    DefId f = def_of(&s, ns, "f");
    const FnBody *fbp = db_query_body_scopes(&s, f);
    assert(fbp && "body_scopes returns a FnBody for the fn");
    FnBody fb = *fbp;
    assert(fb.scope_len >= 2 && "root + at least one nested scope");
    assert(fb.bind_len == 3 && "binds: param a, locals x + y");

    uint32_t sa = bind_scope(&s, fb, "a");
    uint32_t sx = bind_scope(&s, fb, "x");
    uint32_t sy = bind_scope(&s, fb, "y");
    assert(sa != BODY_SCOPE_NONE && sx != BODY_SCOPE_NONE && sy != BODY_SCOPE_NONE);
    const ScopeRow *rows = (const ScopeRow *)s.body_scope_rows.data;
    assert(rows[fb.scope_off + sa].parent == BODY_SCOPE_NONE && "param a in the root scope");
    assert(sx != sy && is_ancestor(&s, fb, sx, sy) &&
           "y (under the if) is nested in a descendant of x's scope");

    // (2) Lookup: a use of `a` resolves to the param's bind_site (SK_PARAM).
    struct GreenNode *groot = db_query_file_ast(&s, fid);
    SyntaxTree *tree = syntax_tree_new(groot);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxNode *use = find_first_kind(root, SK_REF_EXPR);   // the `a` in `x := a`
    assert(use && "found a REF_EXPR use site");
    SyntaxNodePtr bind = db_body_scope_lookup(&s, f, use, intern(&s, "a"));
    assert(bind.kind == SK_PARAM && "use of `a` resolves to the param binding");
    SyntaxNodePtr miss = db_body_scope_lookup(&s, f, use, intern(&s, "nope"));
    assert(miss.kind == SYNTAX_KIND_NONE && "unknown name → no binding");
    syntax_node_release(use);
    syntax_node_release(root);
    syntax_tree_free(tree);

    Fingerprint fp_base = db_slot_fingerprint(&s, QUERY_BODY_SCOPES, (uint64_t)f.idx);
    db_request_end(&s);

    // (3a) Pure value edit (x := a → x := 7) — structure unchanged → fp STABLE.
    const char *VAL =
        "f :: fn(a: i32) i32\n    x := 7\n    if (a)\n        y := x\n    return x\n";
    assert(db_set_source_text(&s, sid, VAL, strlen(VAL)));
    db_request_begin(&s, db_current_revision(&s));
    (void)db_query_body_scopes(&s, def_of(&s, ns, "f"));
    Fingerprint fp_val = db_slot_fingerprint(&s, QUERY_BODY_SCOPES, (uint64_t)f.idx);
    assert(fp_val == fp_base && "value edit leaves the structural fp STABLE");
    db_request_end(&s);

    // (3b) Rename a local (y → z) — binding set changes → fp FLIPS.
    const char *REN =
        "f :: fn(a: i32) i32\n    x := 7\n    if (a)\n        z := x\n    return x\n";
    assert(db_set_source_text(&s, sid, REN, strlen(REN)));
    db_request_begin(&s, db_current_revision(&s));
    (void)db_query_body_scopes(&s, def_of(&s, ns, "f"));
    Fingerprint fp_ren = db_slot_fingerprint(&s, QUERY_BODY_SCOPES, (uint64_t)f.idx);
    assert(fp_ren != fp_base && "renaming a local flips the structural fp");
    db_request_end(&s);

    // (3c) Back to base structure — fp returns to fp_base.
    assert(db_set_source_text(&s, sid, BASE, strlen(BASE)));
    db_request_begin(&s, db_current_revision(&s));
    (void)db_query_body_scopes(&s, def_of(&s, ns, "f"));
    Fingerprint fp_b2 = db_slot_fingerprint(&s, QUERY_BODY_SCOPES, (uint64_t)f.idx);
    assert(fp_b2 == fp_base && "same structure → same fp");
    db_request_end(&s);

    // (3d) Sibling top-level edit — f's body_scopes is firewalled (cuts off).
    const char *SIB =
        "f :: fn(a: i32) i32\n    x := a\n    if (a)\n        y := x\n    return x\n"
        "g :: 5\n";
    assert(db_set_source_text(&s, sid, SIB, strlen(SIB)));
    db_request_begin(&s, db_current_revision(&s));
    (void)db_query_body_scopes(&s, def_of(&s, ns, "f"));
    Fingerprint fp_sib = db_slot_fingerprint(&s, QUERY_BODY_SCOPES, (uint64_t)f.idx);
    assert(fp_sib == fp_base && "a sibling decl does not perturb f's body_scopes fp");
    db_request_end(&s);

    db_free(&s);
    printf("PASS body_scopes: scope tree + bind_site bindings (no types); lookup "
           "resolves to bind_site; structural fp stable-on-value, flips-on-rename, "
           "sibling-firewalled\n");
    return 0;
}
