// Type layer (Phase D2.1) — the DECLARED-INTERFACE queries: turn a DefId into
// an interned type.
//
//   type_of_def(def)      — a decl's overall type (IpIndex). Nominal types
//                           (struct/union/enum) are built here; functions
//                           delegate to fn_signature; typed binds resolve their
//                           annotation. Inferred binds + bodies are D2.4.
//   fn_signature(def)      — a function's (params → ret) type + per-param hover
//                           node-types (FnSignature).
//   resolve_type_expr(ctx) — resolve a type-position syntax node to an IpIndex.
//
// Ported from src/sema/{type_resolve,fn_signature,type_of_def}.c: the type
// algorithms are unchanged; only the dep plumbing is rewired onto the D1 layer.
// A decl's CURRENT location comes from top_level_entry(nsid, name) (the content
// firewall) — there is no decl_ast / multi-file loop / TOP_LEVEL_INDEX side
// effect. The green root is read raw from files.green_roots so type_of_def does
// NOT record a file_ast dep (that would defeat the content firewall — a sibling
// edit would re-run it); the firewall dep is top_level_entry's alone.

#define ORE_ENGINE_PRIVATE
#include "engine.h"
#include "engine_internal.h"
#include "result_columns.h" // db.h, ids.h, intern_pool.h, syntax.h
#include "type_layer.h"

#include "../diag/diag.h" // db_emit, diag_anchor_of_node, DIAG_ERROR

#include "../../ast/ast.h" // ast_first_token
#include "../../ast/ast_decl.h"
#include "../../ast/ast_expr.h"
#include "../../ast/ast_type.h"
#include "../../support/data_structure/stringpool.h"
#include "../../syntax/syntax_kind.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// --- Cross-layer queries (parse.c / scope.c) --------------------------------
extern FileArray db_query_namespace_items(db_query_ctx *ctx, NamespaceId nsid);
extern TopLevelEntry db_query_top_level_entry(db_query_ctx *ctx,
                                              NamespaceId nsid, StrId name);
extern NamespaceScopes db_query_namespace_scopes(db_query_ctx *ctx,
                                                 NamespaceId nsid);
extern DefId db_query_resolve_ref(db_query_ctx *ctx, ScopeId scope, StrId name);
extern DefId db_query_def_identity(db_query_ctx *ctx, NamespaceId nsid,
                                   AstId id);

// --- This layer (mutually recursive) ----------------------------------------
IpIndex db_query_type_of_def(db_query_ctx *ctx, DefId def);
const FnSignature *db_query_fn_signature(db_query_ctx *ctx, DefId def);

// ============================================================================
// NodeTypeBuilder — per-decl resolved-types map (RA InferenceResult).
// ============================================================================

void node_type_builder_begin(struct db *s, NodeTypeBuilder *b,
                             FileId file_local) {
  (void)s;
  b->file_local = file_local;
  hashmap_init(&b->types);
  b->fp = db_fp_u64(0);
}

void node_type_builder_push(const SemaCtx *ctx, SyntaxNode *node,
                            IpIndex type) {
  if (!ctx || !ctx->types || !node)
    return;
  NodeTypeBuilder *b = ctx->types;
  uint64_t key = syntax_node_ptr_hash(syntax_node_ptr_new(node));
  hashmap_put(&b->types, key, (void *)(uintptr_t)type.v);
  // POSITION-INDEPENDENT: fold ONLY the type value, in push order. The node
  // key (kind+byte-range) is deliberately NOT folded — a pure trivia shift
  // would change it without changing any type, so including it would break
  // the firewall. Push order is structural (the descent is position-
  // independent), so order-folded type values catch add/remove/reorder/
  // retype; AST-structure changes reach AST-consumers via their own deps.
  b->fp = db_fp_combine(b->fp, db_fp_u64((uint64_t)type.v));
}

NodeTypesRange node_type_builder_end(NodeTypeBuilder *b, Fingerprint *out_fp) {
  if (out_fp)
    *out_fp = b->fp;
  NodeTypesRange out = {.types = b->types};
  b->types = (HashMap){0};
  return out;
}

IpIndex node_types_range_lookup(struct db *s, NodeTypesRange range,
                                SyntaxNode *node) {
  (void)s;
  if (!node || !hashmap_is_initialized(&range.types))
    return IP_NONE;
  uint64_t key = syntax_node_ptr_hash(syntax_node_ptr_new(node));
  void *v = hashmap_get(&range.types, key);
  if (!v)
    return IP_NONE;
  return (IpIndex){.v = (uint32_t)(uintptr_t)v};
}

// ============================================================================
// Small helpers.
// ============================================================================

static StrId intern_tok(struct db *s, SyntaxToken *t) {
  if (!t)
    return (StrId){0};
  const char *txt = syntax_token_text(t);
  uint32_t len = syntax_token_text_range(t).length;
  return pool_intern(&s->strings, txt, len);
}

// Bare int-literal value (underscores stripped; 0x/0b/0o prefixes via strtoll).
static int64_t literal_int_signed(SyntaxNode *node) {
  Literal lit;
  if (!Literal_cast(node, &lit) || Literal_kind(&lit) != SK_INT_LIT)
    return 0;
  SyntaxToken *tok = Literal_token(&lit);
  if (!tok)
    return 0;
  const char *txt = syntax_token_text(tok);
  uint32_t len = syntax_token_text_range(tok).length;
  char buf[64];
  int64_t v = 0;
  if (len < sizeof(buf)) {
    uint32_t w = 0;
    for (uint32_t i = 0; i < len; i++)
      if (txt[i] != '_')
        buf[w++] = txt[i];
    buf[w] = '\0';
    v = (int64_t)strtoll(buf, NULL, 0);
  }
  syntax_token_release(tok);
  return v;
}

static uint64_t parse_int_literal(SyntaxToken *tok) {
  const char *txt = syntax_token_text(tok);
  uint32_t len = syntax_token_text_range(tok).length;
  char buf[64];
  if (len >= sizeof(buf))
    return 0;
  uint32_t w = 0;
  for (uint32_t i = 0; i < len; i++)
    if (txt[i] != '_')
      buf[w++] = txt[i];
  buf[w] = '\0';
  return strtoull(buf, NULL, 0);
}

// Last IDENT child of a PATH node (the leaf name in type position).
static StrId path_expr_leaf_name(struct db *s, SyntaxNode *path) {
  uint32_t count = syntax_node_num_children(path);
  StrId last = {0};
  for (uint32_t i = 0; i < count; i++) {
    GreenElement g = green_node_child(syntax_node_green(path), i);
    if (g.kind == GREEN_ELEM_TOKEN && green_token_kind(g.token) == SK_IDENT) {
      const char *txt = green_token_text(g.token);
      uint32_t len = green_token_text_len(g.token);
      last = pool_intern(&s->strings, txt, len);
    }
  }
  return last;
}

static DiagAnchor span_of(const SemaCtx *ctx, SyntaxNode *node) {
  return diag_anchor_of_node((uint16_t)ctx->file_local.idx, node);
}

// The value / type-annotation node of a `::`/`:=` bind wrapper (+1; release).
static SyntaxNode *bind_value(SyntaxNode *wrapper) {
  switch ((OreSyntaxKind)syntax_node_kind(wrapper)) {
  case SK_CONST_DECL: {
    ConstDef c;
    if (ConstDef_cast(wrapper, &c))
      return ConstDef_value(&c);
    break;
  }
  case SK_VAR_DECL: {
    VarDef v;
    if (VarDef_cast(wrapper, &v))
      return VarDef_value(&v);
    break;
  }
  default:
    break;
  }
  return NULL;
}
static SyntaxNode *bind_type_annot(SyntaxNode *wrapper) {
  switch ((OreSyntaxKind)syntax_node_kind(wrapper)) {
  case SK_CONST_DECL: {
    ConstDef c;
    if (ConstDef_cast(wrapper, &c))
      return ConstDef_type(&c);
    break;
  }
  case SK_VAR_DECL: {
    VarDef v;
    if (VarDef_cast(wrapper, &v))
      return VarDef_type(&v);
    break;
  }
  default:
    break;
  }
  return NULL;
}

// User-defined name resolution: internal scope → resolve_ref → type_of_def.
// Records the salsa deps so renaming/retyping the target invalidates us.
static IpIndex resolve_user_type_name(struct db *s, NamespaceId nsid,
                                      StrId name) {
  if (name.idx == 0)
    return IP_NONE;
  NamespaceScopes sc = db_query_namespace_scopes(s, nsid); // dep
  if (sc.internal.idx == SCOPE_ID_NONE.idx)
    return IP_NONE;
  DefId target = db_query_resolve_ref(s, sc.internal, name); // dep
  if (target.idx == DEF_ID_NONE.idx)
    return IP_NONE;
  return db_query_type_of_def(s, target); // dep
}

// ============================================================================
// build_fn_type — interned fn type from a param-list + return-type node.
// Shared by the lambda path (fn_signature) and the type-position SK_FN_TYPE.
// Identity is structural (ret, modifiers, params). Scratch params come from
// request_arena (reset at db_request_end), so n_params has no compile cap.
// ============================================================================

IpIndex build_fn_type(const SemaCtx *ctx, SyntaxNode *ret_node,
                      SyntaxNode *param_list) {
  struct db *s = ctx->s;

  uint32_t n_params = 0;
  if (param_list) {
    uint32_t pc = syntax_node_num_children(param_list);
    for (uint32_t i = 0; i < pc; i++) {
      GreenElement g = green_node_child(syntax_node_green(param_list), i);
      if (g.kind == GREEN_ELEM_NODE && green_node_kind(g.node) == SK_PARAM)
        n_params++;
    }
  }

  IpIndex *params = NULL;
  if (n_params > 0) {
    params = arena_alloc(&s->request_arena, n_params * sizeof(IpIndex));
    if (!params)
      return IP_NONE;
  }

  if (param_list) {
    uint32_t pc = syntax_node_num_children(param_list);
    uint32_t out = 0;
    for (uint32_t i = 0; i < pc; i++) {
      SyntaxElement el = syntax_node_child_or_token(param_list, i);
      if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
        if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
          syntax_token_release(el.token);
        continue;
      }
      Param p;
      if (!Param_cast(el.node, &p)) {
        syntax_node_release(el.node);
        continue;
      }
      SyntaxNode *ptype = Param_type(&p);
      IpIndex pti = ptype ? resolve_type_expr(ctx, ptype) : IP_NONE;
      if (ptype)
        syntax_node_release(ptype);
      if (pti.v == IP_NONE.v) {
        syntax_node_release(el.node);
        return IP_NONE;
      }
      params[out++] = pti;
      node_type_builder_push(ctx, el.node, pti); // hover on the param node
      syntax_node_release(el.node);
    }
  }

  IpIndex ret;
  if (!ret_node) {
    ret = IP_VOID_TYPE; // implicit void
  } else {
    ret = resolve_type_expr(ctx, ret_node);
    if (ret.v == IP_NONE.v)
      return IP_NONE;
  }

  IpKey key = {.kind = IPK_FN_TYPE,
               .fn_type = {.ret = ret,
                           .modifiers = 0,
                           .params = params,
                           .n_params = n_params},
               .src_arena = params ? &s->request_arena : NULL,
               .src_gen = s->request_arena.generation};
  return ip_get(&s->intern, key);
}

// ============================================================================
// resolve_type_expr — a type-position node → IpIndex.
// ============================================================================

IpIndex resolve_type_expr(const SemaCtx *ctx, SyntaxNode *node) {
  if (!node)
    return IP_NONE;
  struct db *s = ctx->s;
  NamespaceId nsid = ctx->nsid;
  SyntaxKind k = syntax_node_kind(node);
  IpIndex result = IP_NONE;

  switch ((OreSyntaxKind)k) {
  case SK_REF_TYPE:
  case SK_REF_EXPR: {
    SyntaxToken *name_tok = ast_first_token(node, SK_IDENT);
    StrId name = intern_tok(s, name_tok);
    if (name_tok)
      syntax_token_release(name_tok);
    result = resolve_user_type_name(s, nsid, name);
    if (result.v == IP_NONE.v && name.idx != 0)
      db_emit(s, DIAG_ERROR, span_of(ctx, node), "unknown type '%S'", name);
    break;
  }
  case SK_PATH_TYPE:
  case SK_PATH_EXPR: {
    StrId name = path_expr_leaf_name(s, node);
    result = resolve_user_type_name(s, nsid, name);
    if (result.v == IP_NONE.v && name.idx != 0)
      db_emit(s, DIAG_ERROR, span_of(ctx, node), "unknown type '%S'", name);
    break;
  }

  case SK_CONST_TYPE: {
    // Standalone `const T` → the underlying type (const-ness is a binding
    // property in DefMeta, not an intern-pool type modifier).
    ConstType ct;
    if (ConstType_cast(node, &ct)) {
      SyntaxNode *inner = ConstType_inner(&ct);
      if (inner) {
        result = resolve_type_expr(ctx, inner);
        syntax_node_release(inner);
      }
    }
    break;
  }

  case SK_OPTIONAL_TYPE: {
    OptionalType ot;
    if (OptionalType_cast(node, &ot)) {
      SyntaxNode *inner = OptionalType_inner(&ot);
      IpIndex elem = resolve_type_expr(ctx, inner);
      if (inner)
        syntax_node_release(inner);
      if (elem.v != IP_NONE.v) {
        IpKey key = {.kind = IPK_OPTIONAL_TYPE,
                     .optional_type = {.elem = elem}};
        result = ip_get(&s->intern, key);
      }
    }
    break;
  }

  case SK_PTR_TYPE:
  case SK_SLICE_TYPE:
  case SK_MANY_PTR_TYPE: {
    SyntaxNode *child = NULL;
    if (k == SK_PTR_TYPE) {
      PtrType pt;
      PtrType_cast(node, &pt);
      child = PtrType_pointee(&pt);
    } else if (k == SK_SLICE_TYPE) {
      SliceType st;
      SliceType_cast(node, &st);
      child = SliceType_element(&st);
    } else {
      ManyPtrType mt;
      ManyPtrType_cast(node, &mt);
      child = ManyPtrType_element(&mt);
    }
    bool is_const = false;
    if (child && syntax_node_kind(child) == SK_CONST_TYPE) {
      ConstType inner;
      if (ConstType_cast(child, &inner)) {
        SyntaxNode *unwrap = ConstType_inner(&inner);
        is_const = true;
        syntax_node_release(child);
        child = unwrap;
      }
    }
    IpIndex elem = child ? resolve_type_expr(ctx, child) : IP_NONE;
    if (child)
      syntax_node_release(child);
    if (elem.v != IP_NONE.v) {
      IpKey key = {0};
      if (k == SK_PTR_TYPE) {
        key.kind = IPK_PTR_TYPE;
        key.ptr_type.elem = elem;
        key.ptr_type.is_const = is_const;
      } else if (k == SK_SLICE_TYPE) {
        key.kind = IPK_SLICE_TYPE;
        key.slice_type.elem = elem;
        key.slice_type.is_const = is_const;
      } else {
        key.kind = IPK_MANY_PTR_TYPE;
        key.many_ptr_type.elem = elem;
        key.many_ptr_type.is_const = is_const;
      }
      result = ip_get(&s->intern, key);
    }
    break;
  }

  case SK_ARRAY_TYPE: {
    // Literal-int sizes only; non-literal sizes need const_eval (deferred).
    ArrayType at;
    if (!ArrayType_cast(node, &at))
      break;
    SyntaxNode *size_node = ArrayType_size(&at);
    SyntaxNode *elem_node = ArrayType_element(&at);
    if (!size_node) {
      db_emit(s, DIAG_ERROR, span_of(ctx, node),
              "array type missing size expression");
      if (elem_node)
        syntax_node_release(elem_node);
      break;
    }
    Literal lit;
    if (!Literal_cast(size_node, &lit) || Literal_kind(&lit) != SK_INT_LIT) {
      db_emit(
          s, DIAG_ERROR, span_of(ctx, size_node),
          "array size must be a literal int (const_eval not yet implemented)");
      syntax_node_release(size_node);
      if (elem_node)
        syntax_node_release(elem_node);
      break;
    }
    SyntaxToken *size_tok = Literal_token(&lit);
    uint64_t size = size_tok ? parse_int_literal(size_tok) : 0;
    if (size_tok)
      syntax_token_release(size_tok);
    IpIndex elem = elem_node ? resolve_type_expr(ctx, elem_node) : IP_NONE;
    // (D2.4) the size literal's comptime-int hover type is a body-inference
    // concern; not pushed here.
    syntax_node_release(size_node);
    if (elem_node)
      syntax_node_release(elem_node);
    if (elem.v == IP_NONE.v)
      break;
    IpKey key = {.kind = IPK_ARRAY_TYPE,
                 .array_type = {.elem = elem, .size = size}};
    result = ip_get(&s->intern, key);
    break;
  }

  case SK_FN_TYPE: {
    FnType ft;
    if (!FnType_cast(node, &ft))
      break;
    SyntaxNode *ret = FnType_return_type(&ft);
    SyntaxNode *params = FnType_params(&ft);
    result = build_fn_type(ctx, ret, params);
    if (ret)
      syntax_node_release(ret);
    if (params)
      syntax_node_release(params);
    break;
  }

  default:
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "type-expression kind %d not yet supported", (int)k);
    break;
  }

  node_type_builder_push(ctx, node, result);
  return result;
}

// ============================================================================
// build_struct_type / build_enum_type — nominal type construction.
// ============================================================================

// Build an SK_STRUCT_DECL / SK_UNION_DECL type. Identity is nominal (zir =
// def.idx) and inline-encoded, so `ip_get(IPK_STRUCT_TYPE, {zir})` returns a
// STABLE deduped index up front — published into the def's type cell BEFORE the
// field loop so a self/mutually-referential field (`^Self`) cycling back into
// type_of_def reads the cell (on_cycle) and gets this very index. The field
// list goes into db.struct_field_pool (recompute-friendly, decl_pool pattern),
// NOT the intern pool. Fields are resolved into a request-arena scratch first,
// then bulk-appended AFTER the loop — resolve_type_expr can recursively append
// other structs' fields to the same pool, so deferring our append keeps our
// range contiguous. A field that fails to resolve stores type=IP_NONE (the
// struct stays a valid nominal — no whole-type cancel). *fp_out =
// combine(IpIndex.v, ⊕ field name+type): the index is stable, so this content
// fold is what flips type_of_def's fp on a field-type edit.
static IpIndex build_struct_type(const SemaCtx *base, SyntaxNode *agg,
                                 DefId def, Fingerprint *fp_out) {
  struct db *s = base->s;
  DefKind dk = db_def_kind(s, def); // KIND_STRUCT or KIND_UNION

  IpIndex idx =
      ip_get(&s->intern, (IpKey){.kind = IPK_STRUCT_TYPE,
                                 .struct_type = {.zir_node_id = def.idx}});
  *db_def_type_cell(s, def) = idx; // self-reference anchor
  Fingerprint content = db_fp_u64((uint64_t)idx.v);

  SyntaxNode *field_list = NULL;
  SyntaxKind ak = syntax_node_kind(agg);
  if (ak == SK_STRUCT_DECL) {
    StructDef sd;
    if (StructDef_cast(agg, &sd))
      field_list = StructDef_fields(&sd);
  } else if (ak == SK_UNION_DECL) {
    UnionDef ud;
    if (UnionDef_cast(agg, &ud))
      field_list = UnionDef_variants(&ud);
  }

  uint32_t n_fields = 0;
  if (field_list) {
    uint32_t list_count = syntax_node_num_children(field_list);
    for (uint32_t i = 0; i < list_count; i++) {
      GreenElement g = green_node_child(syntax_node_green(field_list), i);
      if (g.kind == GREEN_ELEM_NODE && green_node_kind(g.node) == SK_FIELD)
        n_fields++;
    }
  }
  AggregateFieldEntry *scratch =
      n_fields ? arena_alloc(&s->request_arena,
                             n_fields * sizeof(AggregateFieldEntry))
               : NULL;

  // Per-field-annotation hover types are stored only for structs
  // (db.structs.type_result.field_node_types); unions have no such column, so
  // skip the builder for them — a NULL ctx->types makes node_type_builder_push
  // a no-op (no wasted hover map). Member-list storage happens for both.
  bool want_hover = (dk == KIND_STRUCT);
  NodeTypeBuilder fb;
  if (want_hover)
    node_type_builder_begin(s, &fb, base->file_local);
  SemaCtx fctx = {.s = s,
                  .file_green_root = base->file_green_root,
                  .nsid = base->nsid,
                  .enclosing_fn = DEF_ID_NONE,
                  .file_local = base->file_local,
                  .types = want_hover ? &fb : NULL};

  uint32_t fout = 0;
  if (field_list) {
    uint32_t list_count = syntax_node_num_children(field_list);
    for (uint32_t i = 0; i < list_count; i++) {
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
      if (fname_tok)
        syntax_token_release(fname_tok);
      // A failed field type stays IP_NONE — the aggregate is still a valid
      // nominal type, just with one ill-typed member (resolve_type_expr
      // already emitted the diag).
      IpIndex ftypei = ftype ? resolve_type_expr(&fctx, ftype) : IP_NONE;
      if (ftype)
        syntax_node_release(ftype);
      node_type_builder_push(&fctx, el.node, ftypei); // no-op for unions
      scratch[fout++] = (AggregateFieldEntry){.name = fname, .type = ftypei};
      content =
          db_fp_combine(content, db_fp_combine(db_fp_u64((uint64_t)fname.idx),
                                               db_fp_u64((uint64_t)ftypei.v)));
      syntax_node_release(el.node);
    }
    syntax_node_release(field_list);
  }

  // Bulk-append the member range to the shared pool AFTER resolution (nested
  // type_of_def appends to the same pool during field resolution, so deferring
  // keeps our range contiguous), then stamp (field_lo,field_len) on the
  // struct or union table.
  uint32_t lo = (uint32_t)s->aggregate_field_pool.count;
  for (uint32_t i = 0; i < fout; i++)
    vec_push(&s->aggregate_field_pool, &scratch[i]);
  uint32_t row = db_def_row(s, def, dk);
  if (dk == KIND_STRUCT) {
    *(uint32_t *)paged_get(&s->structs.field_lo, row) = lo;
    *(uint32_t *)paged_get(&s->structs.field_len, row) = fout;
  } else { // KIND_UNION
    *(uint32_t *)paged_get(&s->unions.field_lo, row) = lo;
    *(uint32_t *)paged_get(&s->unions.field_len, row) = fout;
  }

  if (want_hover) {
    NodeTypesRange field_range = node_type_builder_end(&fb, NULL);
    type_of_decl_node_types_write(s, def, field_range);
  }

  *fp_out = content;
  return idx;
}

// Build an SK_ENUM_DECL type. Nominal stable index (zir) + variants in
// db.enum_variant_pool. Variant values: explicit bare-literal-int, else
// auto-numbered. *fp_out = combine(IpIndex.v, ⊕ variant name+value).
static IpIndex build_enum_type(const SemaCtx *base, SyntaxNode *enum_node,
                               DefId def, Fingerprint *fp_out) {
  struct db *s = base->s;

  // Stable nominal index. No type-cell publish needed (enums have no
  // self-referential variants); type_of_def's epilogue writes the cell.
  IpIndex idx =
      ip_get(&s->intern, (IpKey){.kind = IPK_ENUM_TYPE,
                                 .enum_type = {.zir_node_id = def.idx}});
  Fingerprint content = db_fp_u64((uint64_t)idx.v);

  EnumDef ed;
  SyntaxNode *variants_list = NULL;
  if (EnumDef_cast(enum_node, &ed))
    variants_list = EnumDef_variants(&ed);

  uint32_t n_variants = 0;
  if (variants_list) {
    uint32_t list_count = syntax_node_num_children(variants_list);
    for (uint32_t i = 0; i < list_count; i++) {
      GreenElement g = green_node_child(syntax_node_green(variants_list), i);
      if (g.kind == GREEN_ELEM_NODE && green_node_kind(g.node) == SK_VARIANT)
        n_variants++;
    }
  }
  EnumVariantEntry *scratch =
      n_variants ? arena_alloc(&s->request_arena,
                               n_variants * sizeof(EnumVariantEntry))
                 : NULL;

  uint32_t vout = 0;
  if (variants_list) {
    uint32_t list_count = syntax_node_num_children(variants_list);
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
      StrId vname = intern_tok(s, vname_tok);
      if (vname_tok)
        syntax_token_release(vname_tok);
      int64_t val = v_val ? literal_int_signed(v_val) : (int64_t)vout;
      if (v_val)
        syntax_node_release(v_val);
      scratch[vout++] = (EnumVariantEntry){.name = vname, .value = val};
      content =
          db_fp_combine(content, db_fp_combine(db_fp_u64((uint64_t)vname.idx),
                                               db_fp_u64((uint64_t)val)));
      syntax_node_release(el.node);
    }
    syntax_node_release(variants_list);
  }

  uint32_t lo = (uint32_t)s->enum_variant_pool.count;
  for (uint32_t i = 0; i < vout; i++)
    vec_push(&s->enum_variant_pool, &scratch[i]);
  uint32_t row = db_def_row(s, def, KIND_ENUM);
  *(uint32_t *)paged_get(&s->enums.variant_lo, row) = lo;
  *(uint32_t *)paged_get(&s->enums.variant_len, row) = vout;

  *fp_out = content;
  return idx;
}

// ============================================================================
// FN_SIGNATURE — a function's (params → ret) type + per-param hover types.
// ============================================================================

const FnSignature *db_query_fn_signature(db_query_ctx *ctx, DefId def) {
  struct db *s = (struct db *)ctx;
  // FN_SIGNATURE is KIND_FUNCTION-only at the routing layer; refuse non-fns
  // BEFORE the guard so the query is TOTAL (a non-fn caller gets NULL, not the
  // db_query_begin "slot kind not wired" assert).
  if (db_def_kind(s, def) != KIND_FUNCTION)
    return NULL;
  DB_QUERY_GUARD(ctx, QUERY_FN_SIGNATURE, (uint64_t)def.idx,
                 /* on_cached */ fn_signature_read(s, def),
                 /* on_cycle  */ NULL,
                 /* on_error  */ NULL);

  StrId name = *(StrId *)vec_get(&s->defs.names, def.idx);
  NamespaceId nsid = *(NamespaceId *)vec_get(&s->defs.parent_modules, def.idx);

  IpIndex fn_ty = IP_NONE;
  NodeTypesRange sig_range = {0};

  TopLevelEntry e = db_query_top_level_entry(ctx, nsid, name); // CONTENT dep
  if (e.node_ptr.kind != SYNTAX_KIND_NONE) {
    uint32_t local = file_id_local(e.file);
    struct GreenNode *groot =
        *(struct GreenNode **)vec_get(&s->files.green_roots, local);
    if (groot) {
      SyntaxTree *tree = syntax_tree_new(groot);
      SyntaxNode *rroot = syntax_tree_root(tree);
      SyntaxNode *wrapper = syntax_node_ptr_resolve(e.node_ptr, rroot);
      syntax_node_release(rroot);
      if (wrapper) {
        SyntaxNode *value = bind_value(wrapper); // SK_LAMBDA_EXPR
        LambdaExpr lam;
        if (value && LambdaExpr_cast(value, &lam)) {
          SyntaxNode *params = LambdaExpr_params(&lam);
          SyntaxNode *ret_node = LambdaExpr_return_type(&lam);
          NodeTypeBuilder sb;
          node_type_builder_begin(s, &sb, e.file);
          SemaCtx sctx = {.s = s,
                          .file_green_root = groot,
                          .nsid = nsid,
                          .enclosing_fn = def,
                          .file_local = e.file,
                          .types = &sb};
          fn_ty = build_fn_type(&sctx, ret_node, params);
          sig_range = node_type_builder_end(&sb, NULL);
          if (params)
            syntax_node_release(params);
          if (ret_node)
            syntax_node_release(ret_node);
        }
        if (value)
          syntax_node_release(value);
        syntax_node_release(wrapper);
      }
      syntax_tree_free(tree);
    }
  }

  FnSignature result = {.type = fn_ty, .node_types = sig_range};
  fn_signature_write(s, def, result); // frees old node_types
  db_query_succeed(ctx, QUERY_FN_SIGNATURE, (uint64_t)def.idx,
                   ip_index_is_valid(fn_ty) ? db_fp_u64((uint64_t)fn_ty.v)
                                            : FINGERPRINT_NONE);
  return fn_signature_read(s, def);
}

// ============================================================================
// TYPE_OF_DECL — a decl's overall type.
// ============================================================================

IpIndex db_query_type_of_def(db_query_ctx *ctx, DefId def) {
  struct db *s = (struct db *)ctx;
  // Primitives (i32, bool, …) are seeded DefIds with no per-kind slot, so
  // they must short-circuit BEFORE the guard — otherwise db_query_begin
  // routes to a non-existent KIND_NONE slot. They're immutable: no dep, no
  // succeed. (resolve_ref → type_of_def reaches here for `x: i32`.)
  IpIndex prim = db_primitive_type_for(s, def);
  if (ip_index_is_valid(prim))
    return prim;
  // on_cycle returns the (wip-published) type cell, NOT IP_NONE — this is how
  // a self-referential nominal (`Node { next: ^Node }`) sees its own
  // in-progress IpIndex during the field loop.
  DB_QUERY_GUARD(ctx, QUERY_TYPE_OF_DECL, (uint64_t)def.idx,
                 /* on_cached */ type_of_decl_read(s, def),
                 /* on_cycle  */ type_of_decl_read(s, def),
                 /* on_error  */ IP_NONE);

  DefKind kind = db_def_kind(s, def);

  // Functions delegate to fn_signature (which owns its own top_level_entry
  // dep). No wrapper resolve here, and crucially no top_level_entry dep — so
  // type_of_def(fn) cuts off on a body edit (fn_signature's fp is stable).
  if (kind == KIND_FUNCTION) {
    const FnSignature *sig = db_query_fn_signature(ctx, def);
    IpIndex result = sig ? sig->type : IP_NONE;
    type_of_decl_write(s, def, result);
    db_query_succeed(ctx, QUERY_TYPE_OF_DECL, (uint64_t)def.idx,
                     ip_index_is_valid(result) ? db_fp_u64((uint64_t)result.v)
                                               : FINGERPRINT_NONE);
    return result;
  }

  StrId name = *(StrId *)vec_get(&s->defs.names, def.idx);
  NamespaceId nsid = *(NamespaceId *)vec_get(&s->defs.parent_modules, def.idx);

  IpIndex result = IP_NONE;
  Fingerprint fp = FINGERPRINT_NONE;

  TopLevelEntry e = db_query_top_level_entry(ctx, nsid, name); // CONTENT dep
  if (e.node_ptr.kind != SYNTAX_KIND_NONE) {
    uint32_t local = file_id_local(e.file);
    struct GreenNode *groot =
        *(struct GreenNode **)vec_get(&s->files.green_roots, local);
    if (groot) {
      SyntaxTree *tree = syntax_tree_new(groot);
      SyntaxNode *rroot = syntax_tree_root(tree);
      SyntaxNode *wrapper = syntax_node_ptr_resolve(e.node_ptr, rroot);
      syntax_node_release(rroot);
      if (wrapper) {
        SemaCtx base = {.s = s,
                        .file_green_root = groot,
                        .nsid = nsid,
                        .enclosing_fn = DEF_ID_NONE,
                        .file_local = e.file,
                        .types = NULL};
        // Read meta from top_level_entry (THIS query's content-firewall
        // dep), not defs.meta: top_level_entry's structural-hash fp flips
        // on any modifier-token change (hash-cons token ptr = kind+text),
        // so this is correct-by-construction — type_of_def re-runs and
        // sees fresh meta without an implicit "def_identity ran first"
        // ordering on the defs.meta column.
        DefMeta meta = e.meta;

        if (kind == KIND_STRUCT || kind == KIND_UNION) {
          SyntaxNode *val = bind_value(wrapper); // SK_STRUCT/UNION_DECL
          if (val) {
            result = build_struct_type(&base, val, def, &fp);
            syntax_node_release(val);
          }
        } else if (kind == KIND_ENUM) {
          SyntaxNode *val = bind_value(wrapper); // SK_ENUM_DECL
          if (val) {
            result = build_enum_type(&base, val, def, &fp);
            syntax_node_release(val);
          }
        } else if ((kind == KIND_CONSTANT || kind == KIND_VARIABLE) &&
                   !(meta & META_DISTINCT)) {
          // Typed bind: the annotation is the type. RHS check + inferred
          // binds (no annotation) are D2.4.
          SyntaxNode *annot = bind_type_annot(wrapper);
          if (annot) {
            NodeTypeBuilder vb;
            node_type_builder_begin(s, &vb, e.file);
            SemaCtx vctx = base;
            vctx.types = &vb;
            result = resolve_type_expr(&vctx, annot);
            NodeTypesRange vr = node_type_builder_end(&vb, NULL);
            type_of_decl_node_types_write(s, def, vr);
            syntax_node_release(annot);
            fp = ip_index_is_valid(result) ? db_fp_u64((uint64_t)result.v)
                                           : FINGERPRINT_NONE;
          } else {
            // Inferred bind (no annotation): the type is the RHS's
            // type (e.g. `x :: 42` → comptime_int). enclosing_fn =
            // NONE → no body scope; refs resolve top-level.
            SyntaxNode *val = bind_value(wrapper);
            if (val) {
              NodeTypeBuilder vb;
              node_type_builder_begin(s, &vb, e.file);
              SemaCtx vctx = base;
              vctx.types = &vb;
              result = type_of_expr(&vctx, val);
              NodeTypesRange vr = node_type_builder_end(&vb, NULL);
              type_of_decl_node_types_write(s, def, vr);
              syntax_node_release(val);
              fp = ip_index_is_valid(result) ? db_fp_u64((uint64_t)result.v)
                                             : FINGERPRINT_NONE;
            }
          }
        }
        // KIND_EFFECT/KIND_HANDLER + distinct binds: IP_NONE (D2.4+).
        syntax_node_release(wrapper);
      }
      syntax_tree_free(tree);
    }
  }

  // For nominals the cell already holds the wip index (== result); for binds
  // this records the annotation type. Either way the slot's result column is
  // current before succeed.
  type_of_decl_write(s, def, result);
  db_query_succeed(ctx, QUERY_TYPE_OF_DECL, (uint64_t)def.idx, fp);
  return result;
}

// ============================================================================
// NAMESPACE_TYPE — the file-as-namespace export type (Phase D2.2).
//
// A namespace's type is the nominal struct whose members are its `pub`
// top-level decls. Identity is the nsid (inline-encoded in the pool, stable +
// deduped — like struct/enum). The exported (name → DefId) member list lives
// in db.namespace_field_pool (range stamped on db.namespaces.(field_lo,len)),
// NOT the intern pool. Member TYPES are resolved LAZILY by consumers via
// db_query_type_of_def(member.def) — so namespace_type depends only on
// NAMESPACE_ITEMS (membership, incl. meta/pub) + def_identity per pub member,
// and is firewalled from body edits. fp folds (member name, member def), so it
// flips on add/remove/rename/visibility-toggle of a pub decl, stable otherwise.
// ============================================================================

IpIndex db_query_namespace_type(db_query_ctx *ctx, NamespaceId nsid) {
  struct db *s = (struct db *)ctx;
  DB_QUERY_GUARD(ctx, QUERY_NAMESPACE_TYPE, (uint64_t)nsid.idx,
                 /* on_cached */ namespace_type_read(s, nsid),
                 /* on_cycle  */ IP_NONE,
                 /* on_error  */ IP_NONE);

  FileArray items = db_query_namespace_items(ctx, nsid); // membership dep
  const NamespaceItem *arr = (const NamespaceItem *)items.data;

  // Append each pub member (name → DefId) directly into the shared pool — the
  // range stays contiguous because nothing else appends to namespace_field_pool
  // during this loop (db_query_def_identity only touches the defs table). lo is
  // captured before the loop; (field_lo,field_len) stamped after.
  uint32_t lo = (uint32_t)s->namespace_field_pool.count;
  Fingerprint content = FINGERPRINT_NONE;
  for (uint32_t i = 0; i < items.count; i++) {
    if ((arr[i].meta & META_VIS_MASK) != VIS_PUBLIC)
      continue;
    DefId def = db_query_def_identity(ctx, nsid, arr[i].id); // identity dep
    DeclEntry m = {.name = arr[i].name, .def = def};
    vec_push(&s->namespace_field_pool, &m);
    content = db_fp_combine(content,
                            db_fp_combine(db_fp_u64((uint64_t)arr[i].name.idx),
                                          db_fp_u64((uint64_t)def.idx)));
  }
  *(uint32_t *)vec_get(&s->namespaces.field_lo, nsid.idx) = lo;
  *(uint32_t *)vec_get(&s->namespaces.field_len, nsid.idx) =
      (uint32_t)s->namespace_field_pool.count - lo;

  IpIndex result =
      ip_get(&s->intern, (IpKey){.kind = IPK_NAMESPACE_TYPE,
                                 .namespace_type = {.nsid = nsid}});
  namespace_type_write(s, nsid, result);
  // fp = the (name,def) content fold ONLY — for an inline nominal result.v is a
  // per-nsid constant, so folding it would add no discrimination.
  db_query_succeed(ctx, QUERY_NAMESPACE_TYPE, (uint64_t)nsid.idx, content);
  return result;
}
