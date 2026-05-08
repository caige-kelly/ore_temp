#ifndef ORE_SEMA_EVAL_DUMP_H
#define ORE_SEMA_EVAL_DUMP_H

#include "../ids/ids.h"

struct Sema;

// Walk top-level binds in `mid` and print each one's evaluated
// constant value (or `<not constant>`). Drives query_const_eval.
// Used by `--dump-const-eval`.
void dump_const_eval(struct Sema *s, ModuleId mid);

#endif
