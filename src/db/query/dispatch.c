// Engine ↔ wrappers bridge.
//
// The engine (invalidate.c / query.c) operates on generic (QueryKind,
// uint64_t key) pairs. To pull a recorded dep during verify, it needs
// to invoke the dep's typed wrapper — which takes per-kind args (FileId,
// DefId, packed (NamespaceId,AstId), etc.). This file is the only place
// that knows about both:
//   - the engine's QueryKind enum
//   - every active wrapper's typed signature + key encoding
//
// Each thunk decodes the u64 key into its wrapper's typed args and
// calls the wrapper. The wrapper's own DB_QUERY_GUARD handles
// cache-vs-recompute (db_query_begin → db_verify → either return CACHED
// or fall through to body). The engine never invokes recompute
// directly; it just pulls.
//
// Adding a new active QueryKind:
//   1. Add a static recompute_<name> thunk that decodes the key.
//   2. Add an s->recompute_dispatch[QUERY_<NAME>] = recompute_<name>;
//      line in db_register_query_dispatch.
//
// Scaffold-only QueryKinds (no active wrapper) don't need an entry —
// nothing creates a slot for them, so they never appear as a dep.

#include "../db.h"

#include "ast.h"
#include "body_scopes.h"
#include "decl_ast.h"
#include "def_identity.h"
#include "file_imports.h"
#include "fn_signature.h"
#include "index.h"
#include "infer_body.h"
#include "module_exports.h"
#include "namespace_type.h"
#include "node_to_def.h"
#include "resolve_ref.h"
#include "type_of_def.h"

// --- Recompute thunks. Each owns its key→args decoding. ---

static void recompute_file_ast(struct db *s, uint64_t key) {
  db_query_file_ast(s, (FileId){.idx = (uint32_t)key});
}

static void recompute_decl_ast(struct db *s, uint64_t key) {
  FileId fid = {.idx = (uint32_t)(key >> 32)};
  AstId aid = {.idx = (uint32_t)key};
  db_query_decl_ast(s, fid, aid);
}

static void recompute_def_identity(struct db *s, uint64_t key) {
  NamespaceId nsid = {.idx = (uint32_t)(key >> 32)};
  AstId aid = {.idx = (uint32_t)key};
  db_query_def_identity(s, nsid, aid);
}

static void recompute_top_level_index(struct db *s, uint64_t key) {
  db_query_top_level_index(s, (NamespaceId){.idx = (uint32_t)key});
}

static void recompute_module_exports(struct db *s, uint64_t key) {
  db_query_namespace_scopes(s, (NamespaceId){.idx = (uint32_t)key});
}

static void recompute_namespace_type(struct db *s, uint64_t key) {
  db_query_namespace_type(s, (NamespaceId){.idx = (uint32_t)key});
}

static void recompute_node_to_def(struct db *s, uint64_t key) {
  db_query_node_to_def(s, (FileId){.idx = (uint32_t)key});
}

static void recompute_resolve_ref(struct db *s, uint64_t key) {
  ScopeId scope = {.idx = (uint32_t)(key >> 32)};
  StrId name = {.idx = (uint32_t)key};
  db_query_resolve_ref(s, scope, name);
}

static void recompute_type_of_def(struct db *s, uint64_t key) {
  db_query_type_of_def(s, (DefId){.idx = (uint32_t)key});
}

static void recompute_fn_signature(struct db *s, uint64_t key) {
  db_query_fn_signature(s, (DefId){.idx = (uint32_t)key});
}

static void recompute_infer_body(struct db *s, uint64_t key) {
  db_query_infer_body(s, (DefId){.idx = (uint32_t)key});
}

static void recompute_body_scopes(struct db *s, uint64_t key) {
  db_query_body_scopes(s, (DefId){.idx = (uint32_t)key});
}

static void recompute_file_imports(struct db *s, uint64_t key) {
  db_query_file_imports(s, (FileId){.idx = (uint32_t)key});
}

// Called from db_init once the SoA columns are wired. After this,
// db_verify can resolve any dep via s->recompute_dispatch[kind](s, key).
void db_register_query_dispatch(struct db *s) {
  s->recompute_dispatch[QUERY_FILE_AST] = recompute_file_ast;
  s->recompute_dispatch[QUERY_DECL_AST] = recompute_decl_ast;
  s->recompute_dispatch[QUERY_DEF_IDENTITY] = recompute_def_identity;
  s->recompute_dispatch[QUERY_TOP_LEVEL_INDEX] = recompute_top_level_index;
  s->recompute_dispatch[QUERY_NAMESPACE_SCOPES] = recompute_module_exports;
  s->recompute_dispatch[QUERY_NODE_TO_DECL] = recompute_node_to_def;
  s->recompute_dispatch[QUERY_RESOLVE_REF] = recompute_resolve_ref;
  s->recompute_dispatch[QUERY_TYPE_OF_DECL] = recompute_type_of_def;
  s->recompute_dispatch[QUERY_FN_SIGNATURE] = recompute_fn_signature;
  s->recompute_dispatch[QUERY_INFER_BODY] = recompute_infer_body;
  s->recompute_dispatch[QUERY_BODY_SCOPES] = recompute_body_scopes;
  s->recompute_dispatch[QUERY_FILE_IMPORTS] = recompute_file_imports;
  s->recompute_dispatch[QUERY_NAMESPACE_TYPE] = recompute_namespace_type;
}
