#ifndef ORE_SYNTAX_KIND_H
#define ORE_SYNTAX_KIND_H

// =====================================================================
// OreSyntaxKind — the parser ↔ green-tree ↔ downstream contract.
// =====================================================================
//
// Single uint16_t enum spanning every kind of node and token that
// Ore's parser emits and every consumer (sema, db, ide) dispatches on.
// Low values are TOKEN kinds (leaves of the green tree); values from
// SK_FIRST_NODE_KIND upward are NODE kinds (internal tree nodes).
//
// CONTRACT
// ========
// The parser passes these as `SyntaxKind` (uint16_t) to the green
// builder API: green_builder_start_node(b, SK_FN_DECL), etc. Every
// dispatch site reads kinds back from the tree via syntax_node_kind()
// or syntax_token_kind() and switches on these values.
//
// LIVES HERE because the parser is the canonical author. Future
// extraction to `src/ore/syntax_kind.h` is straightforward once the
// module boundary stabilizes.
//
// COLLAPSE NOTE
// =============
// Where the OLD AstNodeKind had 20+ variants for binary operators
// (BIN_ADD/SUB/MUL/...) the new model has ONE SK_BIN_EXPR node whose
// child includes an operator-token (SK_PLUS, SK_MINUS, ...). To recover
// the operator after the collapse, walk the node's children for the
// op-token and read its kind. Same pattern for assigns, unaries, and
// literals.

#include <stdbool.h>
#include <stdint.h>

#include "../syntax/syntax.h"  // SyntaxKind = uint16_t

typedef enum : uint16_t {
    // ---- 0: Sentinel ----------------------------------------------
    // Matches src/syntax/syntax.h's SYNTAX_KIND_NONE. Never assigned
    // to a real node or token.
    SK_NONE = 0,

    // ===============================================================
    // TOKEN KINDS (leaves)
    // ===============================================================

    // ---- Trivia + lex errors --------------------------------------
    SK_WHITESPACE,
    SK_NEWLINE,
    SK_COMMENT,
    SK_LEX_ERROR,
    SK_EOF,                     // end-of-stream sentinel (lexer/layout only;
                                // green tree has no EOF — vec end is implicit)

    // ---- Virtual layout tokens ------------------------------------
    // Emitted by layout.c per Koka-style indent rules. All have
    // text_len == 0 (zero-width green tokens). Distinct kinds from
    // their explicit `{` `}` `;` counterparts so formatter / syntax
    // highlighter can tell what the user typed vs what layout
    // inserted.
    SK_VIRTUAL_LBRACE,
    SK_VIRTUAL_RBRACE,
    SK_VIRTUAL_SEMI,

    // ---- Identifier + literal lexemes -----------------------------
    SK_IDENT,
    SK_INT_LIT,
    SK_FLOAT_LIT,
    SK_STRING_LIT,
    SK_BYTE_LIT,
    SK_ASM_LIT,

    // ---- Keyword literals -----------------------------------------
    SK_TRUE_KW,
    SK_FALSE_KW,
    SK_NIL_KW,

    // ---- Declaration keywords -------------------------------------
    SK_FN_KW,
    SK_FN_TYPE_KW,      // capital `Fn` — function-type form
    SK_CONST_KW,
    SK_STRUCT_KW,
    SK_ENUM_KW,
    SK_UNION_KW,
    SK_EFFECT_KW,
    SK_HANDLER_KW,
    SK_COMPTIME_KW,

    // ---- Control-flow keywords ------------------------------------
    SK_IF_KW,
    SK_ELIF_KW,
    SK_ELSE_KW,
    SK_LOOP_KW,
    SK_SWITCH_KW,
    SK_BREAK_KW,
    SK_CONTINUE_KW,
    SK_RETURN_KW,
    SK_DEFER_KW,
    SK_ORELSE_KW,

    // ---- Effects keywords -----------------------------------------
    SK_HANDLE_KW,
    SK_MASK_KW,
    SK_WITH_KW,

    // ---- Operators: logical ---------------------------------------
    SK_AMP_AMP,         // &&
    SK_PIPE_PIPE,       // ||
    SK_BANG,            // !

    // ---- Operators: arithmetic ------------------------------------
    SK_PLUS,
    SK_MINUS,
    SK_STAR,
    SK_STAR_STAR,       // **
    SK_SLASH,
    SK_PERCENT,

    // ---- Operators: bitwise ---------------------------------------
    SK_PIPE,            // |
    SK_AMP,             // &
    SK_CARET,           // ^
    SK_SHL,             // <<
    SK_SHR,             // >>

    // ---- Operators: relational ------------------------------------
    SK_EQ_EQ,           // ==
    SK_BANG_EQ,         // !=
    SK_LT,
    SK_LE,
    SK_GT,
    SK_GE,

    // ---- Operators: assignment ------------------------------------
    SK_EQ,
    SK_PLUS_EQ,
    SK_MINUS_EQ,
    SK_STAR_EQ,
    SK_SLASH_EQ,
    SK_PERCENT_EQ,
    SK_PIPE_EQ,
    SK_AMP_EQ,
    SK_TILDE_EQ,                // ~= (XOR-assign; tilde is the XOR token)
    SK_COLON_EQ,        // :=
    SK_PLUS_PLUS,       // ++ (postfix increment)
    SK_MINUS_MINUS,     // -- (postfix decrement)

    // ---- Operators: punctuation / arrows --------------------------
    SK_RARROW,          // ->
    SK_LARROW,          // <-
    SK_FATARROW,        // =>
    SK_COLON,
    SK_COLON_COLON,     // ::
    SK_DOT,
    SK_DOT_DOT,
    SK_DOT_DOT_DOT,
    SK_QUESTION,
    SK_UNDERSCORE,

    // ---- Delimiters -----------------------------------------------
    // EXPLICIT braces/semicolons/commas. Layout-emitted ones are the
    // SK_VIRTUAL_* variants above.
    SK_LPAREN,
    SK_RPAREN,
    SK_LBRACKET,
    SK_RBRACKET,
    SK_LBRACE,
    SK_RBRACE,
    SK_SEMI,
    SK_COMMA,
    SK_AT,
    SK_HASH,
    SK_TILDE,

    // Sentinel: one past the last TOKEN kind.
    SK_LAST_TOKEN_KIND,

    // ===============================================================
    // NODE KINDS (internal tree nodes)
    // ===============================================================
    // Numerically anchored at 256 so the token / node split is visible
    // at a glance during debugging. Plenty of headroom on both sides.
    SK_FIRST_NODE_KIND = 256,

    // ---- Root + error ---------------------------------------------
    SK_SOURCE_FILE,                 // the tree root
    SK_ERROR_NODE,                  // parser error recovery wrapper

    // ---- Top-level declarations -----------------------------------
    SK_FN_DECL,
    SK_STRUCT_DECL,
    SK_ENUM_DECL,
    SK_UNION_DECL,
    SK_EFFECT_DECL,
    SK_CONST_DECL,
    SK_VAR_DECL,
    SK_DESTRUCTURE_DECL,

    // ---- Structural sub-decls (children of decls / lists) ---------
    SK_PARAM,
    SK_FIELD,
    SK_VARIANT,
    SK_INIT_FIELD,

    // ---- List wrapper nodes ---------------------------------------
    // Replace the role the old extras-array played for variable-arity
    // content. Each holds the relevant delimiters (LPAREN/RPAREN,
    // LBRACE/RBRACE) AND the list members as direct children.
    SK_PARAM_LIST,
    SK_ARG_LIST,
    SK_FIELD_LIST,
    SK_VARIANT_LIST,
    SK_INIT_LIST,
    SK_STMT_LIST,

    // ---- Statements -----------------------------------------------
    SK_BLOCK_STMT,
    SK_RETURN_STMT,
    SK_IF_STMT,
    SK_LOOP_STMT,
    SK_SWITCH_STMT,
    SK_SWITCH_ARM,
    SK_BREAK_STMT,
    SK_CONTINUE_STMT,
    SK_DEFER_STMT,
    SK_EXPR_STMT,                   // explicit expression-as-statement wrapper

    // ---- Expressions (heavily collapsed from old AstNodeKind) -----
    SK_LITERAL_EXPR,                // collapsed: type via inner literal token
    SK_PATH_EXPR,
    SK_FIELD_EXPR,
    SK_REF_EXPR,                    // identifier reference (non-path)
    SK_PAREN_EXPR,                  // `(expr)` preserves user-typed parens
    SK_BIN_EXPR,                    // collapsed: op via op-token child
    SK_ASSIGN_EXPR,                 // collapsed: op via op-token child
    SK_PREFIX_EXPR,                 // -x / !x / ~x / &x / *x
    SK_POSTFIX_EXPR,                // x++ / x? / x!
    SK_CALL_EXPR,
    SK_INDEX_EXPR,
    SK_SLICE_EXPR,
    SK_BLOCK_EXPR,                  // block as expression form
    SK_IF_EXPR,                     // if as expression form
    SK_MATCH_EXPR,                  // switch as expression form
    SK_LOOP_EXPR,                   // loop as expression form
    SK_LAMBDA_EXPR,
    SK_HANDLE_EXPR,
    SK_HANDLER_EXPR,
    SK_MASK_EXPR,
    SK_PRODUCT_EXPR,                // .{...} or T{...} initializer
    SK_ENUM_REF_EXPR,               // .Variant
    SK_BUILTIN_EXPR,                // @name(args)

    // ---- Handler clauses (children of SK_HANDLER_EXPR / SK_HANDLE_EXPR) ----
    // A handler body holds one regular op clause per algebraic operation
    // (`name :: [pub] (fn|ctl|val|final ctl|raw ctl) (params) body`) plus
    // up to one each of the three lifecycle clauses (`return (params) {
    // body }`, `initially expr`, `finally expr`). Distinct kinds let
    // sema dispatch on the clause shape without re-reading the leading
    // keyword. The op-kind keyword (`fn`/`ctl`/`val`/`final ctl`/`raw
    // ctl`) is preserved as a child token of SK_OP_CLAUSE.
    SK_OP_CLAUSE,
    SK_RETURN_CLAUSE,
    SK_INITIALLY_CLAUSE,
    SK_FINALLY_CLAUSE,

    // ---- Patterns -------------------------------------------------
    // Aspirational: current Ore doesn't fully distinguish patterns
    // from expressions in destructure positions. Reserved for
    // post-Phase-A refinement when the parser starts emitting
    // separate pattern nodes.
    SK_BIND_PAT,
    SK_WILDCARD_PAT,
    SK_LITERAL_PAT,
    SK_TUPLE_PAT,
    SK_FIELD_PAT,

    // ---- Types ----------------------------------------------------
    SK_REF_TYPE,                    // bare T identifier as type
    SK_PATH_TYPE,                   // multi-segment T.U.V as type
    SK_PTR_TYPE,                    // ^T
    SK_SLICE_TYPE,                  // []T
    SK_ARRAY_TYPE,                  // [N]T
    SK_MANY_PTR_TYPE,               // [^]T
    SK_FN_TYPE,
    SK_OPTIONAL_TYPE,               // ?T
    SK_CONST_TYPE,                  // const T
    SK_EFFECT_ROW_TYPE,             // <H | e>

    // Sentinel: one past the last NODE kind.
    SK_LAST_NODE_KIND,
} OreSyntaxKind;


// =====================================================================
// Helpers
// =====================================================================
//
// `ore_syntax_kind_name(k)` returns a stable, NUL-terminated string
// for diagnostic / debug output. Defined for every value in
// [SK_NONE, SK_LAST_TOKEN_KIND) ∪ [SK_FIRST_NODE_KIND, SK_LAST_NODE_KIND).
// Returns "?" for out-of-range / unknown values (never NULL).
const char *ore_syntax_kind_name(OreSyntaxKind k);

// True iff `k` is in the TOKEN-kind range (1..SK_LAST_TOKEN_KIND).
bool ore_kind_is_token(OreSyntaxKind k);

// True iff `k` is in the NODE-kind range (SK_FIRST_NODE_KIND..SK_LAST_NODE_KIND).
bool ore_kind_is_node(OreSyntaxKind k);


// ---- Token-category classifiers ------------------------------------

// True for SK_WHITESPACE, SK_NEWLINE, SK_COMMENT.
bool ore_kind_is_trivia(OreSyntaxKind k);

// True for SK_VIRTUAL_LBRACE / RBRACE / SEMI.
bool ore_kind_is_virtual_layout(OreSyntaxKind k);

// True for any keyword (decl, control flow, effects, true/false/nil).
bool ore_kind_is_keyword(OreSyntaxKind k);

// True for the literal LEXEME tokens (int, float, string, byte, asm).
// IDENT is NOT a literal token. Use SK_TRUE_KW / FALSE_KW / NIL_KW for
// the keyword literals, which are technically keywords (caught by
// ore_kind_is_keyword).
bool ore_kind_is_literal_token(OreSyntaxKind k);

// True for any token that legally appears as the operator of a
// SK_BIN_EXPR (arithmetic, bitwise, relational, logical, plus the
// orelse keyword which is binary at the language level).
bool ore_kind_is_bin_op_token(OreSyntaxKind k);

// True for any token that is the operator of a SK_ASSIGN_EXPR
// (=, +=, -=, etc.; NOT == or != which are bin ops).
bool ore_kind_is_assign_op_token(OreSyntaxKind k);

// True for any token that is the operator of a SK_PREFIX_EXPR
// (-, !, ~, &, *).
bool ore_kind_is_prefix_op_token(OreSyntaxKind k);

// True for any token that is the operator of a SK_POSTFIX_EXPR
// (++, ?, ! when in postfix position).
bool ore_kind_is_postfix_op_token(OreSyntaxKind k);

// True for SK_LBRACE OR SK_VIRTUAL_LBRACE — useful in parser positions
// that accept either explicit or layout-inserted block openers.
bool ore_kind_is_open_brace(OreSyntaxKind k);

// True for SK_RBRACE OR SK_VIRTUAL_RBRACE.
bool ore_kind_is_close_brace(OreSyntaxKind k);

// True for SK_SEMI OR SK_VIRTUAL_SEMI.
bool ore_kind_is_stmt_sep(OreSyntaxKind k);


// ---- Node-category classifiers -------------------------------------

// True for any top-level declaration node (FN_DECL, STRUCT_DECL, ...).
bool ore_kind_is_decl_node(OreSyntaxKind k);

// True for any statement node.
bool ore_kind_is_stmt_node(OreSyntaxKind k);

// True for any expression node.
bool ore_kind_is_expr_node(OreSyntaxKind k);

// True for any pattern node.
bool ore_kind_is_pat_node(OreSyntaxKind k);

// True for any type node.
bool ore_kind_is_type_node(OreSyntaxKind k);

// True for any list-wrapper node (PARAM_LIST, ARG_LIST, etc.).
bool ore_kind_is_list_node(OreSyntaxKind k);

#endif  // ORE_SYNTAX_KIND_H
