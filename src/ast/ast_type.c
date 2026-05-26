#include "./ast_type.h"


#define DEFINE_CAST(Type, Kind)                                              \
    bool Type##_cast(const SyntaxNode *n, Type *out) {                       \
        if (!n || syntax_node_kind(n) != (Kind)) return false;               \
        out->syntax = (SyntaxNode *)n;                                       \
        return true;                                                         \
    }

DEFINE_CAST(RefType,      SK_REF_TYPE)
DEFINE_CAST(PtrType,      SK_PTR_TYPE)
DEFINE_CAST(SliceType,    SK_SLICE_TYPE)
DEFINE_CAST(ArrayType,    SK_ARRAY_TYPE)
DEFINE_CAST(ManyPtrType,  SK_MANY_PTR_TYPE)
DEFINE_CAST(FnType,       SK_FN_TYPE)
DEFINE_CAST(OptionalType, SK_OPTIONAL_TYPE)
DEFINE_CAST(ConstType,    SK_CONST_TYPE)


static bool is_type_node(OreSyntaxKind k) { return ore_kind_is_type_node(k); }
static bool is_expr_node(OreSyntaxKind k) { return ore_kind_is_expr_node(k); }


SyntaxToken *RefType_name(const RefType *r) {
    return ast_first_token(r->syntax, SK_IDENT);
}

SyntaxNode *PtrType_pointee(const PtrType *p) {
    return ast_first_child_pred(p->syntax, is_type_node);
}

SyntaxNode *SliceType_element(const SliceType *s) {
    return ast_first_child_pred(s->syntax, is_type_node);
}

SyntaxNode *ArrayType_size(const ArrayType *a) {
    return ast_first_child_pred(a->syntax, is_expr_node);
}
SyntaxNode *ArrayType_element(const ArrayType *a) {
    return ast_first_child_pred(a->syntax, is_type_node);
}

SyntaxNode *ManyPtrType_element(const ManyPtrType *m) {
    return ast_first_child_pred(m->syntax, is_type_node);
}

SyntaxNode *FnType_params(const FnType *f) {
    return ast_first_child(f->syntax, SK_PARAM_LIST);
}
SyntaxNode *FnType_return_type(const FnType *f) {
    return ast_first_child_pred(f->syntax, is_type_node);
}

SyntaxNode *OptionalType_inner(const OptionalType *o) {
    return ast_first_child_pred(o->syntax, is_type_node);
}

SyntaxNode *ConstType_inner(const ConstType *c) {
    return ast_first_child_pred(c->syntax, is_type_node);
}
