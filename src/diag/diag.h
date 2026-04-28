#ifndef DIAG_H
#define DIAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "../common/arena.h"
#include "../common/vec.h"
#include "../lexer/token.h"
#include "sourcemap.h"

typedef enum {
    DIAG_ERROR,
    DIAG_WARNING,
    DIAG_NOTE,
} DiagSeverity;

struct Diag {
    DiagSeverity severity;
    struct Span span;
    char msg[512];
};

struct DiagBag {
    Arena* arena;
    Vec* diags;         // Vec of Diag
    size_t error_count;
    size_t warning_count;
};

struct DiagBag diag_bag_new(Arena* arena);
void diag_add(struct DiagBag* bag, DiagSeverity severity, struct Span span,
              const char* fmt, ...);
void diag_error(struct DiagBag* bag, struct Span span, const char* fmt, ...);
bool diag_has_errors(struct DiagBag* bag);
void diag_render(FILE* out, struct DiagBag* bag, struct SourceMap* source_map,
                 bool use_color);

#endif // DIAG_H
