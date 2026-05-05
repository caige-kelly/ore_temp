#ifndef MODULE_LOADER_H
#define MODULE_LOADER_H

#include "../common/vec.h"
#include "../diag/diag.h"

struct Compiler;

struct ModuleReturn {
	Vec* ast;
	Vec* laid_out;
};

char* ore_read_file_to_string(const char* filepath, struct DiagBag* diags);
char* ore_canonical_path(const char* filepath);
char* ore_resolve_import_path(const char* importer_path, const char* import_path);
char* ore_resolve_import_path_in(Arena* scratch_arena, const char* importer_path,
								 const char* import_path);
struct ModuleReturn* ore_parse_file(struct Compiler* compiler, const char* filepath, int file_id);

#endif // MODULE_LOADER_H
