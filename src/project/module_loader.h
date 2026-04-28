#ifndef MODULE_LOADER_H
#define MODULE_LOADER_H

#include "../common/arena.h"
#include "../common/stringpool.h"
#include "../common/vec.h"
#include "../diag/diag.h"
#include "../diag/sourcemap.h"

char* ore_read_file_to_string(const char* filepath);
char* ore_canonical_path(const char* filepath);
char* ore_resolve_import_path(const char* importer_path, const char* import_path);
Vec* ore_parse_file(const char* filepath, StringPool* pool, Arena* arena, int file_id,
					struct SourceMap* source_map, struct DiagBag* diags);

#endif // MODULE_LOADER_H
