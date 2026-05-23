#include "db.h"

#include "uri.h"

#include "../../db/intern_pool/intern_pool.h"
#include "../../db/query/resolve_ref.h"
#include "../../db/query/type_of_def.h"
#include "../../db/workspace/workspace.h"
#include "../../parser/ast.h"
#include "../../sema/sema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void oredb_init(struct OreDb *lsp_db) {
  db_init(&lsp_db->db);
  vec_init(&lsp_db->drafts, sizeof(struct Draft));
}

void oredb_free(struct OreDb *lsp_db) {
  vec_free(&lsp_db->drafts);
  db_free(&lsp_db->db);
}

// Grow the drafts Vec so `drafts[src.idx]` is accessible. New slots
// are zero-initialized — lsp_synced=false, version=0 — matching the
// "never opened" semantics.
static struct Draft *ensure_draft_slot(struct OreDb *lsp_db, SourceId src) {
  while (lsp_db->drafts.count <= src.idx) {
    *(struct Draft *)vec_push_slot(&lsp_db->drafts) = (struct Draft){0};
  }
  return (struct Draft *)vec_get(&lsp_db->drafts, src.idx);
}

SourceId oredb_did_open(struct OreDb *lsp_db, const char *uri, int32_t version,
                        const char *text, size_t text_len) {
  if (!uri)
    return SOURCE_ID_NONE;

  char *path = lsp_uri_to_path(uri);
  if (!path)
    return SOURCE_ID_NONE;

  size_t path_len = strlen(path);

  // Route through the workspace coordinator. workspace_did_open
  // canonicalizes the path via realpath and returns the registered
  // SourceId — we use that directly since the original `path` may
  // not be canonical. File-as-namespace: each file owns its own
  // fresh NamespaceId; sibling files do NOT share scope (must @import).
  SourceId src = workspace_did_open(&lsp_db->db, path, path_len, text,
                                     text_len);
  free(path);

  if (source_id_valid(src)) {
    struct Draft *d = ensure_draft_slot(lsp_db, src);
    d->lsp_synced = true;
    d->version = version;
  }

  return src;
}

SourceId oredb_did_change(struct OreDb *lsp_db, const char *uri,
                          int32_t version, const char *text, size_t text_len) {
  if (!uri)
    return SOURCE_ID_NONE;

  char *path = lsp_uri_to_path(uri);
  if (!path)
    return SOURCE_ID_NONE;

  size_t path_len = strlen(path);
  SourceId src = db_lookup_source_by_path(&lsp_db->db, path, path_len);

  if (!source_id_valid(src)) {
    // Editor sent didChange for a file we've never seen. Spec says
    // didOpen always precedes didChange — log and drop.
    fprintf(stderr, "lsp: dropped didChange for unknown file %s\n", uri);
    free(path);
    return SOURCE_ID_NONE;
  }

  // Stale-packet check: out-of-order didChange (older version than
  // what we already applied). LSP doesn't guarantee strict ordering
  // across reconnects.
  if (src.idx < lsp_db->drafts.count) {
    struct Draft *prev = (struct Draft *)vec_get(&lsp_db->drafts, src.idx);
    if (prev->lsp_synced && version < prev->version) {
      fprintf(stderr,
              "lsp: dropping stale didChange (version %d < %d) for %s\n",
              version, prev->version, uri);
      free(path);
      return SOURCE_ID_NONE;
    }
  }

  // Route through workspace so any future per-edit bookkeeping
  // (file watcher, import-graph refresh, etc.) gathers in one place.
  workspace_did_change(&lsp_db->db, path, path_len, text, text_len);
  free(path);

  struct Draft *d = ensure_draft_slot(lsp_db, src);
  d->lsp_synced = true;
  d->version = version;

  return src;
}

bool oredb_did_close(struct OreDb *lsp_db, const char *uri) {
  if (!uri)
    return false;
  char *path = lsp_uri_to_path(uri);
  if (!path)
    return false;
  SourceId src = db_lookup_source_by_path(&lsp_db->db, path, strlen(path));
  free(path);

  if (!source_id_valid(src) || src.idx >= lsp_db->drafts.count)
    return false;

  struct Draft *d = (struct Draft *)vec_get(&lsp_db->drafts, src.idx);
  if (!d->lsp_synced)
    return false;

  d->lsp_synced = false;
  return true;
}

FileId oredb_typecheck(struct OreDb *lsp_db, SourceId src) {
  if (!source_id_valid(src))
    return FILE_ID_NONE;

  // The file was created at oredb_did_open time via workspace_did_open,
  // so the lookup should always hit. If a consumer ever calls typecheck
  // on a SourceId that bypassed the workspace API, we'd fail here — but
  // that's an API misuse rather than a fallback to paper over.
  FileId fid = db_lookup_file_by_source(&lsp_db->db, src);
  if (!file_id_valid(fid))
    return FILE_ID_NONE;
  NamespaceId nsid = db_get_file_namespace(&lsp_db->db, fid);

  // One salsa request per typecheck — pins effective_revision to
  // current_rev, which is what just got bumped by db_set_source_text
  // (or matches an unchanged source on idempotent calls). Without this
  // wrapper, sema's queries would still run but the effective_rev
  // would float to current_rev directly; pinning gives consistent
  // reads even if another thread were to bump current_rev nsid-pass.
  db_request_begin(&lsp_db->db, db_current_revision(&lsp_db->db));
  sema_check_module(&lsp_db->db, nsid);
  db_request_end(&lsp_db->db);
  return fid;
}

// === Hover ==================================================================

// Enclosing top-level DefId for `node`. Delegates to db_get_def_for_node
// which walks the parent chain via the node_to_def reverse index
// stamped by module_exports. O(parent_depth) — handful of links.
static DefId enclosing_fn_for_node(struct db *s, FileId fid, AstNodeId node) {
  return db_get_def_for_node(s, fid, node);
}

// Resolve a name in path-position. Body scope first (when we know the
// enclosing fn), then module scope. Returns IP_NONE on unresolved.
static IpIndex resolve_path_for_hover(struct db *s, NamespaceId nsid,
                                      DefId enclosing_fn, AstNodeId use_node,
                                      StrId name) {
  if (name.idx == 0)
    return IP_NONE;
  if (def_id_valid(enclosing_fn)) {
    IpIndex local = sema_body_scope_lookup(s, enclosing_fn, use_node, name);
    if (local.v != IP_NONE.v)
      return local;
  }
  ScopeId internal = db_get_namespace_internal_scope(s, nsid);
  if (internal.idx == SCOPE_ID_NONE.idx)
    return IP_NONE;
  DefId target = db_query_resolve_ref(s, internal, name);
  if (!def_id_valid(target))
    return IP_NONE;
  return db_query_type_of_def(s, target);
}

size_t oredb_hover(struct OreDb *lsp_db, SourceId src, uint32_t line0,
                   uint32_t char0, char *buf, size_t buflen) {
  if (!buf || buflen == 0)
    return 0;
  buf[0] = '\0';

  if (!source_id_valid(src))
    return 0;
  FileId fid = db_lookup_file_by_source(&lsp_db->db, src);
  if (!file_id_valid(fid))
    return 0;

  uint32_t off = db_byte_offset_at(&lsp_db->db, fid, line0, char0);
  if (off == UINT32_MAX)
    return 0;
  AstNodeId node = db_node_at_offset(&lsp_db->db, fid, off);
  if (node.idx == AST_NODE_ID_NONE.idx)
    return 0;

  NamespaceId nsid = db_get_file_namespace(&lsp_db->db, fid);
  if (!namespace_id_valid(nsid))
    return 0;

  ASTStore *ast = db_get_file_ast(&lsp_db->db, fid);
  if (!ast)
    return 0;

  AstNodeKind k = ((AstNodeKind *)ast->kinds.data)[node.idx];
  AstNodeData d = ((AstNodeData *)ast->data.data)[node.idx];
  DefId enclosing_fn = enclosing_fn_for_node(&lsp_db->db, fid, node);

  IpIndex type = IP_NONE;
  const char *name_str = NULL;

  switch (k) {
  case AST_EXPR_PATH: {
    StrId name = d.string_id;
    type = resolve_path_for_hover(&lsp_db->db, nsid, enclosing_fn, node, name);
    name_str = pool_get(&lsp_db->db.strings, name);
    break;
  }
  // Top-level decl names hover as the decl's type. The "cursor on the
  // name token" case lands on the AST_DECL_* node because the name's
  // span is part of the decl's span (innermost-containing wins).
  case AST_DECL_CONST:
  case AST_DECL_VAR: {
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    StrId name = {.idx = ex[0]};
    name_str = pool_get(&lsp_db->db.strings, name);
    // If this decl is a top-level def, look up via def_identity +
    // type_of_def. If it's a body-level let-bind, the body_scopes
    // lookup uses the name in the enclosing fn's chain.
    if (def_id_valid(enclosing_fn)) {
      type = sema_body_scope_lookup(&lsp_db->db, enclosing_fn, node, name);
    }
    if (type.v == IP_NONE.v) {
      type = resolve_path_for_hover(&lsp_db->db, nsid, DEF_ID_NONE, node, name);
    }
    break;
  }
  case AST_DECL_PARAM: {
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    StrId name = {.idx = ex[0]};
    name_str = pool_get(&lsp_db->db.strings, name);
    if (def_id_valid(enclosing_fn)) {
      type = sema_body_scope_lookup(&lsp_db->db, enclosing_fn, node, name);
    }
    break;
  }
  default:
    // Synth-type the expression. enclosing_fn may be DEF_ID_NONE for
    // expressions at module level (e.g. const RHS at top-level); the
    // typer handles that.
    type = sema_type_of_expr(&lsp_db->db, ast, node, nsid, enclosing_fn,
                             fid);
    break;
  }

  if (type.v == IP_NONE.v && (!name_str || !name_str[0]))
    return 0;

  char tbuf[256];
  db_format_type(&lsp_db->db, type, tbuf, sizeof tbuf);

  int n;
  if (name_str && name_str[0])
    n = snprintf(buf, buflen, "%s: %s", name_str, tbuf);
  else
    n = snprintf(buf, buflen, "%s", tbuf);
  return n < 0 ? 0 : (size_t)n;
}
