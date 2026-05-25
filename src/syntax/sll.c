#include "sll.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

// =====================================================================
// SLL — sorted intrusive circular doubly-linked list.
//
// Direct port of rowan/src/sll.rs. Pre-conditions and asserts mirror
// rowan's debug_assert checks 1:1; in C they fire in all builds since
// the cost is negligible compared to the surrounding tree walks.
// =====================================================================

SllAddResult sll_link(SllNode **head, const SllNode *elem) {
    assert(head != NULL);
    SllNode *old_head = *head;

    // Case 1: empty ring → become the head.
    if (old_head == NULL) {
        return (SllAddResult){.kind = SLL_ADD_EMPTY_HEAD, .head_slot = head};
    }

    // Case 2: smaller than head → become the new head.
    if (elem->key < old_head->key) {
        return (SllAddResult){.kind = SLL_ADD_SMALLER_THAN_HEAD, .head_slot = head};
    }

    // Case 3: walk backward from head->prev (the largest element) toward
    // head, finding the first predecessor with key < elem->key. Because
    // Case 2 already rejected elem.key < head.key, the walk MUST
    // terminate at old_head at the latest (old_head has the smallest
    // key in the ring).
    SllNode *curr = old_head->prev;
    for (;;) {
        if (curr->key < elem->key) {
            return (SllAddResult){.kind = SLL_ADD_SMALLER_THAN_NOT_HEAD, .curr = curr};
        }
        if (curr->key == elem->key) {
            return (SllAddResult){.kind = SLL_ADD_ALREADY_IN_SLL, .curr = curr};
        }
        curr = curr->prev;
    }
}

void sll_add(SllAddResult res, SllNode *elem) {
    // Always self-link first. For EMPTY_HEAD this is the final state;
    // for splicing cases below, this is a placeholder that the splice
    // overwrites.
    elem->prev = elem;
    elem->next = elem;

    switch (res.kind) {
        case SLL_ADD_EMPTY_HEAD:
            *res.head_slot = elem;
            break;

        case SLL_ADD_SMALLER_THAN_HEAD: {
            SllNode *old_head = *res.head_slot;
            SllNode *prev_of_head = old_head->prev;
            // Splice elem in front of old_head (and after old_head->prev,
            // which wraps to the largest element in the ring).
            elem->prev = prev_of_head;
            elem->next = old_head;
            prev_of_head->next = elem;
            old_head->prev = elem;
            *res.head_slot = elem;
            break;
        }

        case SLL_ADD_SMALLER_THAN_NOT_HEAD: {
            SllNode *curr = res.curr;
            SllNode *next = curr->next;
            elem->prev = curr;
            elem->next = next;
            curr->next = elem;
            next->prev = elem;
            break;
        }

        case SLL_ADD_ALREADY_IN_SLL:
            // Caller's choice: most callers panic; we silently drop here.
            // The result tells callers what happened so they can handle.
            break;
    }
}

void sll_unlink(SllNode **head, SllNode *elem) {
    assert(head != NULL);
    assert(*head != NULL && "sll_unlink: ring is empty");

    SllNode *prev = elem->prev;
    SllNode *next = elem->next;

    // Defensive: dangling links → self. Catches use-after-unlink at
    // dereference time rather than at next-mutation time.
    elem->prev = elem;
    elem->next = elem;

    if (prev == elem) {
        // Was a 1-element ring; head becomes empty.
        assert(next == elem);
        assert(*head == elem);
        *head = NULL;
        return;
    }

    assert(prev->next == elem && "sll_unlink: corrupt ring (prev->next)");
    assert(next->prev == elem && "sll_unlink: corrupt ring (next->prev)");

    prev->next = next;
    next->prev = prev;
    if (*head == elem) {
        *head = next;
    }
}

void sll_adjust(SllNode *elem, uint32_t from, int32_t delta) {
    SllNode *curr = elem;
    do {
        if (curr->key >= from) {
            int64_t k = (int64_t)curr->key + (int64_t)delta;
            assert(k >= 0 && "sll_adjust: key would underflow");
            curr->key = (uint32_t)k;
        }
        curr = curr->next;
    } while (curr != elem);
}
