#ifndef ORE_PARSER_H
#define ORE_PARSER_H

#include <stdint.h>
#include <stdbool.h>

#include "../db/storage/vec.h"
#include "../db/db.h"
#include "../lexer/token.h"
#include "./ast.h"

// -----------------------------------------------------------------------------
// Core Parser State
//
// The parser is the body of QUERY_MODULE_AST. It holds the db handle +
// the module being parsed and writes its outputs directly into that
// module's db storage (no ModuleInfo staging struct). Discipline: a
// parse must NOT call other db_query_* (no cross-query reads from inside
// a query body); it only writes its own module's columns + diagnostics.
// -----------------------------------------------------------------------------
typedef struct {
    struct db  *s;          // db handle (for s->names, request_arena, diag)
    ModuleId    mid;        // module being parsed
    FileId      file;       // its backing file (for span file_id stamping)
    const Vec  *tokens;     // Vec<Token> (layout-normalized real tokens)
    uint32_t    pos;        // current token index

    // Parser outputs. ast lives in db.modules.arenas[mid]; span_map is a
    // transient build buffer in request_arena (flattened into the
    // ModuleNodeData block afterward); top_level_index / node_to_decl
    // live in arenas[mid] and become the db columns directly.
    ASTStore   *ast;
    Vec         span_map;        // Vec<TinySpan>
    Vec         top_level_index; // Vec<TopLevelEntry>
    Vec         node_to_decl;    // Vec<DefId>

    // Reusable child-collection stack (Vec<uint32_t>, request_arena).
    // Variable-length nodes record start = scratch.count, vec_push each
    // child, ast_push_extra(slice), then reset scratch.count = start.
    // No fixed caps; one amortized allocation per parse.
    Vec         scratch;

    // parsing_type: in type position (RHS of `:`, after `->`, inside
    // `Fn(...)`, `[N]`). Disables value-only forms (`[_]T{...}`,
    // `T{...}` literal initializers). Save/restore across recursion.
    bool        parsing_type;
} Parser;

// Core driver — the QUERY_MODULE_AST body. Writes the module's ASTStore
// (in db.modules.arenas[mid]), span_map / top_level_index / node_to_decl,
// and flattens the ModuleNodeData block into the per-module arena. The
// query body (db_query_module_ast) owns lex/layout, the per-module
// arena_reset, fingerprinting, and db_query_succeed.
void parse_module(struct db *s, ModuleId mid, const Vec *tokens);

// -----------------------------------------------------------------------------
// Internal Cursor Primitives (used by parse_expr, parse_decl, parse_stmt)
// -----------------------------------------------------------------------------

bool p_is_eof(const Parser *p);
TokenKind p_peek(const Parser *p);
TokenKind p_peek_at(const Parser *p, uint32_t offset);
const Token* p_current(const Parser *p);
const Token* p_advance(Parser *p);
bool p_match(Parser *p, TokenKind kind);

// If the current token matches `kind`, advances and returns it.
// Otherwise emits a diagnostic and returns NULL.
const Token* p_consume(Parser *p, TokenKind kind, const char *err_msg);

// Emit a parser diagnostic at the current token (db_diag_error, slot-keyed).
void p_error(Parser *p, const char *msg);

// -----------------------------------------------------------------------------
// Internal Node Construction
// -----------------------------------------------------------------------------

// TinySpan spanning [start_tok.start, end_tok.byte_end) in this module's file.
static inline TinySpan p_span(const Parser *p, const Token *start_tok, const Token *end_tok) {
    return span_make_range((uint16_t)p->file.idx, start_tok->start, end_tok->byte_end);
}

// Push a node to the module's ASTStore and record its span in lockstep.
AstNodeId p_push_node(Parser *p, AstNodeKind kind, uint32_t main_token, AstNodeData data, TinySpan span);

// -----------------------------------------------------------------------------
// Scratch stack — unbounded variable-length child collection.
//
// Replaces fixed `uint32_t buf[N]` + `if (count<N)` (silent truncation).
// One reusable Vec<uint32_t> in request_arena, used LIFO: open a region,
// push child idxs, emit them as one extras run, pop. Nested parses are
// fine — a child fully completes (its own region opened+closed) before
// the parent pushes that child's id, so regions strictly nest.
// -----------------------------------------------------------------------------

static inline uint32_t scratch_open(Parser *p) {
    return (uint32_t)p->scratch.count;
}

static inline void scratch_push(Parser *p, uint32_t v) {
    vec_push(&p->scratch, &v);
}

// Reserve a slot now (e.g. a count prefix); returns its scratch index so
// the caller can backpatch it via scratch_set before emitting.
static inline uint32_t scratch_reserve(Parser *p) {
    uint32_t at = (uint32_t)p->scratch.count;
    uint32_t zero = 0;
    vec_push(&p->scratch, &zero);
    return at;
}

static inline void scratch_set(Parser *p, uint32_t at, uint32_t v) {
    ((uint32_t*)p->scratch.data)[at] = v;
}

// Emit [start .. scratch.count) as one extras run, then pop the region.
static inline AstExtraDataIdx scratch_emit(Parser *p, uint32_t start) {
    AstExtraDataIdx e = ast_push_extra(p->ast,
                                       (uint32_t*)p->scratch.data + start,
                                       (uint32_t)p->scratch.count - start);
    p->scratch.count = start;
    return e;
}

#endif // ORE_PARSER_H
