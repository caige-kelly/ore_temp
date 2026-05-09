#include "decl_data.h"

#include <string.h>

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../../common/vec.h"
#include "../../diag/diag.h"
#include "../../parser/ast.h"
#include "../eval/const_eval.h"
#include "../modules/modules.h"  // query_module_ast
#include "../query/ast_dep.h"    // record_ast_dep_for_def
#include "../query/query_engine.h"
#include "../scope/scope.h"
#include "../sema.h"
#include "checker.h"     // resolve_type_expr
#include "type.h"

// =====================================================================
// FnSignature
// =====================================================================

static struct FnSignature *
fn_signature_entry_for(struct Sema *s, DefId fn_def) {
  if (s->fn_signatures.entries == NULL)
    hashmap_init_in(&s->fn_signatures, &s->arena);

  uint64_t key = (uint64_t)fn_def.idx;
  if (hashmap_contains(&s->fn_signatures, key))
    return (struct FnSignature *)hashmap_get(&s->fn_signatures, key);

  struct FnSignature *sig = arena_alloc(&s->arena, sizeof(*sig));
  *sig = (struct FnSignature){0};
  sema_query_slot_init(&sig->query, QUERY_FN_SIGNATURE);
  hashmap_put_or_die(&s->fn_signatures, key, sig, "fn_signatures");
  return sig;
}

// Pull the Lambda Expr out of a fn's owning Bind, or NULL if the
// def isn't fn-shaped. Diagnostics for the wrong-shape case are the
// caller's concern.
static struct Expr *fn_lambda_for_def(struct Sema *s, DefId fn_def) {
  struct DefInfo *di = def_info(s, fn_def);
  if (!di || di->kind != DECL_USER) return NULL;
  // Read via def_origin: after AST re-parse, di->origin still points
  // at the prior arena allocation. node_to_expr is re-keyed by NodeId
  // each parse, so the lookup yields the freshly-parsed Bind node.
  struct Expr *origin = def_origin(s, fn_def);
  if (!origin || origin->kind != expr_Bind) return NULL;
  struct Expr *value = origin->bind.value;
  if (!value || value->kind != expr_Lambda) return NULL;
  return value;
}

struct FnSignature *query_fn_signature(struct Sema *s, DefId fn_def) {
  if (!s || !def_id_is_valid(fn_def)) return NULL;
  struct Expr *lambda = fn_lambda_for_def(s, fn_def);
  if (!lambda) return NULL;

  struct FnSignature *sig = fn_signature_entry_for(s, fn_def);

  struct Span frame_span = lambda->span;
  SEMA_QUERY_GUARD(s, &sig->query, QUERY_FN_SIGNATURE, sig, frame_span,
                   /*on_cached=*/sig,
                   /*on_cycle=*/NULL,
                   /*on_error=*/NULL);

  record_ast_dep_for_def(s, fn_def);

  // Resolve param types. Allocate parallel arrays in the arena.
  size_t n = lambda->lambda.params ? lambda->lambda.params->count : 0;
  struct Type **param_types = NULL;
  ParamKind *param_kinds = NULL;
  if (n > 0) {
    param_types = arena_alloc(&s->arena, sizeof(struct Type *) * n);
    param_kinds = arena_alloc(&s->arena, sizeof(ParamKind) * n);
    for (size_t i = 0; i < n; i++) {
      struct Param *p = (struct Param *)vec_get(lambda->lambda.params, i);
      if (!p) {
        param_types[i] = s->error_type;
        param_kinds[i] = PARAM_RUNTIME;
        continue;
      }
      if (!p->type_ann) {
        diag_error(&s->diags, lambda->span,
                   "function parameter #%zu requires a type annotation", i);
        param_types[i] = s->error_type;
      } else {
        param_types[i] = resolve_type_expr(s, p->type_ann);
      }
      param_kinds[i] = p->kind;
    }
  }

  struct Type *ret_type = lambda->lambda.ret_type
                              ? resolve_type_expr(s, lambda->lambda.ret_type)
                              : s->void_type;

  sig->param_types = param_types;
  sig->param_kinds = param_kinds;
  sig->param_count = n;
  sig->ret_type = ret_type;
  sig->is_comptime = lambda->is_comptime;
  sig->has_effects = lambda->lambda.effect != NULL;

  // Fingerprint over the structurally meaningful contents. Since
  // Type*s are interned, hashing pointer addresses gives type-shape
  // equality. param_kinds and modifier bits get folded in too.
  Fingerprint fp = query_fingerprint_from_u64(n);
  for (size_t i = 0; i < n; i++) {
    fp = query_fingerprint_combine(
        fp, query_fingerprint_from_pointer(param_types[i]));
    fp = query_fingerprint_combine(fp,
                                   query_fingerprint_from_u64(param_kinds[i]));
  }
  fp = query_fingerprint_combine(fp, query_fingerprint_from_pointer(ret_type));
  uint64_t modifiers = ((uint64_t)sig->is_comptime << 1) |
                       (uint64_t)sig->has_effects;
  fp = query_fingerprint_combine(fp, query_fingerprint_from_u64(modifiers));
  query_slot_set_fingerprint(&sig->query, fp);

  sema_query_succeed(s, &sig->query);
  return sig;
}

// =====================================================================
// ParamLocator
// =====================================================================

void param_locator_set(struct Sema *s, DefId param_def, DefId parent_fn,
                       uint32_t index) {
  if (!s || !def_id_is_valid(param_def)) return;
  if (s->param_locators.entries == NULL)
    hashmap_init_in(&s->param_locators, &s->arena);

  uint64_t key = (uint64_t)param_def.idx;
  struct ParamLocator *loc;
  if (hashmap_contains(&s->param_locators, key)) {
    loc = (struct ParamLocator *)hashmap_get(&s->param_locators, key);
  } else {
    loc = arena_alloc(&s->arena, sizeof(struct ParamLocator));
    hashmap_put_or_die(&s->param_locators, key, loc, "param_locators");
  }
  loc->parent_fn = parent_fn;
  loc->index = index;
}

struct ParamLocator *param_locator_get(struct Sema *s, DefId param_def) {
  if (!s || !def_id_is_valid(param_def)) return NULL;
  if (s->param_locators.entries == NULL) return NULL;
  uint64_t key = (uint64_t)param_def.idx;
  if (!hashmap_contains(&s->param_locators, key)) return NULL;
  return (struct ParamLocator *)hashmap_get(&s->param_locators, key);
}

// =====================================================================
// StructSignature
// =====================================================================

static struct StructSignature *
struct_signature_entry_for(struct Sema *s, DefId struct_def) {
  if (s->struct_signatures.entries == NULL)
    hashmap_init_in(&s->struct_signatures, &s->arena);

  uint64_t key = (uint64_t)struct_def.idx;
  if (hashmap_contains(&s->struct_signatures, key))
    return (struct StructSignature *)hashmap_get(&s->struct_signatures, key);

  struct StructSignature *sig = arena_alloc(&s->arena, sizeof(*sig));
  *sig = (struct StructSignature){0};
  sema_query_slot_init(&sig->query, QUERY_STRUCT_SIGNATURE);
  hashmap_put_or_die(&s->struct_signatures, key, sig, "struct_signatures");
  return sig;
}

// Pull the struct Expr out of a struct's owning Bind, or NULL if the
// def isn't struct-shaped.
static struct Expr *struct_expr_for_def(struct Sema *s, DefId struct_def) {
  struct DefInfo *di = def_info(s, struct_def);
  if (!di || di->kind != DECL_USER) return NULL;
  struct Expr *origin = def_origin(s, struct_def);
  if (!origin || origin->kind != expr_Bind) return NULL;
  struct Expr *value = origin->bind.value;
  if (!value || value->kind != expr_Struct) return NULL;
  return value;
}

// Count total flattened fields: each member_Field contributes 1; each
// member_Union contributes one per arm. Pre-pass so we allocate the
// arena array exactly once.
static size_t count_flat_fields(Vec *members) {
  if (!members) return 0;
  size_t total = 0;
  for (size_t i = 0; i < members->count; i++) {
    struct StructMember *m = (struct StructMember *)vec_get(members, i);
    if (!m) continue;
    if (m->kind == member_Field) total += 1;
    else if (m->kind == member_Union)
      total += m->union_def.variants ? m->union_def.variants->count : 0;
  }
  return total;
}

// Append one FieldData entry plus a backing DECL_FIELD def, register
// the def in the struct's child scope, and stamp a FieldLocator. The
// lookup path (expr_Field on TY_STRUCT) reads FieldData directly; the
// DECL_FIELD def exists for path-style references and future LSP
// "find references" indexing. Duplicate-name collisions (across normal
// fields and across union arms) emit a diagnostic and skip the
// scope insertion — the FieldData entry is still appended so type
// recovery downstream is consistent.
static void emit_field(struct Sema *s, struct StructSignature *sig,
                       size_t *out, ScopeId child_scope, DefId parent_struct,
                       struct FieldDef *fd, uint32_t union_group) {
  if (!fd) return;
  struct Type *type = resolve_type_expr(s, fd->type);

  struct DefInfo proto = {
      .kind            = DECL_FIELD,
      .semantic_kind   = SEM_VALUE,
      .name_id         = fd->name.string_id,
      .span            = fd->name.span,
      .origin_id       = (struct NodeId){0},
      .origin          = NULL,
      .owner_scope     = child_scope,
      .child_scope     = SCOPE_ID_INVALID,
      .imported_module = MODULE_ID_INVALID,
      .vis             = fd->visibility,
      .scope_token_id  = 0,
  };
  DefId field_def = def_create(s, proto);
  if (!scope_define_def(s, child_scope, field_def)) {
    diag_error(&s->diags, fd->name.span,
               "duplicate field name in struct");
  }
  field_locator_set(s, field_def, parent_struct, (uint32_t)*out);

  sig->fields[*out] = (struct FieldData){
      .name_id     = fd->name.string_id,
      .span        = fd->name.span,
      .vis         = fd->visibility,
      .type        = type,
      .union_group = union_group,
  };
  (*out)++;
}

struct StructSignature *query_struct_signature(struct Sema *s, DefId struct_def) {
  if (!s || !def_id_is_valid(struct_def)) return NULL;
  struct Expr *struct_expr = struct_expr_for_def(s, struct_def);
  if (!struct_expr) return NULL;

  struct StructSignature *sig = struct_signature_entry_for(s, struct_def);

  struct Span frame_span = struct_expr->span;
  SEMA_QUERY_GUARD(s, &sig->query, QUERY_STRUCT_SIGNATURE, sig, frame_span,
                   /*on_cached=*/sig,
                   /*on_cycle=*/NULL,
                   /*on_error=*/NULL);

  record_ast_dep_for_def(s, struct_def);

  // Lazy child-scope creation. The query slot's RUNNING/DONE protocol
  // makes this idempotent: a cached re-entry returns above without
  // touching the def, and the first call sets child_scope exactly once.
  struct DefInfo *parent_di = def_info(s, struct_def);
  if (parent_di && !scope_id_is_valid(parent_di->child_scope)) {
    parent_di->child_scope =
        scope_create(s, SCOPE_STRUCT, parent_di->owner_scope,
                     /*owner_module=*/MODULE_ID_INVALID);
  }
  ScopeId child_scope = parent_di ? parent_di->child_scope : SCOPE_ID_INVALID;

  Vec *members = struct_expr->struct_expr.members;
  size_t total = count_flat_fields(members);

  sig->fields = total > 0
                    ? arena_alloc(&s->arena, sizeof(struct FieldData) * total)
                    : NULL;
  size_t out = 0;
  uint32_t next_union_group = 1;
  if (members) {
    for (size_t i = 0; i < members->count; i++) {
      struct StructMember *m = (struct StructMember *)vec_get(members, i);
      if (!m) continue;
      if (m->kind == member_Field) {
        emit_field(s, sig, &out, child_scope, struct_def, &m->field,
                   /*union_group=*/0);
      } else if (m->kind == member_Union) {
        uint32_t group = next_union_group++;
        Vec *arms = m->union_def.variants;
        if (!arms) continue;
        for (size_t j = 0; j < arms->count; j++) {
          struct FieldDef *arm = (struct FieldDef *)vec_get(arms, j);
          emit_field(s, sig, &out, child_scope, struct_def, arm, group);
        }
      }
    }
  }

  sig->field_count = out;

  // Fingerprint over (name_id, type ptr, union_group) for each field.
  Fingerprint fp = query_fingerprint_from_u64(out);
  for (size_t i = 0; i < out; i++) {
    fp = query_fingerprint_combine(fp,
        query_fingerprint_from_u64(sig->fields[i].name_id));
    fp = query_fingerprint_combine(fp,
        query_fingerprint_from_pointer(sig->fields[i].type));
    fp = query_fingerprint_combine(fp,
        query_fingerprint_from_u64(sig->fields[i].union_group));
  }
  query_slot_set_fingerprint(&sig->query, fp);

  sema_query_succeed(s, &sig->query);
  return sig;
}

void field_locator_set(struct Sema *s, DefId field_def, DefId parent_struct,
                       uint32_t index) {
  if (!s || !def_id_is_valid(field_def)) return;
  if (s->field_locators.entries == NULL)
    hashmap_init_in(&s->field_locators, &s->arena);

  uint64_t key = (uint64_t)field_def.idx;
  struct FieldLocator *loc;
  if (hashmap_contains(&s->field_locators, key)) {
    loc = (struct FieldLocator *)hashmap_get(&s->field_locators, key);
  } else {
    loc = arena_alloc(&s->arena, sizeof(struct FieldLocator));
    hashmap_put_or_die(&s->field_locators, key, loc, "field_locators");
  }
  loc->parent_struct = parent_struct;
  loc->index = index;
}

struct FieldLocator *field_locator_get(struct Sema *s, DefId field_def) {
  if (!s || !def_id_is_valid(field_def)) return NULL;
  if (s->field_locators.entries == NULL) return NULL;
  uint64_t key = (uint64_t)field_def.idx;
  if (!hashmap_contains(&s->field_locators, key)) return NULL;
  return (struct FieldLocator *)hashmap_get(&s->field_locators, key);
}

// =====================================================================
// EnumSignature
// =====================================================================

static struct EnumSignature *
enum_signature_entry_for(struct Sema *s, DefId enum_def) {
  if (s->enum_signatures.entries == NULL)
    hashmap_init_in(&s->enum_signatures, &s->arena);

  uint64_t key = (uint64_t)enum_def.idx;
  if (hashmap_contains(&s->enum_signatures, key))
    return (struct EnumSignature *)hashmap_get(&s->enum_signatures, key);

  struct EnumSignature *sig = arena_alloc(&s->arena, sizeof(*sig));
  *sig = (struct EnumSignature){0};
  sema_query_slot_init(&sig->query, QUERY_ENUM_SIGNATURE);
  hashmap_put_or_die(&s->enum_signatures, key, sig, "enum_signatures");
  return sig;
}

static struct Expr *enum_expr_for_def(struct Sema *s, DefId enum_def) {
  struct DefInfo *di = def_info(s, enum_def);
  if (!di || di->kind != DECL_USER) return NULL;
  struct Expr *origin = def_origin(s, enum_def);
  if (!origin || origin->kind != expr_Bind) return NULL;
  struct Expr *value = origin->bind.value;
  if (!value || value->kind != expr_Enum) return NULL;
  return value;
}

struct EnumSignature *query_enum_signature(struct Sema *s, DefId enum_def) {
  if (!s || !def_id_is_valid(enum_def)) return NULL;
  struct Expr *enum_expr = enum_expr_for_def(s, enum_def);
  if (!enum_expr) return NULL;

  struct EnumSignature *sig = enum_signature_entry_for(s, enum_def);

  struct Span frame_span = enum_expr->span;
  SEMA_QUERY_GUARD(s, &sig->query, QUERY_ENUM_SIGNATURE, sig, frame_span,
                   /*on_cached=*/sig,
                   /*on_cycle=*/NULL,
                   /*on_error=*/NULL);

  record_ast_dep_for_def(s, enum_def);

  // Lazy child-scope creation. Same pattern as query_struct_signature.
  struct DefInfo *parent_di = def_info(s, enum_def);
  if (parent_di && !scope_id_is_valid(parent_di->child_scope)) {
    parent_di->child_scope =
        scope_create(s, SCOPE_ENUM, parent_di->owner_scope,
                     /*owner_module=*/MODULE_ID_INVALID);
  }
  ScopeId child_scope = parent_di ? parent_di->child_scope : SCOPE_ID_INVALID;

  Vec *raw = enum_expr->enum_expr.variants;
  size_t n = raw ? raw->count : 0;
  struct VariantData *out = NULL;
  if (n > 0)
    out = arena_alloc(&s->arena, sizeof(struct VariantData) * n);

  // C-style auto-increment. Each variant either supplies an explicit
  // constant value (const-evaluated) or takes prev_value + 1. Initial
  // implicit value is 0. A non-int explicit value is an error and
  // the variant falls back to the auto-incremented value so later
  // variants continue sensibly.
  int64_t next_value = 0;
  for (size_t i = 0; i < n; i++) {
    struct EnumVariant *v = (struct EnumVariant *)vec_get(raw, i);
    if (!v) continue;
    int64_t value = next_value;
    if (v->explicit_value) {
      struct ConstValue cv = query_const_eval(s, v->explicit_value);
      if (cv.kind == CONST_INT) {
        value = cv.int_val;
      } else {
        diag_error(&s->diags, v->explicit_value->span,
                   "enum variant value must be a comptime integer");
      }
    }
    out[i] = (struct VariantData){
        .name_id        = v->name.string_id,
        .span           = v->span,
        .explicit_value = v->explicit_value,
        .value          = value,
    };

    // Allocate the DECL_VARIANT def + register in the enum's child
    // scope so qualified `Color.Red` paths can resolve. Duplicate
    // names emit a diagnostic and skip insertion.
    if (scope_id_is_valid(child_scope)) {
      struct DefInfo proto = {
          .kind            = DECL_VARIANT,
          .semantic_kind   = SEM_VALUE,
          .name_id         = v->name.string_id,
          .span            = v->span,
          .origin_id       = (struct NodeId){0},
          .origin          = NULL,
          .owner_scope     = child_scope,
          .child_scope     = SCOPE_ID_INVALID,
          .imported_module = MODULE_ID_INVALID,
          .vis             = Visibility_public,
          .scope_token_id  = 0,
      };
      DefId variant_def = def_create(s, proto);
      if (!scope_define_def(s, child_scope, variant_def)) {
        diag_error(&s->diags, v->span, "duplicate variant name in enum");
      }
      variant_locator_set(s, variant_def, enum_def, (uint32_t)i);
    }
    next_value = value + 1;
  }

  sig->variants = out;
  sig->variant_count = n;

  Fingerprint fp = query_fingerprint_from_u64(n);
  for (size_t i = 0; i < n; i++) {
    fp = query_fingerprint_combine(fp,
        query_fingerprint_from_u64(out[i].name_id));
    fp = query_fingerprint_combine(fp,
        query_fingerprint_from_u64((uint64_t)out[i].value));
  }
  query_slot_set_fingerprint(&sig->query, fp);

  sema_query_succeed(s, &sig->query);
  return sig;
}

void variant_locator_set(struct Sema *s, DefId variant_def, DefId parent_enum,
                         uint32_t index) {
  if (!s || !def_id_is_valid(variant_def)) return;
  if (s->variant_locators.entries == NULL)
    hashmap_init_in(&s->variant_locators, &s->arena);

  uint64_t key = (uint64_t)variant_def.idx;
  struct VariantLocator *loc;
  if (hashmap_contains(&s->variant_locators, key)) {
    loc = (struct VariantLocator *)hashmap_get(&s->variant_locators, key);
  } else {
    loc = arena_alloc(&s->arena, sizeof(struct VariantLocator));
    hashmap_put_or_die(&s->variant_locators, key, loc, "variant_locators");
  }
  loc->parent_enum = parent_enum;
  loc->index = index;
}

struct VariantLocator *variant_locator_get(struct Sema *s, DefId variant_def) {
  if (!s || !def_id_is_valid(variant_def)) return NULL;
  if (s->variant_locators.entries == NULL) return NULL;
  uint64_t key = (uint64_t)variant_def.idx;
  if (!hashmap_contains(&s->variant_locators, key)) return NULL;
  return (struct VariantLocator *)hashmap_get(&s->variant_locators, key);
}
