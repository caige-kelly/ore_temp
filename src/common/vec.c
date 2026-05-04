#include "./vec.h"
#include <stdlib.h>
#include <string.h>

// Initializes a vector for use with standard malloc/free.
void vec_init(Vec *vec, size_t element_size) {
  vec->data = NULL;
  vec->count = 0;
  vec->capacity = 0;
  vec->element_size = element_size;
  vec->arena = NULL; // Explicitly set to NULL for malloc/realloc mode
}

// Initializes a vector for use with an arena allocator.
void vec_init_in(Vec *vec, Arena *arena, size_t element_size) {
  vec->data = NULL; // lazy allocate on first push
  vec->count = 0;
  vec->capacity = 0;
  vec->element_size = element_size;
  vec->arena = arena; // Set the arena for arena mode
}

// Creates a new Vec *itself* from an arena.
Vec *vec_new_in(Arena *arena, size_t element_size) {
  Vec *v = arena_alloc(arena, sizeof(Vec));
  vec_init_in(v, arena, element_size);
  return v;
}

// The new, intelligent vec_push function.
void vec_push(Vec *vec, const void *element) {
  if (vec->count >= vec->capacity) {
    size_t old_capacity_bytes = vec->capacity * vec->element_size;
    size_t new_capacity = vec->capacity < 8 ? 8 : vec->capacity * 2;
    size_t new_capacity_bytes = new_capacity * vec->element_size;

    void *new_data;
    if (vec->arena != NULL) {
      // Arena allocation path: Allocate a new block and copy.
      new_data = arena_alloc(vec->arena, new_capacity_bytes);
      if (vec->data != NULL) {
        memcpy(new_data, vec->data, old_capacity_bytes);
      }
    } else {
      // Standard library allocation path.
      new_data = realloc(vec->data, new_capacity_bytes);
      if (new_data == NULL && new_capacity_bytes > 0) {
        // In a real program, you might want to handle this error.
        exit(1);
      }
    }
    vec->data = new_data;
    vec->capacity = new_capacity;
  }

  // Copy the new element into the vector
  void *dest = (char *)vec->data + vec->count * vec->element_size;
  memcpy(dest, element, vec->element_size);
  vec->count++;
}

void *vec_get(Vec *vec, size_t index) {
  if (index >= vec->count) {
    return NULL;
  }
  return (char *)vec->data + index * vec->element_size;
}

// Frees the data buffer *only* if the vector was not in arena mode.
void vec_free(Vec *vec) {
  if (vec->data != NULL && vec->arena == NULL) {
    free(vec->data);
  }
  vec->data = NULL;
  vec->capacity = 0;
  vec->count = 0;
}
