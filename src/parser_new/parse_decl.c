#include "./parse_decl.h"
#include "./parse_expr.h"

// =====================================================================
// Top-level declarations.
//
// Ore is Odin-style and expression-oriented, but the MODULE scope is
// STRICTLY bind decls — `fn` / `struct` / `enum` / `union` / `effect` /
// `const` / `var` / typed are all encoded as `IDENT (::|:=|:) value`
// patterns. Execution statements (loops, ifs, plain expressions,
// assignments) are NOT legal at module scope — they belong inside fn
// bodies. parse_decl gate-keeps with explicit lookahead and emits a
// hard error on anything else, instead of routing through parse_stmt
// (which would silently accept executable code at top level).
//
// In the green-tree model the SK_SOURCE_FILE root has top-level decls
// as direct children (the parser owns the start/finish of SOURCE_FILE
// in parse_file_green). No explicit "module" wrapper node is emitted.
// =====================================================================

void parse_top_level_decls(Parser *p) {
  while (!p_is_eof(p)) {
    uint32_t before = p->pos;

    bool matched = false;

    // Named bind: `IDENT (::|:=|:) ...`. Covers fn / struct / enum /
    // union / effect / const / var / typed decls — they all share this
    // shape (the keyword lives on the RHS of `::`).
    if (p_peek(p) == SK_IDENT) {
      SyntaxKind nx = p_peek_at(p, 1);
      if (nx == SK_COLON_COLON || nx == SK_COLON_EQ || nx == SK_COLON) {
        parse_named_bind_decl(p);
        matched = true;
      } else if (nx == SK_COMMA) {
        // Bare tuple-destructure `x, y (::|:=) value` (Slice 6.23).
        Checkpoint cp = p_checkpoint(p);
        parse_expr(p, PREC_NONE);
        parse_bare_destructure_tail(p, cp);
        matched = true;
      }
    }
    // (`.{x,y} :=` destructure removed — module-scope destructure is the
    // bare `x, y (::|:=) value` form handled in the IDENT branch above.)
    if (!matched) {
      // Wrap the non-declaration tokens up to the next `;` in an
      // SK_ERROR_NODE — one diag, no cascade.
      p_recover_auto(p,
                "expected a top-level declaration "
                "(fn / struct / enum / const / var); execution "
                "statements are only allowed inside function bodies");
    }

    // Layout emits a `;` after every top-level decl (real or
    // virtual). The bind-decl parsers do NOT consume the trailing semi
    // — that contract is enforced here.
    p_consume(p, SK_SEMI, "expected ';' after top-level declaration");

    // Forward-progress guard: never spin on an unparseable token.
    if (p->pos == before)
      p_advance(p);
  }
}
