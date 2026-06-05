#include "./parse_handler.h"
#include "./parse_expr.h"
#include "./parse_stmt.h"

// =====================================================================
// Handler parser implementation.
//
// This file owns the `handler<E> { … }` / `handle<E>(action) { … }`
// WRAPPER parse and the `with` modifier surface (named / scoped). The
// CLAUSES themselves are ordinary statements — a handler body is just a
// `parse_stmt` block (`parse_handler_clauses` → `parse_block(p,
// parse_handler_clause_stmt)`). Each clause is:
//   - `return(x) body` — the result-transformer (the one non-bind form), or
//   - a `name :: RHS` bind whose RHS node kind IS the op-sort:
//       ctl(…)/final-ctl(…) → SK_CTL_LAMBDA / SK_FINAL_CTL_LAMBDA
//       fn(…)               → SK_LAMBDA_EXPR
//       value               → val op
// The `ctl`/`final-ctl` lambda-introducers live in parse_prefix (shared
// with `fn`); there is no bespoke op-clause parser here.
//
// BODIES are `parse_body`. bare blocks yield void,
// values flow via explicit `return EXPR` / labeled `break :l v`.
//
// Contextual keywords use the pre-interned `p->kws` table; every check is
// one u32 compare against `t->string_id`.
// =====================================================================


// ---- Forward decls --------------------------------------------------
static void parse_handler_clauses(Parser *p);
static void parse_handler_clause_stmt(Parser *p);


// =====================================================================
// handlerExpr — top-level dispatcher.
// =====================================================================
void parse_handler_expr(Parser *p) {
  SyntaxKind kw = p_peek(p);
  if (kw != SK_HANDLE_KW && kw != SK_HANDLER_KW) {
    p_error(p, "expected 'handle' or 'handler'");
    return;
  }
  bool is_handle = (kw == SK_HANDLE_KW);

  // Outer checkpoint: for the `handle` form we retro-wrap from here as
  // SK_CALL_EXPR so the SK_HANDLER_EXPR becomes the callee.
  Checkpoint outer_cp = p_checkpoint(p);

  p_start_node(p, SK_HANDLER_EXPR);
  p_advance(p); // handle | handler

  // No handler modifiers: instance-ness comes from the `named effect`
  // decl + the `x :=` install (parse_with_stmt), not the handler; `scoped`
  // is a no-op; `override` is a `mask`-desugar (deferred).
  parse_handler_expr_x(p);

  p_finish_node(p); // SK_HANDLER_EXPR

  if (is_handle) {
    // Koka: `App handlerVal [(Nothing, arg)]`. Emit
    //   SK_CALL_EXPR { SK_HANDLER_EXPR, SK_ARG_LIST { action } }.
    // Parens are required (ore convention; matches the action being a
    // self-contained expression argument).
    p_start_node(p, SK_ARG_LIST);
    p_consume(p, SK_LPAREN, "expected '(' after handle clauses");
    parse_expr(p, PREC_NONE);
    p_consume(p, SK_RPAREN, "expected ')' after handle action");
    p_finish_node(p); // SK_ARG_LIST
    p_start_node_at(p, outer_cp, SK_CALL_EXPR);
    p_finish_node(p); // SK_CALL_EXPR
  }
}


// =====================================================================
// handlerExprX — shared sub-rule: `[<eff>] { clauses }`. Used by both
// `parse_handler_expr` (after the handle/handler keyword) and the `with`
// instance/handler dispatch in parse_with_stmt (NO keyword — the elided
// `with <E> { … }` / `with x := <E> { … }` forms). NOT static — exposed
// in parse_handler.h.
//
// No modifiers: Koka's `scoped` (no-op), `override` (a `mask`-desugar,
// deferred), and the `named` keyword (instance-ness comes from the
// `named effect` decl + the `x :=` install) are all dropped.
// =====================================================================
void parse_handler_expr_x(Parser *p) {
  // Optional `<eff>` annotation → SK_EFFECT_ROW_TYPE child.
  if (p_peek(p) == SK_LT)
    parse_effect_row(p);
  // Clauses.
  parse_handler_clauses(p);
}


// =====================================================================
// handlerClauses — emit the clause block (bind-style, Slice 6.16).
//
// A handler body is an ordinary statement block. Each clause is either:
//   - the `return(x) body` result-transformer (the lone non-bind form), or
//   - a `name :: RHS` bind, where the RHS shape IS the op-sort:
//       name :: ctl(params) body        → SK_CTL_LAMBDA       (control op)
//       name :: final-ctl(params) body  → SK_FINAL_CTL_LAMBDA (final op)
//       name :: fn(params) body         → SK_LAMBDA_EXPR      (fn op)
//       name :: EXPR                    → value               (val op)
//   (`:=` var-binds are also accepted — mutable handler-local state.)
//
// The op-sort falls out of the RHS node kind (the `ctl`/`final-ctl`
// lambda-introducer arms live in parse_prefix), so there is no bespoke
// clause parser, no sort-token walk, and no `val` keyword. parse_stmt
// handles the bind directly. Sema dispatches on the RHS kind.
// =====================================================================
static void parse_handler_clauses(Parser *p) {
  parse_block(p, parse_handler_clause_stmt);
}


// Per-statement callback for a handler body. `return(x) body` is the
// only non-bind clause (it's the result-transformer, not a named op);
// everything else is an ordinary `name :: RHS` bind via parse_stmt.
static void parse_handler_clause_stmt(Parser *p) {
  if (p_peek(p) == SK_RETURN_KW) {
    // `return(x [: T]) body` — the handler's result-transformer.
    p_start_node(p, SK_RETURN_CLAUSE);
    p_advance(p); // return
    p_consume(p, SK_LPAREN, "expected '(' after 'return' clause keyword");
    p_start_node(p, SK_PARAM_LIST);
    parse_param(p, /*name_required=*/true);
    p_finish_node(p); // SK_PARAM_LIST
    p_consume(p, SK_RPAREN, "expected ')' after return-clause param");
    parse_body(p, parse_stmt);
    p_finish_node(p); // SK_RETURN_CLAUSE
    return;
  }
  // Op-clause = an ordinary bind. The RHS kind is the op-sort.
  parse_stmt(p);
}
