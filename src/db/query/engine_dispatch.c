// Engine dispatch table — maps QueryKind to its layer-side recompute thunk.
//
// Compile-time exhaustiveness: the X-macro ORE_QUERY_KINDS iterates every
// kind. The dispatch table generator produces one entry per kind. A new
// QueryKind without a corresponding `recompute_<KIND>` thunk is a link
// error, not a silent stale-pass risk.
//
// Each thunk has signature `void(db_query_ctx *, uint64_t)`: it decodes
// the kind-specific key encoding back into typed arguments, then calls
// the layer-side wrapper (e.g., db_query_file_ast). The wrapper handles
// cache-vs-recompute internally via DB_QUERY_GUARD.
//
// Key encoding contract (engine.c must match):
//   FILE_AST              : FileId.idx
//   DECL_AST              : (file_local << 32) | u32(ptr_hash)
//   FILE_IMPORTS          : FileId.idx
//   TOP_LEVEL_ENTRY       : (nsid.idx << 32) | name.idx
//   NAMESPACE_SCOPES      : nsid.idx
//   DEF_IDENTITY          : (nsid.idx << 32) | u32(ptr_hash)
//   NODE_TO_DEF           : FileId.idx
//   RESOLVE_REF           : (scope.idx << 32) | name.idx
//   TYPE_OF_DECL          : DefId.idx
//   FN_SIGNATURE          : DefId.idx
//   INFER_BODY            : DefId.idx
//   BODY_SCOPES           : DefId.idx
//   NAMESPACE_TYPE        : nsid.idx
//
// Layer wrappers are declared inline (forward) in this file. Their
// implementations live in stubs.c (Phase 1) or the layer files
// parse.c / scope.c / type.c (Phases 2-5).

#define ORE_ENGINE_PRIVATE
#include "engine.h"
#include "engine_internal.h"
#include "../db.h"
#include "../../support/data_structure/hashmap.h"
#include "../ids/ids.h"
#include "../intern_pool/intern_pool.h"
#include "../../syntax/syntax.h"

#include <stdint.h>

// ----------------------------------------------------------------------------
// Layer wrapper forward declarations
// ----------------------------------------------------------------------------

// Parse layer
extern struct GreenNode *db_query_file_ast(db_query_ctx *ctx, FileId fid);
extern SyntaxNodePtr     db_query_decl_ast(db_query_ctx *ctx, FileId fid,
                                           SyntaxNodePtr ptr);
extern FileArray         db_query_file_imports(db_query_ctx *ctx, FileId fid);

// Scope / name layer
extern TopLevelEntry   db_query_top_level_entry(db_query_ctx *ctx,
                                                NamespaceId nsid, StrId name);
extern NamespaceScopes db_query_namespace_scopes(db_query_ctx *ctx,
                                                 NamespaceId nsid);
extern DefId           db_query_def_identity(db_query_ctx *ctx, NamespaceId nsid,
                                             SyntaxNodePtr ptr);
extern DefId           db_query_resolve_ref(db_query_ctx *ctx, ScopeId scope,
                                            StrId name);

// Type layer
extern IpIndex            db_query_type_of_def(db_query_ctx *ctx, DefId def);
extern const FnSignature *db_query_fn_signature(db_query_ctx *ctx, DefId def);
extern NodeTypesRange     db_query_infer_body(db_query_ctx *ctx, DefId def);
extern const FnBody      *db_query_body_scopes(db_query_ctx *ctx, DefId def);
extern IpIndex            db_query_namespace_type(db_query_ctx *ctx, NamespaceId nsid);

// ----------------------------------------------------------------------------
// Recompute thunks — kind-specific key decoding
//
// Naming convention: `recompute_<KIND>` for each ORE_QUERY_KINDS entry.
// Missing thunk = link error.
// ----------------------------------------------------------------------------

// Parse layer
static void recompute_FILE_AST(db_query_ctx *ctx, uint64_t key) {
    (void)db_query_file_ast(ctx, (FileId){.idx = (uint32_t)key});
}

static void recompute_DECL_AST(db_query_ctx *ctx, uint64_t key) {
    void *rowp = hashmap_get(&((struct db *)ctx)->decl_ast_cache, key);
    if (!rowp) return; // never seen this key — nothing to recompute
    uint32_t row = (uint32_t)(uintptr_t)rowp;
    SyntaxNodePtr ptr =
        *(SyntaxNodePtr *)paged_get(&((struct db *)ctx)->decl_ast.keys, row);
    FileId fid = {.idx = (uint32_t)(key >> 32)};
    (void)db_query_decl_ast(ctx, fid, ptr);
}

static void recompute_FILE_IMPORTS(db_query_ctx *ctx, uint64_t key) {
    (void)db_query_file_imports(ctx, (FileId){.idx = (uint32_t)key});
}

// Scope / name layer
static void recompute_TOP_LEVEL_ENTRY(db_query_ctx *ctx, uint64_t key) {
    NamespaceId nsid = {.idx = (uint32_t)(key >> 32)};
    StrId       name = {.idx = (uint32_t)key};
    (void)db_query_top_level_entry(ctx, nsid, name);
}

static void recompute_NAMESPACE_SCOPES(db_query_ctx *ctx, uint64_t key) {
    (void)db_query_namespace_scopes(ctx, (NamespaceId){.idx = (uint32_t)key});
}

static void recompute_DEF_IDENTITY(db_query_ctx *ctx, uint64_t key) {
    void *rowp = hashmap_get(&((struct db *)ctx)->def_by_identity, key);
    if (!rowp) return;
    uint32_t row = (uint32_t)(uintptr_t)rowp;
    SyntaxNodePtr ptr =
        *(SyntaxNodePtr *)paged_get(&((struct db *)ctx)->def_identity.keys, row);
    NamespaceId nsid = {.idx = (uint32_t)(key >> 32)};
    (void)db_query_def_identity(ctx, nsid, ptr);
}

static void recompute_RESOLVE_REF(db_query_ctx *ctx, uint64_t key) {
    ScopeId scope = {.idx = (uint32_t)(key >> 32)};
    StrId   name  = {.idx = (uint32_t)key};
    (void)db_query_resolve_ref(ctx, scope, name);
}

// Type layer
static void recompute_TYPE_OF_DECL(db_query_ctx *ctx, uint64_t key) {
    (void)db_query_type_of_def(ctx, (DefId){.idx = (uint32_t)key});
}

static void recompute_FN_SIGNATURE(db_query_ctx *ctx, uint64_t key) {
    (void)db_query_fn_signature(ctx, (DefId){.idx = (uint32_t)key});
}

static void recompute_INFER_BODY(db_query_ctx *ctx, uint64_t key) {
    (void)db_query_infer_body(ctx, (DefId){.idx = (uint32_t)key});
}

static void recompute_BODY_SCOPES(db_query_ctx *ctx, uint64_t key) {
    (void)db_query_body_scopes(ctx, (DefId){.idx = (uint32_t)key});
}

static void recompute_NAMESPACE_TYPE(db_query_ctx *ctx, uint64_t key) {
    (void)db_query_namespace_type(ctx, (NamespaceId){.idx = (uint32_t)key});
}

// ----------------------------------------------------------------------------
// Dispatch table
//
// X-macro generates one entry per QueryKind. If a kind is added to
// ORE_QUERY_KINDS without a `recompute_<KIND>` thunk above, this expansion
// fails to compile (undeclared identifier).
// ----------------------------------------------------------------------------

const RecomputeFn db_engine_recompute_dispatch[QUERY_KIND_COUNT] = {
#define X(name) [QUERY_##name] = recompute_##name,
    ORE_QUERY_KINDS(X)
#undef X
};

// ----------------------------------------------------------------------------
// Name table — for telemetry, panics, traces
// ----------------------------------------------------------------------------

const char *db_query_kind_name(QueryKind kind) {
    static const char *names[QUERY_KIND_COUNT] = {
#define X(name) [QUERY_##name] = #name,
        ORE_QUERY_KINDS(X)
#undef X
    };
    if ((unsigned)kind >= (unsigned)QUERY_KIND_COUNT) return "INVALID";
    return names[kind];
}
