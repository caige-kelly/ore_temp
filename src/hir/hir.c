// HIR — constructors and the kind-name table.
//
// Phase B keeps this minimal: arena-allocated zero-initialized nodes
// plus a string table for diagnostics. Lowering and consumers come in
// later phases.

#include "hir.h"

#include <string.h>

#include "../common/arena.h"
#include "../common/vec.h"

struct HirInstr *hir_instr_new(Arena *arena, HirInstrKind kind,
                               struct Span span) {
  if (!arena)
    return NULL;
  struct HirInstr *h = arena_alloc(arena, sizeof(struct HirInstr));
  if (!h)
    return NULL;
  memset(h, 0, sizeof(*h));
  h->kind = kind;
  h->span = span;
  h->semantic_kind = SEM_UNKNOWN;
  return h;
}

struct HirFn *hir_fn_new(Arena *arena, DefId source, struct Span span) {
  if (!arena)
    return NULL;
  struct HirFn *fn = arena_alloc(arena, sizeof(struct HirFn));
  if (!fn)
    return NULL;
  memset(fn, 0, sizeof(*fn));
  fn->source = source;
  fn->span = span;
  fn->params = vec_new_in(arena, sizeof(struct HirParam *));
  fn->body_block = vec_new_in(arena, sizeof(struct HirInstr *));
  return fn;
}

struct HirModule *hir_module_new(Arena *arena, ModuleId source) {
  if (!arena)
    return NULL;
  struct HirModule *mod = arena_alloc(arena, sizeof(struct HirModule));
  if (!mod)
    return NULL;
  memset(mod, 0, sizeof(*mod));
  mod->source = source;
  mod->functions = vec_new_in(arena, sizeof(struct HirFn *));
  return mod;
}

struct HirInstr *hir_make_ref(Arena *arena, DefId def, struct Span span) {
  struct HirInstr *h = hir_instr_new(arena, HIR_REF, span);
  if (!h)
    return NULL;
  h->ref.def = def;
  return h;
}

struct HirInstr *hir_make_field(Arena *arena, struct HirInstr *object,
                                DefId field_def, uint32_t field_name_id,
                                struct Span span) {
  struct HirInstr *h = hir_instr_new(arena, HIR_FIELD, span);
  if (!h)
    return NULL;
  h->field.object = object;
  h->field.field_def = field_def;
  h->field.field_name_id = field_name_id;
  return h;
}

struct HirInstr *hir_make_op_perform(Arena *arena, DefId effect_def,
                                     DefId op_def, Vec *args,
                                     struct Span span) {
  struct HirInstr *h = hir_instr_new(arena, HIR_OP_PERFORM, span);
  if (!h)
    return NULL;
  h->op_perform.effect_def = effect_def;
  h->op_perform.op_def = op_def;
  h->op_perform.args = args;
  return h;
}

struct HirInstr *hir_make_bind(Arena *arena, DefId def, struct HirInstr *init,
                               struct Span span) {
  struct HirInstr *h = hir_instr_new(arena, HIR_BIND, span);
  if (!h)
    return NULL;
  h->bind.def = def;
  h->bind.init = init;
  return h;
}

const char *hir_kind_str(HirInstrKind kind) {
  switch (kind) {
  case HIR_CONST:
    return "Const";
  case HIR_REF:
    return "Ref";
  case HIR_BIN:
    return "Bin";
  case HIR_UNARY:
    return "Unary";
  case HIR_CALL:
    return "Call";
  case HIR_FIELD:
    return "Field";
  case HIR_INDEX:
    return "Index";
  case HIR_IF:
    return "If";
  case HIR_LOOP:
    return "Loop";
  case HIR_SWITCH:
    return "Switch";
  case HIR_BIND:
    return "Bind";
  case HIR_ASSIGN:
    return "Assign";
  case HIR_RETURN:
    return "Return";
  case HIR_BREAK:
    return "Break";
  case HIR_CONTINUE:
    return "Continue";
  case HIR_DEFER:
    return "Defer";
  case HIR_HANDLER_INSTALL:
    return "HandlerInstall";
  case HIR_HANDLER_VALUE:
    return "HandlerValue";
  case HIR_OP_PERFORM:
    return "OpPerform";
  case HIR_PRODUCT:
    return "Product";
  case HIR_ARRAY_LIT:
    return "ArrayLit";
  case HIR_ENUM_REF:
    return "EnumRef";
  case HIR_ASM:
    return "Asm";
  case HIR_TYPE_VALUE:
    return "TypeValue";
  case HIR_BUILTIN:
    return "Builtin";
  case HIR_LAMBDA:
    return "Lambda";
  case HIR_ERROR:
    return "Error";
  }
  return "?";
}
