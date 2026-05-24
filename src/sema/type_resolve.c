#include "../db/db.h"
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
static IpIndex resolve_user_type_name(struct db *s, NamespaceId nsid, StrId name) {
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
IpIndex sema_build_fn_type(struct db *s, ASTStore *ast, AstNodeId ret_node,
                           const uint32_t *param_ids, uint32_t n_params,
                           NamespaceId nsid, FileId file_local);

// Compute the type-expr's IpIndex into `result`, then a single trailing
// sema_cache_node_type stamps every visited type-expr node into the
// per-node cache. The cache write is the load-bearing change for L2:
// hover / completion / any IDE consumer that reads
// FileNodeData.types[node.idx] now gets a real answer for every node
// the typecheck reached (AST_TYPE_PTR, AST_TYPE_ARRAY, AST_EXPR_PATH-
// in-type-position, etc.). No more "is the cache populated for this
// kind?" guessing at the consumer.
IpIndex sema_resolve_type_expr(struct db *s, ASTStore *ast, AstNodeId id,
                               NamespaceId nsid, FileId file_local) {
  if (id.idx == AST_NODE_ID_NONE.idx)
    return IP_NONE;
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
    result = sema_resolve_type_expr(s, ast, d.single_child, nsid, file_local);
    break;

  case AST_TYPE_OPTIONAL: {
    IpIndex elem =
        sema_resolve_type_expr(s, ast, d.single_child, nsid, file_local);
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
    IpIndex elem = sema_resolve_type_expr(s, ast, child, nsid, file_local);
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
    if (size_id.idx == AST_NODE_ID_NONE.idx)
      break;
    AstNodeKind size_k = ((AstNodeKind *)ast->kinds.data)[size_id.idx];
    if (size_k != AST_EXPR_LIT_INT)
      break;
    AstNodeData size_d = ((AstNodeData *)ast->data.data)[size_id.idx];
    const char *size_str = pool_get(&s->strings, size_d.string_id);
    if (!size_str)
      break;
    uint64_t size = strtoull(size_str, NULL, 0);
    IpIndex elem = sema_resolve_type_expr(s, ast, elem_id, nsid, file_local);
    if (elem.v == IP_NONE.v)
      break;
    // Walk the size literal through sema_type_of_expr so the IPK_COMPTIME_INT
    // type lands in the per-node cache for hover. file_local comes from
    // the caller now (post-L2 threading) so no need to re-derive from nsid.
    (void)sema_type_of_expr(s, ast, size_id, nsid, DEF_ID_NONE, file_local);
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
    result = sema_build_fn_type(s, ast, ret_node, param_ids, param_count, nsid,
                                file_local);
    break;
  }

  default:
    break;
  }

  sema_cache_node_type(s, file_local, id, result);
  return result;
}
