#include "../ast/ast_decl.h"
#include "../ast/ast_expr.h"
#include "../db/db.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/decl_ast.h"
#include "../db/query/def_identity.h"
#include "../db/query/fn_signature.h"
#include "../db/query/index.h"
#include "../db/query/type_of_def.h"
#include "../syntax/syntax_kind.h"
#include "../support/data_structure/stringpool.h"
#include "../syntax/syntax.h"
#include "sema.h"

#include <stdlib.h>

// Intern a SyntaxToken's text via the string pool.
static StrId intern_tok(struct db *s, SyntaxToken *t) {
  if (!t) return (StrId){0};
  const char *txt = syntax_token_text(t);
  uint32_t len = syntax_token_text_range(t).length;
  return pool_intern(&s->strings, txt, len);
}

// Read an int literal's u64 value via the Literal wrapper.
static int64_t literal_int_signed(SyntaxNode *node) {
  Literal lit;
  if (!Literal_cast(node, &lit) || Literal_kind(&lit) != SK_INT_LIT)
    return 0;
  SyntaxToken *tok = Literal_token(&lit);
  if (!tok) return 0;
  const char *txt = syntax_token_text(tok);
  uint32_t len = syntax_token_text_range(tok).length;
  char buf[64];
  int64_t v = 0;
  if (len < sizeof(buf)) {
    uint32_t w = 0;
    for (uint32_t i = 0; i < len; i++)
      if (txt[i] != '_') buf[w++] = txt[i];
    buf[w] = '\0';
    char *end;
    v = (int64_t)strtoll(buf, &end, 0);
  }
  syntax_token_release(tok);
  return v;
}

// Build a struct (or union, treated as struct) type from an
// SK_STRUCT_DECL / SK_UNION_DECL node. Uses ip_wip_struct to allocate
// the IpIndex up-front; pointer-cycle support is wired by publishing
// wip.index into the def's per-kind type cell BEFORE the field loop
// runs. A recursive type_of_def(self) call during the field loop
// reads that cell and returns the wip's IpIndex instead of IP_NONE.
//
// zir_node_id = def.idx — nominal identity keyed by the declaring
// def, so two structurally-identical struct decls at different sites
// get distinct IpIndex values.
static IpIndex build_struct_type(struct db *s, SyntaxNode *aggregate_node,
                                 DefId def, NamespaceId nsid,
                                 FileId file_local, GreenNode *groot) {
  // StructDef and UnionDef expose the same "fields" wrapper accessor.
  SyntaxNode *field_list = NULL;
  SyntaxKind ak = syntax_node_kind(aggregate_node);
  if (ak == SK_STRUCT_DECL) {
    StructDef sd; StructDef_cast(aggregate_node, &sd);
    field_list = StructDef_fields(&sd);
  } else if (ak == SK_UNION_DECL) {
    UnionDef ud; UnionDef_cast(aggregate_node, &ud);
    field_list = UnionDef_variants(&ud); // unions reuse the variant list
  }
  if (!field_list) {
    IpKey key = {.kind = IPK_STRUCT_TYPE,
                 .struct_type = {.zir_node_id = def.idx,
                                 .field_names = NULL,
                                 .field_types = NULL,
                                 .n_fields = 0}};
    return ip_get(&s->intern, key);
  }

  // Count SK_FIELD children to size scratch arrays.
  uint32_t n_fields = 0;
  uint32_t list_count = syntax_node_num_children(field_list);
  for (uint32_t i = 0; i < list_count; i++) {
    GreenElement g = green_node_child(syntax_node_green(field_list), i);
    if (g.kind == GREEN_ELEM_NODE && green_node_kind(g.node) == SK_FIELD)
      n_fields++;
  }
  if (n_fields == 0) {
    syntax_node_release(field_list);
    IpKey key = {.kind = IPK_STRUCT_TYPE,
                 .struct_type = {.zir_node_id = def.idx,
                                 .field_names = NULL,
                                 .field_types = NULL,
                                 .n_fields = 0}};
    return ip_get(&s->intern, key);
  }

  StrId *names = arena_alloc(&s->request_arena, n_fields * sizeof(StrId));
  IpIndex *types = arena_alloc(&s->request_arena, n_fields * sizeof(IpIndex));
  if (!names || !types) {
    syntax_node_release(field_list);
    return IP_NONE;
  }
  WipContainerType wip = ip_wip_struct(&s->intern, def.idx, NULL, 0);

  // Publish the wip's IpIndex into the def's type cell BEFORE the
  // field loop runs — load-bearing for self/mutually-referential
  // struct types. db_def_set_kind is idempotent.
  if (db_def_kind(s, def) == KIND_NONE) {
    DefKind promote = (ak == SK_UNION_DECL) ? KIND_UNION : KIND_STRUCT;
    db_def_set_kind(s, def, promote);
  }
  *db_def_type_cell(s, def) = wip.index;

  NodeTypeBuilder fields_b;
  sema_node_type_builder_begin(s, &fields_b, file_local);
  SemaCtx fields_ctx = {
      .s = s,
      .file_green_root = groot,
      .nsid = nsid,
      .enclosing_fn = DEF_ID_NONE,
      .file_local = file_local,
      .types = &fields_b,
  };

  IpIndex final_result = IP_NONE;
  bool cancelled = false;
  uint32_t fout = 0;
  for (uint32_t i = 0; i < list_count && !cancelled; i++) {
    SyntaxElement el = syntax_node_child_or_token(field_list, i);
    if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
      if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
        syntax_token_release(el.token);
      continue;
    }
    if (syntax_node_kind(el.node) != SK_FIELD) {
      syntax_node_release(el.node);
      continue;
    }
    Field fld;
    if (!Field_cast(el.node, &fld)) {
      syntax_node_release(el.node);
      continue;
    }
    SyntaxToken *fname_tok = Field_name(&fld);
    SyntaxNode *ftype = Field_type(&fld);
    StrId fname = intern_tok(s, fname_tok);
    if (fname_tok) syntax_token_release(fname_tok);
    IpIndex ftypei = ftype ? sema_resolve_type_expr(&fields_ctx, ftype)
                            : IP_NONE;
    if (ftype) syntax_node_release(ftype);
    if (ftypei.v == IP_NONE.v) {
      ip_wip_struct_cancel(&s->intern, wip);
      *db_def_type_cell(s, def) = IP_NONE;
      cancelled = true;
      syntax_node_release(el.node);
      break;
    }
    names[fout] = fname;
    types[fout] = ftypei;
    sema_node_type_builder_push(&fields_ctx, el.node, ftypei);
    fout++;
    syntax_node_release(el.node);
  }

  if (!cancelled) {
    ip_wip_struct_finish(&s->intern, wip, names, types, fout);
    final_result = wip.index;
  }

  NodeTypesRange field_range = sema_node_type_builder_end(&fields_b, NULL);
  if (db_def_kind(s, def) == KIND_STRUCT) {
    uint32_t row = db_def_row(s, def, KIND_STRUCT);
    *(NodeTypesRange *)vec_get(&s->structs.field_node_types, row) = field_range;
  }

  syntax_node_release(field_list);
  return final_result;
}

// Build an enum type from an SK_ENUM_DECL node. Variant values:
// auto-numbered (0..N-1) or bare-literal-int only.
static IpIndex build_enum_type(struct db *s, SyntaxNode *enum_node,
                               DefId def) {
  EnumDef ed;
  if (!EnumDef_cast(enum_node, &ed))
    return IP_NONE;
  SyntaxNode *variants_list = EnumDef_variants(&ed);
  if (!variants_list) {
    IpKey key = {.kind = IPK_ENUM_TYPE,
                 .enum_type = {.zir_node_id = def.idx,
                               .variant_names = NULL,
                               .variant_values = NULL,
                               .n_variants = 0}};
    return ip_get(&s->intern, key);
  }

  uint32_t n_variants = 0;
  uint32_t list_count = syntax_node_num_children(variants_list);
  for (uint32_t i = 0; i < list_count; i++) {
    GreenElement g = green_node_child(syntax_node_green(variants_list), i);
    if (g.kind == GREEN_ELEM_NODE && green_node_kind(g.node) == SK_VARIANT)
      n_variants++;
  }

  StrId *names = arena_alloc(&s->request_arena, n_variants * sizeof(StrId));
  int64_t *values =
      arena_alloc(&s->request_arena, n_variants * sizeof(int64_t));
  if (n_variants > 0 && (!names || !values)) {
    syntax_node_release(variants_list);
    return IP_NONE;
  }

  uint32_t vout = 0;
  for (uint32_t i = 0; i < list_count; i++) {
    SyntaxElement el = syntax_node_child_or_token(variants_list, i);
    if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
      if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
        syntax_token_release(el.token);
      continue;
    }
    if (syntax_node_kind(el.node) != SK_VARIANT) {
      syntax_node_release(el.node);
      continue;
    }
    Variant v;
    if (!Variant_cast(el.node, &v)) {
      syntax_node_release(el.node);
      continue;
    }
    SyntaxToken *vname_tok = Variant_name(&v);
    SyntaxNode *v_val = Variant_value(&v);
    names[vout] = intern_tok(s, vname_tok);
    if (vname_tok) syntax_token_release(vname_tok);
    if (!v_val) {
      values[vout] = (int64_t)vout;
    } else {
      values[vout] = literal_int_signed(v_val);
      syntax_node_release(v_val);
    }
    vout++;
    syntax_node_release(el.node);
  }
  syntax_node_release(variants_list);

  IpKey key = {.kind = IPK_ENUM_TYPE,
               .enum_type = {.zir_node_id = def.idx,
                             .variant_names = names,
                             .variant_values = values,
                             .n_variants = vout},
               .src_arena = vout ? &s->request_arena : NULL,
               .src_gen = s->request_arena.generation};
  return ip_get(&s->intern, key);
}

IpIndex sema_type_of_def(struct db *s, DefId def) {
  SyntaxNodePtr def_ptr = *(SyntaxNodePtr *)vec_get(&s->defs.syntax_ptrs, def.idx);
  NamespaceId nsid = *(NamespaceId *)vec_get(&s->defs.parent_modules, def.idx);

  // Run top_level_index for its side effect (populating files[].top_
  // level_indices) but DON'T record it as a dep — sibling-decl edits
  // would otherwise cascade into this body's recompute. The def_
  // identity dep below carries this decl's own identity signal.
  db_query_ensure(s, QUERY_TOP_LEVEL_INDEX, (uint64_t)nsid.idx);
  (void)db_query_def_identity(s, nsid, def_ptr);

  IpIndex result = IP_NONE;
  uint32_t fc = 0;
  const FileId *files = db_get_namespace_files(s, nsid, &fc);
  for (uint32_t i = 0; i < fc; i++) {
    SyntaxNodePtr ptr = db_query_decl_ast(s, files[i], def_ptr);
    if (ptr.kind == SYNTAX_KIND_NONE)
      continue;

    uint32_t local = file_id_local(files[i]);
    GreenNode *groot = *(GreenNode **)vec_get(&s->files.green_roots, local);
    if (!groot)
      continue;
    SyntaxTree *tree = syntax_tree_new(groot);
    SyntaxNode *root_red = syntax_tree_root(tree);
    SyntaxNode *wrapper = syntax_node_ptr_resolve(ptr, root_red);
    syntax_node_release(root_red);
    if (!wrapper) {
      syntax_tree_free(tree);
      continue;
    }
    SyntaxKind wk = syntax_node_kind(wrapper);
    if (wk != SK_CONST_DECL && wk != SK_VAR_DECL) {
      syntax_node_release(wrapper);
      syntax_tree_free(tree);
      break;
    }

    // Read meta from the per-def column (populated by def_identity).
    DefMeta meta = *(DefMeta *)vec_get(&s->defs.meta, def.idx);
    // Distinct binds need separate nominal-identity machinery — chunk 8.
    if (meta & META_DISTINCT) {
      syntax_node_release(wrapper);
      syntax_tree_free(tree);
      break;
    }

    SyntaxNode *value = NULL;
    SyntaxNode *type_annot = NULL;
    if (wk == SK_CONST_DECL) {
      ConstDef c;
      if (ConstDef_cast(wrapper, &c)) {
        value = ConstDef_value(&c);
        type_annot = ConstDef_type(&c);
      }
    } else {
      VarDef v;
      if (VarDef_cast(wrapper, &v)) {
        value = VarDef_value(&v);
        type_annot = VarDef_type(&v);
      }
    }

    // RHS-driven nominal types: aggregate / lambda / handler RHS
    // determines the type, ignoring any `: type` annotation.
    if (value) {
      SyntaxKind vk = syntax_node_kind(value);
      if (vk == SK_STRUCT_DECL || vk == SK_UNION_DECL) {
        result = build_struct_type(s, value, def, nsid, files[i], groot);
        syntax_node_release(value);
        if (type_annot) syntax_node_release(type_annot);
        syntax_node_release(wrapper);
        syntax_tree_free(tree);
        break;
      }
      if (vk == SK_ENUM_DECL) {
        result = build_enum_type(s, value, def);
        syntax_node_release(value);
        if (type_annot) syntax_node_release(type_annot);
        syntax_node_release(wrapper);
        syntax_tree_free(tree);
        break;
      }
      if (vk == SK_LAMBDA_EXPR) {
        result = db_query_fn_signature(s, def);
        syntax_node_release(value);
        if (type_annot) syntax_node_release(type_annot);
        syntax_node_release(wrapper);
        syntax_tree_free(tree);
        break;
      }
    }

    bool has_annotation = (type_annot != NULL);
    bool has_value = (value != NULL);
    if (!has_annotation && !has_value) {
      if (type_annot) syntax_node_release(type_annot);
      if (value) syntax_node_release(value);
      syntax_node_release(wrapper);
      syntax_tree_free(tree);
      break;
    }

    NodeTypeBuilder val_b;
    sema_node_type_builder_begin(s, &val_b, files[i]);
    SemaCtx val_ctx = {.s = s,
                       .file_green_root = groot,
                       .nsid = nsid,
                       .enclosing_fn = DEF_ID_NONE,
                       .file_local = files[i],
                       .types = &val_b};

    if (has_annotation) {
      // Typed bind: annotation wins; RHS is checked against it
      // (bidirectional) so literals get coerced to the concrete type.
      result = sema_resolve_type_expr(&val_ctx, type_annot);
      if (has_value && result.v != IP_NONE.v)
        (void)sema_check_expr(&val_ctx, value, result);
    } else {
      // Inferred bind: type comes from value expression.
      result = sema_type_of_expr(&val_ctx, value);
    }

    NodeTypesRange v_range = sema_node_type_builder_end(&val_b, NULL);
    DefKind k = db_def_kind(s, def);
    if (k == KIND_CONSTANT) {
      uint32_t row = db_def_row(s, def, KIND_CONSTANT);
      *(NodeTypesRange *)vec_get(&s->constants.value_node_types, row) = v_range;
    } else if (k == KIND_VARIABLE) {
      uint32_t row = db_def_row(s, def, KIND_VARIABLE);
      *(NodeTypesRange *)vec_get(&s->variables.value_node_types, row) = v_range;
    }

    if (type_annot) syntax_node_release(type_annot);
    if (value) syntax_node_release(value);
    syntax_node_release(wrapper);
    syntax_tree_free(tree);
    break;
  }

  return result;
}
