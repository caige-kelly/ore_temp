#include "effects.h"

#include <stdio.h>

#include "../sema.h"

static struct EffectSig *effect_sig_find_existing(struct Sema *s,
                                                  struct Expr *source) {
  if (!s || !source)
    return NULL;
  return (struct EffectSig *)hashmap_get(&s->effect_sig_cache,
                                         (uint64_t)(uintptr_t)source);
}

static struct EffectSig *effect_sig_new(struct Sema *s, struct Expr *source) {
  struct EffectSig *sig = arena_alloc(s->arena, sizeof(struct EffectSig));
  if (!sig)
    return NULL;
  sig->source = source;
  sig->terms = vec_new_in(s->arena, sizeof(struct EffectTerm));
  return sig;
}

// File-local aliases for the shared ast.h helpers — keep call sites
// readable in domain terms while sharing one implementation.
static inline struct Decl *resolved_decl_from_effect_expr(struct Expr *expr) {
  return ast_resolved_decl_of(expr);
}

static inline uint32_t name_id_from_effect_expr(struct Expr *expr) {
  return ast_name_id_of(expr);
}

static uint32_t scope_token_id_from_args(Vec *args) {
  if (!args)
    return 0;
  for (size_t i = 0; i < args->count; i++) {
    struct Expr **arg_p = (struct Expr **)vec_get(args, i);
    struct Expr *arg = arg_p ? *arg_p : NULL;
    if (!arg || arg->kind != expr_Ident || !arg->ident.resolved)
      continue;
    if (arg->ident.resolved->semantic_kind == SEM_SCOPE_TOKEN) {
      return arg->ident.resolved->scope_token_id;
    }
  }
  return 0;
}

static void effect_sig_push_term(struct EffectSig *sig,
                                 struct EffectTerm term) {
  if (!sig || !sig->terms)
    return;
  vec_push(sig->terms, &term);
}

static void effect_sig_push_row(struct EffectSig *sig, struct Identifier row) {
  if (!sig || row.string_id == 0)
    return;
  sig->is_open = true;
  sig->row_name_id = row.string_id;
  sig->row_decl = row.resolved;
  // Note: row variables are NOT terms — see effects.h. They live only on
  // is_open/row_name_id/row_decl.
}

static void push_unknown_term(struct EffectSig *sig, struct Expr *expr) {
  struct EffectTerm term = {
      .kind = EFFECT_TERM_UNKNOWN,
      .expr = expr,
  };
  effect_sig_push_term(sig, term);
}

static void effect_sig_collect_term(struct EffectSig *sig, struct Expr *expr) {
  if (!expr)
    return;

  switch (expr->kind) {
  case expr_EffectRow:
    effect_sig_collect_term(sig, expr->effect_row.head);
    effect_sig_push_row(sig, expr->effect_row.row);
    return;
  case expr_Bin:
    if (expr->bin.op == Pipe) {
      effect_sig_collect_term(sig, expr->bin.Left);
      effect_sig_collect_term(sig, expr->bin.Right);
      return;
    }
    // Any other Bin op in an effect annotation is malformed —
    // record an UNKNOWN term so callers see "couldn't classify"
    // rather than nothing.
    push_unknown_term(sig, expr);
    return;
  case expr_Call: {
    struct Expr *callee = expr->call.callee;
    struct Decl *decl = resolved_decl_from_effect_expr(callee);
    uint32_t scope_token_id = scope_token_id_from_args(expr->call.args);
    struct EffectTerm term = {
        .kind = scope_token_id ? EFFECT_TERM_SCOPED : EFFECT_TERM_NAMED,
        .expr = expr,
        .decl = decl,
        .name_id = name_id_from_effect_expr(callee),
        .scope_token_id = scope_token_id,
    };
    if (decl && decl->semantic_kind != SEM_EFFECT)
      term.kind = EFFECT_TERM_UNKNOWN;
    effect_sig_push_term(sig, term);
    return;
  }
  case expr_Ident:
  case expr_Field: {
    struct Decl *decl = resolved_decl_from_effect_expr(expr);
    struct EffectTerm term = {
        .kind = EFFECT_TERM_NAMED,
        .expr = expr,
        .decl = decl,
        .name_id = name_id_from_effect_expr(expr),
    };
    if (decl && decl->semantic_kind != SEM_EFFECT)
      term.kind = EFFECT_TERM_UNKNOWN;
    effect_sig_push_term(sig, term);
    return;
  }
  default:
    break;
  }

  // Catch-all for any expression kind we don't recognize as a valid
  // effect term shape (e.g. a literal or arithmetic expression slipped
  // into an effect annotation by an upstream bug).
  push_unknown_term(sig, expr);
}

struct EffectSig *sema_effect_sig_from_expr(struct Sema *s,
                                            struct Expr *effect) {
  if (!effect)
    return NULL;
  struct EffectSig *existing = effect_sig_find_existing(s, effect);
  if (existing)
    return existing;

  struct EffectSig *sig = effect_sig_new(s, effect);
  if (!sig)
    return NULL;
  effect_sig_collect_term(sig, effect);
  hashmap_put(&s->effect_sig_cache, (uint64_t)(uintptr_t)effect, sig);
  return sig;
}

static void print_effect_term(struct Sema *s, struct EffectTerm *term,
                              bool first) {
  if (!term)
    return;
  if (!first)
    printf(", ");
  const char *name = pool_get(s->pool, term->name_id, 0);
  switch (term->kind) {
  case EFFECT_TERM_SCOPED:
    printf("%s(scope#%u)", name ? name : "?", term->scope_token_id);
    break;
  case EFFECT_TERM_NAMED:
    printf("%s", name ? name : "?");
    break;
  case EFFECT_TERM_UNKNOWN:
    printf("?");
    break;
  }
}

void sema_print_effect_sig(struct Sema *s, struct EffectSig *sig) {
  printf("<");
  bool empty = !sig || !sig->terms || sig->terms->count == 0;
  if (empty && !(sig && sig->is_open)) {
    printf("pure");
  } else {
    if (sig && sig->terms) {
      for (size_t i = 0; i < sig->terms->count; i++) {
        struct EffectTerm *term = (struct EffectTerm *)vec_get(sig->terms, i);
        print_effect_term(s, term, i == 0);
      }
    }
    if (sig && sig->is_open) {
      const char *row_name =
          sig->row_name_id ? pool_get(s->pool, sig->row_name_id, 0) : NULL;
      printf(empty ? "| %s" : " | %s", row_name ? row_name : "?");
    }
  }
  printf(">");
}