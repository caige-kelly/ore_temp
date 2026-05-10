#include "layout.h"

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../../diag/diag.h"
#include "../ids/ids.h"
#include "../query/query_engine.h"
#include "../sema.h"
#include "decl_data.h"
#include "type.h"

// Target ABI assumptions.
//
// 64-bit LP64: pointers, usize/isize, slices' length all 8 bytes /
// 8-byte aligned. Slices are { ptr, len } → 16 bytes / 8 align. When
// we grow a real Target abstraction (multi-arch codegen), this is
// the layer that consults it. For now: every system Ore runs on is
// 64-bit; hardcode and document.
#define ORE_PTR_SIZE 8u
#define ORE_PTR_ALIGN 8u

// Round `n` up to the next multiple of `align` (assumes align is a
// power of two; that's our invariant for Layout.align).
static uint64_t align_up(uint64_t n, uint64_t align) {
  if (align == 0)
    return n;
  return (n + align - 1u) & ~(align - 1u);
}

static struct Layout unknown(void) {
  return (struct Layout){.size = 0, .align = 0, .is_known = false};
}

static struct Layout fixed(uint64_t size, uint64_t align) {
  return (struct Layout){.size = size, .align = align, .is_known = true};
}

// Lookup-or-create the per-Type layout cache entry. Keyed by Type*
// pointer because compound types are interned (so pointer equality =
// type identity). Primitives also have unique Type*s thanks to
// `make_primitive`'s arena allocation.
static struct LayoutEntry *layout_entry_for(struct Sema *s, struct Type *t) {
  if (s->layout_of_type.entries == NULL)
    hashmap_init_in(&s->layout_of_type, &s->arena);
  uint64_t key = (uint64_t)(uintptr_t)t;
  if (hashmap_contains(&s->layout_of_type, key))
    return (struct LayoutEntry *)hashmap_get(&s->layout_of_type, key);
  struct LayoutEntry *e = arena_alloc(&s->arena, sizeof(*e));
  *e = (struct LayoutEntry){0};
  sema_query_slot_init(&e->query, QUERY_LAYOUT_OF_TYPE);
  hashmap_put_or_die(&s->layout_of_type, key, e, "layout_of_type");
  return e;
}

// Layout for primitive scalar types. Mirrors the table that lived
// inline in const_eval.c's expr_Builtin case pre-R5; pulling it here
// makes both paths share one source of truth.
static struct Layout primitive_layout(struct Sema *s, struct Type *t) {
  if (t == s->bool_type)
    return fixed(1, 1);
  if (t == s->u8_type || t == s->i8_type)
    return fixed(1, 1);
  if (t == s->u16_type || t == s->i16_type)
    return fixed(2, 2);
  if (t == s->u32_type || t == s->i32_type || t == s->f32_type)
    return fixed(4, 4);
  if (t == s->u64_type || t == s->i64_type || t == s->f64_type)
    return fixed(8, 8);
  if (t == s->usize_type || t == s->isize_type)
    return fixed(ORE_PTR_SIZE, ORE_PTR_ALIGN);
  if (t == s->void_type)
    return fixed(0, 1);
  // comptime_int / comptime_float / nil / type / noreturn / error
  // have no runtime layout — they're either compile-time-only or
  // ABI-meaningless. Caller handles.
  return unknown();
}

// Layout for an optional `?T`.
//
// Two encodings are common:
//  - "Tagged": { value: T; has_value: bool } — size = align_up(sizeof(T), 1)
//    + 1, padded to align(T). Always works.
//  - "Niche": when T is a non-nullable pointer / slice, the runtime
//    can use the all-zero bit pattern as the nil sentinel and skip
//    the tag byte. Saves space; matches Zig's `?*T` / `?[]T` shape.
//
// Today we use the niche encoding for pointer-shaped T (so `?^i32`
// is 8 bytes, same as `^i32` would be if it were nullable) and the
// tagged encoding otherwise. Same final size as Zig's optimization.
static bool is_niche_optimizable(struct Type *t) {
  return t->kind == TY_PTR || t->kind == TY_MANY_PTR || t->kind == TY_SLICE ||
         t->kind == TY_FN;
}

struct Layout query_layout_of_type(struct Sema *s, struct Type *t) {
  if (!s || !t)
    return unknown();
  if (t->kind == TY_ERROR)
    return unknown();

  struct LayoutEntry *entry = layout_entry_for(s, t);

  struct Span dummy = {0};
  SEMA_QUERY_GUARD(s, &entry->query, QUERY_LAYOUT_OF_TYPE, entry, dummy,
                   /*on_cached=*/entry->layout,
                   /*on_cycle=*/unknown(),
                   /*on_error=*/unknown());

  struct Layout result = unknown();

  switch (t->kind) {
  case TY_FN:
    // Function values are always passed as code pointers. Size +
    // alignment match a single pointer.
    result = fixed(ORE_PTR_SIZE, ORE_PTR_ALIGN);
    break;

  case TY_PTR:
  case TY_MANY_PTR:
    result = fixed(ORE_PTR_SIZE, ORE_PTR_ALIGN);
    break;

  case TY_SLICE:
    // {ptr, len} pair. Both pointer-sized on 64-bit.
    result = fixed(2 * ORE_PTR_SIZE, ORE_PTR_ALIGN);
    break;

  case TY_ARRAY: {
    struct Layout elem = query_layout_of_type(s, t->array.elem);
    if (!elem.is_known)
      break;
    if (t->array.size == 0) {
      result = fixed(0, elem.align ? elem.align : 1);
      break;
    }
    // Total size = N × element size, no inter-element padding (each
    // element is already aligned because elem.size is a multiple of
    // elem.align by the recursive contract).
    result = fixed(elem.size * t->array.size, elem.align);
    break;
  }

  case TY_OPTIONAL: {
    struct Layout inner = query_layout_of_type(s, t->optional.elem);
    if (!inner.is_known)
      break;
    if (is_niche_optimizable(t->optional.elem)) {
      // Niche: borrow the zero bit-pattern as nil.
      result = inner;
    } else {
      // Tagged: { value: inner; has_value: u8 }, padded to inner.align.
      // Place the tag at offset = inner.size; total = align_up(
      // inner.size + 1, inner.align).
      uint64_t total = align_up(inner.size + 1, inner.align ? inner.align : 1);
      result = fixed(total, inner.align ? inner.align : 1);
    }
    break;
  }

  case TY_STRUCT: {
    // C-style layout: walk fields in declaration order; each field
    // starts at align_up(current_offset, field.align). Total size is
    // align_up(after_last_field, struct.align). Anonymous unions
    // (union_group != 0) overlay — group's size is max of arms,
    // group's align is max of arms. Defer the union case for now:
    // emit a "union arms not yet laid out" diagnostic and return
    // unknown if any field has union_group != 0.
    struct StructSignature *sig = query_struct_signature(s, t->struct_.def);
    if (!sig)
      break;

    uint64_t offset = 0;
    uint64_t struct_align = 1;
    bool any_union = false;
    bool any_unknown = false;

    for (size_t i = 0; i < sig->field_count; i++) {
      struct FieldData *f = &sig->fields[i];
      if (f->union_group != 0) {
        any_union = true;
        break;
      }

      // Pre-PR-3.5 cycle detection: a field's type is `t` (or a
      // by-value descendant of `t`) means infinite-size. We rely on
      // SEMA_QUERY_GUARD's RUNNING-state cycle detection above —
      // recursing back into query_layout_of_type for `t` returns
      // the on_cycle sentinel (unknown).
      struct Layout fl = query_layout_of_type(s, f->type);
      if (!fl.is_known) {
        any_unknown = true;
        struct DefInfo *di = def_info(s, t->struct_.def);
        const char *name = di ? pool_get(&s->pool, di->name_id, 0) : "?";
        if (f->type == t) {
          // Direct self-cycle: `Bad :: struct { self: Bad }`. Most
          // user-friendly diagnostic.
          diag_error(&s->diags, f->span,
                     "struct '%s' contains itself by value via field '%s' "
                     "(use `^%s` or `?^%s` for an indirect reference)",
                     name, pool_get(&s->pool, f->name_id, 0), name, name);
        } else if (f->type->kind == TY_STRUCT) {
          // Transitive cycle through another nominal struct, OR the
          // field's struct independently failed to lay out. Either
          // way the user wants a hint. Naming the offender by spelling
          // helps regardless of which layer detected the cycle.
          struct DefInfo *fdi = def_info(s, f->type->struct_.def);
          const char *fname = fdi ? pool_get(&s->pool, fdi->name_id, 0) : "?";
          diag_error(&s->diags, f->span,
                     "struct '%s' field '%s' has unresolvable layout "
                     "(field type '%s' has no known size — likely a "
                     "by-value cycle; use `^%s` or `?^%s`)",
                     name, pool_get(&s->pool, f->name_id, 0), fname, fname,
                     fname);
        }
        // For TY_ARRAY / TY_ENUM / etc. the child layout already
        // emitted (or chose to silently propagate). Don't double-up.
        break;
      }
      // Place this field. Pad as needed.
      uint64_t fa = fl.align ? fl.align : 1;
      offset = align_up(offset, fa);
      offset += fl.size;
      if (fa > struct_align)
        struct_align = fa;
    }

    if (any_unknown || any_union)
      break;

    uint64_t total = align_up(offset, struct_align);
    result = fixed(total, struct_align);
    break;
  }

  case TY_ENUM: {
    // Width is the smallest int that fits the variant value range.
    // Auto-incremented values start at 0; explicit values can drop
    // to negative or jump arbitrarily high. Walk all variants and
    // pick the byte width that covers [min, max].
    struct EnumSignature *sig = query_enum_signature(s, t->enum_.def);
    if (!sig || sig->variant_count == 0) {
      result = fixed(1, 1); // empty / unset enum: zero variants → byte
      break;
    }
    int64_t mn = sig->variants[0].value;
    int64_t mx = sig->variants[0].value;
    for (size_t i = 1; i < sig->variant_count; i++) {
      int64_t v = sig->variants[i].value;
      if (v < mn)
        mn = v;
      if (v > mx)
        mx = v;
    }
    bool has_negative = (mn < 0);
    uint64_t span = (uint64_t)(has_negative ? (mx - mn) : mx);
    if (!has_negative && span <= 0xFF)
      result = fixed(1, 1);
    else if (!has_negative && span <= 0xFFFF)
      result = fixed(2, 2);
    else if (span <= 0xFFFFFFFFu)
      result = fixed(4, 4);
    else
      result = fixed(8, 8);
    break;
  }

  // Compile-time-only / ABI-meaningless types. Layout query is a
  // category error — caller (typically @sizeOf in const_eval) just
  // gets `unknown` and falls through to its own diagnostic.
  case TY_COMPTIME_INT:
  case TY_COMPTIME_FLOAT:
  case TY_NIL:
  case TY_TYPE:
  case TY_NORETURN:
  case TY_ERROR:
    result = unknown();
    break;

  // Primitives — share the static table.
  default:
    result = primitive_layout(s, t);
    break;
  }

  entry->layout = result;
  // Fingerprint: pack size + align + known into a single u64. Two
  // layouts with the same triple hash the same; downstream queries
  // depending on the result early-cut when the layout is unchanged.
  uint64_t fp = (result.size & 0xFFFFFFFFFFFF0000ull) |
                ((result.align & 0xFFull) << 8) | (result.is_known ? 1u : 0u);
  query_slot_set_fingerprint(&entry->query, fp);
  sema_query_succeed(s, &entry->query);
  return result;
}
