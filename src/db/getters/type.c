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
#include <string.h>

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

// Recursion bound, mirroring ip_format's IP_FORMAT_MAX_DEPTH. Each level
// allocates stack buffers (inner / inner_p / inner_r), so an unbounded
// type nesting (e.g. ?^?^...i32) would overflow the stack. The intern
// pool prevents *structural* cycles, but generated/pathological nesting
// can still be arbitrarily deep — cap it and render "..." past the bound.
#define DB_FORMAT_TYPE_MAX_DEPTH 16

static size_t format_rec(struct db *s, IpIndex t, char *buf, size_t cap,
                         int depth) {
  if (cap == 0)
    return 0;
  if (depth > DB_FORMAT_TYPE_MAX_DEPTH) {
    int n = snprintf(buf, cap, "...");
    return n < 0 ? 0 : (size_t)n;
  }
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
    format_rec(s, k.ptr_type.elem, inner, sizeof inner, depth + 1);
    n = snprintf(buf, cap, "^%s", inner);
    break;
  case IP_TAG_PTR_CONST_TYPE:
    format_rec(s, k.ptr_type.elem, inner, sizeof inner, depth + 1);
    n = snprintf(buf, cap, "^const %s", inner);
    break;
  case IP_TAG_SLICE_TYPE:
    format_rec(s, k.slice_type.elem, inner, sizeof inner, depth + 1);
    n = snprintf(buf, cap, "[]%s", inner);
    break;
  case IP_TAG_SLICE_CONST_TYPE:
    format_rec(s, k.slice_type.elem, inner, sizeof inner, depth + 1);
    n = snprintf(buf, cap, "[]const %s", inner);
    break;
  case IP_TAG_MANY_PTR_TYPE:
    format_rec(s, k.many_ptr_type.elem, inner, sizeof inner, depth + 1);
    n = snprintf(buf, cap, "[^]%s", inner);
    break;
  case IP_TAG_MANY_PTR_CONST_TYPE:
    format_rec(s, k.many_ptr_type.elem, inner, sizeof inner, depth + 1);
    n = snprintf(buf, cap, "[^]const %s", inner);
    break;
  case IP_TAG_OPTIONAL_TYPE:
    format_rec(s, k.optional_type.elem, inner, sizeof inner, depth + 1);
    n = snprintf(buf, cap, "?%s", inner);
    break;
  case IP_TAG_ARRAY_TYPE:
    format_rec(s, k.array_type.elem, inner, sizeof inner, depth + 1);
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
      format_rec(s, k.fn_type.params[i], inner_p, sizeof inner_p, depth + 1);
      if (off < cap) {
        m = snprintf(buf + off, cap - off, "%s", inner_p);
        if (m > 0)
          off += (size_t)m;
      }
    }
    char inner_r[128];
    format_rec(s, k.fn_type.ret, inner_r, sizeof inner_r, depth + 1);
    if (off < cap) {
      // Effect row between `)` and `->`, matching source syntax
      // `fn(params) <eff> -> ret`. Pure fns (empty row) omit it, as in source.
      if (k.fn_type.effect_row.v != IP_EMPTY_EFFECT_ROW.v) {
        char inner_e[128];
        format_rec(s, k.fn_type.effect_row, inner_e, sizeof inner_e, depth + 1);
        m = snprintf(buf + off, cap - off, ") %s -> %s", inner_e, inner_r);
      } else {
        m = snprintf(buf + off, cap - off, ") -> %s", inner_r);
      }
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
  case IP_TAG_EFFECT_ROW: {
    // Effects-1 — render `<l1, l2, ...>` resolving each DefId label to
    // its declared name via db.defs.names. The intern pool's own
    // format_recursive can only emit "def#N" placeholders since it
    // doesn't back-reference the db; this is the diag-facing render
    // that names "exn", "io", etc. by literal text. An open tail (row
    // var) is rendered as `<l1, ... | rv#N>` for diag clarity; closed
    // rows (terminator == IP_EMPTY_EFFECT_ROW) omit the tail.
    size_t off = 0;
    int m = snprintf(buf, cap, "<");
    if (m > 0)
      off += (size_t)m;
    for (size_t i = 0; i < k.effect_row.n_labels; i++) {
      if (i > 0 && off < cap) {
        m = snprintf(buf + off, cap - off, ", ");
        if (m > 0)
          off += (size_t)m;
      }
      uint32_t def_idx = k.effect_row.labels[i].idx;
      const char *nm_str = NULL;
      if (def_idx < s->defs.names.count) {
        StrId nm = *(StrId *)vec_get(&s->defs.names, def_idx);
        nm_str = pool_get(&s->strings, nm);
      }
      if (off < cap) {
        if (nm_str && nm_str[0])
          m = snprintf(buf + off, cap - off, "%s", nm_str);
        else
          m = snprintf(buf + off, cap - off, "def#%u", def_idx);
        if (m > 0)
          off += (size_t)m;
      }
    }
    if (k.effect_row.tail.v != IP_EMPTY_EFFECT_ROW.v && off < cap) {
      if (k.effect_row.n_labels > 0)
        m = snprintf(buf + off, cap - off, " | ");
      else
        m = snprintf(buf + off, cap - off, "..");
      if (m > 0)
        off += (size_t)m;
      char inner_t[128];
      format_rec(s, k.effect_row.tail, inner_t, sizeof inner_t, depth + 1);
      if (off < cap) {
        m = snprintf(buf + off, cap - off, "%s", inner_t);
        if (m > 0)
          off += (size_t)m;
      }
    }
    if (off < cap) {
      m = snprintf(buf + off, cap - off, ">");
      if (m > 0)
        off += (size_t)m;
    }
    return off;
  }
  case IP_TAG_ROW_VAR:
    n = snprintf(buf, cap, "rv#%u", k.row_var.id);
    break;
  case IP_TAG_EFFECT_TYPE: {
    // Nominal effect type — resolve declaring DefId → name via
    // db.defs.names (same pattern as STRUCT/ENUM above). The
    // zir_node_id field doubles as the DefId.idx for effect decls.
    uint32_t def_idx = k.effect_type.zir_node_id;
    if (def_idx < s->defs.names.count) {
      StrId nm = *(StrId *)vec_get(&s->defs.names, def_idx);
      const char *nm_str = pool_get(&s->strings, nm);
      if (nm_str && nm_str[0]) {
        n = snprintf(buf, cap, "%s", nm_str);
        break;
      }
    }
    n = snprintf(buf, cap, "effect@%u", def_idx);
    break;
  }
  case IP_TAG_HANDLER_TYPE: {
    char inner_e[128];
    char inner_a[128];
    char inner_r[128];
    format_rec(s, k.handler_type.effect, inner_e, sizeof inner_e, depth + 1);
    format_rec(s, k.handler_type.action, inner_a, sizeof inner_a, depth + 1);
    format_rec(s, k.handler_type.ret, inner_r, sizeof inner_r, depth + 1);
    // handler<effect>(action -> answer): discharges effect, maps action → answer
    n = snprintf(buf, cap, "handler<%s>(%s -> %s)", inner_e, inner_a, inner_r);
    break;
  }
  case IP_TAG_NAMESPACE_TYPE: {
    // Render as `namespace<basename(path)>` for terseness. The full
    // canonical path is recoverable from the diag's primary span if
    // a user needs it; the inline rendering is meant to identify
    // *which* namespace, not where it lives on disk. Falls back to
    // `namespace#<nsid>` if we can't resolve the path (shouldn't
    // happen post-Phase-1c — every namespace has exactly one file —
    // but defensive against evicted / partially-registered state).
    NamespaceId nsid = k.namespace_type.nsid;
    uint32_t n_files = 0;
    const FileId *files = db_get_namespace_files(s, nsid, &n_files);
    const char *base = NULL;
    if (files && n_files > 0) {
      SourceId src = db_get_file_source(s, files[0]);
      StrId path_id = db_get_source_path(s, src);
      const char *path = pool_get(&s->strings, path_id);
      if (path && path[0]) {
        // basename: last '/' onward (POSIX paths; the workspace
        // canonicalizes via realpath, so paths are always absolute
        // POSIX-style on supported platforms).
        const char *slash = strrchr(path, '/');
        base = slash ? slash + 1 : path;
      }
    }
    if (base && base[0])
      n = snprintf(buf, cap, "namespace<%s>", base);
    else
      n = snprintf(buf, cap, "namespace#%u", nsid.idx);
    break;
  }
  default:
    n = snprintf(buf, cap, "IP[%u]", t.v);
    break;
  }
  return n < 0 ? 0 : (size_t)n;
}

size_t db_format_type(struct db *s, IpIndex t, char *buf, size_t cap) {
  return format_rec(s, t, buf, cap, 0);
}
