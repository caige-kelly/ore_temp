// TEMPORARY — Phase 1 only.
//
// Stub implementations of the layer-side query wrappers. The engine
// links cleanly against these no-op stubs so engine primitives can be
// smoke-tested in isolation. Stubs get DELETED file-by-file as Phases
// 2-5 replace each layer with real implementations.
//
// Each stub follows the same shape:
//   1. Allocate the slot if HashMap-routed (alloc is idempotent).
//   2. DB_QUERY_GUARD — if already cached/cycle/error, return sentinel.
//   3. If we get here, the slot was EMPTY → COMPUTE; succeed immediately
//      with FINGERPRINT_NONE so the slot is DONE for next time.
//   4. Return a "no value" sentinel (FINGERPRINT_NONE / IP_NONE /
//      DEF_ID_NONE / false / NULL).
//
// The behavior is: every layer query "succeeds" with no actual work.
// The Phase 1 engine_smoke_test exercises only engine primitives via
// db_query_begin/succeed/fail/stamp_direct directly — it never calls
// these wrappers. Consumers outside the test harness (sema, LSP,
// compiler driver) won't get meaningful results until Phases 2-5 land
// real wrappers. That's expected during the rewrite period.

#include "engine.h"
#include "engine_internal.h"

#include "../ids/ids.h"
#include "../intern_pool/intern_pool.h"
#include "../../syntax/syntax.h"

#include <stdbool.h>
#include <stdint.h>

// ----------------------------------------------------------------------------
// Parse layer stubs
// ----------------------------------------------------------------------------

Fingerprint db_query_file_ast(db_query_ctx *ctx, FileId fid) {
    DB_QUERY_GUARD(ctx, QUERY_FILE_AST, (uint64_t)fid.idx,
                   db_slot_fingerprint(ctx, QUERY_FILE_AST, (uint64_t)fid.idx),
                   FINGERPRINT_NONE, FINGERPRINT_NONE);
    db_query_succeed(ctx, QUERY_FILE_AST, (uint64_t)fid.idx, FINGERPRINT_NONE);
    return FINGERPRINT_NONE;
}

SyntaxNodePtr db_query_decl_ast(db_query_ctx *ctx, FileId fid,
                                 SyntaxNodePtr ptr) {
    SyntaxNodePtr none = {0};
    uint64_t key = ((uint64_t)fid.idx << 32) |
                   (uint32_t)syntax_node_ptr_hash(ptr);
    db_query_slot_alloc(ctx, QUERY_DECL_AST, key);
    DB_QUERY_GUARD(ctx, QUERY_DECL_AST, key, none, none, none);
    db_query_succeed(ctx, QUERY_DECL_AST, key, FINGERPRINT_NONE);
    return none;
}

void *db_query_file_imports(db_query_ctx *ctx, FileId fid) {
    DB_QUERY_GUARD(ctx, QUERY_FILE_IMPORTS, (uint64_t)fid.idx,
                   NULL, NULL, NULL);
    db_query_succeed(ctx, QUERY_FILE_IMPORTS, (uint64_t)fid.idx,
                     FINGERPRINT_NONE);
    return NULL;
}

// ----------------------------------------------------------------------------
// Scope / name layer stubs
// ----------------------------------------------------------------------------

Fingerprint db_query_top_level_entry(db_query_ctx *ctx, NamespaceId nsid,
                                      StrId name) {
    uint64_t key = ((uint64_t)nsid.idx << 32) | (uint32_t)name.idx;
    db_query_slot_alloc(ctx, QUERY_TOP_LEVEL_ENTRY, key);
    DB_QUERY_GUARD(ctx, QUERY_TOP_LEVEL_ENTRY, key,
                   db_slot_fingerprint(ctx, QUERY_TOP_LEVEL_ENTRY, key),
                   FINGERPRINT_NONE, FINGERPRINT_NONE);
    db_query_succeed(ctx, QUERY_TOP_LEVEL_ENTRY, key, FINGERPRINT_NONE);
    return FINGERPRINT_NONE;
}

Fingerprint db_query_namespace_scopes(db_query_ctx *ctx, NamespaceId nsid) {
    DB_QUERY_GUARD(ctx, QUERY_NAMESPACE_SCOPES, (uint64_t)nsid.idx,
                   db_slot_fingerprint(ctx, QUERY_NAMESPACE_SCOPES,
                                       (uint64_t)nsid.idx),
                   FINGERPRINT_NONE, FINGERPRINT_NONE);
    db_query_succeed(ctx, QUERY_NAMESPACE_SCOPES, (uint64_t)nsid.idx,
                     FINGERPRINT_NONE);
    return FINGERPRINT_NONE;
}

DefId db_query_def_identity(db_query_ctx *ctx, NamespaceId nsid,
                             SyntaxNodePtr ptr) {
    uint64_t key = ((uint64_t)nsid.idx << 32) |
                   (uint32_t)syntax_node_ptr_hash(ptr);
    db_query_slot_alloc(ctx, QUERY_DEF_IDENTITY, key);
    DB_QUERY_GUARD(ctx, QUERY_DEF_IDENTITY, key,
                   DEF_ID_NONE, DEF_ID_NONE, DEF_ID_NONE);
    db_query_succeed(ctx, QUERY_DEF_IDENTITY, key, FINGERPRINT_NONE);
    return DEF_ID_NONE;
}

bool db_query_node_to_def(db_query_ctx *ctx, FileId fid) {
    DB_QUERY_GUARD(ctx, QUERY_NODE_TO_DEF, (uint64_t)fid.idx,
                   true, false, false);
    db_query_succeed(ctx, QUERY_NODE_TO_DEF, (uint64_t)fid.idx,
                     FINGERPRINT_NONE);
    return true;
}

DefId db_query_resolve_ref(db_query_ctx *ctx, ScopeId scope, StrId name) {
    uint64_t key = ((uint64_t)scope.idx << 32) | (uint32_t)name.idx;
    db_query_slot_alloc(ctx, QUERY_RESOLVE_REF, key);
    DB_QUERY_GUARD(ctx, QUERY_RESOLVE_REF, key,
                   DEF_ID_NONE, DEF_ID_NONE, DEF_ID_NONE);
    db_query_succeed(ctx, QUERY_RESOLVE_REF, key, FINGERPRINT_NONE);
    return DEF_ID_NONE;
}

// ----------------------------------------------------------------------------
// Type layer stubs
// ----------------------------------------------------------------------------

IpIndex db_query_type_of_def(db_query_ctx *ctx, DefId def) {
    DB_QUERY_GUARD(ctx, QUERY_TYPE_OF_DECL, (uint64_t)def.idx,
                   IP_NONE, IP_NONE, IP_NONE);
    db_query_succeed(ctx, QUERY_TYPE_OF_DECL, (uint64_t)def.idx,
                     FINGERPRINT_NONE);
    return IP_NONE;
}

IpIndex db_query_fn_signature(db_query_ctx *ctx, DefId def) {
    DB_QUERY_GUARD(ctx, QUERY_FN_SIGNATURE, (uint64_t)def.idx,
                   IP_NONE, IP_NONE, IP_NONE);
    db_query_succeed(ctx, QUERY_FN_SIGNATURE, (uint64_t)def.idx,
                     FINGERPRINT_NONE);
    return IP_NONE;
}

IpIndex db_query_infer_body(db_query_ctx *ctx, DefId def) {
    DB_QUERY_GUARD(ctx, QUERY_INFER_BODY, (uint64_t)def.idx,
                   IP_NONE, IP_NONE, IP_NONE);
    db_query_succeed(ctx, QUERY_INFER_BODY, (uint64_t)def.idx,
                     FINGERPRINT_NONE);
    return IP_NONE;
}

bool db_query_body_scopes(db_query_ctx *ctx, DefId def) {
    DB_QUERY_GUARD(ctx, QUERY_BODY_SCOPES, (uint64_t)def.idx,
                   true, false, false);
    db_query_succeed(ctx, QUERY_BODY_SCOPES, (uint64_t)def.idx,
                     FINGERPRINT_NONE);
    return true;
}

IpIndex db_query_namespace_type(db_query_ctx *ctx, NamespaceId nsid) {
    DB_QUERY_GUARD(ctx, QUERY_NAMESPACE_TYPE, (uint64_t)nsid.idx,
                   IP_NONE, IP_NONE, IP_NONE);
    db_query_succeed(ctx, QUERY_NAMESPACE_TYPE, (uint64_t)nsid.idx,
                     FINGERPRINT_NONE);
    return IP_NONE;
}
