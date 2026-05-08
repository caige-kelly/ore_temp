#ifndef ORE_PARSER_PARSE_SOURCE_H
#define ORE_PARSER_PARSE_SOURCE_H

#include "../common/arena.h"
#include "../common/stringpool.h"
#include "../common/vec.h"

struct DiagBag;

// Pure parsing pipeline — lex + layout + parse with no file I/O.
//
// Used by sema's `query_module_ast` to parse on demand from
// in-memory source text. Returns NULL on hard structural failure
// (allocator / null inputs); parser-level diagnostics go to `diags`
// and the AST is still returned so consumers can recover.
//
// Arenas:
//   ast_arena      — where the AST nodes live (caller's long-term
//                    allocator; sema passes its `arena`).
//   scratch_arena  — temporary storage for tokens and layout
//                    vectors; safe to reset/free after the call.
//
// `source` must be null-terminated; `source_len` is its byte length
// for fingerprint convenience (lexer reads to NUL).
Vec* parse_source(Arena* ast_arena, Arena* scratch_arena, StringPool* pool,
                  struct DiagBag* diags, int file_id, const char* source,
                  size_t source_len);

#endif
