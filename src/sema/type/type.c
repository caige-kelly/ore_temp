#include "type.h"

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../../common/stringpool.h"
#include "../sema.h"
#include "intern.h"

// Map a primitive TypeKind to its reserved IpIndex slot. Returns
// IP_NONE for kinds that don't have a reserved index (TY_ERROR has
// one, but TY_STRUCT/TY_ENUM/etc. are compound and don't apply).
static IpIndex primitive_ip_for_kind(TypeKind kind) {
  switch (kind) {
  case TY_BOOL:           return IP_BOOL_TYPE;
  case TY_U8:             return IP_U8_TYPE;
  case TY_I8:             return IP_I8_TYPE;
  case TY_U16:            return IP_U16_TYPE;
  case TY_I16:            return IP_I16_TYPE;
  case TY_U32:            return IP_U32_TYPE;
  case TY_I32:            return IP_I32_TYPE;
  case TY_U64:            return IP_U64_TYPE;
  case TY_I64:            return IP_I64_TYPE;
  case TY_F32:            return IP_F32_TYPE;
  case TY_F64:            return IP_F64_TYPE;
  case TY_USIZE:          return IP_USIZE_TYPE;
  case TY_ISIZE:          return IP_ISIZE_TYPE;
  case TY_VOID:           return IP_VOID_TYPE;
  case TY_NORETURN:       return IP_NORETURN_TYPE;
  case TY_TYPE:           return IP_TYPE_TYPE;
  case TY_COMPTIME_INT:   return IP_COMPTIME_INT_TYPE;
  case TY_COMPTIME_FLOAT: return IP_COMPTIME_FLOAT_TYPE;
  case TY_NIL:            return IP_NIL_TYPE;
  case TY_ERROR:          return IP_ERROR_TYPE;
  default:                return IP_NONE;
  }
}

// Register a Type* in the IpIndex → Type* bridge table. Grows the
// `types_by_ip` vec as needed and sets the slot. Idempotent: calling
// with the same idx writes the same pointer.
static void register_type_for_ip(struct Sema *s, IpIndex idx, struct Type *t) {
  if (!ip_index_is_valid(idx) || !s->types_by_ip) return;
  while (s->types_by_ip->count <= idx.v) {
    struct Type *null_p = NULL;
    vec_push(s->types_by_ip, &null_p);
  }
  struct Type **slot = (struct Type **)vec_get(s->types_by_ip, idx.v);
  *slot = t;
}

// Allocate one Type for `kind` into the arena, stash it via `*field`
// on Sema, and register `name` in the primitive_types map keyed by
// its pool id. `name`/`name_len` are NULL/0 for kinds without a
// surface spelling (TY_ERROR, the comptime_* placeholders).
//
// R4 Step 3a: also hooks the primitive Type* to its reserved IpIndex
// and registers it in the bridge table. After this, callers can
// round-trip via type->ip and type_of_ip(s, idx).
static struct Type *make_primitive(struct Sema *s, TypeKind kind,
                                   const char *name, size_t name_len) {
  struct Type *t = arena_alloc(&s->arena, sizeof(struct Type));
  t->kind = kind;
  t->ip = primitive_ip_for_kind(kind);
  register_type_for_ip(s, t->ip, t);
  if (name && name_len > 0) {
    StrId name_id = pool_intern(&s->pool, name, name_len);
    hashmap_put_or_die(&s->primitive_types, (uint64_t)name_id.v, t,
                       "primitive_types");
  }
  return t;
}

// === IpIndex ↔ Type* bridge ===
//
// Used during the R4 migration when callers need to convert between
// the two identity representations. Once all type kinds are migrated,
// type_of_ip becomes the canonical lookup and ip_of_type collapses
// to a field read.

struct Type *type_of_ip(struct Sema *s, IpIndex idx) {
  if (!s || !s->types_by_ip || !ip_index_is_valid(idx)) return NULL;
  if (idx.v >= s->types_by_ip->count) return NULL;
  struct Type **slot = (struct Type **)vec_get(s->types_by_ip, idx.v);
  return slot ? *slot : NULL;
}

IpIndex ip_of_type(struct Sema *s, struct Type *t) {
  (void)s;
  return t ? t->ip : IP_NONE;
}

#define PRIM(kind, lit) make_primitive(s, (kind), (lit), sizeof(lit) - 1)
#define PRIM_NONAME(kind) make_primitive(s, (kind), NULL, 0)

void sema_types_init(struct Sema *s) {
  if (!s)
    return;
  if (s->primitive_types.entries == NULL)
    hashmap_init_in(&s->primitive_types, &s->arena);

  s->error_type = PRIM_NONAME(TY_ERROR);
  s->void_type = PRIM(TY_VOID, "void");
  s->noreturn_type = PRIM(TY_NORETURN, "noreturn");
  s->bool_type = PRIM(TY_BOOL, "bool");

  s->u8_type = PRIM(TY_U8, "u8");
  s->u16_type = PRIM(TY_U16, "u16");
  s->u32_type = PRIM(TY_U32, "u32");
  s->u64_type = PRIM(TY_U64, "u64");
  s->usize_type = PRIM(TY_USIZE, "usize");

  s->i8_type = PRIM(TY_I8, "i8");
  s->i16_type = PRIM(TY_I16, "i16");
  s->i32_type = PRIM(TY_I32, "i32");
  s->i64_type = PRIM(TY_I64, "i64");
  s->isize_type = PRIM(TY_ISIZE, "isize");

  s->f32_type = PRIM(TY_F32, "f32");
  s->f64_type = PRIM(TY_F64, "f64");

  s->comptime_int_type = PRIM(TY_COMPTIME_INT, "comptime_int");
  s->comptime_float_type = PRIM(TY_COMPTIME_FLOAT, "comptime_float");

  s->nil_type = PRIM(TY_NIL, "nil");
  s->type_type = PRIM(TY_TYPE, "type");

  // Bring up the compound-type interners (fn / ptr / slice / array).
  // No initial members — they get populated lazily as type_fn / etc.
  // are called.
  sema_type_interns_init(s);

  // `string` reified as `[]const u8`. Pre-PR-3 this was an opaque
  // TY_STRING primitive that didn't interop with slice operations
  // (.len, indexing, slicing). Now it's a real const-slice — the
  // u8 elem type is already initialized above; sema_type_interns_init
  // ran one line earlier so the slice interner is ready.
  //
  // The "string" name still resolves via primitive_types (registered
  // here so type_for_primitive_name returns the canonical slice
  // pointer instead of a separate TY_STRING type).
  s->string_type = type_slice(s, s->u8_type, /*is_const=*/true);
  StrId string_name = pool_intern(&s->pool, "string", 6);
  hashmap_put_or_die(&s->primitive_types, (uint64_t)string_name.v, s->string_type,
                     "primitive_types");
}

#undef PRIM
#undef PRIM_NONAME

struct Type *type_for_primitive_name(struct Sema *s, StrId name_id) {
  // Note: name_id == 0 is a legitimate pool id — pool_intern returns
  // the byte offset into its data buffer, and the first string
  // interned (often "void" since it's registered first) lands at
  // offset 0. Don't filter on name_id == 0; the hashmap miss path
  // already returns NULL for genuine misses.
  if (!s || s->primitive_types.entries == NULL)
    return NULL;
  if (!hashmap_contains(&s->primitive_types, (uint64_t)name_id.v))
    return NULL;
  return (struct Type *)hashmap_get(&s->primitive_types, (uint64_t)name_id.v);
}

// Short kind name only — full structure rendering lives in
// type/display.{h,c} via `type_to_string`.
const char *type_name(const struct Type *t) {
  if (!t)
    return "<null>";
  switch (t->kind) {
  case TY_ERROR:
    return "<error>";
  case TY_VOID:
    return "void";
  case TY_NORETURN:
    return "noreturn";
  case TY_BOOL:
    return "bool";
  case TY_U8:
    return "u8";
  case TY_U16:
    return "u16";
  case TY_U32:
    return "u32";
  case TY_U64:
    return "u64";
  case TY_USIZE:
    return "usize";
  case TY_I8:
    return "i8";
  case TY_I16:
    return "i16";
  case TY_I32:
    return "i32";
  case TY_I64:
    return "i64";
  case TY_ISIZE:
    return "isize";
  case TY_F32:
    return "f32";
  case TY_F64:
    return "f64";
  case TY_COMPTIME_INT:
    return "comptime_int";
  case TY_COMPTIME_FLOAT:
    return "comptime_float";
  case TY_NIL:
    return "nil";
  case TY_TYPE:
    return "type";
  case TY_FN:
    return "fn";
  case TY_PTR:
    return "ptr";
  case TY_MANY_PTR:
    return "many_ptr";
  case TY_SLICE:
    return "slice";
  case TY_ARRAY:
    return "array";
  case TY_OPTIONAL:
    return "optional";
  case TY_STRUCT:
    return "struct";
  case TY_ENUM:
    return "enum";
  }
  return "<unknown>";
}

bool type_is_int(const struct Type *t) {
  if (!t)
    return false;
  switch (t->kind) {
  case TY_U8:
  case TY_U16:
  case TY_U32:
  case TY_U64:
  case TY_USIZE:
  case TY_I8:
  case TY_I16:
  case TY_I32:
  case TY_I64:
  case TY_ISIZE:
  case TY_COMPTIME_INT:
    return true;
  default:
    return false;
  }
}

bool type_is_float(const struct Type *t) {
  if (!t)
    return false;
  return t->kind == TY_F32 || t->kind == TY_F64 || t->kind == TY_COMPTIME_FLOAT;
}

bool type_is_unsigned(const struct Type *t) {
  if (!t)
    return false;
  switch (t->kind) {
  case TY_U8:
  case TY_U16:
  case TY_U32:
  case TY_U64:
  case TY_USIZE:
    return true;
  default:
    return false;
  }
}

bool type_is_numeric(const struct Type *t) {
  return type_is_int(t) || type_is_float(t);
}

bool type_is_comptime(const struct Type *t) {
  if (!t)
    return false;
  return t->kind == TY_COMPTIME_INT || t->kind == TY_COMPTIME_FLOAT;
}
