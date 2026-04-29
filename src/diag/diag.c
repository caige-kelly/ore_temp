#include "diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct DiagBag diag_bag_new(Arena* arena) {
    struct DiagBag bag = {0};
    bag.arena = arena;
    bag.diags = vec_new_in(arena, sizeof(struct Diag));
    return bag;
}

static const char* severity_str(DiagSeverity severity) {
    switch (severity) {
        case DIAG_ERROR:   return "error";
        case DIAG_WARNING: return "warning";
        case DIAG_NOTE:    return "note";
    }
    return "diagnostic";
}

static const char* severity_color(DiagSeverity severity) {
    switch (severity) {
        case DIAG_ERROR:   return "\033[1;31m";
        case DIAG_WARNING: return "\033[1;33m";
        case DIAG_NOTE:    return "\033[1;34m";
    }
    return "";
}

static void diag_vadd(struct DiagBag* bag, DiagSeverity severity, struct Span span,
                      const char* fmt, va_list ap) {
    if (!bag || !bag->diags) return;
    struct Diag diag = {0};
    diag.severity = severity;
    diag.span = span;
    diag.has_span = span.line > 0 && span.column > 0;
    vsnprintf(diag.msg, sizeof(diag.msg), fmt, ap);
    vec_push(bag->diags, &diag);
    if (severity == DIAG_ERROR) bag->error_count++;
    if (severity == DIAG_WARNING) bag->warning_count++;
}

static void diag_vadd_path(struct DiagBag* bag, DiagSeverity severity, const char* path,
                           const char* fmt, va_list ap) {
    if (!bag || !bag->diags) return;
    struct Diag diag = {0};
    diag.severity = severity;
    if (path) snprintf(diag.path, sizeof(diag.path), "%s", path);
    vsnprintf(diag.msg, sizeof(diag.msg), fmt, ap);
    vec_push(bag->diags, &diag);
    if (severity == DIAG_ERROR) bag->error_count++;
    if (severity == DIAG_WARNING) bag->warning_count++;
}

void diag_add(struct DiagBag* bag, DiagSeverity severity, struct Span span,
              const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    diag_vadd(bag, severity, span, fmt, ap);
    va_end(ap);
}

void diag_error(struct DiagBag* bag, struct Span span, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    diag_vadd(bag, DIAG_ERROR, span, fmt, ap);
    va_end(ap);
}

void diag_error_path(struct DiagBag* bag, const char* path, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    diag_vadd_path(bag, DIAG_ERROR, path, fmt, ap);
    va_end(ap);
}

bool diag_has_errors(struct DiagBag* bag) {
    return bag && bag->error_count > 0;
}

static size_t underline_len_for_span(struct Span span, size_t line_len) {
    int raw_len = span.end - span.start;
    size_t len = raw_len > 0 ? (size_t)raw_len : 1;
    size_t col = span.column > 0 ? (size_t)span.column : 1;
    if (col > line_len) return 1;
    size_t remaining = line_len - col + 1;
    if (len > remaining) len = remaining;
    return len > 0 ? len : 1;
}

void diag_render(FILE* out, struct DiagBag* bag, struct SourceMap* source_map,
                 bool use_color) {
    if (!out || !bag || !bag->diags) return;

    for (size_t i = 0; i < bag->diags->count; i++) {
        struct Diag* diag = (struct Diag*)vec_get(bag->diags, i);
        if (!diag) continue;

        const char* path = diag->has_span
            ? sourcemap_path(source_map, diag->span.file_id)
            : (diag->path[0] ? diag->path : NULL);
        if (!path) path = "<unknown>";
        const char* sev = severity_str(diag->severity);
        const char* color = use_color ? severity_color(diag->severity) : "";
        const char* reset = use_color ? "\033[0m" : "";

        if (diag->has_span) {
            fprintf(out, "%s:%d:%d: %s%s%s: %s\n",
                path,
                diag->span.line,
                diag->span.column,
                color,
                sev,
                reset,
                diag->msg);
        } else {
            fprintf(out, "%s: %s%s%s: %s\n",
                path,
                color,
                sev,
                reset,
                diag->msg);
            continue;
        }

        size_t line_len = 0;
        const char* line = sourcemap_get_line(source_map, diag->span.file_id,
            diag->span.line, &line_len);
        if (!line) continue;

        fprintf(out, "  %4d | %.*s\n", diag->span.line, (int)line_len, line);
        fprintf(out, "       | ");
        int col = diag->span.column > 0 ? diag->span.column : 1;
        for (int c = 1; c < col; c++) fputc(' ', out);
        size_t underline_len = underline_len_for_span(diag->span, line_len);
        fputc('^', out);
        for (size_t u = 1; u < underline_len; u++) fputc('~', out);
        fputc('\n', out);
    }
}
