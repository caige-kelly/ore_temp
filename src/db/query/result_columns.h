#ifndef ORE_DB_QUERY_RESULT_COLUMNS_H
#define ORE_DB_QUERY_RESULT_COLUMNS_H

// ============================================================================
// Result column accessors — the pure-query contract in code form.
//
// One `read_X` / `write_X` static-inline pair per query kind. These are
// the SINGLE site that knows where each query's memoized result lives.
// Layer wrappers (parse.c, scope.c, type.c, and today's stubs.c) call
// these to honor the pure-query contract:
//
//   - On cache hit (DB_QUERY_GUARD's on_cached arm): read_X returns the
//     slot's memoized value. No recomputation; the engine's slot.state
//     proved the column is current at this revision.
//
//   - On compute path: the wrapper computes, then write_X stores the
//     result into the slot's result column BEFORE db_query_succeed.
//     Column-write before succeed is load-bearing — succeed flips the
//     slot to DONE, and any verify-time observer must see a coherent
//     (slot, result-column) pair.
//
// Layer files include this header alongside engine_internal.h. The
// engine itself never reaches for these accessors — kind-specific result
// types are a layer concern, not an engine concern.
//
// Privacy: like engine_internal.h, this header is privileged. Includers
// must `#define ORE_ENGINE_PRIVATE` before include.
// ============================================================================

#ifndef ORE_ENGINE_PRIVATE
#error "result_columns.h is private; define ORE_ENGINE_PRIVATE before including (allowed: src/db/query/*.c)"
#endif

#include "../db.h"
#include "../ids/ids.h"
#include "../intern_pool/intern_pool.h"
#include "../../support/data_structure/hashmap.h"
#include "../../support/data_structure/vec.h"
#include "../../syntax/syntax.h"

#include <stdbool.h>
#include <stdint.h>


// --- FILE_AST -> db.files.green_roots[fid_local] ---
static inline struct GreenNode *file_ast_read(struct db *s, FileId f) {
    uint32_t local = file_id_local(f);
    if (local >= s->files.green_roots.count) return NULL;
    return *(struct GreenNode **)vec_get(&s->files.green_roots, local);
}
static inline void file_ast_write(struct db *s, FileId f, struct GreenNode *v) {
    uint32_t local = file_id_local(f);
    if (local >= s->files.green_roots.count) return;
    *(struct GreenNode **)vec_get(&s->files.green_roots, local) = v;
}


// --- LINE_INDEX -> db.files.line_starts[fid_local] (FileArray by value) ---
static inline FileArray line_index_read(struct db *s, FileId f) {
    uint32_t local = file_id_local(f);
    FileArray empty = {0};
    if (local >= s->files.line_starts.count) return empty;
    return *(FileArray *)vec_get(&s->files.line_starts, local);
}
static inline void line_index_write(struct db *s, FileId f, FileArray v) {
    uint32_t local = file_id_local(f);
    if (local >= s->files.line_starts.count) return;
    *(FileArray *)vec_get(&s->files.line_starts, local) = v;
}

// --- FILE_IMPORTS -> db.files.imports[fid_local] (FileArray by value) ---
static inline FileArray file_imports_read(struct db *s, FileId f) {
    uint32_t local = file_id_local(f);
    FileArray empty = {0};
    if (local >= s->files.imports.count) return empty;
    return *(FileArray *)vec_get(&s->files.imports, local);
}
static inline void file_imports_write(struct db *s, FileId f, FileArray v) {
    uint32_t local = file_id_local(f);
    if (local >= s->files.imports.count) return;
    *(FileArray *)vec_get(&s->files.imports, local) = v;
}

// --- TOP_LEVEL_ENTRY -> db.top_level_entry.results[row] (HashMap-routed) ---
static inline TopLevelEntry top_level_entry_read(struct db *s, uint64_t key) {
    TopLevelEntry empty = {0};
    void *rp = hashmap_get(&s->top_level_entry_cache, key);
    if (!rp) return empty;
    return *(TopLevelEntry *)paged_get(&s->top_level_entry.results, (uint32_t)(uintptr_t)rp);
}
static inline void top_level_entry_write(struct db *s, uint64_t key, TopLevelEntry v) {
    void *rp = hashmap_get(&s->top_level_entry_cache, key);
    if (!rp) return;
    *(TopLevelEntry *)paged_get(&s->top_level_entry.results, (uint32_t)(uintptr_t)rp) = v;
}

// --- NAMESPACE_ITEMS -> db.namespaces.items[nsid.idx] (Vec-indexed) ---
// FileArray header over a malloc'd NamespaceItem[] body (see file_imports).
static inline FileArray namespace_items_read(struct db *s, NamespaceId n) {
    FileArray empty = {0};
    if (n.idx >= s->namespaces.items.count) return empty;
    return *(FileArray *)vec_get(&s->namespaces.items, n.idx);
}
static inline void namespace_items_write(struct db *s, NamespaceId n, FileArray v) {
    if (n.idx >= s->namespaces.items.count) return;
    *(FileArray *)vec_get(&s->namespaces.items, n.idx) = v;
}

// --- NAMESPACE_SCOPES -> db.namespaces.exports[nsid.idx] (Vec-indexed) ---
static inline NamespaceScopes namespace_scopes_read(struct db *s, NamespaceId n) {
    NamespaceScopes empty = {0};
    if (n.idx >= s->namespaces.exports.count) return empty;
    return *(NamespaceScopes *)vec_get(&s->namespaces.exports, n.idx);
}
static inline void namespace_scopes_write(struct db *s, NamespaceId n, NamespaceScopes v) {
    if (n.idx >= s->namespaces.exports.count) return;
    *(NamespaceScopes *)vec_get(&s->namespaces.exports, n.idx) = v;
}

// --- DEF_IDENTITY -> db.def_identity.results[row] (HashMap-routed) ---
static inline DefId def_identity_read(struct db *s, uint64_t key) {
    void *rp = hashmap_get(&s->def_by_identity, key);
    if (!rp) return DEF_ID_NONE;
    return *(DefId *)paged_get(&s->def_identity.results, (uint32_t)(uintptr_t)rp);
}
static inline void def_identity_write(struct db *s, uint64_t key, DefId v) {
    void *rp = hashmap_get(&s->def_by_identity, key);
    if (!rp) return;
    *(DefId *)paged_get(&s->def_identity.results, (uint32_t)(uintptr_t)rp) = v;
}

// --- RESOLVE_REF -> db.resolve_ref.results[row] (HashMap-routed) ---
static inline DefId resolve_ref_read(struct db *s, uint64_t key) {
    void *rp = hashmap_get(&s->resolve_ref_cache, key);
    if (!rp) return DEF_ID_NONE;
    return *(DefId *)paged_get(&s->resolve_ref.results, (uint32_t)(uintptr_t)rp);
}
static inline void resolve_ref_write(struct db *s, uint64_t key, DefId v) {
    void *rp = hashmap_get(&s->resolve_ref_cache, key);
    if (!rp) return;
    *(DefId *)paged_get(&s->resolve_ref.results, (uint32_t)(uintptr_t)rp) = v;
}

// --- TYPE_OF_DECL -> per-kind .type or .type_result.type ---
//
// The per-kind routing (which column / struct field holds the IpIndex) lives
// in ONE place — db_def_type_cell (db.h). These wrappers add only the
// graceful guards db_def_type_cell deliberately omits (out-of-range DefId,
// unclassified KIND_NONE → IP_NONE / no-op) and then read/write through the
// cell. Keeping a single routing switch means adding a DefKind touches
// db_def_type_cell + db_def_set_kind, not three parallel switches.
static inline IpIndex type_of_decl_read(struct db *s, DefId def) {
    if (def.idx == 0 || def.idx >= s->defs.kinds.count) return IP_NONE;
    if (*(DefKind *)vec_get(&s->defs.kinds, def.idx) == KIND_NONE) return IP_NONE;
    return *db_def_type_cell(s, def);
}
static inline void type_of_decl_write(struct db *s, DefId def, IpIndex v) {
    if (def.idx == 0 || def.idx >= s->defs.kinds.count) return;
    if (*(DefKind *)vec_get(&s->defs.kinds, def.idx) == KIND_NONE) return;
    *db_def_type_cell(s, def) = v;
}

// Writes the per-decl hover NodeTypesRange embedded in TYPE_OF_DECL's result
// struct (struct field types, variable/constant value types), freeing the
// prior HashMap first so recompute doesn't leak — same free-old discipline as
// fn_signature_write / infer_body_write. Functions use signature_result.node_types
// (written by fn_signature_write); unions/enums/effects/handlers carry no hover
// range, so this is a no-op for them.
static inline void type_of_decl_node_types_write(struct db *s, DefId def, NodeTypesRange v) {
    if (def.idx == 0 || def.idx >= s->defs.kinds.count) return;
    DefKind k = *(DefKind *)vec_get(&s->defs.kinds, def.idx);
    uint32_t row = *(uint32_t *)vec_get(&s->defs.kind_row, def.idx);
    NodeTypesRange *cell;
    switch (k) {
    case KIND_STRUCT:   cell = &((StructType   *)paged_get(&s->structs.type_result,   row))->field_node_types; break;
    case KIND_VARIABLE: cell = &((VariableType *)paged_get(&s->variables.type_result, row))->value_node_types; break;
    case KIND_CONSTANT: cell = &((ConstantType *)paged_get(&s->constants.type_result, row))->value_node_types; break;
    default: return;
    }
    hashmap_free(&cell->types);
    *cell = v;
}

// Per-kind side-data accessors (read-only).
// These return the NodeTypesRange embedded in the kind's type_result.
// The HashMap inside is engine-owned heap; caller must NOT free.
static inline NodeTypesRange struct_field_node_types_read(struct db *s, DefId def) {
    NodeTypesRange empty = {0};
    if (def.idx == 0 || def.idx >= s->defs.kinds.count) return empty;
    if (*(DefKind *)vec_get(&s->defs.kinds, def.idx) != KIND_STRUCT) return empty;
    uint32_t row = *(uint32_t *)vec_get(&s->defs.kind_row, def.idx);
    if (row >= s->structs.type_result.count) return empty;
    return ((StructType *)paged_get(&s->structs.type_result, row))->field_node_types;
}
static inline NodeTypesRange variable_value_node_types_read(struct db *s, DefId def) {
    NodeTypesRange empty = {0};
    if (def.idx == 0 || def.idx >= s->defs.kinds.count) return empty;
    if (*(DefKind *)vec_get(&s->defs.kinds, def.idx) != KIND_VARIABLE) return empty;
    uint32_t row = *(uint32_t *)vec_get(&s->defs.kind_row, def.idx);
    if (row >= s->variables.type_result.count) return empty;
    return ((VariableType *)paged_get(&s->variables.type_result, row))->value_node_types;
}
static inline NodeTypesRange constant_value_node_types_read(struct db *s, DefId def) {
    NodeTypesRange empty = {0};
    if (def.idx == 0 || def.idx >= s->defs.kinds.count) return empty;
    if (*(DefKind *)vec_get(&s->defs.kinds, def.idx) != KIND_CONSTANT) return empty;
    uint32_t row = *(uint32_t *)vec_get(&s->defs.kind_row, def.idx);
    if (row >= s->constants.type_result.count) return empty;
    return ((ConstantType *)paged_get(&s->constants.type_result, row))->value_node_types;
}

// --- FN_SIGNATURE -> db.fns.signature_result[kind_row] (FnSignature) ---
//
// Returns const FnSignature * (borrowed pointer into the column).
// Caller reads .type / .node_types without freeing.
// fn_signature_write frees the old node_types HashMap before
// overwriting, so recompute doesn't leak.
static inline const FnSignature *fn_signature_read(struct db *s, DefId def) {
    if (def.idx == 0 || def.idx >= s->defs.kinds.count) return NULL;
    if (*(DefKind *)vec_get(&s->defs.kinds, def.idx) != KIND_FUNCTION) return NULL;
    uint32_t row = *(uint32_t *)vec_get(&s->defs.kind_row, def.idx);
    if (row >= s->fns.signature_result.count) return NULL;
    return (const FnSignature *)paged_get(&s->fns.signature_result, row);
}
static inline void fn_signature_write(struct db *s, DefId def, FnSignature v) {
    if (def.idx == 0 || def.idx >= s->defs.kinds.count) return;
    if (*(DefKind *)vec_get(&s->defs.kinds, def.idx) != KIND_FUNCTION) return;
    uint32_t row = *(uint32_t *)vec_get(&s->defs.kind_row, def.idx);
    if (row >= s->fns.signature_result.count) return;
    FnSignature *slot = (FnSignature *)paged_get(&s->fns.signature_result, row);
    hashmap_free(&slot->node_types.types);
    *slot = v;
}

// --- INFER_BODY -> db.fns.body_node_types[kind_row] (NodeTypesRange) ---
//
// Returns the NodeTypesRange by value (struct shallow-copy is fine —
// engine owns the heap). infer_body_write frees the prior HashMap
// before overwriting.
static inline NodeTypesRange infer_body_read(struct db *s, DefId def) {
    NodeTypesRange empty = {0};
    if (def.idx == 0 || def.idx >= s->defs.kinds.count) return empty;
    if (*(DefKind *)vec_get(&s->defs.kinds, def.idx) != KIND_FUNCTION) return empty;
    uint32_t row = *(uint32_t *)vec_get(&s->defs.kind_row, def.idx);
    if (row >= s->fns.body_node_types.count) return empty;
    return *(NodeTypesRange *)paged_get(&s->fns.body_node_types, row);
}
static inline void infer_body_write(struct db *s, DefId def, NodeTypesRange v) {
    if (def.idx == 0 || def.idx >= s->defs.kinds.count) return;
    if (*(DefKind *)vec_get(&s->defs.kinds, def.idx) != KIND_FUNCTION) return;
    uint32_t row = *(uint32_t *)vec_get(&s->defs.kind_row, def.idx);
    if (row >= s->fns.body_node_types.count) return;
    NodeTypesRange *slot = (NodeTypesRange *)paged_get(&s->fns.body_node_types, row);
    hashmap_free(&slot->types);
    *slot = v;
}

// --- BODY_SCOPES -> db.fns.body[kind_row] (FnBody with embedded HashMap) ---
//
// Returns const FnBody * (borrowed pointer). FnBody now embeds a
// HashMap (scope_map) folded in by H11; return-by-pointer avoids
// shallow-copying the HashMap struct's pointer fields out of the
// engine's ownership. body_scopes_write frees the prior scope_map
// HashMap before overwriting.
static inline const FnBody *body_scopes_read(struct db *s, DefId def) {
    if (def.idx == 0 || def.idx >= s->defs.kinds.count) return NULL;
    if (*(DefKind *)vec_get(&s->defs.kinds, def.idx) != KIND_FUNCTION) return NULL;
    uint32_t row = *(uint32_t *)vec_get(&s->defs.kind_row, def.idx);
    if (row >= s->fns.body.count) return NULL;
    return (const FnBody *)paged_get(&s->fns.body, row);
}
static inline void body_scopes_write(struct db *s, DefId def, FnBody v) {
    if (def.idx == 0 || def.idx >= s->defs.kinds.count) return;
    if (*(DefKind *)vec_get(&s->defs.kinds, def.idx) != KIND_FUNCTION) return;
    uint32_t row = *(uint32_t *)vec_get(&s->defs.kind_row, def.idx);
    if (row >= s->fns.body.count) return;
    FnBody *slot = (FnBody *)paged_get(&s->fns.body, row);
    hashmap_free(&slot->scope_map);
    *slot = v;
}

// --- NAMESPACE_TYPE -> db.namespaces.namespace_type[nsid.idx] (own column) ---
// H23 split this out of NamespaceScopes.struct_type so each query owns
// exactly one named result column.
static inline IpIndex namespace_type_read(struct db *s, NamespaceId n) {
    if (n.idx >= s->namespaces.namespace_type.count) return IP_NONE;
    return *(IpIndex *)vec_get(&s->namespaces.namespace_type, n.idx);
}
static inline void namespace_type_write(struct db *s, NamespaceId n, IpIndex v) {
    if (n.idx >= s->namespaces.namespace_type.count) return;
    *(IpIndex *)vec_get(&s->namespaces.namespace_type, n.idx) = v;
}

#endif // ORE_DB_QUERY_RESULT_COLUMNS_H
