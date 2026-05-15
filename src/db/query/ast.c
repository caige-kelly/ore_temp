#include "ast.h"
#include "invalidate.h"
#include "../../lexer/lexer.h"
#include "../../lexer/layout.h"
#include "../../parser/parser.h"
#include "../workspace/module_info.h"  // ModuleInfo (parser output builder — transitional)

#include <string.h>

Fingerprint db_query_module_ast(struct db *s, ModuleId mod) {
    ModuleId *stable_mod = (ModuleId*)vec_get(&s->modules.ids, mod.idx);

    // DB_QUERY_GUARD evaluates the on_cached expression only when the
    // begin returns CACHED; we re-locate inside that branch rather than
    // caching a QuerySlot* across the macro (Vec column reallocs would
    // invalidate it).
    DB_QUERY_GUARD(s, QUERY_MODULE_AST, stable_mod,
                   db_locate_slot(s, QUERY_MODULE_AST, stable_mod)->fingerprint,
                   FINGERPRINT_NONE,
                   FINGERPRINT_NONE);
    
    // 1. Get Source
    FileId file_id = *(FileId*)vec_get(&s->modules.files, mod.idx);
    uint32_t src_idx = file_id_local(file_id);
    const char *source = *(const char **)vec_get(&s->sources.texts, src_idx);
    size_t source_len = *(uint32_t*)vec_get(&s->sources.text_lens, src_idx);
    
    // 2. Lex
    Vec raw_tokens, line_starts;
    vec_init(&raw_tokens, sizeof(Token));
    vec_init(&line_starts, sizeof(uint32_t));
    
    lex(source, source_len, &s->strings, &raw_tokens, &line_starts);
    *(Vec*)vec_get(&s->modules.line_starts, mod.idx) = line_starts;
    
    // 3. Layout
    Vec real_tokens, trivia_tokens, trivia_offsets;
    vec_init(&real_tokens, sizeof(Token));
    vec_init(&trivia_tokens, sizeof(Token));
    vec_init(&trivia_offsets, sizeof(uint32_t));
    
    layout(&raw_tokens, line_starts.data, line_starts.count, &real_tokens, &trivia_tokens, &trivia_offsets);
    
    *(Vec*)vec_get(&s->modules.trivia_tokens, mod.idx) = trivia_tokens;
    *(Vec*)vec_get(&s->modules.trivia_offsets, mod.idx) = trivia_offsets;
    
    // 4. Parse — parse_module allocates ASTStore in mod->arena and inits
    //    the side-tables (span_map, top_level_index, node_to_decl).
    struct ModuleInfo m;
    module_info_init(&m, mod, STR_ID_NONE, file_id);

    parse_module(&m, &real_tokens, NULL);
    
    // 5. Compute AST Fingerprint
    Fingerprint f1 = db_fp_bytes(m.ast->kinds.data, m.ast->kinds.count * sizeof(AstNodeKind));
    Fingerprint f2 = db_fp_bytes(m.ast->main_tokens.data, m.ast->main_tokens.count * sizeof(uint32_t));
    Fingerprint f3 = db_fp_bytes(m.ast->data.data, m.ast->data.count * sizeof(AstNodeData));
    Fingerprint f4 = db_fp_bytes(m.ast->extra.data, m.ast->extra.count * sizeof(uint32_t));
    Fingerprint final_fp = db_fp_combine(db_fp_combine(f1, f2), db_fp_combine(f3, f4));
    
    m.durable_fp = final_fp;
    *(Fingerprint*)vec_get(&s->modules.durable_fps, mod.idx) = final_fp;
    
    // 6. Persist results
    *(void**)vec_get(&s->modules.asts, mod.idx) = m.ast;
    *(Vec*)vec_get(&s->modules.top_level_indices, mod.idx) = m.top_level_index;
    *(Vec*)vec_get(&s->modules.node_to_decls, mod.idx) = m.node_to_decl;
    *(void**)vec_get(&s->modules.ast_id_maps, mod.idx) = m.ast_id_map;
    
    // 7. Flatten Side Data
    uint32_t node_count = m.span_map.count;
    void *side_data = arena_alloc(&s->arena, node_count * 16);
    
    TinySpan *spans = (TinySpan*)side_data;
    AstNodeId *parents = (AstNodeId*)((uint8_t*)side_data + node_count * 8); // TinySpan is 8 bytes
    uint32_t *types = (uint32_t*)((uint8_t*)parents + node_count * 4); // AstNodeId is 4 bytes
    
    if (m.span_map.data && node_count > 0) {
        memcpy(spans, m.span_map.data, node_count * sizeof(TinySpan));
    } else if (node_count > 0) {
        memset(spans, 0, node_count * sizeof(TinySpan));
    }
    
    if (m.parent_map.data && m.parent_map.count == node_count) {
        memcpy(parents, m.parent_map.data, node_count * sizeof(AstNodeId));
    } else if (node_count > 0) {
        memset(parents, 0, node_count * sizeof(AstNodeId));
    }
    
    if (m.type_map.data && m.type_map.count == node_count) {
        memcpy(types, m.type_map.data, node_count * sizeof(uint32_t));
    } else if (node_count > 0) {
        memset(types, 0, node_count * sizeof(uint32_t));
    }
    
    ModuleNodeData *nd = (ModuleNodeData*)vec_get(&s->modules.node_data, mod.idx);
    nd->spans   = spans;
    nd->parents = parents;
    nd->types   = (IpIndex*)types;
    *(uint32_t*)vec_get(&s->modules.node_counts, mod.idx) = node_count;
    
    vec_free(&raw_tokens);
    vec_free(&real_tokens);
    
    db_query_succeed(s, QUERY_MODULE_AST, stable_mod, final_fp);
    return final_fp;
}
