#ifndef MODULE_LOADER_H
#define MODULE_LOADER_H

#include "../common/stringpool.h"
#include "../common/vec.h"
#include "../diag/diag.h"

struct Compiler;

struct ModuleReturn {
	Vec* ast;
	Vec* laid_out;
	Vec* tokens;
};

char* ore_read_file_to_string(const char* filepath, struct DiagBag* diags);
char* ore_canonical_path(const char* filepath);
char* ore_resolve_import_path(const char* importer_path, const char* import_path);
char* ore_resolve_import_path_in(Arena* scratch_arena, const char* importer_path,
								 const char* import_path);
struct ModuleReturn* ore_parse_file(struct Compiler* compiler, const char* filepath, int file_id);

// Pure parsing pipeline — lex + layout + parse with no file I/O and
// no compiler-options-driven side effects (no dump_raw / dump_lex).
//
// Used by sema's `query_module_ast` to parse on demand from
// in-memory source text. The legacy `ore_parse_file` becomes a thin
// wrapper that reads the file then calls this.
//
// Arenas:
//   ast_arena      — where the AST nodes live (caller's long-term
//                    allocator; sema passes its `arena`).
//   scratch_arena  — temporary storage for tokens and layout
//                    vectors; safe to reset/free after the call
//                    completes.
//
// `source` must be null-terminated; `source_len` is its byte length
// for fingerprint convenience (lexer reads to NUL).
//
// Returns NULL on a hard structural failure (allocator / null inputs).
// Parser-level diagnostics go to `diags` and the AST is still
// returned so consumers can recover.
Vec* parse_source(Arena* ast_arena, Arena* scratch_arena, StringPool* pool,
				  struct DiagBag* diags, int file_id, const char* source,
				  size_t source_len);

#endif // MODULE_LOADER_H
