#include "../../parser/ast.h"
#include "../db.h"
#include "../intern_pool/intern_pool.h"
#include "../storage/stringpool.h"
#include "../workspace/ast_id_map.h"
#include "ast.h"
#include "def_identity.h"
#include "fn_signature.h"
#include "query.h"
#include "query_engine.h"
#include "resolve_ref.h"
#include "type_of_def.h"

#include <stdlib.h>
#include <string.h>

// Hard cap on field/variant count for stack-local aggregate construction.
// The intern pool memcpys field arrays into its own arena, so the cap
// only bounds in-flight stack use, not the final type's size. 1024 fields
// is well beyond any realistic Ore type; if hit, we cancel the wip and
// return IP_NONE.
#define MAX_AGG_CHILDREN 1024
#define MAX_FN_PARAMS 256

// Forward declarations — the helpers below are mutually recursive
// through resolve_type_expr → resolve_user_type_name → db_query_type_of_def
// → build_struct/enum/fn_type → resolve_type_expr.
static IpIndex resolve_type_expr(struct db *s, ASTStore *ast, AstNodeId id,
                                 ModuleId mid);
static IpIndex resolve_user_type_name(struct db *s, ModuleId mid, StrId name);
static IpIndex build_fn_type(struct db *s, ASTStore *ast, AstNodeId ret_node,
                             const uint32_t *param_ids, uint32_t n_params,
                             ModuleId mid);

// Identifier-spelled primitive type names resolve here (e.g. `i32`,
// `u8`, `bool`). Ore follows Zig: these are universal identifiers
// (AST_EXPR_PATH), not dedicated tokens, so the primitives table is a
// straight name → IpIndex map kept here rather than in the intern
// pool (which is identity-by-payload, not by spelling).
//
// User-defined identifiers (struct/enum names like `Vec3`) miss this
// table and return IP_NONE — chunk 3 wires them through
// db_query_resolve_ref + def_identity → type chain.
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

// Resolve a user-spelled identifier (not a primitive name) by walking
// the module's internal scope via resolve_ref → def_identity → type.
// Each of those calls registers its own salsa dep, so renaming a decl
// or replacing its body correctly invalidates this type.
//
// Chunk 3 only walks the module's internal scope — function-local
// scopes and nested-type scopes are deferred.
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

// Recursively resolve a type-position AST expression to an IpIndex.
// Const-as-modifier handling: `^const T`, `[]const T`, `[^]const T`
// parse as a wrapping AST_TYPE_CONST under the constructor; we fold
// that into the constructor's is_const flag rather than emitting a
// separate IpIndex. A standalone AST_TYPE_CONST (no PTR/SLICE/MANYPTR
// parent) is treated as the underlying type — const-as-binding-property
// already lives in DefMeta.
//
// Identifier-spelled names (AST_EXPR_PATH) first try the primitives
// table; on miss, fall through to resolve_user_type_name which threads
// through the salsa graph (chunk 3).
//
// Still returns IP_NONE for: AST_TYPE_ANYTYPE, anonymous inline
// aggregates (chunk 4+), non-literal array sizes (chunk 6 const_eval),
// fn types in type position (chunk 4).
static IpIndex resolve_type_expr(struct db *s, ASTStore *ast, AstNodeId id,
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
    return IP_NONE;

  case AST_EXPR_PATH: {
    IpIndex p = lookup_primitive_name(&s->strings, d.string_id);
    if (p.v != IP_NONE.v)
      return p;
    return resolve_user_type_name(s, mid, d.string_id);
  }

  case AST_TYPE_CONST:
    return resolve_type_expr(s, ast, d.single_child, mid);

  case AST_TYPE_OPTIONAL: {
    IpIndex elem = resolve_type_expr(s, ast, d.single_child, mid);
    if (elem.v == IP_NONE.v)
      return IP_NONE;
    IpKey key = {.kind = IPK_OPTIONAL_TYPE, .optional_type = {.elem = elem}};
    return ip_get(&s->intern, key);
  }

  case AST_TYPE_PTR:
  case AST_TYPE_SLICE:
  case AST_TYPE_MANYPTR: {
    AstNodeId child = d.single_child;
    bool is_const = false;
    if (child.idx != AST_NODE_ID_NONE.idx) {
      AstNodeKind ck = ((AstNodeKind *)ast->kinds.data)[child.idx];
      if (ck == AST_TYPE_CONST) {
        is_const = true;
        child = ((AstNodeData *)ast->data.data)[child.idx].single_child;
      }
    }
    IpIndex elem = resolve_type_expr(s, ast, child, mid);
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
    // Extras: [size_expr, elem_type]. Size is const-eval'd in chunk 6;
    // chunk 2 only handles the trivial bare-literal-int case so
    // `[16]u8` etc. work today.
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
    IpIndex elem = resolve_type_expr(s, ast, elem_id, mid);
    if (elem.v == IP_NONE.v)
      return IP_NONE;
    IpKey key = {.kind = IPK_ARRAY_TYPE,
                 .array_type = {.elem = elem, .size = size}};
    return ip_get(&s->intern, key);
  }

  case AST_TYPE_FN: {
    // Anonymous fn type in type position (`Fn(T1, T2) <eff?> R`). Extras:
    // [ret_id, effect_id, param_count, p0, p1, ...]. Effects are ignored
    // for chunk 4 (no field in IPK_FN_TYPE) — chunk 8 wires them.
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    AstNodeId ret_node = {.idx = ex[0]};
    uint32_t param_count = ex[2];
    const uint32_t *param_ids = &ex[3];
    return build_fn_type(s, ast, ret_node, param_ids, param_count, mid);
  }

  default:
    return IP_NONE;
  }
}

// Build an interned fn type from a param-id array + return-type node.
// Shared by lambda RHS (db_query_fn_signature) and AST_TYPE_FN (type
// position). Identity is purely structural — (ret, modifiers, params) —
// so an anonymous fn type in a field position dedups with a top-level
// fn that happens to have the same shape. Modifiers stay 0 until chunk 7
// (comptime/async tags).
static IpIndex build_fn_type(struct db *s, ASTStore *ast, AstNodeId ret_node,
                             const uint32_t *param_ids, uint32_t n_params,
                             ModuleId mid) {
  if (n_params > MAX_FN_PARAMS)
    return IP_NONE;

  IpIndex params[MAX_FN_PARAMS];
  for (uint32_t i = 0; i < n_params; i++) {
    AstNodeId param_id = {.idx = param_ids[i]};
    if (param_id.idx == AST_NODE_ID_NONE.idx)
      return IP_NONE;
    AstNodeKind pk = ((AstNodeKind *)ast->kinds.data)[param_id.idx];
    if (pk != AST_DECL_PARAM)
      return IP_NONE;
    // Param extras: [name (0=anon), type, is_comptime].
    AstNodeData pd = ((AstNodeData *)ast->data.data)[param_id.idx];
    const uint32_t *pex = &((uint32_t *)ast->extra.data)[pd.extra_idx.idx];
    AstNodeId ptype = {.idx = pex[1]};
    IpIndex pti = resolve_type_expr(s, ast, ptype, mid);
    if (pti.v == IP_NONE.v)
      return IP_NONE;
    params[i] = pti;
  }

  IpIndex ret;
  if (ret_node.idx == AST_NODE_ID_NONE.idx) {
    ret = IP_VOID_TYPE; // implicit void on a missing return-type slot
  } else {
    ret = resolve_type_expr(s, ast, ret_node, mid);
    if (ret.v == IP_NONE.v)
      return IP_NONE;
  }

  IpKey key = {.kind = IPK_FN_TYPE,
               .fn_type = {.ret = ret,
                           .modifiers = 0,
                           .params = params,
                           .n_params = n_params}};
  return ip_get(&s->intern, key);
}

// Build a struct (or union, treated as struct in chunk 3) type from
// an AST_DECL_STRUCT / AST_DECL_UNION node. Uses the intern-pool wip
// API so the IpIndex is available before fields are resolved — that
// makes pointer-cycles through `^Self` possible in principle, but
// chunk 3 doesn't yet trampoline back to the wip on the recursive
// type_of_def call, so self-referential types still resolve to
// IP_NONE for now.
//
// zir_node_id = def.idx — nominal identity is keyed by the declaring
// def, so two structurally-identical struct decls at different sites
// produce distinct IpIndex values.
static IpIndex build_struct_type(struct db *s, ASTStore *ast,
                                 AstNodeId aggregate_node, DefId def,
                                 ModuleId mid) {
  AstNodeData ad = ((AstNodeData *)ast->data.data)[aggregate_node.idx];
  if (ad.extra_idx.idx == 0)
    return IP_NONE;
  const uint32_t *ex = &((uint32_t *)ast->extra.data)[ad.extra_idx.idx];
  uint32_t n_fields = ex[0];
  if (n_fields > MAX_AGG_CHILDREN)
    return IP_NONE;
  if (n_fields == 0) {
    IpKey key = {.kind = IPK_STRUCT_TYPE,
                 .struct_type = {.zir_node_id = def.idx,
                                 .field_names = NULL,
                                 .field_types = NULL,
                                 .n_fields = 0}};
    return ip_get(&s->intern, key);
  }

  StrId names[MAX_AGG_CHILDREN];
  IpIndex types[MAX_AGG_CHILDREN];
  WipContainerType wip = ip_wip_struct(&s->intern, def.idx, NULL, 0);

  for (uint32_t i = 0; i < n_fields; i++) {
    AstNodeId field_id = {.idx = ex[1 + i]};
    if (field_id.idx == AST_NODE_ID_NONE.idx) {
      ip_wip_struct_cancel(&s->intern, wip);
      return IP_NONE;
    }
    AstNodeKind fk = ((AstNodeKind *)ast->kinds.data)[field_id.idx];
    if (fk != AST_DECL_FIELD) {
      ip_wip_struct_cancel(&s->intern, wip);
      return IP_NONE;
    }
    // Field extras: [name (0=anon), type, vis, fpos (0=auto)].
    AstNodeData fd = ((AstNodeData *)ast->data.data)[field_id.idx];
    const uint32_t *fex = &((uint32_t *)ast->extra.data)[fd.extra_idx.idx];
    StrId fname = {.idx = fex[0]};
    AstNodeId ftype = {.idx = fex[1]};
    IpIndex ftypei = resolve_type_expr(s, ast, ftype, mid);
    if (ftypei.v == IP_NONE.v) {
      // Any unresolved field type fails the whole struct. Cleaner than
      // emitting a malformed nominal type; downstream queries treat
      // IP_NONE as "not yet known."
      ip_wip_struct_cancel(&s->intern, wip);
      return IP_NONE;
    }
    names[i] = fname;
    types[i] = ftypei;
  }

  ip_wip_struct_finish(&s->intern, wip, names, types, n_fields);
  return wip.index;
}

// Build an enum type from an AST_DECL_ENUM node. No wip API for enums
// — ip_get with IPK_ENUM_TYPE goes straight through since enums don't
// have the self-referential pattern that structs do.
//
// Chunk 3 only handles auto-numbered (0..N-1) or bare-literal-int
// variant values. Computed / expression values need chunk 6 const_eval.
static IpIndex build_enum_type(struct db *s, ASTStore *ast,
                               AstNodeId enum_node, DefId def) {
  AstNodeData ad = ((AstNodeData *)ast->data.data)[enum_node.idx];
  if (ad.extra_idx.idx == 0)
    return IP_NONE;
  const uint32_t *ex = &((uint32_t *)ast->extra.data)[ad.extra_idx.idx];
  uint32_t n_variants = ex[0];
  if (n_variants > MAX_AGG_CHILDREN)
    return IP_NONE;

  StrId names[MAX_AGG_CHILDREN];
  int64_t values[MAX_AGG_CHILDREN];

  for (uint32_t i = 0; i < n_variants; i++) {
    AstNodeId v_id = {.idx = ex[1 + i]};
    if (v_id.idx == AST_NODE_ID_NONE.idx)
      return IP_NONE;
    AstNodeKind vk = ((AstNodeKind *)ast->kinds.data)[v_id.idx];
    if (vk != AST_DECL_VARIANT)
      return IP_NONE;
    AstNodeData vd = ((AstNodeData *)ast->data.data)[v_id.idx];
    const uint32_t *vex = &((uint32_t *)ast->extra.data)[vd.extra_idx.idx];
    StrId vname = {.idx = vex[0]};
    AstNodeId v_val = {.idx = vex[1]};
    int64_t value;
    if (v_val.idx == 0) {
      value = (int64_t)i;
    } else {
      AstNodeKind v_val_k = ((AstNodeKind *)ast->kinds.data)[v_val.idx];
      if (v_val_k != AST_EXPR_LIT_INT)
        return IP_NONE;
      AstNodeData v_val_d = ((AstNodeData *)ast->data.data)[v_val.idx];
      const char *vs = pool_get(&s->strings, v_val_d.string_id);
      if (!vs)
        return IP_NONE;
      value = (int64_t)strtoll(vs, NULL, 0);
    }
    names[i] = vname;
    values[i] = value;
  }

  IpKey key = {.kind = IPK_ENUM_TYPE,
               .enum_type = {.zir_node_id = def.idx,
                             .variant_names = names,
                             .variant_values = values,
                             .n_variants = n_variants}};
  return ip_get(&s->intern, key);
}

// Map a literal AST node kind to its concrete intern-pool type. Untyped
// numeric literals get the comptime_int / comptime_float types (Zig
// convention); the concrete sized type is chosen later by coercion at
// the use site.
static IpIndex type_from_literal_kind(AstNodeKind k) {
  switch (k) {
  case AST_EXPR_LIT_INT:
    return IP_COMPTIME_INT_TYPE;
  case AST_EXPR_LIT_FLOAT:
    return IP_COMPTIME_FLOAT_TYPE;
  case AST_EXPR_LIT_BOOL:
    return IP_BOOL_TYPE;
  case AST_EXPR_LIT_BYTE:
    return IP_U8_TYPE;
  case AST_EXPR_LIT_STRING:
    return IP_STRING_SLICE_TYPE;
  case AST_EXPR_LIT_NIL:
    return IP_NIL_TYPE;
  default:
    return IP_NONE;
  }
}

// Fn signature query. Mirrors db_query_type_of_def's structure but keys
// its own slot in db.defs.slots_signature so call-site checking can
// depend on just the signature without dragging in type_of_def's full
// dispatch. The result is the same IpIndex that type_of_def returns for
// a fn def — they share db.defs.types[def.idx] as the storage column.
IpIndex db_query_fn_signature(struct db *s, DefId def) {
  if (def.idx == DEF_ID_NONE.idx)
    return IP_NONE;

  DB_QUERY_GUARD(s, QUERY_FN_SIGNATURE, &def,
                 *(IpIndex *)vec_get(&s->defs.types, def.idx), IP_NONE,
                 IP_NONE);

  AstId ast_id = *(AstId *)vec_get(&s->defs.ast_ids, def.idx);
  ModuleId mid = *(ModuleId *)vec_get(&s->defs.parent_modules, def.idx);

  (void)db_query_def_identity(s, mid, ast_id);

  IpIndex result = IP_NONE;
  uint32_t fc = 0;
  const FileId *files = db_module_files(s, mid, &fc);
  for (uint32_t i = 0; i < fc; i++) {
    (void)db_query_file_ast(s, files[i]);

    uint32_t local = file_id_local(files[i]);
    struct AstIdMap *map =
        *(struct AstIdMap **)vec_get(&s->files.ast_id_maps, local);
    if (!map)
      continue;
    AstNodeId node = ast_id_map_get(map, ast_id);
    if (node.idx == AST_NODE_ID_NONE.idx)
      continue;

    ASTStore *ast = *(ASTStore **)vec_get(&s->files.asts, local);
    AstNodeKind dk = ((AstNodeKind *)ast->kinds.data)[node.idx];
    if (dk != AST_DECL_CONST && dk != AST_DECL_VAR)
      break;

    AstNodeData d = ((AstNodeData *)ast->data.data)[node.idx];
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    AstNodeId value_id = {.idx = ex[2]};
    if (value_id.idx == AST_NODE_ID_NONE.idx)
      break;

    AstNodeKind vk = ((AstNodeKind *)ast->kinds.data)[value_id.idx];
    if (vk != AST_EXPR_LAMBDA)
      break; // signature is only defined for fn-bound decls

    // Lambda extras: [ret_id, body_id, effect_id, param_count, p0, ...].
    AstNodeData ld = ((AstNodeData *)ast->data.data)[value_id.idx];
    const uint32_t *lex = &((uint32_t *)ast->extra.data)[ld.extra_idx.idx];
    AstNodeId ret_node = {.idx = lex[0]};
    uint32_t param_count = lex[3];
    const uint32_t *param_ids = &lex[4];

    result = build_fn_type(s, ast, ret_node, param_ids, param_count, mid);
    break;
  }

  *(IpIndex *)vec_get(&s->defs.types, def.idx) = result;

  Fingerprint fp = db_fp_u64((uint64_t)def.idx);
  fp = db_fp_combine(fp, db_fp_u64((uint64_t)result.v));
  db_query_succeed(s, QUERY_FN_SIGNATURE, &def, fp);
  return result;
}

IpIndex db_query_type_of_def(struct db *s, DefId def) {
  if (def.idx == DEF_ID_NONE.idx)
    return IP_NONE;

  DB_QUERY_GUARD(s, QUERY_TYPE_OF_DECL, &def,
                 *(IpIndex *)vec_get(&s->defs.types, def.idx), IP_NONE,
                 IP_NONE);

  AstId ast_id = *(AstId *)vec_get(&s->defs.ast_ids, def.idx);
  ModuleId mid = *(ModuleId *)vec_get(&s->defs.parent_modules, def.idx);

  // Dep on identity — rename / remove / meta-change invalidates the type.
  (void)db_query_def_identity(s, mid, ast_id);

  // Walk this module's files; first ast_id_map hit wins. db_query_file_ast
  // is called per file so the AST dep is recorded — any AST edit
  // invalidates this query (coarse, but correct; finer-grained per-decl
  // AST slots are a later optimization).
  IpIndex result = IP_NONE;
  uint32_t fc = 0;
  const FileId *files = db_module_files(s, mid, &fc);
  for (uint32_t i = 0; i < fc; i++) {
    (void)db_query_file_ast(s, files[i]);

    uint32_t local = file_id_local(files[i]);
    struct AstIdMap *map =
        *(struct AstIdMap **)vec_get(&s->files.ast_id_maps, local);
    if (!map)
      continue;
    AstNodeId node = ast_id_map_get(map, ast_id);
    if (node.idx == AST_NODE_ID_NONE.idx)
      continue;

    ASTStore *ast = *(ASTStore **)vec_get(&s->files.asts, local);
    AstNodeKind dk = ((AstNodeKind *)ast->kinds.data)[node.idx];
    if (dk != AST_DECL_CONST && dk != AST_DECL_VAR)
      break; // fn/struct/enum/distinct/union — deferred to later chunks

    // Extras layout for CONST/VAR: [name_strid, type_id, value_id, meta].
    AstNodeData d = ((AstNodeData *)ast->data.data)[node.idx];
    uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    DefMeta meta = (DefMeta)ex[3];

    // Distinct binds need a separate nominal-identity story (no
    // IPK_DISTINCT today) — deferred to chunk 8 alongside bit-packing.
    if (meta & META_DISTINCT)
      break;

    AstNodeId value_id = {.idx = ex[2]};

    // If the RHS is itself an aggregate decl, the type IS that
    // aggregate — drive the nominal type from the value side, ignoring
    // any `: type` annotation. (Union is structurally a struct in
    // chunk 3; chunk 8 may split it out with its own IPK.) Fn decls
    // delegate to db_query_fn_signature so call-site checking can
    // depend on just the signature.
    if (value_id.idx != AST_NODE_ID_NONE.idx) {
      AstNodeKind vk = ((AstNodeKind *)ast->kinds.data)[value_id.idx];
      if (vk == AST_DECL_STRUCT || vk == AST_DECL_UNION) {
        result = build_struct_type(s, ast, value_id, def, mid);
        break;
      }
      if (vk == AST_DECL_ENUM) {
        result = build_enum_type(s, ast, value_id, def);
        break;
      }
      if (vk == AST_EXPR_LAMBDA) {
        result = db_query_fn_signature(s, def);
        break;
      }
    }

    if (ex[1] != 0) {
      // Typed bind: the annotation wins over the RHS — resolve_type_expr
      // handles primitives + type constructors + user identifiers via
      // resolve_ref. Coercion-checking the RHS against the annotation
      // is chunk 5.
      AstNodeId type_id = {.idx = ex[1]};
      result = resolve_type_expr(s, ast, type_id, mid);
      break;
    }
    if (value_id.idx == AST_NODE_ID_NONE.idx)
      break;

    AstNodeKind vk = ((AstNodeKind *)ast->kinds.data)[value_id.idx];
    result = type_from_literal_kind(vk);
    break;
  }

  // Write result before db_query_succeed so the on-cached branch above
  // reads the canonical value on subsequent calls.
  *(IpIndex *)vec_get(&s->defs.types, def.idx) = result;

  Fingerprint fp = db_fp_u64((uint64_t)def.idx);
  fp = db_fp_combine(fp, db_fp_u64((uint64_t)result.v));
  db_query_succeed(s, QUERY_TYPE_OF_DECL, &def, fp);
  return result;
}
