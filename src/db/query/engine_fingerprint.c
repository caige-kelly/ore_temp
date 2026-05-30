// Fingerprint helpers — FNV-1a 64-bit.
//
// All three functions guarantee a non-zero output. FINGERPRINT_NONE (== 0)
// is reserved as the "no fingerprint" sentinel; a real fingerprint must
// never collide with it.
//
// FNV-1a chosen over xxhash/wyhash for code simplicity. The fingerprint
// is computed on cold paths (a query body's succeed); throughput isn't
// the bottleneck. Cryptographic strength isn't needed — we only need
// "different inputs → different fingerprints with overwhelming
// probability."

#include "engine.h"

#include <stddef.h>
#include <stdint.h>

static const Fingerprint FNV_OFFSET_64 = 0xcbf29ce484222325ULL;
static const Fingerprint FNV_PRIME_64 = 0x00000100000001b3ULL;

// If a computation lands on FINGERPRINT_NONE, bump to a canonical
// non-zero. Adding FNV_PRIME_64 (not multiplying — that could land
// back on zero for unfortunate inputs) is the simplest remap that
// preserves "different inputs → different outputs" in the rare
// collision-with-zero case.
static inline Fingerprint nonzero(Fingerprint h) {
  return h ? h : FNV_PRIME_64;
}

// Fold n bytes of `p` into the accumulator `seed` (FNV-1a inner loop).
static inline Fingerprint fold_bytes(Fingerprint seed, const uint8_t *p,
                                     size_t n) {
  Fingerprint h = seed;
  for (size_t i = 0; i < n; i++) {
    h ^= (Fingerprint)p[i];
    h *= FNV_PRIME_64;
  }
  return h;
}

Fingerprint db_fp_u64(uint64_t x) {
  return nonzero(fold_bytes(FNV_OFFSET_64, (const uint8_t *)&x, sizeof x));
}

Fingerprint db_fp_combine(Fingerprint a, Fingerprint b) {
  // Treat `a` as the seed and fold b's 8 bytes into it. The result
  // depends on order: combine(a, b) != combine(b, a). Callers fold
  // their query's inputs in a fixed order so reruns produce the same
  // fingerprint.
  return nonzero(fold_bytes(a, (const uint8_t *)&b, sizeof b));
}

Fingerprint db_fp_bytes(const void *p, size_t n) {
  return nonzero(fold_bytes(FNV_OFFSET_64, (const uint8_t *)p, n));
}
