#include "module_info.h"

#include <string.h>

#include "../query/query.h"
#include "ast_id_map.h"

// Default first-chunk capacity for the per-module arena. Modest because
// many modules in a workspace will be small (single decl files); larger
// modules grow via arena chunk doubling. Lexer/parser passes that know
// the token count up front size their Big Four tables explicitly via
// vec_init_in_arena, so this default mostly sets the floor.
#define ORE_MODULE_ARENA_DEFAULT_CAP (16 * 1024)

void module_info_init(struct ModuleInfo *mod, ModuleId id, StrId name, FileId file) {
    if (!mod) return;

    // db_alloc_module already zeroed the struct (arena_alloc zeroes).
    // Setting fields explicitly here for clarity and defensive re-init
    // safety. The embedded `Arena arena` is zero-initialized — arena_init
    // resets it again, so this is safe.

    mod->id   = id;
    mod->name = name;
    mod->file = file;

    arena_init(&mod->arena, ORE_MODULE_ARENA_DEFAULT_CAP);

    // Vec fields stay zero-initialized — empty count/capacity is the
    // correct "no parse yet" state. The parser/lexer initializes them
    // via vec_init_in_arena once the token count is known.

    // ASTStore + AstIdMap pointers stay NULL until the parser fills them.
    // (ast_id_map is lazy-allocated by the parser's top-level-index pass
    // because it depends on the Big Four side-table being sized first.)

    mod->durable_fp = FINGERPRINT_NONE;

    // Initialize the 4 per-module query slots with the correct kinds so
    // any consumer that reads slot->kind before the first compute sees
    // a sensible value.
    db_query_slot_init(&mod->slot_module_ast,        QUERY_MODULE_AST);
    db_query_slot_init(&mod->slot_top_level_index,   QUERY_TOP_LEVEL_INDEX);
    db_query_slot_init(&mod->slot_module_exports,    QUERY_MODULE_EXPORTS);
    db_query_slot_init(&mod->slot_module_def_map,    QUERY_MODULE_DEF_MAP);
}

void module_info_reset(struct ModuleInfo *mod) {
    if (!mod) return;

    // Wipe the per-module arena. Everything that lived inside it (AST,
    // Big Four buffers, ast_id_map's HashMap entries, top_level_index,
    // node_to_decl) is gone in O(1).
    arena_reset(&mod->arena);

    // The Vec structs (embedded in this struct, NOT in mod->arena) now
    // point at freed memory. Zero them so the next parse's
    // vec_init_in_arena starts from a clean state.
    mod->line_starts        = (Vec){0};
    mod->span_map           = (Vec){0};
    mod->parent_map         = (Vec){0};
    mod->trivia_map.tokens  = (Vec){0};
    mod->trivia_map.offsets = (Vec){0};
    mod->type_map           = (Vec){0};
    mod->top_level_index    = (Vec){0};
    mod->node_to_decl       = (Vec){0};

    // AST + AstIdMap pointers pointed into mod->arena, now dangling.
    mod->ast        = NULL;
    mod->ast_id_map = NULL;

    // The new parse will compute and stamp a fresh fingerprint via
    // db_query_succeed on slot_module_ast. Until then, leave the old
    // fingerprint in place so revalidation can compare against it for
    // early-cutoff.
    //
    // We do NOT touch the slots themselves — slot state, deps, and diag
    // accumulators are managed by the query engine across reparses.
}

void module_info_free(struct ModuleInfo *mod) {
    if (!mod) return;

    // Free the per-module arena chunks. The ModuleInfo struct itself
    // lives in db.arena and is reclaimed when that arena is freed
    // (after this loop runs in db_free).
    //
    // Slot internals (slot->deps backing buffer, slot->diag_arena
    // chunks) are released BEFORE this point by the slot_release
    // visitor in db_free — see lifecycle.c. We don't double-free here.
    arena_free(&mod->arena);

    // Zero what we can, defensively.
    mod->ast        = NULL;
    mod->ast_id_map = NULL;
}
