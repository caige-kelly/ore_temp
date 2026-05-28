#include "../ast/ast_expr.h"
#include "../ast/ast_type.h"
#include "../db/db.h"
#include "../db/diag/diag.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/resolve_ref.h"
#include "../db/query/type_of_def.h"
#include "../syntax/syntax_kind.h"
#include "../support/data_structure/stringpool.h"
#include "../syntax/syntax.h"
#include "sema.h"

#include <stdlib.h>
#include <string.h>

// User-defined identifier resolution via the module's internal scope.
// Records the salsa deps on resolve_ref + type_of_def for the resolved
// def, so renaming/removing/retyping that decl correctly invalidates
// any query that called us.
static IpIndex resolve_user_type_name(struct db *s, NamespaceId nsid,
                                      StrId name) {
  if (name.idx == 0)
    return IP_NONE;
  // TODO(phase-D): route through db_query_namespace_scopes result column.
  ScopeId internal = db_get_namespace_internal_scope(s, nsid);
  if (internal.idx == SCOPE_ID_NONE.idx)
    return IP_NONE;
  DefId target = db_query_resolve_ref(s, internal, name);
  if (target.idx == DEF_ID_NONE.idx)
    return IP_NONE;
  return db_query_type_of_def(s, target);
}

// Forward decl — defined in sema/fn_signature.c since it's the natural
// sibling of build_fn_type's lambda-driving call site.
IpIndex sema_build_fn_type(const SemaCtx *ctx, SyntaxNode *ret_node,
                           SyntaxNode *param_list);

// Intern a SyntaxToken's text. Returns {0} on NULL.
static StrId intern_token_text(struct db *s, SyntaxToken *t) {
  if (!t)
    return (StrId){0};
  const char *txt = syntax_token_text(t);
  uint32_t len = syntax_token_text_range(t).length;
  return pool_intern(&s->strings, txt, len);
}

// Read an int literal's u64 value via the Literal wrapper. Caller
// passes the inner SK_INT_LIT token from Literal_token. strtoull
// handles 0x/0b/0o prefixes; underscores are stripped first.
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

// PATH_EXPR has no single name token; sema's resolve_path_lookup will
// own the dotted-segments walk in a future port. For now, in type
// position we use the LAST IDENT child as the leaf name. This is the
// one place we navigate raw — PathExpr's wrapper deliberately exposes
// no name() because there isn't one (it's a sequence).
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

// Build a reparse-stable DiagAnchor for `node` so diags resolve to
// source even when fine-grained memoization caches them across edits.
static DiagAnchor span_of(const SemaCtx *ctx, SyntaxNode *node) {
  return diag_anchor_of_node((uint16_t)ctx->file_local.idx, node);
}

IpIndex sema_resolve_type_expr(const SemaCtx *ctx, SyntaxNode *node) {
  if (!node)
    return IP_NONE;
  struct db *s = ctx->s;
  NamespaceId nsid = ctx->nsid;
  SyntaxKind k = syntax_node_kind(node);

  IpIndex result = IP_NONE;

  switch (k) {
  // (void/noreturn/type/anytype used to be dedicated AST kinds. They
  //  now lex as plain identifiers and resolve through the REF/PATH
  //  case below — primitives are real DefIds in every namespace's
  //  parent scope, so the same resolve_ref → type_of_def chain that
  //  handles user types finds them.)
  // SK_REF_TYPE is what parser_new emits for a bare-ident type
  // (e.g., `f32` after `:`). SK_REF_EXPR appears here only when a
  // hand-crafted tree puts an expression-shaped name in type
  // position. Both have the same green-tree shape — a single SK_IDENT
  // child — so we extract it directly via ast_first_token instead of
  // going through a kind-specific cast.
  case SK_REF_TYPE:
  case SK_REF_EXPR: {
    SyntaxToken *name_tok = ast_first_token(node, SK_IDENT);
    StrId name = intern_token_text(s, name_tok);
    if (name_tok) syntax_token_release(name_tok);
    result = resolve_user_type_name(s, nsid, name);
    break;
  }
  // SK_PATH_TYPE is the multi-segment counterpart for type position.
  // The leaf-name walker (last SK_IDENT direct-child token) works for
  // both kinds, since the green-tree shape is the same.
  case SK_PATH_TYPE:
  case SK_PATH_EXPR:
    result = resolve_user_type_name(s, nsid, path_expr_leaf_name(s, node));
    break;

  case SK_CONST_TYPE: {
    // Standalone `const T` (not folded into a PTR/SLICE/MANYPTR
    // parent) is treated as the underlying type — const-ness in this
    // position is a binding property carried in DefMeta, not a type
    // modifier the intern pool needs to encode.
    ConstType ct;
    if (ConstType_cast(node, &ct)) {
      SyntaxNode *inner = ConstType_inner(&ct);
      if (inner) {
        result = sema_resolve_type_expr(ctx, inner);
        syntax_node_release(inner);
      }
    }
    break;
  }

  case SK_OPTIONAL_TYPE: {
    OptionalType ot;
    if (OptionalType_cast(node, &ot)) {
      SyntaxNode *inner = OptionalType_inner(&ot);
      IpIndex elem = sema_resolve_type_expr(ctx, inner);
      if (inner)
        syntax_node_release(inner);
      if (elem.v != IP_NONE.v) {
        IpKey key = {.kind = IPK_OPTIONAL_TYPE, .optional_type = {.elem = elem}};
        result = ip_get(&s->intern, key);
      }
    }
    break;
  }

  case SK_PTR_TYPE:
  case SK_SLICE_TYPE:
  case SK_MANY_PTR_TYPE: {
    // Const-as-modifier handling: a wrapping SK_CONST_TYPE under the
    // constructor folds into is_const=true on the constructor's IpKey
    // (giving ^const T / []const T / [^]const T canonical forms).
    SyntaxNode *child = NULL;
    if (k == SK_PTR_TYPE) {
      PtrType pt; PtrType_cast(node, &pt); child = PtrType_pointee(&pt);
    } else if (k == SK_SLICE_TYPE) {
      SliceType st; SliceType_cast(node, &st); child = SliceType_element(&st);
    } else {
      ManyPtrType mt; ManyPtrType_cast(node, &mt);
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
    IpIndex elem = child ? sema_resolve_type_expr(ctx, child) : IP_NONE;
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
    // Chunk 2 only handles the trivial bare-literal-int size case;
    // non-literal sizes need const_eval (deferred).
    ArrayType at;
    if (!ArrayType_cast(node, &at))
      break;
    SyntaxNode *size_node = ArrayType_size(&at);
    SyntaxNode *elem_node = ArrayType_element(&at);
    if (!size_node) {
      db_emit(s, DIAG_ERROR, span_of(ctx, node),
              "array type missing size expression");
      if (elem_node) syntax_node_release(elem_node);
      break;
    }
    // Only literal-int sizes for now. Other expression kinds need
    // const_eval (deferred).
    Literal lit;
    if (!Literal_cast(size_node, &lit) ||
        Literal_kind(&lit) != SK_INT_LIT) {
      db_emit(s, DIAG_ERROR, span_of(ctx, size_node),
              "array size must be a literal int (const_eval not yet "
              "implemented)");
      syntax_node_release(size_node);
      if (elem_node) syntax_node_release(elem_node);
      break;
    }
    SyntaxToken *size_tok = Literal_token(&lit);
    uint64_t size = size_tok ? parse_int_literal(size_tok) : 0;
    if (size_tok) syntax_token_release(size_tok);
    IpIndex elem = elem_node ? sema_resolve_type_expr(ctx, elem_node) : IP_NONE;
    // Walk the size literal through sema_type_of_expr so the
    // IPK_COMPTIME_INT type lands in the per-node cache for hover.
    (void)sema_type_of_expr(ctx, size_node);
    syntax_node_release(size_node);
    if (elem_node) syntax_node_release(elem_node);
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
    result = sema_build_fn_type(ctx, ret, params);
    if (ret) syntax_node_release(ret);
    if (params) syntax_node_release(params);
    break;
  }

  default:
    // Type-expression kind sema doesn't know how to resolve yet
    // (effect-row decls, error-union `!T`, distinct constructors).
    // Emit a diagnostic so the failure is loud — the caller would
    // otherwise see IP_NONE silently propagate.
    db_emit(s, DIAG_ERROR, span_of(ctx, node),
            "type-expression kind %d not yet supported", (int)k);
    break;
  }

  sema_node_type_builder_push(ctx, node, result);
  return result;
}
