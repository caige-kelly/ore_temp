#ifndef PARSER_H
#define PARSER_H

#include "../lexer/token.h"
#include "../common/vec.h"
#include "../common/stringpool.h"
#include "../common/arena.h"
#include "../diag/diag.h"
#include "./ast.h"


struct Parser {
    Vec* tokens; // laid-out token stream
    size_t current; // current position
    StringPool* pool; // for looking up lexemes
    Arena* arena; // owns all AST nodes
    struct DiagBag* diags; // optional parser diagnostics sink
    bool had_error;
    bool parsing_type; // whether we're parsing a type
    // > 0 while parsing the body of a `handler { ... }` or `handle (t) { ... }`
    // — gates `initially`/`finally` keywords from appearing at the head of
    // a stmt outside that context.
    int in_handler_block_depth;
    // True when trailing-lambda postfix (`f { block }` and `f fn(...) body`)
    // is allowed in the current expression context. Disabled inside
    // contexts that do their own body consumption (e.g., `with caller body`)
    // to avoid double-consuming the body block.
    bool allow_trailing_lam;
    // Pre-interned string IDs for keywords/names looked up on the hot path.
    // Set once in parser_new_in_with_diags so the parser doesn't
    // re-intern these every time it inspects a Bind name in a handler
    // block or checks a parameter type for "Scope".
    struct {
        StrId initially;
        StrId finally;
        StrId scope;
        StrId behind;
    } interned;
    // Per-parse NodeId counter. Starts at 1 each parse — local
    // counter values 1..N stay stable across re-parses of the same
    // input so invalidation slots remain findable. Emitted NodeIds
    // are this counter OR'd with the file_id in the high bits (see
    // file_id_shifted below), so the same local value in two
    // different inputs lands in distinct NodeId-space regions.
    uint32_t next_local_id;
    // Precomputed `(file_id << NODE_ID_FILE_SHIFT)` so alloc_expr
    // does one OR instead of a shift per node.
    uint32_t file_id_shifted;
};

// Initialize a parser. The compiler always supplies an arena and diag bag;
// the no-arena and no-diags overloads were dead and the no-arena one
// leaked the Arena it allocated. Removed.
//
// `file_id` partitions NodeId space — every InputId emits NodeIds
// disjoint from every other input, so per-NodeId sema caches don't
// collide across modules within one Sema instance.
struct Parser parser_new_in_with_diags(Vec* tokens, StringPool* pool, Arena* arena,
                                       struct DiagBag* diags, int file_id);

// Parse the full token stream into a list
Vec* parse(struct Parser* p);

void print_ast(struct Expr* expr, StringPool* pool, int indent);


#endif // PARSER_H