#include "node_cache.h"

#include <stdlib.h>
#include <string.h>

// Hash-cons interner. Maps content-hash → bucket-list of green nodes
// or tokens with that hash. Each lookup walks the bucket list, doing
// a content-equality check; on miss, allocates a fresh element and
// adds it to the list.
//
// The rowan dedup heuristic (only nodes with ≤ 3 children participate
// in node dedup) lives in node_cache_intern_node below — large nodes
// skip the cache lookup entirely.

#define ORE_NODE_CACHE_BUCKET_ARENA_CAP (16 * 1024)
#define ORE_NODE_DEDUP_MAX_CHILDREN 3

NodeCache *node_cache_new(void) {
  NodeCache *c = (NodeCache *)calloc(1, sizeof(NodeCache));
  if (!c)
    abort();
  hashmap_init(&c->nodes);
  hashmap_init(&c->tokens);
  arena_init(&c->bucket_arena, ORE_NODE_CACHE_BUCKET_ARENA_CAP);
  return c;
}

static bool free_node_bucket(uint64_t key, void *value, void *user_data) {
  (void)key;
  (void)user_data;
  CacheBucket *b = (CacheBucket *)value;
  while (b) {
    green_node_release((GreenNode *)b->element);
    b = b->next;
  }
  return true;
}
static bool free_token_bucket(uint64_t key, void *value, void *user_data) {
  (void)key;
  (void)user_data;
  CacheBucket *b = (CacheBucket *)value;
  while (b) {
    green_token_release((GreenToken *)b->element);
    b = b->next;
  }
  return true;
}

void node_cache_destroy(NodeCache *cache) {
  if (!cache)
    return;
  // Release every interned element. Bucket structs themselves are
  // freed via arena_free below.
  hashmap_foreach(&cache->nodes, free_node_bucket, NULL);
  hashmap_foreach(&cache->tokens, free_token_bucket, NULL);
  hashmap_free(&cache->nodes);
  hashmap_free(&cache->tokens);
  arena_free(&cache->bucket_arena);
  free(cache);
}

static CacheBucket *bucket_alloc(NodeCache *cache, void *elem,
                                 CacheBucket *next) {
  CacheBucket *b =
      (CacheBucket *)arena_alloc(&cache->bucket_arena, sizeof(CacheBucket));
  b->element = elem;
  b->next = next;
  return b;
}

GreenToken *node_cache_intern_token(NodeCache *cache, SyntaxKind kind,
                                    const char *text, uint32_t text_len) {
  uint64_t hash = green_token_compute_hash(kind, text, text_len);

  CacheBucket *head = (CacheBucket *)hashmap_get(&cache->tokens, hash);
  for (CacheBucket *b = head; b; b = b->next) {
    GreenToken *t = (GreenToken *)b->element;
    if (green_token_equals(t, kind, text, text_len)) {
      green_token_retain(t);
      return t;
    }
  }

  // Miss — allocate and link into the bucket list.
  GreenToken *fresh = green_token_alloc(kind, text, text_len);
  // The cache holds one reference; the caller gets another. Bump.
  green_token_retain(fresh);
  CacheBucket *b = bucket_alloc(cache, fresh, head);
  hashmap_put_or_die(&cache->tokens, hash, b, "node_cache: token bucket");
  return fresh;
}

GreenNode *node_cache_intern_node(NodeCache *cache, SyntaxKind kind,
                                  const GreenElement *children,
                                  uint32_t num_children) {
  // Large nodes skip dedup — the cost outweighs the savings.
  if (num_children > ORE_NODE_DEDUP_MAX_CHILDREN) {
    return green_node_alloc(kind, children, num_children);
  }

  uint64_t hash = green_node_compute_hash(kind, children, num_children);

  CacheBucket *head = (CacheBucket *)hashmap_get(&cache->nodes, hash);
  for (CacheBucket *b = head; b; b = b->next) {
    GreenNode *n = (GreenNode *)b->element;
    if (green_node_equals(n, kind, children, num_children)) {
      // Hit. The existing node already holds child refs equivalent to
      // the ones the caller's elements hold, so we don't touch the
      // caller's refs — they get released by the caller (the builder,
      // when it rolls back the children[] slots that just got consumed
      // into this new wrapper node). Return the cached node with one
      // extra ref for the caller.
      green_node_retain(n);
      return n;
    }
  }

  // Miss — allocate, register, return. green_node_alloc retains each
  // child once for the new node; the caller's parallel refs are still
  // present and the caller releases them (see builder.c).
  GreenNode *fresh = green_node_alloc(kind, children, num_children);
  green_node_retain(fresh); // cache holds one extra ref
  CacheBucket *b = bucket_alloc(cache, fresh, head);
  hashmap_put_or_die(&cache->nodes, hash, b, "node_cache: node bucket");
  return fresh;
}
