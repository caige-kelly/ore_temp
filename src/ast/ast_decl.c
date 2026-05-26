#include "./ast_decl.h"


// ---- Cast helpers ---------------------------------------------------
//
// Every cast tests the kind and stores the borrowed pointer on success.
// The pointer is const-stripped — wrappers expose a NON-const SyntaxNode*
// internally so accessors can call non-const navigation functions
// (which need to bump the parent's refcount).

#define DEFINE_CAST(Type, Kind)                                              \
    bool Type##_cast(const SyntaxNode *n, Type *out) {                       \
        if (!n || syntax_node_kind(n) != (Kind)) return false;               \
        out->syntax = (SyntaxNode *)n;                                       \
        return true;                                                         \
    }

DEFINE_CAST(FnDef,     SK_FN_DECL)
DEFINE_CAST(StructDef, SK_STRUCT_DECL)
DEFINE_CAST(EnumDef,   SK_ENUM_DECL)
DEFINE_CAST(UnionDef,  SK_UNION_DECL)
DEFINE_CAST(EffectDef, SK_EFFECT_DECL)
DEFINE_CAST(ConstDef,  SK_CONST_DECL)
DEFINE_CAST(VarDef,    SK_VAR_DECL)
DEFINE_CAST(ImportDef, SK_IMPORT_DECL)
DEFINE_CAST(Param,     SK_PARAM)
DEFINE_CAST(Field,     SK_FIELD)
DEFINE_CAST(Variant,   SK_VARIANT)


// ---- Predicates used by ast_first_*_pred ----------------------------

static bool is_type_node(OreSyntaxKind k) { return ore_kind_is_type_node(k); }
static bool is_expr_node(OreSyntaxKind k) { return ore_kind_is_expr_node(k); }


// ---- FnDef ----------------------------------------------------------

SyntaxToken *FnDef_name(const FnDef *f) {
    return ast_first_token(f->syntax, SK_IDENT);
}

SyntaxNode *FnDef_params(const FnDef *f) {
    return ast_first_child(f->syntax, SK_PARAM_LIST);
}

SyntaxNode *FnDef_return_type(const FnDef *f) {
    return ast_first_child_pred(f->syntax, is_type_node);
}

SyntaxNode *FnDef_body(const FnDef *f) {
    return ast_first_child(f->syntax, SK_BLOCK_STMT);
}


// ---- StructDef ------------------------------------------------------

SyntaxToken *StructDef_name(const StructDef *s) {
    return ast_first_token(s->syntax, SK_IDENT);
}

SyntaxNode *StructDef_fields(const StructDef *s) {
    return ast_first_child(s->syntax, SK_FIELD_LIST);
}


// ---- EnumDef --------------------------------------------------------

SyntaxToken *EnumDef_name(const EnumDef *e) {
    return ast_first_token(e->syntax, SK_IDENT);
}

SyntaxNode *EnumDef_variants(const EnumDef *e) {
    return ast_first_child(e->syntax, SK_VARIANT_LIST);
}


// ---- UnionDef -------------------------------------------------------

SyntaxToken *UnionDef_name(const UnionDef *u) {
    return ast_first_token(u->syntax, SK_IDENT);
}

SyntaxNode *UnionDef_variants(const UnionDef *u) {
    return ast_first_child(u->syntax, SK_VARIANT_LIST);
}


// ---- EffectDef ------------------------------------------------------

SyntaxToken *EffectDef_name(const EffectDef *e) {
    return ast_first_token(e->syntax, SK_IDENT);
}

SyntaxNode *EffectDef_ops(const EffectDef *e) {
    return ast_first_child(e->syntax, SK_FIELD_LIST);
}


// ---- ConstDef -------------------------------------------------------

SyntaxToken *ConstDef_name(const ConstDef *c) {
    return ast_first_token(c->syntax, SK_IDENT);
}

SyntaxNode *ConstDef_type(const ConstDef *c) {
    return ast_first_child_pred(c->syntax, is_type_node);
}

SyntaxNode *ConstDef_value(const ConstDef *c) {
    return ast_first_child_pred(c->syntax, is_expr_node);
}


// ---- VarDef ---------------------------------------------------------

SyntaxToken *VarDef_name(const VarDef *v) {
    return ast_first_token(v->syntax, SK_IDENT);
}

SyntaxNode *VarDef_type(const VarDef *v) {
    return ast_first_child_pred(v->syntax, is_type_node);
}

SyntaxNode *VarDef_value(const VarDef *v) {
    return ast_first_child_pred(v->syntax, is_expr_node);
}


// ---- ImportDef ------------------------------------------------------

SyntaxNode *ImportDef_path(const ImportDef *i) {
    return ast_first_child_pred(i->syntax, is_expr_node);
}


// ---- Param ----------------------------------------------------------

SyntaxToken *Param_name(const Param *p) {
    return ast_first_token(p->syntax, SK_IDENT);
}

SyntaxNode *Param_type(const Param *p) {
    return ast_first_child_pred(p->syntax, is_type_node);
}


// ---- Field ----------------------------------------------------------

SyntaxToken *Field_name(const Field *f) {
    return ast_first_token(f->syntax, SK_IDENT);
}

SyntaxNode *Field_type(const Field *f) {
    return ast_first_child_pred(f->syntax, is_type_node);
}

SyntaxNode *Field_default(const Field *f) {
    return ast_first_child_pred(f->syntax, is_expr_node);
}


// ---- Variant --------------------------------------------------------

SyntaxToken *Variant_name(const Variant *v) {
    return ast_first_token(v->syntax, SK_IDENT);
}

SyntaxNode *Variant_payload(const Variant *v) {
    // A variant payload may be either a SK_PARAM_LIST (tuple-style)
    // or a SK_FIELD_LIST (record-style). Try the first; fall back to
    // the second.
    SyntaxNode *p = ast_first_child(v->syntax, SK_PARAM_LIST);
    if (p) return p;
    return ast_first_child(v->syntax, SK_FIELD_LIST);
}

SyntaxNode *Variant_value(const Variant *v) {
    // The discriminant expression follows an `=` token. Walk children
    // until we see SK_EQ, then return the next node child.
    uint32_t num = syntax_node_num_children(v->syntax);
    bool past_eq = false;
    for (uint32_t i = 0; i < num; i++) {
        SyntaxElement el = syntax_node_child_or_token(v->syntax, i);
        if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
            if (!past_eq && syntax_token_kind(el.token) == SK_EQ)
                past_eq = true;
            syntax_token_release(el.token);
            continue;
        }
        if (el.kind == SYNTAX_ELEM_NODE && el.node) {
            if (past_eq)
                return el.node;
            syntax_node_release(el.node);
        }
    }
    return NULL;
}
