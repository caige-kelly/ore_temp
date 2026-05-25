#include "vec.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

void vec_init(Vec *v, size_t element_size) {
  *v = (Vec){
      .data = NULL,
      .count = 0,
      .capacity = 0,
      .element_size = element_size,
      .arena = NULL,
  };
}

void vec_init_in_arena(Vec *v, Arena *arena, size_t max_count,
                       size_t element_size) {
  assert(arena && "vec_init_in_arena: arena must be non-NULL");
  *v = (Vec){
      .data = (max_count > 0) ? arena_alloc_raw(arena, max_count * element_size)
                              : NULL,
      .count = 0,
      .capacity = max_count,
      .element_size = element_size,
      .arena = arena,
  };
}

// Grow the buffer to at least min_capacity. Only valid for malloc-backed
// Vecs; arena-backed Vecs fail their capacity assert before reaching
// here. Exposed (non-static) so the inline vec_push_slot in vec.h can
// reuse this one growth path.
void vec_grow(Vec *v, size_t min_capacity) {
  size_t new_capacity = v->capacity < 8 ? 8 : v->capacity * 2;
  while (new_capacity < min_capacity)
    new_capacity *= 2;

  size_t bytes = new_capacity * v->element_size;
  void *p = realloc(v->data, bytes);
  if (!p && bytes > 0) {
    // OOM on a long-lived collection is unrecoverable for a
    // compiler. Match hashmap's _or_die contract.
    abort();
  }

  v->data = p;
  v->capacity = new_capacity;
}

void vec_push(Vec *v, const void *element) {
  if (v->count == v->capacity) {
    if (v->arena) {
      // Arena-backed Vecs have a fixed capacity from init time.
      // Callers must size the slab generously (e.g. parser sizes
      // Big Four side-tables to token_count, an exact upper
      // bound on AST node count).
      assert(0 && "vec_push: arena-backed Vec is at capacity");
      return;
    }
    vec_grow(v, v->count + 1);
  }

  void *dest = (char *)v->data + v->count * v->element_size;
  memcpy(dest, element, v->element_size);
  v->count++;
}

// Bulk append: one growth check + one block memcpy of `n` contiguous
// elements. Replaces an N-call vec_push loop (e.g. ast_push_extra) —
// the per-element call + per-element memmove become a single copy.
// Malloc-backed contract identical to vec_push (arena asserts).
void vec_append_n(Vec *v, const void *elems, size_t n) {
  if (n == 0)
    return;
  if (v->count + n > v->capacity) {
    if (v->arena) {
      assert(0 && "vec_append_n: arena-backed Vec is at capacity");
      return;
    }
    vec_grow(v, v->count + n);
  }
  void *dest = (char *)v->data + v->count * v->element_size;
  memcpy(dest, elems, n * v->element_size);
  v->count += n;
}

void vec_reserve(Vec *v, size_t min_capacity) {
  // Capacity hint for malloc-backed Vecs: grow once up front so a
  // known-size append sequence never reallocs (kills doubling churn —
  // on macOS large realloc routes through a kernel mach_vm_copy).
  // No-op for arena-backed Vecs (fixed capacity by contract) and when
  // already large enough. Does not change count.
  if (v->arena)
    return;
  if (min_capacity > v->capacity)
    vec_grow(v, min_capacity);
}

void vec_push_zero(Vec *v) {
  if (v->count == v->capacity) {
    if (v->arena) {
      assert(0 && "vec_push_zero: arena-backed Vec is at capacity");
      return;
    }
    vec_grow(v, v->count + 1);
  }

  void *dest = (char *)v->data + v->count * v->element_size;
  memset(dest, 0, v->element_size);
  v->count++;
}

void *vec_get(Vec *v, size_t index) {
  if (index >= v->count)
    return NULL;
  return (char *)v->data + index * v->element_size;
}

void vec_clear(Vec *v) { v->count = 0; }

void vec_free(Vec *v) {
  if (v->arena == NULL) {
    free(v->data);
  }
  v->data = NULL;
  v->count = 0;
  v->capacity = 0;
  // Keep element_size + arena pointer so re-use after free is well-defined.
}
