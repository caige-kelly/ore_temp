#include "../ast/ast_decl.h"
#include "../ast/ast_expr.h"
#include "../ast/ast_stmt.h"
#include "../db/db.h"
#include "../db/diag/diag.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/decl_ast.h"
#include "../db/query/def_identity.h"
#include "../db/query/fn_signature.h"
#include "../db/query/index.h"
#include "../parser/syntax_kind.h"
#include "../support/data_structure/hashmap.h"
#include "../support/data_structure/stringpool.h"
#include "../syntax/syntax.h"
#include "sema.h"

// Builds the per-fn body scope tree. Rust-analyzer's ExprScopes pattern
// adapted to SyntaxNode + SyntaxNodePtr. The tree itself lives in two
// shared pools (db.body_scope_rows / _binds) sliced by FnBody (off,len)
// pairs; the per-node → scope mapping is owned per-fn in db.fns.scope_map
// (HashMap<syntax_node_ptr_hash, ScopeId-as-pointer>). Scope ids are
// fn-LOCAL (0-based), so the fn's slice is self-contained and the pools
// stay append-only.
//
// Walk-time scope-pushing rules:
//   SK_BLOCK_STMT               opens new scope (parent = current)
//   SK_IF_EXPR then-branch      opens new scope; if-let bind lands here
//   SK_IF_EXPR else-branch      opens new scope (no if-let bind)
//   SK_LOOP_EXPR                opens new scope (body)
//   SK_SWITCH_ARM               opens new scope (future: pattern binds)
//   SK_CONST_DECL/SK_VAR_DECL   pushes a bind into the *current* scope
//                               (statement-position let-bind)
//
// Re-entrancy: typing a let-bind RHS may call sema_type_of_expr, whose
// REF lookup calls sema_body_scope_lookup(s, def, ...) for this same fn.
// db.fns.body[row] is published with the live offsets before the walk
// and its scope_len/bind_len grow on every push, so those re-entrant
// lookups see correct partial state. INVARIANT: the walk must not
// trigger another fn's body_scopes build — the shared pools assume one
// build in flight.

// === Builder state ==========================================================

typedef struct {
  struct db *s;
  uint32_t   fn_row;      // this fn's row in db.fns
  uint32_t   scope_off;   // base in db.body_scope_rows
  uint32_t   bind_off;    // base in db.body_scope_binds
  HashMap   *scope_map;   // db.fns.scope_map[fn_row], owned by db
} BodyScopeBuilder;

// Re-fetch this fn's FnBody cell. NOT cached across calls: db.fns can
// realloc when the walk classifies another def (db_def_set_kind).
static FnBody *bb_fnbody(BodyScopeBuilder *b) {
  return (FnBody *)vec_get(&b->s->fns.body, b->fn_row);
}

static uint32_t scope_push(BodyScopeBuilder *b, uint32_t parent,
                           SyntaxNode *block_node) {
  uint32_t id = (uint32_t)b->s->body_scope_rows.count - b->scope_off;
  SyntaxNodePtr bp = block_node ? syntax_node_ptr_new(block_node)
                                : (SyntaxNodePtr){0};
  ScopeRow row = {.parent = parent, .block_node = bp};
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

// Map node → scope. `node` is BORROWED — caller manages its lifetime.
// Storing only the hash means the live map keys survive even when the
// underlying SyntaxNode handle is released by the walk.
static void tag_node(BodyScopeBuilder *b, SyntaxNode *node, uint32_t scope_id) {
  if (!node || !b->scope_map)
    return;
  uint64_t key = syntax_node_ptr_hash(syntax_node_ptr_new(node));
  // Encode scope_id+1 into the void* slot so a successful get() yields
  // a non-NULL pointer even for scope_id == 0 (the fn-root scope is
  // the most common one). hashmap_get returns NULL for misses, which
  // we map to BODY_SCOPE_NONE in the lookup.
  hashmap_put(b->scope_map, key, (void *)(uintptr_t)((uint64_t)scope_id + 1));
}

// === Helpers ================================================================

static TinySpan span_of(const SemaCtx *ctx, SyntaxNode *node) {
  TextRange r = syntax_node_text_range(node);
  return span_make((uint16_t)ctx->file_local.idx, r.start, r.length);
}

static StrId intern_tok(struct db *s, SyntaxToken *t) {
  if (!t)
    return (StrId){0};
  const char *txt = syntax_token_text(t);
  uint32_t len = syntax_token_text_range(t).length;
  return pool_intern(&s->strings, txt, len);
}

// === Walk ==================================================================

static void walk(const SemaCtx *ctx, SyntaxNode *node, BodyScopeBuilder *b,
                 uint32_t current_scope);

// Recurse into every node/token child of `node`. Used for the transparent
// "no scope-opening, no bind-pushing" default case. This is the documented
// raw-navigation exception — no typed wrapper exposes "iterate every child
// regardless of kind", because most callers want filtered child accessors.
static void walk_children(const SemaCtx *ctx, SyntaxNode *node,
                          BodyScopeBuilder *b, uint32_t current_scope) {
  uint32_t total = syntax_node_num_children(node);
  for (uint32_t i = 0; i < total; i++) {
    SyntaxElement el = syntax_node_child_or_token(node, i);
    if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      walk(ctx, el.node, b, current_scope);
      syntax_node_release(el.node);
    } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      syntax_token_release(el.token);
    }
  }
}

// Recurse into every node child of a SK_STMT_LIST. The list's tokens
// (semicolons) are skipped.
static void walk_stmts(const SemaCtx *ctx, SyntaxNode *stmts,
                       BodyScopeBuilder *b, uint32_t scope) {
  if (!stmts)
    return;
  uint32_t total = syntax_node_num_children(stmts);
  for (uint32_t i = 0; i < total; i++) {
    SyntaxElement el = syntax_node_child_or_token(stmts, i);
    if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      walk(ctx, el.node, b, scope);
      syntax_node_release(el.node);
    } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      syntax_token_release(el.token);
    }
  }
}

// Type a let-bind RHS or annotation. Annotation wins when present;
// when both are given, the value is checked against the annotation
// (emits "expected {0}" on mismatch via sema_check_expr).
static IpIndex type_of_bind(const SemaCtx *ctx, SyntaxNode *type_node,
                            SyntaxNode *value_node) {
  if (type_node) {
    IpIndex annotated = sema_resolve_type_expr(ctx, type_node);
    if (annotated.v != IP_NONE.v && value_node) {
      (void)sema_check_expr(ctx, value_node, annotated);
    }
    return annotated;
  }
  if (value_node)
    return sema_type_of_expr(ctx, value_node);
  return IP_NONE;
}

static void walk(const SemaCtx *ctx, SyntaxNode *node, BodyScopeBuilder *b,
                 uint32_t current_scope) {
  if (!node)
    return;

  tag_node(b, node, current_scope);

  struct db *s = ctx->s;
  SyntaxKind k = syntax_node_kind(node);

  // SK_BLOCK_STMT — opens a new scope, recurses into its STMT_LIST.
  if (k == SK_BLOCK_STMT) {
    BlockStmt bs;
    if (!BlockStmt_cast(node, &bs)) {
      walk_children(ctx, node, b, current_scope);
      return;
    }
    uint32_t child = scope_push(b, current_scope, node);
    SyntaxNode *stmts = BlockStmt_stmts(&bs);
    walk_stmts(ctx, stmts, b, child);
    if (stmts) syntax_node_release(stmts);
    return;
  }

  // SK_CONST_DECL / SK_VAR_DECL — statement-position let-bind.
  if (k == SK_CONST_DECL || k == SK_VAR_DECL) {
    SyntaxToken *name_tok = NULL;
    SyntaxNode  *type_node = NULL;
    SyntaxNode  *value_node = NULL;
    if (k == SK_CONST_DECL) {
      ConstDef cd;
      if (ConstDef_cast(node, &cd)) {
        name_tok = ConstDef_name(&cd);
        type_node = ConstDef_type(&cd);
        value_node = ConstDef_value(&cd);
      }
    } else {
      VarDef vd;
      if (VarDef_cast(node, &vd)) {
        name_tok = VarDef_name(&vd);
        type_node = VarDef_type(&vd);
        value_node = VarDef_value(&vd);
      }
    }
    StrId name = intern_tok(s, name_tok);
    if (name_tok) syntax_token_release(name_tok);

    // Tag subtrees BEFORE typing so any re-entrant lookup sees them in
    // the right scope. type_of_bind may walk into these via
    // sema_type_of_expr → sema_body_scope_lookup.
    if (type_node) walk(ctx, type_node, b, current_scope);
    if (value_node) walk(ctx, value_node, b, current_scope);

    if (name.idx != 0) {
      // ALWAYS push the bind, even when the RHS type is IP_NONE. A
      // failed type-of-RHS (e.g., an unimplemented builtin) is
      // independent of the binding's existence: the name IS declared,
      // just with an unknown type. Skipping the push here would make
      // downstream uses emit "undefined identifier" diags, which is
      // misleading since the binding syntactically exists.
      IpIndex t = type_of_bind(ctx, type_node, value_node);
      bind_push(b, current_scope, name, t);
    }
    if (type_node) syntax_node_release(type_node);
    if (value_node) syntax_node_release(value_node);
    return;
  }

  // SK_IF_EXPR — cond + then + else. Detects if-let when the cond is a
  // ConstDef/VarDef.
  if (k == SK_IF_EXPR) {
    IfExpr ie;
    if (!IfExpr_cast(node, &ie)) {
      walk_children(ctx, node, b, current_scope);
      return;
    }
    SyntaxNode *cond_id = IfExpr_condition(&ie);
    SyntaxNode *then_id = IfExpr_then_branch(&ie);
    SyntaxNode *else_id = IfExpr_else_branch(&ie);

    // If-let detection: cond is itself a let-bind decl.
    SyntaxKind ck = cond_id ? syntax_node_kind(cond_id) : SYNTAX_KIND_NONE;
    bool is_if_let = (ck == SK_CONST_DECL || ck == SK_VAR_DECL);

    if (is_if_let) {
      SyntaxToken *name_tok = NULL;
      SyntaxNode  *rhs_id   = NULL;
      if (ck == SK_CONST_DECL) {
        ConstDef cd;
        if (ConstDef_cast(cond_id, &cd)) {
          name_tok = ConstDef_name(&cd);
          rhs_id   = ConstDef_value(&cd);
        }
      } else {
        VarDef vd;
        if (VarDef_cast(cond_id, &vd)) {
          name_tok = VarDef_name(&vd);
          rhs_id   = VarDef_value(&vd);
        }
      }
      StrId name = intern_tok(s, name_tok);
      if (name_tok) syntax_token_release(name_tok);

      // RHS lives in the PARENT scope (it can't see its own bind).
      if (rhs_id) walk(ctx, rhs_id, b, current_scope);

      uint32_t then_scope = scope_push(b, current_scope, cond_id);
      tag_node(b, cond_id, then_scope);

      if (name.idx != 0 && rhs_id) {
        IpIndex rhs_t = sema_type_of_expr(ctx, rhs_id);
        if (rhs_t.v != IP_NONE.v) {
          if (ip_tag(&s->intern, rhs_t) == IP_TAG_OPTIONAL_TYPE) {
            IpKey ik = ip_key(&s->intern, rhs_t);
            bind_push(b, then_scope, name, ik.optional_type.elem);
          } else {
            db_emit(s, DIAG_ERROR, span_of(ctx, cond_id),
                    "if-let pattern requires optional type, got %T", rhs_t);
          }
        }
      }

      if (then_id) walk(ctx, then_id, b, then_scope);

      uint32_t else_scope = scope_push(b, current_scope, else_id);
      if (else_id) walk(ctx, else_id, b, else_scope);

      if (rhs_id) syntax_node_release(rhs_id);
      if (cond_id) syntax_node_release(cond_id);
      if (then_id) syntax_node_release(then_id);
      if (else_id) syntax_node_release(else_id);
      return;
    }

    if (cond_id) walk(ctx, cond_id, b, current_scope);
    uint32_t then_scope = scope_push(b, current_scope, then_id);
    if (then_id) walk(ctx, then_id, b, then_scope);
    uint32_t else_scope = scope_push(b, current_scope, else_id);
    if (else_id) walk(ctx, else_id, b, else_scope);
    if (cond_id) syntax_node_release(cond_id);
    if (then_id) syntax_node_release(then_id);
    if (else_id) syntax_node_release(else_id);
    return;
  }

  // SK_LOOP_EXPR — opens a loop scope. Init/cond/step (if/when added)
  // and body all run inside the loop scope so any loop-local bind
  // doesn't leak out.
  if (k == SK_LOOP_EXPR) {
    LoopExpr le;
    if (!LoopExpr_cast(node, &le)) {
      walk_children(ctx, node, b, current_scope);
      return;
    }
    uint32_t loop_scope = scope_push(b, current_scope, node);
    SyntaxNode *body = LoopExpr_body(&le);
    if (body) {
      walk(ctx, body, b, loop_scope);
      syntax_node_release(body);
    }
    // TODO(loop-init): once the grammar exposes init/cond/step children
    // via the LoopExpr wrapper, walk them in loop_scope as well.
    return;
  }

  // SK_SWITCH_ARM — opens an arm scope (future: pattern binds land here).
  if (k == SK_SWITCH_ARM) {
    SwitchArm arm;
    if (!SwitchArm_cast(node, &arm)) {
      walk_children(ctx, node, b, current_scope);
      return;
    }
    uint32_t arm_scope = scope_push(b, current_scope, node);
    SyntaxNode *pat = SwitchArm_pattern(&arm);
    SyntaxNode *body = SwitchArm_body(&arm);
    if (pat) {
      walk(ctx, pat, b, arm_scope);
      syntax_node_release(pat);
    }
    if (body) {
      walk(ctx, body, b, arm_scope);
      syntax_node_release(body);
    }
    return;
  }

  // Default: transparent recursion into every child. Tagging happens
  // at the top of walk(); children inherit current_scope.
  walk_children(ctx, node, b, current_scope);
}

// === Lookup ================================================================

IpIndex sema_body_scope_lookup(struct db *s, DefId fn_def, SyntaxNode *use_node,
                               StrId name, bool *found_out) {
  if (found_out)
    *found_out = false;
  if (fn_def.idx == DEF_ID_NONE.idx || name.idx == 0 || !use_node)
    return IP_NONE;
  if (db_def_kind(s, fn_def) != KIND_FUNCTION)
    return IP_NONE;

  uint32_t row = db_def_row(s, fn_def, KIND_FUNCTION);
  FnBody fb = *(FnBody *)vec_get(&s->fns.body, row);
  HashMap *scope_map = (HashMap *)vec_get(&s->fns.scope_map, row);
  if (!hashmap_is_initialized(scope_map))
    return IP_NONE;

  uint64_t key = syntax_node_ptr_hash(syntax_node_ptr_new(use_node));
  void *v = hashmap_get(scope_map, key);
  if (!v)
    return IP_NONE;
  // Encoding: stored value is scope_id + 1.
  uint32_t scope = (uint32_t)((uintptr_t)v - 1);

  const ScopeRow *rows = (const ScopeRow *)s->body_scope_rows.data;
  const ScopedBind *binds = (const ScopedBind *)s->body_scope_binds.data;

  // Walk from the use-site scope outward. Within each scope, scan
  // forward (binds appended in source order) — latest match wins,
  // implementing shadowing. Scope ids are fn-local: index the fn's
  // slice via fb.scope_off / fb.bind_off.
  //
  // `seen` decouples "found a bind for this name in this scope" from
  // the type value. A bind whose RHS didn't type still EXISTS (its
  // type is just IP_NONE). Stopping at that bind reports "found, type
  // unknown" to the caller instead of masking shadowing.
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

// === Builder driver ========================================================

FnBody sema_body_scopes(struct db *s, DefId fn_def) {
  FnBody empty = {0};

  SyntaxNodePtr ptr =
      *(SyntaxNodePtr *)vec_get(&s->defs.syntax_ptrs, fn_def.idx);
  NamespaceId nsid =
      *(NamespaceId *)vec_get(&s->defs.parent_modules, fn_def.idx);

  // Depend on the module's top-level index — this body reads the
  // module's file list below; a file-set change must re-run it.
  (void)db_query_top_level_index(s, nsid);
  (void)db_query_def_identity(s, nsid, ptr);
  IpIndex sig = db_query_fn_signature(s, fn_def);
  if (sig.v == IP_NONE.v)
    return empty;

  // Locate the lambda SyntaxNode via the per-decl AST query — its
  // structural fingerprint is what makes a sibling edit early-cut this
  // query rather than re-running the scope build.
  SyntaxTree *body_tree = NULL;
  SyntaxNode *lambda_node = NULL;
  FileId body_fid = FILE_ID_NONE;
  GreenNode *body_groot = NULL;
  uint32_t fc = 0;
  const FileId *files = db_get_namespace_files(s, nsid, &fc);
  for (uint32_t i = 0; i < fc; i++) {
    SyntaxNodePtr cur_ptr = db_query_decl_ast(s, files[i], ptr);
    if (cur_ptr.kind == SYNTAX_KIND_NONE)
      continue;
    uint32_t local = file_id_local(files[i]);
    GreenNode *groot =
        *(GreenNode **)vec_get(&s->files.green_roots, local);
    if (!groot)
      continue;
    SyntaxTree *tree = syntax_tree_new(groot);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxNode *decl = syntax_node_ptr_resolve(cur_ptr, root);
    syntax_node_release(root);
    if (!decl) {
      syntax_tree_free(tree);
      continue;
    }
    SyntaxKind dk = syntax_node_kind(decl);
    if (dk != SK_CONST_DECL && dk != SK_VAR_DECL) {
      syntax_node_release(decl);
      syntax_tree_free(tree);
      break;
    }
    SyntaxNode *value = NULL;
    if (dk == SK_CONST_DECL) {
      ConstDef cd;
      if (ConstDef_cast(decl, &cd))
        value = ConstDef_value(&cd);
    } else {
      VarDef vd;
      if (VarDef_cast(decl, &vd))
        value = VarDef_value(&vd);
    }
    syntax_node_release(decl);
    if (!value) {
      syntax_tree_free(tree);
      break;
    }
    if (syntax_node_kind(value) != SK_LAMBDA_EXPR) {
      syntax_node_release(value);
      syntax_tree_free(tree);
      break;
    }
    body_tree = tree;
    lambda_node = value;
    body_fid = files[i];
    body_groot = groot;
    break;
  }

  if (!body_tree || !lambda_node)
    return empty;

  // Lambda surface — params + return-type + body.
  LambdaExpr lam;
  if (!LambdaExpr_cast(lambda_node, &lam)) {
    syntax_node_release(lambda_node);
    syntax_tree_free(body_tree);
    return empty;
  }
  SyntaxNode *params = LambdaExpr_params(&lam);
  SyntaxNode *ret    = LambdaExpr_return_type(&lam);
  SyntaxNode *body   = LambdaExpr_body(&lam);

  uint32_t fn_row = db_def_row(s, fn_def, KIND_FUNCTION);

  // Reset the per-fn scope_map. On first build the column slot is a
  // zero HashMap (uninitialized); on re-run it holds the prior build's
  // entries — clear in place so we reuse the bucket allocation.
  HashMap *scope_map = (HashMap *)vec_get(&s->fns.scope_map, fn_row);
  if (hashmap_is_initialized(scope_map))
    hashmap_clear(scope_map);
  else
    hashmap_init(scope_map);

  BodyScopeBuilder b = {
      .s         = s,
      .fn_row    = fn_row,
      .scope_off = (uint32_t)s->body_scope_rows.count,
      .bind_off  = (uint32_t)s->body_scope_binds.count,
      .scope_map = scope_map,
  };

  // Publish the (so-far-empty) ranges BEFORE walking so re-entrant
  // lookups see correct offsets; scope_len / bind_len grow on each push.
  *bb_fnbody(&b) = (FnBody){.scope_off = b.scope_off,
                            .scope_len = 0,
                            .bind_off  = b.bind_off,
                            .bind_len  = 0};

  // Root scope holds params. Even with no params the root scope must
  // exist (it's the parent of every block opened below).
  uint32_t root = scope_push(&b, BODY_SCOPE_NONE, lambda_node);

  // Tag the lambda + its signature-position sub-trees (params, return
  // type) into the root scope so body_scope_lookup at any signature-
  // position node resolves through the root's binds. Without this,
  // signature-position nodes hash to no entry and the lookup falls off
  // immediately.
  tag_node(&b, lambda_node, root);

  // Build the SemaCtx for the walk. body_scopes doesn't open its own
  // NodeTypeBuilder — infer_body / fn_signature own those — so .types
  // is NULL. type_of_bind's recursive sema_resolve_type_expr /
  // sema_type_of_expr calls land outside any active builder, which is
  // correct: those values get re-typed and cached by infer_body's walk
  // on its own pass.
  SemaCtx walk_ctx = {
      .s                = s,
      .file_green_root  = body_groot,
      .nsid             = nsid,
      .enclosing_fn     = fn_def,
      .file_local       = body_fid,
      .types            = NULL,
  };

  // Visit each param + the return-type expr so every signature-position
  // descendant gets tagged into the root scope. walk's default case
  // (walk_children) recurses transparently — no scope-opening for
  // param subtrees.
  if (params) {
    uint32_t total = syntax_node_num_children(params);
    for (uint32_t i = 0; i < total; i++) {
      SyntaxElement el = syntax_node_child_or_token(params, i);
      if (el.kind == SYNTAX_ELEM_NODE && el.node) {
        walk(&walk_ctx, el.node, &b, root);
        syntax_node_release(el.node);
      } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
        syntax_token_release(el.token);
      }
    }
  }
  if (ret) walk(&walk_ctx, ret, &b, root);

  // Push the param binds. Param i's type comes from the fn signature's
  // params array (set by sema_fn_signature). A parser/sig mismatch (one
  // side has fewer entries than the other) just trims the shorter pair.
  IpKey sig_key = ip_key(&s->intern, sig);
  const IpIndex *sig_params = sig_key.fn_type.params;
  size_t n_sig_params = sig_key.fn_type.n_params;
  size_t pi = 0;
  if (params) {
    uint32_t total = syntax_node_num_children(params);
    for (uint32_t i = 0; i < total && pi < n_sig_params; i++) {
      SyntaxElement el = syntax_node_child_or_token(params, i);
      if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
        syntax_token_release(el.token);
        continue;
      }
      if (el.kind != SYNTAX_ELEM_NODE || !el.node)
        continue;
      if (syntax_node_kind(el.node) != SK_PARAM) {
        syntax_node_release(el.node);
        continue;
      }
      Param p;
      if (!Param_cast(el.node, &p)) {
        syntax_node_release(el.node);
        continue;
      }
      SyntaxToken *pname_tok = Param_name(&p);
      StrId pname = intern_tok(s, pname_tok);
      if (pname_tok) syntax_token_release(pname_tok);
      syntax_node_release(el.node);
      if (pname.idx == 0) {
        pi++;
        continue;
      }
      IpIndex pty = sig_params[pi++];
      if (pty.v == IP_NONE.v)
        continue;
      bind_push(&b, root, pname, pty);
    }
  }

  // Pass 2: build scope tree + tag body nodes + push body let-binds.
  if (body) walk(&walk_ctx, body, &b, root);

  if (params) syntax_node_release(params);
  if (ret)    syntax_node_release(ret);
  if (body)   syntax_node_release(body);
  syntax_node_release(lambda_node);
  syntax_tree_free(body_tree);

  return *bb_fnbody(&b);
}
