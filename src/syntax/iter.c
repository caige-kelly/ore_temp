#include "syntax.h"

#include <assert.h>
#include <stdlib.h>

// =====================================================================
// Tree iterators.
//
// All iterators are cursor structs that the caller drives via _next.
// Each yielded handle is RETURNS_OWNED; the caller must release.
// The cursor itself owns any in-progress state and is freed via _free
// at the end (idempotent — _free a partially-consumed cursor releases
// whatever it still owns).
// =====================================================================


// =====================================================================
// SyntaxAncestors — yields start, start.parent, ..., root.
// =====================================================================

void syntax_ancestors_init(SyntaxAncestors *it, SyntaxNode *start) {
    it->current = start;
    if (start) syntax_node_retain(start);
    it->emitted_first = false;
}

SyntaxNode *syntax_ancestors_next(SyntaxAncestors *it) {
    if (!it->current) return NULL;
    if (!it->emitted_first) {
        it->emitted_first = true;
        // Hand out a +1 ref to caller, keep our own ref.
        syntax_node_retain(it->current);
        return it->current;
    }
    // Advance to parent.
    SyntaxNode *parent = syntax_node_parent(it->current);  // +1 ref
    syntax_node_release(it->current);
    it->current = parent;
    if (!parent) return NULL;
    syntax_node_retain(parent);
    return parent;
}

void syntax_ancestors_free(SyntaxAncestors *it) {
    if (it->current) {
        syntax_node_release(it->current);
        it->current = NULL;
    }
}


// =====================================================================
// SyntaxChildren — yields direct node children (skips tokens).
// =====================================================================

void syntax_children_init(SyntaxChildren *it, SyntaxNode *parent,
                           SyntaxDirection dir) {
    it->parent = parent;
    syntax_node_retain(parent);
    it->dir = dir;
    if (dir == SYNTAX_DIR_NEXT) {
        it->next_index = 0;
    } else {
        uint32_t count = syntax_node_num_children(parent);
        it->next_index = count;  // we decrement before use
    }
}

SyntaxNode *syntax_children_next(SyntaxChildren *it) {
    uint32_t count = syntax_node_num_children(it->parent);
    for (;;) {
        uint32_t idx;
        if (it->dir == SYNTAX_DIR_NEXT) {
            if (it->next_index >= count) return NULL;
            idx = it->next_index++;
        } else {
            if (it->next_index == 0) return NULL;
            idx = --it->next_index;
        }
        SyntaxNode *child = syntax_node_child(it->parent, idx);
        if (child) return child;  // node child
        // Token — skip and continue.
    }
}

void syntax_children_free(SyntaxChildren *it) {
    if (it->parent) {
        syntax_node_release(it->parent);
        it->parent = NULL;
    }
}


// =====================================================================
// SyntaxChildrenElem — yields all direct children (nodes + tokens).
// =====================================================================

void syntax_children_elem_init(SyntaxChildrenElem *it, SyntaxNode *parent,
                                SyntaxDirection dir) {
    it->parent = parent;
    syntax_node_retain(parent);
    it->dir = dir;
    if (dir == SYNTAX_DIR_NEXT) {
        it->next_index = 0;
    } else {
        uint32_t count = syntax_node_num_children(parent);
        it->next_index = count;
    }
}

SyntaxElement syntax_children_elem_next(SyntaxChildrenElem *it) {
    uint32_t count = syntax_node_num_children(it->parent);
    uint32_t idx;
    if (it->dir == SYNTAX_DIR_NEXT) {
        if (it->next_index >= count) return SYNTAX_ELEMENT_NONE;
        idx = it->next_index++;
    } else {
        if (it->next_index == 0) return SYNTAX_ELEMENT_NONE;
        idx = --it->next_index;
    }
    return syntax_node_child_or_token(it->parent, idx);
}

void syntax_children_elem_free(SyntaxChildrenElem *it) {
    if (it->parent) {
        syntax_node_release(it->parent);
        it->parent = NULL;
    }
}


// =====================================================================
// SyntaxPreorder — depth-first preorder with Enter/Leave events.
// =====================================================================
//
// State machine: each call to _next computes the next event from
// `current` + `descending`. Algorithm (rowan-style):
//
//   start:
//     emit Enter(root); current = root; descending = true.
//   loop:
//     if descending:
//       if current has children: descend into first; emit Enter(child).
//       else: emit Leave(current); descending = false.
//     else (just emitted Leave or we're moving right):
//       if current == root: finished.
//       else if current has a next sibling: move to it; emit Enter(sibling); descending = true.
//       else: move to parent; emit Leave(parent); descending = false.
//
// Tokens are leaves (no children, no Leave) — but we emit both Enter
// and Leave with the same range for symmetry (matches rowan's
// preorder_with_tokens). Consumers can filter by event.kind.

void syntax_preorder_init(SyntaxPreorder *it, SyntaxNode *root) {
    it->root = root;
    syntax_node_retain(root);
    it->current = (SyntaxElement){.kind = SYNTAX_ELEM_NODE, .node = NULL};
    it->descending = false;
    it->started = false;
    it->finished = false;
    it->skip_pending = false;
}

void syntax_preorder_skip_subtree(SyntaxPreorder *it) {
    // Only meaningful after an Enter event — i.e., when descending is true
    // and we have a real current. Calling at other times is a no-op.
    if (it->descending && !it->finished && !syntax_element_is_none(it->current)) {
        it->skip_pending = true;
    }
}

// Helper: get the first child of current (if it's a node).
// Returns NONE if no children.
static SyntaxElement preorder_first_child(SyntaxElement e) {
    if (e.kind != SYNTAX_ELEM_NODE || !e.node) return SYNTAX_ELEMENT_NONE;
    return syntax_node_first_child_or_token(e.node);
}

// Helper: next sibling of current (works on both nodes and tokens).
static SyntaxElement preorder_next_sibling(SyntaxElement e) {
    if (syntax_element_is_none(e)) return SYNTAX_ELEMENT_NONE;
    if (e.kind == SYNTAX_ELEM_NODE)
        return syntax_node_next_sibling_or_token(e.node);
    else
        return syntax_token_next_sibling_or_token(e.token);
}

// Helper: parent of current (returns a SyntaxNode wrapped in element).
// Returns NONE for the root node, since we don't ascend past it.
static SyntaxElement preorder_parent(SyntaxPreorder *it, SyntaxElement e) {
    SyntaxNode *parent = NULL;
    if (e.kind == SYNTAX_ELEM_NODE) {
        if (e.node == it->root) return SYNTAX_ELEMENT_NONE;
        parent = syntax_node_parent(e.node);
    } else {
        parent = syntax_token_parent(e.token);
    }
    if (!parent) return SYNTAX_ELEMENT_NONE;
    return (SyntaxElement){.kind = SYNTAX_ELEM_NODE, .node = parent};
}

// Helper: returns true if `e` is or is-equivalent-to root. Compares
// by (kind, range) since SyntaxNode handles aren't pointer-equal.
static bool preorder_is_root(SyntaxPreorder *it, SyntaxElement e) {
    if (e.kind != SYNTAX_ELEM_NODE || !e.node) return false;
    return syntax_node_kind(e.node) == syntax_node_kind(it->root) &&
           text_range_eq(syntax_node_text_range(e.node),
                          syntax_node_text_range(it->root));
}

// Helper: retain the embedded handle so the caller gets a fresh +1 ref.
static SyntaxElement retain_elem(SyntaxElement e) {
    if (e.kind == SYNTAX_ELEM_NODE && e.node) syntax_node_retain(e.node);
    else if (e.kind == SYNTAX_ELEM_TOKEN && e.token) syntax_token_retain(e.token);
    return e;
}

SyntaxWalkEvent syntax_preorder_next(SyntaxPreorder *it) {
    if (it->finished) return SYNTAX_WALK_EVENT_NONE;

    if (!it->started) {
        // First call: emit Enter(root).
        it->started = true;
        it->current = (SyntaxElement){.kind = SYNTAX_ELEM_NODE, .node = it->root};
        syntax_node_retain(it->root);  // it->current owns +1
        it->descending = true;
        return (SyntaxWalkEvent){
            .kind = SYNTAX_WALK_ENTER,
            .element = retain_elem(it->current),
        };
    }

    for (;;) {
        if (it->descending) {
            // skip_subtree: emit Leave(current) immediately without descending.
            if (it->skip_pending) {
                it->skip_pending = false;
                it->descending = false;
                return (SyntaxWalkEvent){
                    .kind = SYNTAX_WALK_LEAVE,
                    .element = retain_elem(it->current),
                };
            }
            SyntaxElement child = preorder_first_child(it->current);
            if (!syntax_element_is_none(child)) {
                // Descend.
                SYN_ELEM_RELEASE(it->current);
                it->current = child;
                // descending stays true; emit Enter.
                return (SyntaxWalkEvent){
                    .kind = SYNTAX_WALK_ENTER,
                    .element = retain_elem(it->current),
                };
            }
            // No children — emit Leave(current), then switch to "move right".
            SyntaxWalkEvent ev = {
                .kind = SYNTAX_WALK_LEAVE,
                .element = retain_elem(it->current),
            };
            it->descending = false;
            return ev;
        } else {
            // Just emitted Leave (or we're moving right). Check if current
            // is the root — if so, we're done.
            if (preorder_is_root(it, it->current)) {
                SYN_ELEM_RELEASE(it->current);
                it->finished = true;
                return SYNTAX_WALK_EVENT_NONE;
            }
            SyntaxElement sibling = preorder_next_sibling(it->current);
            if (!syntax_element_is_none(sibling)) {
                // Move right; emit Enter(sibling); descend on next call.
                SYN_ELEM_RELEASE(it->current);
                it->current = sibling;
                it->descending = true;
                return (SyntaxWalkEvent){
                    .kind = SYNTAX_WALK_ENTER,
                    .element = retain_elem(it->current),
                };
            }
            // No next sibling — move to parent and emit Leave(parent).
            SyntaxElement parent = preorder_parent(it, it->current);
            SYN_ELEM_RELEASE(it->current);
            if (syntax_element_is_none(parent)) {
                it->finished = true;
                return SYNTAX_WALK_EVENT_NONE;
            }
            it->current = parent;
            // Stay in "moving right" mode; emit Leave(parent).
            return (SyntaxWalkEvent){
                .kind = SYNTAX_WALK_LEAVE,
                .element = retain_elem(it->current),
            };
        }
    }
}

void syntax_preorder_free(SyntaxPreorder *it) {
    SYN_ELEM_RELEASE(it->current);
    if (it->root) {
        syntax_node_release(it->root);
        it->root = NULL;
    }
}


// =====================================================================
// SyntaxDescendants — preorder-filtered to nodes, excluding root.
// =====================================================================

void syntax_descendants_init(SyntaxDescendants *it, SyntaxNode *root) {
    syntax_preorder_init(&it->po, root);
}

SyntaxNode *syntax_descendants_next(SyntaxDescendants *it) {
    for (;;) {
        SyntaxWalkEvent ev = syntax_preorder_next(&it->po);
        if (syntax_walk_event_is_none(ev)) return NULL;
        if (ev.kind != SYNTAX_WALK_ENTER) {
            SYN_ELEM_RELEASE(ev.element);
            continue;
        }
        if (ev.element.kind != SYNTAX_ELEM_NODE) {
            SYN_ELEM_RELEASE(ev.element);
            continue;
        }
        // Skip the root itself.
        SyntaxNode *n = ev.element.node;
        if (syntax_node_kind(n) == syntax_node_kind(it->po.root) &&
            text_range_eq(syntax_node_text_range(n),
                           syntax_node_text_range(it->po.root))) {
            SYN_ELEM_RELEASE(ev.element);
            continue;
        }
        return n;  // caller owns
    }
}

void syntax_descendants_free(SyntaxDescendants *it) {
    syntax_preorder_free(&it->po);
}


// =====================================================================
// SyntaxDescendantsElem — preorder filtered to Enter events for both
// nodes and tokens. Excludes the root.
// =====================================================================

void syntax_descendants_elem_init(SyntaxDescendantsElem *it, SyntaxNode *root) {
    syntax_preorder_init(&it->po, root);
}

SyntaxElement syntax_descendants_elem_next(SyntaxDescendantsElem *it) {
    for (;;) {
        SyntaxWalkEvent ev = syntax_preorder_next(&it->po);
        if (syntax_walk_event_is_none(ev)) return SYNTAX_ELEMENT_NONE;
        if (ev.kind != SYNTAX_WALK_ENTER) {
            SYN_ELEM_RELEASE(ev.element);
            continue;
        }
        // Skip the root itself.
        if (ev.element.kind == SYNTAX_ELEM_NODE) {
            SyntaxNode *n = ev.element.node;
            if (syntax_node_kind(n) == syntax_node_kind(it->po.root) &&
                text_range_eq(syntax_node_text_range(n),
                               syntax_node_text_range(it->po.root))) {
                SYN_ELEM_RELEASE(ev.element);
                continue;
            }
        }
        return ev.element;
    }
}

void syntax_descendants_elem_free(SyntaxDescendantsElem *it) {
    syntax_preorder_free(&it->po);
}


// =====================================================================
// SyntaxSiblings — yields self, then walks in `dir` direction.
// =====================================================================

void syntax_siblings_init(SyntaxSiblings *it, SyntaxNode *start, SyntaxDirection dir) {
    it->cur = start;
    if (start) syntax_node_retain(start);
    it->dir = dir;
    it->emitted_first = false;
}

SyntaxNode *syntax_siblings_next(SyntaxSiblings *it) {
    if (!it->cur) return NULL;
    if (!it->emitted_first) {
        it->emitted_first = true;
        syntax_node_retain(it->cur);
        return it->cur;
    }
    SyntaxNode *nxt = (it->dir == SYNTAX_DIR_NEXT)
                          ? syntax_node_next_sibling(it->cur)
                          : syntax_node_prev_sibling(it->cur);
    syntax_node_release(it->cur);
    it->cur = nxt;
    if (!nxt) return NULL;
    syntax_node_retain(nxt);
    return nxt;
}

void syntax_siblings_free(SyntaxSiblings *it) {
    if (it->cur) {
        syntax_node_release(it->cur);
        it->cur = NULL;
    }
}


// =====================================================================
// SyntaxSiblingsElem — sibling iteration with node+token, starting from
// either a SyntaxNode or a SyntaxToken.
// =====================================================================

static void siblings_elem_init_common(SyntaxSiblingsElem *it, SyntaxElement start,
                                       SyntaxDirection dir) {
    it->cur = start;
    if (start.kind == SYNTAX_ELEM_NODE && start.node)        syntax_node_retain(start.node);
    else if (start.kind == SYNTAX_ELEM_TOKEN && start.token) syntax_token_retain(start.token);
    it->dir = dir;
    it->emitted_first = false;
}

void syntax_siblings_elem_init_node(SyntaxSiblingsElem *it, SyntaxNode *start, SyntaxDirection dir) {
    siblings_elem_init_common(it,
        (SyntaxElement){.kind = SYNTAX_ELEM_NODE, .node = start}, dir);
}

void syntax_siblings_elem_init_token(SyntaxSiblingsElem *it, SyntaxToken *start, SyntaxDirection dir) {
    siblings_elem_init_common(it,
        (SyntaxElement){.kind = SYNTAX_ELEM_TOKEN, .token = start}, dir);
}

static SyntaxElement elem_next_sibling_or_token(SyntaxElement e, SyntaxDirection dir) {
    if (syntax_element_is_none(e)) return SYNTAX_ELEMENT_NONE;
    if (e.kind == SYNTAX_ELEM_NODE) {
        return (dir == SYNTAX_DIR_NEXT)
                   ? syntax_node_next_sibling_or_token(e.node)
                   : syntax_node_prev_sibling_or_token(e.node);
    }
    return (dir == SYNTAX_DIR_NEXT)
               ? syntax_token_next_sibling_or_token(e.token)
               : syntax_token_prev_sibling_or_token(e.token);
}

SyntaxElement syntax_siblings_elem_next(SyntaxSiblingsElem *it) {
    if (syntax_element_is_none(it->cur)) return SYNTAX_ELEMENT_NONE;
    if (!it->emitted_first) {
        it->emitted_first = true;
        return retain_elem(it->cur);
    }
    SyntaxElement nxt = elem_next_sibling_or_token(it->cur, it->dir);
    SYN_ELEM_RELEASE(it->cur);
    it->cur = nxt;
    if (syntax_element_is_none(nxt)) return SYNTAX_ELEMENT_NONE;
    return retain_elem(it->cur);
}

void syntax_siblings_elem_free(SyntaxSiblingsElem *it) {
    SYN_ELEM_RELEASE(it->cur);
    it->cur = SYNTAX_ELEMENT_NONE;
}


// =====================================================================
// Token-rooted ancestors — reuses SyntaxAncestors over token's parent.
// =====================================================================

void syntax_token_ancestors_init(SyntaxAncestors *it, SyntaxToken *start) {
    // The token's parent is always non-NULL; ancestors yields parent,
    // grandparent, ..., root.
    SyntaxNode *parent = start ? syntax_token_parent(start) : NULL;
    // syntax_ancestors_init retains start; since we already have +1 on parent
    // from syntax_token_parent, we need to balance: retain + release would
    // be a wash, so we just hand parent to init (which retains again) and
    // release our local +1.
    syntax_ancestors_init(it, parent);
    if (parent) syntax_node_release(parent);
}
