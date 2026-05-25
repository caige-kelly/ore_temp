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

// Exported via sema.h — reused by db_query_infer_body to size its
// NodeTypeBuilder's range over the body's AST sub-tree. Walks every
// descendant of `root` and records the min/max AstNodeId.idx seen.
// (For an empty / NONE root, both out params are zeroed.)
void sema_ast_subtree_range(ASTStore *ast, AstNodeId root, uint32_t *out_min,
                            uint32_t *out_max) {
  if (root.idx == AST_NODE_ID_NONE.idx) {
    *out_min = 0;
    *out_max = 0;
    return;
  }
  RangeCtx ctx = {.ast = ast, .min = root.idx, .max = root.idx};
  ast_visit_children(ast, root, range_visit, &ctx);
  *out_min = ctx.min;
  *out_max = ctx.max;
}

static void find_body_range(ASTStore *ast, AstNodeId body, uint32_t *out_min,
                            uint32_t *out_max) {
  sema_ast_subtree_range(ast, body, out_min, out_max);
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
static void walk(const SemaCtx *ctx, AstNodeId node, BodyScopeBuilder *b,
                 uint32_t current_scope);

// Walk callback context — embeds the SemaCtx (type-resolution state)
// plus the body-scope-specific fields (BodyScopeBuilder + current
// scope ID). The first field is the SemaCtx so any future code that
// wants to view a WalkCtx as a SemaCtx can do so via `&wctx->sema`.
typedef struct {
  SemaCtx sema;
  BodyScopeBuilder *b;
  uint32_t scope;
} WalkCtx;

static void walk_child(AstNodeId child, void *ud) {
  WalkCtx *wctx = (WalkCtx *)ud;
  walk(&wctx->sema, child, wctx->b, wctx->scope);
}

static void walk_children(const SemaCtx *ctx, AstNodeId node,
                          BodyScopeBuilder *b, uint32_t scope) {
  WalkCtx wctx = {.sema = *ctx, .b = b, .scope = scope};
  ast_visit_children(ctx->ast, node, walk_child, &wctx);
}

// Type a let-bind RHS or annotation. Annotation wins when present;
// when both annotation and value are given, the value is checked
// against the annotation (emits "expected {0}" on mismatch via
// sema_check_expr's coercion path).
static IpIndex type_of_bind(const SemaCtx *ctx, AstNodeId type_id,
                            AstNodeId value_id) {
  if (type_id.idx != AST_NODE_ID_NONE.idx) {
    IpIndex annotated = sema_resolve_type_expr(ctx, type_id);
    if (annotated.v != IP_NONE.v && value_id.idx != AST_NODE_ID_NONE.idx) {
      (void)sema_check_expr(ctx, value_id, annotated);
    }
    return annotated;
  }
  if (value_id.idx != AST_NODE_ID_NONE.idx)
    return sema_type_of_expr(ctx, value_id);
  return IP_NONE;
}

static void walk(const SemaCtx *ctx, AstNodeId node, BodyScopeBuilder *b,
                 uint32_t current_scope) {
  if (node.idx == AST_NODE_ID_NONE.idx)
    return;

  tag_node(b, node, current_scope);

  struct db *s = ctx->s;
  ASTStore *ast = ctx->ast;
  FileId file_local = ctx->file_local;
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
      walk(ctx, stmt, b, child);
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
    walk(ctx, type_id, b, current_scope);
    walk(ctx, value_id, b, current_scope);

    if (name.idx != 0) {
      // ALWAYS push the bind, even when the RHS type is IP_NONE. A
      // failed type-of-RHS (e.g., an unimplemented builtin like
      // @ptrCast, an anytype-returning call) is independent of the
      // binding's existence: the name IS declared, just with an
      // unknown type. Skipping the push here used to make the loop
      // body emit "undefined identifier 'base'" — a misleading diag
      // since `base := @ptrCast(...)` syntactically declares it.
      //
      // sema_body_scope_lookup uses a separate `found` flag now so
      // downstream type_of_expr resolutions can distinguish "found,
      // type unknown" from "name truly undefined". The actual
      // upstream root cause (missing @ptrCast / @sizeOf) is a
      // separate sema gap that surfaces its own diagnostic at the
      // call site, not at every subsequent use.
      IpIndex t = type_of_bind(ctx, type_id, value_id);
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
      walk(ctx, rhs_id, b, current_scope);

      uint32_t then_scope = scope_push(b, current_scope, cond_id);
      tag_node(b, cond_id, then_scope);

      if (name.idx != 0 && rhs_id.idx != AST_NODE_ID_NONE.idx) {
        IpIndex rhs_t = sema_type_of_expr(ctx, rhs_id);
        if (rhs_t.v != IP_NONE.v) {
          if (ip_tag(&s->intern, rhs_t) == IP_TAG_OPTIONAL_TYPE) {
            IpKey ik = ip_key(&s->intern, rhs_t);
            bind_push(b, then_scope, name, ik.optional_type.elem);
          } else {
            AstSpan span = astspan_make(file_local, cond_id);
            if (!astspan_is_none(span)) {
              db_emit(s, DIAG_ERROR, span,
                      "if-let pattern requires optional type, got %T", rhs_t);
            }
          }
        }
      }

      walk(ctx, then_id, b, then_scope);

      uint32_t else_scope = scope_push(b, current_scope, else_id);
      walk(ctx, else_id, b, else_scope);
      return;
    }

    walk(ctx, cond_id, b, current_scope);
    uint32_t then_scope = scope_push(b, current_scope, then_id);
    walk(ctx, then_id, b, then_scope);
    uint32_t else_scope = scope_push(b, current_scope, else_id);
    walk(ctx, else_id, b, else_scope);
    return;
  }

  case AST_STMT_LOOP: {
    // Extras: [label, init, cond, step, body]. The C-style
    // `loop (i := 0; ...)` puts the bind in `init` — it must scope
    // to the loop, not leak out. So we open a loop scope here and all
    // four slots run inside it.
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    uint32_t loop_scope = scope_push(b, current_scope, node);
    walk(ctx, (AstNodeId){.idx = ex[1]}, b, loop_scope);
    walk(ctx, (AstNodeId){.idx = ex[2]}, b, loop_scope);
    walk(ctx, (AstNodeId){.idx = ex[3]}, b, loop_scope);
    walk(ctx, (AstNodeId){.idx = ex[4]}, b, loop_scope);
    return;
  }

  case AST_STMT_SWITCH_ARM: {
    // Extras: [pat_count, pat0..N, body]. Pattern binds (future) would
    // land in this arm-scope; for now we just isolate the arm body.
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    uint32_t pc = ex[0];
    uint32_t arm_scope = scope_push(b, current_scope, node);
    for (uint32_t i = 0; i < pc; i++)
      walk(ctx, (AstNodeId){.idx = ex[1 + i]}, b, arm_scope);
    walk(ctx, (AstNodeId){.idx = ex[1 + pc]}, b, arm_scope);
    return;
  }

  default:
    // Transparent recursion: switch scrutinee, return operand, defer
    // body, expression subtrees, etc. Children inherit current_scope
    // so their nodes get tagged correctly.
    walk_children(ctx, node, b, current_scope);
    return;
  }
}

// === Lookup =================================================================

IpIndex sema_body_scope_lookup(struct db *s, DefId fn_def, AstNodeId use_node,
                               StrId name, bool *found_out) {
  if (found_out)
    *found_out = false;
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
  //
  // `seen` tracks "did we find any bind for this name in this scope".
  // Decoupled from the type value because a bind whose RHS didn't
  // type still EXISTS — its type is just IP_NONE. Walking past such a
  // bind would mask shadowing and let an outer-scope bind win
  // incorrectly; stopping at the IP_NONE-typed bind tells the caller
  // "yes this is defined, just unknown" (caller can suppress
  // undefined-identifier diags).
  uint32_t scope = n2s[fb.n2s_off + off];
  while (scope != BODY_SCOPE_NONE) {
    IpIndex found = IP_NONE;
    bool seen = false;
    for (uint32_t i = 0; i < fb.bind_len; i++) {
      const ScopedBind *bd = &binds[fb.bind_off + i];
      if (bd->scope_id == scope && bd->name.idx == name.idx) {
        found = bd->type;
        seen = true;
      }
    }
    if (seen) {
      if (found_out)
        *found_out = true;
      return found;
    }
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
  NamespaceId nsid =
      *(NamespaceId *)vec_get(&s->defs.parent_modules, fn_def.idx);

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

  // node_to_scope is sized to cover the LAMBDA's full subtree — params +
  // return-type + body — so signature-position nodes (param name tokens,
  // type-expr identifiers inside params/ret) also map to a scope. The
  // root scope holds the param binds, so a body_scope_lookup at a
  // signature-position node resolves sibling-param names. Pre-emptive
  // for `fn(x: i32, y: typeof(x))` shapes; today it just means hover at
  // a param name in the signature dispatches through the same body-
  // scope path as the body case.
  uint32_t lambda_min = 0, lambda_max = 0;
  find_body_range(ast, lambda_node, &lambda_min, &lambda_max);
  // Include the lambda node itself in the range (find_body_range seeds
  // min/max with the root's own idx so this is already satisfied, but
  // be explicit for the reader).
  if (lambda_node.idx < lambda_min)
    lambda_min = lambda_node.idx;
  if (lambda_node.idx > lambda_max)
    lambda_max = lambda_node.idx;
  uint32_t body_min = lambda_min;
  uint32_t span = lambda_max - lambda_min + 1;

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

  // Tag the lambda + its signature-position sub-trees (params, return-
  // type) into the root scope so body_scope_lookup at any signature-
  // position node resolves through the root's binds. Without this,
  // signature-position nodes have node_to_scope = BODY_SCOPE_NONE and
  // the lookup-walk falls off immediately. (Body nodes get tagged
  // below by `walk`.)
  tag_node(&b, lambda_node, root);
  AstNodeId ret_node = {.idx = lex[0]};
  // Build a SemaCtx for the walk. body_scopes doesn't open its own
  // NodeTypeBuilder — the per-decl queries (infer_body / fn_signature)
  // own those — so .types = NULL. type_of_bind's recursive
  // sema_resolve_type_expr / sema_type_of_expr calls land outside any
  // active builder, which is correct: those values will be re-typed
  // and cached by infer_body's walk on its own pass.
  SemaCtx walk_ctx = {
      .s = s,
      .ast = ast,
      .nsid = nsid,
      .enclosing_fn = fn_def,
      .file_local = body_fid,
      .types = NULL,
  };
  // Visit each param + the return-type expr to tag every descendant
  // with the root scope. walk's default case is transparent recursion
  // (tag_node + recurse into children), exactly what we want here —
  // no scope-opening, no bind-pushing for param subtrees.
  for (uint32_t i = 0; i < n_ast_params; i++) {
    AstNodeId pid = {.idx = param_ids[i]};
    if (pid.idx == AST_NODE_ID_NONE.idx)
      continue;
    walk(&walk_ctx, pid, &b, root);
  }
  if (ret_node.idx != AST_NODE_ID_NONE.idx)
    walk(&walk_ctx, ret_node, &b, root);

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
    walk(&walk_ctx, body_node, &b, root);

  return *bb_fnbody(&b);
}
