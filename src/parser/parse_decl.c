#include "parse_decl.h"
#include "ast.h"
#include "parse_expr.h"

// =============================================================================
// Top-level declarations.
//
// Ore is Odin-style and expression-oriented: there is no separate decl
// grammar. A top-level item is just `parse_expr(p, PREC_NONE)`, and the
// bind operators (`::` / `:=` / `:`) are a guarded low-precedence Pratt
// infix (see parse_expr.c) that yields AST_DECL_CONST / AST_DECL_VAR.
//
// So this file only drives the loop and lifts each bind into the module's
// TopLevelEntry index. The decl node is self-describing — its extras are
// [name_strid, type_id, value_id, meta] — so nothing here needs parser
// context or a top-level flag.
// =============================================================================

static inline AstNodeKind node_kind(const Parser *p, AstNodeId id) {
  return ((AstNodeKind *)p->ast->kinds.data)[id.idx];
}

void parse_top_level_decls(Parser *p) {
  // extras = [decl_count, decl0, decl1, ...] — collected unbounded on
  // the scratch stack. Nested parse_expr scratch regions strictly nest
  // above this one and unwind before we push the next decl idx.
  uint32_t st = scratch_open(p);
  uint32_t cnt_at = scratch_reserve(p); // decl_count placeholder
  uint32_t decl_count = 0;

  while (!p_is_eof(p)) {
    uint32_t before = p->pos;
    AstNodeId node = parse_expr(p, PREC_NONE);

    if (node.idx != 0) {
      AstNodeKind k = node_kind(p, node);
      if (k == AST_DECL_CONST || k == AST_DECL_VAR) {
        // Self-describing decl: extras = [name, type, value, meta].
        uint32_t *ex =
            &((uint32_t *)p->ast->extra.data)
                [((AstNodeData *)p->ast->data.data)[node.idx].extra_idx.idx];
        StrId name = {ex[0]};
        DefMeta meta = (DefMeta)ex[3];

        TopLevelEntry entry = {
            .name = name,
            .node = node,
            .meta = meta,
            .ast_id = ast_id_compute(k, name),
        };
        vec_push(&p->top_level_index, &entry);

        scratch_push(p, node.idx);
        decl_count++;
      } else {
        p_error(p, "expected a declaration at top level");
      }
    }

    p_consume(p, TK_SEMI, "Expected ';' after top-level declaration");

    // Forward-progress guard: never spin if no token was consumed.
    if (p->pos == before)
      p_advance(p);
  }

  scratch_set(p, cnt_at, decl_count);
  AstExtraDataIdx extra = scratch_emit(p, st);
  AstNodeData data = {0};
  data.extra_idx = extra;

  const Token *first =
      p->tokens->count > 0 ? vec_get((Vec *)p->tokens, 0) : NULL;
  const Token *last = p->tokens->count > 0
                          ? vec_get((Vec *)p->tokens, p->tokens->count - 1)
                          : NULL;
  TinySpan span = TINYSPAN_NONE;
  if (first && last)
    span = p_span(p, first, last);

  p_push_node(p, AST_DECL_MODULE, 0, data, span);
}
