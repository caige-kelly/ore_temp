#include "query_engine.h"

#include "../../common/vec.h"
#include "../sema.h"

// FNV-1a 64-bit. Chosen over xxHash/SipHash for simplicity and zero
// dependencies; we're hashing tiny inputs (pointers, small structs)
// where any of these is fine. If profiling later shows fingerprint
// hashing as a hot path, swap the impl — the API is unchanged.
//
// Avoids a fingerprint of literal 0 by mapping it to a tagged constant,
// since FINGERPRINT_NONE == 0 means "no fingerprint set" elsewhere.

#define FNV_OFFSET 0xcbf29ce484222325ULL
#define FNV_PRIME  0x00000100000001b3ULL

static Fingerprint fnv1a(const uint8_t *bytes, size_t len) {
  uint64_t h = FNV_OFFSET;
  for (size_t i = 0; i < len; i++) {
    h ^= (uint64_t)bytes[i];
    h *= FNV_PRIME;
  }
  return h == FINGERPRINT_NONE ? FNV_PRIME : h;
}

Fingerprint query_fingerprint_from_pointer(const void *ptr) {
  uintptr_t v = (uintptr_t)ptr;
  return fnv1a((const uint8_t *)&v, sizeof(v));
}

Fingerprint query_fingerprint_from_bytes(const void *data, size_t size) {
  return fnv1a((const uint8_t *)data, size);
}

Fingerprint query_fingerprint_from_u64(uint64_t v) {
  return fnv1a((const uint8_t *)&v, sizeof(v));
}

Fingerprint query_fingerprint_combine(Fingerprint a, Fingerprint b) {
  uint64_t buf[2] = {a, b};
  return fnv1a((const uint8_t *)buf, sizeof(buf));
}

void query_slot_set_fingerprint(struct QuerySlot *slot, Fingerprint fp) {
  slot->fingerprint = fp;
}

void sema_bump_revision(struct Sema *s) { s->current_revision++; }

uint64_t sema_current_revision(struct Sema *s) { return s->current_revision; }

void query_slot_visit_deps(struct QuerySlot *slot, QueryDepVisitor visit,
                           void *user_data) {
  if (!slot->deps)
    return;
  for (size_t i = 0; i < slot->deps->count; i++) {
    struct QueryDep *dep = (struct QueryDep *)vec_get(slot->deps, i);
    if (!visit(*dep, user_data))
      return;
  }
}

size_t query_slot_dep_count(struct QuerySlot *slot) {
  return slot->deps ? slot->deps->count : 0;
}
