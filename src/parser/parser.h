#ifndef PARSER_H
#define PARSER_H

#include "../lexer/token.h"
#include "../common/vec.h"
#include "../common/stringpool.h"
#include "../common/arena.h"
#include "../diag/diag.h"
#include "./ast.h"


struct Parser {
    // --- Inputs ---
    Vec* tokens;              // laid-out token stream
    size_t current;           // current position
    StringPool* pool;         // for looking up lexemes
    
    // --- Outputs (The Database) ---
    ASTStore* ast;            // REPLACES `Arena* arena`. The parser only pushes to this.
    struct DiagBag* diags;    // optional parser diagnostics sink

    // --- State & Context ---
    bool had_error;
    bool parsing_type;        // whether we're parsing a type
    
    // > 0 while parsing the body of a `handler { ... }` or `handle (t) { ... }`
    int in_handler_block_depth;
    
    // True when trailing-lambda postfix (`f { block }`) is allowed
    bool allow_trailing_lam;

    // Pre-interned string IDs for keywords/names on the hot path.
    struct {
        StrId initially;
        StrId finally;
        StrId scope;
        StrId behind;
    } interned;
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