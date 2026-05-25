#include "syntax.h"
#include "node_cache.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

// GreenBuilder — parser-facing tree construction.
//
// State machine:
//   - parents[] is a stack of "open node" frames, each carrying the
//     SyntaxKind and the index in children[] where its first child sits.
//   - children[] is a flat Vec<GreenElement> holding all elements
//     emitted so far (both open and committed).
//
// start_node pushes a frame. finish_node pops the top frame, slices
// children[first..end], builds (or interns) a GreenNode from them,
// and replaces those slots in children[] with a single GreenElement
// pointing at the new node.
//
// Checkpoint captures the current children[] length. start_node_at(cp)
// pushes a frame whose first_child = cp — i.e., the new node will
// wrap everything emitted AFTER cp.
//
// Vec<GreenElement> backs the children stack; we don't use the support
// Vec because GreenElement is non-POD (union with rel_offset field).
// Inline impl is small and avoids a typed-Vec wrapper.

typedef struct {
    SyntaxKind kind;
    uint32_t   first_child;  // index in builder.children where this frame's children begin
} BuilderFrame;

struct GreenBuilder {
    NodeCache *cache;  // BORROWED

    BuilderFrame *parents;
    uint32_t      parents_count;
    uint32_t      parents_cap;

    GreenElement *children;
    uint32_t      children_count;
    uint32_t      children_cap;
};

static void grow_parents(GreenBuilder *b, uint32_t min_cap) {
    if (b->parents_cap >= min_cap) return;
    uint32_t new_cap = b->parents_cap ? b->parents_cap : 8;
    while (new_cap < min_cap) new_cap *= 2;
    b->parents = (BuilderFrame *)realloc(b->parents,
                                          (size_t)new_cap * sizeof(BuilderFrame));
    if (!b->parents) abort();
    b->parents_cap = new_cap;
}

static void grow_children(GreenBuilder *b, uint32_t min_cap) {
    if (b->children_cap >= min_cap) return;
    uint32_t new_cap = b->children_cap ? b->children_cap : 16;
    while (new_cap < min_cap) new_cap *= 2;
    b->children = (GreenElement *)realloc(b->children,
                                            (size_t)new_cap * sizeof(GreenElement));
    if (!b->children) abort();
    b->children_cap = new_cap;
}

GreenBuilder *green_builder_new(NodeCache *cache) {
    GreenBuilder *b = (GreenBuilder *)calloc(1, sizeof(GreenBuilder));
    if (!b) abort();
    b->cache = cache;
    return b;
}

void green_builder_destroy(GreenBuilder *b) {
    if (!b) return;
    // Release any orphaned children (should only happen if the caller
    // bailed out of a parse mid-build without finishing).
    for (uint32_t i = 0; i < b->children_count; i++) {
        GreenElement *e = &b->children[i];
        if (e->kind == GREEN_ELEM_NODE)
            green_node_release(e->node);
        else
            green_token_release(e->token);
    }
    free(b->parents);
    free(b->children);
    free(b);
}

void green_builder_start_node(GreenBuilder *b, SyntaxKind kind) {
    grow_parents(b, b->parents_count + 1);
    b->parents[b->parents_count++] = (BuilderFrame){
        .kind = kind,
        .first_child = b->children_count,
    };
}

void green_builder_token(GreenBuilder *b, SyntaxKind kind,
                          const char *text, uint32_t text_len) {
    GreenToken *t = node_cache_intern_token(b->cache, kind, text, text_len);
    grow_children(b, b->children_count + 1);
    b->children[b->children_count++] = (GreenElement){
        .kind = GREEN_ELEM_TOKEN,
        .rel_offset = 0,  // assigned by node_alloc when this becomes part of a node
        .token = t,
    };
}

void green_builder_finish_node(GreenBuilder *b) {
    assert(b->parents_count > 0 && "finish_node without matching start_node");
    BuilderFrame frame = b->parents[--b->parents_count];
    uint32_t first = frame.first_child;
    uint32_t count = b->children_count - first;

    GreenNode *node = node_cache_intern_node(b->cache, frame.kind,
                                              &b->children[first], count);

    // Replace the consumed slots with a single node element.
    b->children_count = first;
    grow_children(b, b->children_count + 1);
    b->children[b->children_count++] = (GreenElement){
        .kind = GREEN_ELEM_NODE,
        .rel_offset = 0,
        .node = node,
    };
}

Checkpoint green_builder_checkpoint(GreenBuilder *b) {
    return (Checkpoint)b->children_count;
}

void green_builder_start_node_at(GreenBuilder *b, Checkpoint cp,
                                  SyntaxKind kind) {
    assert(cp <= b->children_count && "checkpoint after end of children");
    grow_parents(b, b->parents_count + 1);
    b->parents[b->parents_count++] = (BuilderFrame){
        .kind = kind,
        .first_child = (uint32_t)cp,
    };
}

GreenNode *green_builder_finish(GreenBuilder *b) {
    assert(b->parents_count == 0 &&
           "green_builder_finish: builder has open nodes");
    assert(b->children_count == 1 &&
           "green_builder_finish: builder produced != 1 top-level node");
    assert(b->children[0].kind == GREEN_ELEM_NODE &&
           "green_builder_finish: top-level element must be a node");

    GreenNode *root = b->children[0].node;
    b->children_count = 0;
    return root;
}
