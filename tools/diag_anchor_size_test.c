// Phase P P7.1.1 — Compile-time guards for the new diag type sizes.
//
// These static_asserts lock down the post-Phase-P layout choices:
//
//   DiagAnchor       = 16 B   (tagged union; FILE/BODY/FILE_RAW variants)
//   _Alignof(...)    = 4      (no uint64_t fields anywhere; uint32_t DeclKey
//                              is what keeps this at 4 — see plan P1.c)
//   DiagArg          = 20 B   (1 + 3 + 16 with DiagArgKind = enum : uint8_t)
//   Diag             = 40 B   (anchor 16 + template_id 4 + 4 pad + args 8 +
//                              code/n_args/severity/tag/owner_kind + 2 pad)
//   DiagBundleRef    = 8 B    (col 2 + pad 2 + key 4)
//
// This test is the N1 guard against a future refactor accidentally
// promoting DeclKey back to uint64_t — that would force DiagAnchor to
// 24 B and DiagArg to 32 B, breaking the cache-line economy.
//
// Keep-zone, no linking against the rest of the codebase needed since
// these are pure size invariants. We just include diag.h and let the
// static_asserts there speak for themselves; if any of them fail at
// compile time the build breaks here.

#include "../src/db/diag/diag.h"

#include <stdio.h>

int main(void) {
    // The static_asserts in diag.h are what actually gate the sizes —
    // if any fail, this file won't compile. Reaching main() means they
    // all passed; we print the sizes for observability.
    printf("PASS diag_anchor_size: DiagAnchor=%zu align=%zu  DiagArg=%zu  "
           "Diag=%zu  DiagBundleRef=%zu\n",
           sizeof(DiagAnchor), _Alignof(DiagAnchor), sizeof(DiagArg),
           sizeof(Diag), sizeof(DiagBundleRef));
    return 0;
}
