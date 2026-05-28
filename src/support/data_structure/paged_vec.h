#ifndef ORE_SUPPORT_PAGED_VEC_H
#define ORE_SUPPORT_PAGED_VEC_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

// =====================================================================
// PagedVec<T> — fixed-page segmented vector with pointer-stable storage.
//
// Append-only typed container. Memory layout: a dynamic array of page
// pointers, where each page is a contiguous block of PAGED_PAGE_LEN
// elements of `element_size` bytes (1024 elements per page, Salsa-style).
// Pages, once allocated, are never moved — pointers returned by
// paged_get remain valid for the lifetime of the PagedVec.
//
// Use this in place of Vec wherever readers may hold a pointer across
// a subsequent push (slot tables, result columns with pointer-returning
// accessors). Source text, token streams, transient buffers — keep Vec.
//
// O(1) index lookup via packed addressing: page = i >> 10; slot = i & 0x3FF.
// O(1) amortized push (occasional page allocation; the page pointer
// array also grows by doubling, but readers don't hold pointers INTO
// that array).
//
// Concurrency-prepared (not concurrent today). `count` is _Atomic so a
// single producer can publish to readers on other threads via the
// memory_order_relaxed default. Single-threaded cost is negligible
// (compiles to plain load/store on x86; relaxed atomics on ARM).
// Multi-producer push is not implemented — would need a CAS loop on
// count plus race-free page allocation, deferred until the parallel-
// query phase needs it.
//
// What this is NOT:
//   - NOT a malloc replacement (that's Vec).
//   - NOT for variable-size elements (every element is element_size).
//   - NOT a free-list (pop / element removal is intentionally absent).
//   - NOT a pool (no per-page lifetime hooks, no reuse of freed pages).
//
// Patterns adopted from production references:
//   - Salsa's fixed 1024-slot pages with (page << 10) | slot ID encoding
//     (salsa/src/table.rs:103-121, lines 538-551).
//   - Zig's lazy per-shelf allocation (lib/std/segmented_list.zig:206-227),
//     adapted to fixed page size for simpler indexing.
// =====================================================================

#define PAGED_PAGE_LEN_BITS  10u
#define PAGED_PAGE_LEN       (1u << PAGED_PAGE_LEN_BITS)   // 1024 elements
#define PAGED_PAGE_LEN_MASK  (PAGED_PAGE_LEN - 1u)

typedef struct {
    void           **pages;         // page pointer array (heap-allocated, resizable)
    size_t           page_count;    // pages currently allocated (live entries in pages[])
    size_t           page_cap;      // capacity of pages[] (resizes by doubling)
    size_t           element_size;  // bytes per element (set at init)
    _Atomic size_t   count;         // total element count across all pages
} PagedVec;

// Initialize an empty PagedVec for elements of `element_size` bytes.
// No allocation happens here — the first push allocates the first page.
void paged_init(PagedVec *p, size_t element_size);

// Release every page and the page pointer array. The PagedVec struct
// itself is left zero-initialized (safe to re-init or destroy).
void paged_free(PagedVec *p);

// Return a borrowed pointer to element[index]. Pointer is STABLE for
// the lifetime of the PagedVec (subsequent pushes do not invalidate).
// Returns NULL if index >= count.
void *paged_get(const PagedVec *p, size_t index);

// Append a copy of *element. Returns the new element's index.
// Allocates a new page on page-boundary crossings.
size_t paged_push(PagedVec *p, const void *element);

// Append a zero-initialized element. Returns the new element's index.
// Pages are calloc'd, so no separate zeroing is needed; this is faster
// than paged_push(&zero_buffer) when the caller doesn't have an element
// value already on hand.
size_t paged_push_zero(PagedVec *p);

// Reset count to 0 without releasing pages. Pages stay allocated for
// reuse on subsequent pushes (the same memory backs new elements;
// callers who held pointers to previous-content elements still see
// stable addresses, though the contents may now be overwritten).
void paged_clear(PagedVec *p);

// Atomic read of count. Inline so calls in hot loops compile to a
// single load. memory_order_relaxed: callers that need ordering with
// respect to specific element writes should add their own fences.
#include <stdatomic.h>
static inline size_t paged_count(const PagedVec *p) {
    return atomic_load_explicit(&p->count, memory_order_relaxed);
}

#endif // ORE_SUPPORT_PAGED_VEC_H
