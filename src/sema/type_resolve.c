#include "../db/db.h"
#include "../db/diag/diag.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/resolve_ref.h"
#include "../db/query/type_of_def.h"
#include "../db/storage/stringpool.h"
#include "../parser/ast.h"
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
IpIndex sema_build_fn_type(const SemaCtx *ctx, AstNodeId ret_node,
                           const uint32_t *param_ids, uint32_t n_params);

// Compute the type-expr's IpIndex into `result`, then a single trailing
// sema_node_type_builder_push (via the ctx's active builder) stamps
// every visited type-expr node. Per-decl queries own the builder and
// the unified router (db_query_node_type) reads it back.
IpIndex sema_resolve_type_expr(const SemaCtx *ctx, AstNodeId id) {
  if (id.idx == AST_NODE_ID_NONE.idx)
    return IP_NONE;
  // Locals named to match pre-refactor body code — avoids a sweep
  // through the switch's many references.
  struct db *s = ctx->s;
  ASTStore *ast = ctx->ast;
  NamespaceId nsid = ctx->nsid;
  AstNodeKind k = ((AstNodeKind *)ast->kinds.data)[id.idx];
  AstNodeData d = ((AstNodeData *)ast->data.data)[id.idx];

  IpIndex result = IP_NONE;

  switch (k) {
    // (void/noreturn/type/anytype used to be dedicated AST kinds here.
    //  They lex as plain identifiers now and resolve through the
    //  AST_EXPR_PATH case below; primitives are real DefIds in every
    //  namespace's parent scope (db_init_primitives), so the same
    //  resolve_ref → type_of_def chain that handles user types finds
    //  them. No separate primitive lookup table.)

  case AST_EXPR_PATH:
    result = resolve_user_type_name(s, nsid, d.string_id);
    break;

  case AST_TYPE_CONST:
    // Standalone `const T` (not folded into a PTR/SLICE/MANYPTR
    // parent) is treated as the underlying type — const-ness in this
    // position is a binding property carried in DefMeta, not a type
    // modifier the intern pool needs to encode.
    result = sema_resolve_type_expr(ctx, d.single_child);
    break;

  case AST_TYPE_OPTIONAL: {
    IpIndex elem = sema_resolve_type_expr(ctx, d.single_child);
    if (elem.v != IP_NONE.v) {
      IpKey key = {.kind = IPK_OPTIONAL_TYPE, .optional_type = {.elem = elem}};
      result = ip_get(&s->intern, key);
    }
    break;
  }

  case AST_TYPE_PTR:
  case AST_TYPE_SLICE:
  case AST_TYPE_MANYPTR: {
    // Const-as-modifier handling: a wrapping AST_TYPE_CONST under the
    // constructor folds into is_const=true on the constructor's IpKey
    // (giving ^const T / []const T / [^]const T canonical forms). A
    // standalone CONST is handled by the case above.
    AstNodeId child = d.single_child;
    bool is_const = false;
    if (child.idx != AST_NODE_ID_NONE.idx) {
      AstNodeKind ck = ((AstNodeKind *)ast->kinds.data)[child.idx];
      if (ck == AST_TYPE_CONST) {
        is_const = true;
        child = ((AstNodeData *)ast->data.data)[child.idx].single_child;
      }
    }
    IpIndex elem = sema_resolve_type_expr(ctx, child);
    if (elem.v != IP_NONE.v) {
      IpKey key = {0};
      if (k == AST_TYPE_PTR) {
        key.kind = IPK_PTR_TYPE;
        key.ptr_type.elem = elem;
        key.ptr_type.is_const = is_const;
      } else if (k == AST_TYPE_SLICE) {
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

  case AST_TYPE_ARRAY: {
    // Extras: [size_expr, elem_type]. Chunk 2 only handles the trivial
    // bare-literal-int size case; non-literal sizes need chunk 6
    // const_eval.
    uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    AstNodeId size_id = {.idx = ex[0]};
    AstNodeId elem_id = {.idx = ex[1]};
    if (size_id.idx == AST_NODE_ID_NONE.idx) {
      TinySpan span = db_get_node_span(s, ctx->file_local, id);
      if (span != TINYSPAN_NONE)
        db_emit(s, DIAG_ERROR, span, "array type missing size expression");
      break;
    }
    AstNodeKind size_k = ((AstNodeKind *)ast->kinds.data)[size_id.idx];
    if (size_k != AST_EXPR_LIT_INT) {
      TinySpan span = db_get_node_span(s, ctx->file_local, size_id);
      if (span != TINYSPAN_NONE) {
        db_emit(s, DIAG_ERROR, span,
                "array size must be a literal int (const_eval not yet "
                "implemented)");
      }
      break;
    }
    AstNodeData size_d = ((AstNodeData *)ast->data.data)[size_id.idx];
    const char *size_str = pool_get(&s->strings, size_d.string_id);
    if (!size_str)
      break;
    uint64_t size = strtoull(size_str, NULL, 0);
    IpIndex elem = sema_resolve_type_expr(ctx, elem_id);
    if (elem.v == IP_NONE.v)
      break;
    // Walk the size literal through sema_type_of_expr so the IPK_COMPTIME_INT
    // type lands in the per-node cache for hover. file_local comes from
    // the caller now (post-L2 threading) so no need to re-derive from nsid.
    (void)sema_type_of_expr(ctx, size_id);
    IpKey key = {.kind = IPK_ARRAY_TYPE,
                 .array_type = {.elem = elem, .size = size}};
    result = ip_get(&s->intern, key);
    break;
  }

  case AST_TYPE_FN: {
    // Anonymous fn type — extras [ret_id, effect_id, param_count, p0…].
    // Effects ignored for now (no field in IPK_FN_TYPE; chunk 8 wires
    // them).
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    AstNodeId ret_node = {.idx = ex[0]};
    uint32_t param_count = ex[2];
    const uint32_t *param_ids = &ex[3];
    result = sema_build_fn_type(ctx, ret_node, param_ids, param_count);
    break;
  }

  default: {
    // Type-expression kind sema doesn't know how to resolve yet
    // (effect-row decls, error-union `!T`, distinct constructors,
    // etc.). Emit a diagnostic so the failure is loud — the caller
    // would otherwise see IP_NONE silently propagate into struct
    // field types, fn signatures, etc.
    TinySpan span = db_get_node_span(s, ctx->file_local, id);
    if (span != TINYSPAN_NONE) {
      db_emit(s, DIAG_ERROR, span, "type-expression kind %s not yet supported",
              ast_kind_name(k));
    }
    break;
  }
  }

  sema_node_type_builder_push(ctx, id, result);

  return result;
}
