#include "query_engine.h"

#include <string.h>

#define HASH_SEED 0xcbf29ce484222325ULL
#define HASH_MUL 0x9E3779B97F4A7C15ULL  // odd; golden-ratio constant
#define HASH_NONE_REMAP 0x00000100000001b3ULL

// Word-at-a-time block hash. Replaced byte-wise FNV-1a: the durable
// fingerprint hashes tens of MB of AST arrays every parse, and FNV-1a's
// one-byte-per-iteration loop was ~14% of the query. This processes 8
// bytes/iteration + a strong final avalanche — same contract
// (deterministic, well-distributed; the absolute value is arbitrary and
// is NOT persisted across runs/architectures, so endianness is a
// non-issue for the in-memory Salsa early-cutoff stamp). Changing the
// algorithm intentionally rebaselines every fingerprint value.
static Fingerprint hash_block(const uint8_t *bytes, size_t len) {
  uint64_t h = HASH_SEED;
  size_t i = 0;
  for (; i + 8 <= len; i += 8) {
    uint64_t v;
    memcpy(&v, bytes + i, 8); // unaligned-safe; lowers to one load
    h ^= v;
    h *= HASH_MUL;
  }
  if (i < len) {
    uint64_t v = 0;
    memcpy(&v, bytes + i, len - i); // 1..7 trailing bytes, no over-read
    h ^= v;
    h *= HASH_MUL;
  }
  // Finalize (avalanche) — per-step mixing alone is weak; this gives
  // each input bit broad influence over the 64-bit result.
  h ^= h >> 32;
  h *= 0xD6E8FEB86659FD93ULL;
  h ^= h >> 32;
  return h == FINGERPRINT_NONE ? HASH_NONE_REMAP : h;
}

Fingerprint db_fp_pointer(const void *ptr) {
  uintptr_t v = (uintptr_t)ptr;
  return hash_block((const uint8_t *)&v, sizeof(v));
}

Fingerprint db_fp_bytes(const void *bytes, size_t len) {
  return hash_block((const uint8_t *)bytes, len);
}

Fingerprint db_fp_u64(uint64_t v) {
  return hash_block((const uint8_t *)&v, sizeof(v));
}

Fingerprint db_fp_combine(Fingerprint a, Fingerprint b) {
  uint64_t buf[2] = {a, b};
  return hash_block((const uint8_t *)buf, sizeof(buf));
}
