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
#include "fits.h"
#include "type.h"

void dump_tyck(struct Sema *s, ModuleId mid) {
  struct ModuleInfo *m = module_info(s, mid);
  if (!m) {
    printf("=== tyck === <invalid module>\n");
    return;
  }
  Vec *idx = m->top_level_index;
  printf("=== tyck === module=%u top_level=%zu\n", mid.idx,
         idx ? idx->count : 0);
  if (!idx) return;

  for (size_t i = 0; i < idx->count; i++) {
    struct TopLevelEntry *e = (struct TopLevelEntry *)vec_get(idx, i);
    if (!e || !e->node) continue;
    if (e->node->kind != expr_Bind) continue;

    DefId def = query_def_for_name(s, mid, e->name_id);
    struct Type *t = query_type_of_decl(s, def);
    const char *name = pool_get(&s->pool, e->name_id, 0);
    const char *tname = type_name(t);

    struct Expr *value = e->node->bind.value;
    struct ConstValue v = value ? query_const_eval(s, value)
                                : (struct ConstValue){.kind = CONST_NONE};

    char vbuf[64];
    const char *vstr = const_value_to_str(v, vbuf, sizeof(vbuf));

    if (t->kind == TY_ERROR) {
      printf("    %-16s : %-14s = %s   <error>\n",
             name ? name : "?", tname, vstr);
    } else {
      printf("    %-16s : %-14s = %s\n",
             name ? name : "?", tname, vstr);
    }
  }
}
