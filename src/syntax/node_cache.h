#ifndef ORE_SYNTAX_NODE_CACHE_H
#define ORE_SYNTAX_NODE_CACHE_H

// PRIVATE — internal header, only included from src/syntax/*.c.
// Public NodeCache API is declared in syntax.h.

#include "syntax.h"

#include "../support/data_structure/hashmap.h"

// Defined in green.c. Used by the cache for hash key derivation +
// content equality.
uint64_t green_node_compute_hash(SyntaxKind kind, const GreenElement *children,
                                 uint32_t num_children);
uint64_t green_token_compute_hash(SyntaxKind kind, const char *text,
                                  uint32_t text_len);
bool     green_node_equals(const GreenNode *n, SyntaxKind kind,
                            const GreenElement *children, uint32_t num_children);
bool     green_token_equals(const GreenToken *t, SyntaxKind kind,
                             const char *text, uint32_t text_len);
GreenNode *green_node_alloc(SyntaxKind kind, const GreenElement *children,
                            uint32_t num_children);
GreenToken *green_token_alloc(SyntaxKind kind, const char *text,
                              uint32_t text_len);
uint64_t green_node_hash_of(const GreenNode *n);
uint64_t green_token_hash_of(const GreenToken *t);

// Trivia-EXCLUDING structural hash of a subtree — folds node kinds +
// non-trivia token identities, skips trivia. Position-independent and
// insensitive to whitespace/comment edits (contract C3), but sensitive
// to renames (C4). Used as the per-decl content fingerprint for
// incremental cutoff. O(subtree size); not cached (cf. content_hash).
uint64_t green_structural_hash(const GreenNode *n);

// Cache layout. The HashMap maps hash → bucket-list-head. Each bucket
// is a linked list of (hash, ptr) pairs (we hand-roll the list to
// allow multiple entries per hash bucket without growing the HashMap's
// internal collision chains).
//
// For simplicity in v1: use the HashMap with hash as the key and a
// pointer to a small "BucketList" struct as the value. Bucket lists
// are arena-allocated; the arena is the cache's lifetime owner.
//
// Performance note: this is one extra indirection vs. rowan's
// raw_entry_mut hash-table API. Acceptable for v1 — measure before
// optimizing.

typedef struct CacheBucket CacheBucket;
struct CacheBucket {
    void        *element;  // GreenNode * or GreenToken *
    CacheBucket *next;
};

struct NodeCache {
    HashMap nodes;   // u64 hash → CacheBucket *
    HashMap tokens;  // u64 hash → CacheBucket *
    Arena   bucket_arena;
};

// RETURNS_OWNED. Caller owns one reference; the cache holds an
// additional one (so the GreenToken survives even if the caller
// releases). Idempotent: re-interning the same content returns the
// same pointer.
GreenToken *node_cache_intern_token(NodeCache *cache, SyntaxKind kind,
                                    const char *text, uint32_t text_len);

// RETURNS_OWNED. Same contract as the token variant.
//
// Following rowan's heuristic: only attempts deduplication when
// num_children <= 3. Larger nodes are constructed fresh (the dedup
// lookup overhead exceeds the savings).
//
// Children's references are retained by the resulting node (one ref
// per child, transferred from the caller's references).
GreenNode *node_cache_intern_node(NodeCache *cache, SyntaxKind kind,
                                  const GreenElement *children,
                                  uint32_t num_children);

#endif // ORE_SYNTAX_NODE_CACHE_H
