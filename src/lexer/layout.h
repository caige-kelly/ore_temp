#ifndef LAYOUT_H
#define LAYOUT_H

#include "./token.h"
#include "../common/vec.h"
#include "../common/stringpool.h"
#include "../diag/diag.h"

enum LayoutFrameKind {
    framekind_Root,
    framekind_Explicit,
    framekind_Layout,
};

struct LayoutFrame {
    size_t indent;
    enum LayoutFrameKind kind;
};

// The main entry point for the layout normalization pipeline.
// Mirrors Koka's `layout` (Syntax/Layout.hs) — runs check_comments,
// remove_whitespace, remove_comments, and indent_layout in order.
Vec* normalizer_in(Vec* tokens, StringPool* pool, Arena* arena,
                   struct DiagBag* diags);

#endif
