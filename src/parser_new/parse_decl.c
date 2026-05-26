#include "./parse_decl.h"
#include "./parse_expr.h"

// =====================================================================
// Top-level declarations.
//
// Ore is Odin-style and expression-oriented: there is no separate decl
// grammar. A top-level item is just `parse_expr(p, PREC_NONE)`, and the
// bind operators (`::` / `:=` / `:`) are a guarded low-precedence Pratt
// infix (see parse_expr.c) that yields SK_CONST_DECL / SK_VAR_DECL /
// SK_DESTRUCTURE_DECL inside the green tree.
//
// In the green-tree model the SK_SOURCE_FILE root has top-level decls
// as direct children (the parser owns the start/finish of SOURCE_FILE
// in parse_file_green). No explicit "module" wrapper node is emitted.
// =====================================================================

void parse_top_level_decls(Parser *p) {
    while (!p_is_eof(p)) {
        uint32_t before = p->pos;

        // parse_expr at PREC_NONE accepts the bind ops (which produce
        // SK_*_DECL nodes). If parse_expr emits a non-decl node at top
        // level, sema flags it later — the parser doesn't enforce that
        // here (the green tree only carries kinds; no easy "what was
        // the topmost node?" peek without builder introspection).
        parse_expr(p, PREC_NONE);

        // Layout emits a `;` after every top-level decl (real or
        // virtual). Consume it so the loop can proceed; missing
        // terminator records an error and the loop continues at the
        // next token (forward-progress guard below).
        p_consume(p, SK_SEMI, "expected ';' after top-level declaration");

        // Forward-progress guard: never spin on an unparseable token.
        if (p->pos == before) p_advance(p);
    }
}
