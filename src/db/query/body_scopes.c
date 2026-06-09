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
#include "capability.h"     // db_read_*, db_get_*_untracked
#include "engine.h"
#include "engine_internal.h"
#include "result_columns.h" // db.h, ids.h, intern_pool.h, syntax.h

#include "../diag/ast_id.h" // DeclAstIdMap (P7.1.6)

#include "../../ast/ast_decl.h"
#include "../../ast/ast_expr.h"
#include "../../support/data_structure/hashmap.h"
#include "../../support/data_structure/stringpool.h"
#include "../../syntax/syntax_kind.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

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

// Per-child scope override: the named child walks in `scope` instead of the
// caller's default.
typedef struct {
  SyntaxNode *child;
  uint32_t    scope;
} ChildScope;

// Like walk_children, but every node child walks in `def_scope` UNLESS it
// matches one of the `n` overrides (by node identity) — then in that scope.
// No child is ever dropped: a child not named in `ov` still gets `def_scope`,
// so adding a child to a construct can never silently leave its names unscoped.
// This is what makes the scope-introducing cases below non-fragile — they set
// up scopes/bindings then hand the WHOLE child set here, rather than walking a
// hand-picked subset and returning.
static void walk_children_scoped(SyntaxNode *node, BSBuilder *b,
                                 uint32_t def_scope, const ChildScope *ov,
                                 uint32_t n) {
  uint32_t total = syntax_node_num_children(node);
  for (uint32_t i = 0; i < total; i++) {
    SyntaxElement el = syntax_node_child_or_token(node, i);
    if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      uint32_t sc = def_scope;
      SyntaxNodePtr p = syntax_node_ptr_new(el.node);
      for (uint32_t j = 0; j < n; j++) {
        if (ov[j].child &&
            syntax_node_ptr_eq(p, syntax_node_ptr_new(ov[j].child))) {
          sc = ov[j].scope;
          break;
        }
      }
      walk(el.node, b, sc);
      syntax_node_release(el.node);
    } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      syntax_token_release(el.token);
    }
  }
}

// Walk an SK_ARG_LIST of a with-call: the synthetic continuation lambda
// (identified by `cont_p`) has its BODY routed into `cont_scope` (where the
// loose binder `x` is bound) and the lambda node itself is NOT walked (so the
// opaque-nested-lambda guard doesn't drop its body); every other arg walks in
// `outer`.
static void walk_with_arglist(SyntaxNode *args, BSBuilder *b, uint32_t outer,
                              uint32_t cont_scope, SyntaxNodePtr cont_p) {
  uint32_t total = syntax_node_num_children(args);
  for (uint32_t i = 0; i < total; i++) {
    SyntaxElement el = syntax_node_child_or_token(args, i);
    if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      if (syntax_node_ptr_eq(syntax_node_ptr_new(el.node), cont_p)) {
        LambdaExpr lam;
        SyntaxNode *body = NULL;
        if (LambdaExpr_cast(el.node, &lam))
          body = LambdaExpr_body(&lam);
        if (body) {
          walk(body, b, cont_scope);
          syntax_node_release(body);
        }
      } else {
        walk(el.node, b, outer);
      }
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

  // SK_BLOCK_STMT — opens a new scope; all children (the STMT_LIST) live in it.
  if (k == SK_BLOCK_STMT) {
    uint32_t child = scope_push(b, current_scope, node);
    walk_children_scoped(node, b, child, NULL, 0);
    return;
  }

  // SK_BIND_DECL — statement-position let-bind. The annotation + RHS live in
  // the CURRENT scope (can't see the new bind), so the default scope is fine
  // for every child; only the binding itself is registered specially.
  if (k == SK_BIND_DECL) {
    SyntaxToken *name_tok = NULL;
    BindDef bd;
    if (BindDef_cast(node, &bd))
      name_tok = BindDef_name(&bd);
    StrId name = intern_tok(s, name_tok);
    if (name_tok)
      syntax_token_release(name_tok);
    walk_children_scoped(node, b, current_scope, NULL, 0);
    // Push the bind into the current scope; bind_site = the decl node.
    if (name.idx != 0)
      bind_push(b, current_scope, name, node);
    return;
  }

  // SK_IF_EXPR — cond + optional <capture> + then + optional else.
  // Capture (SK_CAPTURE) binds the unwrapped optional inside the then-
  // scope only. Cond is a pure expression — no SK_BIND_DECL leak.
  if (k == SK_IF_EXPR) {
    IfExpr ie;
    if (!IfExpr_cast(node, &ie)) {
      walk_children_scoped(node, b, current_scope, NULL, 0);
      return;
    }
    SyntaxNode *capture = IfExpr_capture(&ie);
    SyntaxNode *then_b = IfExpr_then_branch(&ie);
    SyntaxNode *else_b = IfExpr_else_branch(&ie);

    // cond stays on the default (current) scope — a capture can't see its own
    // bind. then-branch + capture get a then-scope holding the if-let binding;
    // else gets its own sibling scope (no capture). Everything else (cond, any
    // future child) safely defaults to current.
    ChildScope ov[3];
    uint32_t n = 0;
    if (capture || then_b) {
      uint32_t then_scope =
          scope_push(b, current_scope, capture ? capture : then_b);
      if (capture) {
        Capture cap;
        SyntaxToken *name_tok = NULL;
        if (Capture_cast(capture, &cap))
          name_tok = Capture_name(&cap);
        StrId name = intern_tok(s, name_tok);
        if (name_tok) syntax_token_release(name_tok);
        // Bind the captured name in the then-scope; the unwrapped-optional
        // TYPE is infer_body's job (handle_if_cond pushes it at the capture).
        if (name.idx != 0)
          bind_push(b, then_scope, name, capture);
        ov[n++] = (ChildScope){capture, then_scope};
      }
      if (then_b) ov[n++] = (ChildScope){then_b, then_scope};
    }
    if (else_b) {
      uint32_t else_scope = scope_push(b, current_scope, else_b);
      ov[n++] = (ChildScope){else_b, else_scope};
    }
    walk_children_scoped(node, b, current_scope, ov, n);

    if (capture) syntax_node_release(capture);
    if (then_b)  syntax_node_release(then_b);
    if (else_b)  syntax_node_release(else_b);
    return;
  }

  // SK_LOOP_EXPR — opens a loop_scope. Capture (when present) binds an
  // unwrapped optional / range index that's re-evaluated each iteration;
  // its bind_site is the SK_CAPTURE node. Without a capture, plain
  // walk_children handles cond + body uniformly.
  if (k == SK_LOOP_EXPR) {
    uint32_t loop_scope = scope_push(b, current_scope, node);
    LoopExpr le;
    if (!LoopExpr_cast(node, &le)) {
      walk_children_scoped(node, b, loop_scope, NULL, 0);
      return;
    }
    SyntaxNode *capture = LoopExpr_capture(&le);
    SyntaxNode *else_b  = LoopExpr_else_branch(&le);

    if (capture) {
      Capture cap;
      SyntaxToken *name_tok = NULL;
      if (Capture_cast(capture, &cap))
        name_tok = Capture_name(&cap);
      StrId name = intern_tok(s, name_tok);
      if (name_tok) syntax_token_release(name_tok);
      if (name.idx != 0)
        bind_push(b, loop_scope, name, capture);
    }

    // Default = loop_scope: cond, capture, body, AND the continue-expr
    // `: (step)` all run there (the step sees the capture + outer locals).
    // Only `else` differs — it runs on normal exit (cond false / nil), so no
    // capture payload; it walks in the OUTER scope. Note the continue-expr
    // needs no mention: it lands on the safe default, so this can't repeat the
    // forgotten-child bug.
    ChildScope ov[1];
    uint32_t n = 0;
    if (else_b) ov[n++] = (ChildScope){else_b, current_scope};
    walk_children_scoped(node, b, loop_scope, ov, n);

    if (capture) syntax_node_release(capture);
    if (else_b)  syntax_node_release(else_b);
    return;
  }

  // SK_SWITCH_ARM — opens an arm scope (future: pattern binds land here); all
  // children (pattern + body) live in it.
  if (k == SK_SWITCH_ARM) {
    uint32_t arm_scope = scope_push(b, current_scope, node);
    walk_children_scoped(node, b, arm_scope, NULL, 0);
    return;
  }

  // SK_RETURN_CLAUSE — a handler's `return(x) body`. `x` (the action's result)
  // is bound in a child scope visible to the clause body only; its own type
  // annotation (in the SK_PARAM_LIST) walks in the outer scope — it can't see
  // `x`. Mirrors the if-let capture / loop binder.
  if (k == SK_RETURN_CLAUSE) {
    uint32_t clause_scope = scope_push(b, current_scope, node);
    SyntaxNode *plist = ast_first_child(node, SK_PARAM_LIST);
    if (plist) {
      SyntaxNode *param = ast_first_child(plist, SK_PARAM);
      if (param) {
        Param pp;
        SyntaxToken *pn = NULL;
        if (Param_cast(param, &pp))
          pn = Param_name(&pp);
        StrId pname = intern_tok(s, pn);
        if (pn)
          syntax_token_release(pn);
        if (pname.idx != 0)
          bind_push(b, clause_scope, pname, param); // bind-site = the SK_PARAM
        syntax_node_release(param);
      }
      syntax_node_release(plist);
    }
    // Param-list (incl. the annotation) in the OUTER scope; the body sees `x`.
    uint32_t total = syntax_node_num_children(node);
    for (uint32_t i = 0; i < total; i++) {
      SyntaxElement el = syntax_node_child_or_token(node, i);
      if (el.kind == SYNTAX_ELEM_NODE && el.node) {
        if (syntax_node_kind(el.node) == SK_PARAM_LIST)
          walk(el.node, b, current_scope);
        else
          walk(el.node, b, clause_scope);
        syntax_node_release(el.node);
      } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
        syntax_token_release(el.token);
      }
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

  // SK_CALL_EXPR — a `with`-desugared call (SK_WITH_KW marker) carries its
  // continuation as a trailing lambda holding the rest-of-block. Unlike a
  // genuine nested lambda, that continuation IS part of THIS fn (CPS-shaped),
  // so we DO descend into its body — in a child scope so it sees the enclosing
  // locals — and bind the loose `x` there. Reunites the pieces the parser left
  // loose (parse_expr.c:1561). Plain calls fall through to the default below.
  if (k == SK_CALL_EXPR) {
    CallExpr ce;
    if (CallExpr_cast(node, &ce) && CallExpr_is_with(&ce)) {
      SyntaxNode *cont = CallExpr_with_continuation(&ce);
      if (cont) {
        uint32_t cont_scope = scope_push(b, current_scope, cont);
        SyntaxNode *binder = CallExpr_with_binder(&ce);
        if (binder) {
          Param pp;
          SyntaxToken *bn = NULL;
          if (Param_cast(binder, &pp))
            bn = Param_name(&pp);
          StrId bname = intern_tok(s, bn);
          if (bn)
            syntax_token_release(bn);
          if (bname.idx != 0)
            bind_push(b, cont_scope, bname, binder); // bind-site = loose SK_PARAM
          syntax_node_release(binder);
        }
        // Walk the call's children in current_scope, routing the continuation
        // lambda's body (inside the arg-list) into cont_scope.
        SyntaxNodePtr cont_p = syntax_node_ptr_new(cont);
        uint32_t total = syntax_node_num_children(node);
        for (uint32_t i = 0; i < total; i++) {
          SyntaxElement el = syntax_node_child_or_token(node, i);
          if (el.kind == SYNTAX_ELEM_NODE && el.node) {
            if (syntax_node_kind(el.node) == SK_ARG_LIST)
              walk_with_arglist(el.node, b, current_scope, cont_scope, cont_p);
            else
              walk(el.node, b, current_scope);
            syntax_node_release(el.node);
          } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
            syntax_token_release(el.token);
          }
        }
        syntax_node_release(cont);
        return;
      }
    }
    // not a with-call (or no continuation) → default recursion below.
  }

  // Default: transparent recursion; children inherit current_scope.
  walk_children(node, b, current_scope);
}

// ============================================================================
// BODY_SCOPES query.
// ============================================================================

// P7.1.6 — preorder walk of the lambda subtree, recording a
// SyntaxNodePtr per node into the per-fn DeclAstIdMap. RelAstId is
// the index into `ptrs`. Indices are body-local: paired with the
// stable DeclKey, they survive sibling reparses that don't touch this
// body (the property that makes cached INFER diags re-resolvable
// after a neighbour edit). Walks the whole lambda (params + return +
// body) so any node BODY_SCOPES or INFER_BODY touches is anchorable.
static void decl_ast_id_map_walk(SyntaxNode *node, DeclAstIdMap *map) {
  if (!node)
    return;
  uint32_t id = map->next_id++;
  hashmap_put_or_die(&map->rev,
                     syntax_node_ptr_hash(syntax_node_ptr_new(node)),
                     (void *)(uintptr_t)((uint64_t)id + 1),
                     "decl_ast_id_map_walk");

  uint32_t total = syntax_node_num_children(node);
  for (uint32_t i = 0; i < total; i++) {
    SyntaxElement el = syntax_node_child_or_token(node, i);
    if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      decl_ast_id_map_walk(el.node, map);
      syntax_node_release(el.node);
    } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      syntax_token_release(el.token);
    }
  }
}

// Producer-side rebuild of the DeclAstIdMap row owned by TYPE_OF_DECL.
// Walks the entire decl WRAPPER (signature + body for fns; whole
// wrapper for non-fns). RelAstIds are preorder indices within the
// wrapper, so any sub-node any cached query emits a diag against can
// be recovered structurally by `decl_ast_id_resolve` at publish time.
// The typed setter resets the row (free + init) and hands back the
// mut pointer. Returns NULL only if the row is unallocated (treat
// as no-op).
void decl_ast_id_map_refresh(struct db *s, DefId def,
                             SyntaxNode *wrapper_node) {
  if (!wrapper_node)
    return;
  DeclAstIdMap *m = db_write_decl_ast_id_map_reset(s, def);
  if (m)
    decl_ast_id_map_walk(wrapper_node, m);
}

// Preorder visitor for decl_ast_id_resolve. State: counts down to
// zero on the target visit; on zero, returns the node (+1 ref) and
// short-circuits the walk. Mirrors the order of decl_ast_id_map_walk
// so RelAstId computed at emit time selects the same node here.
typedef struct {
  uint32_t    remaining;
  SyntaxNode *found; // +1 ref if non-NULL
} DeclAstIdFinder;

static void decl_ast_id_find(SyntaxNode *node, DeclAstIdFinder *f) {
  if (!node || f->found)
    return;
  if (f->remaining == 0) {
    syntax_node_retain(node);
    f->found = node;
    return;
  }
  f->remaining--;
  uint32_t total = syntax_node_num_children(node);
  for (uint32_t i = 0; i < total && !f->found; i++) {
    SyntaxElement el = syntax_node_child_or_token(node, i);
    if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      decl_ast_id_find(el.node, f);
      syntax_node_release(el.node);
    } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      syntax_token_release(el.token);
    }
  }
}

// decl_ast_id_resolve — walk the CURRENT body subtree in preorder,
// return the rel-th node (+1 ref). RelAstId is the preorder index
// over the body; walking the live tree in the same order returns
// the same logical node, even if the body's absolute byte range
// shifted (sibling edit) — that's the structural invariance the
// salsa cutoff guarantees.
//
// Returns NULL if the def isn't bound to a lambda OR the index is
// out of range (a stale RelAstId from an INFER_BODY that DID re-run
// after structural change would land here; the caller's diag is
// then unresolvable and falls back to the file-head anchor).
// F3 (Phase P audit) — preorder walker variant: instead of stopping at
// the rel-th node, push every visited node (+1 ref) into out_nodes.
// Used by DiagResolver's slot-of-one body cache so K diags for one fn
// cost 1 walk + K array reads (was K walks before this).
static void decl_ast_id_collect_walk(SyntaxNode *node, Vec *out) {
  if (!node)
    return;
  syntax_node_retain(node);
  vec_push(out, &node);
  uint32_t total = syntax_node_num_children(node);
  for (uint32_t i = 0; i < total; i++) {
    SyntaxElement el = syntax_node_child_or_token(node, i);
    if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      decl_ast_id_collect_walk(el.node, out);
      syntax_node_release(el.node);
    } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      syntax_token_release(el.token);
    }
  }
}

// Public sibling of decl_ast_id_resolve — return the full preorder
// array (each entry is a +1 SyntaxNode*). out_nodes must be a
// vec_init'd Vec<SyntaxNode*>; caller is responsible for releasing
// every element AND vec_freeing. On any failure (def isn't a fn,
// lambda missing, tree unavailable) leaves out_nodes empty.
void decl_ast_id_preorder_collect(db_query_ctx *ctx, DefId def,
                                  Vec *out_nodes) {
  // PUBLIC HELPER — called from both query-internal paths (frame
  // active) AND keep-zone tests (no frame). Use untracked reads; the
  // dep chain (if any) is the CALLER's responsibility — for query
  // callers, db_query_top_level_entry below records the content dep
  // that matters here.
  //
  // Walks the entire decl WRAPPER (signature + body for fns; whole
  // wrapper for non-fns). Mirrors the builder in decl_ast_id_map_walk
  // so RelAstId computed at emit time selects the same node here.
  struct db *s = (struct db *)ctx;
  StrId name = db_get_def_name_untracked(s, def);
  NamespaceId nsid = db_get_def_parent_module_untracked(s, def);
  TopLevelEntry e = db_query_top_level_entry(ctx, nsid, name);
  if (e.node_ptr.kind == SYNTAX_KIND_NONE)
    return;
  uint32_t local = file_id_local(e.file);
  struct GreenNode *groot =
      *(struct GreenNode **)vec_get(&s->files.green_roots, local); // LINT_UNTRACKED_OK: file dep recorded via db_query_top_level_entry above
  if (!groot)
    return;
  SyntaxTree *tree = syntax_tree_new(groot);
  SyntaxNode *rroot = syntax_tree_root(tree);
  SyntaxNode *wrapper = syntax_node_ptr_resolve(e.node_ptr, rroot);
  syntax_node_release(rroot);
  if (wrapper) {
    decl_ast_id_collect_walk(wrapper, out_nodes);
    syntax_node_release(wrapper);
  }
  syntax_tree_free(tree);
}

SyntaxNode *decl_ast_id_resolve(db_query_ctx *ctx, DefId def,
                                uint32_t rel) {
  // Public helper — same dual-call rule as decl_ast_id_preorder_collect.
  // Walks the decl WRAPPER (signature + body for fns; whole wrapper for
  // non-fns) in preorder, returning the rel-th node. Mirrors the order
  // of decl_ast_id_map_walk so RelAstId computed at emit time selects
  // the same node here.
  struct db *s = (struct db *)ctx;
  StrId name = db_get_def_name_untracked(s, def);
  NamespaceId nsid = db_get_def_parent_module_untracked(s, def);
  TopLevelEntry e = db_query_top_level_entry(ctx, nsid, name);
  if (e.node_ptr.kind == SYNTAX_KIND_NONE)
    return NULL;
  uint32_t local = file_id_local(e.file);
  struct GreenNode *groot =
      *(struct GreenNode **)vec_get(&s->files.green_roots, local); // LINT_UNTRACKED_OK: file dep via db_query_top_level_entry above
  if (!groot)
    return NULL;
  SyntaxTree *tree = syntax_tree_new(groot);
  SyntaxNode *rroot = syntax_tree_root(tree);
  SyntaxNode *wrapper = syntax_node_ptr_resolve(e.node_ptr, rroot);
  syntax_node_release(rroot);
  if (!wrapper) {
    syntax_tree_free(tree);
    return NULL;
  }
  DeclAstIdFinder f = {.remaining = rel, .found = NULL};
  decl_ast_id_find(wrapper, &f);
  syntax_node_release(wrapper);
  syntax_tree_free(tree);
  return f.found;
}

const FnBody *db_query_body_scopes(db_query_ctx *ctx, DefId def) {
  struct db *s = (struct db *)ctx;
  // BODY_SCOPES is KIND_FUNCTION-only at the routing layer; refuse non-fns
  // BEFORE the guard so the query is TOTAL (a non-fn caller gets NULL, not the
  // db_query_begin "slot kind not wired" assert). Nothing depends on
  // body_scopes(non-fn), so no memoization is needed. Untracked here is
  // correct: we're BEFORE the query frame opens.
  if (db_get_def_kind_untracked(s, def) != KIND_FUNCTION)
    return NULL;
  DB_QUERY_GUARD(ctx, QUERY_BODY_SCOPES, (uint64_t)def.idx,
                 /* on_cached */ body_scopes_read(s, def),
                 /* on_cycle  */ NULL,
                 /* on_error  */ NULL);

  FnBody empty = {0}; // used by the no-lambda fallback below
  // Producer-side self-data: BODY_SCOPES owns `def`'s body row.
  StrId name = db_get_def_name_untracked(s, def);
  NamespaceId nsid = db_get_def_parent_module_untracked(s, def);

  // Locate the lambda via the content firewall (same preamble as type_of_def).
  TopLevelEntry e = db_query_top_level_entry(ctx, nsid, name); // CONTENT dep
  SyntaxTree *tree = NULL;
  SyntaxNode *lambda_node = NULL;
  if (e.node_ptr.kind != SYNTAX_KIND_NONE) {
    // LINT_UNTRACKED_OK — TOP_LEVEL_ENTRY above carries the body-stable
    // invalidation; a tracked FILE_AST read here would record the
    // whole-file hash as a dep, killing per-body salsa granularity.
    // See plan Phase 3.1.
    struct GreenNode *groot = db_read_file_ast_untracked(ctx, e.file);
    if (groot) {
      tree = syntax_tree_new(groot);              // BORROWS groot
      SyntaxNode *rroot = syntax_tree_root(tree); // +1
      SyntaxNode *wrapper = syntax_node_ptr_resolve(e.node_ptr, rroot);
      syntax_node_release(rroot);
      if (wrapper) {
        SyntaxNode *val = NULL;
        BindDef bd;
        if (BindDef_cast(wrapper, &bd))
          val = BindDef_value(&bd);
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
    // P7.1.6 — body no longer resolves to a lambda (e.g. const RHS
    // was edited to a non-lambda). Wipe any prior DeclAstIdMap so a
    // stale ptr table can't outlive its tree. The reset setter
    // free+inits the row in place (returns NULL → no row to wipe).
    (void)db_write_decl_ast_id_map_reset(s, def);
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

  // (Phase-3.1 follow-up: decl_ast_id_map is owned by TYPE_OF_DECL
  // and walks the entire decl wrapper, not just the lambda body.
  // TYPE_OF_DECL runs before BODY_SCOPES for any KIND_FUNCTION via
  // the per-decl loop in db_check_namespace, so by the time we get
  // here the map is already populated against the current wrapper.)

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
