#ifndef ORE_AST_H
#define ORE_AST_H

// =====================================================================
// Typed AST wrappers — rust-analyzer style.
// =====================================================================
//
// Each wrapper is a zero-cost view over a `SyntaxNode *` from
// [src/syntax/](../syntax/syntax.h). A wrapper proves at the type level
// that the underlying SyntaxKind matches: `FnDef_cast(n, &fn)` returns
// `true` iff `syntax_node_kind(n) == SK_FN_DECL`, and only then is
// the wrapper safe to use.
//
// OWNERSHIP MODEL
// ===============
// A wrapper struct (e.g. `FnDef`) holds the underlying `SyntaxNode *`
// as a BORROWED reference — the wrapper does NOT retain/release it.
// Caller is responsible for keeping the SyntaxNode alive for the
// wrapper's lifetime.
//
// Wrapper ACCESSORS that return SyntaxNode* / SyntaxToken* delegate
// to the src/syntax navigation API. Those returned handles are
// RETURNS_OWNED — caller must release them via
// syntax_node_release / syntax_token_release.
//
// CONVENTIONS
// ===========
// - Cast functions: `bool Foo_cast(const SyntaxNode *n, Foo *out)`.
//   Returns true on success; on false, `*out` is unchanged.
// - Accessors that return a required child: `RETURNS_OWNED` SyntaxNode*
//   (or SyntaxToken*); NULL if the child is missing (parse error).
// - Accessors that return an optional child: same shape; NULL means
//   "not present" (caller checks).
// - Op-token accessors on collapsed nodes (BinExpr, AssignExpr, etc.):
//   `SyntaxKind Foo_op_kind(const Foo *f)` returns the kind of the
//   op token (SK_PLUS, SK_STAR_EQ, …). Returns SK_NONE if no op
//   token is found (recovery case).
//
// USAGE PATTERN
// =============
//   FnDef fn;
//   if (FnDef_cast(node, &fn)) {
//       SyntaxToken *name = FnDef_name(&fn);
//       if (name) {
//           // ... use name->text via syntax_token_text(name)
//           syntax_token_release(name);
//       }
//   }
//

#include <stdbool.h>
#include <stdint.h>

#include "../syntax/syntax.h"
#include "../parser/syntax_kind.h"


// ---------------------------------------------------------------------
// Generic helpers — used inside the wrapper accessors and exposed for
// the few sema sites that need to navigate ad-hoc.
// ---------------------------------------------------------------------

// RETURNS_OWNED. The first node-typed child of `n` whose kind == `k`,
// or NULL. Thin alias around syntax_node_first_child_by_kind to keep
// the wrapper bodies' intent obvious at a glance.
SyntaxNode  *ast_first_child(SyntaxNode *n, SyntaxKind k);

// RETURNS_OWNED. The first token child of `n` whose kind == `k`,
// or NULL.
SyntaxToken *ast_first_token(SyntaxNode *n, SyntaxKind k);

// RETURNS_OWNED. The Nth (0-indexed) node-typed child of `n` whose
// kind == `k`, or NULL. Used for things like the second IDENT in a
// decl that has multiple ident tokens.
SyntaxNode  *ast_nth_child(SyntaxNode *n, SyntaxKind k, uint32_t nth);

// RETURNS_OWNED. The first child element matching ANY of the kinds
// in `kinds[0..count)`. Used for op-token lookups where a collapsed
// node admits multiple operator-token kinds (e.g. BinExpr accepts
// SK_PLUS, SK_MINUS, etc.).
SyntaxToken *ast_first_token_any(SyntaxNode *n,
                                  const SyntaxKind *kinds, uint32_t count);

// RETURNS_OWNED. The first token child satisfying the predicate
// `pred(kind) == true`. The classifier-based variant — handy when
// the set of accepted kinds is large (e.g. all bin-op tokens).
typedef bool (*AstKindPredicate)(OreSyntaxKind);
SyntaxToken *ast_first_token_pred(SyntaxNode *n, AstKindPredicate pred);

// RETURNS_OWNED. The first node-typed child satisfying the predicate.
SyntaxNode  *ast_first_child_pred(SyntaxNode *n, AstKindPredicate pred);


#endif  // ORE_AST_H
