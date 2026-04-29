#ifndef LAYOUT_H
#define LAYOUT_H

#include "./token.h"
#include "../common/vec.h"
#include "../common/stringpool.h"

enum LayoutFrameKind {
    framekind_Root,
    framekind_Explicit,
    framekind_Layout,
};

struct LayoutFrame {
    size_t indent;
    enum LayoutFrameKind kind;
};

struct LayoutNormalizer {
    Vec* tokens;
    size_t current;
    Vec* output;
    Vec* frames;
    bool expecting_brace_body;
    enum TokenKind line_last_sig;
    size_t current_line_indent;
    size_t delimiter_depth;
    size_t brace_frame_depths;
    bool at_line_start;
};

// Creates and initializes a new LayoutNormalizer.
struct LayoutNormalizer normalizer_new(Vec* tokens);
struct LayoutNormalizer normalizer_new_in(Vec* tokens, Arena* arena);

// The main entry point for the layout normalization process.
Vec* normalizer(Vec* tokens, StringPool* pool);
Vec* normalizer_in(Vec* tokens, StringPool* pool, Arena* arena);


#endif