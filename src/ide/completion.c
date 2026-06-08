// Completion — `.` trigger member-access completion.
//
// Same architectural pattern as ide_hover_at: pure read of the typed
// node cache (no sema re-synthesis), request-wrap because the
// namespace-member resolution calls db_query_type_of_def.

#include "ide.h"

#include "../db/db.h"
#include "../db/intern_pool/intern_pool.h"
#include "../syntax/syntax.h"

#include <stdbool.h>
#include <string.h>

// Query entry points (no per-query headers post-D1; db_query_ctx == struct db).
extern IpIndex db_query_node_type(db_query_ctx *ctx, FileId fid,
                                  SyntaxNode *node);
extern IpIndex db_query_type_of_def(db_query_ctx *ctx, DefId def);

// LSP CompletionItemKind subset (mirrors values in ide.h).
#define LSP_KIND_METHOD 2
#define LSP_KIND_FUNCTION 3
#define LSP_KIND_FIELD 5
#define LSP_KIND_VARIABLE 6
#define LSP_KIND_PROPERTY 10
#define LSP_KIND_ENUM_MEMBER 20
#define LSP_KIND_CONSTANT 21

static char *arena_strdup(Arena *arena, const char *s) {
  if (!s)
    return NULL;
  size_t len = strlen(s);
  char *out = (char *)arena_alloc_raw(arena, len + 1);
  memcpy(out, s, len);
  out[len] = '\0';
  return out;
}

static char *arena_format_type(struct db *s, Arena *arena, IpIndex type) {
  char tmp[256];
  db_format_type(s, type, tmp, sizeof tmp);
  return arena_strdup(arena, tmp);
}

static void push_completion(Arena *arena, Vec *out, const char *label,
                            const char *detail, int32_t kind) {
  if (!label || !label[0])
    return;
  IdeCompletion c = {
      .label = arena_strdup(arena, label),
      .detail = detail ? arena_strdup(arena, detail) : NULL,
      .kind = kind,
  };
  vec_push(out, &c);
}

static int32_t lsp_kind_from_def_kind(DefKind k) {
  switch (k) {
  case KIND_FUNCTION:
    return LSP_KIND_FUNCTION;
  case KIND_CONSTANT:
    return LSP_KIND_CONSTANT;
  case KIND_VARIABLE:
    return LSP_KIND_VARIABLE;
  case KIND_STRUCT:
  case KIND_UNION:
  case KIND_ENUM:
  case KIND_EFFECT:
    return LSP_KIND_FIELD;
  default:
    return LSP_KIND_VARIABLE;
  }
}

size_t ide_completions_at(struct db *db, FileId fid, uint32_t line0,
                          uint32_t char0, Arena *arena, Vec *out) {
  if (!file_id_valid(fid))
    return 0;

  // Cursor is just past the trigger char. Read source bytes to find
  // the dot + walk back to the receiver's last token.
  SourceId sid = db_get_file_source(db, fid);
  if (!source_id_valid(sid))
    return 0;
  if (sid.idx >= db->sources.texts.count)
    return 0;
  const char *text = *(const char **)vec_get(&db->sources.texts, sid.idx);
  uint32_t text_len = *(uint32_t *)vec_get(&db->sources.text_lens, sid.idx);
  if (!text || text_len == 0)
    return 0;

  uint32_t cursor_off = db_byte_offset_at(db, fid, line0, char0);
  if (cursor_off == UINT32_MAX || cursor_off == 0)
    return 0;

  // Walk back over whitespace to find the trigger `.`.
  uint32_t i = cursor_off;
  while (i > 0 && i <= text_len && (text[i - 1] == ' ' || text[i - 1] == '\t'))
    i--;
  if (i == 0 || text[i - 1] != '.')
    return 0;
  uint32_t dot_off = i - 1;
  if (dot_off == 0)
    return 0;

  // Innermost node containing the byte just before `.` is the receiver.
  SyntaxNode *recv_node = db_node_at_offset(db, fid, dot_off - 1);
  if (!recv_node)
    return 0;

  // Unified node→type router: walks parents to enclosing def, drives
  // the matching per-decl query (infer_body etc.), returns the
  // receiver's resolved type via the per-decl HashMap-backed
  // NodeTypesRange tables.
  db_request_begin(db, db_current_revision(db));
  IpIndex recv_type = db_query_node_type(db, fid, recv_node);
  syntax_node_release(recv_node);
  if (recv_type.v == IP_NONE.v) {
    db_request_end(db);
    return 0;
  }

  // Auto-deref single pointers — `ptr.field` reads through `^T`.
  IpTag tag = ip_tag(&db->intern, recv_type);
  if (tag == IP_TAG_PTR_TYPE || tag == IP_TAG_PTR_CONST_TYPE) {
    IpKey pk = ip_key(&db->intern, recv_type);
    recv_type = pk.ptr_type.elem;
    tag = ip_tag(&db->intern, recv_type);
  }

  switch (tag) {
  case IP_TAG_STRUCT_TYPE: {
    // Nominal: identity is the declaring def (zir_node_id == def.idx). Fields
    // live in the db aggregate-field pool, not the inline IpKey (D2.1b).
    DefId d = {.idx = ip_key(&db->intern, recv_type).struct_type.zir_node_id};
    uint32_t nf = db_aggregate_field_count(db, d);
    for (uint32_t fi = 0; fi < nf; fi++) {
      AggregateFieldEntry e = db_aggregate_field_at(db, d, fi);
      const char *name = pool_get(&db->strings, e.name);
      char *detail = arena_format_type(db, arena, e.type);
      IpTag ft = ip_tag(&db->intern, e.type);
      int32_t kind = (ft == IP_TAG_FN_TYPE) ? LSP_KIND_METHOD : LSP_KIND_FIELD;
      push_completion(arena, out, name, detail, kind);
    }
    break;
  }
  case IP_TAG_ENUM_TYPE: {
    DefId d = {.idx = ip_key(&db->intern, recv_type).enum_type.zir_node_id};
    char *detail = arena_format_type(db, arena, recv_type);
    uint32_t nv = 0;
    const EnumVariantEntry *vs = db_enum_variants(db, d, &nv);
    for (uint32_t vi = 0; vi < nv; vi++) {
      const char *name = pool_get(&db->strings, vs[vi].name);
      push_completion(arena, out, name, detail, LSP_KIND_ENUM_MEMBER);
    }
    break;
  }
  case IP_TAG_SLICE_TYPE:
  case IP_TAG_SLICE_CONST_TYPE: {
    push_completion(arena, out, "len", "usize", LSP_KIND_PROPERTY);
    IpKey sk = ip_key(&db->intern, recv_type);
    bool is_const = (tag == IP_TAG_SLICE_CONST_TYPE);
    IpKey mp = {
        .kind = IPK_MANY_PTR_TYPE,
        .many_ptr_type = {.elem = sk.slice_type.elem, .is_const = is_const}};
    IpIndex ptr_type = ip_get(&db->intern, mp);
    char *detail = arena_format_type(db, arena, ptr_type);
    push_completion(arena, out, "ptr", detail, LSP_KIND_PROPERTY);
    break;
  }
  case IP_TAG_ARRAY_TYPE:
    push_completion(arena, out, "len", "usize", LSP_KIND_PROPERTY);
    break;
  case IP_TAG_NAMESPACE_TYPE: {
    // Members live in the db namespace-field pool, keyed by the inline nsid;
    // member TYPES are lazy (db_query_type_of_def per member).
    NamespaceId nsid = ip_key(&db->intern, recv_type).namespace_type.nsid;
    uint32_t nm = db_namespace_member_count(db, nsid);
    for (uint32_t mi = 0; mi < nm; mi++) {
      DeclEntry m = db_namespace_member_at(db, nsid, mi);
      const char *name = pool_get(&db->strings, m.name);
      IpIndex member_type = db_query_type_of_def(db, m.def);
      char *detail = (member_type.v != IP_NONE.v)
                         ? arena_format_type(db, arena, member_type)
                         : NULL;
      DefKind dk = db_def_kind(db, m.def);
      push_completion(arena, out, name, detail, lsp_kind_from_def_kind(dk));
    }
    break;
  }
  default:
    // Field access on a non-aggregate — nothing to complete.
    break;
  }

  db_request_end(db);
  return out->count;
}
