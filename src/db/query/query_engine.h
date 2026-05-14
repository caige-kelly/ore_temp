#ifndef ORE_DB_QUERY_ENGINE_H
#define ORE_DB_QUERY_ENGINE_H

#include <stdint.h>
#include <stddef.h>

#include "query.h"

// Fingerprint helpers — FNV-1a 64-bit. fnv1a maps a literal-zero hash
// to FNV_PRIME so a real fingerprint never collides with FINGERPRINT_NONE.
// Bodies build a result fingerprint with these helpers and pass it to
// db_query_succeed; there is no separate slot setter (folded into the
// succeed call to keep slot lookups to one per phase boundary).
Fingerprint db_fp_pointer(const void *p);
Fingerprint db_fp_bytes(const void *bytes, size_t len);
Fingerprint db_fp_u64(uint64_t v);
Fingerprint db_fp_combine(Fingerprint a, Fingerprint b);

#endif // ORE_DB_QUERY_ENGINE_H
