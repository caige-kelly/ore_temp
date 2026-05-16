#include "query_engine.h"

#define FNV_OFFSET 0xcbf29ce484222325ULL
#define FNV_PRIME 0x00000100000001b3ULL

static Fingerprint fnv1a(const uint8_t *bytes, size_t len) {
  uint64_t h = FNV_OFFSET;
  for (size_t i = 0; i < len; i++) {
    h ^= (uint64_t)bytes[i];
    h *= FNV_PRIME;
  }
  return h == FINGERPRINT_NONE ? FNV_PRIME : h;
}

Fingerprint db_fp_pointer(const void *ptr) {
  uintptr_t v = (uintptr_t)ptr;
  return fnv1a((const uint8_t *)&v, sizeof(v));
}

Fingerprint db_fp_bytes(const void *bytes, size_t len) {
  return fnv1a((const uint8_t *)bytes, len);
}

Fingerprint db_fp_u64(uint64_t v) {
  return fnv1a((const uint8_t *)&v, sizeof(v));
}

Fingerprint db_fp_combine(Fingerprint a, Fingerprint b) {
  uint64_t buf[2] = {a, b};
  return fnv1a((const uint8_t *)buf, sizeof(buf));
}
