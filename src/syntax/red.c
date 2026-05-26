#include "sll.h"
#include "syntax.h"

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// =====================================================================
// SyntaxTree + SyntaxNode + SyntaxToken (red tree) implementation.
// =====================================================================
//
// Two modes, controlled by a `mutable_` flag set at allocation:
//
//   IMMUTABLE (default): every navigation produces a fresh SyntaxNode
//   handle. Each handle holds +1 ref on its parent; cascade-free walks
//   up the chain when the last leaf-handle releases. The green tree
//   under an immutable tree is shared and refcount-managed.
//
//   MUTABLE: each parent maintains an intrusive sorted linked list
//   (SLL) of live child handles. Navigation looks up the SLL by index
//   before allocating; if a handle for (parent, index) already exists,
//   we retain and return it. This means two calls to first_child()
//   on the same mutable parent return the SAME SyntaxNode pointer.
//   That property is what allows future mutation ops (Phase 4d) to be
//   observed by every live handle to a mutated subtree.
//
// LAYOUT INVARIANT
// ================
// SyntaxNode and SyntaxToken share an IDENTICAL prefix layout up to
// and including the `sll` field. This is enforced by _Static_assert
// at the bottom of this file. The reason: the SLL container_of arith
// must work for either type. Recovery is:
//
//     SyntaxNode *outer = (SyntaxNode *)((char *)sll_ptr -
//                                          offsetof(struct SyntaxNode, sll));
//
// followed by reading `outer->is_token` to dispatch on the actual type.
// Because the prefix is byte-identical, reading common fields
// (refcount, parent, index, is_token, mutable_) is safe regardless of
// the actual underlying type — only the `green` field interpretation
// differs (GreenNode* vs GreenToken*).

struct SyntaxNode {
  uint32_t refcount;
  SyntaxNode *parent; // NULL for root; +1 ref held
  uint32_t index;     // mirrors sll.key (kept in sync)
  uint8_t is_token;   // always 0 for SyntaxNode
  uint8_t mutable_;   // 0 = immutable, 1 = mutable
  uint16_t _pad;
  GreenNode *green; // borrowed (parent chain keeps alive)
  uint32_t offset;  // immutable: precomputed abs offset
  SllNode sll;      // ring entry under parent->first
  SllNode *first;   // head of THIS node's children SLL
};

struct SyntaxToken {
  uint32_t refcount;
  SyntaxNode *parent; // tokens are never roots
  uint32_t index;
  uint8_t is_token; // always 1 for SyntaxToken
  uint8_t mutable_;
  uint16_t _pad;
  GreenToken *green;
  uint32_t offset;
  SllNode sll;
  SllNode *first; // unused for tokens (no children)
};

struct SyntaxTree {
  GreenNode *root_green;
  bool mutable_;
};

// ---- Layout invariants --------------------------------------------

_Static_assert(offsetof(struct SyntaxNode, sll) ==
                   offsetof(struct SyntaxToken, sll),
               "SLL offset must match across SyntaxNode/Token");
_Static_assert(offsetof(struct SyntaxNode, is_token) ==
                   offsetof(struct SyntaxToken, is_token),
               "is_token offset must match across SyntaxNode/Token");
_Static_assert(offsetof(struct SyntaxNode, mutable_) ==
                   offsetof(struct SyntaxToken, mutable_),
               "mutable_ offset must match across SyntaxNode/Token");
_Static_assert(offsetof(struct SyntaxNode, refcount) ==
                   offsetof(struct SyntaxToken, refcount),
               "refcount offset must match across SyntaxNode/Token");
_Static_assert(offsetof(struct SyntaxNode, parent) ==
                   offsetof(struct SyntaxToken, parent),
               "parent offset must match across SyntaxNode/Token");
_Static_assert(offsetof(struct SyntaxNode, index) ==
                   offsetof(struct SyntaxToken, index),
               "index offset must match across SyntaxNode/Token");

// Recover the outer struct from an embedded SllNode pointer. Returned
// as `SyntaxNode *` for byte addressing — caller dispatches on is_token.
#define OUTER_FROM_SLL(sllp)                                                   \
  ((SyntaxNode *)((char *)(sllp) - offsetof(struct SyntaxNode, sll)))

// Common-prefix init for both node and token allocations.
static void init_common(SyntaxNode *as_node, SyntaxNode *parent, uint32_t index,
                        uint32_t offset, uint8_t is_token, uint8_t mutable_) {
  as_node->refcount = 1;
  as_node->parent = parent;
  as_node->index = index;
  as_node->is_token = is_token;
  as_node->mutable_ = mutable_;
  as_node->_pad = 0;
  as_node->offset = offset;
  as_node->sll.prev = &as_node->sll;
  as_node->sll.next = &as_node->sll;
  as_node->sll.key = index;
  as_node->first = NULL;
}

// =====================================================================
// SyntaxTree
// =====================================================================

SyntaxTree *syntax_tree_new(GreenNode *root) {
  SyntaxTree *t = (SyntaxTree *)malloc(sizeof(SyntaxTree));
  if (!t)
    abort();
  t->root_green = root;
  t->mutable_ = false;
  return t;
}

SyntaxTree *syntax_tree_new_mut(GreenNode *root) {
  SyntaxTree *t = (SyntaxTree *)malloc(sizeof(SyntaxTree));
  if (!t)
    abort();
  t->root_green = root;
  t->mutable_ = true;
  return t;
}

void syntax_tree_free(SyntaxTree *t) {
  if (!t)
    return;
  green_node_release(t->root_green);
  free(t);
}

// Allocate a fresh root SyntaxNode wrapping the tree's root green node.
// The root retains its own green so it stays valid independently of
// the SyntaxTree lifetime.
SyntaxNode *syntax_tree_root(SyntaxTree *t) {
  SyntaxNode *n = (SyntaxNode *)malloc(sizeof(SyntaxNode));
  if (!n)
    abort();
  init_common(n, NULL, 0, 0, /*is_token=*/0, /*mutable_=*/(uint8_t)t->mutable_);
  n->green = t->root_green;
  green_node_retain(n->green);
  return n;
}

// =====================================================================
// Child allocation (SLL-aware for mutable parents)
// =====================================================================
//
// Both node_child and token_child:
//   - If parent is mutable: probe parent's SLL for an existing handle
//     at (key == index). If found, retain and return it. Otherwise
//     allocate fresh, link into SLL, retain parent.
//   - If parent is immutable: always fresh allocation + parent retain.
//
// The two helpers are duplicated rather than abstracted via callbacks
// because the only difference is the typed `green` field — and that
// difference matters at allocation time (size_of differs subtly only
// by the allocation tag; in practice the structs have identical size
// but we keep the types distinct for type-safety at the boundaries).

static SyntaxNode *node_child(SyntaxNode *parent, GreenNode *green,
                              uint32_t index, uint32_t offset) {
  if (parent && parent->mutable_) {
    SllNode probe = {.prev = NULL, .next = NULL, .key = index};
    SllAddResult r = sll_link(&parent->first, &probe);
    if (r.kind == SLL_ADD_ALREADY_IN_SLL) {
      SyntaxNode *existing = OUTER_FROM_SLL(r.curr);
      assert(existing->is_token == 0 &&
             "SLL slot occupied by token, expected node");
      assert(existing->green == green && "SLL slot's green pointer mismatch");
      existing->refcount++;
      return existing;
    }
    SyntaxNode *n = (SyntaxNode *)malloc(sizeof(SyntaxNode));
    if (!n)
      abort();
    init_common(n, parent, index, offset, /*is_token=*/0, /*mutable_=*/1);
    n->green = green;
    green_node_retain(green);
    syntax_node_retain(parent);
    sll_add(r, &n->sll);
    return n;
  }
  SyntaxNode *n = (SyntaxNode *)malloc(sizeof(SyntaxNode));
  if (!n)
    abort();
  init_common(n, parent, index, offset, /*is_token=*/0, /*mutable_=*/0);
  n->green = green;
  green_node_retain(green);
  syntax_node_retain(parent);
  return n;
}

static SyntaxToken *token_child(SyntaxNode *parent, GreenToken *green,
                                uint32_t index, uint32_t offset) {
  if (parent && parent->mutable_) {
    SllNode probe = {.prev = NULL, .next = NULL, .key = index};
    SllAddResult r = sll_link(&parent->first, &probe);
    if (r.kind == SLL_ADD_ALREADY_IN_SLL) {
      SyntaxNode *existing_n = OUTER_FROM_SLL(r.curr);
      assert(existing_n->is_token == 1 &&
             "SLL slot occupied by node, expected token");
      SyntaxToken *existing = (SyntaxToken *)existing_n;
      assert(existing->green == green && "SLL slot's green pointer mismatch");
      existing->refcount++;
      return existing;
    }
    SyntaxToken *t = (SyntaxToken *)malloc(sizeof(SyntaxToken));
    if (!t)
      abort();
    init_common((SyntaxNode *)t, parent, index, offset, /*is_token=*/1,
                /*mutable_=*/1);
    t->green = green;
    green_token_retain(green);
    syntax_node_retain(parent);
    sll_add(r, &t->sll);
    return t;
  }
  SyntaxToken *t = (SyntaxToken *)malloc(sizeof(SyntaxToken));
  if (!t)
    abort();
  init_common((SyntaxNode *)t, parent, index, offset, /*is_token=*/1,
              /*mutable_=*/0);
  t->green = green;
  green_token_retain(green);
  syntax_node_retain(parent);
  return t;
}

// =====================================================================
// Refcount + cascade-free
// =====================================================================
//
// MUTABLE-MODE INVARIANT
// ----------------------
// A mutable child holds +1 ref on its parent (just like immutable).
// The parent does NOT hold a ref on the child via the SLL — the SLL
// is a borrowed back-pointer for navigation only. Therefore, when the
// last handle to a mutable subtree releases, the cascade walks up
// naturally: child's rc→0, unlink from parent's SLL, drop child,
// dec parent's rc. By the time we free the parent, its SLL is empty
// (every child's release also unlinked it).
//
// We assert `n->first == NULL` at the point of freeing a NodeData
// to catch any bug where a child is freed but its SLL membership in
// the parent persists, or the parent free runs without all children
// having released.

void syntax_node_retain(SyntaxNode *n) {
  if (!n)
    return;
  n->refcount++;
}

void syntax_node_release(SyntaxNode *n) {
  if (!n)
    return;
  assert(n->refcount > 0 && "double-release of SyntaxNode");
  if (--n->refcount > 0)
    return;

  SyntaxNode *parent = n->parent;
  if (n->mutable_ && parent) {
    sll_unlink(&parent->first, &n->sll);
  }
  // Every red cell owns +1 on its green pointer (set up in node_child
  // for non-root, in syntax_tree_root for the root). Drop that ref now
  // so the green-tree refcount cascade can proceed.
  green_node_release(n->green);
  assert(n->first == NULL && "SyntaxNode freed with live children in SLL");
  free(n);
  if (parent)
    syntax_node_release(parent);
}

void syntax_token_retain(SyntaxToken *t) {
  if (!t)
    return;
  t->refcount++;
}

void syntax_token_release(SyntaxToken *t) {
  if (!t)
    return;
  assert(t->refcount > 0 && "double-release of SyntaxToken");
  if (--t->refcount > 0)
    return;

  SyntaxNode *parent = t->parent;
  if (t->mutable_ && parent) {
    sll_unlink(&parent->first, &t->sll);
  }
  // Every red cell owns +1 on its green pointer (set up in token_child
  // for non-root). Drop that ref now.
  green_token_release(t->green);
  assert(t->first == NULL && "SyntaxToken freed with non-NULL first slot");
  free(t);
  if (parent)
    syntax_node_release(parent);
}

// =====================================================================
// Mutable-mode getters
// =====================================================================

bool syntax_node_is_mutable(const SyntaxNode *n) { return n->mutable_ != 0; }
bool syntax_token_is_mutable(const SyntaxToken *t) { return t->mutable_ != 0; }

// =====================================================================
// Absolute offset computation
// =====================================================================
//
// For immutable nodes, `offset` is precomputed at construction. For
// mutable nodes we recompute on demand by walking up the parent chain
// summing each level's rel_offset, because the green tree underneath
// may have mutated since the handle was constructed (Phase 4d) and
// any cached offset would be stale.

static uint32_t node_abs_offset(const SyntaxNode *n) {
  if (!n->mutable_)
    return n->offset;
  uint32_t accum = 0;
  const SyntaxNode *cur = n;
  while (cur->parent) {
    GreenElement g = green_node_child(cur->parent->green, cur->index);
    accum += g.rel_offset;
    cur = cur->parent;
  }
  return accum;
}

static uint32_t token_abs_offset(const SyntaxToken *t) {
  if (!t->mutable_)
    return t->offset;
  assert(t->parent);
  GreenElement g = green_node_child(t->parent->green, t->index);
  return g.rel_offset + node_abs_offset(t->parent);
}

// =====================================================================
// Pure-read accessors
// =====================================================================

SyntaxKind syntax_node_kind(const SyntaxNode *n) {
  return green_node_kind(n->green);
}

TextRange syntax_node_text_range(const SyntaxNode *n) {
  return (TextRange){.start = node_abs_offset(n),
                     .length = green_node_text_len(n->green)};
}

const GreenNode *syntax_node_green(const SyntaxNode *n) { return n->green; }
uint32_t syntax_node_num_children(const SyntaxNode *n) {
  return green_node_num_children(n->green);
}

SyntaxKind syntax_token_kind(const SyntaxToken *t) {
  return green_token_kind(t->green);
}

TextRange syntax_token_text_range(const SyntaxToken *t) {
  return (TextRange){.start = token_abs_offset(t),
                     .length = green_token_text_len(t->green)};
}

const char *syntax_token_text(const SyntaxToken *t) {
  return green_token_text(t->green);
}
const GreenToken *syntax_token_green(const SyntaxToken *t) { return t->green; }

// =====================================================================
// Navigation
// =====================================================================

SyntaxNode *syntax_node_parent(SyntaxNode *n) {
  if (!n->parent)
    return NULL;
  syntax_node_retain(n->parent);
  return n->parent;
}

SyntaxNode *syntax_token_parent(SyntaxToken *t) {
  syntax_node_retain(t->parent);
  return t->parent;
}

SyntaxNode *syntax_node_child(SyntaxNode *n, uint32_t i) {
  if (i >= green_node_num_children(n->green))
    return NULL;
  GreenElement child = green_node_child(n->green, i);
  if (child.kind != GREEN_ELEM_NODE)
    return NULL;
  uint32_t child_offset = node_abs_offset(n) + child.rel_offset;
  return node_child(n, child.node, i, child_offset);
}

SyntaxNode *syntax_node_first_child(SyntaxNode *n) {
  uint32_t count = green_node_num_children(n->green);
  for (uint32_t i = 0; i < count; i++) {
    GreenElement child = green_node_child(n->green, i);
    if (child.kind == GREEN_ELEM_NODE) {
      uint32_t off = node_abs_offset(n) + child.rel_offset;
      return node_child(n, child.node, i, off);
    }
  }
  return NULL;
}

SyntaxNode *syntax_node_next_sibling(SyntaxNode *n) {
  if (!n->parent)
    return NULL;
  SyntaxNode *parent = n->parent;
  uint32_t parent_count = green_node_num_children(parent->green);
  for (uint32_t i = n->index + 1; i < parent_count; i++) {
    GreenElement child = green_node_child(parent->green, i);
    if (child.kind == GREEN_ELEM_NODE) {
      uint32_t off = node_abs_offset(parent) + child.rel_offset;
      return node_child(parent, child.node, i, off);
    }
  }
  return NULL;
}

SyntaxNode *syntax_node_prev_sibling(SyntaxNode *n) {
  if (!n->parent || n->index == 0)
    return NULL;
  SyntaxNode *parent = n->parent;
  for (uint32_t i = n->index; i > 0; i--) {
    uint32_t idx = i - 1;
    GreenElement g = green_node_child(parent->green, idx);
    if (g.kind == GREEN_ELEM_NODE) {
      uint32_t off = node_abs_offset(parent) + g.rel_offset;
      return node_child(parent, g.node, idx, off);
    }
  }
  return NULL;
}

// =====================================================================
// Element navigation (node-or-token)
// =====================================================================

static SyntaxElement wrap_child(SyntaxNode *parent, uint32_t i) {
  GreenElement g = green_node_child(parent->green, i);
  if (!g.node && !g.token)
    return SYNTAX_ELEMENT_NONE;
  uint32_t off = node_abs_offset(parent) + g.rel_offset;
  if (g.kind == GREEN_ELEM_NODE) {
    return (SyntaxElement){
        .kind = SYNTAX_ELEM_NODE,
        .node = node_child(parent, g.node, i, off),
    };
  }
  return (SyntaxElement){
      .kind = SYNTAX_ELEM_TOKEN,
      .token = token_child(parent, g.token, i, off),
  };
}

SyntaxElement syntax_node_child_or_token(SyntaxNode *n, uint32_t i) {
  if (i >= green_node_num_children(n->green))
    return SYNTAX_ELEMENT_NONE;
  return wrap_child(n, i);
}

SyntaxElement syntax_node_first_child_or_token(SyntaxNode *n) {
  if (green_node_num_children(n->green) == 0)
    return SYNTAX_ELEMENT_NONE;
  return wrap_child(n, 0);
}

SyntaxElement syntax_node_last_child_or_token(SyntaxNode *n) {
  uint32_t count = green_node_num_children(n->green);
  if (count == 0)
    return SYNTAX_ELEMENT_NONE;
  return wrap_child(n, count - 1);
}

SyntaxElement syntax_node_next_sibling_or_token(SyntaxNode *n) {
  if (!n->parent)
    return SYNTAX_ELEMENT_NONE;
  if (n->index + 1 >= green_node_num_children(n->parent->green))
    return SYNTAX_ELEMENT_NONE;
  return wrap_child(n->parent, n->index + 1);
}

SyntaxElement syntax_node_prev_sibling_or_token(SyntaxNode *n) {
  if (!n->parent || n->index == 0)
    return SYNTAX_ELEMENT_NONE;
  return wrap_child(n->parent, n->index - 1);
}

SyntaxElement syntax_token_next_sibling_or_token(SyntaxToken *t) {
  if (t->index + 1 >= green_node_num_children(t->parent->green))
    return SYNTAX_ELEMENT_NONE;
  return wrap_child(t->parent, t->index + 1);
}

SyntaxElement syntax_token_prev_sibling_or_token(SyntaxToken *t) {
  if (t->index == 0)
    return SYNTAX_ELEMENT_NONE;
  return wrap_child(t->parent, t->index - 1);
}

// =====================================================================
// Cross-parent token navigation (Phase 4f item 8)
// =====================================================================

// Internal: given an element, return its leftmost (or rightmost) leaf
// token. CONSUMES `e` (releases the element's handle); returns a new
// owned SyntaxToken or NULL if the element's subtree has no tokens.
static SyntaxToken *element_leftmost_token(SyntaxElement e) {
  if (e.kind == SYNTAX_ELEM_TOKEN)
    return e.token; // already a token
  SyntaxToken *t = syntax_node_first_token(e.node);
  syntax_node_release(e.node);
  return t;
}

static SyntaxToken *element_rightmost_token(SyntaxElement e) {
  if (e.kind == SYNTAX_ELEM_TOKEN)
    return e.token;
  SyntaxToken *t = syntax_node_last_token(e.node);
  syntax_node_release(e.node);
  return t;
}

SyntaxToken *syntax_token_next_token(SyntaxToken *t) {
  // First: try the immediate next sibling-or-token.
  SyntaxElement sib = syntax_token_next_sibling_or_token(t);
  if (!syntax_element_is_none(sib)) {
    return element_leftmost_token(sib);
  }
  // Walk up ancestors until one has a next sibling-or-token; descend
  // into that sibling's leftmost leaf token.
  SyntaxNode *cur = t->parent;
  syntax_node_retain(cur);
  while (cur) {
    SyntaxElement sib2 = syntax_node_next_sibling_or_token(cur);
    if (!syntax_element_is_none(sib2)) {
      syntax_node_release(cur);
      return element_leftmost_token(sib2);
    }
    SyntaxNode *parent = syntax_node_parent(cur); // +1
    syntax_node_release(cur);
    cur = parent;
  }
  return NULL;
}

SyntaxToken *syntax_token_prev_token(SyntaxToken *t) {
  SyntaxElement sib = syntax_token_prev_sibling_or_token(t);
  if (!syntax_element_is_none(sib)) {
    return element_rightmost_token(sib);
  }
  SyntaxNode *cur = t->parent;
  syntax_node_retain(cur);
  while (cur) {
    SyntaxElement sib2 = syntax_node_prev_sibling_or_token(cur);
    if (!syntax_element_is_none(sib2)) {
      syntax_node_release(cur);
      return element_rightmost_token(sib2);
    }
    SyntaxNode *parent = syntax_node_parent(cur);
    syntax_node_release(cur);
    cur = parent;
  }
  return NULL;
}

// =====================================================================
// Internal helpers used by ptr.c
// =====================================================================

TextRange syntax_node_child_range(const SyntaxNode *n, uint32_t i) {
  if (i >= green_node_num_children(n->green))
    return TEXT_RANGE_NONE;
  GreenElement child = green_node_child(n->green, i);
  uint32_t start = node_abs_offset(n) + child.rel_offset;
  uint32_t len = (child.kind == GREEN_ELEM_NODE)
                     ? green_node_text_len(child.node)
                     : green_token_text_len(child.token);
  return (TextRange){.start = start, .length = len};
}

GreenElement syntax_node_child_green(const SyntaxNode *n, uint32_t i) {
  return green_node_child(n->green, i);
}

// =====================================================================
// Leaf navigation: first_token / last_token of a subtree
// =====================================================================

SyntaxToken *syntax_node_first_token(SyntaxNode *n) {
  SyntaxNode *current = n;
  syntax_node_retain(current);

  for (;;) {
    uint32_t count = green_node_num_children(current->green);
    if (count == 0) {
      syntax_node_release(current);
      return NULL;
    }
    uint32_t cur_off = node_abs_offset(current);
    for (uint32_t i = 0; i < count; i++) {
      GreenElement g = green_node_child(current->green, i);
      uint32_t len = (g.kind == GREEN_ELEM_NODE)
                         ? green_node_text_len(g.node)
                         : green_token_text_len(g.token);
      if (g.kind == GREEN_ELEM_TOKEN) {
        uint32_t off = cur_off + g.rel_offset;
        SyntaxToken *tok = token_child(current, g.token, i, off);
        syntax_node_release(current);
        return tok;
      }
      if (len > 0) {
        uint32_t off = cur_off + g.rel_offset;
        SyntaxNode *child = node_child(current, g.node, i, off);
        syntax_node_release(current);
        current = child;
        goto next_level;
      }
    }
    syntax_node_release(current);
    return NULL;
  next_level:;
  }
}

SyntaxToken *syntax_node_last_token(SyntaxNode *n) {
  SyntaxNode *current = n;
  syntax_node_retain(current);

  for (;;) {
    uint32_t count = green_node_num_children(current->green);
    if (count == 0) {
      syntax_node_release(current);
      return NULL;
    }
    uint32_t cur_off = node_abs_offset(current);
    for (uint32_t i = count; i > 0; i--) {
      uint32_t idx = i - 1;
      GreenElement g = green_node_child(current->green, idx);
      uint32_t len = (g.kind == GREEN_ELEM_NODE)
                         ? green_node_text_len(g.node)
                         : green_token_text_len(g.token);
      if (g.kind == GREEN_ELEM_TOKEN) {
        uint32_t off = cur_off + g.rel_offset;
        SyntaxToken *tok = token_child(current, g.token, idx, off);
        syntax_node_release(current);
        return tok;
      }
      if (len > 0) {
        uint32_t off = cur_off + g.rel_offset;
        SyntaxNode *child = node_child(current, g.node, idx, off);
        syntax_node_release(current);
        current = child;
        goto next_level;
      }
    }
    syntax_node_release(current);
    return NULL;
  next_level:;
  }
}

// =====================================================================
// Token-precise positioning (TokenAtOffset)
// =====================================================================
//
// Inclusive-on-both-ends matching: at each level we collect children
// whose range [start, end] contains the offset. The result is:
//   0 matches → NONE
//   1 match   → recurse into it; if it's a token, return SINGLE
//   2 matches → boundary; descend into each to find their respective
//               single token, return BETWEEN(left, right)
//
// This algorithm is a direct port of rowan's `token_at_offset`.

static TokenAtOffset token_at_offset_recurse(SyntaxNode *node, uint32_t offset);

// Recurse into a SyntaxElement (token or node). CONSUMES the element's
// embedded handle. Returns a fresh TokenAtOffset whose token handles
// are RETURNS_OWNED.
static TokenAtOffset token_at_offset_recurse_elem(SyntaxElement e,
                                                  uint32_t offset) {
  if (e.kind == SYNTAX_ELEM_TOKEN) {
    return (TokenAtOffset){.kind = TOKEN_AT_OFFSET_SINGLE, .single = e.token};
  }
  TokenAtOffset r = token_at_offset_recurse(e.node, offset);
  syntax_node_release(e.node);
  return r;
}

// Recurse over a SyntaxNode. BORROWS `node` (caller retains ownership).
static TokenAtOffset token_at_offset_recurse(SyntaxNode *node,
                                             uint32_t offset) {
  if (green_node_text_len(node->green) == 0)
    return (TokenAtOffset){.kind = TOKEN_AT_OFFSET_NONE};

  uint32_t count = green_node_num_children(node->green);
  SyntaxElement left_e = SYNTAX_ELEMENT_NONE, right_e = SYNTAX_ELEMENT_NONE;

  for (uint32_t i = 0; i < count; i++) {
    TextRange cr = syntax_node_child_range(node, i);
    if (cr.length == 0)
      continue;
    if (cr.start <= offset && offset <= text_range_end(cr)) {
      if (syntax_element_is_none(left_e)) {
        left_e = syntax_node_child_or_token(node, i);
      } else {
        right_e = syntax_node_child_or_token(node, i);
        break; // at most two matches in a sorted-children layout
      }
    }
  }

  if (syntax_element_is_none(left_e)) {
    return (TokenAtOffset){.kind = TOKEN_AT_OFFSET_NONE};
  }
  if (syntax_element_is_none(right_e)) {
    return token_at_offset_recurse_elem(left_e, offset); // consumes left_e
  }

  // Boundary case.
  TokenAtOffset left_r = token_at_offset_recurse_elem(left_e, offset);
  TokenAtOffset right_r = token_at_offset_recurse_elem(right_e, offset);

  if (left_r.kind == TOKEN_AT_OFFSET_SINGLE &&
      right_r.kind == TOKEN_AT_OFFSET_SINGLE) {
    return (TokenAtOffset){
        .kind = TOKEN_AT_OFFSET_BETWEEN,
        .left = left_r.single,
        .right = right_r.single,
    };
  }
  // Defensive cleanup if one branch returned NONE unexpectedly.
  if (left_r.kind == TOKEN_AT_OFFSET_SINGLE)
    syntax_token_release(left_r.single);
  if (right_r.kind == TOKEN_AT_OFFSET_SINGLE)
    syntax_token_release(right_r.single);
  return (TokenAtOffset){.kind = TOKEN_AT_OFFSET_NONE};
}

TokenAtOffset syntax_token_at_offset(SyntaxNode *root, uint32_t offset) {
  if (!root)
    return (TokenAtOffset){.kind = TOKEN_AT_OFFSET_NONE};
  TextRange r = syntax_node_text_range(root);
  if (offset < r.start || offset > text_range_end(r))
    return (TokenAtOffset){.kind = TOKEN_AT_OFFSET_NONE};
  return token_at_offset_recurse(root, offset);
}

// =====================================================================
// clone_for_update
// =====================================================================
//
// Recursively walks the parent chain. The top ancestor becomes a fresh
// mutable root (no parent, retains its own green). Each descendant
// is allocated as a mutable child SLL-linked under its parent via
// node_child. The result mirrors `node`'s logical position but lives
// in a new mutable tree; the original `node` and its tree are
// untouched.

SyntaxNode *syntax_node_clone_for_update(SyntaxNode *node) {
  if (!node)
    return NULL;
  assert(!node->mutable_ && "clone_for_update: input must be immutable");
  if (!node->parent) {
    // Fresh mutable root.
    SyntaxNode *root = (SyntaxNode *)malloc(sizeof(SyntaxNode));
    if (!root)
      abort();
    init_common(root, NULL, 0, 0, /*is_token=*/0, /*mutable_=*/1);
    root->green = node->green;
    green_node_retain(root->green);
    return root;
  }
  SyntaxNode *cloned_parent = syntax_node_clone_for_update(node->parent);
  // node_child SLL-links + bumps parent rc (cloned_parent rc becomes 2).
  // Then we release our own ref → cloned_parent rc back to 1, held by clone.
  SyntaxNode *clone =
      node_child(cloned_parent, node->green, node->index, node->offset);
  syntax_node_release(cloned_parent);
  return clone;
}

SyntaxNode *syntax_node_clone_subtree(const SyntaxNode *node) {
  if (!node)
    return NULL;
  SyntaxNode *root = (SyntaxNode *)malloc(sizeof(SyntaxNode));
  if (!root)
    abort();
  init_common(root, NULL, 0, 0, /*is_token=*/0, /*mutable_=*/0);
  root->green = node->green;
  green_node_retain(root->green);
  return root;
}

// =====================================================================
// Kind-filtered matchers (Phase 4f items 2)
// =====================================================================

SyntaxNode *syntax_node_first_child_by_kind(SyntaxNode *n, SyntaxKind k) {
  uint32_t count = green_node_num_children(n->green);
  for (uint32_t i = 0; i < count; i++) {
    GreenElement g = green_node_child(n->green, i);
    if (g.kind == GREEN_ELEM_NODE && green_node_kind(g.node) == k) {
      uint32_t off = node_abs_offset(n) + g.rel_offset;
      return node_child(n, g.node, i, off);
    }
  }
  return NULL;
}

SyntaxElement syntax_node_first_child_or_token_by_kind(SyntaxNode *n,
                                                       SyntaxKind k) {
  uint32_t count = green_node_num_children(n->green);
  for (uint32_t i = 0; i < count; i++) {
    GreenElement g = green_node_child(n->green, i);
    SyntaxKind gk = (g.kind == GREEN_ELEM_NODE) ? green_node_kind(g.node)
                                                : green_token_kind(g.token);
    if (gk == k)
      return wrap_child(n, i);
  }
  return SYNTAX_ELEMENT_NONE;
}

SyntaxNode *syntax_node_next_sibling_by_kind(SyntaxNode *n, SyntaxKind k) {
  if (!n->parent)
    return NULL;
  SyntaxNode *parent = n->parent;
  uint32_t count = green_node_num_children(parent->green);
  for (uint32_t i = n->index + 1; i < count; i++) {
    GreenElement g = green_node_child(parent->green, i);
    if (g.kind == GREEN_ELEM_NODE && green_node_kind(g.node) == k) {
      uint32_t off = node_abs_offset(parent) + g.rel_offset;
      return node_child(parent, g.node, i, off);
    }
  }
  return NULL;
}

SyntaxElement syntax_node_next_sibling_or_token_by_kind(SyntaxNode *n,
                                                        SyntaxKind k) {
  if (!n->parent)
    return SYNTAX_ELEMENT_NONE;
  SyntaxNode *parent = n->parent;
  uint32_t count = green_node_num_children(parent->green);
  for (uint32_t i = n->index + 1; i < count; i++) {
    GreenElement g = green_node_child(parent->green, i);
    SyntaxKind gk = (g.kind == GREEN_ELEM_NODE) ? green_node_kind(g.node)
                                                : green_token_kind(g.token);
    if (gk == k)
      return wrap_child(parent, i);
  }
  return SYNTAX_ELEMENT_NONE;
}

SyntaxElement syntax_token_next_sibling_or_token_by_kind(SyntaxToken *t,
                                                         SyntaxKind k) {
  SyntaxNode *parent = t->parent;
  uint32_t count = green_node_num_children(parent->green);
  for (uint32_t i = t->index + 1; i < count; i++) {
    GreenElement g = green_node_child(parent->green, i);
    SyntaxKind gk = (g.kind == GREEN_ELEM_NODE) ? green_node_kind(g.node)
                                                : green_token_kind(g.token);
    if (gk == k)
      return wrap_child(parent, i);
  }
  return SYNTAX_ELEMENT_NONE;
}

// =====================================================================
// Range-based positioning (Phase 4f items 4, 5)
// =====================================================================

SyntaxElement syntax_node_child_or_token_at_range(SyntaxNode *n,
                                                  TextRange range) {
  uint32_t count = green_node_num_children(n->green);
  for (uint32_t i = 0; i < count; i++) {
    TextRange cr = syntax_node_child_range(n, i);
    if (cr.length == 0)
      continue;
    if (text_range_contains(cr, range)) {
      return wrap_child(n, i);
    }
  }
  return SYNTAX_ELEMENT_NONE;
}

SyntaxElement syntax_node_covering_element(SyntaxNode *n, TextRange range) {
  TextRange node_range = syntax_node_text_range(n);
  if (!text_range_contains(node_range, range)) {
    return SYNTAX_ELEMENT_NONE;
  }
  // Start with self as the candidate; hold +1 on it.
  syntax_node_retain(n);
  SyntaxElement cur = {.kind = SYNTAX_ELEM_NODE, .node = n};

  for (;;) {
    if (cur.kind == SYNTAX_ELEM_TOKEN)
      return cur;
    SyntaxElement child = syntax_node_child_or_token_at_range(cur.node, range);
    if (syntax_element_is_none(child))
      return cur; // current is smallest
    // Descend.
    syntax_node_release(cur.node);
    cur = child;
  }
}

// =====================================================================
// Mutation primitives (Phase 4d)
// =====================================================================
//
// All operations require mutable mode. The core loop is `respine`,
// which walks up from a mutated node replacing each ancestor's green
// pointer. detach/attach/splice/replace_with are user-facing wrappers
// that compose green-side helpers + respine + SLL maintenance.

// Adjust SLL keys + sync each outer struct's `index` field. The two
// are stored in two places (sll.key for ring sorting; index for green
// tree indexing) and must agree at all times for navigation to work.
static void sll_adjust_and_sync(SllNode *elem, uint32_t from, int32_t delta) {
  sll_adjust(elem, from, delta);
  SllNode *curr = elem;
  do {
    SyntaxNode *outer = OUTER_FROM_SLL(curr);
    outer->index = curr->key;
    curr = curr->next;
  } while (curr != elem);
}

// Internal: walks up `node`, swapping each ancestor's green pointer
// for one that reflects `new_green` at the appropriate child slot.
// `new_green` arrives RETURNS_OWNED (consumed). Refcounts:
//   - each Cell's green pointer represents +1 on its pointee.
//   - swap moves the +1 invariant from new_green (caller's local) into
//     node.green Cell; the old green becomes "in our hands" with +1.
//   - we release that +1 each iteration after building the next level.
static void respine(SyntaxNode *node, GreenNode *new_green) {
  SyntaxNode *cur = node;
  for (;;) {
    GreenNode *old_green = cur->green;
    cur->green = new_green;
    // cur->green now holds new_green's +1 (transferred from caller).
    // old_green's +1 is now in our hands (must be released here).

    if (!cur->parent) {
      green_node_release(old_green);
      return;
    }
    // Build parent's new green: replace child[cur->index] with new_green.
    GreenElement my_elem = {
        .kind = GREEN_ELEM_NODE,
        .node = new_green,
        .rel_offset = 0, // recomputed by green_node_alloc
    };
    GreenNode *new_parent_green =
        green_node_replace_child(cur->parent->green, cur->index, my_elem);
    // new_parent_green has +1 (ours). It retained new_green internally.
    green_node_release(old_green);
    new_green = new_parent_green;
    cur = cur->parent;
  }
}

// Helper: extract the NodeData header from a SyntaxElement.
static SyntaxNode *elem_data(SyntaxElement e) {
  if (e.kind == SYNTAX_ELEM_NODE)
    return e.node;
  return (SyntaxNode *)e.token;
}

// Detach a SyntaxNode/Token from its parent. Shared body for both
// types; the only difference is the green retain (node vs token).
static void detach_common(SyntaxNode *node) {
  assert(node->mutable_ && "detach: node must be mutable");
  assert(node->refcount > 0);

  SyntaxNode *parent = node->parent;
  if (!parent)
    return; // already detached

  // No explicit green retain needed here: the red wrapper already owns
  // +1 on its green (via node_child / token_child). Even after respine
  // drops the OLD parent green's hold on us, our red wrapper's +1 keeps
  // the green alive until the wrapper itself is released.

  uint32_t my_index = node->index;

  // Shift later siblings' indices down by 1 (in SLL keys AND in their
  // outer struct's `index` field).
  sll_adjust_and_sync(&node->sll, my_index + 1, -1);

  // Unlink ourselves from parent's SLL.
  sll_unlink(&parent->first, &node->sll);

  // Build new parent green = parent's green minus our slot.
  GreenNode *new_parent_green =
      green_node_remove_child(parent->green, my_index);

  // Respine: replace parent.green and propagate up.
  respine(parent, new_parent_green);

  // Drop parent ref.
  node->parent = NULL;
  syntax_node_release(parent);
}

void syntax_node_detach(SyntaxNode *node) { detach_common(node); }
void syntax_token_detach(SyntaxToken *tok) { detach_common((SyntaxNode *)tok); }

// Attach `child` (a detached mutable element) to `parent` at `index`.
// On entry `child->parent == NULL`. Net result: parent's green grows,
// later siblings shift up, child becomes a tracked SLL member.
static void attach_child(SyntaxNode *parent, uint32_t index,
                         SyntaxElement child) {
  assert(parent->mutable_ && "attach: parent must be mutable");
  SyntaxNode *child_data = elem_data(child);
  assert(child_data->mutable_ && "attach: child must be mutable");
  assert(child_data->parent == NULL && "attach: child must be detached");
  assert(index <= green_node_num_children(parent->green) &&
         "attach: index out of range");

  // Set child's parent/index BEFORE SLL operations so adjust works.
  child_data->parent = parent;
  child_data->index = index;
  child_data->sll.key = index;
  syntax_node_retain(parent);

  // Shift existing siblings with key >= index up by 1.
  if (parent->first) {
    sll_adjust_and_sync(parent->first, index, +1);
  }

  // Link into parent's SLL.
  SllAddResult r = sll_link(&parent->first, &child_data->sll);
  assert(r.kind != SLL_ADD_ALREADY_IN_SLL && "attach: index collision");
  sll_add(r, &child_data->sll);

  // Build new parent green with our child inserted.
  GreenElement gelem;
  if (child.kind == SYNTAX_ELEM_NODE) {
    gelem = (GreenElement){
        .kind = GREEN_ELEM_NODE, .node = child_data->green, .rel_offset = 0};
  } else {
    gelem = (GreenElement){.kind = GREEN_ELEM_TOKEN,
                           .token = ((SyntaxToken *)child_data)->green,
                           .rel_offset = 0};
  }
  GreenNode *new_parent_green =
      green_node_insert_child(parent->green, index, gelem);
  // new_parent_green retained child's green (+1) for its own slot.
  // The child red wrapper's +1 on its green stays as-is — it's owned
  // by the wrapper and dropped only when the wrapper is released
  // (Phase 1.5 discipline: every red cell owns +1 on its green).

  respine(parent, new_parent_green);
}

void syntax_node_splice_children(SyntaxNode *parent, uint32_t from, uint32_t to,
                                 const SyntaxElement *replace, uint32_t n) {
  assert(parent->mutable_ && "splice: parent must be mutable");
  assert(from <= to);
  assert(to <= green_node_num_children(parent->green));

  uint32_t count = to - from;

  // Snapshot the to-be-detached handles. Each detach shifts indices,
  // so we must materialize all handles before detaching anyone.
  SyntaxElement *snapshot = NULL;
  if (count > 0) {
    snapshot = (SyntaxElement *)malloc((size_t)count * sizeof(SyntaxElement));
    if (!snapshot)
      abort();
    for (uint32_t i = 0; i < count; i++) {
      snapshot[i] = syntax_node_child_or_token(parent, from + i);
    }
    // Detach each in REVERSE order so the indices we captured stay
    // valid (detaching the last sibling first doesn't shift earlier
    // siblings).
    for (uint32_t i = count; i > 0; i--) {
      if (snapshot[i - 1].kind == SYNTAX_ELEM_NODE)
        syntax_node_detach(snapshot[i - 1].node);
      else
        syntax_token_detach(snapshot[i - 1].token);
    }
    // Release the detached handles — caller's responsibility to keep
    // any that they want; the splice API treats them as discarded.
    for (uint32_t i = 0; i < count; i++) {
      SYN_ELEM_RELEASE(snapshot[i]);
    }
    free(snapshot);
  }

  // Now attach the new children at indices [from, from+n).
  for (uint32_t i = 0; i < n; i++) {
    attach_child(parent, from + i, replace[i]);
  }
}

GreenNode *syntax_node_replace_with(SyntaxNode *node, GreenNode *replacement) {
  assert(green_node_kind(replacement) == syntax_node_kind(node) &&
         "replace_with: kind mismatch");

  if (node->mutable_) {
    // Respine in place; return the new root green.
    green_node_retain(replacement); // respine consumes its arg's +1
    respine(node, replacement);
    SyntaxNode *root = node;
    while (root->parent)
      root = root->parent;
    green_node_retain(root->green);
    return root->green;
  }

  // Immutable path: bubble up building new green nodes.
  if (!node->parent) {
    green_node_retain(replacement);
    return replacement;
  }
  GreenNode *current = replacement;
  green_node_retain(current);
  const SyntaxNode *cur = node;
  while (cur->parent) {
    GreenElement elem = {
        .kind = GREEN_ELEM_NODE, .node = current, .rel_offset = 0};
    GreenNode *up =
        green_node_replace_child(cur->parent->green, cur->index, elem);
    green_node_release(current); // up retained it
    current = up;
    cur = cur->parent;
  }
  return current;
}

GreenNode *syntax_token_replace_with(SyntaxToken *tok,
                                     GreenToken *replacement) {
  assert(green_token_kind(replacement) == syntax_token_kind(tok) &&
         "replace_with: kind mismatch");
  assert(tok->parent != NULL && "token has no parent");

  // Build new parent green with our slot replaced by the new token.
  GreenElement elem = {
      .kind = GREEN_ELEM_TOKEN, .token = replacement, .rel_offset = 0};
  GreenNode *new_parent_green =
      green_node_replace_child(tok->parent->green, tok->index, elem);

  if (tok->mutable_) {
    respine(tok->parent, new_parent_green);
    SyntaxNode *root = tok->parent;
    while (root->parent)
      root = root->parent;
    green_node_retain(root->green);
    return root->green;
  }

  // Immutable: bubble up.
  GreenNode *current = new_parent_green;
  const SyntaxNode *cur = tok->parent;
  while (cur->parent) {
    GreenElement e = {
        .kind = GREEN_ELEM_NODE, .node = current, .rel_offset = 0};
    GreenNode *up = green_node_replace_child(cur->parent->green, cur->index, e);
    green_node_release(current);
    current = up;
    cur = cur->parent;
  }
  return current;
}
