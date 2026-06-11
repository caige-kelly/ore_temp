// D3.0 gate — the node-type router (db_query_node_type) + db_node_enclosing_def.
// Consumer-facing "type at this syntax node", used by hover + completion.
//   1. db_node_enclosing_def: a body node → its enclosing top-level fn DefId.
//   2. db_query_node_type: param (signature range), body local + ref (infer_body
//      range), a top-level-const value ref (type_of_def value_node_types range),
//      a member-access receiver. The router SELF-ENSURES the per-decl queries.
// KEEP_ZONE, ASan.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/intern_pool/intern_pool.h"
#include "../src/db/query/engine.h"
#include "../src/db/workspace/workspace.h"
#include "../src/syntax/syntax.h"
#include "../src/syntax/syntax_kind.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern IpIndex   db_query_node_type(db_query_ctx *, FileId, SyntaxNode *);
extern DefId     db_node_enclosing_def(db_query_ctx *, FileId, SyntaxNode *);
extern FileArray db_query_namespace_items(db_query_ctx *, NamespaceId);
extern DefId     db_query_def_identity(db_query_ctx *, NamespaceId, AstId);
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
// n-th (0-based, preorder) descendant of `node` with `kind`; +1 ref, caller frees.
static SyntaxNode *find_nth_kind(SyntaxNode *node, SyntaxKind kind, int *rem) {
    uint32_t n = syntax_node_num_children(node);
    for (uint32_t i = 0; i < n; i++) {
        SyntaxElement el = syntax_node_child_or_token(node, i);
        if (el.kind == SYNTAX_ELEM_NODE && el.node) {
            if (syntax_node_kind(el.node) == kind) {
                if (*rem == 0) return el.node;
                (*rem)--;
            }
            SyntaxNode *f = find_nth_kind(el.node, kind, rem);
            syntax_node_release(el.node);
            if (f) return f;
        } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
            syntax_token_release(el.token);
        }
    }
    return NULL;
}
static SyntaxNode *nth_kind(SyntaxNode *root, SyntaxKind k, int n) {
    int rem = n;
    return find_nth_kind(root, k, &rem);
}

static const char *BASE =
    "x :: 7\n"
    "y :: x\n"
    "g :: fn(a: i32) -> i32\n"
    "    s := a\n"
    "    return s\n";

int main(void) {
    struct db s;
    db_init(&s);
    FileId fid = open_file(&s, "/n.ore", BASE);
    NamespaceId ns = db_get_file_namespace(&s, fid);

    db_request_begin(&s, db_current_revision(&s));

    struct GreenNode *groot = db_query_file_ast(&s, fid);
    SyntaxTree *tree = syntax_tree_new(groot);
    SyntaxNode *root = syntax_tree_root(tree);

    // REF preorder: #0 = `x` in `y :: x`; #1 = `a` in `s := a`; #2 = `s` in `return s`.
    SyntaxNode *ref_x = nth_kind(root, SK_REF_EXPR, 0);
    SyntaxNode *ref_a = nth_kind(root, SK_REF_EXPR, 1);
    SyntaxNode *ref_s = nth_kind(root, SK_REF_EXPR, 2);
    SyntaxNode *param = nth_kind(root, SK_PARAM, 0);
    assert(ref_x && ref_a && ref_s && param && "found the test nodes");

    // (1) enclosing def: the param `a` is enclosed by fn `g`; the ref `x` (in a
    //     top-level const) is enclosed by `y`.
    assert(db_node_enclosing_def(&s, fid, param).idx == def_of(&s, ns, "g").idx &&
           "param `a` → enclosing def g");
    assert(db_node_enclosing_def(&s, fid, ref_x).idx == def_of(&s, ns, "y").idx &&
           "ref `x` (in `y :: x`) → enclosing def y");

    // (2) node types: param + body refs resolve to i32; the const-value ref `x`
    //     resolves to comptime_int (type_of_def value_node_types range).
    assert(ip_index_eq(db_query_node_type(&s, fid, param), IP_I32_TYPE) &&
           "param `a` → i32 (signature range)");
    assert(ip_index_eq(db_query_node_type(&s, fid, ref_a), IP_I32_TYPE) &&
           "body ref `a` → i32 (infer_body range)");
    assert(ip_index_eq(db_query_node_type(&s, fid, ref_s), IP_I32_TYPE) &&
           "body local `s` → i32 (infer_body range)");
    assert(ip_index_eq(db_query_node_type(&s, fid, ref_x), IP_COMPTIME_INT_TYPE) &&
           "const-value ref `x` → comptime_int (type_of_def range)");

    syntax_node_release(ref_x);
    syntax_node_release(ref_a);
    syntax_node_release(ref_s);
    syntax_node_release(param);
    syntax_node_release(root);
    syntax_tree_free(tree);
    db_request_end(&s);

    db_free(&s);
    printf("PASS node_type: enclosing-def of a node; type-at-node for param "
           "(sig), body ref/local (infer_body), const-value ref (type_of_def)\n");
    return 0;
}
