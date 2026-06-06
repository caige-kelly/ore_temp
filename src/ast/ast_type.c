#include "./ast_type.h"

#define DEFINE_CAST(Type, Kind)                                                \
  bool Type##_cast(const SyntaxNode *n, Type *out) {                           \
    if (!n || syntax_node_kind(n) != (Kind))                                   \
      return false;                                                            \
    out->syntax = (SyntaxNode *)n;                                             \
    return true;                                                               \
  }

DEFINE_CAST(RefType, SK_REF_TYPE)
DEFINE_CAST(PtrType, SK_PTR_TYPE)
DEFINE_CAST(SliceType, SK_SLICE_TYPE)
DEFINE_CAST(ArrayType, SK_ARRAY_TYPE)
DEFINE_CAST(ManyPtrType, SK_MANY_PTR_TYPE)
DEFINE_CAST(FnType, SK_FN_TYPE)
DEFINE_CAST(OptionalType, SK_OPTIONAL_TYPE)
DEFINE_CAST(ConstType, SK_CONST_TYPE)
// Slice 6.18: kind-qualified nominal types. `...Ref` suffix avoids colliding
// with the db-layer StructType / EnumType result structs.
DEFINE_CAST(StructTypeRef, SK_STRUCT_TYPE)
DEFINE_CAST(UnionTypeRef, SK_UNION_TYPE)
DEFINE_CAST(EnumTypeRef, SK_ENUM_TYPE)
DEFINE_CAST(HandlerTypeRef, SK_HANDLER_TYPE)
DEFINE_CAST(EffectTypeRef, SK_EFFECT_TYPE)
DEFINE_CAST(DistinctType, SK_DISTINCT_TYPE)
DEFINE_CAST(BitFieldType, SK_BIT_FIELD_TYPE)

static bool is_type_node(OreSyntaxKind k) { return ore_kind_is_type_node(k); }

SyntaxToken *RefType_name(const RefType *r) {
  return ast_first_token(r->syntax, SK_IDENT);
}

// Slice 6.18: all five kind-qualified nominal types share the `{ keyword,
// IDENT }` shape — the name is the lone IDENT token.
SyntaxToken *StructTypeRef_name(const StructTypeRef *t) {
  return ast_first_token(t->syntax, SK_IDENT);
}
SyntaxToken *UnionTypeRef_name(const UnionTypeRef *t) {
  return ast_first_token(t->syntax, SK_IDENT);
}
SyntaxToken *EnumTypeRef_name(const EnumTypeRef *t) {
  return ast_first_token(t->syntax, SK_IDENT);
}
SyntaxToken *HandlerTypeRef_name(const HandlerTypeRef *t) {
  return ast_first_token(t->syntax, SK_IDENT);
}
SyntaxToken *EffectTypeRef_name(const EffectTypeRef *t) {
  return ast_first_token(t->syntax, SK_IDENT);
}

// Slice 6.19: the backing type node (`T` in `distinct T`); skips the
// leading `distinct` keyword token, mirrors PtrType_pointee.
SyntaxNode *DistinctType_backing(const DistinctType *d) {
  return ast_first_child_pred(d->syntax, is_type_node);
}

// Slice 6.22: backing type of a bit_field — the lone DIRECT type-node child
// (field types are grandchildren under SK_BIT_FIELD_LIST), so this mirrors
// DistinctType_backing exactly.
SyntaxNode *BitFieldType_backing(const BitFieldType *bf) {
  return ast_first_child_pred(bf->syntax, is_type_node);
}
SyntaxNode *BitFieldType_fields(const BitFieldType *bf) {
  return ast_first_child(bf->syntax, SK_BIT_FIELD_LIST);
}

SyntaxNode *PtrType_pointee(const PtrType *p) {
  return ast_first_child_pred(p->syntax, is_type_node);
}

SyntaxNode *SliceType_element(const SliceType *s) {
  return ast_first_child_pred(s->syntax, is_type_node);
}

SyntaxNode *ArrayType_size(const ArrayType *a) {
  return ast_first_child_pred(a->syntax, ore_kind_is_value_node);
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
SyntaxNode *FnType_effect_row(const FnType *f) {
  return ast_first_child(f->syntax, SK_EFFECT_ROW_TYPE);
}
// Return type follows the optional effect-row child. is_type_node includes
// SK_EFFECT_ROW_TYPE (it's in the contiguous SK_REF_TYPE..SK_EFFECT_ROW_TYPE
// range), so a naive first-type-child match would return the effect row.
// Walk children explicitly and skip it.
SyntaxNode *FnType_return_type(const FnType *f) {
  uint32_t n = syntax_node_num_children(f->syntax);
  for (uint32_t i = 0; i < n; i++) {
    SyntaxElement el = syntax_node_child_or_token(f->syntax, i);
    if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
      if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
        syntax_token_release(el.token);
      continue;
    }
    SyntaxKind k = syntax_node_kind(el.node);
    if (k == SK_PARAM_LIST || k == SK_EFFECT_ROW_TYPE) {
      syntax_node_release(el.node);
      continue;
    }
    if (is_type_node((OreSyntaxKind)k))
      return el.node;
    syntax_node_release(el.node);
  }
  return NULL;
}

SyntaxNode *OptionalType_inner(const OptionalType *o) {
  return ast_first_child_pred(o->syntax, is_type_node);
}

SyntaxNode *ConstType_inner(const ConstType *c) {
  return ast_first_child_pred(c->syntax, is_type_node);
}
