#ifndef ORE_SUPPORT_FXHASH_H
#define ORE_SUPPORT_FXHASH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// FxHash — small, fast, non-cryptographic hash used by Firefox and
// rust-analyzer/rowan. Not collision-resistant; intended for in-process
// hash-cons interners and HashMap key derivation where adversarial
// input is not a concern.
//
// Algorithm: multiplicative-shift over u64-sized chunks (Fx variant of
// FNV). Single-round per 8-byte chunk; ~1 cycle per byte amortized.
// Header-only — compiler inlines everything at call sites.
//
// Usage:
//   FxHasher h = fxhash_init();
//   fxhash_u64(&h, kind_and_count);
//   fxhash_bytes(&h, ptr, len);
//   uint64_t result = fxhash_finish(&h);

#define FXHASH_SEED 0xcbf29ce484222325ULL

typedef struct {
    uint64_t state;
} FxHasher;

static inline FxHasher fxhash_init(void) {
    return (FxHasher){.state = FXHASH_SEED};
}

static inline void fxhash_u64(FxHasher *h, uint64_t v) {
    // rotate-left 5, XOR, multiply — the FxHash mixing step.
    h->state = (h->state ^ v) * 0x517cc1b727220a95ULL;
}

static inline void fxhash_u32(FxHasher *h, uint32_t v) {
    fxhash_u64(h, (uint64_t)v);
}

static inline void fxhash_ptr(FxHasher *h, const void *p) {
    fxhash_u64(h, (uint64_t)(uintptr_t)p);
}

// Hash a byte buffer. Loads 8 bytes at a time on the hot path; falls
// back to byte-by-byte for the tail. Endianness-dependent (no swap) —
// FxHash is in-process only, never persisted, so endian neutrality
// doesn't matter.
static inline void fxhash_bytes(FxHasher *h, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    while (len >= 8) {
        uint64_t chunk;
        memcpy(&chunk, p, 8);
        fxhash_u64(h, chunk);
        p += 8;
        len -= 8;
    }
    if (len) {
        uint64_t tail = 0;
        memcpy(&tail, p, len);
        fxhash_u64(h, tail);
    }
}

static inline uint64_t fxhash_finish(const FxHasher *h) {
    return h->state;
}

#undef FXHASH_SEED

#endif // ORE_SUPPORT_FXHASH_H
