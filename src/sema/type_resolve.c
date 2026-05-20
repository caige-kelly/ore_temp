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

StrId sema_decl_name_from_node(ASTStore *ast, uint32_t name_node_idx) {
  if (name_node_idx == AST_NODE_ID_NONE.idx)
    return (StrId){0};
  AstNodeKind nk = ((AstNodeKind *)ast->kinds.data)[name_node_idx];
  if (nk != AST_EXPR_PATH)
    return (StrId){0};
  AstNodeData nd = ((AstNodeData *)ast->data.data)[name_node_idx];
  return nd.string_id;
}

// Identifier-spelled primitive type names. First-letter switch + strcmp
// across the ip_primitives.def set. Returns IP_NONE on miss (caller then
// falls through to user-type resolution via the scope/resolve_ref chain).
static IpIndex lookup_primitive_name(StringPool *sp, StrId id) {
  if (id.idx == 0)
    return IP_NONE;
  const char *s = pool_get(sp, id);
  if (!s)
    return IP_NONE;
  switch (s[0]) {
  case 'b':
    if (!strcmp(s, "bool"))
      return IP_BOOL_TYPE;
    break;
  case 'c':
    if (!strcmp(s, "comptime_int"))
      return IP_COMPTIME_INT_TYPE;
    if (!strcmp(s, "comptime_float"))
      return IP_COMPTIME_FLOAT_TYPE;
    break;
  case 'e':
    if (!strcmp(s, "error"))
      return IP_ERROR_TYPE;
    break;
  case 'f':
    if (!strcmp(s, "f32"))
      return IP_F32_TYPE;
    if (!strcmp(s, "f64"))
      return IP_F64_TYPE;
    break;
  case 'i':
    if (!strcmp(s, "i8"))
      return IP_I8_TYPE;
    if (!strcmp(s, "i16"))
      return IP_I16_TYPE;
    if (!strcmp(s, "i32"))
      return IP_I32_TYPE;
    if (!strcmp(s, "i64"))
      return IP_I64_TYPE;
    if (!strcmp(s, "isize"))
      return IP_ISIZE_TYPE;
    break;
  case 'n':
    if (!strcmp(s, "noreturn"))
      return IP_NORETURN_TYPE;
    if (!strcmp(s, "nil"))
      return IP_NIL_TYPE;
    break;
  case 't':
    if (!strcmp(s, "type"))
      return IP_TYPE_TYPE;
    break;
  case 'u':
    if (!strcmp(s, "u8"))
      return IP_U8_TYPE;
    if (!strcmp(s, "u16"))
      return IP_U16_TYPE;
    if (!strcmp(s, "u32"))
      return IP_U32_TYPE;
    if (!strcmp(s, "u64"))
      return IP_U64_TYPE;
    if (!strcmp(s, "usize"))
      return IP_USIZE_TYPE;
    break;
  case 'v':
    if (!strcmp(s, "void"))
      return IP_VOID_TYPE;
    break;
  }
  return IP_NONE;
}

// User-defined identifier resolution via the module's internal scope.
// Records the salsa deps on resolve_ref + type_of_def for the resolved
// def, so renaming/removing/retyping that decl correctly invalidates
// any query that called us.
static IpIndex resolve_user_type_name(struct db *s, ModuleId mid, StrId name) {
  if (name.idx == 0)
    return IP_NONE;
  if (mid.idx >= s->modules.internal_scopes.count)
    return IP_NONE;
  ScopeId internal =
      *(ScopeId *)vec_get(&s->modules.internal_scopes, mid.idx);
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
                           ModuleId mid);

IpIndex sema_resolve_type_expr(struct db *s, ASTStore *ast, AstNodeId id,
                               ModuleId mid) {
  if (id.idx == AST_NODE_ID_NONE.idx)
    return IP_NONE;
  AstNodeKind k = ((AstNodeKind *)ast->kinds.data)[id.idx];
  AstNodeData d = ((AstNodeData *)ast->data.data)[id.idx];

  switch (k) {
  case AST_TYPE_VOID:
    return IP_VOID_TYPE;
  case AST_TYPE_NORETURN:
    return IP_NORETURN_TYPE;
  case AST_TYPE_TYPE:
    return IP_TYPE_TYPE;
  case AST_TYPE_ANYTYPE:
    return IP_NONE; // no IpIndex for anytype today

  case AST_EXPR_PATH: {
    IpIndex p = lookup_primitive_name(&s->strings, d.string_id);
    if (p.v != IP_NONE.v)
      return p;
    return resolve_user_type_name(s, mid, d.string_id);
  }

  case AST_TYPE_CONST:
    // Standalone `const T` (not folded into a PTR/SLICE/MANYPTR
    // parent) is treated as the underlying type — const-ness in this
    // position is a binding property carried in DefMeta, not a type
    // modifier the intern pool needs to encode.
    return sema_resolve_type_expr(s, ast, d.single_child, mid);

  case AST_TYPE_OPTIONAL: {
    IpIndex elem = sema_resolve_type_expr(s, ast, d.single_child, mid);
    if (elem.v == IP_NONE.v)
      return IP_NONE;
    IpKey key = {.kind = IPK_OPTIONAL_TYPE,
                 .optional_type = {.elem = elem}};
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
    IpIndex elem = sema_resolve_type_expr(s, ast, child, mid);
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
    IpIndex elem = sema_resolve_type_expr(s, ast, elem_id, mid);
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
    return sema_build_fn_type(s, ast, ret_node, param_ids, param_count, mid);
  }

  default:
    return IP_NONE;
  }
}
