#include "request.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "../db.h"
#include "../storage/arena.h"
#include "../storage/vec.h"

void db_request_begin(struct db *s, uint64_t revision) {
    assert(s != NULL);
    assert(revision != 0 && "revision 0 is the unpinned sentinel; pass current_revision or later");
    assert(s->query_stack.count == 0 && "request begin while a query is still on the stack");

    uint64_t old = atomic_load(&s->rev_control);
    atomic_store(&s->rev_control, (old & ~REV_REQUEST_MASK) | (revision & REV_REQUEST_MASK));
    atomic_store(&s->cancel_requested, false);
    arena_reset(&s->request_arena);
}

void db_request_end(struct db *s) {
    assert(s != NULL);
    assert(s->query_stack.count == 0 && "request end while a query is still on the stack");

    uint64_t old = atomic_load(&s->rev_control);
    atomic_store(&s->rev_control, old & ~REV_REQUEST_MASK);
    arena_reset(&s->request_arena);
}

void db_request_cancel(struct db *s) {
    assert(s != NULL);
    atomic_store(&s->cancel_requested, true);
}

bool db_check_cancel(struct db *s) {
    assert(s != NULL);
    return atomic_load(&s->cancel_requested);
}

uint64_t db_effective_revision(struct db *s) {
    assert(s != NULL);
    uint64_t c = atomic_load(&s->rev_control);
    uint64_t r = c & REV_REQUEST_MASK;
    return r != 0 ? r : ((c & REV_CURRENT_MASK) >> 32);
}

uint64_t db_current_revision(struct db *s) {
    return (atomic_load(&s->rev_control) & REV_CURRENT_MASK) >> 32;
}

bool db_invalidation_enabled(struct db *s) {
    return (atomic_load(&s->rev_control) & REV_INVALIDATION_MASK) != 0;
}
