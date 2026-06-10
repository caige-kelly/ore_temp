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
//   FILE_IMPORTS          : FileId.idx
//   TOP_LEVEL_ENTRY       : (nsid.idx << 32) | name.idx
//   NAMESPACE_SCOPES      : nsid.idx
//   DEF_IDENTITY          : (nsid.idx << 32) | astid.idx  (fully reversible)
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
#include "../../syntax/syntax.h"
#include "../db.h"
#include "../ids/ids.h"
#include "../intern_pool/intern_pool.h"
#include "engine.h"
#include "engine_internal.h"

#include <stdint.h>

// ----------------------------------------------------------------------------
// Layer wrapper forward declarations
// ----------------------------------------------------------------------------

// Parse layer
extern struct GreenNode *db_query_file_ast(db_query_ctx *ctx, FileId fid);
extern FileArray db_query_line_index(db_query_ctx *ctx, FileId fid);
extern FileArray db_query_file_imports(db_query_ctx *ctx, FileId fid);

// Scope / name layer
extern FileArray db_query_namespace_items(db_query_ctx *ctx, NamespaceId nsid);
extern TopLevelEntry db_query_top_level_entry(db_query_ctx *ctx,
                                              NamespaceId nsid, StrId name);
extern NamespaceScopes db_query_namespace_scopes(db_query_ctx *ctx,
                                                 NamespaceId nsid);
extern DefId db_query_def_identity(db_query_ctx *ctx, NamespaceId nsid,
                                   AstId id);
extern DefId db_query_resolve_ref(db_query_ctx *ctx, ScopeId scope, StrId name);

// Type layer
extern IpIndex db_query_type_of_def(db_query_ctx *ctx, DefId def);
extern const FnSignature *db_query_fn_signature(db_query_ctx *ctx, DefId def);
extern NodeTypesRange db_query_infer_body(db_query_ctx *ctx, DefId def);
extern const FnBody *db_query_body_scopes(db_query_ctx *ctx, DefId def);
extern IpIndex db_query_namespace_type(db_query_ctx *ctx, NamespaceId nsid);
extern IpIndex db_query_infer_instance(db_query_ctx *ctx, IpIndex inst);

// ----------------------------------------------------------------------------
// Recompute thunks — kind-specific key decoding
//
// Naming convention: `recompute_<KIND>` for each ORE_QUERY_KINDS entry.
// Missing thunk = link error.
// ----------------------------------------------------------------------------

// Input layer — inputs are SET, never computed. The slot's fingerprint
// is maintained by db_input_set (the setter). The dep walk pulls this
// thunk before comparing fingerprints, so it must be a no-op that leaves
// the authoritative slot fingerprint untouched.
static void recompute_SOURCE_TEXT(db_query_ctx *ctx, uint64_t key) {
  (void)ctx;
  (void)key;
}

static void recompute_FILE_SET(db_query_ctx *ctx, uint64_t key) {
  (void)ctx;
  (void)key;
}

// Parse layer
static void recompute_FILE_AST(db_query_ctx *ctx, uint64_t key) {
  (void)db_query_file_ast(ctx, (FileId){.idx = (uint32_t)key});
}

static void recompute_LINE_INDEX(db_query_ctx *ctx, uint64_t key) {
  (void)db_query_line_index(ctx, (FileId){.idx = (uint32_t)key});
}

static void recompute_FILE_IMPORTS(db_query_ctx *ctx, uint64_t key) {
  (void)db_query_file_imports(ctx, (FileId){.idx = (uint32_t)key});
}

// Scope / name layer
static void recompute_NAMESPACE_ITEMS(db_query_ctx *ctx, uint64_t key) {
  (void)db_query_namespace_items(ctx, (NamespaceId){.idx = (uint32_t)key});
}

static void recompute_TOP_LEVEL_ENTRY(db_query_ctx *ctx, uint64_t key) {
  NamespaceId nsid = {.idx = (uint32_t)(key >> 32)};
  StrId name = {.idx = (uint32_t)key};
  (void)db_query_top_level_entry(ctx, nsid, name);
}

static void recompute_NAMESPACE_SCOPES(db_query_ctx *ctx, uint64_t key) {
  (void)db_query_namespace_scopes(ctx, (NamespaceId){.idx = (uint32_t)key});
}

static void recompute_DEF_IDENTITY(db_query_ctx *ctx, uint64_t key) {
  // Key = (nsid<<32 | astid) is fully reversible, so the args reconstruct
  // straight from the key — no keys column needed.
  NamespaceId nsid = {.idx = (uint32_t)(key >> 32)};
  AstId id = {.idx = (uint32_t)key};
  (void)db_query_def_identity(ctx, nsid, id);
}

static void recompute_RESOLVE_REF(db_query_ctx *ctx, uint64_t key) {
  ScopeId scope = {.idx = (uint32_t)(key >> 32)};
  StrId name = {.idx = (uint32_t)key};
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

// CHECK is INPUT-class (driver-stamped via db_input_set, never computed); the
// thunk is a required no-op like the other INPUT kinds — it is never invoked
// because nothing db_query_begin's QUERY_CHECK.
static void recompute_CHECK(db_query_ctx *ctx, uint64_t key) {
  (void)ctx;
  (void)key;
}

static void recompute_NAMESPACE_TYPE(db_query_ctx *ctx, uint64_t key) {
  (void)db_query_namespace_type(ctx, (NamespaceId){.idx = (uint32_t)key});
}

// Monomorphization — the routing key IS the interned IPK_INSTANCE IpIndex.v
// (top 32 bits zero), so reconstruct the IpIndex straight from the key.
static void recompute_INFER_INSTANCE(db_query_ctx *ctx, uint64_t key) {
  (void)db_query_infer_instance(ctx, (IpIndex){.v = (uint32_t)key});
}

// ----------------------------------------------------------------------------
// Dispatch table
//
// X-macro generates one entry per QueryKind. If a kind is added to
// ORE_QUERY_KINDS without a `recompute_<KIND>` thunk above, this expansion
// fails to compile (undeclared identifier).
// ----------------------------------------------------------------------------

const RecomputeFn db_engine_recompute_dispatch[QUERY_KIND_COUNT] = {
#define X(name, cls) [QUERY_##name] = recompute_##name,
    ORE_QUERY_KINDS(X)
#undef X
};

// ----------------------------------------------------------------------------
// Name table — for telemetry, panics, traces
// ----------------------------------------------------------------------------

const char *db_query_kind_name(QueryKind kind) {
  static const char *names[QUERY_KIND_COUNT] = {
#define X(name, cls) [QUERY_##name] = #name,
      ORE_QUERY_KINDS(X)
#undef X
  };
  if ((unsigned)kind >= (unsigned)QUERY_KIND_COUNT)
    return "INVALID";
  return names[kind];
}

// ----------------------------------------------------------------------------
// Input-vs-derived classification — single source of truth is the
// INPUT/DERIVED tag in ORE_QUERY_KINDS. INPUT kinds are set via
// db_input_set + have no-op recompute thunks; DERIVED kinds compute +
// db_query_succeed. The engine asserts this at both boundaries.
// ----------------------------------------------------------------------------

#define QUERY_KIND_CLASS_INPUT true
#define QUERY_KIND_CLASS_DERIVED false

bool db_query_kind_is_input(QueryKind kind) {
  static const bool is_input[QUERY_KIND_COUNT] = {
#define X(name, cls) [QUERY_##name] = QUERY_KIND_CLASS_##cls,
      ORE_QUERY_KINDS(X)
#undef X
  };
  if ((unsigned)kind >= (unsigned)QUERY_KIND_COUNT)
    return false;
  return is_input[kind];
}
