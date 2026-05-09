#include "dump.h"

#include <stdio.h>

#include "../../common/stringpool.h"
#include "../../common/vec.h"
#include "../../parser/ast.h"
#include "../eval/const_eval.h"
#include "../modules/def_map.h"
#include "../modules/modules.h"
#include "../sema.h"
#include "checker.h"
#include "display.h"
#include "expr_check.h"
#include "fits.h"
#include "type.h"

void dump_tyck(struct Sema *s, ModuleId mid) {
  struct ModuleInfo *m = module_info(s, mid);
  if (!m) {
    printf("=== tyck === <invalid module>\n");
    return;
  }
  Vec *idx = query_top_level_index(s, mid);
  printf("=== tyck === module=%u top_level=%zu\n", mid.idx,
         idx ? idx->count : 0);
  if (!idx) return;

  char tbuf[256], vbuf[64];

  for (size_t i = 0; i < idx->count; i++) {
    struct TopLevelEntry *e = (struct TopLevelEntry *)vec_get(idx, i);
    if (!e || !e->node) continue;
    if (e->node->kind != expr_Bind) continue;

    DefId def = query_def_for_name(s, mid, e->name_id);
    struct Type *t = query_type_of_def(s, def);
    const char *name = pool_get(&s->pool, e->name_id, 0);
    const char *tname = type_to_string(s, t, tbuf, sizeof(tbuf));

    struct Expr *value = e->node->bind.value;
    // For value expressions, also kick query_type_of_expr so any
    // body errors (mismatched call args, etc.) flow through to
    // diagnostics. We don't show the per-expr types in the dump
    // (they'd flood the output); the dump just records the
    // top-level decl's type. The diagnostics emitted during the
    // walk are the value of running this.
    if (value) (void)query_type_of_expr(s, value);

    struct ConstValue v = value ? query_const_eval(s, value)
                                : (struct ConstValue){.kind = CONST_NONE};
    const char *vstr = const_value_to_str(v, vbuf, sizeof(vbuf));

    if (t->kind == TY_ERROR) {
      printf("    %-16s : %-22s = %s   <error>\n",
             name ? name : "?", tname, vstr);
    } else {
      printf("    %-16s : %-22s = %s\n",
             name ? name : "?", tname, vstr);
    }
  }
}
