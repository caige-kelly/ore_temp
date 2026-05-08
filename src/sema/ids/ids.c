#include "ids.h"

#include <stddef.h>

#include "../../common/vec.h"
#include "../sema.h"

// Table representation: each ID family owns a Vec<void*> on Sema.
// Storing pointers (rather than embedding info structs by value) lets
// each layer define its own info struct in its own header without
// ids.c needing the full definition. The cost is one extra
// indirection per lookup; the benefit is layer independence.

void sema_ids_init(struct Sema *s) {
  if (s->defs_table)
    return;

  s->defs_table = vec_new_in(s->arena, sizeof(void *));
  s->scopes_table = vec_new_in(s->arena, sizeof(void *));
  s->modules_table = vec_new_in(s->arena, sizeof(void *));
  s->bodies_table = vec_new_in(s->arena, sizeof(void *));

  void *placeholder = NULL;
  vec_push(s->defs_table, &placeholder);
  vec_push(s->scopes_table, &placeholder);
  vec_push(s->modules_table, &placeholder);
  vec_push(s->bodies_table, &placeholder);
}

static uint32_t intern_into(Vec *tab, void *info) {
  vec_push(tab, &info);
  return (uint32_t)(tab->count - 1);
}

DefId sema_intern_def(struct Sema *s, struct DefInfo *info) {
  return (DefId){intern_into(s->defs_table, info)};
}

ScopeId sema_intern_scope(struct Sema *s, struct ScopeInfo *info) {
  return (ScopeId){intern_into(s->scopes_table, info)};
}

ModuleId sema_intern_module(struct Sema *s, struct ModuleInfo *info) {
  return (ModuleId){intern_into(s->modules_table, info)};
}

BodyId sema_intern_body(struct Sema *s, struct BodyInfo *info) {
  return (BodyId){intern_into(s->bodies_table, info)};
}

static void *lookup(Vec *tab, uint32_t idx) {
  if (!tab || idx == 0 || idx >= tab->count)
    return NULL;
  void **slot = (void **)vec_get(tab, idx);
  return slot ? *slot : NULL;
}

struct DefInfo *def_info(struct Sema *s, DefId id) {
  return (struct DefInfo *)lookup(s->defs_table, id.idx);
}

struct ScopeInfo *scope_info(struct Sema *s, ScopeId id) {
  return (struct ScopeInfo *)lookup(s->scopes_table, id.idx);
}

struct ModuleInfo *module_info(struct Sema *s, ModuleId id) {
  return (struct ModuleInfo *)lookup(s->modules_table, id.idx);
}

struct BodyInfo *body_info(struct Sema *s, BodyId id) {
  return (struct BodyInfo *)lookup(s->bodies_table, id.idx);
}

uint32_t sema_def_count(struct Sema *s) {
  return s->defs_table ? (uint32_t)s->defs_table->count : 0;
}

uint32_t sema_scope_count(struct Sema *s) {
  return s->scopes_table ? (uint32_t)s->scopes_table->count : 0;
}

uint32_t sema_module_count(struct Sema *s) {
  return s->modules_table ? (uint32_t)s->modules_table->count : 0;
}

uint32_t sema_body_count(struct Sema *s) {
  return s->bodies_table ? (uint32_t)s->bodies_table->count : 0;
}
