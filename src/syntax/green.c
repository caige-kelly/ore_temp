#include "syntax.h"
#include "syntax_kind.h" // ore_kind_is_trivia + OreSyntaxKind

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "../support/data_structure/fxhash.h"

// GreenNode + GreenToken implementation.
//
// Both types are flexible-array-member structs allocated with one
// malloc each, refcounted, immutable post-construction. Release
// drops one ref; when refcount hits 0, cascade-frees children (for
// nodes) and the allocation itself.
//
// AoS by design (per the architecture doc): every navigation touches
// (kind, width, children) together, and the hash-cons interner needs
// the node to be a self-contained hashable unit.

struct GreenNode {
  SyntaxKind kind;
  uint32_t text_len;
  uint32_t refcount;
  uint32_t num_children;
  uint64_t content_hash; // cached on construction; used by the cache
  // Flexible array of GreenElement.
  GreenElement children[];
};

struct GreenToken {
  SyntaxKind kind;
  uint32_t refcount;
  uint32_t text_len;
  uint64_t content_hash; // cached on construction
  char text[];           // NUL-terminated (text_len does not include NUL)
};

// ---- Hash helpers (internal — used by the cache) -------------------

// Hash key for a token: (kind, text bytes). Identical kind+text → same hash.
uint64_t green_token_compute_hash(SyntaxKind kind, const char *text,
                                  uint32_t text_len) {
  FxHasher h = fxhash_init();
  fxhash_u32(&h, kind);
  fxhash_bytes(&h, text, text_len);
  return fxhash_finish(&h);
}

// Hash key for a node: (kind, children-by-pointer). Children are
// already interned, so pointer-identity equality suffices for the
// outer node — we don't need to recurse.
uint64_t green_node_compute_hash(SyntaxKind kind, const GreenElement *children,
                                 uint32_t num_children) {
  FxHasher h = fxhash_init();
  fxhash_u32(&h, kind);
  fxhash_u32(&h, num_children);
  for (uint32_t i = 0; i < num_children; i++) {
    const GreenElement *c = &children[i];
    fxhash_u32(&h, c->kind);
    if (c->kind == GREEN_ELEM_NODE)
      fxhash_ptr(&h, c->node);
    else
      fxhash_ptr(&h, c->token);
  }
  return fxhash_finish(&h);
}

// Structural (trivia-EXCLUDING) hash. content_hash / compute_hash above
// fold ALL children by pointer and are therefore trivia-SENSITIVE (a
// whitespace/comment edit changes a child trivia token → different hash).
// This recurses the subtree folding node kinds + NON-trivia token
// identities and SKIPPING trivia tokens, so it is the right per-decl
// content fingerprint for incremental cutoff: trivia edits don't change
// it (contract C3), but renames do (a hash-consed token's pointer encodes
// kind+text, C4). Position-independent (green nodes/tokens carry no
// absolute offsets). Recursion depth is bounded by what the parser
// produced — no deeper than the parser's own recursive descent survived.
static void green_structural_hash_rec(FxHasher *h, const GreenNode *n) {
  fxhash_u32(h, n->kind);
  for (uint32_t i = 0; i < n->num_children; i++) {
    const GreenElement *c = &n->children[i];
    if (c->kind == GREEN_ELEM_NODE) {
      green_structural_hash_rec(h, c->node);
    } else if (!ore_kind_is_trivia((OreSyntaxKind)green_token_kind(c->token))) {
      fxhash_ptr(h, c->token); // hash-cons: pointer == (kind, text) identity
    }
  }
}

uint64_t green_structural_hash(const GreenNode *n) {
  FxHasher h = fxhash_init();
  green_structural_hash_rec(&h, n);
  return fxhash_finish(&h);
}

// Equality check for cache lookup: same kind + same children (by pointer).
bool green_node_equals(const GreenNode *n, SyntaxKind kind,
                       const GreenElement *children, uint32_t num_children) {
  if (n->kind != kind || n->num_children != num_children)
    return false;
  for (uint32_t i = 0; i < num_children; i++) {
    const GreenElement *a = &n->children[i];
    const GreenElement *b = &children[i];
    if (a->kind != b->kind)
      return false;
    if (a->kind == GREEN_ELEM_NODE) {
      if (a->node != b->node)
        return false;
    } else {
      if (a->token != b->token)
        return false;
    }
  }
  return true;
}

bool green_token_equals(const GreenToken *t, SyntaxKind kind, const char *text,
                        uint32_t text_len) {
  return t->kind == kind && t->text_len == text_len &&
         memcmp(t->text, text, text_len) == 0;
}

// ---- Construction (internal — called by the cache) -----------------

// Allocate a fresh GreenNode. Caller is responsible for setting child
// rel_offsets correctly. Each child reference is retained by this node.
GreenNode *green_node_alloc(SyntaxKind kind, const GreenElement *children,
                            uint32_t num_children) {
  size_t bytes =
      sizeof(GreenNode) + (size_t)num_children * sizeof(GreenElement);
  GreenNode *n = (GreenNode *)malloc(bytes);
  if (!n)
    abort();
  n->kind = kind;
  n->refcount = 1;
  n->num_children = num_children;

  uint32_t text_len = 0;
  for (uint32_t i = 0; i < num_children; i++) {
    n->children[i] = children[i];
    n->children[i].rel_offset = text_len;
    uint32_t child_len;
    if (children[i].kind == GREEN_ELEM_NODE) {
      child_len = green_node_text_len(children[i].node);
      green_node_retain(children[i].node);
    } else {
      child_len = green_token_text_len(children[i].token);
      green_token_retain(children[i].token);
    }
    text_len += child_len;
  }
  n->text_len = text_len;
  n->content_hash = green_node_compute_hash(kind, n->children, num_children);
  return n;
}

GreenToken *green_token_alloc(SyntaxKind kind, const char *text,
                              uint32_t text_len) {
  size_t bytes = sizeof(GreenToken) + (size_t)text_len + 1;
  GreenToken *t = (GreenToken *)malloc(bytes);
  if (!t)
    abort();
  t->kind = kind;
  t->refcount = 1;
  t->text_len = text_len;
  memcpy(t->text, text, text_len);
  t->text[text_len] = '\0';
  t->content_hash = green_token_compute_hash(kind, text, text_len);
  return t;
}

// ---- Public API ----------------------------------------------------

void green_node_retain(GreenNode *n) {
  if (!n)
    return;
  n->refcount++;
}

void green_token_retain(GreenToken *t) {
  if (!t)
    return;
  t->refcount++;
}

void green_node_release(GreenNode *n) {
  if (!n)
    return;
  assert(n->refcount > 0 && "double-release of GreenNode");
  if (--n->refcount > 0)
    return;
  // Cascade: release each child's reference, then free this node.
  for (uint32_t i = 0; i < n->num_children; i++) {
    if (n->children[i].kind == GREEN_ELEM_NODE)
      green_node_release(n->children[i].node);
    else
      green_token_release(n->children[i].token);
  }
  free(n);
}

void green_token_release(GreenToken *t) {
  if (!t)
    return;
  assert(t->refcount > 0 && "double-release of GreenToken");
  if (--t->refcount > 0)
    return;
  free(t);
}

SyntaxKind green_node_kind(const GreenNode *n) { return n->kind; }
SyntaxKind green_token_kind(const GreenToken *t) { return t->kind; }
uint32_t green_node_text_len(const GreenNode *n) { return n->text_len; }
uint32_t green_token_text_len(const GreenToken *t) { return t->text_len; }
uint32_t green_node_num_children(const GreenNode *n) { return n->num_children; }

GreenElement green_node_child(const GreenNode *n, uint32_t i) {
  if (i >= n->num_children) {
    return (GreenElement){
        .kind = GREEN_ELEM_NODE, .rel_offset = 0, .node = NULL};
  }
  return n->children[i];
}

const char *green_token_text(const GreenToken *t) { return t->text; }

// ---- Cache-facing helpers ------------------------------------------
//
// node_cache.c uses these via the same header — they're declared as
// extern in node_cache.h (private to the syntax module).

uint64_t green_node_hash_of(const GreenNode *n) { return n->content_hash; }
uint64_t green_token_hash_of(const GreenToken *t) { return t->content_hash; }

// =====================================================================
// Mutation helpers — pure-functional, bypass NodeCache
// =====================================================================
//
// Every helper constructs a fresh GreenNode via `green_node_alloc`,
// which retains each child passed in. The input node and its children
// are unmodified. Replaced/removed children have THEIR original
// refcount preserved (the new node simply doesn't reference them);
// it's the caller's responsibility to release the old node separately,
// at which point its references to its original children drop.

GreenNode *green_node_replace_child(const GreenNode *n, uint32_t idx,
                                    GreenElement new_child) {
  assert(idx < n->num_children && "replace_child: idx out of range");
  GreenElement *buf =
      (GreenElement *)malloc((size_t)n->num_children * sizeof(GreenElement));
  if (!buf)
    abort();
  for (uint32_t i = 0; i < n->num_children; i++) {
    buf[i] = (i == idx) ? new_child : n->children[i];
  }
  GreenNode *out = green_node_alloc(n->kind, buf, n->num_children);
  free(buf);
  return out;
}

GreenNode *green_node_splice_children(const GreenNode *n, uint32_t from,
                                      uint32_t to, const GreenElement *replace,
                                      uint32_t count) {
  assert(from <= to && "splice_children: from > to");
  assert(to <= n->num_children && "splice_children: to out of range");
  assert((count == 0 || replace != NULL) &&
         "splice_children: NULL replace + non-zero count");

  uint32_t new_count = n->num_children - (to - from) + count;
  GreenElement *buf = NULL;
  if (new_count > 0) {
    buf = (GreenElement *)malloc((size_t)new_count * sizeof(GreenElement));
    if (!buf)
      abort();
    uint32_t out_idx = 0;
    for (uint32_t i = 0; i < from; i++)
      buf[out_idx++] = n->children[i];
    for (uint32_t i = 0; i < count; i++)
      buf[out_idx++] = replace[i];
    for (uint32_t i = to; i < n->num_children; i++)
      buf[out_idx++] = n->children[i];
    assert(out_idx == new_count);
  }
  GreenNode *out = green_node_alloc(n->kind, buf, new_count);
  free(buf);
  return out;
}

GreenNode *green_node_insert_child(const GreenNode *n, uint32_t idx,
                                   GreenElement new_child) {
  assert(idx <= n->num_children && "insert_child: idx out of range");
  return green_node_splice_children(n, idx, idx, &new_child, 1);
}

GreenNode *green_node_remove_child(const GreenNode *n, uint32_t idx) {
  assert(idx < n->num_children && "remove_child: idx out of range");
  return green_node_splice_children(n, idx, idx + 1, NULL, 0);
}
