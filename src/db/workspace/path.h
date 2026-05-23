#ifndef ORE_DB_WORKSPACE_PATH_H
#define ORE_DB_WORKSPACE_PATH_H

#include <stddef.h>

#define ORE_PATH_MAX 4096

// Lexical path normalization — collapse ./ and ../ components, drop
// trailing slashes, no I/O. If `rel` is absolute (leading '/'), `dir`
// is ignored.
//
// Writes the result into `out` (up to out_cap-1 bytes plus NUL).
// Returns the number of bytes written excluding NUL, or 0 on failure
// (overflow or invalid input).
//
// Pure lexical normalization is NOT sufficient for cross-platform
// file identity (case-insensitive FS + symlinks). Use realpath() on
// top of this when canonical identity matters.
size_t path_normalize(const char *dir, size_t dir_len,
                      const char *rel, size_t rel_len,
                      char *out, size_t out_cap);

// dirname length: everything up to (but not including) the last '/'.
// Returns 0 if there's no slash (caller treats as "").
size_t path_dirname_len(const char *path, size_t path_len);

#endif // ORE_DB_WORKSPACE_PATH_H
