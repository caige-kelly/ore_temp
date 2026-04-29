#ifndef ORE_SEMA_DECLS_H
#define ORE_SEMA_DECLS_H

#include <stdbool.h>

#include "sema.h"

bool sema_collect_declarations(struct Sema* sema);
struct Type* sema_type_of_decl(struct Sema* sema, struct Decl* decl);
struct Type* sema_signature_of_decl(struct Sema* sema, struct Decl* decl);
struct Type* sema_type_from_decl(struct Sema* sema, struct Decl* decl);

#endif // ORE_SEMA_DECLS_H
