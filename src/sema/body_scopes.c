#include "../db/db.h"
#include "../db/diag/diag.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/ast.h"
#include "../db/query/def_identity.h"
#include "../db/query/fn_signature.h"
#include "../db/workspace/ast_id_map.h"
#include "../parser/ast.h"
#include "sema.h"

#include <stdlib.h>
#include <string.h>

// Builds the per-fn body scope tree. Rust-analyzer's ExprScopes pattern
// adapted to AstNodeId (no HIR layer). Three flat arrays (db.h shapes):
//
//   scopes        Vec<ScopeRow>        — tree (parent pointers)
//   binds         Vec<ScopedBind>      — flat pool, each tagged with owning scope
//   node_to_scope uint32_t * (body span) — O(1) lookup: node → enclosing scope
//
// Walk-time scope-pushing rules:
//   AST_STMT_BLOCK              opens new scope (parent = current)
//   AST_STMT_IF then-branch     opens new scope; if-let bind lands here
//   AST_STMT_IF else-branch     opens new scope (no if-let bind)
//   AST_STMT_LOOP               opens new scope (init/cond/step/body)
//   AST_STMT_SWITCH_ARM         opens new scope (future: pattern binds)
//   AST_DECL_VAR/CONST stmt     pushes a bind into the *current* scope
//
// Self-recursion: typing a let-bind's RHS may call sema_type_of_expr,
// whose PATH lookup calls sema_body_scopes_get(s, def). We store the
// BodyScopes* into db.defs.body_scopes BEFORE walking, so partial state
// is visible to those re-entrant lookups (which is correct — earlier
// binds in source order should be visible to later RHS expressions).

typedef struct {
  ASTStore *ast;
  uint32_t  min;
  uint32_t  max;
} RangeCtx;

static void range_visit(AstNodeId child, void *ud) {
  RangeCtx *ctx = (RangeCtx *)ud;
  if (child.idx == AST_NODE_ID_NONE.idx)
    return;
  if (child.idx < ctx->min)
    ctx->min = child.idx;
  if (child.idx > ctx->max)
    ctx->max = child.idx;
  ast_visit_children(ctx->ast, child, range_visit, ctx);
}

static void find_body_range(ASTStore *ast, AstNodeId body, uint32_t *out_min,
                            uint32_t *out_max) {
  if (body.idx == AST_NODE_ID_NONE.idx) {
    *out_min = 0;
    *out_max = 0;
    return;
  }
  RangeCtx ctx = {.ast = ast, .min = body.idx, .max = body.idx};
  ast_visit_children(ast, body, range_visit, &ctx);
  *out_min = ctx.min;
  *out_max = ctx.max;
}

static uint32_t scope_push(BodyScopes *bs, uint32_t parent,
                           AstNodeId block_node) {
  uint32_t id = (uint32_t)bs->scopes.count;
  ScopeRow row = {.parent = parent, .block_node = block_node};
  vec_push(&bs->scopes, &row);
  return id;
}

static void bind_push(BodyScopes *bs, uint32_t scope_id, StrId name,
                      IpIndex type) {
  ScopedBind sb = {.scope_id = scope_id, .name = name, .type = type};
  vec_push(&bs->binds, &sb);
}

static void tag_node(BodyScopes *bs, AstNodeId node, uint32_t scope_id) {
  if (node.idx == AST_NODE_ID_NONE.idx)
    return;
  if (node.idx < bs->body_root_min)
    return;
  uint32_t off = node.idx - bs->body_root_min;
  if (off >= bs->node_to_scope_count)
    return;
  bs->node_to_scope[off] = scope_id;
}

// Forward declaration for recursive walk.
static void walk(struct db *s, ASTStore *ast, AstNodeId node, ModuleId mid,
                 DefId enclosing_fn, uint32_t file_local, BodyScopes *bs,
                 uint32_t current_scope);

// Recurse into all children with the same `current_scope`. Used for
// expression subtrees and other "transparent" forms that don't open
// scopes themselves but may contain nodes we need to tag.
typedef struct {
  struct db *s;
  ASTStore  *ast;
  ModuleId   mid;
  DefId      enclosing_fn;
  uint32_t   file_local;
  BodyScopes *bs;
  uint32_t   scope;
} WalkCtx;

static void walk_child(AstNodeId child, void *ud) {
  WalkCtx *ctx = (WalkCtx *)ud;
  walk(ctx->s, ctx->ast, child, ctx->mid, ctx->enclosing_fn, ctx->file_local,
       ctx->bs, ctx->scope);
}

static void walk_children(struct db *s, ASTStore *ast, AstNodeId node,
                          ModuleId mid, DefId enclosing_fn, uint32_t file_local,
                          BodyScopes *bs, uint32_t scope) {
  WalkCtx ctx = {.s = s, .ast = ast, .mid = mid, .enclosing_fn = enclosing_fn,
                 .file_local = file_local, .bs = bs, .scope = scope};
  ast_visit_children(ast, node, walk_child, &ctx);
}

// Type a let-bind RHS or annotation. Annotation wins when present.
static IpIndex type_of_bind(struct db *s, ASTStore *ast, AstNodeId type_id,
                            AstNodeId value_id, ModuleId mid, DefId fn,
                            uint32_t file_local) {
  if (type_id.idx != AST_NODE_ID_NONE.idx)
    return sema_resolve_type_expr(s, ast, type_id, mid);
  if (value_id.idx != AST_NODE_ID_NONE.idx)
    return sema_type_of_expr(s, ast, value_id, mid, fn, file_local);
  return IP_NONE;
}

static void walk(struct db *s, ASTStore *ast, AstNodeId node, ModuleId mid,
                 DefId enclosing_fn, uint32_t file_local, BodyScopes *bs,
                 uint32_t current_scope) {
  if (node.idx == AST_NODE_ID_NONE.idx)
    return;

  tag_node(bs, node, current_scope);

  AstNodeKind k = ((AstNodeKind *)ast->kinds.data)[node.idx];
  AstNodeData d = ((AstNodeData *)ast->data.data)[node.idx];

  switch (k) {
  case AST_STMT_BLOCK: {
    // New scope. Extras: [stmt_count, s0, s1, ...].
    uint32_t child = scope_push(bs, current_scope, node);
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    uint32_t count = ex[0];
    for (uint32_t i = 0; i < count; i++) {
      AstNodeId stmt = {.idx = ex[1 + i]};
      walk(s, ast, stmt, mid, enclosing_fn, file_local, bs, child);
    }
    return;
  }

  case AST_DECL_CONST:
  case AST_DECL_VAR: {
    // Statement-position let-bind. Extras: [name, type, value, meta].
    // Bind lands in `current_scope`. Recurse into type/value subtrees
    // so their nodes get tagged (e.g. RHS path lookups need this).
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    StrId name = {.idx = ex[0]};
    AstNodeId type_id = {.idx = ex[1]};
    AstNodeId value_id = {.idx = ex[2]};

    // Tag subtrees BEFORE typing so any reentrant lookup sees them in
    // the right scope. Type-of-RHS may walk into these via
    // sema_type_of_expr → sema_body_scopes_get.
    walk(s, ast, type_id, mid, enclosing_fn, file_local, bs, current_scope);
    walk(s, ast, value_id, mid, enclosing_fn, file_local, bs, current_scope);

    if (name.idx != 0) {
      IpIndex t = type_of_bind(s, ast, type_id, value_id, mid, enclosing_fn,
                               file_local);
      if (t.v != IP_NONE.v)
        bind_push(bs, current_scope, name, t);
    }
    return;
  }

  case AST_STMT_IF: {
    // Extras: [cond, then, else]. The cond may be an if-let
    // (AST_DECL_VAR/CONST), in which case the bind lands in a new
    // then-scope and the cond's RHS types in the parent scope.
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    AstNodeId cond_id = {.idx = ex[0]};
    AstNodeId then_id = {.idx = ex[1]};
    AstNodeId else_id = {.idx = ex[2]};

    bool is_if_let = false;
    if (cond_id.idx != AST_NODE_ID_NONE.idx) {
      AstNodeKind ck = ((AstNodeKind *)ast->kinds.data)[cond_id.idx];
      is_if_let = (ck == AST_DECL_VAR || ck == AST_DECL_CONST);
    }

    if (is_if_let) {
      AstNodeData cd = ((AstNodeData *)ast->data.data)[cond_id.idx];
      const uint32_t *cex = &((uint32_t *)ast->extra.data)[cd.extra_idx.idx];
      StrId name = {.idx = cex[0]};
      AstNodeId rhs_id = {.idx = cex[2]};

      // RHS lives in the PARENT scope (it can't see its own bind).
      walk(s, ast, rhs_id, mid, enclosing_fn, file_local, bs, current_scope);

      uint32_t then_scope = scope_push(bs, current_scope, cond_id);
      tag_node(bs, cond_id, then_scope);

      if (name.idx != 0 && rhs_id.idx != AST_NODE_ID_NONE.idx) {
        IpIndex rhs_t = sema_type_of_expr(s, ast, rhs_id, mid, enclosing_fn,
                                          file_local);
        if (rhs_t.v != IP_NONE.v) {
          if (ip_tag(&s->intern, rhs_t) == IP_TAG_OPTIONAL_TYPE) {
            IpKey ik = ip_key(&s->intern, rhs_t);
            bind_push(bs, then_scope, name, ik.optional_type.elem);
          } else {
            ModuleNodeData *nd =
                (ModuleNodeData *)vec_get(&s->files.node_data, file_local);
            if (nd && nd->spans) {
              TinySpan span = nd->spans[cond_id.idx];
              db_diag_error_t(
                  s, span,
                  "if-let pattern requires optional type, got {0}", rhs_t);
            }
          }
        }
      }

      walk(s, ast, then_id, mid, enclosing_fn, file_local, bs, then_scope);

      uint32_t else_scope = scope_push(bs, current_scope, else_id);
      walk(s, ast, else_id, mid, enclosing_fn, file_local, bs, else_scope);
      return;
    }

    walk(s, ast, cond_id, mid, enclosing_fn, file_local, bs, current_scope);
    uint32_t then_scope = scope_push(bs, current_scope, then_id);
    walk(s, ast, then_id, mid, enclosing_fn, file_local, bs, then_scope);
    uint32_t else_scope = scope_push(bs, current_scope, else_id);
    walk(s, ast, else_id, mid, enclosing_fn, file_local, bs, else_scope);
    return;
  }

  case AST_STMT_LOOP: {
    // Extras: [label, init, cond, step, body]. The C-style
    // `loop (i := 0; ...)` puts the bind in `init` — it must scope
    // to the loop, not leak out. So we open a loop scope here and all
    // four slots run inside it.
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    uint32_t loop_scope = scope_push(bs, current_scope, node);
    walk(s, ast, (AstNodeId){.idx = ex[1]}, mid, enclosing_fn, file_local, bs,
         loop_scope);
    walk(s, ast, (AstNodeId){.idx = ex[2]}, mid, enclosing_fn, file_local, bs,
         loop_scope);
    walk(s, ast, (AstNodeId){.idx = ex[3]}, mid, enclosing_fn, file_local, bs,
         loop_scope);
    walk(s, ast, (AstNodeId){.idx = ex[4]}, mid, enclosing_fn, file_local, bs,
         loop_scope);
    return;
  }

  case AST_STMT_SWITCH_ARM: {
    // Extras: [pat_count, pat0..N, body]. Pattern binds (future) would
    // land in this arm-scope; for now we just isolate the arm body.
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    uint32_t pc = ex[0];
    uint32_t arm_scope = scope_push(bs, current_scope, node);
    for (uint32_t i = 0; i < pc; i++)
      walk(s, ast, (AstNodeId){.idx = ex[1 + i]}, mid, enclosing_fn,
           file_local, bs, arm_scope);
    walk(s, ast, (AstNodeId){.idx = ex[1 + pc]}, mid, enclosing_fn, file_local,
         bs, arm_scope);
    return;
  }

  default:
    // Transparent recursion: switch scrutinee, return operand, defer
    // body, expression subtrees, etc. Children inherit current_scope
    // so their nodes get tagged correctly.
    walk_children(s, ast, node, mid, enclosing_fn, file_local, bs,
                  current_scope);
    return;
  }
}

// === Lookup =================================================================

IpIndex sema_body_scope_lookup(BodyScopes *bs, AstNodeId use_node, StrId name) {
  if (!bs || name.idx == 0 || use_node.idx == AST_NODE_ID_NONE.idx)
    return IP_NONE;
  if (use_node.idx < bs->body_root_min)
    return IP_NONE;
  uint32_t off = use_node.idx - bs->body_root_min;
  if (off >= bs->node_to_scope_count)
    return IP_NONE;

  uint32_t scope = bs->node_to_scope[off];
  ScopeRow *rows = (ScopeRow *)bs->scopes.data;
  ScopedBind *binds = (ScopedBind *)bs->binds.data;

  // Walk from the use-site scope outward. Within each scope, scan
  // forward (binds appended in source order) — latest match wins,
  // implementing shadowing.
  while (scope != BODY_SCOPE_NONE) {
    IpIndex found = IP_NONE;
    for (size_t i = 0; i < bs->binds.count; i++) {
      if (binds[i].scope_id == scope && binds[i].name.idx == name.idx)
        found = binds[i].type;
    }
    if (found.v != IP_NONE.v)
      return found;
    if (scope >= bs->scopes.count)
      return IP_NONE;
    scope = rows[scope].parent;
  }
  return IP_NONE;
}

BodyScopes *sema_body_scopes_get(struct db *s, DefId fn_def) {
  if (fn_def.idx == DEF_ID_NONE.idx)
    return NULL;
  if (fn_def.idx >= s->defs.body_scopes.count)
    return NULL;
  return *(BodyScopes **)vec_get(&s->defs.body_scopes, fn_def.idx);
}

// === Builder ================================================================

BodyScopes *sema_body_scopes(struct db *s, DefId fn_def) {
  AstId ast_id = *(AstId *)vec_get(&s->defs.ast_ids, fn_def.idx);
  ModuleId mid = *(ModuleId *)vec_get(&s->defs.parent_modules, fn_def.idx);

  (void)db_query_def_identity(s, mid, ast_id);
  IpIndex sig = db_query_fn_signature(s, fn_def);
  if (sig.v == IP_NONE.v)
    return NULL;

  // Locate the lambda AST node. db_query_file_ast records the per-file
  // AST dep — an edit anywhere in a module file invalidates this query.
  ASTStore *ast = NULL;
  AstNodeId lambda_node = AST_NODE_ID_NONE;
  uint32_t body_file_local = 0;
  uint32_t fc = 0;
  const FileId *files = db_module_files(s, mid, &fc);
  for (uint32_t i = 0; i < fc; i++) {
    (void)db_query_file_ast(s, files[i]);
    uint32_t local = file_id_local(files[i]);
    struct AstIdMap *map =
        *(struct AstIdMap **)vec_get(&s->files.ast_id_maps, local);
    if (!map)
      continue;
    AstNodeId node = ast_id_map_get(map, ast_id);
    if (node.idx == AST_NODE_ID_NONE.idx)
      continue;

    ASTStore *cand_ast = *(ASTStore **)vec_get(&s->files.asts, local);
    AstNodeKind dk = ((AstNodeKind *)cand_ast->kinds.data)[node.idx];
    if (dk != AST_DECL_CONST && dk != AST_DECL_VAR)
      break;

    AstNodeData d = ((AstNodeData *)cand_ast->data.data)[node.idx];
    const uint32_t *ex = &((uint32_t *)cand_ast->extra.data)[d.extra_idx.idx];
    AstNodeId value_id = {.idx = ex[2]};
    if (value_id.idx == AST_NODE_ID_NONE.idx)
      break;
    AstNodeKind vk = ((AstNodeKind *)cand_ast->kinds.data)[value_id.idx];
    if (vk != AST_EXPR_LAMBDA)
      break;

    ast = cand_ast;
    lambda_node = value_id;
    body_file_local = local;
    break;
  }

  if (!ast || lambda_node.idx == AST_NODE_ID_NONE.idx)
    return NULL;

  // Lazy-init the BodyScopes slot. Struct is arena-allocated (pointer-
  // stable for db lifetime); internal Vecs/array are malloc-owned and
  // get cleared + repopulated on re-runs. db_free does the final teardown.
  BodyScopes **bs_slot =
      (BodyScopes **)vec_get(&s->defs.body_scopes, fn_def.idx);
  if (!*bs_slot) {
    BodyScopes *fresh = (BodyScopes *)arena_alloc(&s->arena, sizeof(BodyScopes));
    vec_init(&fresh->scopes, sizeof(ScopeRow));
    vec_init(&fresh->binds, sizeof(ScopedBind));
    fresh->node_to_scope = NULL;
    fresh->node_to_scope_count = 0;
    fresh->body_root_min = 0;
    *bs_slot = fresh;
  } else {
    vec_clear(&(*bs_slot)->scopes);
    vec_clear(&(*bs_slot)->binds);
    // node_to_scope realloc'd below as needed.
  }
  BodyScopes *bs = *bs_slot;

  // Lambda extras: [ret_id, body_id, effect_id, param_count, p0, ...].
  AstNodeData ld = ((AstNodeData *)ast->data.data)[lambda_node.idx];
  const uint32_t *lex = &((uint32_t *)ast->extra.data)[ld.extra_idx.idx];
  uint32_t n_ast_params = lex[3];
  const uint32_t *param_ids = &lex[4];
  AstNodeId body_node = {.idx = lex[1]};

  // Pass 1: find the body's node-id span so node_to_scope is sized
  // exactly. The lambda itself isn't in the body — only its body
  // subtree.
  uint32_t body_min = 0, body_max = 0;
  if (body_node.idx != AST_NODE_ID_NONE.idx) {
    find_body_range(ast, body_node, &body_min, &body_max);
  }

  uint32_t span = (body_node.idx == AST_NODE_ID_NONE.idx)
                      ? 0
                      : (body_max - body_min + 1);
  if (span != bs->node_to_scope_count) {
    free(bs->node_to_scope);
    bs->node_to_scope = span ? (uint32_t *)malloc(span * sizeof(uint32_t)) : NULL;
    bs->node_to_scope_count = span;
  }
  for (uint32_t i = 0; i < span; i++)
    bs->node_to_scope[i] = BODY_SCOPE_NONE;
  bs->body_root_min = body_min;

  // Root scope holds params. Even when there are no params the root
  // scope must exist (it's the parent of every block opened below).
  uint32_t root = scope_push(bs, BODY_SCOPE_NONE, lambda_node);

  IpKey sig_key = ip_key(&s->intern, sig);
  const IpIndex *sig_params = sig_key.fn_type.params;
  size_t n_sig_params = sig_key.fn_type.n_params;
  uint32_t n_params = (uint32_t)((n_ast_params < n_sig_params) ? n_ast_params
                                                               : n_sig_params);
  for (uint32_t i = 0; i < n_params; i++) {
    AstNodeId pid = {.idx = param_ids[i]};
    if (pid.idx == AST_NODE_ID_NONE.idx)
      continue;
    AstNodeKind pk = ((AstNodeKind *)ast->kinds.data)[pid.idx];
    if (pk != AST_DECL_PARAM)
      continue;
    AstNodeData pd = ((AstNodeData *)ast->data.data)[pid.idx];
    const uint32_t *pex = &((uint32_t *)ast->extra.data)[pd.extra_idx.idx];
    StrId pname = {.idx = pex[0]};
    if (pname.idx == 0)
      continue;
    IpIndex pty = sig_params[i];
    if (pty.v == IP_NONE.v)
      continue;
    bind_push(bs, root, pname, pty);
  }

  // Pass 2: build scope tree, tag body nodes, push body let-binds.
  if (body_node.idx != AST_NODE_ID_NONE.idx)
    walk(s, ast, body_node, mid, fn_def, body_file_local, bs, root);

  return bs;
}
