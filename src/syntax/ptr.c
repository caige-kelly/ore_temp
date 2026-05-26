#include "syntax.h"

#include <assert.h>
#include <stdlib.h>

// =====================================================================
// SyntaxNodePtr — stable identity across reparses.
// =====================================================================
//
// A SyntaxNodePtr is just (kind, range). To resolve in a fresh tree,
// walk from the root and descend through children whose range
// contains the pointer's range, binary-searching at each level.
// Complexity: O(depth * log(width)).
//
// Children in a green node are sorted by start offset (rel_offset is
// monotonically non-decreasing in the children array), so binary
// search works directly. Returns the SyntaxNode whose (kind, range)
// matches exactly, or NULL if no match exists.

// Internal helpers from red.c.
TextRange syntax_node_child_range(const SyntaxNode *n, uint32_t i);
GreenElement syntax_node_child_green(const SyntaxNode *n, uint32_t i);

SyntaxNodePtr syntax_node_ptr_new(const SyntaxNode *node) {
  return (SyntaxNodePtr){
      .kind = syntax_node_kind(node),
      .range = syntax_node_text_range(node),
  };
}

// Find the index of the child whose absolute range contains `target.start`,
// using binary search on the children's start offsets. Returns UINT32_MAX
// if no such child exists.
static uint32_t find_child_containing(const SyntaxNode *parent,
                                      TextRange target) {
  uint32_t count = syntax_node_num_children(parent);
  if (count == 0)
    return UINT32_MAX;

  uint32_t lo = 0, hi = count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    TextRange child = syntax_node_child_range(parent, mid);
    uint32_t child_end = child.start + child.length;
    if (target.start >= child_end) {
      lo = mid + 1;
    } else if (target.start < child.start) {
      hi = mid;
    } else {
      // target.start is within [child.start, child_end).
      // Verify the full target range fits inside this child.
      if (text_range_contains(child, target))
        return mid;
      // The target straddles a child boundary — can't resolve.
      return UINT32_MAX;
    }
  }
  return UINT32_MAX;
}

SyntaxNode *syntax_node_ptr_resolve(SyntaxNodePtr ptr, SyntaxNode *root) {
  if (!root)
    return NULL;

  // The root itself might be the match (e.g., a ptr to the file's
  // root decl walks zero levels).
  if (syntax_node_kind(root) == ptr.kind &&
      text_range_eq(syntax_node_text_range(root), ptr.range)) {
    syntax_node_retain(root);
    return root;
  }

  // Walk down. `current` holds a +1 reference; we release it as we
  // descend.
  SyntaxNode *current = root;
  syntax_node_retain(current);

  for (;;) {
    TextRange current_range = syntax_node_text_range(current);
    // If we've reached the target byte range but our current
    // (kind, range) doesn't match, no further descent can help —
    // a deeper node would have a STRICTLY SMALLER range than the
    // target. Fail.
    if (text_range_eq(current_range, ptr.range)) {
      // Kind mismatch (we'd have returned above on a match).
      syntax_node_release(current);
      return NULL;
    }

    uint32_t idx = find_child_containing(current, ptr.range);
    if (idx == UINT32_MAX) {
      syntax_node_release(current);
      return NULL;
    }

    // Peek the child element BEFORE allocating — if it's a token
    // (which can never have a matching SyntaxNodePtr — those are
    // anchored on nodes only), fail.
    GreenElement child = syntax_node_child_green(current, idx);
    if (child.kind != GREEN_ELEM_NODE) {
      syntax_node_release(current);
      return NULL;
    }

    SyntaxNode *next = syntax_node_child(current, idx);
    assert(next != NULL &&
           "child returned NULL despite green peek showing a node");

    // Check exact match on the new node.
    if (syntax_node_kind(next) == ptr.kind &&
        text_range_eq(syntax_node_text_range(next), ptr.range)) {
      syntax_node_release(current);
      return next; // caller owns
    }

    // Descend further.
    syntax_node_release(current);
    current = next;
  }
}
