// Phase 4e — rowan-parity smoke test.
//
// Ports the conceptual content of `rowan/examples/s_expressions.rs` to C.
// Demonstrates that src/syntax/ can host a real grammar end-to-end:
//
//   - Hand-written lexer + parser for arithmetic S-expressions
//   - Build a green tree via GreenBuilder
//   - Immutable navigation and text rendering
//   - clone_for_update on an interior subtree + an atom rewrite
//   - Verify original immutable tree is unchanged
//   - SyntaxNodePtr roundtrip across the mutated tree
//
// This is the "if this passes under ASan, we ported rowan" demonstration.

#include "../src/syntax/syntax.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIE(...) do { fprintf(stderr, "syntax_smoke_test: " __VA_ARGS__); \
                       fprintf(stderr, "\n"); exit(1); } while (0)


// =====================================================================
// SyntaxKind — mirrors the s_expressions.rs enum
// =====================================================================

enum {
    SK_L_PAREN = 1,
    SK_R_PAREN,
    SK_WORD,
    SK_WHITESPACE,
    SK_ERROR,
    SK_LIST,
    SK_ATOM,
    SK_ROOT,
};


// =====================================================================
// Lexer — produces (kind, lexeme) pairs from source
// =====================================================================

typedef struct {
    int      kind;
    const char *text;
    uint32_t len;
} LexTok;

typedef struct {
    LexTok *buf;
    uint32_t cap;
    uint32_t count;
} LexResult;

static void lex(const char *src, LexResult *out) {
    out->cap = 16;
    out->count = 0;
    out->buf = malloc(out->cap * sizeof(LexTok));

    const char *p = src;
    while (*p) {
        if (out->count == out->cap) {
            out->cap *= 2;
            out->buf = realloc(out->buf, out->cap * sizeof(LexTok));
        }
        LexTok *t = &out->buf[out->count++];
        t->text = p;
        if (*p == '(') {
            t->kind = SK_L_PAREN; t->len = 1; p++;
        } else if (*p == ')') {
            t->kind = SK_R_PAREN; t->len = 1; p++;
        } else if (isspace((unsigned char)*p)) {
            const char *s = p;
            while (*p && isspace((unsigned char)*p)) p++;
            t->kind = SK_WHITESPACE; t->len = (uint32_t)(p - s);
        } else if (isalnum((unsigned char)*p) || *p == '+' || *p == '-' ||
                   *p == '*' || *p == '/' || *p == '_') {
            const char *s = p;
            while (*p && (isalnum((unsigned char)*p) || *p == '+' || *p == '-' ||
                          *p == '*' || *p == '/' || *p == '_')) p++;
            t->kind = SK_WORD; t->len = (uint32_t)(p - s);
        } else {
            t->kind = SK_ERROR; t->len = 1; p++;
        }
    }
}


// =====================================================================
// Parser — produces a green tree
// =====================================================================

typedef struct {
    LexResult     *lex;
    uint32_t       cursor;
    GreenBuilder  *builder;
} Parser;

static int peek_kind(Parser *p) {
    if (p->cursor >= p->lex->count) return 0;
    return p->lex->buf[p->cursor].kind;
}

static void bump(Parser *p) {
    LexTok *t = &p->lex->buf[p->cursor++];
    green_builder_token(p->builder, (SyntaxKind)t->kind, t->text, t->len);
}

static void skip_ws(Parser *p) {
    while (peek_kind(p) == SK_WHITESPACE) bump(p);
}

// SexpRes: 0 = OK, 1 = EOF, 2 = unexpected RPAREN
static int parse_sexp(Parser *p);

static void parse_list(Parser *p) {
    green_builder_start_node(p->builder, SK_LIST);
    bump(p);  // '('
    for (;;) {
        int r = parse_sexp(p);
        if (r == 1) break;       // EOF inside list — accept (error)
        if (r == 2) { bump(p); break; }  // ')'
    }
    green_builder_finish_node(p->builder);
}

static int parse_sexp(Parser *p) {
    skip_ws(p);
    int k = peek_kind(p);
    if (k == 0) return 1;
    if (k == SK_R_PAREN) return 2;
    if (k == SK_L_PAREN) { parse_list(p); return 0; }
    if (k == SK_WORD) {
        green_builder_start_node(p->builder, SK_ATOM);
        bump(p);
        green_builder_finish_node(p->builder);
        return 0;
    }
    if (k == SK_ERROR) { bump(p); return 0; }
    return 0;
}

static GreenNode *parse(const char *src, NodeCache *cache) {
    LexResult lr;
    lex(src, &lr);

    Parser p = {.lex = &lr, .cursor = 0, .builder = green_builder_new(cache)};
    green_builder_start_node(p.builder, SK_ROOT);
    for (;;) {
        int r = parse_sexp(&p);
        if (r == 1) break;
        if (r == 2) {  // unexpected ')'
            green_builder_start_node(p.builder, SK_ERROR);
            bump(&p);
            green_builder_finish_node(p.builder);
        }
    }
    skip_ws(&p);
    green_builder_finish_node(p.builder);

    GreenNode *g = green_builder_finish(p.builder);
    green_builder_destroy(p.builder);
    free(lr.buf);
    return g;
}


// =====================================================================
// Test 1: parse roundtrip + child structure
// =====================================================================

static void test_parse_roundtrip(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = parse("(+ (* 15 2) 62)", cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    // Total text should roundtrip exactly.
    SyntaxText st = syntax_text_of(root);
    char buf[64];
    syntax_text_to_cstr(&st, buf, sizeof(buf));
    if (strcmp(buf, "(+ (* 15 2) 62)") != 0)
        DIE("roundtrip: '%s', want '(+ (* 15 2) 62)'", buf);

    // ROOT has exactly one LIST child.
    SyntaxNode *list = syntax_node_first_child(root);
    if (!list) DIE("root has no list child");
    if (syntax_node_kind(list) != SK_LIST)
        DIE("root child kind = %u, want LIST", syntax_node_kind(list));

    TextRange r = syntax_node_text_range(list);
    if (r.start != 0 || r.length != 15)
        DIE("list range = {%u,%u}, want {0,15}", r.start, r.length);

    // Verify the 7 children of the outer LIST against rowan's exact
    // assertion from rowan/examples/s_expressions.rs::test_parser:
    //   L_PAREN@0..1, ATOM@1..2, WHITESPACE@2..3, LIST@3..11,
    //   WHITESPACE@11..12, ATOM@12..14, R_PAREN@14..15.
    struct { int kind; uint32_t start; uint32_t length; } expected[7] = {
        {SK_L_PAREN,     0,  1},
        {SK_ATOM,        1,  1},
        {SK_WHITESPACE,  2,  1},
        {SK_LIST,        3,  8},
        {SK_WHITESPACE, 11,  1},
        {SK_ATOM,       12,  2},
        {SK_R_PAREN,    14,  1},
    };
    for (uint32_t i = 0; i < 7; i++) {
        SyntaxElement e = syntax_node_child_or_token(list, i);
        SyntaxKind k = (e.kind == SYNTAX_ELEM_NODE)
                           ? syntax_node_kind(e.node)
                           : syntax_token_kind(e.token);
        TextRange tr = (e.kind == SYNTAX_ELEM_NODE)
                           ? syntax_node_text_range(e.node)
                           : syntax_token_text_range(e.token);
        if ((int)k != expected[i].kind)
            DIE("child %u kind = %u, want %u", i, k, expected[i].kind);
        if (tr.start != expected[i].start || tr.length != expected[i].length)
            DIE("child %u range = {%u,%u}, want {%u,%u}", i,
                tr.start, tr.length, expected[i].start, expected[i].length);
        SYN_ELEM_RELEASE(e);
    }

    SYN_RELEASE(list);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_parse_roundtrip: OK\n");
}


// =====================================================================
// Test 2: SyntaxNodePtr resolves across an unchanged tree
// =====================================================================

static void test_node_ptr_roundtrip(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g1 = parse("(+ (* 15 2) 62)", cache);
    SyntaxTree *tree1 = syntax_tree_new(g1);
    SyntaxNode *root1 = syntax_tree_root(tree1);

    // Get the inner LIST (the '(* 15 2)' subexpr).
    SyntaxNode *outer = syntax_node_first_child(root1);
    SyntaxElement inner_e = syntax_node_child_or_token(outer, 3);
    if (inner_e.kind != SYNTAX_ELEM_NODE) DIE("inner not a node");
    if (syntax_node_kind(inner_e.node) != SK_LIST) DIE("inner kind != LIST");

    SyntaxNodePtr ptr = syntax_node_ptr_new(inner_e.node);
    if (ptr.kind != SK_LIST) DIE("ptr.kind");
    if (ptr.range.start != 3 || ptr.range.length != 8)
        DIE("ptr.range = {%u,%u}, want {3,8}", ptr.range.start, ptr.range.length);

    // Re-parse the same text; resolve the ptr against the new tree.
    GreenNode *g2 = parse("(+ (* 15 2) 62)", cache);
    SyntaxTree *tree2 = syntax_tree_new(g2);
    SyntaxNode *root2 = syntax_tree_root(tree2);

    SyntaxNode *resolved = syntax_node_ptr_resolve(ptr, root2);
    if (!resolved) DIE("ptr did not resolve");
    if (syntax_node_kind(resolved) != SK_LIST) DIE("resolved kind != LIST");
    TextRange rr = syntax_node_text_range(resolved);
    if (rr.start != 3 || rr.length != 8) DIE("resolved range mismatch");

    SYN_RELEASE(resolved);
    SYN_ELEM_RELEASE(inner_e);
    SYN_RELEASE(outer);
    SYN_RELEASE(root2);
    syntax_tree_free(tree2);
    SYN_RELEASE(root1);
    syntax_tree_free(tree1);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_node_ptr_roundtrip: OK\n");
}


// =====================================================================
// Test 3: clone_for_update + replace_with — the headline rowan demo
// =====================================================================
//
// Take the immutable parse of "(+ (* 15 2) 62)", clone_for_update into
// a mutable copy, replace the '2' atom with '7'. Verify:
//   - the mutable tree now reads "(+ (* 15 7) 62)"
//   - the original immutable tree is unchanged
static void test_clone_and_rewrite(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = parse("(+ (* 15 2) 62)", cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    // Navigate to the '2' atom: ROOT/LIST/LIST(inner)/ATOM(last word before ')').
    SyntaxNode *outer = syntax_node_first_child(root);
    SyntaxElement inner_e = syntax_node_child_or_token(outer, 3);
    SyntaxNode *inner = inner_e.node;  // outer LIST
    // inner is "(* 15 2)": L_PAREN, ATOM(*), WS, ATOM(15), WS, ATOM(2), R_PAREN
    SyntaxElement two_atom_e = syntax_node_child_or_token(inner, 5);
    if (two_atom_e.kind != SYNTAX_ELEM_NODE) DIE("two_atom not a node");
    if (syntax_node_kind(two_atom_e.node) != SK_ATOM) DIE("two_atom kind != ATOM");

    // Clone into a mutable tree.
    SyntaxNode *mut_two = syntax_node_clone_for_update(two_atom_e.node);
    if (!syntax_node_is_mutable(mut_two)) DIE("clone not mutable");

    // Build a replacement ATOM containing '7'.
    NodeCache *aux = node_cache_new();
    GreenBuilder *bb = green_builder_new(aux);
    green_builder_start_node(bb, SK_ATOM);
        green_builder_token(bb, SK_WORD, "7", 1);
    green_builder_finish_node(bb);
    GreenNode *new_atom = green_builder_finish(bb);
    green_builder_destroy(bb);

    GreenNode *new_root_g = syntax_node_replace_with(mut_two, new_atom);

    // Navigate back to root of the mutable tree.
    SyntaxNode *mut_root = mut_two;
    syntax_node_retain(mut_root);  // we'll walk and release at the end
    for (;;) {
        SyntaxNode *p = syntax_node_parent(mut_root);
        if (!p) break;
        SYN_RELEASE(mut_root);
        mut_root = p;
    }

    char buf[64];
    SyntaxText st = syntax_text_of(mut_root);
    syntax_text_to_cstr(&st, buf, sizeof(buf));
    if (strcmp(buf, "(+ (* 15 7) 62)") != 0)
        DIE("mutated tree = '%s', want '(+ (* 15 7) 62)'", buf);

    // Original immutable tree is UNCHANGED.
    st = syntax_text_of(root);
    syntax_text_to_cstr(&st, buf, sizeof(buf));
    if (strcmp(buf, "(+ (* 15 2) 62)") != 0)
        DIE("original mutated! '%s'", buf);

    green_node_release(new_root_g);
    green_node_release(new_atom);
    node_cache_destroy(aux);

    SYN_RELEASE(mut_root);
    SYN_RELEASE(mut_two);
    SYN_ELEM_RELEASE(two_atom_e);
    SYN_ELEM_RELEASE(inner_e);
    SYN_RELEASE(outer);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_clone_and_rewrite: OK\n");
}


// =====================================================================
// Test 4: structural sharing via NodeCache (parse-level dedup)
// =====================================================================
//
// Tokens are always interned (no children-count gate). Verify that two
// distinct parses producing the same ATOM text yield the SAME GreenToken
// pointer at the WORD level. (Nodes with >3 children skip the cache per
// rowan's heuristic, so ROOT/LIST won't pointer-equal in larger parses.)
static void test_structural_sharing(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g1 = parse("(+ 1 2)", cache);
    GreenNode *g2 = parse("(+ 1 2)", cache);

    // Find the '+' ATOM in each parse; its inner WORD token should be the
    // same GreenToken pointer (tokens dedupe unconditionally).
    SyntaxTree *t1 = syntax_tree_new(g1); SyntaxNode *r1 = syntax_tree_root(t1);
    SyntaxTree *t2 = syntax_tree_new(g2); SyntaxNode *r2 = syntax_tree_root(t2);

    SyntaxNode *list1 = syntax_node_first_child(r1);
    SyntaxNode *list2 = syntax_node_first_child(r2);

    // Inside LIST: index 1 is the '+' ATOM (after L_PAREN at 0).
    SyntaxElement atom1_e = syntax_node_child_or_token(list1, 1);
    SyntaxElement atom2_e = syntax_node_child_or_token(list2, 1);

    // Inside ATOM: index 0 is the WORD token.
    SyntaxElement plus1_e = syntax_node_child_or_token(atom1_e.node, 0);
    SyntaxElement plus2_e = syntax_node_child_or_token(atom2_e.node, 0);

    const GreenToken *gt1 = syntax_token_green(plus1_e.token);
    const GreenToken *gt2 = syntax_token_green(plus2_e.token);
    if (gt1 != gt2) DIE("token dedup failed: distinct '+' tokens (%p vs %p)",
                          (const void *)gt1, (const void *)gt2);

    SYN_ELEM_RELEASE(plus1_e);
    SYN_ELEM_RELEASE(plus2_e);
    SYN_ELEM_RELEASE(atom1_e);
    SYN_ELEM_RELEASE(atom2_e);
    SYN_RELEASE(list1);
    SYN_RELEASE(list2);
    SYN_RELEASE(r1);
    SYN_RELEASE(r2);
    syntax_tree_free(t1);
    syntax_tree_free(t2);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_structural_sharing: OK\n");
}


// =====================================================================
// Test 5: math.rs — Pratt-style parser via checkpoint / start_node_at
// =====================================================================
//
// Ports `rowan/examples/math.rs` to validate the checkpoint mechanism.
// Source: "1 + 2 * 3 + 4". Builds an operator-precedence tree using
// retroactive node-start-at-checkpoint. Renders with the same indent
// format rowan's main() uses; asserts character-for-character equality
// with rowan's output.

enum {
    M_WHITESPACE = 100,
    M_ADD,
    M_SUB,
    M_MUL,
    M_DIV,
    M_NUMBER,
    M_ERROR,
    M_OPERATION,
    M_ROOT,
};

static const char *math_kind_name(SyntaxKind k) {
    switch (k) {
        case M_WHITESPACE: return "WHITESPACE";
        case M_ADD:        return "ADD";
        case M_SUB:        return "SUB";
        case M_MUL:        return "MUL";
        case M_DIV:        return "DIV";
        case M_NUMBER:     return "NUMBER";
        case M_ERROR:      return "ERROR";
        case M_OPERATION:  return "OPERATION";
        case M_ROOT:       return "ROOT";
        default:           return "?";
    }
}

typedef struct { int kind; const char *text; } MathTok;
typedef struct {
    GreenBuilder *b;
    const MathTok *toks;
    uint32_t       count, cursor;
} MathParser;

static int math_peek(MathParser *p) {
    while (p->cursor < p->count && p->toks[p->cursor].kind == M_WHITESPACE) {
        const MathTok *t = &p->toks[p->cursor++];
        green_builder_token(p->b, (SyntaxKind)t->kind, t->text,
                              (uint32_t)strlen(t->text));
    }
    return p->cursor < p->count ? p->toks[p->cursor].kind : -1;
}

static void math_bump(MathParser *p) {
    if (p->cursor < p->count) {
        const MathTok *t = &p->toks[p->cursor++];
        green_builder_token(p->b, (SyntaxKind)t->kind, t->text,
                              (uint32_t)strlen(t->text));
    }
}

static void math_parse_val(MathParser *p) {
    if (math_peek(p) == M_NUMBER) { math_bump(p); return; }
    green_builder_start_node(p->b, M_ERROR);
    math_bump(p);
    green_builder_finish_node(p->b);
}

static void math_handle_op(MathParser *p, const int *set, uint32_t set_n,
                             void (*next)(MathParser *)) {
    Checkpoint cp = green_builder_checkpoint(p->b);
    next(p);
    for (;;) {
        int k = math_peek(p);
        bool in_set = false;
        for (uint32_t i = 0; i < set_n; i++) if (set[i] == k) { in_set = true; break; }
        if (!in_set) break;
        green_builder_start_node_at(p->b, cp, M_OPERATION);
        math_bump(p);
        next(p);
        green_builder_finish_node(p->b);
    }
}

static void math_parse_mul(MathParser *p) {
    static const int set[] = {M_MUL, M_DIV};
    math_handle_op(p, set, 2, math_parse_val);
}
static void math_parse_add(MathParser *p) {
    static const int set[] = {M_ADD, M_SUB};
    math_handle_op(p, set, 2, math_parse_mul);
}

typedef struct { char *buf; size_t cap; size_t off; } Sink;

static void sink_printf(Sink *s, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    if (s->off < s->cap) {
        s->off += (size_t)vsnprintf(s->buf + s->off, s->cap - s->off, fmt, ap);
    }
    va_end(ap);
}

static void math_print(int indent, SyntaxElement e, Sink *out) {
    for (int i = 0; i < indent; i++) sink_printf(out, " ");
    SyntaxKind k = (e.kind == SYNTAX_ELEM_NODE)
                       ? syntax_node_kind(e.node)
                       : syntax_token_kind(e.token);
    const char *kname = math_kind_name(k);
    if (e.kind == SYNTAX_ELEM_NODE) {
        sink_printf(out, "- %s\n", kname);
        uint32_t count = syntax_node_num_children(e.node);
        for (uint32_t i = 0; i < count; i++) {
            SyntaxElement c = syntax_node_child_or_token(e.node, i);
            math_print(indent + 2, c, out);
            SYN_ELEM_RELEASE(c);
        }
    } else {
        sink_printf(out, "- \"%s\" %s\n", syntax_token_text(e.token), kname);
    }
}

static void test_math_pratt_parser(void) {
    static const MathTok TOKS[] = {
        {M_NUMBER, "1"}, {M_WHITESPACE, " "}, {M_ADD, "+"}, {M_WHITESPACE, " "},
        {M_NUMBER, "2"}, {M_WHITESPACE, " "}, {M_MUL, "*"}, {M_WHITESPACE, " "},
        {M_NUMBER, "3"}, {M_WHITESPACE, " "}, {M_ADD, "+"}, {M_WHITESPACE, " "},
        {M_NUMBER, "4"},
    };
    NodeCache *cache = node_cache_new();
    MathParser p = {.b = green_builder_new(cache), .toks = TOKS,
                    .count = sizeof(TOKS) / sizeof(TOKS[0]), .cursor = 0};
    green_builder_start_node(p.b, M_ROOT);
    math_parse_add(&p);
    green_builder_finish_node(p.b);
    GreenNode *g = green_builder_finish(p.b);
    green_builder_destroy(p.b);

    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxElement re = {.kind = SYNTAX_ELEM_NODE, .node = root};
    syntax_node_retain(root);  // wrap_node borrows; we'll release one ref after print

    char buf[2048];
    Sink sink = {.buf = buf, .cap = sizeof(buf), .off = 0};
    math_print(0, re, &sink);

    // Expected output mirrors what `cargo run --example math` produces.
    const char *expected =
        "- ROOT\n"
        "  - OPERATION\n"
        "    - OPERATION\n"
        "      - \"1\" NUMBER\n"
        "      - \" \" WHITESPACE\n"
        "      - \"+\" ADD\n"
        "      - OPERATION\n"
        "        - \" \" WHITESPACE\n"
        "        - \"2\" NUMBER\n"
        "        - \" \" WHITESPACE\n"
        "        - \"*\" MUL\n"
        "        - \" \" WHITESPACE\n"
        "        - \"3\" NUMBER\n"
        "      - \" \" WHITESPACE\n"
        "    - \"+\" ADD\n"
        "    - \" \" WHITESPACE\n"
        "    - \"4\" NUMBER\n";

    if (strcmp(buf, expected) != 0) {
        fprintf(stderr, "math output mismatch:\n--- got ---\n%s\n--- want ---\n%s\n",
                buf, expected);
        DIE("math output differs from rowan");
    }

    // Drop both refs (the syntax_tree_root return + the explicit retain
    // above). SYN_RELEASE nulls its argument after the first release, so
    // we use two distinct handles.
    SyntaxNode *root2 = root;
    SYN_RELEASE(root);   // release the +1 from retain above
    SYN_RELEASE(root2);  // release the original from syntax_tree_root
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_math_pratt_parser: OK\n");
}


int main(void) {
    fprintf(stderr, "syntax_smoke_test: starting\n");
    test_parse_roundtrip();
    test_node_ptr_roundtrip();
    test_clone_and_rewrite();
    test_structural_sharing();
    test_math_pratt_parser();
    fprintf(stderr, "syntax_smoke_test: all PASS\n");
    return 0;
}
