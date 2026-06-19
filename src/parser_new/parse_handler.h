#ifndef ORE_PARSER_NEW_PARSE_HANDLER_H
#define ORE_PARSER_NEW_PARSE_HANDLER_H

#include "./parser.h"

// =====================================================================
// Handler parser — direct C port of Koka's handlerExpr family with
// Zig-strict bodies.
//
// Framing (locked 2026-06-05): "what if Zig implemented effects." Koka
// contributes the surface — named/scoped modifiers, op-clause flavors
// (val / fn / ctl / final-ctl), and the `return` clause, with-statement
// integration. Zig contributes the body model — every clause body is a
// `parse_body()` block; values flow via explicit `return EXPR`; no
// bare-expression-body sugar.
//
// Single-shot state-machine model (no multi-shot / libmprompt /
// first-class escaping handlers). See ParserKws in parser.h for the
// op-clause surface.
//
// Source refs in koka/src/Syntax/Parse.hs r3211:
//   handlerExpr       — 1794-1807
//   handlerExprStat   — 1809-1815
//   handlerExprX      — 1817-1820
//   handlerOverride   — 1822-1827
//   handlerClauses    — 1829-1846
//   opClauses         — 1906-1916
//   handlerOpX        — 1918-1934
//   handlerOp         — 1938-1992
//   opParams          — 1994-2002
//
// AST surface:
//
//   handler { clauses }         → SK_HANDLER_EXPR
//   handle (action) { clauses } → SK_CALL_EXPR
//                                   { SK_HANDLER_EXPR, ARG_LIST { action } }
//
// Modifier storage (soft-agreed 2026-06-05): the parser keeps `named`
// / `scoped` / `override` as token children of SK_HANDLER_EXPR; sema
// reads them once during inference. (Handlers now type as plain fn-typed
// values — the collapse — so there is no handler payload to bake into.)
// =====================================================================

// Prefix-dispatcher entry. Called when the next significant token is
// `(handle|handler)`.
//
//   handler<E> { clauses }      → SK_HANDLER_EXPR
//   handle<E> (action) { … }    → SK_CALL_EXPR
//                                   { SK_HANDLER_EXPR, ARG_LIST { action } }
//
// Clauses are ordinary `name :: RHS` binds (bind-style, Slice 6.16); the
// RHS node kind is the op-sort (SK_CTL_LAMBDA / SK_FINAL_CTL_LAMBDA /
// SK_LAMBDA_EXPR / value). The lone non-bind clause is `return(x) body`,
// the result-transformer. No handler modifiers (no `named` keyword —
// instance-ness comes from the `named effect` decl + the `x :=` install;
// `scoped` is a no-op; `override` is a deferred `mask`-desugar).
void parse_handler_expr(Parser *p);

// Shared sub-rule: `[<eff>] { clauses }` — the handler body AFTER any
// `handle`/`handler` keyword. Caller owns the SK_HANDLER_EXPR start/finish
// boundary. Used by `parse_handler_expr` (keyword form) AND by
// `parse_with_stmt`'s elided forms (`with <E> { … }` / `with x := <E>
// { … }`), which have no keyword.
void parse_handler_expr_x(Parser *p);

#endif // ORE_PARSER_NEW_PARSE_HANDLER_H
