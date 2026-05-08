#include "dump.h"

#include <stdio.h>

#include "../../common/stringpool.h"
#include "../../common/vec.h"
#include "../../parser/ast.h"
#include "../modules/def_map.h"
#include "../modules/modules.h"
#include "../sema.h"
#include "const_eval.h"

void dump_const_eval(struct Sema *s, ModuleId mid) {
  struct ModuleInfo *m = module_info(s, mid);
  if (!m) {
    printf("=== const_eval === <invalid module>\n");
    return;
  }
  Vec *idx = m->top_level_index;
  printf("=== const_eval === module=%u top_level=%zu\n", mid.idx,
         idx ? idx->count : 0);
  if (!idx) return;

  for (size_t i = 0; i < idx->count; i++) {
    struct TopLevelEntry *e = (struct TopLevelEntry *)vec_get(idx, i);
    if (!e || !e->node) continue;
    if (e->node->kind != expr_Bind) continue;

    struct ConstValue v = query_const_eval(s, e->node->bind.value);
    const char *name = pool_get(s->pool, e->name_id, 0);
    if (v.kind == CONST_INT) {
      printf("    %s = %lld\n", name, (long long)v.int_val);
    } else if (v.kind == CONST_FLOAT) {
      printf("    %s = %f\n", name, v.float_val);
    } else {
      printf("    %s = <not constant>\n", name);
    }
  }
}
