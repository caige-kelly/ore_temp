#include "parse_stmt.h"
#include "parse_expr.h"

AstNodeId parse_block(Parser *p) {
    const Token *start_tok = p_consume(p, TK_LBRACE, "Expected '{' to start block");
    if (!start_tok) return AST_NODE_ID_NONE;
    uint32_t op_index = p->pos - 1;
    
    uint32_t stmts[1024];
    uint32_t stmt_count = 0;
    
    while (!p_is_eof(p) && p_peek(p) != TK_RBRACE) {
        AstNodeId stmt = parse_stmt(p);
        if (stmt.idx != 0 && stmt_count < 1024) {
            stmts[stmt_count++] = stmt.idx;
        }
        
        // Recover if we fail to parse a statement and don't advance
        if (stmt.idx == 0) {
            if (p_peek(p) == TK_RBRACE) break;
            p_advance(p); 
        }
    }
    
    const Token *end_tok = p_consume(p, TK_RBRACE, "Expected '}' to end block");
    if (!end_tok) return AST_NODE_ID_NONE;
    
    // Pack: [count, stmt0, stmt1, ...]
    uint32_t extra_payload[1025];
    extra_payload[0] = stmt_count;
    for (uint32_t i = 0; i < stmt_count; i++) {
        extra_payload[i+1] = stmts[i];
    }
    
    AstExtraDataIdx extra = ast_push_extra(p->mod->ast, extra_payload, stmt_count + 1);
    AstNodeData data = {0};
    data.extra_idx = extra;
    
    return p_push_node(p, AST_STMT_BLOCK, op_index, data, p_span(p, start_tok, end_tok));
}

AstNodeId parse_stmt(Parser *p) {
    TokenKind kind = p_peek(p);
    
    switch (kind) {
    case TK_LBRACE:
        return parse_block(p);
        
    case TK_RETURN: {
        const Token *start_tok = p_advance(p);
        uint32_t op_index = p->pos - 1;
        AstNodeId expr = AST_NODE_ID_NONE;
        
        if (p_peek(p) != TK_SEMI && p_peek(p) != TK_RBRACE && !p_is_eof(p)) {
            expr = parse_expr(p, 0);
        }
        
        const Token *end_tok = start_tok;
        if (p_match(p, TK_SEMI)) {
            end_tok = vec_get((Vec*)p->tokens, p->pos - 1);
        } else if (expr.idx != 0) {
            TinySpan expr_span = *(TinySpan*)vec_get(&p->mod->span_map, expr.idx);
            TinySpan full_span = {
                .file_id = p->mod->file.idx,
            .start = start_tok->start,
            .length = (expr_span.start + expr_span.length) - (start_tok->start),
            };
            AstNodeData data = {0};
            data.single_child = expr;
            return p_push_node(p, AST_STMT_RETURN, op_index, data, full_span);
        }
        
        AstNodeData data = {0};
        data.single_child = expr;
        return p_push_node(p, AST_STMT_RETURN, op_index, data, p_span(p, start_tok, end_tok));
    }
    
    case TK_DEFER: {
        const Token *start_tok = p_advance(p);
        uint32_t op_index = p->pos - 1;
        AstNodeId stmt = parse_stmt(p);
        if (stmt.idx == 0) return AST_NODE_ID_NONE;
        
        AstNodeData data = {0};
        data.single_child = stmt;
        
        TinySpan stmt_span = *(TinySpan*)vec_get(&p->mod->span_map, stmt.idx);
        TinySpan full_span = {
            .file_id = p->mod->file.idx,
            .start = start_tok->start,
            .length = (stmt_span.start + stmt_span.length) - (start_tok->start),
        };
        return p_push_node(p, AST_STMT_DEFER, op_index, data, full_span);
    }
    
    case TK_BREAK:
    case TK_CONTINUE: {
        const Token *start_tok = p_advance(p);
        uint32_t op_index = p->pos - 1;
        
        const Token *end_tok = start_tok;
        if (p_match(p, TK_SEMI)) {
            end_tok = vec_get((Vec*)p->tokens, p->pos - 1);
        }
        
        AstNodeData data = {0};
        AstNodeKind ast_kind = (kind == TK_BREAK) ? AST_STMT_BREAK : AST_STMT_CONTINUE;
        return p_push_node(p, ast_kind, op_index, data, p_span(p, start_tok, end_tok));
    }
    
    case TK_IF: {
        const Token *start_tok = p_advance(p);
        uint32_t op_index = p->pos - 1;
        
        AstNodeId cond = parse_expr(p, 0);
        AstNodeId then_block = parse_block(p);
        AstNodeId else_block = AST_NODE_ID_NONE;
        
        const Token *end_tok = vec_get((Vec*)p->tokens, p->pos - 1);
        
        if (p_match(p, TK_ELSE)) {
            if (p_peek(p) == TK_IF) {
                else_block = parse_stmt(p);
            } else {
                else_block = parse_block(p);
            }
            end_tok = vec_get((Vec*)p->tokens, p->pos - 1);
        }
        
        // Pack: [cond, then, else]
        uint32_t extra_payload[3] = { cond.idx, then_block.idx, else_block.idx };
        AstExtraDataIdx extra = ast_push_extra(p->mod->ast, extra_payload, 3);
        AstNodeData data = {0};
        data.extra_idx = extra;
        
        return p_push_node(p, AST_STMT_IF, op_index, data, p_span(p, start_tok, end_tok));
    }
    
    case TK_LOOP: {
        const Token *start_tok = p_advance(p);
        uint32_t op_index = p->pos - 1;
        
        AstNodeId cond = AST_NODE_ID_NONE;
        if (p_peek(p) != TK_LBRACE) {
            cond = parse_expr(p, 0);
        }
        
        AstNodeId body = parse_block(p);
        const Token *end_tok = vec_get((Vec*)p->tokens, p->pos - 1);
        
        // Pack: [cond, body]
        uint32_t extra_payload[2] = { cond.idx, body.idx };
        AstExtraDataIdx extra = ast_push_extra(p->mod->ast, extra_payload, 2);
        AstNodeData data = {0};
        data.extra_idx = extra;
        
        return p_push_node(p, AST_STMT_LOOP, op_index, data, p_span(p, start_tok, end_tok));
    }

    default: {
        AstNodeId expr = parse_expr(p, 0);
        if (expr.idx == 0) {
            // Panic recovery: skip the bad token so we don't infinitely loop
            p_advance(p);
            return AST_NODE_ID_NONE;
        }
        
        const Token *end_tok = vec_get((Vec*)p->tokens, p->pos - 1);
        
        if (p_match(p, TK_SEMI)) {
            end_tok = vec_get((Vec*)p->tokens, p->pos - 1);
        }
        
        AstNodeData data = {0};
        data.single_child = expr;
        
        TinySpan expr_span = *(TinySpan*)vec_get(&p->mod->span_map, expr.idx);
        TinySpan full_span = {
            .file_id = p->mod->file.idx,
            .start = expr_span.start,
            .length = (end_tok->byte_end) - (expr_span.start),
        };
        
        uint32_t expr_op_idx = *(uint32_t*)vec_get(&p->mod->ast->main_tokens, expr.idx);
        return p_push_node(p, AST_STMT_EXPR, expr_op_idx, data, full_span);
    }
    }
}
