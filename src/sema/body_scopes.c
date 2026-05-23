#include "../db/db.h"
#include "../db/diag/diag.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/decl_ast.h"
#include "../db/query/def_identity.h"
#include "../db/query/fn_signature.h"
#include "../db/query/index.h"
#include "../parser/ast.h"
#include "sema.h"

// Builds the per-fn body scope tree. Rust-analyzer's ExprScopes pattern
// adapted to AstNodeId (no HIR layer). The data is stored flat in three
// shared db pools — db.body_scope_rows / db.body_scope_binds /
// db.node_to_scope — and db.fns.body[row] records this fn's (off,len)
// ranges into them (a FnBody). Scope ids are fn-LOCAL (0-based), so the
// fn's slice is self-contained and the pools are append-only.
//
// Walk-time scope-pushing rules:
//   AST_STMT_BLOCK              opens new scope (parent = current)
//   AST_STMT_IF then-branch     opens new scope; if-let bind lands here
//   AST_STMT_IF else-branch     opens new scope (no if-let bind)
//   AST_STMT_LOOP               opens new scope (init/cond/step/body)
//   AST_STMT_SWITCH_ARM         opens new scope (future: pattern binds)
//   AST_DECL_VAR/CONST stmt     pushes a bind into the *current* scope
//
// Re-entrancy: typing a let-bind's RHS may call sema_type_of_expr, whose
// PATH lookup calls sema_body_scope_lookup(s, def, ...) for this same fn.
// db.fns.body[row] is published with the live offsets before the walk and
// its scope_len/bind_len grow on every push, so those re-entrant lookups
// see correct partial state. INVARIANT: the walk must not trigger another
// fn's body_scopes build — the shared pools assume one build in flight.

typedef struct {
  ASTStore *ast;
  uint32_t min;
  uint32_t max;
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

// Build-time context for one fn's body-scope construction. scope/bind/n2s
// _off are this fn's base offsets into the shared pools; scope ids handed
// out are fn-LOCAL (0-based).
typedef struct {
  struct db *s;
  uint32_t fn_row;    // this fn's row in db.fns
  uint32_t scope_off; // base in db.body_scope_rows
  uint32_t bind_off;  // base in db.body_scope_binds
  uint32_t n2s_off;   // base in db.node_to_scope
  uint32_t n2s_count; // size of this fn's node_to_scope slice
  uint32_t body_root_min;
} BodyScopeBuilder;

// Re-fetch this fn's FnBody cell. NOT cached across calls: db.fns can
// realloc when the walk classifies another def (db_def_set_kind).
static FnBody *bb_fnbody(BodyScopeBuilder *b) {
  return (FnBody *)vec_get(&b->s->fns.body, b->fn_row);
}

static uint32_t scope_push(BodyScopeBuilder *b, uint32_t parent,
                           AstNodeId block_node) {
  uint32_t id = (uint32_t)b->s->body_scope_rows.count - b->scope_off;
  ScopeRow row = {.parent = parent, .block_node = block_node};
  vec_push(&b->s->body_scope_rows, &row);
  bb_fnbody(b)->scope_len =
      (uint32_t)b->s->body_scope_rows.count - b->scope_off;
  return id;
}

static void bind_push(BodyScopeBuilder *b, uint32_t scope_id, StrId name,
                      IpIndex type) {
  ScopedBind sb = {.scope_id = scope_id, .name = name, .type = type};
  vec_push(&b->s->body_scope_binds, &sb);
  bb_fnbody(b)->bind_len = (uint32_t)b->s->body_scope_binds.count - b->bind_off;
}

static void tag_node(BodyScopeBuilder *b, AstNodeId node, uint32_t scope_id) {
  if (node.idx == AST_NODE_ID_NONE.idx)
    return;
  if (node.idx < b->body_root_min)
    return;
  uint32_t off = node.idx - b->body_root_min;
  if (off >= b->n2s_count)
    return;
  // node_to_scope does not grow during the walk (the slice was appended
  // up front), so its data pointer is stable here.
  uint32_t *n2s = (uint32_t *)b->s->node_to_scope.data;
  n2s[b->n2s_off + off] = scope_id;
}

// Forward declaration for recursive walk.
static void walk(struct db *s, ASTStore *ast, AstNodeId node, NamespaceId nsid,
                 DefId enclosing_fn, FileId file_local, BodyScopeBuilder *b,
                 uint32_t current_scope);

// Recurse into all children with the same `current_scope`. Used for
// expression subtrees and other "transparent" forms that don't open
// scopes themselves but may contain nodes we need to tag.
typedef struct {
  struct db *s;
  ASTStore *ast;
  NamespaceId nsid;
  DefId enclosing_fn;
  FileId file_local;
  BodyScopeBuilder *b;
  uint32_t scope;
} WalkCtx;

static void walk_child(AstNodeId child, void *ud) {
  WalkCtx *ctx = (WalkCtx *)ud;
  walk(ctx->s, ctx->ast, child, ctx->nsid, ctx->enclosing_fn, ctx->file_local,
       ctx->b, ctx->scope);
}

static void walk_children(struct db *s, ASTStore *ast, AstNodeId node,
                          NamespaceId nsid, DefId enclosing_fn, FileId file_local,
                          BodyScopeBuilder *b, uint32_t scope) {
  WalkCtx ctx = {.s = s,
                 .ast = ast,
                 .nsid = nsid,
                 .enclosing_fn = enclosing_fn,
                 .file_local = file_local,
                 .b = b,
                 .scope = scope};
  ast_visit_children(ast, node, walk_child, &ctx);
}

// Type a let-bind RHS or annotation. Annotation wins when present.
static IpIndex type_of_bind(struct db *s, ASTStore *ast, AstNodeId type_id,
                            AstNodeId value_id, NamespaceId nsid, DefId fn,
                            FileId file_local) {
  if (type_id.idx != AST_NODE_ID_NONE.idx)
    return sema_resolve_type_expr(s, ast, type_id, nsid);
  if (value_id.idx != AST_NODE_ID_NONE.idx)
    return sema_type_of_expr(s, ast, value_id, nsid, fn, file_local);
  return IP_NONE;
}

static void walk(struct db *s, ASTStore *ast, AstNodeId node, NamespaceId nsid,
                 DefId enclosing_fn, FileId file_local, BodyScopeBuilder *b,
                 uint32_t current_scope) {
  if (node.idx == AST_NODE_ID_NONE.idx)
    return;

  tag_node(b, node, current_scope);

  AstNodeKind k = ((AstNodeKind *)ast->kinds.data)[node.idx];
  AstNodeData d = ((AstNodeData *)ast->data.data)[node.idx];

  switch (k) {
  case AST_STMT_BLOCK: {
    // New scope. Extras: [stmt_count, s0, s1, ...].
    uint32_t child = scope_push(b, current_scope, node);
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    uint32_t count = ex[0];
    for (uint32_t i = 0; i < count; i++) {
      AstNodeId stmt = {.idx = ex[1 + i]};
      walk(s, ast, stmt, nsid, enclosing_fn, file_local, b, child);
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
    // sema_type_of_expr → sema_body_scope_lookup.
    walk(s, ast, type_id, nsid, enclosing_fn, file_local, b, current_scope);
    walk(s, ast, value_id, nsid, enclosing_fn, file_local, b, current_scope);

    if (name.idx != 0) {
      IpIndex t = type_of_bind(s, ast, type_id, value_id, nsid, enclosing_fn,
                               file_local);
      if (t.v != IP_NONE.v)
        bind_push(b, current_scope, name, t);
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
      walk(s, ast, rhs_id, nsid, enclosing_fn, file_local, b, current_scope);

      uint32_t then_scope = scope_push(b, current_scope, cond_id);
      tag_node(b, cond_id, then_scope);

      if (name.idx != 0 && rhs_id.idx != AST_NODE_ID_NONE.idx) {
        IpIndex rhs_t =
            sema_type_of_expr(s, ast, rhs_id, nsid, enclosing_fn, file_local);
        if (rhs_t.v != IP_NONE.v) {
          if (ip_tag(&s->intern, rhs_t) == IP_TAG_OPTIONAL_TYPE) {
            IpKey ik = ip_key(&s->intern, rhs_t);
            bind_push(b, then_scope, name, ik.optional_type.elem);
          } else {
            TinySpan span = db_get_node_span(s, file_local, cond_id);
            if (span != TINYSPAN_NONE) {
              db_emit_error_t(s, span,
                              "if-let pattern requires optional type, got {0}",
                              rhs_t);
            }
          }
        }
      }

      walk(s, ast, then_id, nsid, enclosing_fn, file_local, b, then_scope);

      uint32_t else_scope = scope_push(b, current_scope, else_id);
      walk(s, ast, else_id, nsid, enclosing_fn, file_local, b, else_scope);
      return;
    }

    walk(s, ast, cond_id, nsid, enclosing_fn, file_local, b, current_scope);
    uint32_t then_scope = scope_push(b, current_scope, then_id);
    walk(s, ast, then_id, nsid, enclosing_fn, file_local, b, then_scope);
    uint32_t else_scope = scope_push(b, current_scope, else_id);
    walk(s, ast, else_id, nsid, enclosing_fn, file_local, b, else_scope);
    return;
  }

  case AST_STMT_LOOP: {
    // Extras: [label, init, cond, step, body]. The C-style
    // `loop (i := 0; ...)` puts the bind in `init` — it must scope
    // to the loop, not leak out. So we open a loop scope here and all
    // four slots run inside it.
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    uint32_t loop_scope = scope_push(b, current_scope, node);
    walk(s, ast, (AstNodeId){.idx = ex[1]}, nsid, enclosing_fn, file_local, b,
         loop_scope);
    walk(s, ast, (AstNodeId){.idx = ex[2]}, nsid, enclosing_fn, file_local, b,
         loop_scope);
    walk(s, ast, (AstNodeId){.idx = ex[3]}, nsid, enclosing_fn, file_local, b,
         loop_scope);
    walk(s, ast, (AstNodeId){.idx = ex[4]}, nsid, enclosing_fn, file_local, b,
         loop_scope);
    return;
  }

  case AST_STMT_SWITCH_ARM: {
    // Extras: [pat_count, pat0..N, body]. Pattern binds (future) would
    // land in this arm-scope; for now we just isolate the arm body.
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    uint32_t pc = ex[0];
    uint32_t arm_scope = scope_push(b, current_scope, node);
    for (uint32_t i = 0; i < pc; i++)
      walk(s, ast, (AstNodeId){.idx = ex[1 + i]}, nsid, enclosing_fn, file_local,
           b, arm_scope);
    walk(s, ast, (AstNodeId){.idx = ex[1 + pc]}, nsid, enclosing_fn, file_local,
         b, arm_scope);
    return;
  }

  default:
    // Transparent recursion: switch scrutinee, return operand, defer
    // body, expression subtrees, etc. Children inherit current_scope
    // so their nodes get tagged correctly.
    walk_children(s, ast, node, nsid, enclosing_fn, file_local, b,
                  current_scope);
    return;
  }
}

// === Lookup =================================================================

IpIndex sema_body_scope_lookup(struct db *s, DefId fn_def, AstNodeId use_node,
                               StrId name) {
  if (fn_def.idx == DEF_ID_NONE.idx || name.idx == 0 ||
      use_node.idx == AST_NODE_ID_NONE.idx)
    return IP_NONE;
  if (db_def_kind(s, fn_def) != KIND_FUNCTION)
    return IP_NONE;

  FnBody fb =
      *(FnBody *)vec_get(&s->fns.body, db_def_row(s, fn_def, KIND_FUNCTION));
  if (use_node.idx < fb.body_root_min)
    return IP_NONE;
  uint32_t off = use_node.idx - fb.body_root_min;
  if (off >= fb.n2s_len)
    return IP_NONE;

  const uint32_t *n2s = (const uint32_t *)s->node_to_scope.data;
  const ScopeRow *rows = (const ScopeRow *)s->body_scope_rows.data;
  const ScopedBind *binds = (const ScopedBind *)s->body_scope_binds.data;

  // Walk from the use-site scope outward. Within each scope, scan
  // forward (binds appended in source order) — latest match wins,
  // implementing shadowing. Scope ids are fn-local: index the fn's
  // slice via fb.scope_off / fb.bind_off.
  uint32_t scope = n2s[fb.n2s_off + off];
  while (scope != BODY_SCOPE_NONE) {
    IpIndex found = IP_NONE;
    for (uint32_t i = 0; i < fb.bind_len; i++) {
      const ScopedBind *bd = &binds[fb.bind_off + i];
      if (bd->scope_id == scope && bd->name.idx == name.idx)
        found = bd->type;
    }
    if (found.v != IP_NONE.v)
      return found;
    if (scope >= fb.scope_len)
      return IP_NONE;
    scope = rows[fb.scope_off + scope].parent;
  }
  return IP_NONE;
}

// === Builder ================================================================

FnBody sema_body_scopes(struct db *s, DefId fn_def) {
  FnBody empty = {0};

  AstId ast_id = *(AstId *)vec_get(&s->defs.ast_ids, fn_def.idx);
  NamespaceId nsid = *(NamespaceId *)vec_get(&s->defs.parent_modules, fn_def.idx);

  // Depend on the module's top-level index — this body reads the
  // module's file list below; a file-set change must re-run it.
  (void)db_query_top_level_index(s, nsid);
  (void)db_query_def_identity(s, nsid, ast_id);
  IpIndex sig = db_query_fn_signature(s, fn_def);
  if (sig.v == IP_NONE.v)
    return empty;

  // Locate the lambda AST node via the per-decl AST query — its
  // structural fingerprint is what makes a sibling edit early-cut this
  // query rather than re-running the scope build.
  ASTStore *ast = NULL;
  AstNodeId lambda_node = AST_NODE_ID_NONE;
  FileId body_fid = FILE_ID_NONE;
  uint32_t fc = 0;
  const FileId *files = db_get_namespace_files(s, nsid, &fc);
  for (uint32_t i = 0; i < fc; i++) {
    AstNodeId node = db_query_decl_ast(s, files[i], ast_id);
    if (node.idx == AST_NODE_ID_NONE.idx)
      continue;

    ASTStore *cand_ast = db_get_file_ast(s, files[i]);
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
    body_fid = files[i];
    break;
  }

  if (!ast || lambda_node.idx == AST_NODE_ID_NONE.idx)
    return empty;

  // Lambda extras: [ret_id, body_id, effect_id, param_count, p0, ...].
  AstNodeData ld = ((AstNodeData *)ast->data.data)[lambda_node.idx];
  const uint32_t *lex = &((uint32_t *)ast->extra.data)[ld.extra_idx.idx];
  uint32_t n_ast_params = lex[3];
  const uint32_t *param_ids = &lex[4];
  AstNodeId body_node = {.idx = lex[1]};

  // Pass 1: find the body's node-id span so node_to_scope is sized
  // exactly. The lambda itself isn't in the body — only its subtree.
  uint32_t body_min = 0, body_max = 0;
  if (body_node.idx != AST_NODE_ID_NONE.idx)
    find_body_range(ast, body_node, &body_min, &body_max);
  uint32_t span =
      (body_node.idx == AST_NODE_ID_NONE.idx) ? 0 : (body_max - body_min + 1);

  // This fn's slices begin at the current pool tails (append-only).
  BodyScopeBuilder b = {
      .s = s,
      .fn_row = db_def_row(s, fn_def, KIND_FUNCTION),
      .scope_off = (uint32_t)s->body_scope_rows.count,
      .bind_off = (uint32_t)s->body_scope_binds.count,
      .n2s_off = (uint32_t)s->node_to_scope.count,
      .n2s_count = span,
      .body_root_min = body_min,
  };
  // Append + zero-fill this fn's node_to_scope slice.
  for (uint32_t i = 0; i < span; i++) {
    uint32_t none = BODY_SCOPE_NONE;
    vec_push(&s->node_to_scope, &none);
  }
  // Publish the (so-far-empty) ranges before walking so re-entrant
  // lookups see correct offsets; scope_len/bind_len grow on each push.
  *bb_fnbody(&b) = (FnBody){.scope_off = b.scope_off,
                            .scope_len = 0,
                            .bind_off = b.bind_off,
                            .bind_len = 0,
                            .n2s_off = b.n2s_off,
                            .n2s_len = span,
                            .body_root_min = body_min};

  // Root scope holds params. Even with no params the root scope must
  // exist (it's the parent of every block opened below).
  uint32_t root = scope_push(&b, BODY_SCOPE_NONE, lambda_node);

  IpKey sig_key = ip_key(&s->intern, sig);
  const IpIndex *sig_params = sig_key.fn_type.params;
  size_t n_sig_params = sig_key.fn_type.n_params;
  uint32_t n_params =
      (uint32_t)((n_ast_params < n_sig_params) ? n_ast_params : n_sig_params);
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
    bind_push(&b, root, pname, pty);
  }

  // Pass 2: build scope tree, tag body nodes, push body let-binds.
  if (body_node.idx != AST_NODE_ID_NONE.idx)
    walk(s, ast, body_node, nsid, fn_def, body_fid, &b, root);

  return *bb_fnbody(&b);
}
