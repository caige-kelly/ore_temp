#include "parser.h"

// -----------------------------------------------------------------------------
// Cursor Primitives
// -----------------------------------------------------------------------------

bool p_is_eof(const Parser *p) {
    return p_peek(p) == TK_EOF;
}

const Token* p_current(const Parser *p) {
    if (p->pos >= p->tokens->count) {
        if (p->tokens->count == 0) return NULL; // Safety fallback
        // Return the last token (which should be TK_EOF)
        return vec_get((Vec*)p->tokens, p->tokens->count - 1);
    }
    return vec_get((Vec*)p->tokens, p->pos);
}

TokenKind p_peek(const Parser *p) {
    const Token *t = p_current(p);
    return t ? t->kind : TK_EOF;
}

TokenKind p_peek_at(const Parser *p, uint32_t offset) {
    uint32_t idx = p->pos + offset;
    if (idx >= p->tokens->count) {
        if (p->tokens->count == 0) return TK_EOF;
        const Token *last = vec_get((Vec*)p->tokens, p->tokens->count - 1);
        return last->kind;
    }
    const Token *t = vec_get((Vec*)p->tokens, idx);
    return t->kind;
}

const Token* p_advance(Parser *p) {
    const Token *t = p_current(p);
    if (!p_is_eof(p)) {
        p->pos++;
    }
    return t;
}

bool p_match(Parser *p, TokenKind kind) {
    if (p_peek(p) == kind) {
        p_advance(p);
        return true;
    }
    return false;
}

const Token* p_consume(Parser *p, TokenKind kind, const char *err_msg) {
    if (p_peek(p) == kind) {
        return p_advance(p);
    }
    p_error(p, err_msg);
    return NULL;
}

void p_error(Parser *p, const char *msg) {
    (void)p;
    (void)msg;
    // TODO: Wire up diag_emit using p->diags and p_current(p)->start
}

// -----------------------------------------------------------------------------
// Node Construction
// -----------------------------------------------------------------------------

AstNodeId p_push_node(Parser *p, AstNodeKind kind, uint32_t main_token, AstNodeData data, TinySpan span) {
    // 1. Push to durable AST store
    AstNodeId id = ast_push_node(p->mod->ast, kind, main_token, data);
    
    // 2. Push to volatile span map (indices perfectly align).
    //    Defensive: stamp file_id in case the caller built `span` without it.
    span = span_with_file(span, (uint16_t)p->mod->file.idx);
    vec_push(&p->mod->span_map, &span);
    
    return id;
}

// -----------------------------------------------------------------------------
// Core Driver
// -----------------------------------------------------------------------------

void parse_module(struct ModuleInfo *mod, const Vec *tokens, struct DiagBag *diags) {
    // We cap the vectors at the total number of real tokens.
    // In reality, AST nodes <= token count.
    size_t max_nodes = tokens->count;
    if (max_nodes == 0) max_nodes = 1;
    
    // 1. Initialize the ASTStore in the module's arena
    ast_store_init(mod->ast, &mod->arena, max_nodes);
    
    // 2. Initialize the volatile side-tables that parallel the ASTStore
    vec_init_in_arena(&mod->span_map, &mod->arena, max_nodes, sizeof(TinySpan));
    vec_init_in_arena(&mod->top_level_index, &mod->arena, 32, sizeof(TopLevelEntry));
    vec_init_in_arena(&mod->node_to_decl, &mod->arena, max_nodes, sizeof(DefId));
    
    // Push Sentinel Span for Node 0 to keep indices perfectly aligned
    TinySpan dummy_span = {0};
    vec_push(&mod->span_map, &dummy_span);
    
    DefId dummy_def = {0};
    vec_push(&mod->node_to_decl, &dummy_def);

    // 3. Init parser state
    Parser p = {
        .mod = mod,
        .tokens = tokens,
        .pos = 0,
        .diags = diags,
    };
    
    // Dispatch to the top-level declaration parser
    extern void parse_top_level_decls(Parser *p);
    parse_top_level_decls(&p);
}
