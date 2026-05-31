#ifndef ORE_SUPPORT_FS_H
#define ORE_SUPPORT_FS_H

#include <stddef.h>

// Read a whole file into a malloc'd, NUL-terminated buffer.
// Returns NULL on any failure (open, seek, alloc, short read).
// out_len (may be NULL) receives the byte length excluding the NUL.
// Silent on failure — callers wrap with a diag if they want one.
//
// L5: consolidated from two near-identical copies that used to live in
// src/consumers/driver/build.c and src/db/workspace/workspace.c. The
// driver version printed to stderr on open failure; that's now the
// caller's responsibility (a one-liner around the returned NULL).
char *fs_slurp_file(const char *path, size_t *out_len);

#endif // ORE_SUPPORT_FS_H
