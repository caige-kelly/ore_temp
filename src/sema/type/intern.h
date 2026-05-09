#ifndef ORE_SEMA_TYPE_INTERN_H
#define ORE_SEMA_TYPE_INTERN_H

// Compound-type interners.
//
// Each compound TypeKind (TY_FN, TY_PTR, TY_SLICE, TY_ARRAY) gets a
// hashmap on Sema that canonicalizes by structural content. Two
// requests for the same shape return the same Type*; type equality
// reduces to pointer equality everywhere downstream.
//
// The interners are append-only — a Type, once interned, lives for
// the Sema's lifetime. The hashmaps key on a content hash; collisions
// are resolved by walking same-bucket entries and structural-equal
// comparison.
//
// Public API is `type_fn` / `type_ptr` / `type_slice` / `type_array`
// in type.h. This file defines the storage and init.

struct Sema;

// Initialize the compound-type interner hashmaps. Called from
// sema_types_init. Idempotent.
void sema_type_interns_init(struct Sema *s);

#endif
