#include "parse_expr.h"

// -----------------------------------------------------------------------------
// Precedence Table
// -----------------------------------------------------------------------------

static int get_infix_precedence(TokenKind kind) {
    switch (kind) {
    case TK_EQ: case TK_PLUS_EQ: case TK_MINUS_EQ: case TK_STAR_EQ: case TK_SLASH_EQ: 
    case TK_PERCENT_EQ: case TK_AMP_EQ: case TK_PIPE_EQ: case TK_CARET_EQ:
        return 1; // Assignment
    case TK_PIPE_PIPE: return 2; // Logical OR
    case TK_AMP_AMP: return 3;   // Logical AND
    case TK_PIPE: return 4;      // Bitwise OR
    case TK_CARET: return 5;     // Bitwise XOR
    case TK_AMP: return 6;       // Bitwise AND
    case TK_EQ_EQ: case TK_BANG_EQ: return 7; // Equality
    case TK_LT: case TK_GT: case TK_LE: case TK_GE: return 8; // Comparison
    case TK_SHL: case TK_SHR: return 9; // Shift
    case TK_PLUS: case TK_MINUS: return 10; // Term
    case TK_STAR: case TK_SLASH: case TK_PERCENT: return 11; // Factor
    case TK_STAR_STAR: return 12; // Power
    case TK_LPAREN: case TK_DOT: case TK_LBRACKET: return 14; // Call/Field/Index
    default: return 0;
    }
}

static AstNodeKind get_binary_op_kind(TokenKind kind) {
    switch (kind) {
    case TK_PLUS: return AST_EXPR_BIN_ADD;
    case TK_MINUS: return AST_EXPR_BIN_SUB;
    case TK_STAR: return AST_EXPR_BIN_MUL;
    case TK_SLASH: return AST_EXPR_BIN_DIV;
    case TK_PERCENT: return AST_EXPR_BIN_MOD;
    case TK_EQ_EQ: return AST_EXPR_BIN_EQ;
    case TK_BANG_EQ: return AST_EXPR_BIN_NEQ;
    case TK_LT: return AST_EXPR_BIN_LT;
    case TK_LE: return AST_EXPR_BIN_LE;
    case TK_GT: return AST_EXPR_BIN_GT;
    case TK_GE: return AST_EXPR_BIN_GE;
    case TK_AMP_AMP: return AST_EXPR_BIN_AND;
    case TK_PIPE_PIPE: return AST_EXPR_BIN_OR;
    case TK_AMP: return AST_EXPR_BIN_BIT_AND;
    case TK_PIPE: return AST_EXPR_BIN_BIT_OR;
    case TK_CARET: return AST_EXPR_BIN_BIT_XOR;
    case TK_SHL: return AST_EXPR_BIN_SHL;
    case TK_SHR: return AST_EXPR_BIN_SHR;
    case TK_EQ: return AST_EXPR_ASSIGN;
    case TK_PLUS_EQ: return AST_EXPR_ASSIGN_ADD;
    case TK_MINUS_EQ: return AST_EXPR_ASSIGN_SUB;
    case TK_STAR_EQ: return AST_EXPR_ASSIGN_MUL;
    case TK_SLASH_EQ: return AST_EXPR_ASSIGN_DIV;
    case TK_PERCENT_EQ: return AST_EXPR_ASSIGN_MOD;
    case TK_AMP_EQ: return AST_EXPR_ASSIGN_BIT_AND;
    case TK_PIPE_EQ: return AST_EXPR_ASSIGN_BIT_OR;
    case TK_CARET_EQ: return AST_EXPR_ASSIGN_BIT_XOR;
    default: return AST_ERROR;
    }
}

// -----------------------------------------------------------------------------
// Pratt Parsing
// -----------------------------------------------------------------------------

static AstNodeId parse_prefix(Parser *p) {
    const Token *start_tok = p_current(p);
    TokenKind kind = start_tok->kind;
    uint32_t op_index = p->pos;
    
    switch (kind) {
    case TK_INT_LIT:
    case TK_FLOAT_LIT: {
        p_advance(p);
        AstNodeData data = {0};
        // We pack the StrId into the payload. The evaluator will parse the string to a raw value.
        data.string_id = start_tok->string_id; 
        AstNodeKind ast_k = (kind == TK_INT_LIT) ? AST_EXPR_LIT_INT : AST_EXPR_LIT_FLOAT;
        return p_push_node(p, ast_k, op_index, data, p_span(p, start_tok, start_tok));
    }
    case TK_STRING_LIT: {
        p_advance(p);
        AstNodeData data = {0};
        data.string_id = start_tok->string_id;
        return p_push_node(p, AST_EXPR_LIT_STRING, op_index, data, p_span(p, start_tok, start_tok));
    }
    case TK_TRUE:
    case TK_FALSE: {
        p_advance(p);
        AstNodeData data = {0};
        data.bool_val = (kind == TK_TRUE);
        return p_push_node(p, AST_EXPR_LIT_BOOL, op_index, data, p_span(p, start_tok, start_tok));
    }
    case TK_NIL: {
        p_advance(p);
        AstNodeData data = {0};
        return p_push_node(p, AST_EXPR_LIT_NIL, op_index, data, p_span(p, start_tok, start_tok));
    }
    case TK_IDENTIFIER: {
        p_advance(p);
        AstNodeData data = {0};
        data.string_id = start_tok->string_id;
        return p_push_node(p, AST_EXPR_PATH, op_index, data, p_span(p, start_tok, start_tok));
    }
    case TK_MINUS:
    case TK_BANG:
    case TK_TILDE:
    case TK_AMP:
    case TK_STAR: {
        p_advance(p);
        AstNodeId operand = parse_expr(p, 13); // Unary precedence is very high
        if (operand.idx == 0) return AST_NODE_ID_NONE;
        
        AstNodeData data = {0};
        data.single_child = operand;
        
        AstNodeKind ast_kind = AST_ERROR;
        if (kind == TK_MINUS) ast_kind = AST_EXPR_UNARY_NEG;
        else if (kind == TK_BANG) ast_kind = AST_EXPR_UNARY_NOT;
        else if (kind == TK_TILDE) ast_kind = AST_EXPR_UNARY_BIT_NOT;
        else if (kind == TK_AMP) ast_kind = AST_EXPR_UNARY_REF;
        else if (kind == TK_STAR) ast_kind = AST_EXPR_UNARY_DEREF;
        
        TinySpan op_span = *(TinySpan*)vec_get(&p->mod->span_map, operand.idx);
        TinySpan full_span = {
            .file_id = p->mod->file.idx,
            .start = start_tok->start,
            .length = (op_span.start + op_span.length) - (start_tok->start),
        };
        return p_push_node(p, ast_kind, op_index, data, full_span);
    }
    case TK_LPAREN: {
        p_advance(p);
        AstNodeId inner = parse_expr(p, 0);
        const Token *end_tok = p_consume(p, TK_RPAREN, "Expected ')' after expression");
        if (!end_tok) return AST_NODE_ID_NONE;
        
        AstNodeData data = {0};
        data.single_child = inner;
        return p_push_node(p, AST_EXPR_GROUP, op_index, data, p_span(p, start_tok, end_tok));
    }
    default:
        p_error(p, "Expected expression");
        return AST_NODE_ID_NONE;
    }
}

static AstNodeId parse_infix(Parser *p, AstNodeId left, TinySpan left_span) {
    TokenKind kind = p_peek(p);
    uint32_t op_index = p->pos;
    p_advance(p);
    
    // Function Call
    if (kind == TK_LPAREN) { 
        uint32_t args[256];
        uint32_t arg_count = 0;
        
        while (!p_is_eof(p) && p_peek(p) != TK_RPAREN) {
            AstNodeId arg = parse_expr(p, 0);
            if (arg_count < 256) {
                args[arg_count++] = arg.idx;
            }
            if (!p_match(p, TK_COMMA)) break;
        }
        
        const Token *end_tok = p_consume(p, TK_RPAREN, "Expected ')' after arguments");
        if (!end_tok) return left;
        
        // Pack: [callee, arg_count, arg0, arg1, ...]
        uint32_t extra_payload[258];
        extra_payload[0] = left.idx;
        extra_payload[1] = arg_count;
        for (uint32_t i = 0; i < arg_count; i++) {
            extra_payload[i+2] = args[i];
        }
        
        AstExtraDataIdx extra_ref = ast_push_extra(p->mod->ast, extra_payload, arg_count + 2);
        AstNodeData data = {0};
        data.extra_idx = extra_ref;
        
        TinySpan full_span = {
            .file_id = p->mod->file.idx,
            .start = left_span.start,
            .length = (end_tok->byte_end) - (left_span.start),
        };
        return p_push_node(p, AST_EXPR_CALL, op_index, data, full_span);
    }
    
    // Normal Binary
    int prec = get_infix_precedence(kind);
    int next_prec = prec; 
    // Assignments are right-associative
    if (kind >= TK_EQ && kind <= TK_CARET_EQ) {
        next_prec = prec - 1; 
    }
    
    AstNodeId right = parse_expr(p, next_prec);
    if (right.idx == 0) return left; // Graceful degradation
    
    AstNodeData data = {0};
    data.bin.lhs = left;
    data.bin.rhs = right;
    
    AstNodeKind ast_kind = get_binary_op_kind(kind);
    
    TinySpan right_span = *(TinySpan*)vec_get(&p->mod->span_map, right.idx);
    TinySpan full_span = {
        .file_id = p->mod->file.idx,
            .start = left_span.start,
            .length = (right_span.start + right_span.length) - (left_span.start),
    };
    
    return p_push_node(p, ast_kind, op_index, data, full_span);
}

AstNodeId parse_expr(Parser *p, int precedence) {
    AstNodeId left = parse_prefix(p);
    if (left.idx == 0) return left;
    
    TinySpan left_span = *(TinySpan*)vec_get(&p->mod->span_map, left.idx);
    
    while (precedence < get_infix_precedence(p_peek(p))) {
        left = parse_infix(p, left, left_span);
        left_span = *(TinySpan*)vec_get(&p->mod->span_map, left.idx);
    }
    
    return left;
}
