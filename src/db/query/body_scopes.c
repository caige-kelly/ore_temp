// Body-scope layer (Phase D2.3) — the per-fn lexical scope structure.
//
//   body_scopes(fn) — build the fn body's scope tree (ScopeRow.parent links) +
//                     name bindings (ScopedBind{scope, name, bind_site}) + the
//                     node→scope map (FnBody.scope_map). PURELY STRUCTURAL:
//                     RA's ExprScopes adapted to SyntaxNode. NO types here —
//                     ScopedBind carries the binding's `bind_site` node, not a
//                     type; infer_body (D2.4) owns local types, keyed off the
//                     bind_site this query records. Because the walk does no
//                     typing it calls NO nested queries, so there is no
//                     re-entrancy hazard, no partial-publish dance, and no
//                     per-push FnBody re-fetch.
//
//   body_scope_lookup(fn, use, name) — resolve a use-site name to its nearest
//                     enclosing binding's bind_site (BODY-LOCAL resolution).
//
// Dep shape mirrors type_of_def: top_level_entry(nsid, name) is the sole
// content firewall (a sibling edit cuts off), and the green root is read RAW
// from files.green_roots so a file_ast dep is NOT recorded (that would defeat
// the firewall). No fn_signature dep — param NAMES come from the param syntax;
// param TYPES are D2.4's concern. No QUERY_TOP_LEVEL_INDEX side-effect ensure,
// no decl_ast, no multi-file loop (all D1-superseded).

#define ORE_ENGINE_PRIVATE
#include "engine.h"
#include "engine_internal.h"
#include "result_columns.h" // db.h, ids.h, intern_pool.h, syntax.h

#include "../../ast/ast_decl.h"
#include "../../ast/ast_expr.h"
#include "../../ast/ast_stmt.h"
#include "../../support/data_structure/hashmap.h"
#include "../../support/data_structure/stringpool.h"
#include "../../syntax/syntax_kind.h"

#include <stdbool.h>
#include <stdint.h>

// --- Cross-layer query (parse.c) --------------------------------------------
extern TopLevelEntry db_query_top_level_entry(db_query_ctx *ctx,
                                              NamespaceId nsid, StrId name);

// This layer (the lookup ensures the build ran).
const FnBody *db_query_body_scopes(db_query_ctx *ctx, DefId def);

// ============================================================================
// Builder state. Scope ids are fn-LOCAL (0-based off the fn's pool base), so
// the fn's slice into the shared body_scope_* pools is self-contained.
// ============================================================================

typedef struct {
  struct db *s;
  uint32_t scope_off; // base in body_scope_rows
  uint32_t bind_off;  // base in body_scope_binds
  HashMap *scope_map; // node-ptr-hash -> scope_id+1 (built fresh, local)
  Fingerprint fp;     // POSITION-INDEPENDENT structural fold
} BSBuilder;

static uint32_t scope_push(BSBuilder *b, uint32_t parent,
                           SyntaxNode *block_node) {
  uint32_t id = (uint32_t)b->s->body_scope_rows.count - b->scope_off;
  SyntaxNodePtr bp =
      block_node ? syntax_node_ptr_new(block_node) : (SyntaxNodePtr){0};
  ScopeRow row = {.parent = parent, .block_node = bp};
  vec_push(&b->s->body_scope_rows, &row);
  // Fold the parent link in push order (NOT block_node byte ranges — those
  // are trivia-sensitive). Captures add/remove/reshape of the scope tree.
  b->fp = db_fp_combine(b->fp, db_fp_u64((uint64_t)parent));
  return id;
}

static void bind_push(BSBuilder *b, uint32_t scope_id, StrId name,
                      SyntaxNode *bind_node) {
  SyntaxNodePtr bs =
      bind_node ? syntax_node_ptr_new(bind_node) : (SyntaxNodePtr){0};
  ScopedBind sb = {.scope_id = scope_id, .name = name, .bind_site = bs};
  vec_push(&b->s->body_scope_binds, &sb);
  // Fold (scope, name) in push order (NOT bind_site byte ranges). Captures
  // add/remove/rename of a local; stable across pure value edits.
  b->fp = db_fp_combine(b->fp, db_fp_combine(db_fp_u64((uint64_t)scope_id),
                                             db_fp_u64((uint64_t)name.idx)));
}

// Map node → scope. Stores scope_id+1 so a hit is non-NULL even for scope 0.
static void tag_node(BSBuilder *b, SyntaxNode *node, uint32_t scope_id) {
  if (!node || !b->scope_map)
    return;
  uint64_t key = syntax_node_ptr_hash(syntax_node_ptr_new(node));
  hashmap_put(b->scope_map, key, (void *)(uintptr_t)((uint64_t)scope_id + 1));
}

static StrId intern_tok(struct db *s, SyntaxToken *t) {
  if (!t)
    return (StrId){0};
  const char *txt = syntax_token_text(t);
  uint32_t len = syntax_token_text_range(t).length;
  return pool_intern(&s->strings, txt, len);
}

// ============================================================================
// Walk — tags every node into its scope; opens scopes for blocks/if/loop/arm;
// pushes a bind for each statement-position let / param / if-let. No typing.
// ============================================================================

static void walk(SyntaxNode *node, BSBuilder *b, uint32_t current_scope);

// Recurse into every node child (tokens released). Used for STMT_LISTs and the
// transparent default case.
static void walk_children(SyntaxNode *node, BSBuilder *b, uint32_t scope) {
  uint32_t total = syntax_node_num_children(node);
  for (uint32_t i = 0; i < total; i++) {
    SyntaxElement el = syntax_node_child_or_token(node, i);
    if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      walk(el.node, b, scope);
      syntax_node_release(el.node);
    } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      syntax_token_release(el.token);
    }
  }
}

static void walk(SyntaxNode *node, BSBuilder *b, uint32_t current_scope) {
  if (!node)
    return;
  tag_node(b, node, current_scope);
  SyntaxKind k = syntax_node_kind(node);
  struct db *s = b->s;

  // SK_BLOCK_STMT — opens a new scope, recurses into its STMT_LIST.
  if (k == SK_BLOCK_STMT) {
    BlockStmt bs;
    if (!BlockStmt_cast(node, &bs)) {
      walk_children(node, b, current_scope);
      return;
    }
    uint32_t child = scope_push(b, current_scope, node);
    SyntaxNode *stmts = BlockStmt_stmts(&bs);
    if (stmts) {
      walk_children(stmts, b, child);
      syntax_node_release(stmts);
    }
    return;
  }

  // SK_CONST_DECL / SK_VAR_DECL — statement-position let-bind.
  if (k == SK_CONST_DECL || k == SK_VAR_DECL) {
    SyntaxToken *name_tok = NULL;
    SyntaxNode *type_node = NULL;
    SyntaxNode *value_node = NULL;
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
    if (name_tok)
      syntax_token_release(name_tok);
    // The annotation + RHS live in the CURRENT scope (can't see the bind).
    if (type_node) {
      walk(type_node, b, current_scope);
      syntax_node_release(type_node);
    }
    if (value_node) {
      walk(value_node, b, current_scope);
      syntax_node_release(value_node);
    }
    // Push the bind into the current scope; bind_site = the decl node.
    if (name.idx != 0)
      bind_push(b, current_scope, name, node);
    return;
  }

  // SK_IF_EXPR — cond + then + else. Detects if-let (cond is a let-bind decl).
  if (k == SK_IF_EXPR) {
    IfExpr ie;
    if (!IfExpr_cast(node, &ie)) {
      walk_children(node, b, current_scope);
      return;
    }
    SyntaxNode *cond = IfExpr_condition(&ie);
    SyntaxNode *then_b = IfExpr_then_branch(&ie);
    SyntaxNode *else_b = IfExpr_else_branch(&ie);
    SyntaxKind ck = cond ? syntax_node_kind(cond) : SYNTAX_KIND_NONE;
    bool is_if_let = (ck == SK_CONST_DECL || ck == SK_VAR_DECL);

    if (is_if_let) {
      SyntaxToken *name_tok = NULL;
      SyntaxNode *rhs = NULL;
      if (ck == SK_CONST_DECL) {
        ConstDef cd;
        if (ConstDef_cast(cond, &cd)) {
          name_tok = ConstDef_name(&cd);
          rhs = ConstDef_value(&cd);
        }
      } else {
        VarDef vd;
        if (VarDef_cast(cond, &vd)) {
          name_tok = VarDef_name(&vd);
          rhs = VarDef_value(&vd);
        }
      }
      StrId name = intern_tok(s, name_tok);
      if (name_tok)
        syntax_token_release(name_tok);
      // RHS lives in the PARENT scope (can't see its own bind).
      if (rhs)
        walk(rhs, b, current_scope);
      uint32_t then_scope = scope_push(b, current_scope, cond);
      tag_node(b, cond, then_scope);
      // The if-let binding lands in the then-scope; bind_site = cond node.
      // The unwrapped-optional TYPE is D2.4's job (infer_body).
      if (name.idx != 0)
        bind_push(b, then_scope, name, cond);
      if (then_b)
        walk(then_b, b, then_scope);
      if (else_b) { // no phantom scope when there's no else branch
        uint32_t else_scope = scope_push(b, current_scope, else_b);
        walk(else_b, b, else_scope);
      }
      if (rhs)
        syntax_node_release(rhs);
      if (cond)
        syntax_node_release(cond);
      if (then_b)
        syntax_node_release(then_b);
      if (else_b)
        syntax_node_release(else_b);
      return;
    }

    if (cond)
      walk(cond, b, current_scope);
    if (then_b) {
      uint32_t then_scope = scope_push(b, current_scope, then_b);
      walk(then_b, b, then_scope);
    }
    if (else_b) {
      uint32_t else_scope = scope_push(b, current_scope, else_b);
      walk(else_b, b, else_scope);
    }
    if (cond)
      syntax_node_release(cond);
    if (then_b)
      syntax_node_release(then_b);
    if (else_b)
      syntax_node_release(else_b);
    return;
  }

  // SK_LOOP_EXPR — opens a loop scope for the body.
  if (k == SK_LOOP_EXPR) {
    LoopExpr le;
    if (!LoopExpr_cast(node, &le)) {
      walk_children(node, b, current_scope);
      return;
    }
    uint32_t loop_scope = scope_push(b, current_scope, node);
    SyntaxNode *body = LoopExpr_body(&le);
    if (body) {
      walk(body, b, loop_scope);
      syntax_node_release(body);
    }
    return;
  }

  // SK_SWITCH_ARM — opens an arm scope (future: pattern binds land here).
  if (k == SK_SWITCH_ARM) {
    SwitchArm arm;
    if (!SwitchArm_cast(node, &arm)) {
      walk_children(node, b, current_scope);
      return;
    }
    uint32_t arm_scope = scope_push(b, current_scope, node);
    SyntaxNode *pat = SwitchArm_pattern(&arm);
    SyntaxNode *body = SwitchArm_body(&arm);
    if (pat) {
      walk(pat, b, arm_scope);
      syntax_node_release(pat);
    }
    if (body) {
      walk(body, b, arm_scope);
      syntax_node_release(body);
    }
    return;
  }

  // SK_LAMBDA_EXPR — a NESTED lambda is OPAQUE here. Its params + body own a
  // separate scope that body_scopes does not model yet (nested-lambda body
  // inference is deferred, D2.4b). Recursing in would leak the inner lambda's
  // params/locals into THIS fn's scope — a use of an inner name would wrongly
  // resolve against the outer fn. So do NOT descend; infer.c types the nested
  // lambda signature-only, keeping the deferral consistent on both sides.
  // (The fn's OWN lambda is handled by the query preamble, not here, so any
  // SK_LAMBDA_EXPR reaching this walk is necessarily nested.)
  if (k == SK_LAMBDA_EXPR)
    return;

  // Default: transparent recursion; children inherit current_scope.
  walk_children(node, b, current_scope);
}

// ============================================================================
// BODY_SCOPES query.
// ============================================================================

const FnBody *db_query_body_scopes(db_query_ctx *ctx, DefId def) {
  struct db *s = (struct db *)ctx;
  // BODY_SCOPES is KIND_FUNCTION-only at the routing layer; refuse non-fns
  // BEFORE the guard so the query is TOTAL (a non-fn caller gets NULL, not the
  // db_query_begin "slot kind not wired" assert). Nothing depends on
  // body_scopes(non-fn), so no memoization is needed.
  if (db_def_kind(s, def) != KIND_FUNCTION)
    return NULL;
  DB_QUERY_GUARD(ctx, QUERY_BODY_SCOPES, (uint64_t)def.idx,
                 /* on_cached */ body_scopes_read(s, def),
                 /* on_cycle  */ NULL,
                 /* on_error  */ NULL);

  FnBody empty = {0}; // used by the no-lambda fallback below
  StrId name = *(StrId *)vec_get(&s->defs.names, def.idx);
  NamespaceId nsid = *(NamespaceId *)vec_get(&s->defs.parent_modules, def.idx);

  // Locate the lambda via the content firewall (same preamble as type_of_def).
  TopLevelEntry e = db_query_top_level_entry(ctx, nsid, name); // CONTENT dep
  SyntaxTree *tree = NULL;
  SyntaxNode *lambda_node = NULL;
  if (e.node_ptr.kind != SYNTAX_KIND_NONE) {
    uint32_t local = file_id_local(e.file);
    struct GreenNode *groot =
        *(struct GreenNode **)vec_get(&s->files.green_roots, local);
    if (groot) {
      tree = syntax_tree_new(groot);              // BORROWS groot
      SyntaxNode *rroot = syntax_tree_root(tree); // +1
      SyntaxNode *wrapper = syntax_node_ptr_resolve(e.node_ptr, rroot);
      syntax_node_release(rroot);
      if (wrapper) {
        SyntaxNode *val = NULL;
        SyntaxKind wk = syntax_node_kind(wrapper);
        if (wk == SK_CONST_DECL) {
          ConstDef cd;
          if (ConstDef_cast(wrapper, &cd))
            val = ConstDef_value(&cd);
        } else if (wk == SK_VAR_DECL) {
          VarDef vd;
          if (VarDef_cast(wrapper, &vd))
            val = VarDef_value(&vd);
        }
        if (val) {
          if (syntax_node_kind(val) == SK_LAMBDA_EXPR)
            lambda_node = val; // keep
          else
            syntax_node_release(val);
        }
        syntax_node_release(wrapper);
      }
    }
  }

  if (!lambda_node) {
    if (tree)
      syntax_tree_free(tree);
    body_scopes_write(s, def, empty);
    db_query_succeed(ctx, QUERY_BODY_SCOPES, (uint64_t)def.idx,
                     FINGERPRINT_NONE);
    return body_scopes_read(s, def);
  }

  // Build into a fresh local scope_map; ownership transfers to the slot on
  // body_scopes_write (which frees the prior one).
  HashMap scope_map;
  hashmap_init(&scope_map);
  BSBuilder b = {
      .s = s,
      .scope_off = (uint32_t)s->body_scope_rows.count,
      .bind_off = (uint32_t)s->body_scope_binds.count,
      .scope_map = &scope_map,
      .fp = FINGERPRINT_NONE,
  };

  LambdaExpr lam;
  if (LambdaExpr_cast(lambda_node, &lam)) {
    SyntaxNode *params = LambdaExpr_params(&lam);
    SyntaxNode *ret = LambdaExpr_return_type(&lam);
    SyntaxNode *body = LambdaExpr_body(&lam);

    // Root scope holds the params. It must exist even with no params (it is
    // the parent of every block opened below).
    uint32_t root = scope_push(&b, BODY_SCOPE_NONE, lambda_node);
    tag_node(&b, lambda_node, root);

    // Param binds (names from the syntax; types are D2.4) + tag signature
    // subtrees into the root scope.
    if (params) {
      uint32_t total = syntax_node_num_children(params);
      for (uint32_t i = 0; i < total; i++) {
        SyntaxElement el = syntax_node_child_or_token(params, i);
        if (el.kind == SYNTAX_ELEM_NODE && el.node) {
          if (syntax_node_kind(el.node) == SK_PARAM) {
            Param p;
            if (Param_cast(el.node, &p)) {
              SyntaxToken *pn = Param_name(&p);
              StrId pname = intern_tok(s, pn);
              if (pn)
                syntax_token_release(pn);
              if (pname.idx != 0)
                bind_push(&b, root, pname, el.node);
            }
          }
          walk(el.node, &b, root);
          syntax_node_release(el.node);
        } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
          syntax_token_release(el.token);
        }
      }
      syntax_node_release(params);
    }
    if (ret) {
      walk(ret, &b, root);
      syntax_node_release(ret);
    }
    if (body) {
      walk(body, &b, root);
      syntax_node_release(body);
    }
  }

  syntax_node_release(lambda_node);
  if (tree)
    syntax_tree_free(tree);

  FnBody result = {
      .scope_off = b.scope_off,
      .scope_len = (uint32_t)s->body_scope_rows.count - b.scope_off,
      .bind_off = b.bind_off,
      .bind_len = (uint32_t)s->body_scope_binds.count - b.bind_off,
      .scope_map = scope_map,
  };
  body_scopes_write(s, def, result);
  db_query_succeed(ctx, QUERY_BODY_SCOPES, (uint64_t)def.idx, b.fp);
  return body_scopes_read(s, def);
}

// ============================================================================
// Body-local name resolution. Returns the nearest enclosing binding's
// bind_site ({.kind = SYNTAX_KIND_NONE} on miss) — the BINDING, not a type.
// ============================================================================

SyntaxNodePtr db_body_scope_lookup(db_query_ctx *ctx, DefId fn_def,
                                   SyntaxNode *use_node, StrId name) {
  struct db *s = (struct db *)ctx;
  SyntaxNodePtr none = {.kind = SYNTAX_KIND_NONE};
  if (fn_def.idx == DEF_ID_NONE.idx || name.idx == 0 || !use_node)
    return none;

  const FnBody *fbp = db_query_body_scopes(ctx, fn_def); // dep + ensure
  if (!fbp)
    return none;
  FnBody fb = *fbp;
  if (!hashmap_is_initialized(&fb.scope_map))
    return none;

  uint64_t key = syntax_node_ptr_hash(syntax_node_ptr_new(use_node));
  void *v = hashmap_get(&fb.scope_map, key);
  if (!v)
    return none;
  uint32_t scope = (uint32_t)((uintptr_t)v - 1);

  const ScopeRow *rows = (const ScopeRow *)s->body_scope_rows.data;
  const ScopedBind *binds = (const ScopedBind *)s->body_scope_binds.data;

  // Walk use-scope outward; within a scope scan forward (latest match wins =
  // shadowing). Scope ids are fn-local — index via fb.scope_off / fb.bind_off.
  while (scope != BODY_SCOPE_NONE) {
    SyntaxNodePtr found = none;
    bool seen = false;
    for (uint32_t i = 0; i < fb.bind_len; i++) {
      const ScopedBind *bd = &binds[fb.bind_off + i];
      if (bd->scope_id == scope && bd->name.idx == name.idx) {
        found = bd->bind_site;
        seen = true;
      }
    }
    if (seen)
      return found;
    if (scope >= fb.scope_len)
      return none;
    scope = rows[fb.scope_off + scope].parent;
  }
  return none;
}
