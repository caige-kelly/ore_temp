// TEMPORARY — Phase 1 only.
//
// Stub implementations of the layer-side query wrappers. The engine
// links cleanly against these no-op stubs so engine primitives can be
// smoke-tested in isolation. Stubs get DELETED file-by-file as Phases
// 2-5 replace each layer with real implementations.
//
// These stubs implement the PURE-QUERY CONTRACT — see engine.h's
// "Pure Query Model" section and engine_internal.h's "Result column
// convention" section. The pattern every stub follows:
//
//   1. (HashMap-routed kinds only) Allocate the slot — idempotent.
//   2. DB_QUERY_GUARD with on_cached = read_result_X(...). On cache
//      hit, return the result column's current value WITHOUT recomputing.
//   3. Compute the result (here: a sentinel — no real work).
//   4. WRITE the result to the slot's result column.
//   5. db_query_succeed(ctx, kind, key, fingerprint).
//   6. Return the computed result.
//
// Column-write happens BEFORE succeed because succeed flips the slot
// to DONE; any verify-time observer must see column and slot in a
// coherent state.
//
// Future parse.c / scope.c / type.c authors: copy this pattern. The
// only thing that changes is the "compute" step — replace the sentinel
// with the actual layer-specific work. Do not change the order of
// steps 4 and 5, and do not write results anywhere outside the named
// result column.

#define ORE_ENGINE_PRIVATE
#include "engine.h"
#include "engine_internal.h"
#include "result_columns.h"   // pulls in db.h, ids.h, intern_pool.h, syntax.h

#include <stdbool.h>
#include <stdint.h>

// Result-column accessors (file_ast_read/write, etc.) live in
// result_columns.h — included above. See its header for the contract.


// ----------------------------------------------------------------------------
// Parse layer
//
// NOTE: db_query_file_ast, db_query_line_index, db_query_file_imports, and
// db_query_namespace_items / db_query_top_level_entry are all implemented
// for real in parse.c (Phase C). No parse-layer stubs remain.
// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
// Scope / name layer
//
// NOTE: db_query_top_level_entry / db_query_namespace_items are in parse.c;
// db_query_namespace_scopes / db_query_def_identity / db_query_resolve_ref
// are implemented for real in scope.c (Phase D1). No scope-layer stubs
// remain.
// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
// Type layer stubs
// ----------------------------------------------------------------------------

IpIndex db_query_type_of_def(db_query_ctx *ctx, DefId def) {
    struct db *s = (struct db *)ctx;
    DB_QUERY_GUARD(ctx, QUERY_TYPE_OF_DECL, (uint64_t)def.idx,
                   /* on_cached */ type_of_decl_read(s, def),
                   /* on_cycle  */ IP_NONE,
                   /* on_error  */ IP_NONE);
    IpIndex result = IP_NONE;
    type_of_decl_write(s, def, result);
    db_query_succeed(ctx, QUERY_TYPE_OF_DECL, (uint64_t)def.idx,
                     FINGERPRINT_NONE);
    return result;
}

const FnSignature *db_query_fn_signature(db_query_ctx *ctx, DefId def) {
    struct db *s = (struct db *)ctx;
    DB_QUERY_GUARD(ctx, QUERY_FN_SIGNATURE, (uint64_t)def.idx,
                   /* on_cached */ fn_signature_read(s, def),
                   /* on_cycle  */ NULL,
                   /* on_error  */ NULL);
    FnSignature result = {0};
    fn_signature_write(s, def, result);
    db_query_succeed(ctx, QUERY_FN_SIGNATURE, (uint64_t)def.idx,
                     FINGERPRINT_NONE);
    return fn_signature_read(s, def);
}

NodeTypesRange db_query_infer_body(db_query_ctx *ctx, DefId def) {
    struct db *s = (struct db *)ctx;
    NodeTypesRange empty = {0};
    DB_QUERY_GUARD(ctx, QUERY_INFER_BODY, (uint64_t)def.idx,
                   /* on_cached */ infer_body_read(s, def),
                   /* on_cycle  */ empty,
                   /* on_error  */ empty);
    NodeTypesRange result = empty;
    infer_body_write(s, def, result);
    db_query_succeed(ctx, QUERY_INFER_BODY, (uint64_t)def.idx,
                     FINGERPRINT_NONE);
    return result;
}

const FnBody *db_query_body_scopes(db_query_ctx *ctx, DefId def) {
    struct db *s = (struct db *)ctx;
    DB_QUERY_GUARD(ctx, QUERY_BODY_SCOPES, (uint64_t)def.idx,
                   /* on_cached */ body_scopes_read(s, def),
                   /* on_cycle  */ NULL,
                   /* on_error  */ NULL);
    FnBody result = {0};
    body_scopes_write(s, def, result);
    db_query_succeed(ctx, QUERY_BODY_SCOPES, (uint64_t)def.idx,
                     FINGERPRINT_NONE);
    return body_scopes_read(s, def);
}

IpIndex db_query_namespace_type(db_query_ctx *ctx, NamespaceId nsid) {
    struct db *s = (struct db *)ctx;
    DB_QUERY_GUARD(ctx, QUERY_NAMESPACE_TYPE, (uint64_t)nsid.idx,
                   /* on_cached */ namespace_type_read(s, nsid),
                   /* on_cycle  */ IP_NONE,
                   /* on_error  */ IP_NONE);
    IpIndex result = IP_NONE;
    namespace_type_write(s, nsid, result);
    db_query_succeed(ctx, QUERY_NAMESPACE_TYPE, (uint64_t)nsid.idx,
                     FINGERPRINT_NONE);
    return result;
}
