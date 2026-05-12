#ifndef ORE_SEMA_TYPE_DUMP_H
#define ORE_SEMA_TYPE_DUMP_H

#include "../ids/ids.h"

struct Sema;

// Walk top-level decls in `mid` and print each one's resolved type
// alongside its const-evaluated value (or `<not constant>`). Drives
// query_type_of_decl + query_const_eval. Used by `--dump-tyck`.
void dump_tyck(struct Sema *s, ModuleId mid);

#endif
