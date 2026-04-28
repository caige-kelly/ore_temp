#ifndef SOURCEMAP_H
#define SOURCEMAP_H

#include <stddef.h>
#include <stdint.h>

#include "../common/arena.h"
#include "../common/stringpool.h"
#include "../common/vec.h"

struct SourceFile {
    int file_id;
    uint32_t path_id;
    char* source;       // heap-owned by the SourceMap until process teardown
    size_t source_len;
    Vec* line_starts;   // Vec of size_t byte offsets, 0-based
};

struct SourceMap {
    Arena* arena;
    StringPool* pool;
    Vec* files;         // Vec of SourceFile*
    int next_file_id;
};

struct SourceMap sourcemap_new(Arena* arena, StringPool* pool);
struct SourceFile* sourcemap_add_file(struct SourceMap* map, int requested_file_id,
                                      const char* path, char* source);
struct SourceFile* sourcemap_file_for_id(struct SourceMap* map, int file_id);
const char* sourcemap_path(struct SourceMap* map, int file_id);
const char* sourcemap_get_line(struct SourceMap* map, int file_id, int line,
                               size_t* line_len);
void sourcemap_free_sources(struct SourceMap* map);

#endif // SOURCEMAP_H
