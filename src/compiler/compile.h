#ifndef ORE_COMPILER_COMPILE_H
#define ORE_COMPILER_COMPILE_H

// One canonical "compile a file" pipeline shared by every consumer
// (CLI driver, LSP server, tests, future REPL / playground).
//
// History: pre-2026-05-23 the driver and the LSP each had their own
// pipeline. They drifted — the driver opened two requests with a
// manual ORE_PROFILE_LOOP slot-reset between them (missing
// db_diags_clear, so iterations accumulated stale diags), while the
// LSP routed through oredb_typecheck (one request, no profile loop).
// The same bug could surface in one and not the other. This module is
// the unified entry point both consumers go through.
//
// Layering:
//
//   workspace_did_open / workspace_did_change_*  ← I/O boundary; the
//     caller is responsible for getting source bytes into the db
//     (driver: slurp_file; LSP: receives text from editor).
//
//   compile_file (THIS module)                    ← pure: parse + sema
//     + diag collection. No disk I/O, no protocol concerns. Opens its
//     own request boundary.
//
//   Driver / LSP / IDE service                    ← thin protocol
//     translators. Each parses its own args/messages, calls
//     compile_file, formats output.
//
// Idempotent: salsa slots short-circuit on unchanged input via the
// hash-compare fast-path in db_set_source_text. Revalidation is driven
// by revision bumps from input setters (db_create_source,
// db_set_source_text, db_create_file, workspace_did_evict_source).

#include "../db/ids/ids.h"
#include "../db/storage/vec.h"

#include <stdbool.h>
#include <stdint.h>

struct db;

// Options for compile_file. Zero-init means "defaults" — single
// typecheck pass. NULL `opts` is also a valid input.
//
// Debug dumps (ast_dump_module, sema_dump_module) are NOT exposed
// here: they're consumer-specific concerns (driver-only today). The
// caller invokes them in its own request boundary AFTER compile_file
// returns — the dumps cache-hit cheaply on the queries compile_file
// just populated.
typedef struct {
    // Profile loop iteration count. >= 1; 1 = no profile loop.
    // Driver sets this from ORE_PROFILE_LOOP for perf sampling. The
    // LSP always passes 1. When > 1, each iteration stales
    // QUERY_FILE_AST and clears its diags before re-parsing — closes
    // the diag-accumulation leak the driver had pre-2026-05-23.
    int  profile_count;
} CompileFileOpts;

// Compile a file that's already been registered via workspace_did_open
// (or a sibling admit function).
//
// Pipeline:
//   1. profile_count > 1 (optional): re-parse N times for perf
//      sampling; each iteration runs in its own request boundary,
//      and EXPLICITLY clears file_ast diags between iterations.
//   2. Main request: sema_check_module (scopes + DefIds + typecheck
//      + infer) → db_collect_diags_for_file.
//
// Parameters:
//   db          Ore database. Queries may populate slots.
//   src         SourceId of the file to compile (must be valid and
//               registered). Returns FILE_ID_NONE if invalid.
//   opts        Compile options (NULL means defaults).
//   out_diags   Caller-initialized Vec (vec_init or arena-backed),
//               populated with shallow-copy Diag entries. The Diag.args
//               pointers reference the diag unit's arena — caller must
//               consume before the next recompute that would reset it.
//
// Returns the FileId of the compiled file, or FILE_ID_NONE if src
// isn't a valid registered source.
FileId compile_file(struct db *db, SourceId src,
                    const CompileFileOpts *opts, Vec *out_diags);

#endif // ORE_COMPILER_COMPILE_H
