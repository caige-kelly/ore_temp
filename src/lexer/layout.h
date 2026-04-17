#ifndef LAYOUT_H
#define LAYOUT_H

#include <stddef.h>
#include <stdbool.h>
#include "./token.h"
#include "./lexer.h"
#include "../common/vec.h"

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

// The main entry point for the layout normalization process.
Vec* layout_normalize_tokens(Vec* tokens);


#endif