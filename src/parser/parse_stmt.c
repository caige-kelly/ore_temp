#include "parse_stmt.h"
#include "parse_expr.h"

// A statement is just an expression at PREC_NONE (binds, control flow,
// returns, etc. are all expression forms — see parse_expr.c / GRAMMAR.md).
// The optional trailing `;` is the layout-injected sibling separator.
AstNodeId parse_stmt(Parser *p) {
    AstNodeId node = parse_expr(p, PREC_NONE);
    p_match(p, TK_SEMI);
    return node;
}

AstNodeId parse_block(Parser *p) {
    const Token *start_tok = p_consume(p, TK_LBRACE, "Expected '{' to start block");
    if (!start_tok) return AST_NODE_ID_NONE;
    uint32_t op_index = p->pos - 1;

    uint32_t stmts[1024];
    uint32_t stmt_count = 0;

    while (!p_is_eof(p) && p_peek(p) != TK_RBRACE) {
        // Skip stray separators (layout injects `;` before `}`).
        if (p_peek(p) == TK_SEMI) { p_advance(p); continue; }

        uint32_t before = p->pos;
        AstNodeId stmt = parse_stmt(p);
        if (stmt.idx != 0 && stmt_count < 1024) {
            stmts[stmt_count++] = stmt.idx;
        }
        // Forward-progress guard: never spin on an unparseable token.
        if (p->pos == before) p_advance(p);
    }

    const Token *end_tok = p_consume(p, TK_RBRACE, "Expected '}' to end block");
    if (!end_tok) return AST_NODE_ID_NONE;

    uint32_t extra_payload[1025];
    extra_payload[0] = stmt_count;
    for (uint32_t i = 0; i < stmt_count; i++) extra_payload[i + 1] = stmts[i];

    AstExtraDataIdx extra = ast_push_extra(p->mod->ast, extra_payload, stmt_count + 1);
    AstNodeData data = {0};
    data.extra_idx = extra;

    return p_push_node(p, AST_STMT_BLOCK, op_index, data, p_span(p, start_tok, end_tok));
}
