#ifndef ORE_SYNTAX_SLL_H
#define ORE_SYNTAX_SLL_H

// =====================================================================
// SLL — Sorted (by key, ascending) intrusive circular doubly-linked list.
//
// Port of rowan/src/sll.rs. Used by mutable SyntaxNodes to track all
// live child handles attached to a given parent so that mutations
// (detach / attach / splice) can keep every handle's index field in
// sync via a single ring walk.
//
// The list is INTRUSIVE: callers embed `SllNode` in their own struct
// and use `container_of`-style arithmetic to recover the outer pointer.
// The standalone test uses a dedicated `TestElem { SllNode sll; ... }`
// to demonstrate that SLL has no NodeData coupling.
//
// All operations are O(ring_size) worst case. No allocation; no recursion.
// =====================================================================

#include <stdint.h>

typedef struct SllNode SllNode;
struct SllNode {
    SllNode *prev;
    SllNode *next;
    uint32_t key;
};

// `sll_link` walks the ring (without mutating it) and reports where /
// whether to insert. Callers then pass the result to `sll_add` to
// actually perform the splice. Splitting find-where from apply mirrors
// rowan's API and surfaces the rare "duplicate key" case explicitly.
typedef enum {
    SLL_ADD_EMPTY_HEAD,             // ring is empty; elem becomes the head
    SLL_ADD_SMALLER_THAN_HEAD,      // elem's key < head's key; elem becomes new head
    SLL_ADD_SMALLER_THAN_NOT_HEAD,  // insert elem immediately after `curr`
    SLL_ADD_ALREADY_IN_SLL,         // a node with this key already exists; no insert
} SllAddKind;

typedef struct {
    SllAddKind   kind;
    SllNode    **head_slot;  // valid for EMPTY_HEAD / SMALLER_THAN_HEAD
    SllNode     *curr;       // valid for SMALLER_THAN_NOT_HEAD / ALREADY_IN_SLL
} SllAddResult;

// Find the insertion point for `elem` in the ring anchored at `*head`.
// Does NOT mutate. Caller is responsible for `elem->key` being set before
// the call. `head` must be a valid pointer; *head may be NULL (empty ring).
SllAddResult sll_link(SllNode **head, const SllNode *elem);

// Apply a result from `sll_link`. Self-links `elem` first; then splices
// into the ring per the result's kind. ALREADY_IN_SLL is a no-op (caller
// chose to drop the duplicate).
void sll_add(SllAddResult res, SllNode *elem);

// Convenience for "link + add unconditionally". Returns the result so
// callers can still detect ALREADY_IN_SLL if they care.
static inline SllAddResult sll_init(SllNode **head, SllNode *elem) {
    SllAddResult r = sll_link(head, elem);
    sll_add(r, elem);
    return r;
}

// Remove `elem` from its ring. *head is updated to a successor (or NULL)
// if elem was the head. After the call, elem->prev = elem->next = elem
// (defensive self-link so accidental dereferences during the same edit
// don't UAF). Precondition: *head must be non-NULL.
void sll_unlink(SllNode **head, SllNode *elem);

// Walk elem's ring and add `delta` to every key >= `from`. `delta` may
// be negative; key arithmetic uses int64 to safely handle wraparound.
// Used after a child splice: every later sibling's index shifts by ±1.
void sll_adjust(SllNode *elem, uint32_t from, int32_t delta);

#endif  // ORE_SYNTAX_SLL_H
