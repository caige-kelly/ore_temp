#include "sourcemap.h"

#include <stdlib.h>
#include <string.h>

static struct SourceFile* source_file_for_path(struct SourceMap* map, const char* path) {
    if (!map || !path || !map->files) return NULL;
    for (size_t i = 0; i < map->files->count; i++) {
        struct SourceFile** file_p = (struct SourceFile**)vec_get(map->files, i);
        struct SourceFile* file = file_p ? *file_p : NULL;
        const char* existing = file ? pool_get(map->pool, file->path_id, 0) : NULL;
        if (existing && strcmp(existing, path) == 0) return file;
    }
    return NULL;
}

struct SourceMap sourcemap_new(Arena* arena, StringPool* pool) {
    struct SourceMap map = {0};
    map.arena = arena;
    map.pool = pool;
    map.files = vec_new_in(arena, sizeof(struct SourceFile*));
    map.next_file_id = 0;
    return map;
}

struct SourceFile* sourcemap_add_file(struct SourceMap* map, int requested_file_id,
                                      const char* path, char* source) {
    if (!map || !path || !source) return NULL;

    struct SourceFile* existing = source_file_for_path(map, path);
    if (existing) {
        free(source);
        return existing;
    }

    struct SourceFile* file = arena_alloc(map->arena, sizeof(struct SourceFile));
    file->file_id = requested_file_id >= 0 ? requested_file_id : map->next_file_id;
    if (file->file_id >= map->next_file_id) map->next_file_id = file->file_id + 1;
    file->path_id = pool_intern(map->pool, path, strlen(path));
    file->source = source;
    file->source_len = strlen(source);
    file->line_starts = vec_new_in(map->arena, sizeof(size_t));

    size_t start = 0;
    vec_push(file->line_starts, &start);
    for (size_t i = 0; i < file->source_len; i++) {
        if (source[i] == '\n' && i + 1 < file->source_len) {
            size_t next = i + 1;
            vec_push(file->line_starts, &next);
        }
    }

    vec_push(map->files, &file);
    return file;
}

struct SourceFile* sourcemap_file_for_id(struct SourceMap* map, int file_id) {
    if (!map || !map->files) return NULL;
    for (size_t i = 0; i < map->files->count; i++) {
        struct SourceFile** file_p = (struct SourceFile**)vec_get(map->files, i);
        if (file_p && *file_p && (*file_p)->file_id == file_id) return *file_p;
    }
    return NULL;
}

const char* sourcemap_path(struct SourceMap* map, int file_id) {
    struct SourceFile* file = sourcemap_file_for_id(map, file_id);
    if (!file) return NULL;
    return pool_get(map->pool, file->path_id, 0);
}

const char* sourcemap_get_line(struct SourceMap* map, int file_id, int line,
                               size_t* line_len) {
    if (line_len) *line_len = 0;
    struct SourceFile* file = sourcemap_file_for_id(map, file_id);
    if (!file || line <= 0 || !file->line_starts) return NULL;
    size_t idx = (size_t)(line - 1);
    if (idx >= file->line_starts->count) return NULL;

    size_t* start_p = (size_t*)vec_get(file->line_starts, idx);
    if (!start_p) return NULL;
    size_t start = *start_p;
    size_t end = file->source_len;
    if (idx + 1 < file->line_starts->count) {
        size_t* next_p = (size_t*)vec_get(file->line_starts, idx + 1);
        if (next_p) end = *next_p;
    }
    while (end > start && (file->source[end - 1] == '\n' || file->source[end - 1] == '\r')) {
        end--;
    }
    if (line_len) *line_len = end - start;
    return file->source + start;
}

void sourcemap_free_sources(struct SourceMap* map) {
    if (!map || !map->files) return;
    for (size_t i = 0; i < map->files->count; i++) {
        struct SourceFile** file_p = (struct SourceFile**)vec_get(map->files, i);
        struct SourceFile* file = file_p ? *file_p : NULL;
        if (!file || !file->source) continue;
        free(file->source);
        file->source = NULL;
        file->source_len = 0;
    }
}
