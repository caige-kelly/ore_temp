#include "./parser.h"

// Forward declarations for the per-area parsers. Bodies live in
// parse_decl.c / parse_stmt.c / parse_expr.c.
void parse_top_level_decls(Parser *p);


// =====================================================================
// Cursor primitives
// =====================================================================

bool p_is_eof(const Parser *p) { return p_peek(p) == SK_EOF; }

const Token *p_current(const Parser *p) {
    // Typed unchecked read: bounds check is folded into the pos clamp
    // below, so vec_get's call overhead isn't earned on this hot path.
    const Token *toks = (const Token *)p->tokens->data;
    if (p->pos >= p->tokens->count) {
        if (p->tokens->count == 0) return NULL;
        return &toks[p->tokens->count - 1];
    }
    return &toks[p->pos];
}

SyntaxKind p_peek(const Parser *p) {
    const Token *t = p_current(p);
    return t ? t->kind : SK_EOF;
}

SyntaxKind p_peek_at(const Parser *p, uint32_t offset) {
    uint32_t idx = p->pos + offset;
    if (idx >= p->tokens->count) {
        if (p->tokens->count == 0) return SK_EOF;
        const Token *last = (const Token *)p->tokens->data + p->tokens->count - 1;
        return last->kind;
    }
    const Token *t = (const Token *)p->tokens->data + idx;
    return t->kind;
}

const Token *p_advance(Parser *p) {
    const Token *t = p_current(p);
    if (p_is_eof(p)) return t;

    // Emit the consumed token to the green builder. Virtual layout
    // tokens (SK_VIRTUAL_LBRACE/RBRACE/SEMI) have zero text width;
    // they emit with text_len == 0 (token_len returns 0 because
    // start == byte_end for virtuals).
    if (t && p->gb) {
        uint32_t len = token_len(t);
        const char *text = p->source ? p->source + t->start : "";
        green_builder_token(p->gb, t->kind, text, len);
    }

    p->pos++;
    return t;
}

// The brace/semi pairs are the ONLY places where a kind ask implies
// "accept the virtual sibling too". Centralized here so both p_match
// and p_consume share one rule.
static inline bool kind_matches_with_virtual(SyntaxKind asked, SyntaxKind cur) {
    if (asked == cur) return true;
    if (asked == SK_LBRACE && cur == SK_VIRTUAL_LBRACE) return true;
    if (asked == SK_RBRACE && cur == SK_VIRTUAL_RBRACE) return true;
    if (asked == SK_SEMI && cur == SK_VIRTUAL_SEMI) return true;
    return false;
}

bool p_match(Parser *p, SyntaxKind kind) {
    if (kind_matches_with_virtual(kind, p_peek(p))) {
        p_advance(p);
        return true;
    }
    return false;
}

const Token *p_consume(Parser *p, SyntaxKind kind, const char *err_msg) {
    if (kind_matches_with_virtual(kind, p_peek(p))) {
        return p_advance(p);
    }
    p_error(p, err_msg);
    return NULL;
}

bool p_check(const Parser *p, SyntaxKind kind) {
    return kind_matches_with_virtual(kind, p_peek(p));
}

void p_error(Parser *p, const char *msg) {
    ParseError e = { .tok_pos = p->pos, .msg = msg };
    vec_push(&p->errors, &e);
}


// =====================================================================
// Public entry point
// =====================================================================

GreenNode *parse_file_green(const Vec *tokens, const char *source,
                             NodeCache *cache, Vec *out_errors) {
    Parser p = {
        .gb = green_builder_new(cache),
        .tokens = tokens,
        .source = source,
        .pos = 0,
        .parsing_type = false,
        .in_distinct_rhs = false,
    };
    p.errors = *out_errors;  // caller initialized; we operate in place

    p_start_node(&p, SK_SOURCE_FILE);
    parse_top_level_decls(&p);
    p_finish_node(&p);

    GreenNode *root = green_builder_finish(p.gb);
    green_builder_destroy(p.gb);

    // Move the errors back to the caller's slot. The Vec is value-typed
    // (data + count + cap), so this is safe — no aliasing concerns.
    *out_errors = p.errors;

    return root;
}
