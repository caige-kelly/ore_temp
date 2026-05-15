#include "parse_decl.h"
#include "ast.h"
#include "parse_expr.h"
#include "parse_stmt.h"

AstNodeId parse_type(Parser *p) {
    const Token *start_tok = p_current(p);
    uint32_t op_index = p->pos;
    TokenKind kind = p_peek(p);
    
    if (kind == TK_IDENTIFIER) {
        p_advance(p);
        AstNodeData data = {0};
        data.string_id = start_tok->string_id;
        return p_push_node(p, AST_TYPE_PATH, op_index, data, p_span(p, start_tok, start_tok));
    }
    
    if (kind == TK_STAR) {
        p_advance(p);
        AstNodeId inner = parse_type(p);
        AstNodeData data = {0};
        data.single_child = inner;
        TinySpan full_span = p_span(p, start_tok, vec_get((Vec*)p->tokens, p->pos - 1));
        return p_push_node(p, AST_TYPE_PTR, op_index, data, full_span);
    }
    
    if (kind == TK_LBRACKET) {
        p_advance(p);
        if (p_match(p, TK_RBRACKET)) {
            AstNodeId inner = parse_type(p);
            AstNodeData data = {0};
            data.single_child = inner;
            return p_push_node(p, AST_TYPE_SLICE, op_index, data, p_span(p, start_tok, vec_get((Vec*)p->tokens, p->pos - 1)));
        } else {
            AstNodeId expr = parse_expr(p, 0);
            p_consume(p, TK_RBRACKET, "Expected ']' after array size");
            AstNodeId inner = parse_type(p);
            
            uint32_t extra_payload[2] = { expr.idx, inner.idx };
            AstExtraDataIdx extra = ast_push_extra(p->mod->ast, extra_payload, 2);
            AstNodeData data = {0};
            data.extra_idx = extra;
            return p_push_node(p, AST_TYPE_ARRAY, op_index, data, p_span(p, start_tok, vec_get((Vec*)p->tokens, p->pos - 1)));
        }
    }
    
    if (kind == TK_FN_TYPE) {
        p_advance(p);
        p_consume(p, TK_LPAREN, "Expected '(' after Fn");
        while (!p_is_eof(p) && p_peek(p) != TK_RPAREN) {
            parse_type(p);
            if (!p_match(p, TK_COMMA)) break;
        }
        p_consume(p, TK_RPAREN, "Expected ')'");
        if (p_match(p, TK_RARROW)) {
            parse_type(p);
        }
        AstNodeData data = {0};
        return p_push_node(p, AST_TYPE_FN, op_index, data, p_span(p, start_tok, vec_get((Vec*)p->tokens, p->pos - 1)));
    }
    
    p_error(p, "Expected type");
    return AST_NODE_ID_NONE;
}

static AstNodeId parse_fn_decl(Parser *p, Visibility vis) {
    const Token *start_tok = p_consume(p, TK_FN, "Expected 'fn'");
    if (!start_tok) return AST_NODE_ID_NONE;
    uint32_t op_index = p->pos - 1;
    
    const Token *name_tok = p_consume(p, TK_IDENTIFIER, "Expected function name");
    AstNodeData name_data = {0};
    if (name_tok) name_data.string_id = name_tok->string_id;
    AstNodeId name_id = p_push_node(p, AST_EXPR_PATH, p->pos - 1, name_data, p_span(p, name_tok ? name_tok : start_tok, name_tok ? name_tok : start_tok));
    
    p_consume(p, TK_LPAREN, "Expected '(' after function name");
    
    uint32_t params[256];
    uint32_t param_count = 0;
    while (!p_is_eof(p) && p_peek(p) != TK_RPAREN) {
        const Token *p_name = p_consume(p, TK_IDENTIFIER, "Expected param name");
        AstNodeData pd = {0};
        if (p_name) pd.string_id = p_name->string_id;
        AstNodeId pid = p_push_node(p, AST_EXPR_PATH, p->pos - 1, pd, p_span(p, p_name ? p_name : p_current(p), p_name ? p_name : p_current(p)));
        
        p_consume(p, TK_COLON, "Expected ':'");
        AstNodeId ptype = parse_type(p);
        
        uint32_t ex[2] = { pid.idx, ptype.idx };
        AstExtraDataIdx extra = ast_push_extra(p->mod->ast, ex, 2);
        AstNodeData vdata = {0};
        vdata.extra_idx = extra;
        AstNodeId param_node = p_push_node(p, AST_DECL_VAL, op_index, vdata, p_span(p, p_name ? p_name : p_current(p), vec_get((Vec*)p->tokens, p->pos - 1)));
        
        if (param_node.idx != 0 && param_count < 256) params[param_count++] = param_node.idx;
        if (!p_match(p, TK_COMMA)) break;
    }
    p_consume(p, TK_RPAREN, "Expected ')' after parameters");
    
    AstNodeId ret_type = AST_NODE_ID_NONE;
    if (p_match(p, TK_RARROW)) { // TK_RARROW is '->'
        ret_type = parse_type(p);
    }
    
    AstNodeId body = AST_NODE_ID_NONE;
    if (p_peek(p) == TK_LBRACE) {
        body = parse_block(p);
    } else {
        p_match(p, TK_SEMI); // Forward decls end in semicolon
    }
    
    const Token *end_tok = vec_get((Vec*)p->tokens, p->pos - 1);
    
    // extra: [name, ret_type, body, param_count, param0, ...]
    uint32_t extra_payload[260];
    extra_payload[0] = name_id.idx;
    extra_payload[1] = ret_type.idx;
    extra_payload[2] = body.idx;
    extra_payload[3] = param_count;
    for (uint32_t i=0; i<param_count; i++) extra_payload[i+4] = params[i];
    
    AstExtraDataIdx extra = ast_push_extra(p->mod->ast, extra_payload, param_count + 4);
    AstNodeData data = {0};
    data.extra_idx = extra;
    
    AstNodeId fn_node = p_push_node(p, AST_DECL_FN, op_index, data, p_span(p, start_tok, end_tok));
    
    // // Record in top-level index if we have a name
    // if (name_tok) {
    //     TopLevelEntry entry = {
    //         .name = name_tok->string_id,
    //         .node = fn_node,
    //         .vis = vis,
    //         .ast_id = ast_id_compute(AST_DECL_FN, name_tok->string_id),
    //     };
    //     vec_push(&p->mod->top_level_index, &entry);
    // }
    
    return fn_node;
}

AstNodeId parse_decl(Parser *p) {
    const Token *name_tok = p_consume(p, TK_IDENTIFIER, "Expected identifier");
    if (!name_tok) return AST_NODE_ID_NONE;

    AstNodeData name_data = {0};
    name_data.string_id = name_tok->string_id;

    // 1. Determine the declaration style
    bool is_const = false;
    bool is_var   = false;
    AstNodeId type_id = AST_NODE_ID_NONE;

    if (p_match(p, TK_COLON_COLON)) {
        is_const = true;
    } else if (p_match(p, TK_COLON_EQ)) {
        is_var = true;
    } else if (p_match(p, TK_COLON)) {
        type_id = parse_type(p);
        // Expect an assignment '=' next if it's an explicit type declaration
        p_consume(p, TK_EQ, "Expected '=' after type specification");
    } else {
        p_error(p, "Expected '::', ':=', or ':' after identifier");
        return AST_NODE_ID_NONE;
    }

    // 2. Loop and gather modifiers into our DefMeta byte
    DefMeta meta = 0; 
    bool gathering_modifiers = true;

    while (gathering_modifiers) {
        TokenKind kind = p_peek(p);

        switch (kind) {
            case TK_PUB:
                p_advance(p);
                meta &= ~META_VIS_MASK; 
                meta |= VIS_PUBLIC;     
                break;
            case TK_ABSTRACT:
                p_advance(p);
                meta &= ~META_VIS_MASK; 
                meta |= VIS_INTERNAL;
                break;
            case TK_PVT:
                p_advance(p);
                meta &= ~META_VIS_MASK;
                break;
            case TK_COMPTIME:
                p_advance(p);
                meta |= META_COMPTIME;
                break;
            case TK_SCOPED:
                printf("scoped\n");
                p_advance(p);
                meta |= META_SCOPED;
                break;
            case TK_NAMED:
                p_advance(p);
                meta |= META_NAMED;
                break;
            case TK_LINEAR:
                p_advance(p);
                meta |= META_LINEAR;
                break;
            case TK_DISTINCT:
                p_advance(p);
                meta |= META_DISTINCT;
                break;
            default:
                // We hit something that isn't a modifier (like 'effect', 'fn', an expression, etc.)
                printf("something else\n");
                gathering_modifiers = false;
                break;
        }
    }

    // 3. Now that we have the full 'meta' byte, push your AST node!
    AstNodeId decl_id = AST_NODE_ID_NONE;
    
    if (is_const) {
        decl_id = p_push_node(p, AST_DECL_CONST, p->pos - 1, name_data, p_span(p, name_tok, name_tok));
    } else {
        decl_id = p_push_node(p, AST_DECL_VAR, p->pos - 1, name_data, p_span(p, name_tok, name_tok));
    }

    // Record in top-level index if we have a name
    if (name_tok) {
        TopLevelEntry entry = {
            .name = name_tok->string_id,
            .node = decl_id,
            .meta = meta,
            .ast_id = ast_id_compute((is_const ? AST_DECL_CONST : AST_DECL_VAR), name_tok->string_id),
        };
        vec_push(&p->mod->top_level_index, &entry);
    }
    p_advance(p);
    p_advance(p);
    return decl_id;
}

void parse_top_level_decls(Parser *p) {
    uint32_t decls[1024];
    uint32_t decl_count = 0;
    
    while (!p_is_eof(p)) {
        AstNodeId decl = parse_decl(p);
        if (decl.idx != 0 && decl_count < 1024) {
            decls[decl_count++] = decl.idx;
        }
    }
    
    uint32_t extra_payload[1025];
    extra_payload[0] = decl_count;
    for (uint32_t i = 0; i < decl_count; i++) {
        extra_payload[i+1] = decls[i];
    }
    
    AstExtraDataIdx extra = ast_push_extra(p->mod->ast, extra_payload, decl_count + 1);
    AstNodeData data = {0};
    data.extra_idx = extra;
    
    const Token *first = p->tokens->count > 0 ? vec_get((Vec*)p->tokens, 0) : NULL;
    const Token *last = p->tokens->count > 0 ? vec_get((Vec*)p->tokens, p->tokens->count - 1) : NULL;
    TinySpan span = TINYSPAN_NONE;
    if (first && last) span = p_span(p, first, last);
    
    p_push_node(p, AST_DECL_MODULE, 0, data, span);
}
