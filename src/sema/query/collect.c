#include "collect.h"

#include "../../common/hashmap.h"
#include "../../common/vec.h"
#include "../eval/const_eval.h"
#include "../modules/def_map.h"
#include "../modules/inputs.h"
#include "../modules/modules.h"
#include "../resolve/resolve.h"
#include "../resolve/scope_index.h"
#include "../sema.h"
#include "../type/decl_data.h"
#include "../type/decl_info.h"
#include "../type/expr_check.h"
#include "../type/layout.h"

struct SlotCtx {
  SemaSlotVisitor visit;
  void *ud;
};

#define DEFINE_VISIT(name, EntryType, field)                                   \
  static bool name(uint64_t k, void *val, void *ud) {                          \
    (void)k;                                                                   \
    struct SlotCtx *sv = (struct SlotCtx *)ud;                                 \
    EntryType *e = (EntryType *)val;                                           \
    if (e)                                                                     \
      sv->visit(&e->field, sv->ud);                                            \
    return true;                                                               \
  }

DEFINE_VISIT(visit_decl_info, struct SemaDeclInfo, type_query)
DEFINE_VISIT(visit_type_of_expr, struct TypeOfExprEntry, query)
DEFINE_VISIT(visit_fn_signature, struct FnSignature, query)
DEFINE_VISIT(visit_struct_signature, struct StructSignature, query)
DEFINE_VISIT(visit_enum_signature, struct EnumSignature, query)
DEFINE_VISIT(visit_resolve_ref, struct ResolveRefEntry, query)
DEFINE_VISIT(visit_resolve_path, struct ResolvePathEntry, query)
DEFINE_VISIT(visit_scope_index, struct ScopeIndexResult, query)
DEFINE_VISIT(visit_const_eval, struct ConstEvalEntry, query)
DEFINE_VISIT(visit_is_comptime, struct IsComptimeEntry, query)
DEFINE_VISIT(visit_layout, struct LayoutEntry, query)
DEFINE_VISIT(visit_def_map_entry, struct DefMapEntry, query)

#undef DEFINE_VISIT

void sema_for_each_slot(struct Sema *s, SemaSlotVisitor visit, void *ud) {
  if (!s || !visit)
    return;
  struct SlotCtx sv = {.visit = visit, .ud = ud};

  hashmap_foreach(&s->decl_info, visit_decl_info, &sv);
  hashmap_foreach(&s->type_of_expr_entries, visit_type_of_expr, &sv);
  hashmap_foreach(&s->fn_signatures, visit_fn_signature, &sv);
  hashmap_foreach(&s->struct_signatures, visit_struct_signature, &sv);
  hashmap_foreach(&s->enum_signatures, visit_enum_signature, &sv);
  hashmap_foreach(&s->resolve_ref_entries, visit_resolve_ref, &sv);
  hashmap_foreach(&s->resolve_path_entries, visit_resolve_path, &sv);
  hashmap_foreach(&s->fn_scope_index_cache, visit_scope_index, &sv);
  hashmap_foreach(&s->const_eval_entries, visit_const_eval, &sv);
  hashmap_foreach(&s->is_comptime_entries, visit_is_comptime, &sv);
  hashmap_foreach(&s->layout_of_type, visit_layout, &sv);

  // Vec-based id tables: slot 0 is a NULL sentinel for the *_INVALID
  // accessor pattern; start at 1.
  if (s->inputs_table) {
    for (size_t i = 1; i < s->inputs_table->count; i++) {
      void *p = vec_get(s->inputs_table, i);
      if (!p)
        continue;
      struct InputInfo *info = *(struct InputInfo **)p;
      if (info)
        visit(&info->ast_query, ud);
    }
  }
  if (s->modules_table) {
    for (size_t i = 1; i < s->modules_table->count; i++) {
      void *p = vec_get(s->modules_table, i);
      if (!p)
        continue;
      struct ModuleInfo *mi = *(struct ModuleInfo **)p;
      if (!mi)
        continue;
      visit(&mi->def_map_query, ud);
      visit(&mi->exports_query, ud);
      visit(&mi->top_level_query, ud);
      visit(&mi->node_to_decl_index_query, ud);
      hashmap_foreach(&mi->def_map_entries, visit_def_map_entry, &sv);
    }
  }
}
