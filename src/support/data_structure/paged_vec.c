#include "paged_vec.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

// Page-pointer array starts at 4 entries and doubles on demand. Tiny
// initial allocation because most slot columns will only ever hold a
// handful of pages.
#define PAGED_INITIAL_PAGE_CAP 4u

static void grow_page_array(PagedVec *p, size_t min_page_cap) {
  size_t new_cap = p->page_cap < PAGED_INITIAL_PAGE_CAP ? PAGED_INITIAL_PAGE_CAP
                                                        : p->page_cap * 2;
  while (new_cap < min_page_cap)
    new_cap *= 2;

  void **new_pages = realloc(p->pages, new_cap * sizeof(*new_pages));
  if (!new_pages)
    abort(); // OOM on long-lived storage is unrecoverable

  // Zero the freshly-added slots so an uninitialized page pointer is
  // recognizable in debuggers and a partial-init failure-recovery
  // path could free only the pages we actually allocated.
  for (size_t i = p->page_cap; i < new_cap; i++)
    new_pages[i] = NULL;

  p->pages = new_pages;
  p->page_cap = new_cap;
}

void paged_init(PagedVec *p, size_t element_size) {
  assert(element_size > 0 && "paged_init: element_size must be > 0");
  p->pages = NULL;
  p->page_count = 0;
  p->page_cap = 0;
  p->element_size = element_size;
  atomic_store_explicit(&p->count, 0, memory_order_relaxed);
}

void paged_free(PagedVec *p) {
  for (size_t i = 0; i < p->page_count; i++) {
    free(p->pages[i]);
  }
  free(p->pages);
  p->pages = NULL;
  p->page_count = 0;
  p->page_cap = 0;
  atomic_store_explicit(&p->count, 0, memory_order_relaxed);
  // Keep element_size so re-init isn't required for reuse.
}

void *paged_get(const PagedVec *p, size_t index) {
  size_t cur = atomic_load_explicit(&p->count, memory_order_relaxed);
  if (index >= cur)
    return NULL;
  size_t page_idx = index >> PAGED_PAGE_LEN_BITS;
  size_t slot_idx = index & PAGED_PAGE_LEN_MASK;
  return (char *)p->pages[page_idx] + slot_idx * p->element_size;
}

// Internal: ensure the page that holds `index` is allocated. Returns
// a pointer to the slot's bytes within the page (NOT to-element-typed;
// caller writes via memcpy or memset).
static void *ensure_slot(PagedVec *p, size_t index) {
  size_t page_idx = index >> PAGED_PAGE_LEN_BITS;
  size_t slot_idx = index & PAGED_PAGE_LEN_MASK;

  if (page_idx >= p->page_count) {
    if (page_idx >= p->page_cap) {
      grow_page_array(p, page_idx + 1);
    }
    // Allocate every page from the last-known page up to and
    // including page_idx. In normal append-only use, this loop
    // runs at most once (we push past a single page boundary
    // at a time), but it's correct for any growth pattern.
    while (p->page_count <= page_idx) {
      void *page = calloc(PAGED_PAGE_LEN, p->element_size);
      if (!page)
        abort();
      p->pages[p->page_count++] = page;
    }
  }
  return (char *)p->pages[page_idx] + slot_idx * p->element_size;
}

size_t paged_push(PagedVec *p, const void *element) {
  size_t idx = atomic_load_explicit(&p->count, memory_order_relaxed);
  void *dest = ensure_slot(p, idx);
  memcpy(dest, element, p->element_size);
  // Release: any reader that loads count >= idx+1 with Acquire must
  // see the memcpy above. memory_order_release matches Salsa's
  // pattern (table.rs:407-434).
  atomic_store_explicit(&p->count, idx + 1, memory_order_release);
  return idx;
}

size_t paged_push_zero(PagedVec *p) {
  size_t idx = atomic_load_explicit(&p->count, memory_order_relaxed);
  // ensure_slot calloc's new pages, so the slot is already zero
  // unless we're reusing a slot post-clear. memset for safety.
  void *dest = ensure_slot(p, idx);
  memset(dest, 0, p->element_size);
  atomic_store_explicit(&p->count, idx + 1, memory_order_release);
  return idx;
}

void paged_clear(PagedVec *p) {
  atomic_store_explicit(&p->count, 0, memory_order_relaxed);
  // Pages stay allocated; their contents become "previous-content"
  // (callers shouldn't read stale data, and paged_get bounds-checks
  // against count so it won't expose anything beyond 0).
}
