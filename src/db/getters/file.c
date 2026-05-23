// File readers — accessors for the file-table SoA columns.

#include "../db.h"
#include "../query/node_to_def.h" // db_query_node_to_def — for db_get_def_for_node
#include "../workspace/ast_id_map.h" // struct AstIdMap (opaque pointer type)

SourceId db_get_file_source(struct db *s, FileId fid) {
  if (!file_id_valid(fid))
    return SOURCE_ID_NONE;
  uint32_t local = file_id_local(fid);
  if (local >= s->files.source_id.count)
    return SOURCE_ID_NONE;
  return *(SourceId *)vec_get(&s->files.source_id, local);
}

NamespaceId db_get_file_namespace(struct db *s, FileId fid) {
  if (!file_id_valid(fid))
    return NAMESPACE_ID_NONE;
  uint32_t local = file_id_local(fid);
  if (local >= s->files.module_id.count)
    return NAMESPACE_ID_NONE;
  return *(NamespaceId *)vec_get(&s->files.module_id, local);
}

// Source → File. O(1) via the file_by_source HashMap (populated by
// db_create_file). 1:1 source-to-file today; N:1 future would
// promote the value to a Vec<FileId> offset.
FileId db_lookup_file_by_source(struct db *s, SourceId src) {
  if (!source_id_valid(src))
    return FILE_ID_NONE;
  void *v = hashmap_get(&s->file_by_source, (uint64_t)src.idx);
  uint32_t idx = (uint32_t)(uintptr_t)v;
  if (idx == 0)
    return FILE_ID_NONE;
  return file_id_make_physical(idx);
}

// Phase 8 eviction gate — return true if the file's backing source
// has been evicted (FS watcher reported the file as deleted). After
// eviction the per-file arena and source text are freed; readers
// must bail before any pointer deref.
static inline bool file_is_evicted(struct db *s, uint32_t local) {
  if (local >= s->files.source_id.count)
    return true;
  SourceId sid = *(SourceId *)vec_get(&s->files.source_id, local);
  return db_get_source_evicted(s, sid);
}

// Parsed AST for a file. NULL if the parse query hasn't run for this
// file (state EMPTY) OR the file has been evicted. ASTStore is the
// parser's output structure; opaque to db, fully defined in
// src/parser/ast.h. The void* in the column avoids a layering include
// from db.h into the parser.
struct ASTStore;
struct ASTStore *db_get_file_ast(struct db *s, FileId fid) {
  if (!file_id_valid(fid))
    return NULL;
  uint32_t local = file_id_local(fid);
  if (local >= s->files.asts.count)
    return NULL;
  if (file_is_evicted(s, local))
    return NULL;
  return *(struct ASTStore **)vec_get(&s->files.asts, local);
}

// AstId → AstNodeId map for a file. Populated by the parse query so
// downstream queries (def_identity, type_of_def, infer_body) can
// resolve a parser-side stable AstId to the current parse's AstNodeId.
struct AstIdMap *db_get_file_ast_id_map(struct db *s, FileId fid) {
  if (!file_id_valid(fid))
    return NULL;
  uint32_t local = file_id_local(fid);
  if (local >= s->files.ast_id_maps.count)
    return NULL;
  if (file_is_evicted(s, local))
    return NULL;
  return *(struct AstIdMap **)vec_get(&s->files.ast_id_maps, local);
}

// Span for an AST node within a file. Returns TINYSPAN_NONE if the
// file is invalid, evicted, the parse hasn't run, or the node id is
// out of range. Used by diag emitters to attach a source location
// to error messages.
TinySpan db_get_node_span(struct db *s, FileId fid, AstNodeId node) {
  if (!file_id_valid(fid))
    return TINYSPAN_NONE;
  uint32_t local = file_id_local(fid);
  if (local >= s->files.node_data.count || local >= s->files.node_counts.count)
    return TINYSPAN_NONE;
  if (file_is_evicted(s, local))
    return TINYSPAN_NONE;
  FileNodeData *nd = (FileNodeData *)vec_get(&s->files.node_data, local);
  if (!nd || !nd->spans)
    return TINYSPAN_NONE;
  uint32_t count = *(uint32_t *)vec_get(&s->files.node_counts, local);
  if (node.idx >= count)
    return TINYSPAN_NONE;
  return nd->spans[node.idx];
}

// Enclosing top-level DefId for an AST node. Walks the parent chain
// from `node` upward until it finds a node tagged with a DefId (the
// decl root). O(parent_depth) — typically a handful of links for
// body-level nodes. Returns DEF_ID_NONE if the file has no parse, no
// parents, is evicted, or the chain reaches the root without a hit.
DefId db_get_def_for_node(struct db *s, FileId fid, AstNodeId node) {
  if (!file_id_valid(fid) || node.idx == AST_NODE_ID_NONE.idx)
    return DEF_ID_NONE;
  uint32_t local = file_id_local(fid);
  // CRITICAL: gate BEFORE db_query_node_to_def. The query body
  // dereferences the per-file arena; running it on an evicted file
  // would UAF.
  if (local < s->files.source_id.count && file_is_evicted(s, local))
    return DEF_ID_NONE;
  // Ensure the file's node→DefId reverse index is current — it is the
  // QUERY_NODE_TO_DECL query's output (stamped into FileNodeData.defs).
  db_query_node_to_def(s, fid);
  if (local >= s->files.node_data.count || local >= s->files.node_counts.count)
    return DEF_ID_NONE;
  FileNodeData *nd = (FileNodeData *)vec_get(&s->files.node_data, local);
  if (!nd || !nd->defs || !nd->parents)
    return DEF_ID_NONE;
  uint32_t count = *(uint32_t *)vec_get(&s->files.node_counts, local);

  uint32_t cur = node.idx;
  // Bound the walk by node_count to avoid pathological cycles in a
  // malformed parents table.
  for (uint32_t i = 0; i < count && cur != 0 && cur < count; i++) {
    if (def_id_valid(nd->defs[cur]))
      return nd->defs[cur];
    cur = nd->parents[cur].idx;
  }
  return DEF_ID_NONE;
}
