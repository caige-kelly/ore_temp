// Module readers — accessors for the modules-table SoA.

#include "../db.h"

// Borrow a module's file slice: returns the FileId* base and writes the
// count. Pointer is valid until the next file_pool mutation.
const FileId *db_get_module_files(struct db *s, ModuleId mid,
                                  uint32_t *out_count) {
  if (!module_id_valid(mid) || mid.idx + 1 >= s->modules.file_offsets.count) {
    *out_count = 0;
    return NULL;
  }
  uint32_t *off = (uint32_t *)s->modules.file_offsets.data;
  uint32_t start = off[mid.idx];
  uint32_t end = off[mid.idx + 1];
  *out_count = end - start;
  return (const FileId *)s->modules.file_pool.data + start;
}
