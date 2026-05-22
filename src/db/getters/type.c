// Type renderer — IpIndex → human-readable string.
//
// Recursive printer for IpIndex values. Reserved primitives use their
// canonical spelling; compound types (^T, ?T, []T, [N]T, fn(...) → R)
// recover their shape via ip_key and recurse. Nominal types (struct/
// enum) use their declaring DefId — stored as zir_node_id in the key —
// to recover the decl name from db.defs.names.
//
// Snprintf-style: returns the number of bytes that WOULD have been
// written to a sufficiently large buffer (NUL not counted), and always
// NUL-terminates within `cap`.

#include "../db.h"
#include "../intern_pool/intern_pool.h"

#include <stdio.h>

// Primitive type names, generated from the X-macro in
// db/intern_pool/ip_primitives.def. PRIMITIVE_NAMES[IpIndex.v] = "i32",
// "bool", etc. Gaps for non-primitive reserved slots (e.g. IP_BOOL_TRUE)
// stay NULL via designated initializers and fall through to the
// compound-type dispatch below.
static const char *const PRIMITIVE_NAMES[] = {
#define X(lower, UPPER, SIZE, ALIGN) [IP_INDEX_##UPPER##_TYPE] = #lower,
#include "../intern_pool/ip_primitives.def"
#undef X
};

size_t db_format_type(struct db *s, IpIndex t, char *buf, size_t cap) {
  if (cap == 0)
    return 0;
  if (t.v == IP_NONE.v) {
    int n = snprintf(buf, cap, "?");
    return n < 0 ? 0 : (size_t)n;
  }

  if (t.v < (sizeof PRIMITIVE_NAMES / sizeof PRIMITIVE_NAMES[0]) &&
      PRIMITIVE_NAMES[t.v]) {
    int n = snprintf(buf, cap, "%s", PRIMITIVE_NAMES[t.v]);
    return n < 0 ? 0 : (size_t)n;
  }

  IpTag tag = ip_tag(&s->intern, t);
  IpKey k = ip_key(&s->intern, t);
  char inner[256];
  int n = 0;
  switch (tag) {
  case IP_TAG_PTR_TYPE:
    db_format_type(s, k.ptr_type.elem, inner, sizeof inner);
    n = snprintf(buf, cap, "^%s", inner);
    break;
  case IP_TAG_PTR_CONST_TYPE:
    db_format_type(s, k.ptr_type.elem, inner, sizeof inner);
    n = snprintf(buf, cap, "^const %s", inner);
    break;
  case IP_TAG_SLICE_TYPE:
    db_format_type(s, k.slice_type.elem, inner, sizeof inner);
    n = snprintf(buf, cap, "[]%s", inner);
    break;
  case IP_TAG_SLICE_CONST_TYPE:
    db_format_type(s, k.slice_type.elem, inner, sizeof inner);
    n = snprintf(buf, cap, "[]const %s", inner);
    break;
  case IP_TAG_MANY_PTR_TYPE:
    db_format_type(s, k.many_ptr_type.elem, inner, sizeof inner);
    n = snprintf(buf, cap, "[^]%s", inner);
    break;
  case IP_TAG_MANY_PTR_CONST_TYPE:
    db_format_type(s, k.many_ptr_type.elem, inner, sizeof inner);
    n = snprintf(buf, cap, "[^]const %s", inner);
    break;
  case IP_TAG_OPTIONAL_TYPE:
    db_format_type(s, k.optional_type.elem, inner, sizeof inner);
    n = snprintf(buf, cap, "?%s", inner);
    break;
  case IP_TAG_ARRAY_TYPE:
    db_format_type(s, k.array_type.elem, inner, sizeof inner);
    n = snprintf(buf, cap, "[%llu]%s", (unsigned long long)k.array_type.size,
                 inner);
    break;
  case IP_TAG_FN_TYPE: {
    size_t off = 0;
    int m = snprintf(buf, cap, "fn(");
    if (m > 0)
      off += (size_t)m;
    for (size_t i = 0; i < k.fn_type.n_params; i++) {
      if (i > 0 && off < cap) {
        m = snprintf(buf + off, cap - off, ", ");
        if (m > 0)
          off += (size_t)m;
      }
      char inner_p[128];
      db_format_type(s, k.fn_type.params[i], inner_p, sizeof inner_p);
      if (off < cap) {
        m = snprintf(buf + off, cap - off, "%s", inner_p);
        if (m > 0)
          off += (size_t)m;
      }
    }
    char inner_r[128];
    db_format_type(s, k.fn_type.ret, inner_r, sizeof inner_r);
    if (off < cap) {
      m = snprintf(buf + off, cap - off, ") -> %s", inner_r);
      if (m > 0)
        off += (size_t)m;
    }
    return off;
  }
  case IP_TAG_STRUCT_TYPE:
  case IP_TAG_ENUM_TYPE: {
    uint32_t def_idx = (tag == IP_TAG_STRUCT_TYPE) ? k.struct_type.zir_node_id
                                                   : k.enum_type.zir_node_id;
    const char *kw = (tag == IP_TAG_STRUCT_TYPE) ? "struct" : "enum";
    if (def_idx < s->defs.names.count) {
      StrId nm = *(StrId *)vec_get(&s->defs.names, def_idx);
      const char *nm_str = pool_get(&s->strings, nm);
      if (nm_str && nm_str[0]) {
        n = snprintf(buf, cap, "%s %s", kw, nm_str);
        break;
      }
    }
    n = snprintf(buf, cap, "%s@%u", kw, def_idx);
    break;
  }
  default:
    n = snprintf(buf, cap, "IP[%u]", t.v);
    break;
  }
  return n < 0 ? 0 : (size_t)n;
}
