// D2.4 gate — body inference (infer_body + type_of_expr/check_expr).
//   1. inferred top-level bind `x :: 42` → type_of_def is comptime_int.
//   2. infer_body types the body: a param ref and a local let-bind ref both
//      resolve to their types via db_body_scope_lookup → bind_site → node-map
//      (the D2.4 local-type rewiring; body_scopes is structural).
//   3. infer_body's fp flips on a body edit and cuts off on a sibling edit
//      (content firewall via top_level_entry).
// KEEP_ZONE, ASan.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/intern_pool/intern_pool.h"
#include "../src/db/query/engine.h"
#include "../src/db/workspace/workspace.h"
#include "../src/syntax/syntax.h"
#include "../src/syntax/syntax_kind.h"
#include "../src/ast/ast_expr.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern FileArray     db_query_namespace_items(db_query_ctx *, NamespaceId);
extern DefId         db_query_def_identity(db_query_ctx *, NamespaceId, AstId);
extern IpIndex       db_query_type_of_def(db_query_ctx *, DefId);
extern NodeTypesRange db_query_infer_body(db_query_ctx *, DefId);
extern struct GreenNode *db_query_file_ast(db_query_ctx *, FileId);
extern IpIndex       node_types_range_lookup(struct db *, NodeTypesRange, SyntaxNode *);

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
// The n-th (0-based, preorder) descendant of `node` with kind `kind`; +1 ref.
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
static SyntaxNode *find_first_kind(SyntaxNode *n, SyntaxKind k) {
    int rem = 0;
    return find_nth_kind(n, k, &rem);
}

// infer_body(fn), then look up the type of the n-th `kind` node in the fn's
// lambda body. (find_first_kind(root, LAMBDA) gets the fn's own lambda; the
// search is within its body — so a nested lambda is found by searching the
// outer body for SK_LAMBDA_EXPR.)
static IpIndex type_of_body_node(struct db *s, NamespaceId ns, FileId fid,
                                 const char *fnname, SyntaxKind kind, int nth) {
    NodeTypesRange range = db_query_infer_body(s, def_of(s, ns, fnname));
    struct GreenNode *groot = db_query_file_ast(s, fid);
    SyntaxTree *tree = syntax_tree_new(groot);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxNode *lambda = find_first_kind(root, SK_LAMBDA_EXPR);
    IpIndex t = IP_NONE;
    if (lambda) {
        LambdaExpr lam;
        if (LambdaExpr_cast(lambda, &lam)) {
            SyntaxNode *body = LambdaExpr_body(&lam);
            if (body) {
                int rem = nth;
                SyntaxNode *target = find_nth_kind(body, kind, &rem);
                if (target) { t = node_types_range_lookup(s, range, target); syntax_node_release(target); }
                syntax_node_release(body);
            }
        }
        syntax_node_release(lambda);
    }
    syntax_node_release(root);
    syntax_tree_free(tree);
    return t;
}

static const char *BASE =
    "x :: 42\n"
    "g :: fn(a: i32) i32\n"
    "    s := a\n"
    "    return s\n";

int main(void) {
    struct db s;
    db_init(&s);
    FileId fid = open_file(&s, "/m.ore", BASE);
    NamespaceId ns = db_get_file_namespace(&s, fid);
    SourceId sid = db_get_file_source(&s, fid);

    db_request_begin(&s, db_current_revision(&s));

    // (1) inferred top-level bind `x :: 42` → comptime_int.
    DefId x = def_of(&s, ns, "x");
    assert(ip_index_eq(db_query_type_of_def(&s, x), IP_COMPTIME_INT_TYPE) &&
           "inferred bind x :: 42 → comptime_int");

    // (2) infer_body(g): the body's refs resolve to types.
    DefId g = def_of(&s, ns, "g");
    NodeTypesRange range = db_query_infer_body(&s, g);

    struct GreenNode *groot = db_query_file_ast(&s, fid);
    SyntaxTree *tree = syntax_tree_new(groot);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxNode *lambda = find_first_kind(root, SK_LAMBDA_EXPR);
    assert(lambda && "found g's lambda");
    LambdaExpr lam;
    assert(LambdaExpr_cast(lambda, &lam));
    SyntaxNode *body = LambdaExpr_body(&lam);
    assert(body && "lambda has a body");

    int r0 = 0; SyntaxNode *use_a = find_nth_kind(body, SK_REF_EXPR, &r0);  // `a` in `s := a`
    int r1 = 1; SyntaxNode *use_s = find_nth_kind(body, SK_REF_EXPR, &r1);  // `s` in `return s`
    assert(use_a && use_s && "found the two body REF uses");
    assert(ip_index_eq(node_types_range_lookup(&s, range, use_a), IP_I32_TYPE) &&
           "param ref `a` resolves to i32 (bind_site → node-map)");
    assert(ip_index_eq(node_types_range_lookup(&s, range, use_s), IP_I32_TYPE) &&
           "local let `s` (= a) resolves to i32");
    syntax_node_release(use_a);
    syntax_node_release(use_s);
    syntax_node_release(body);
    syntax_node_release(lambda);
    syntax_node_release(root);
    syntax_tree_free(tree);

    Fingerprint fp_base = db_slot_fingerprint(&s, QUERY_INFER_BODY, (uint64_t)g.idx);
    db_request_end(&s);

    // (3a) body edit (s := a → s := a + a) → infer_body fp FLIPS.
    const char *E2 = "x :: 42\ng :: fn(a: i32) i32\n    s := a + a\n    return s\n";
    assert(db_set_source_text(&s, sid, E2, strlen(E2)));
    db_request_begin(&s, db_current_revision(&s));
    (void)db_query_infer_body(&s, def_of(&s, ns, "g"));
    Fingerprint fp2 = db_slot_fingerprint(&s, QUERY_INFER_BODY, (uint64_t)g.idx);
    assert(fp2 != fp_base && "body edit flips infer_body fp");
    db_request_end(&s);

    // (3b) back to base → fp returns.
    assert(db_set_source_text(&s, sid, BASE, strlen(BASE)));
    db_request_begin(&s, db_current_revision(&s));
    (void)db_query_infer_body(&s, def_of(&s, ns, "g"));
    Fingerprint fp_b2 = db_slot_fingerprint(&s, QUERY_INFER_BODY, (uint64_t)g.idx);
    assert(fp_b2 == fp_base && "same body → same fp");
    db_request_end(&s);

    // (3c) sibling top-level edit → g's infer_body cuts off (fp stable).
    const char *E3 = "x :: 42\ng :: fn(a: i32) i32\n    s := a\n    return s\ny :: 7\n";
    assert(db_set_source_text(&s, sid, E3, strlen(E3)));
    db_request_begin(&s, db_current_revision(&s));
    (void)db_query_infer_body(&s, def_of(&s, ns, "g"));
    Fingerprint fp3 = db_slot_fingerprint(&s, QUERY_INFER_BODY, (uint64_t)g.idx);
    assert(fp3 == fp_base && "a sibling decl does not perturb g's infer_body fp");
    db_request_end(&s);

    // (4) switch over an enum (synth): exhaustive arms unify to i32.
    FileId swf = open_file(&s,
        "/sw.ore",
        "Color :: enum\n    Red\n    Green\n    Blue\n"
        "pick :: fn(c: Color, a: i32, b: i32) i32\n"
        "    r := switch (c)\n        .Red => a\n        .Green => b\n        .Blue => a\n"
        "    return r\n");
    NamespaceId swns = db_get_file_namespace(&s, swf);
    db_request_begin(&s, db_current_revision(&s));
    assert(ip_index_eq(type_of_body_node(&s, swns, swf, "pick", SK_SWITCH_EXPR, 0), IP_I32_TYPE) &&
           "switch over enum: arms unify to i32");
    db_request_end(&s);

    // (5) orelse: `x orelse d` with x:?i32 → i32.
    FileId orf = open_file(&s, "/or.ore",
        "unwrap :: fn(x: ?i32, d: i32) i32\n    r := x orelse d\n    return r\n");
    NamespaceId orns = db_get_file_namespace(&s, orf);
    db_request_begin(&s, db_current_revision(&s));
    assert(ip_index_eq(type_of_body_node(&s, orns, orf, "unwrap", SK_BIN_EXPR, 0), IP_I32_TYPE) &&
           "?i32 orelse i32 → i32");
    db_request_end(&s);

    // (6a) loop header — while form: `loop (n > 0) ...` must type `n` in
    // the cond as the enclosing fn-param's type (i32). Pre-D3.4 the loop
    // header was never walked, leaving header refs unscoped → undefined.
    FileId loopw = open_file(&s, "/loopw.ore",
        "spin :: fn(n: i32) i32\n"
        "    loop (n > 0)\n"
        "        n\n"
        "    return n\n");
    NamespaceId loopw_ns = db_get_file_namespace(&s, loopw);
    db_request_begin(&s, db_current_revision(&s));
    // ref 0 is `n` in cond `n > 0` — the test case that was broken.
    assert(ip_index_eq(type_of_body_node(&s, loopw_ns, loopw, "spin",
                                          SK_REF_EXPR, 0), IP_I32_TYPE) &&
           "loop (n > 0): `n` in cond resolves to fn-param type i32");
    db_request_end(&s);

    // (6b) loop header — C-for form: `loop (i := 0; i < 10; i = i + 1)`
    // binds `i` for cond/step/body. Pre-D3.4 the init's VarDef was never
    // walked, leaving `i` bound nowhere → every header + body use was
    // undefined.
    FileId loopc = open_file(&s, "/loopc.ore",
        "iter :: fn() i32\n"
        "    loop (i := 0; i < 10; i = i + 1)\n"
        "        i\n"
        "    return 0\n");
    NamespaceId loopc_ns = db_get_file_namespace(&s, loopc);
    db_request_begin(&s, db_current_revision(&s));
    // ref 0 is `i` in cond `i < 10`. Pre-D3.4 this was IP_NONE.
    IpIndex i_in_cond = type_of_body_node(&s, loopc_ns, loopc, "iter",
                                          SK_REF_EXPR, 0);
    assert(ip_index_is_valid(i_in_cond) &&
           "loop (i := 0; i < 10; ...): `i` in cond resolves to its bind type");
    db_request_end(&s);

    // (7) nested lambda (signature-only): the inner `fn(a:i32) i32 {…}` → a fn type.
    FileId lamf = open_file(&s, "/lam.ore",
        "mk :: fn() i32\n    h := fn(a: i32) i32\n        return a\n    return h(0)\n");
    NamespaceId lamns = db_get_file_namespace(&s, lamf);
    db_request_begin(&s, db_current_revision(&s));
    IpIndex hty = type_of_body_node(&s, lamns, lamf, "mk", SK_LAMBDA_EXPR, 0);  // the nested h
    assert(ip_index_is_valid(hty) && ip_tag(&s.intern, hty) == IP_TAG_FN_TYPE &&
           "nested lambda types to a fn type (signature-only)");
    db_request_end(&s);

    db_free(&s);
    printf("PASS infer_body: inferred bind → comptime_int; param + local refs; "
           "switch arms unify; orelse unwraps; nested lambda → fn type; "
           "loop headers (while + C-for) scope correctly; fp firewall\n");
    return 0;
}
