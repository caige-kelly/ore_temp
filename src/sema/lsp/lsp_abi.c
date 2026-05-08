#include "lsp_abi.h"

#include <stdlib.h>
#include <string.h>

#include "../../common/arena.h"
#include "../../common/stringpool.h"
#include "../../diag/diag.h"
#include "../ids/ids.h"
#include "../index/position.h"
#include "../index/refs.h"
#include "../modules/inputs.h"
#include "../modules/modules.h"
#include "../request/cancel.h"
#include "../request/snapshot.h"
#include "../scope/scope.h"
#include "../sema.h"

// LSP ABI thin wrappers.
//
// Each function constructs/owns the Rust-facing types from the
// internal ID-keyed structures. Memory model: the database holds
// an arena that owns most data; what we hand back to Rust is
// either:
//   - a borrowed pointer into the arena (lifetime = until next
//     mutation of the same db; Rust copies if it needs to keep
//     it longer)
//   - a freshly malloc'd struct (caller releases via the matching
//     destroy/release call)
//
// We don't expose Sema's internal HashMaps or Vecs across the
// FFI boundary — Rust gets POD only.

// === Internal: OreDb wrapper ===

struct OreDb {
    Arena arena;
    StringPool pool;
    struct DiagBag diags;
    struct Sema sema;
};

// Initial-capacity defaults for the arena and string pool. 64 KiB
// is comfortable for tiny programs and grows on demand. LSP shells
// can override later via a config knob.
#define ORE_DB_DEFAULT_ARENA_CAP   (64 * 1024)
#define ORE_DB_DEFAULT_POOL_CAP    (64 * 1024)

OreDb *ore_db_create(void) {
    OreDb *db = (OreDb *)calloc(1, sizeof(OreDb));
    if (!db)
        return NULL;
    arena_init(&db->arena, ORE_DB_DEFAULT_ARENA_CAP);
    pool_init(&db->pool, ORE_DB_DEFAULT_POOL_CAP);
    db->diags = diag_bag_new(&db->arena);

    db->sema = (struct Sema){
        .arena = &db->arena,
        .pool = &db->pool,
        .diags = &db->diags,
        .slot_budget = 50000,
    };

    // Bring up the bottom-of-stack: ID tables, inputs, prelude.
    sema_ids_init(&db->sema);
    sema_inputs_init(&db->sema);
    prelude_init(&db->sema);
    return db;
}

void ore_db_destroy(OreDb *db) {
    if (!db)
        return;
    arena_free(&db->arena);
    pool_free(&db->pool);
    // DiagBag entries live in the arena — arena_free already
    // released them. No separate diag_free.
    free(db);
}

// === Input management ===

static InputId resolve_path_to_input(OreDb *db, const char *path) {
    return sema_register_input(&db->sema, path);
}

bool ore_db_set_input(OreDb *db, const char *path, const char *text,
                      size_t text_len) {
    if (!db || !path)
        return false;
    InputId id = resolve_path_to_input(db, path);
    if (!input_id_is_valid(id))
        return false;
    sema_set_input_source(&db->sema, id, text, text_len);
    return true;
}

bool ore_db_invalidate_input(OreDb *db, const char *path) {
    if (!db || !path)
        return false;
    InputId id = resolve_path_to_input(db, path);
    if (!input_id_is_valid(id))
        return false;
    sema_invalidate_input(&db->sema, id);
    return true;
}

// === Position helpers ===

static OreSpan span_to_ore(struct Span span) {
    return (OreSpan){
        .file_id = span.file_id,
        .start_line = span.line,
        .start_col = span.column,
        .end_line = span.line_end,
        .end_col = span.column_end,
    };
}

static OreDefKind kind_for_def(struct DefInfo *info) {
    if (!info)
        return ORE_DEF_NONE;
    if (info->kind == DECL_PRIMITIVE)
        return ORE_DEF_PRIMITIVE;
    if (info->kind == DECL_PARAM)
        return ORE_DEF_PARAM;
    if (info->kind == DECL_FIELD)
        return ORE_DEF_FIELD;
    switch (info->semantic_kind) {
    case SEM_VALUE: return ORE_DEF_VALUE;
    case SEM_TYPE:  return ORE_DEF_TYPE;
    case SEM_EFFECT: return ORE_DEF_EFFECT;
    case SEM_MODULE: return ORE_DEF_MODULE;
    default: return ORE_DEF_VALUE;
    }
}

// Find the ModuleId for a path. Inputs map 1:1 to modules in the
// single-crate world; the helper walks the input table to find
// the input matching `path`, then locates the corresponding
// ModuleId via module_by_path.
static ModuleId module_for_path(struct Sema *s, const char *path) {
    if (!path)
        return MODULE_ID_INVALID;
    uint32_t path_id = pool_intern(s->pool, path, strlen(path));
    if (!hashmap_contains(&s->module_by_path, (uint64_t)path_id))
        return MODULE_ID_INVALID;
    void *slot = hashmap_get(&s->module_by_path, (uint64_t)path_id);
    return (ModuleId){(uint32_t)(uintptr_t)slot};
}

OreDefRef ore_db_resolve_at(OreDb *db, const char *path, uint32_t line,
                            uint32_t col) {
    OreDefRef out = {0};
    if (!db || !path)
        return out;

    ModuleId mid = module_for_path(&db->sema, path);
    if (!module_id_is_valid(mid))
        return out;

    struct NodeId node = query_node_at_position(&db->sema, mid, line, col);
    if (node.id == 0)
        return out;

    DefId def = query_def_at_position(&db->sema, mid, line, col);
    struct DefInfo *info = def_info(&db->sema, def);
    if (!info)
        return out;

    out.def_id = def.idx;
    out.kind = kind_for_def(info);
    out.defining_span = span_to_ore(info->span);
    // For use_span we'd want the Ident's actual span; for now
    // approximate via the position. Refining lands when the
    // node_to_expr index threading matures.
    out.use_span = (OreSpan){
        .file_id = -1,
        .start_line = (int32_t)line,
        .start_col = (int32_t)col,
        .end_line = (int32_t)line,
        .end_col = (int32_t)col + 1,
    };
    out.name = pool_get(db->sema.pool, info->name_id, 0);
    return out;
}

// === References ===

void ore_spans_release(OreSpans *spans) {
    if (!spans)
        return;
    free(spans->items);
    spans->items = NULL;
    spans->count = 0;
}

OreSpans ore_db_references(OreDb *db, uint32_t def_id) {
    OreSpans out = {0};
    if (!db)
        return out;
    DefId def = (DefId){def_id};
    Vec *nodes = query_references_of(&db->sema, def);
    if (!nodes || nodes->count == 0)
        return out;

    OreSpan *items = (OreSpan *)calloc(nodes->count, sizeof(OreSpan));
    if (!items)
        return out;

    size_t count = 0;
    for (size_t i = 0; i < nodes->count; i++) {
        struct NodeId *node_id = (struct NodeId *)vec_get(nodes, i);
        if (!node_id)
            continue;
        // Find the AST Expr* via node_to_expr to read its span.
        if (db->sema.node_to_expr.entries == NULL)
            continue;
        if (!hashmap_contains(&db->sema.node_to_expr,
                              (uint64_t)node_id->id))
            continue;
        struct Expr *e = (struct Expr *)hashmap_get(
            &db->sema.node_to_expr, (uint64_t)node_id->id);
        if (!e)
            continue;
        items[count++] = span_to_ore(e->span);
    }
    out.items = items;
    out.count = count;
    return out;
}

// === Diagnostics ===

void ore_diagnostics_release(OreDiagnostics *diags) {
    if (!diags)
        return;
    free(diags->items);
    diags->items = NULL;
    diags->count = 0;
}

OreDiagnostics ore_db_diagnostics(OreDb *db, const char *path) {
    OreDiagnostics out = {0};
    if (!db || !path)
        return out;
    // Real implementation walks the DiagBag, filters by
    // file_id matching `path`, allocates an OreDiagnostic[] copy.
    // Today the diag bag layer doesn't expose filtered iteration;
    // wiring lands when codes.h ships and DiagBag gains the
    // dedup-by-(code, span) story from Layer 6 of the previous
    // plan. Returning an empty list is a safe no-op stub.
    (void)path;
    return out;
}

// === Cancellation ===

struct OreCancelToken {
    struct CancelToken inner;
};

OreCancelToken *ore_cancel_token_create(void) {
    OreCancelToken *tok = (OreCancelToken *)calloc(1, sizeof(OreCancelToken));
    if (!tok)
        return NULL;
    cancel_token_init(&tok->inner);
    return tok;
}

void ore_cancel_token_destroy(OreCancelToken *tok) {
    free(tok);
}

void ore_db_set_cancel(OreDb *db, OreCancelToken *tok) {
    if (!db)
        return;
    sema_set_active_cancel(&db->sema, tok ? &tok->inner : NULL);
}

void ore_db_cancel(OreCancelToken *tok) {
    if (!tok)
        return;
    cancel_token_signal(&tok->inner);
}

// === Snapshots ===

struct OreSnapshot {
    struct Snapshot inner;
};

OreSnapshot *ore_db_snapshot_begin(OreDb *db) {
    if (!db)
        return NULL;
    OreSnapshot *snap = (OreSnapshot *)calloc(1, sizeof(OreSnapshot));
    if (!snap)
        return NULL;
    snap->inner = sema_snapshot_begin(&db->sema);
    return snap;
}

void ore_db_snapshot_end(OreSnapshot *snap) {
    if (!snap)
        return;
    sema_snapshot_end(&snap->inner);
    free(snap);
}
