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
    uint32_t node_id;
    uint32_t file_id;
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
void diag_error_path(struct DiagBag* bag, const char* path, const char* fmt, ...);
bool diag_has_errors(struct DiagBag* bag);

// Frame-aware emission. Routes to the currently executing query's slot
// so cached recomputes preserve their diagnostics across edits. Asserts
// that a query frame is active — there is NO fallback to the global bag.
// Callers that emit outside any query body (parser, LSP boundary, IO
// errors before query setup) must use `diag_error(&bag, ...)` directly
// against an explicit `DiagBag*`; that API has different (non-cached)
// semantics and is correct for those sites.
//
// Step 1 ships this with the slot-routing stubbed out (writes go to
// the global bag) so call-site migration is a no-op refactor; step 3
// flips the body to push into query_stack_top(s)->slot->diags.
struct Sema;  // forward decl — Sema lives in src/sema/sema.h, included by diag.c
void diag_emit(struct Sema* s, uint32_t node_id, const char* fmt, ...);
void diag_emit_severity(struct Sema* s, DiagSeverity severity,
                        struct Span span, const char* fmt, ...);

// Walk every slot's per-slot accumulator plus the sema-global bag
// (which now holds only parse-time / IO emissions), copying each
// Diag into `dest`. Output is sorted by (has_span, file_id, line,
// column, span.start) so the render order is stable across runs
// regardless of hashmap iteration order.
//
// If `file_id_filter >= 0`, only diagnostics whose span.file_id
// matches are collected; otherwise all are. `dest` must be
// pre-initialized via diag_bag_new(); entries are copied in.
void diag_collect_all(struct Sema* s, struct DiagBag* dest,
                      int file_id_filter);
void diag_render(FILE* out, struct DiagBag* bag, struct SourceMap* source_map,
                 bool use_color);

// Drop every accumulated diagnostic, resetting error/warning
// counts to zero. The backing arena is not reset — entries linger
// in memory until the arena is freed — but they're unreachable
// through `bag->diags` after this call. Used by the LSP server
// before each typecheck pass so push notifications carry only
// the current revision's diagnostics.
void diag_bag_clear(struct DiagBag* bag);

#endif // DIAG_H
