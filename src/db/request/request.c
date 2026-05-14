#include "request.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "../db.h"
#include "../storage/arena.h"
#include "../storage/vec.h"

/*
    See request.h for the LSP-correctness contract. This file is the
    extern-only implementation of those five functions. None of them
    are hot enough to inline against the header convention established
    by ids/intern_pool — state-touching functions stay extern.
*/

void db_request_begin(struct db *s, uint64_t revision) {
    assert(s != NULL);
    assert(revision != 0 && "revision 0 is the unpinned sentinel; pass current_revision or later");
    assert(s->request_revision == 0 && "nested db_request_begin — must db_request_end first");
    assert(s->query_stack.count == 0 && "request begin while a query is still on the stack");

    s->request_revision = revision;
    atomic_store(&s->cancel_requested, false);
    arena_reset(&s->request_arena);
}

void db_request_end(struct db *s) {
    assert(s != NULL);
    assert(s->query_stack.count == 0 && "request end while a query is still on the stack");

    s->request_revision = 0;
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
    return s->request_revision != 0 ? s->request_revision : s->current_revision;
}
