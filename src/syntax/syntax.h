#ifndef ORE_SYNTAX_H
#define ORE_SYNTAX_H

// =====================================================================
// Ore Syntax — red/green tree (rowan-style) for C.
// =====================================================================
//
// This header is the single public API surface for the syntax library.
// Everything under src/syntax/ is implementation detail; only this
// header is included from outside the module.
//
// EXTRACTION CONTRACT
// ====================
// The syntax library is designed to be extractable as a standalone C
// library. To preserve that property:
//
//   - This header depends only on the C standard library and on
//     src/support/data_structure/ primitives (Arena, Vec, HashMap,
//     FxHash). It does NOT include anything from src/db/, src/parser/,
//     src/sema/, or any other Ore-specific area.
//
//   - SyntaxKind values are opaque uint16_t. The library never references
//     any Ore-specific enum. Callers (parsers + consumers) define their
//     own SyntaxKind enum and pass values across the API as plain
//     uint16_t.
//
//   - Future extraction (when stable): move src/syntax/ + the relevant
//     pieces of src/support/data_structure/ into a separate repo. This
//     header's API surface is the contract that survives extraction.
//
// ARCHITECTURE OVERVIEW
// =====================
// Two layers, following rowan:
//
//   1. GREEN TREE — immutable, content-addressed, refcounted, hash-
//      consed. The "facts" about source structure. Identical subtrees
//      (same kind + same children pointers) deduplicate into a single
//      GreenNode instance. Constructed by the parser via GreenBuilder.
//
//   2. RED TREE — lazy, allocated-on-demand wrapper around the green
//      tree. SyntaxNodes carry parent pointers and absolute byte
//      offsets, enabling navigation in any direction. Multiple
//      SyntaxNode handles may exist for the same green node at the
//      same tree position; they compare equal via their underlying
//      (green ptr, offset, parent) triple.
//
// OWNERSHIP DISCIPLINE
// ====================
// C doesn't have RAII; reference-counting is manual. To make the API
// safe to use we document ownership on every function:
//
//   RETURNS_OWNED — caller is responsible for releasing the returned
//                   handle (via the appropriate _release function).
//                   The function takes a +1 refcount.
//
//   RETURNS_BORROWED — the returned pointer is valid only as long as
//                      the source handle is alive. Caller MUST NOT
//                      call _release on it.
//
//   TAKES_OWNERSHIP — the function consumes the caller's reference.
//                     Caller MUST NOT use the handle after the call.
//                     (Rare; we prefer borrowed-by-default + explicit
//                     clone.)
//
//   INC_REFS — the function adds a reference to the argument. Caller's
//              own reference is unaffected.
//
// The helper macro SYN_RELEASE(handle) releases a handle and nulls the
// local pointer in one step, catching use-after-release at the next
// dereference rather than at the next double-free.
//
// THREADING
// =========
// v1 is single-threaded. Refcounts are plain u32 (not atomic). Multi-
// threaded green-tree sharing is a future enhancement requiring
// atomic refcounts and a lock-free node cache.
//
// =====================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../support/data_structure/arena.h"
#include "../support/data_structure/hashmap.h"
#include "../support/data_structure/vec.h"


// =====================================================================
// Core types
// =====================================================================

// SyntaxKind — opaque to the library. Callers define their own enum.
// Reserved value: SYNTAX_KIND_NONE (0) — used as a sentinel for "no
// kind" in builder error paths. Callers should not assign 0 to any
// real kind.
typedef uint16_t SyntaxKind;
#define SYNTAX_KIND_NONE ((SyntaxKind)0)

// Source byte range. Half-open [start, start + length).
typedef struct {
    uint32_t start;
    uint32_t length;
} TextRange;

#define TEXT_RANGE_NONE ((TextRange){.start = 0, .length = 0})

static inline uint32_t text_range_end(TextRange r) { return r.start + r.length; }
static inline bool text_range_contains(TextRange outer, TextRange inner) {
    return inner.start >= outer.start && text_range_end(inner) <= text_range_end(outer);
}
static inline bool text_range_eq(TextRange a, TextRange b) {
    return a.start == b.start && a.length == b.length;
}

// Forward declarations — all concrete types are opaque to consumers.
typedef struct GreenNode    GreenNode;
typedef struct GreenToken   GreenToken;
typedef struct SyntaxNode   SyntaxNode;
typedef struct SyntaxToken  SyntaxToken;
typedef struct SyntaxTree   SyntaxTree;
typedef struct NodeCache    NodeCache;
typedef struct GreenBuilder GreenBuilder;

// SyntaxElement — discriminated union of a SyntaxNode or SyntaxToken.
// Returned by APIs that iterate children/siblings without knowing in
// advance whether the next element is a node or a token (which is the
// common case during sema walks and hover lookups).
//
// Ownership: the embedded handle is RETURNS_OWNED by convention.
// Use SYN_ELEM_RELEASE(e) to release whichever variant is held.
typedef enum : uint8_t {
    SYNTAX_ELEM_NODE  = 0,
    SYNTAX_ELEM_TOKEN = 1,
} SyntaxElementKind;

typedef struct {
    SyntaxElementKind kind;
    union {
        SyntaxNode  *node;
        SyntaxToken *token;
    };
} SyntaxElement;

#define SYNTAX_ELEMENT_NONE ((SyntaxElement){.kind = SYNTAX_ELEM_NODE, .node = NULL})

static inline bool syntax_element_is_none(SyntaxElement e) {
    return (e.kind == SYNTAX_ELEM_NODE && e.node == NULL) ||
           (e.kind == SYNTAX_ELEM_TOKEN && e.token == NULL);
}

// SyntaxNodePtr — stable identity that survives reparses.
//
// Stores (kind, range). To resolve against a fresh tree (after the
// source has been reparsed), call syntax_node_ptr_resolve. The pointer
// finds the green node whose kind and absolute byte range match the
// recorded values via descend-and-binary-search from the root.
//
// Use this as the canonical anchor for diagnostics, hover targets,
// cached analysis results, and anything else that must remain valid
// across reparses.
typedef struct {
    SyntaxKind kind;
    TextRange  range;
} SyntaxNodePtr;


// =====================================================================
// NodeCache — hash-cons interner
// =====================================================================
//
// Stores all unique GreenNodes and GreenTokens, deduplicating identical
// subtrees. One cache is shared across as many parse sessions as the
// caller wants — keeping it alive across reparses gives structural
// sharing (unchanged subtrees produce the same GreenNode pointer in
// successive parses).
//
// Following rowan's heuristic: nodes with > 3 children are NOT
// deduplicated (the dedup-lookup cost outweighs the savings). Tokens
// are always deduplicated. Expect ~15-20% memory savings on real
// codebases from node deduplication; token dedup is much higher
// (every `+`, every `;`, every common identifier becomes one allocation).

// RETURNS_OWNED. Allocates a fresh cache via malloc.
NodeCache *node_cache_new(void);
void       node_cache_destroy(NodeCache *cache);


// =====================================================================
// GreenBuilder — construction API
// =====================================================================
//
// Parser-facing. Stack of in-progress nodes:
//
//   builder_start_node(b, EXPR_BINOP);
//     builder_start_node(b, EXPR_LITERAL);
//       builder_token(b, INT, "5", 1);
//     builder_finish_node(b);
//     builder_token(b, PLUS, "+", 1);
//     builder_start_node(b, EXPR_LITERAL);
//       builder_token(b, INT, "3", 1);
//     builder_finish_node(b);
//   builder_finish_node(b);
//
//   GreenNode *root = builder_finish(b);  // RETURNS_OWNED
//
// Checkpoint pattern for Pratt-style operator precedence:
//
//   Checkpoint cp = builder_checkpoint(b);
//   parse_primary(b);
//   if (peek() == PLUS) {
//       builder_start_node_at(b, cp, EXPR_BINOP);  // wraps the primary
//       builder_token(b, PLUS, "+", 1);
//       parse_primary(b);
//       builder_finish_node(b);
//   }
//
// The builder borrows the NodeCache for the lifetime of the build.
// Multiple builders may share one cache (sequentially, single-threaded).

typedef uint32_t Checkpoint;

// RETURNS_OWNED. Heap-allocates a builder borrowing `cache` for its
// lifetime. The cache must outlive the builder.
GreenBuilder *green_builder_new(NodeCache *cache);
void          green_builder_destroy(GreenBuilder *b);

void green_builder_start_node(GreenBuilder *b, SyntaxKind kind);

// Emit a token. text need not outlive the call; the builder copies
// text into the interned GreenToken on demand.
void green_builder_token(GreenBuilder *b, SyntaxKind kind,
                          const char *text, uint32_t text_len);

void green_builder_finish_node(GreenBuilder *b);

// Capture the current position. start_node_at(cp, kind) later wraps
// everything emitted AFTER cp inside a new node of `kind`.
Checkpoint green_builder_checkpoint(GreenBuilder *b);
void       green_builder_start_node_at(GreenBuilder *b, Checkpoint cp,
                                         SyntaxKind kind);

// RETURNS_OWNED. Finishes the outermost in-progress node and returns
// it. The builder must have exactly one open node when called.
// After this returns, the builder is in a clean state and may be
// re-used for another parse (its cache reference persists).
GreenNode *green_builder_finish(GreenBuilder *b);


// =====================================================================
// GreenNode + GreenToken — immutable tree primitives
// =====================================================================
//
// Refcounted. Every handle held by the caller represents one reference.
// Use _retain to add a reference, _release to drop one. When the last
// reference drops, the node is freed (and its children's references
// are released — cascading-free).
//
// The green tree is IMMUTABLE post-construction. Do not attempt to
// modify a returned GreenNode/GreenToken.

void green_node_retain(GreenNode *n);    // INC_REFS
void green_node_release(GreenNode *n);   // drops one ref; may free

void green_token_retain(GreenToken *t);  // INC_REFS
void green_token_release(GreenToken *t); // drops one ref; may free

SyntaxKind green_node_kind(const GreenNode *n);
SyntaxKind green_token_kind(const GreenToken *t);

// Total byte width of the subtree under this node.
uint32_t green_node_text_len(const GreenNode *n);
uint32_t green_token_text_len(const GreenToken *t);

uint32_t green_node_num_children(const GreenNode *n);

// Element variant — a green child is either a node or a token.
typedef enum : uint8_t {
    GREEN_ELEM_NODE  = 0,
    GREEN_ELEM_TOKEN = 1,
} GreenElementKind;

typedef struct {
    GreenElementKind kind;
    uint32_t         rel_offset;  // byte offset within parent
    union {
        GreenNode  *node;
        GreenToken *token;
    };
} GreenElement;

// RETURNS_BORROWED. Returns child `i` (0-indexed). Caller MUST NOT
// release the embedded node/token pointer — it's owned by the parent
// GreenNode. If i is out of range, returns a zero-initialized
// GreenElement (kind == GREEN_ELEM_NODE, node == NULL).
GreenElement green_node_child(const GreenNode *n, uint32_t i);

// RETURNS_BORROWED. Token text, NUL-terminated for convenience.
// Always non-NULL; zero-length tokens return "".
const char *green_token_text(const GreenToken *t);


// ---- Green-tree mutation (pure-functional) -------------------------
//
// These return a FRESH GreenNode reflecting the requested edit. The
// input `n` is unchanged; its refcount is unaffected. The returned
// node has rc=1 and retains all of its (kept + inserted) children;
// callers release it like any other RETURNS_OWNED green node.
//
// Mutated nodes BYPASS the NodeCache — once you mutate, structural
// sharing with parse-produced trees is broken by definition. (The
// hash-cons interner is for canonical parse output only.)
//
// Used by `respine` (Phase 4d) to propagate edits up the parent chain.
// May also be used standalone by callers that want to produce a
// modified green tree without involving the red-tree layer.

// RETURNS_OWNED. Replaces child `idx` with `new_child`. Asserts idx
// in range.
GreenNode *green_node_replace_child(const GreenNode *n, uint32_t idx,
                                      GreenElement new_child);

// RETURNS_OWNED. Inserts `new_child` at position `idx` (so it becomes
// the new child[idx]; existing children at and after idx shift up by
// one). idx == n's num_children appends.
GreenNode *green_node_insert_child(const GreenNode *n, uint32_t idx,
                                     GreenElement new_child);

// RETURNS_OWNED. Removes child at `idx`. Asserts idx in range.
GreenNode *green_node_remove_child(const GreenNode *n, uint32_t idx);

// RETURNS_OWNED. Replaces the half-open range [from, to) with the
// `count` elements at `replace`. `from <= to <= n's num_children`;
// `replace` may be NULL iff count == 0. Net delta = count - (to - from).
GreenNode *green_node_splice_children(const GreenNode *n,
                                        uint32_t from, uint32_t to,
                                        const GreenElement *replace,
                                        uint32_t count);


// =====================================================================
// SyntaxTree — root of a parsed file
// =====================================================================
//
// Owns the root GreenNode (one reference) and provides cheap access
// to the root SyntaxNode.

// RETURNS_OWNED. Takes ownership of `root` — caller MUST NOT release
// `root` after calling. The tree is IMMUTABLE: navigation always
// produces fresh SyntaxNode handles; no two handles ever alias the
// same NodeData allocation.
SyntaxTree *syntax_tree_new(GreenNode *root /* TAKES_OWNERSHIP */);

// RETURNS_OWNED. Like `syntax_tree_new` but flags the tree as MUTABLE:
// navigation maintains a per-parent intrusive sorted linked list of
// live child handles, so two calls returning the same logical child
// position yield the SAME SyntaxNode pointer (with refcount bumped).
// This is the property that lets future mutation ops (detach, attach,
// splice — Phase 4d) be observed by every live handle.
//
// Mutable trees do NOT participate in the NodeCache (mutation is the
// opposite of structural sharing). The green tree under a mutable
// SyntaxTree is still refcounted normally.
SyntaxTree *syntax_tree_new_mut(GreenNode *root /* TAKES_OWNERSHIP */);

void        syntax_tree_free(SyntaxTree *t);

// RETURNS_OWNED. The caller releases the returned SyntaxNode when done.
// The returned node inherits the tree's mutability mode.
SyntaxNode *syntax_tree_root(SyntaxTree *t);


// =====================================================================
// SyntaxNode — red tree (navigation handle)
// =====================================================================
//
// A SyntaxNode wraps a (GreenNode, parent_SyntaxNode, index_in_parent,
// abs_offset) tuple. Constructed lazily during navigation; multiple
// SyntaxNodes may exist for the same logical tree position. They
// compare equal via the underlying tuple, not pointer identity.
//
// Each handle is one reference; cascade-free walks up the parent
// chain when the last child reference drops.

void syntax_node_retain(SyntaxNode *n);   // INC_REFS
void syntax_node_release(SyntaxNode *n);  // drops one ref; may free

SyntaxKind  syntax_node_kind(const SyntaxNode *n);
TextRange   syntax_node_text_range(const SyntaxNode *n);

// RETURNS_BORROWED. The underlying green node — caller MUST NOT
// release it. Useful for hash-key derivation or direct child iteration
// without going through SyntaxNode allocation.
const GreenNode *syntax_node_green(const SyntaxNode *n);

// RETURNS_OWNED. NULL if `n` is the root.
//
// Navigation functions take NON-const SyntaxNode * because they
// manipulate the parent's refcount (the returned child holds a +1
// ref on its parent for cascade-free correctness). Const-correctness
// would mean expressing "logically read-only, internally mutates
// refcount" which C has no idiomatic way to spell — rowan handles
// this via Rust's Cell<u32>. We accept the API ergonomics hit.
SyntaxNode *syntax_node_parent(SyntaxNode *n);

// Number of CHILD GREEN ELEMENTS (nodes + tokens combined). Matches
// green_node_num_children for the underlying green node.
uint32_t    syntax_node_num_children(const SyntaxNode *n);

// RETURNS_OWNED. NULL if i is out of range OR if child i is a token
// (use green_node_child via syntax_node_green for token access).
// Walks the underlying green child at index i; allocates a fresh
// SyntaxNode wrapping it.
SyntaxNode *syntax_node_child(SyntaxNode *n, uint32_t i);

// RETURNS_OWNED. The first node-typed child, NULL if none.
SyntaxNode *syntax_node_first_child(SyntaxNode *n);

// RETURNS_OWNED. The next node-typed sibling, NULL if none.
SyntaxNode *syntax_node_next_sibling(SyntaxNode *n);

// True iff this node has the given kind. Convenience for filter chains.
static inline bool syntax_node_has_kind(const SyntaxNode *n, SyntaxKind k) {
    return syntax_node_kind(n) == k;
}


// ---- Backward navigation -------------------------------------------
//
// Symmetric to next_sibling. Used by completion and parser-recovery
// patterns that walk leftward from a position.

// RETURNS_OWNED. The previous node-typed sibling, NULL if none.
SyntaxNode *syntax_node_prev_sibling(SyntaxNode *n);


// ---- Element navigation (node-or-token) ----------------------------
//
// These return SyntaxElement so iteration covers both nodes and
// tokens without re-walking. SyntaxElement is the "did I get a node
// or a token at position i?" return type.

// RETURNS_OWNED (the embedded handle). Out-of-range returns SYNTAX_ELEMENT_NONE.
SyntaxElement syntax_node_child_or_token(SyntaxNode *n, uint32_t i);

// RETURNS_OWNED. The first child element (node OR token), NONE if `n`
// has no children.
SyntaxElement syntax_node_first_child_or_token(SyntaxNode *n);

// RETURNS_OWNED. The last child element (node OR token), NONE if `n`
// has no children.
SyntaxElement syntax_node_last_child_or_token(SyntaxNode *n);

// RETURNS_OWNED. The next sibling element of `n` (looking at the
// parent's children array). NONE if `n` is the last child.
SyntaxElement syntax_node_next_sibling_or_token(SyntaxNode *n);
SyntaxElement syntax_node_prev_sibling_or_token(SyntaxNode *n);


// ---- SyntaxToken — red-tree wrapper around GreenToken --------------
//
// Tokens are first-class navigation handles, parallel to SyntaxNode.
// Each token has a parent (tokens are always children of a node;
// the root is always a node).

void syntax_token_retain(SyntaxToken *t);
void syntax_token_release(SyntaxToken *t);

SyntaxKind        syntax_token_kind(const SyntaxToken *t);
TextRange         syntax_token_text_range(const SyntaxToken *t);
const char       *syntax_token_text(const SyntaxToken *t);
const GreenToken *syntax_token_green(const SyntaxToken *t);

// RETURNS_OWNED. A token always has a parent.
SyntaxNode *syntax_token_parent(SyntaxToken *t);

// RETURNS_OWNED (the embedded handle). Sibling iteration crossing
// node/token boundaries.
SyntaxElement syntax_token_next_sibling_or_token(SyntaxToken *t);
SyntaxElement syntax_token_prev_sibling_or_token(SyntaxToken *t);

// RETURNS_OWNED. The next/prev token in DOCUMENT order, crossing
// parent boundaries as needed (i.e., not restricted to siblings).
// Returns NULL if `t` is the last/first token in the tree.
//
// Algorithm: walk up ancestors until one has a next/prev sibling-or-
// token, then descend into that sibling's leftmost (next) or
// rightmost (prev) leaf token.
SyntaxToken *syntax_token_next_token(SyntaxToken *t);
SyntaxToken *syntax_token_prev_token(SyntaxToken *t);


// ---- Leaf navigation -----------------------------------------------
//
// "Give me the leftmost/rightmost token in this subtree." Used by
// span arithmetic and hover-precision logic.

// RETURNS_OWNED. NULL if `n`'s subtree contains no tokens (e.g.,
// an empty node or a node whose children are all empty).
SyntaxToken *syntax_node_first_token(SyntaxNode *n);
SyntaxToken *syntax_node_last_token(SyntaxNode *n);


// ---- Token-precise positioning -------------------------------------
//
// "What token does the cursor land on at byte offset X?" The canonical
// LSP hover/completion first step.
//
// Resolution uses INCLUSIVE-on-both-ends interval matching: at every
// level we find the (one or two) children whose range contains `offset`.
// If two children match, the offset sits on the boundary between them
// — the result is the BETWEEN variant carrying both tokens. Otherwise
// SINGLE, or NONE if offset is outside the tree.
//
// All embedded handles in the result are RETURNS_OWNED. Use
// TOKEN_AT_OFFSET_RELEASE(r) to release whichever handles the variant
// carries.

typedef enum : uint8_t {
    TOKEN_AT_OFFSET_NONE    = 0,  // offset outside the subtree (or empty subtree)
    TOKEN_AT_OFFSET_SINGLE  = 1,  // exactly one token contains offset
    TOKEN_AT_OFFSET_BETWEEN = 2,  // offset is the boundary between two tokens
} TokenAtOffsetKind;

typedef struct {
    TokenAtOffsetKind kind;
    SyntaxToken      *single;     // valid for SINGLE
    SyntaxToken      *left;       // valid for BETWEEN
    SyntaxToken      *right;      // valid for BETWEEN
} TokenAtOffset;

// Releases whichever handle(s) the result carries.
#define TOKEN_AT_OFFSET_RELEASE(r) do {                                      \
    TokenAtOffset _r = (r);                                                  \
    switch (_r.kind) {                                                       \
        case TOKEN_AT_OFFSET_SINGLE:                                         \
            syntax_token_release(_r.single); break;                          \
        case TOKEN_AT_OFFSET_BETWEEN:                                        \
            syntax_token_release(_r.left);                                   \
            syntax_token_release(_r.right); break;                           \
        case TOKEN_AT_OFFSET_NONE: break;                                    \
    }                                                                        \
} while (0)

TokenAtOffset syntax_token_at_offset(SyntaxNode *root, uint32_t offset);


// ---- Mutable mode --------------------------------------------------
//
// Most consumers want immutable trees: parse-once, query-many. Mutable
// trees are an alternate mode used by future refactor tooling (rename,
// extract-fn, code actions) where the tree is edited in place and
// every live handle to an edited node observes the edit.
//
// A SyntaxNode/Token is born mutable iff its tree was created via
// syntax_tree_new_mut OR iff it descends from a mutable parent OR if
// it was produced by syntax_node_clone_for_update. Immutable trees can
// be CLONED into a mutable copy without touching the original.

bool syntax_node_is_mutable (const SyntaxNode  *n);
bool syntax_token_is_mutable(const SyntaxToken *t);

// RETURNS_OWNED. Deep-clones the ancestor chain of `node` to root,
// producing a fresh mutable tree whose leaf-handle is logically
// equivalent to `node` (same kind, same green pointer, same path).
// The green tree underneath is SHARED (its refcount is bumped, not
// duplicated). The original `node` and its tree are unaffected.
//
// Precondition: `node` must be immutable. Cloning a mutable handle
// asserts/aborts (rowan parity — `clone_for_update` is only for the
// immutable→mutable transition).
//
// The returned handle's root is reachable via repeated syntax_node_parent
// calls; the root has parent == NULL.
SyntaxNode *syntax_node_clone_for_update(SyntaxNode *node);

// RETURNS_OWNED. Re-roots `node`'s subtree as the root of a fresh immutable
// tree, sharing the underlying GreenNode (its rc is bumped). The returned
// SyntaxNode has parent == NULL and offset == 0; its green tree is the
// same allocation as `node->green`. The original tree is unaffected.
//
// Use this when you want to extract a subtree as its own standalone tree
// for analysis or transformation, without inheriting the original's
// parent chain or mutability mode.
SyntaxNode *syntax_node_clone_subtree(const SyntaxNode *node);


// ---- Kind-filtered matchers ----------------------------------------
//
// Convenience filters that combine navigation + kind check in one call.
// Each walks at most as far as the existing navigation primitive; absent
// matches return NULL / SYNTAX_ELEMENT_NONE.

// RETURNS_OWNED. The first node-typed child whose kind == `k`, or NULL.
SyntaxNode *syntax_node_first_child_by_kind(SyntaxNode *n, SyntaxKind k);

// RETURNS_OWNED. The first child element (node OR token) whose kind == `k`.
SyntaxElement syntax_node_first_child_or_token_by_kind(SyntaxNode *n, SyntaxKind k);

// RETURNS_OWNED. The next node-typed sibling of `n` whose kind == `k`, or NULL.
SyntaxNode *syntax_node_next_sibling_by_kind(SyntaxNode *n, SyntaxKind k);

// RETURNS_OWNED. The next sibling element whose kind == `k`.
SyntaxElement syntax_node_next_sibling_or_token_by_kind(SyntaxNode *n, SyntaxKind k);

// RETURNS_OWNED. Same but starting from a token.
SyntaxElement syntax_token_next_sibling_or_token_by_kind(SyntaxToken *t, SyntaxKind k);


// ---- Range-based positioning ---------------------------------------

// RETURNS_OWNED. The smallest element (node or token) that fully
// contains `range`. Returns SYNTAX_ELEMENT_NONE if `range` is not
// contained in `n`'s subtree.
//
// "Smallest" = descend through children as long as a single child fully
// contains the range; stop when no child fully contains it (the current
// element is then the smallest containing one).
SyntaxElement syntax_node_covering_element(SyntaxNode *n, TextRange range);

// RETURNS_OWNED. The DIRECT child of `n` whose range fully contains
// `range`. Returns SYNTAX_ELEMENT_NONE if no single child fully contains
// it (e.g., the range spans two siblings). Unlike `covering_element`,
// this does NOT recurse — it answers "which immediate child holds this
// range?", not "what's the deepest node containing it?"
SyntaxElement syntax_node_child_or_token_at_range(SyntaxNode *n, TextRange range);


// ---- Mutation primitives -------------------------------------------
//
// All operations require their target to be MUTABLE (assert otherwise).
// A typical usage flow is:
//
//   1. Get a mutable handle:
//        SyntaxNode *root = syntax_tree_root(tree_mut);
//        // or:
//        SyntaxNode *mut = syntax_node_clone_for_update(immutable);
//   2. Mutate via detach/attach/splice/replace_with.
//   3. Read the resulting green tree via syntax_node_green(root) or
//      via the SyntaxText / SyntaxNodePtr helpers (which see the new
//      state because they read from node.green which has been respined).
//   4. Release the handle when done.

// Detach `node` from its parent. After this, `node->parent` is NULL
// and the node becomes a standalone subtree root. Asserts mutable.
// The original handle's refcount is unchanged (caller still owns it).
//
// If `node` was already detached (parent NULL), this is a no-op.
void syntax_node_detach (SyntaxNode  *node);
void syntax_token_detach(SyntaxToken *tok);

// Splice children: delete children[from..to), then insert `replace[0..n]`
// at position `from`. Each element of `replace` must be a detached
// mutable handle whose parent is NULL (asserted). The parent's green
// tree is rebuilt and respined all the way to root.
//
// Net child-count delta = `n` - (to - from). Detached children that
// were spliced out have their handles released by this function;
// callers that wish to retain them should explicitly detach + retain
// before calling.
void syntax_node_splice_children(SyntaxNode *parent,
                                  uint32_t from, uint32_t to,
                                  const SyntaxElement *replace, uint32_t n);

// Replace `node` with a fresh GreenNode. Kind must match (asserts).
//
// For MUTABLE nodes: respines in place — the entire ancestor chain
// from `node` to root gets fresh GreenNodes propagated up. The
// returned GreenNode is the NEW ROOT of the (now-modified) tree
// (RETURNS_OWNED — caller releases). Every live handle to a mutated
// node observes the new state via its (cached, but now up-to-date)
// `green` pointer.
//
// For IMMUTABLE nodes: builds a new tree by walking up from `node`,
// replacing each ancestor's child slot. The original tree is
// unaffected. The returned GreenNode is the new root.
GreenNode  *syntax_node_replace_with (SyntaxNode  *node, GreenNode  *replacement);
GreenNode  *syntax_token_replace_with(SyntaxToken *tok,  GreenToken *replacement);


// =====================================================================
// Tree iterators
// =====================================================================
//
// Following rowan's pattern: iterators are CURSOR STRUCTS the caller
// drives via _next. Each _next call returns the next element (or
// SYNTAX_ELEMENT_NONE / NULL for terminators). The caller releases
// returned handles via SYN_RELEASE / SYN_ELEM_RELEASE.
//
// All iterators are SINGLE-USE; once _next returns NONE the cursor is
// exhausted. Cursors are stack-allocated.
//
// Pattern:
//   SyntaxNodeIter it;
//   syntax_node_iter_descendants(&it, root);
//   for (SyntaxNode *n; (n = syntax_node_iter_next(&it)); ) {
//       // ... use n ...
//       SYN_RELEASE(n);
//   }
//   syntax_node_iter_free(&it);

// ---- Direction --------
typedef enum : uint8_t { SYNTAX_DIR_NEXT = 0, SYNTAX_DIR_PREV = 1 } SyntaxDirection;

// ---- Ancestors (walks up from `start` until root) -------------
//
// Yields nodes in walk-order: start, start.parent, start.parent.parent, ...
// up to the root. Each yielded node is RETURNS_OWNED; caller releases.
typedef struct {
    SyntaxNode *current;   // owned; advances each step
    bool        emitted_first;
} SyntaxAncestors;

void        syntax_ancestors_init(SyntaxAncestors *it, SyntaxNode *start);
SyntaxNode *syntax_ancestors_next(SyntaxAncestors *it);
void        syntax_ancestors_free(SyntaxAncestors *it);

// Token-rooted ancestors: yields token.parent, parent.parent, ..., root.
// (Does NOT yield the token itself — only ancestor SyntaxNodes.)
void        syntax_token_ancestors_init(SyntaxAncestors *it, SyntaxToken *start);

// ---- Children iteration (nodes only) -----------------------------
typedef struct {
    SyntaxNode     *parent;
    uint32_t        next_index;
    SyntaxDirection dir;
} SyntaxChildren;

void        syntax_children_init(SyntaxChildren *it, SyntaxNode *parent,
                                  SyntaxDirection dir);
SyntaxNode *syntax_children_next(SyntaxChildren *it);
void        syntax_children_free(SyntaxChildren *it);

// ---- Children iteration with tokens (any element) ----------------
typedef struct {
    SyntaxNode     *parent;
    uint32_t        next_index;
    SyntaxDirection dir;
} SyntaxChildrenElem;

void          syntax_children_elem_init(SyntaxChildrenElem *it, SyntaxNode *parent,
                                         SyntaxDirection dir);
SyntaxElement syntax_children_elem_next(SyntaxChildrenElem *it);
void          syntax_children_elem_free(SyntaxChildrenElem *it);

// ---- WalkEvent (for preorder/postorder traversals) ---------------
typedef enum : uint8_t {
    SYNTAX_WALK_ENTER = 0,
    SYNTAX_WALK_LEAVE = 1,
} SyntaxWalkEventKind;

typedef struct {
    SyntaxWalkEventKind kind;
    SyntaxElement       element;  // RETURNS_OWNED on the embedded handle
} SyntaxWalkEvent;

#define SYNTAX_WALK_EVENT_NONE \
    ((SyntaxWalkEvent){.kind = SYNTAX_WALK_ENTER, .element = SYNTAX_ELEMENT_NONE})

static inline bool syntax_walk_event_is_none(SyntaxWalkEvent e) {
    return syntax_element_is_none(e.element);
}

// ---- Preorder traversal -----------------------------------------
//
// Yields Enter(node/token), then recursively the subtree, then
// Leave(node) (tokens have no subtree so no Leave is emitted for
// them — but for symmetry, we DO emit Leave for tokens with the
// same range as the Enter event; consumers can ignore if they only
// care about nodes).
//
// Equivalent to rowan's preorder_with_tokens().
typedef struct {
    // The cursor holds an owned SyntaxElement representing the current
    // position. The state machine alternates between "Enter just emitted,
    // descend or move-right next" and "Leave-equivalent processing".
    SyntaxElement   current;
    bool            descending;     // next step should descend into current
    SyntaxNode     *root;           // the subtree root (we don't ascend past it)
    bool            started;
    bool            finished;
    bool            skip_pending;   // set by syntax_preorder_skip_subtree
} SyntaxPreorder;

void            syntax_preorder_init(SyntaxPreorder *it, SyntaxNode *root);
SyntaxWalkEvent syntax_preorder_next(SyntaxPreorder *it);
void            syntax_preorder_free(SyntaxPreorder *it);

// ---- Descendants (nodes only, preorder, excluding root) ---------
//
// Convenience wrapper around preorder filtering to nodes only and
// skipping the root.
typedef struct {
    SyntaxPreorder po;
} SyntaxDescendants;

void        syntax_descendants_init(SyntaxDescendants *it, SyntaxNode *root);
SyntaxNode *syntax_descendants_next(SyntaxDescendants *it);
void        syntax_descendants_free(SyntaxDescendants *it);


// ---- Descendants with tokens (preorder, excluding root) ---------
//
// Like SyntaxDescendants but yields BOTH nodes and tokens (filtered
// to ENTER events; LEAVE events are absorbed). Useful for refactor
// tools that need to iterate every element in document order.
typedef struct {
    SyntaxPreorder po;
} SyntaxDescendantsElem;

void          syntax_descendants_elem_init(SyntaxDescendantsElem *it, SyntaxNode *root);
SyntaxElement syntax_descendants_elem_next(SyntaxDescendantsElem *it);
void          syntax_descendants_elem_free(SyntaxDescendantsElem *it);


// ---- Preorder::skip_subtree -------------------------------------
//
// Instruct the preorder cursor to skip descending into the CURRENT
// Enter event's children. Call after observing an Enter event but
// before the next call to syntax_preorder_next; the next call will
// proceed as if the current node has no children (yields Leave
// immediately, then moves to next sibling / up the chain).
//
// No-op if the cursor is not currently positioned on an Enter event
// (e.g., called twice in a row, or after Leave).
void syntax_preorder_skip_subtree(SyntaxPreorder *it);


// ---- Siblings (forward or backward iteration including self) -----
//
// Yields `self` first, then walks in `dir` direction yielding each
// sibling until exhausted. Self is included so callers can build
// "context lists" naturally.
typedef struct {
    SyntaxNode      *cur;      // owned; advances each step
    SyntaxDirection  dir;
    bool             emitted_first;
} SyntaxSiblings;

void        syntax_siblings_init(SyntaxSiblings *it, SyntaxNode *start, SyntaxDirection dir);
SyntaxNode *syntax_siblings_next(SyntaxSiblings *it);
void        syntax_siblings_free(SyntaxSiblings *it);

// Same shape, but yields both nodes and tokens.
typedef struct {
    SyntaxElement    cur;      // owned
    SyntaxDirection  dir;
    bool             emitted_first;
} SyntaxSiblingsElem;

void          syntax_siblings_elem_init_node (SyntaxSiblingsElem *it, SyntaxNode  *start, SyntaxDirection dir);
void          syntax_siblings_elem_init_token(SyntaxSiblingsElem *it, SyntaxToken *start, SyntaxDirection dir);
SyntaxElement syntax_siblings_elem_next      (SyntaxSiblingsElem *it);
void          syntax_siblings_elem_free      (SyntaxSiblingsElem *it);


// =====================================================================
// SyntaxText — lazy text view over a subtree
// =====================================================================
//
// A SyntaxText is a (node, range) pair describing a slice of source
// text covered by a subtree. The text isn't materialized — it's
// iterated lazily by walking the subtree's leaf tokens and clipping
// against the slice's range.
//
// Use cases:
//   - Sema reading the source text of a node (e.g., the literal
//     digits of a NUMBER token, or the entire body of a function).
//   - Diagnostic rendering ("expected X here" with the actual source).
//   - LSP completions matching against partial typed text.
//
// SyntaxText is a STACK VALUE — no allocation, no refcount. It
// borrows the underlying SyntaxNode for its lifetime: the caller
// must keep the SyntaxNode alive while the SyntaxText is in use.

typedef struct {
    SyntaxNode *node;   // BORROWED; caller keeps alive
    TextRange   range;  // absolute byte range within the document
} SyntaxText;

// Construct a SyntaxText covering the entire subtree under `n`.
SyntaxText syntax_text_of(SyntaxNode *n);

// Construct a sub-slice of a SyntaxText. `range` is in document-
// absolute byte coordinates. Caller is responsible for ensuring the
// range is contained within `src->range`; otherwise behavior is
// undefined (debug builds assert).
SyntaxText syntax_text_slice(const SyntaxText *src, TextRange range);

uint32_t syntax_text_len(const SyntaxText *st);
bool     syntax_text_is_empty(const SyntaxText *st);

// Visit each contiguous chunk of source text inside this SyntaxText
// in document order. Returns false to stop early; the visit returns
// false in that case too. Returns true after a full walk.
typedef bool (*SyntaxTextChunkVisitor)(const char *text, uint32_t text_len,
                                        void *user);
bool syntax_text_for_each_chunk(const SyntaxText *st,
                                 SyntaxTextChunkVisitor visitor, void *user);

// Materialize into a caller-provided buffer. Writes up to bufcap-1
// bytes plus a NUL terminator. Returns the total byte length (which
// may be > bufcap-1, indicating truncation — snprintf-style).
size_t syntax_text_to_cstr(const SyntaxText *st, char *buf, size_t bufcap);

// Return the byte at the given document-absolute offset, or -1 if
// offset is outside this SyntaxText's range.
int syntax_text_byte_at(const SyntaxText *st, uint32_t offset);

// True if any byte in this SyntaxText equals `c`. Convenience.
bool syntax_text_contains_byte(const SyntaxText *st, char c);

// Find the first occurrence of `c`. Returns document-absolute offset
// or UINT32_MAX on miss.
uint32_t syntax_text_find_byte(const SyntaxText *st, char c);

// Compare to a NUL-terminated C string. True iff the SyntaxText's
// content exactly equals the string's bytes.
bool syntax_text_eq_cstr(const SyntaxText *st, const char *cstr);


// ---- UTF-8 char-level API (Phase 4f) -------------------------------
//
// Char-level analogs of the byte-level helpers above. For ASCII source
// these behave identically to their byte counterparts (every byte is a
// 1-byte char); for multi-byte UTF-8 input they decode runs of 1–4
// bytes per call.
//
// `offset` is always a BYTE offset (matches rowan's TextSize); if
// `offset` lands inside a multi-byte sequence (i.e., is not at a UTF-8
// boundary), char_at returns -1. Malformed UTF-8 bytes also return -1.

// Returns the Unicode code point at byte `offset`, or -1 if offset is
// outside this SyntaxText OR not at a valid UTF-8 boundary.
int32_t syntax_text_char_at(const SyntaxText *st, uint32_t offset);

// True if any code point equals `c`.
bool syntax_text_contains_char(const SyntaxText *st, int32_t c);

// First byte offset where code point `c` appears. UINT32_MAX on miss.
uint32_t syntax_text_find_char(const SyntaxText *st, int32_t c);

// Early-exit fold over chunks. `fn` returns `false` to terminate the
// walk; the final accumulator is returned via *out_acc. Returns true
// if the walk completed without early exit, false if `fn` aborted.
typedef bool (*SyntaxTextChunkFoldFn)(const char *text, uint32_t len,
                                       void *acc, void *user);
bool syntax_text_try_fold_chunks(const SyntaxText *st,
                                  SyntaxTextChunkFoldFn fn,
                                  void *acc, void *user);


// =====================================================================
// SyntaxNodePtr — stable identity across reparses
// =====================================================================
//
// To use:
//   SyntaxNodePtr ptr = syntax_node_ptr_new(some_node);
//   // ... save ptr in a diagnostic, cache, etc.
//   // ... reparse the file ...
//   SyntaxNode *fresh = syntax_node_ptr_resolve(ptr, new_tree_root);
//   if (fresh) { /* found the same logical node */ }
//
// Resolution complexity: O(depth * log(width)). Walks from root,
// binary-searching children by range at each level.

SyntaxNodePtr syntax_node_ptr_new(const SyntaxNode *node);

// Hash a SyntaxNodePtr to u64. Suitable for use as a HashMap key when
// you need to map SyntaxNodePtrs to something (e.g., DefId, ScopeId,
// IpIndex). Folds (kind, start, length) via FxHash; collisions are
// astronomically unlikely within a single file.
uint64_t syntax_node_ptr_hash(SyntaxNodePtr ptr);

// RETURNS_OWNED. NULL if the path doesn't resolve in the new tree
// (e.g., the offending code was deleted).
SyntaxNode *syntax_node_ptr_resolve(SyntaxNodePtr ptr, SyntaxNode *root);

static inline bool syntax_node_ptr_eq(SyntaxNodePtr a, SyntaxNodePtr b) {
    return a.kind == b.kind && text_range_eq(a.range, b.range);
}


// =====================================================================
// Release-helper macro
// =====================================================================
//
// Releases the handle and nulls the local pointer. Catches use-after-
// release at the next deref instead of at the next double-free. Use
// at every release site:
//
//   SyntaxNode *child = syntax_node_first_child(parent);
//   // ... use child ...
//   SYN_RELEASE(child);  // child is now NULL
//
// Works for SyntaxNode, GreenNode, GreenToken. The macro is type-
// dispatched at compile time via _Generic.

#define SYN_RELEASE(p)                                                          \
    do {                                                                        \
        if (p) {                                                                \
            _Generic((p),                                                       \
                SyntaxNode *:  syntax_node_release,                             \
                SyntaxToken *: syntax_token_release,                            \
                GreenNode *:   green_node_release,                              \
                GreenToken *:  green_token_release)(p);                         \
            (p) = NULL;                                                         \
        }                                                                       \
    } while (0)

// Release a SyntaxElement (which is a runtime discriminated union).
#define SYN_ELEM_RELEASE(e)                                                     \
    do {                                                                        \
        if ((e).kind == SYNTAX_ELEM_NODE) {                                     \
            if ((e).node) { syntax_node_release((e).node); (e).node = NULL; }   \
        } else {                                                                \
            if ((e).token) { syntax_token_release((e).token); (e).token = NULL; } \
        }                                                                       \
    } while (0)


#endif // ORE_SYNTAX_H
