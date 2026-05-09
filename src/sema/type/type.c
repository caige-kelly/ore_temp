#include "type.h"

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../../common/stringpool.h"
#include "../sema.h"

// Allocate one Type for `kind` into the arena, stash it via `*field`
// on Sema, and register `name` in the primitive_types map keyed by
// its pool id. `name`/`name_len` are NULL/0 for kinds without a
// surface spelling (TY_ERROR, the comptime_* placeholders).
static struct Type *make_primitive(struct Sema *s, TypeKind kind,
                                   const char *name, size_t name_len) {
  struct Type *t = arena_alloc(&s->arena, sizeof(struct Type));
  t->kind = kind;
  if (name && name_len > 0) {
    uint32_t name_id = pool_intern(&s->pool, name, name_len);
    hashmap_put(&s->primitive_types, (uint64_t)name_id, t);
  }
  return t;
}

#define PRIM(kind, lit) make_primitive(s, (kind), (lit), sizeof(lit) - 1)
#define PRIM_NONAME(kind) make_primitive(s, (kind), NULL, 0)

void sema_types_init(struct Sema *s) {
  if (!s) return;
  if (s->primitive_types.entries == NULL)
    hashmap_init_in(&s->primitive_types, &s->arena);

  s->error_type = PRIM_NONAME(TY_ERROR);
  s->void_type  = PRIM(TY_VOID, "void");
  s->bool_type  = PRIM(TY_BOOL, "bool");

  s->u8_type    = PRIM(TY_U8,    "u8");
  s->u16_type   = PRIM(TY_U16,   "u16");
  s->u32_type   = PRIM(TY_U32,   "u32");
  s->u64_type   = PRIM(TY_U64,   "u64");
  s->usize_type = PRIM(TY_USIZE, "usize");

  s->i8_type    = PRIM(TY_I8,    "i8");
  s->i16_type   = PRIM(TY_I16,   "i16");
  s->i32_type   = PRIM(TY_I32,   "i32");
  s->i64_type   = PRIM(TY_I64,   "i64");
  s->isize_type = PRIM(TY_ISIZE, "isize");

  s->f32_type = PRIM(TY_F32, "f32");
  s->f64_type = PRIM(TY_F64, "f64");

  s->comptime_int_type   = PRIM(TY_COMPTIME_INT,   "comptime_int");
  s->comptime_float_type = PRIM(TY_COMPTIME_FLOAT, "comptime_float");
}

#undef PRIM
#undef PRIM_NONAME

struct Type *type_for_primitive_name(struct Sema *s, uint32_t name_id) {
  if (!s || name_id == 0) return NULL;
  if (s->primitive_types.entries == NULL) return NULL;
  if (!hashmap_contains(&s->primitive_types, (uint64_t)name_id))
    return NULL;
  return (struct Type *)hashmap_get(&s->primitive_types, (uint64_t)name_id);
}

const char *type_name(const struct Type *t) {
  if (!t) return "<null>";
  switch (t->kind) {
  case TY_ERROR:           return "<error>";
  case TY_VOID:            return "void";
  case TY_BOOL:            return "bool";
  case TY_U8:              return "u8";
  case TY_U16:             return "u16";
  case TY_U32:             return "u32";
  case TY_U64:             return "u64";
  case TY_USIZE:           return "usize";
  case TY_I8:              return "i8";
  case TY_I16:             return "i16";
  case TY_I32:             return "i32";
  case TY_I64:             return "i64";
  case TY_ISIZE:           return "isize";
  case TY_F32:             return "f32";
  case TY_F64:             return "f64";
  case TY_COMPTIME_INT:    return "comptime_int";
  case TY_COMPTIME_FLOAT:  return "comptime_float";
  }
  return "<unknown>";
}

bool type_is_int(const struct Type *t) {
  if (!t) return false;
  switch (t->kind) {
  case TY_U8: case TY_U16: case TY_U32: case TY_U64: case TY_USIZE:
  case TY_I8: case TY_I16: case TY_I32: case TY_I64: case TY_ISIZE:
  case TY_COMPTIME_INT:
    return true;
  default:
    return false;
  }
}

bool type_is_float(const struct Type *t) {
  if (!t) return false;
  return t->kind == TY_F32 || t->kind == TY_F64 ||
         t->kind == TY_COMPTIME_FLOAT;
}

bool type_is_unsigned(const struct Type *t) {
  if (!t) return false;
  switch (t->kind) {
  case TY_U8: case TY_U16: case TY_U32: case TY_U64: case TY_USIZE:
    return true;
  default:
    return false;
  }
}
