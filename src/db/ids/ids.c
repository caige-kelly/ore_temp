#include "ids.h"
#include "../db.h"

void db_ids_init(struct db *s) {
    // Sources table
    vec_init_in(&s->sources, &s->global_arena, sizeof(struct Source));

    // ASTStore table
    vec_init_in(&s->module_asts, &s->global_arena, sizeof(ASTStore*));

    // Defs Tables (Columns)
    vec_init_in(&s->defs.names, &s->global_arena, sizeof(StrId));
    vec_init_in(&s->defs.parent_modules, &s->global_arena, sizeof(ModuleId));
    vec_init_in(&s->defs.kinds, &s->global_arena, sizeof(DefKind));
    vec_init_in(&s->defs.signatures, &s->global_arena, sizeof(TypeId));
    vec_init_in(&s->defs.values, &s->global_arena, sizeof(ValueId));
    vec_init_in(&s->defs.effects, &s->global_arena, sizeof(EffectId));
    vec_init_in(&s->defs.handler, &s->global_arena, sizeof(HandlerId));
    vec_init_in(&s->defs.file, &s->global_arena, sizeof(FileId));
    vec_init_in(&s->defs.node, &s->global_arena, sizeof(AstNodeId));
    vec_init_in(&s->defs.extras, &s->global_arena, sizeof(AstExtraDataIdx));

    // Seed Slot 0 (The "NONE" row)
    // This makes ID 0 act like a 'NULL pointer'.
    SourceId none_src = SOURCE_ID_NONE;
    vec_push(&s->sources, &none_src);

    ASTStore none_ast = ASTSTORE_ID_NONE;
    vec_push(&s->module_asts, &none_ast);

    StrId none_str = STR_ID_NONE;
    vec_push(&s->defs.names, &none_str);

    ModuleId none_mod = MODULE_ID_NONE;
    vec_push(&s->defs.parent_modules, &none_mod);
    
    DefKind none_kind = KIND_ID_NONE;
    vec_push(&s->defs.kinds, &none_kind);
    
    TypeId none_type = TYPE_ID_NONE;
    vec_push(&s->defs.signatures, &none_type);

    ValueId none_value = VALUE_ID_NONE;
    vec_push(&s->defs.values, &none_value);

    EffectId none_effect = EFFECT_ID_NONE;
    vec_push(&s->defs.effects, &none_effect);

    HandlerId none_handler = HANDLER_ID_NONE;
    vec_push(&s->defs.handler, &none_handler);

    FileId none_file = FILE_ID_NONE;
    vec_push(&s->defs.file, &none_file);

    AstNodeId none_node = ASTNODE_ID_NONE;
    vec_push(&s->defs.node, &none_node);

    AstExtraDataIdx none_extra = ASTEXTRA_ID_NONE;
    vec_push(&s->defs.extras, &none_extra);
}

DefId db_alloc_def(struct db *s) {
    uint32_t idx = (uint32_t)s->defs.names.count;
    
    // You MUST push into EVERY column in the struct 'defs'
    // to keep the parallel arrays aligned.
    vec_push(&s->defs.names, 0);
    vec_push(&s->defs.parent_modules, 0);
    vec_push(&s->defs.kinds, 0);
    vec_push(&s->defs.signatures, 0);
    vec_push(&s->defs.values, 0);
    vec_push(&s->defs.effects, 0);
    vec_push(&s->defs.handler, 0);
    vec_push(&s->defs.file, 0);
    vec_push(&s->defs.node, 0);
    vec_push(&s->defs.extras, 0);
    
    return (DefId){ .idx = idx };
}