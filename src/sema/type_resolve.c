#include "../db/db.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/def_identity.h"
#include "../db/query/resolve_ref.h"
#include "../db/query/type_of_def.h"
#include "../db/storage/stringpool.h"
#include "../parser/ast.h"
#include "sema.h"

#include <stdlib.h>
#include <string.h>

// Identifier-spelled primitive type names. StrIds for these are pre-
// interned at db_init via PRIMITIVE_LIST and stored on s->names.X; the
// lookup is StrId.idx equality against those fields — no strcmp, no
// hashmap, just inline u32 compares. Compiler trees the dispatch.
//
// (StrId.idx is the BYTE OFFSET into the StringPool buffer, not a
// sequential index, so a direct PRIMITIVE_MAP[id.idx] lookup wouldn't
// hit. The s->names indirection is what makes this O(1)-ish without
// re-architecting the pool.)
//
// Returns IP_NONE on miss (caller then falls through to user-type
// resolution via the scope/resolve_ref chain).
static IpIndex lookup_primitive_name(struct db *s, StrId id) {
  if (id.idx == 0)
    return IP_NONE;
  uint32_t k = id.idx;
  if (k == s->names.BOOL.idx)
    return IP_BOOL_TYPE;
  if (k == s->names.U8.idx)
    return IP_U8_TYPE;
  if (k == s->names.I8.idx)
    return IP_I8_TYPE;
  if (k == s->names.U16.idx)
    return IP_U16_TYPE;
  if (k == s->names.I16.idx)
    return IP_I16_TYPE;
  if (k == s->names.U32.idx)
    return IP_U32_TYPE;
  if (k == s->names.I32.idx)
    return IP_I32_TYPE;
  if (k == s->names.U64.idx)
    return IP_U64_TYPE;
  if (k == s->names.I64.idx)
    return IP_I64_TYPE;
  if (k == s->names.F32.idx)
    return IP_F32_TYPE;
  if (k == s->names.F64.idx)
    return IP_F64_TYPE;
  if (k == s->names.USIZE.idx)
    return IP_USIZE_TYPE;
  if (k == s->names.ISIZE.idx)
    return IP_ISIZE_TYPE;
  if (k == s->names.VOID.idx)
    return IP_VOID_TYPE;
  if (k == s->names.NORETURN.idx)
    return IP_NORETURN_TYPE;
  if (k == s->names.TYPE_NAME.idx)
    return IP_TYPE_TYPE;
  if (k == s->names.ANYTYPE.idx)
    return IP_ANYTYPE_TYPE;
  if (k == s->names.COMPTIME_INT.idx)
    return IP_COMPTIME_INT_TYPE;
  if (k == s->names.COMPTIME_FLOAT.idx)
    return IP_COMPTIME_FLOAT_TYPE;
  if (k == s->names.ERROR_NAME.idx)
    return IP_ERROR_TYPE;
  return IP_NONE;
}

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
                           NamespaceId nsid);

IpIndex sema_resolve_type_expr(struct db *s, ASTStore *ast, AstNodeId id,
                               NamespaceId nsid) {
  if (id.idx == AST_NODE_ID_NONE.idx)
    return IP_NONE;
  AstNodeKind k = ((AstNodeKind *)ast->kinds.data)[id.idx];
  AstNodeData d = ((AstNodeData *)ast->data.data)[id.idx];

  switch (k) {
    // (void/noreturn/type/anytype used to be dedicated AST kinds here.
    //  They lex as plain identifiers now and resolve through the
    //  AST_EXPR_PATH case below via lookup_primitive_name.)

  case AST_EXPR_PATH: {
    IpIndex p = lookup_primitive_name(s, d.string_id);
    if (p.v != IP_NONE.v)
      return p;
    return resolve_user_type_name(s, nsid, d.string_id);
  }

  case AST_TYPE_CONST:
    // Standalone `const T` (not folded into a PTR/SLICE/MANYPTR
    // parent) is treated as the underlying type — const-ness in this
    // position is a binding property carried in DefMeta, not a type
    // modifier the intern pool needs to encode.
    return sema_resolve_type_expr(s, ast, d.single_child, nsid);

  case AST_TYPE_OPTIONAL: {
    IpIndex elem = sema_resolve_type_expr(s, ast, d.single_child, nsid);
    if (elem.v == IP_NONE.v)
      return IP_NONE;
    IpKey key = {.kind = IPK_OPTIONAL_TYPE, .optional_type = {.elem = elem}};
    return ip_get(&s->intern, key);
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
    IpIndex elem = sema_resolve_type_expr(s, ast, child, nsid);
    if (elem.v == IP_NONE.v)
      return IP_NONE;
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
    return ip_get(&s->intern, key);
  }

  case AST_TYPE_ARRAY: {
    // Extras: [size_expr, elem_type]. Chunk 2 only handles the trivial
    // bare-literal-int size case; non-literal sizes need chunk 6
    // const_eval.
    uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    AstNodeId size_id = {.idx = ex[0]};
    AstNodeId elem_id = {.idx = ex[1]};
    if (size_id.idx == AST_NODE_ID_NONE.idx)
      return IP_NONE;
    AstNodeKind size_k = ((AstNodeKind *)ast->kinds.data)[size_id.idx];
    if (size_k != AST_EXPR_LIT_INT)
      return IP_NONE;
    AstNodeData size_d = ((AstNodeData *)ast->data.data)[size_id.idx];
    const char *size_str = pool_get(&s->strings, size_d.string_id);
    if (!size_str)
      return IP_NONE;
    uint64_t size = strtoull(size_str, NULL, 0);
    IpIndex elem = sema_resolve_type_expr(s, ast, elem_id, nsid);
    if (elem.v == IP_NONE.v)
      return IP_NONE;
    IpKey key = {.kind = IPK_ARRAY_TYPE,
                 .array_type = {.elem = elem, .size = size}};
    return ip_get(&s->intern, key);
  }

  case AST_TYPE_FN: {
    // Anonymous fn type — extras [ret_id, effect_id, param_count, p0…].
    // Effects ignored for now (no field in IPK_FN_TYPE; chunk 8 wires
    // them).
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    AstNodeId ret_node = {.idx = ex[0]};
    uint32_t param_count = ex[2];
    const uint32_t *param_ids = &ex[3];
    return sema_build_fn_type(s, ast, ret_node, param_ids, param_count, nsid);
  }

  default:
    return IP_NONE;
  }
}
