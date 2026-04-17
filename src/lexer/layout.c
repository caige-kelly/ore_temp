#include "./layout.h"
#include <stdlib.h> // Required for malloc

// Creates and initializes a new LayoutNormalizer.
struct LayoutNormalizer normalizer_new(Vec* tokens) {
    // The normalizer owns its output and frames vectors, so we allocate them here.
    Vec* output_vec = malloc(sizeof(Vec));
    vec_init(output_vec, sizeof(struct Token)); // This vector will store tokens.

    Vec* frames_vec = malloc(sizeof(Vec));
    vec_init(frames_vec, sizeof(struct LayoutFrame)); // This vector will store layout frames.

    struct LayoutNormalizer ln = {
        .tokens = tokens,                 // The input token stream.
        .current = 0,
        .output = output_vec,             // The output token stream.
        .frames = frames_vec,             // The stack of layout frames.
        .expecting_brace_body = false,
        .line_last_sig = Eof,             // A safe default, any non-significant token works.
        .current_line_indent = 0,
        .delimiter_depth = 0,
        .brace_frame_depths = 0,
        .at_line_start = true,
    };

    return ln;
}

// The main entry point for the layout normalization process.
Vec* layout_normalize_tokens(Vec* tokens) {
    struct LayoutNormalizer ln = normalizer_new(tokens);

    // Push the root frame onto the stack. This frame represents the top-level of the file.
    struct LayoutFrame root_frame = { .indent = 0, .kind = framekind_Root };
    vec_push(ln.frames, &root_frame);

    // The main processing loop will go here.
    // We will consume tokens from `ln.tokens` and push them to `ln.output`.
    // For now, we'll just fast-forward to the end.
    ln.current = ln.tokens->count -1; // Go to the EOF token

    // After the loop, we process the final EOF token.
    struct Token* eof_token = (struct Token*)vec_get(ln.tokens, ln.current);
    if (eof_token) {
        // In a real implementation, we would close any open layout blocks here.
        vec_push(ln.output, eof_token);
    }

    // The `frames` vector was owned by the normalizer, so we free it now.
    vec_free(ln.frames);
    free(ln.frames);

    // The `output` vector is being returned, so the caller is now responsible for it.
    return ln.output;
}
