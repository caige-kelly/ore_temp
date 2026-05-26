#include "./parse_stmt.h"
#include "./parse_expr.h"

// =====================================================================
// Statements + blocks.
// =====================================================================
//
// A statement is a single expression at PREC_NONE. The terminating `;`
// is NOT consumed here — the caller owns it. (Koka-style layout
// guarantees exactly one `;` after every statement, including the last
// before `}`, so the call site can require it without ambiguity.)

void parse_stmt(Parser *p) {
    parse_expr(p, PREC_NONE);
}

bool parse_block(Parser *p) {
    // Block: { stmt; stmt; ... }
    //
    // Green tree shape:
    //   SK_BLOCK_STMT
    //     LBRACE (or VIRTUAL_LBRACE)
    //     SK_STMT_LIST
    //       <stmt_node> SEMI <stmt_node> SEMI ...
    //     RBRACE (or VIRTUAL_RBRACE)

    p_start_node(p, SK_BLOCK_STMT);

    const Token *open = p_consume(p, SK_LBRACE, "expected '{' to start block");
    if (!open) {
        p_finish_node(p);
        return false;
    }

    p_start_node(p, SK_STMT_LIST);

    while (!p_is_eof(p) && !p_check(p, SK_RBRACE)) {
        uint32_t before = p->pos;

        parse_stmt(p);

        // Mandatory terminator: layout emits a `;` after every statement,
        // including the last one immediately before `}`.
        p_consume(p, SK_SEMI, "expected ';' after statement");

        // Forward-progress guard: never spin on an unparseable token.
        if (p->pos == before) p_advance(p);
    }

    p_finish_node(p);  // SK_STMT_LIST

    p_consume(p, SK_RBRACE, "expected '}' to end block");
    p_finish_node(p);  // SK_BLOCK_STMT
    return true;
}
