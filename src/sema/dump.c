#include "../db/db.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/def_identity.h"
#include "../db/query/infer_body.h"
#include "../db/query/resolve_ref.h"
#include "../db/query/type_of_def.h"
#include "../db/storage/stringpool.h"
#include "sema.h"

#include <stdint.h>
#include <stdio.h>

// === Type renderer ==========================================================
//
// Recursive printer for IpIndex values. Reserved primitives use their
// canonical spelling; compound types (^T, ?T, []T, [N]T, fn(...) → R)
// recover their shape via ip_key and recurse. Nominal types (struct/
// enum) use their declaring DefId — stored as zir_node_id in the key —
// to recover the decl name from db.defs.names.
//
// (Moved out of build.c so the driver doesn't need to know about IpTag
// or the intern pool's key shapes.)

// Generated from ip_primitives.def — `PRIMITIVE_NAMES[IpIndex.v]`
// gives the spelling for every reserved primitive in one O(1) load.
// Adding a new row to ip_primitives.def automatically extends this
// table; gaps (for non-primitive reserved slots like IP_BOOL_TRUE)
// stay NULL via C99 designated initializers and fall through to the
// compound-type dispatch below.
static const char *const PRIMITIVE_NAMES[] = {
#define X(lower, UPPER, SIZE, ALIGN) [IP_INDEX_##UPPER##_TYPE] = #lower,
#include "../db/intern_pool/ip_primitives.def"
#undef X
};

static int format_ip_type(char *buf, size_t cap, struct db *db, IpIndex t) {
  if (cap == 0)
    return 0;
  if (t.v == IP_NONE.v)
    return snprintf(buf, cap, "?");
  InternPool *pool = &db->intern;

  if (t.v < (sizeof PRIMITIVE_NAMES / sizeof PRIMITIVE_NAMES[0]) &&
      PRIMITIVE_NAMES[t.v])
    return snprintf(buf, cap, "%s", PRIMITIVE_NAMES[t.v]);

  IpTag tag = ip_tag(pool, t);
  IpKey k = ip_key(pool, t);
  char inner[256];
  switch (tag) {
  case IP_TAG_PTR_TYPE:
    format_ip_type(inner, sizeof inner, db, k.ptr_type.elem);
    return snprintf(buf, cap, "^%s", inner);
  case IP_TAG_PTR_CONST_TYPE:
    format_ip_type(inner, sizeof inner, db, k.ptr_type.elem);
    return snprintf(buf, cap, "^const %s", inner);
  case IP_TAG_SLICE_TYPE:
    format_ip_type(inner, sizeof inner, db, k.slice_type.elem);
    return snprintf(buf, cap, "[]%s", inner);
  case IP_TAG_SLICE_CONST_TYPE:
    format_ip_type(inner, sizeof inner, db, k.slice_type.elem);
    return snprintf(buf, cap, "[]const %s", inner);
  case IP_TAG_MANY_PTR_TYPE:
    format_ip_type(inner, sizeof inner, db, k.many_ptr_type.elem);
    return snprintf(buf, cap, "[^]%s", inner);
  case IP_TAG_MANY_PTR_CONST_TYPE:
    format_ip_type(inner, sizeof inner, db, k.many_ptr_type.elem);
    return snprintf(buf, cap, "[^]const %s", inner);
  case IP_TAG_OPTIONAL_TYPE:
    format_ip_type(inner, sizeof inner, db, k.optional_type.elem);
    return snprintf(buf, cap, "?%s", inner);
  case IP_TAG_ARRAY_TYPE:
    format_ip_type(inner, sizeof inner, db, k.array_type.elem);
    return snprintf(buf, cap, "[%llu]%s",
                    (unsigned long long)k.array_type.size, inner);
  case IP_TAG_FN_TYPE: {
    size_t off = 0;
    int n = snprintf(buf, cap, "fn(");
    if (n > 0)
      off += (size_t)n;
    for (size_t i = 0; i < k.fn_type.n_params; i++) {
      if (i > 0 && off < cap) {
        n = snprintf(buf + off, cap - off, ", ");
        if (n > 0)
          off += (size_t)n;
      }
      char inner_p[128];
      format_ip_type(inner_p, sizeof inner_p, db, k.fn_type.params[i]);
      if (off < cap) {
        n = snprintf(buf + off, cap - off, "%s", inner_p);
        if (n > 0)
          off += (size_t)n;
      }
    }
    char inner_r[128];
    format_ip_type(inner_r, sizeof inner_r, db, k.fn_type.ret);
    if (off < cap) {
      n = snprintf(buf + off, cap - off, ") -> %s", inner_r);
      if (n > 0)
        off += (size_t)n;
    }
    return (int)off;
  }
  case IP_TAG_STRUCT_TYPE:
  case IP_TAG_ENUM_TYPE: {
    uint32_t def_idx = (tag == IP_TAG_STRUCT_TYPE)
                           ? k.struct_type.zir_node_id
                           : k.enum_type.zir_node_id;
    const char *kw = (tag == IP_TAG_STRUCT_TYPE) ? "struct" : "enum";
    if (def_idx < db->defs.names.count) {
      StrId nm = *(StrId *)vec_get(&db->defs.names, def_idx);
      const char *nm_str = pool_get(&db->strings, nm);
      if (nm_str && nm_str[0])
        return snprintf(buf, cap, "%s %s", kw, nm_str);
    }
    return snprintf(buf, cap, "%s@%u", kw, def_idx);
  }
  default:
    return snprintf(buf, cap, "IP[%u]", t.v);
  }
}

// === Main dump =============================================================

void sema_dump_module(struct db *s, ModuleId mid) {
  ScopeId export_scope = SCOPE_ID_NONE;
  ScopeId internal_scope = SCOPE_ID_NONE;
  if (mid.idx < s->modules.exports.count)
    export_scope = *(ScopeId *)vec_get(&s->modules.exports, mid.idx);
  if (mid.idx < s->modules.internal_scopes.count)
    internal_scope =
        *(ScopeId *)vec_get(&s->modules.internal_scopes, mid.idx);

  if (export_scope.idx == SCOPE_ID_NONE.idx)
    return;

  // === Export scope: pubs with types ===
  printf("\nTop-Level Defs (export scope):\n");
  uint32_t off_start =
      *(uint32_t *)vec_get(&s->scopes.decl_offsets, export_scope.idx);
  uint32_t off_end =
      *(uint32_t *)vec_get(&s->scopes.decl_offsets, export_scope.idx + 1);
  for (uint32_t i = off_start; i < off_end; i++) {
    DeclEntry *de = (DeclEntry *)vec_get(&s->scopes.decl_pool, i);
    DefId def = db_query_def_identity(s, mid, de->ast_id);
    IpIndex t = db_query_type_of_def(s, def);
    char tbuf[256];
    format_ip_type(tbuf, sizeof tbuf, s, t);
    printf("  def=%u  name=%-20s ast_id=%08x  type=%s\n", def.idx,
           pool_get(&s->strings, de->name), de->ast_id.idx, tbuf);
  }
  uint32_t int_start = 0, int_end = 0;
  if (internal_scope.idx != SCOPE_ID_NONE.idx) {
    int_start =
        *(uint32_t *)vec_get(&s->scopes.decl_offsets, internal_scope.idx);
    int_end =
        *(uint32_t *)vec_get(&s->scopes.decl_offsets, internal_scope.idx + 1);
  }
  printf("  (internal scope: %u defs, export scope: %u defs)\n\n",
         int_end - int_start, off_end - off_start);

  if (internal_scope.idx == SCOPE_ID_NONE.idx)
    return;

  // === Name-resolution sanity check ===
  db_request_begin(s, 1);
  uint32_t mismatches = 0;
  for (uint32_t i = int_start; i < int_end; i++) {
    DeclEntry *de = (DeclEntry *)vec_get(&s->scopes.decl_pool, i);
    DefId expected = db_query_def_identity(s, mid, de->ast_id);
    DefId resolved = db_query_resolve_ref(s, internal_scope, de->name);
    if (resolved.idx != expected.idx) {
      printf("  [resolve mismatch] name=%s resolved=%u expected=%u\n",
             pool_get(&s->strings, de->name), resolved.idx, expected.idx);
      mismatches++;
    }
  }
  db_request_end(s);
  if (mismatches == 0)
    printf("Name resolution: %u/%u round-trip ok\n\n",
           int_end - int_start, int_end - int_start);

  // === Internal-scope top-level types ===
  printf("Top-Level Types (internal scope):\n");
  db_request_begin(s, 1);
  for (uint32_t i = int_start; i < int_end; i++) {
    DeclEntry *de = (DeclEntry *)vec_get(&s->scopes.decl_pool, i);
    DefId def = db_query_def_identity(s, mid, de->ast_id);
    IpIndex t = db_query_type_of_def(s, def);
    char tbuf[256];
    format_ip_type(tbuf, sizeof tbuf, s, t);
    printf("  def=%u  name=%-20s type=%s\n", def.idx,
           pool_get(&s->strings, de->name), tbuf);
  }
  db_request_end(s);
  printf("\n");

  // === Per-fn local scopes ===
  printf("Body Inference (per-fn local scope):\n");
  db_request_begin(s, 1);
  size_t fn_count = 0;
  for (uint32_t i = int_start; i < int_end; i++) {
    DeclEntry *de = (DeclEntry *)vec_get(&s->scopes.decl_pool, i);
    DefId def = db_query_def_identity(s, mid, de->ast_id);
    IpIndex sig = db_query_infer_body(s, def);
    if (sig.v == IP_NONE.v)
      continue;
    char tbuf[256];
    format_ip_type(tbuf, sizeof tbuf, s, sig);
    printf("  fn %s : %s\n", pool_get(&s->strings, de->name), tbuf);
    Vec *scope = NULL;
    if (def.idx < s->defs.local_scopes.count)
      scope = *(Vec **)vec_get(&s->defs.local_scopes, def.idx);
    if (scope) {
      LocalBind *binds = (LocalBind *)scope->data;
      for (size_t e = 0; e < scope->count; e++) {
        char tb[256];
        format_ip_type(tb, sizeof tb, s, binds[e].type);
        printf("    local %-16s : %s\n",
               pool_get(&s->strings, binds[e].name), tb);
      }
      if (scope->count == 0)
        printf("    (no params)\n");
    } else {
      printf("    (no scope)\n");
    }
    fn_count++;
  }
  db_request_end(s);
  if (fn_count == 0)
    printf("  (no fn defs)\n");
  printf("\n");
}
