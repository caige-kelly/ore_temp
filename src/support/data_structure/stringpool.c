#include "../../db/storage/stringpool.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

// =====================================================================
// StringPool — content-deduped string interner.
//
// Storage layout (one contiguous, growable malloc buffer `buffer`):
//   [u32 length][bytes...][\0]
//
//   The block's byte offset within `buffer` IS the StrId, so
//   offset→pointer is `buffer + id` — O(1). Two strings with identical
//   content share the same offset (and therefore the same StrId). The
//   trailing \0 lets pool_get return a C string. `buffer` grows via
//   realloc; no caller retains a pool_get pointer across an intern, so
//   a moving realloc is safe (see plan's pointer-stability audit).
//
// Slot table (heap-allocated, rebuilt on growth):
//   Open-addressed; linear probe. Each slot holds either POOL_EMPTY or
//   the byte offset of the stored block. Indexed by `hash & mask` where
//   `mask = slot_count - 1` (slot_count must be a power of two).
//
// Empty string convention: StrId{0} is reserved as the empty string.
// pool_init seeds the arena at offset 0 with an empty block so any
// non-empty intern allocates after it (offset 0 stays reserved).
// pool_intern("") fast-paths to StrId{0} without touching the slot
// table.
// =====================================================================

#define POOL_EMPTY 0xFFFFFFFFu

// First-chunk capacity for the contiguous string-bytes buffer. Grows
// by doubling; the initial value just avoids early reallocs.
#define STRINGPOOL_INITIAL_CAP (64u * 1024u)

// Ensure `buffer` has room for `need` more bytes. Doubling growth →
// amortized O(1) append. realloc may move the buffer; that is safe
// because no live pointer into `buffer` outlives this call (find_slot /
// slots_resize dereference and finish before any reserve; pool_get
// results are transient — audited).
static void pool_reserve(StringPool *p, size_t need) {
  if (p->len + need <= p->cap)
    return;
  size_t nc = p->cap ? p->cap : STRINGPOOL_INITIAL_CAP;
  while (nc < p->len + need)
    nc *= 2;
  p->buffer = realloc(p->buffer, nc);
  assert(p->buffer && "stringpool: buffer realloc failed");
  p->cap = nc;
}

// FNV-1a 32-bit. Cheap; adequate for short identifiers. Switch to
// xxh3 / wyhash if profiling shows hashing dominates.
static uint32_t hash_bytes(const char *s, size_t len) {
  uint32_t h = 0x811c9dc5u;
  for (size_t i = 0; i < len; i++) {
    h ^= (unsigned char)s[i];
    h *= 0x01000193u;
  }
  return h;
}

static const char *block_at(StringPool *pool, uint32_t id) {
  return pool->buffer + id; // O(1) — direct offset into the buffer.
}

// Locate the slot for (str, len, hash). On HIT (string already present),
// returns the slot index with *out_id set to the existing StrId byte
// offset. On MISS (string not present), returns the insertion slot
// index with *out_id == POOL_EMPTY.
//
// Linear probe with cyclic wrap. The slot table must always have at
// least one empty slot (enforced by the load-factor check in
// pool_intern), so this loop is guaranteed to terminate.
static size_t find_slot(StringPool *pool, const char *str, size_t len,
                        uint32_t hash, uint32_t *out_id) {
  size_t mask = pool->slot_count - 1;
  size_t i = (size_t)hash & mask;

  for (;;) {
    uint32_t id = pool->slots[i];
    if (id == POOL_EMPTY) {
      *out_id = POOL_EMPTY;
      return i;
    }

    const char *entry = block_at(pool, id);
    // Length prefix is always 4-byte-aligned (block_at returns
    // buffer + offset; offsets are bumped by 4+len+1 bytes, see
    // pool_intern). Direct load is safe and lowers to one ldr on
    // ARM64; the memcpy form was preventing the compiler from
    // hoisting the read out of the probe loop.
    uint32_t existing_len = *(const uint32_t *)entry;

    if (existing_len == (uint32_t)len) {
      const char *existing_str = entry + sizeof(uint32_t);
      if (memcmp(existing_str, str, len) == 0) {
        *out_id = id;
        return i;
      }
    }

    i = (i + 1) & mask;
  }
}

// Resize the slot table to `new_count` (power of two) and rehash every
// existing entry. Called when the load factor would exceed ~70%.
static void slots_resize(StringPool *pool, size_t new_count) {
  assert(new_count >= 16 && (new_count & (new_count - 1)) == 0 &&
         "stringpool: slot_count must be a power of two >= 16");

  uint32_t *old_slots = pool->slots;
  size_t old_count = pool->slot_count;

  pool->slots = malloc(new_count * sizeof(uint32_t));
  memset(pool->slots, 0xFF, new_count * sizeof(uint32_t));
  pool->slot_count = new_count;
  // slot_used count carries over — we're rehashing identical entries.

  size_t mask = new_count - 1;
  for (size_t i = 0; i < old_count; i++) {
    uint32_t id = old_slots[i];
    if (id == POOL_EMPTY)
      continue;

    const char *entry = block_at(pool, id);
    uint32_t len;
    memcpy(&len, entry, sizeof(uint32_t));
    const char *str = entry + sizeof(uint32_t);

    uint32_t h = hash_bytes(str, len);
    size_t j = (size_t)h & mask;
    while (pool->slots[j] != POOL_EMPTY)
      j = (j + 1) & mask;
    pool->slots[j] = id;
  }

  free(old_slots);
}

// Pre-grow the slot table so an upcoming batch of ~expected_more interns
// won't trip the incremental doubling/rehash storm (each doubling
// rehashes every existing entry). Sizes to the smallest power of two
// that keeps the post-batch load factor under ~70% — matching
// pool_intern's resize threshold ((used+1)*10 > count*7). No-op if the
// table is already large enough. Capacity-only: StrIds and intern
// behavior are unchanged.
void pool_reserve_slots(StringPool *pool, size_t expected_more) {
  size_t needed = pool->slot_used + expected_more;
  size_t target = 16;
  while (target * 7 < needed * 10)
    target *= 2;
  if (target > pool->slot_count)
    slots_resize(pool, target);
}

void pool_init(StringPool *pool, size_t initial_slots) {
  assert(initial_slots >= 16 && (initial_slots & (initial_slots - 1)) == 0 &&
         "pool_init: initial_slots must be a power of two >= 16");

  *pool = (StringPool){0};

  pool->slots = malloc(initial_slots * sizeof(uint32_t));
  memset(pool->slots, 0xFF, initial_slots * sizeof(uint32_t));
  pool->slot_count = initial_slots;
  pool->slot_used = 0;

  pool->buffer = malloc(STRINGPOOL_INITIAL_CAP);
  assert(pool->buffer && "pool_init: buffer malloc failed");
  pool->cap = STRINGPOOL_INITIAL_CAP;
  pool->len = 0;

  // Seed offset 0 with the empty-string block so any subsequent intern
  // allocates at offset >= the seeded block's size (offset 0 stays
  // reserved as StrId{0}). NOT registered in the slot table; empty
  // string fast-paths in pool_intern / pool_get.
  uint32_t zero_len = 0;
  memcpy(pool->buffer, &zero_len, sizeof(uint32_t));
  pool->buffer[sizeof(uint32_t)] = '\0';
  pool->len = sizeof(uint32_t) + 1;
}

void pool_free(StringPool *pool) {
  free(pool->slots);
  free(pool->buffer);
  memset(pool, 0, sizeof(StringPool));
}

StrId pool_intern(StringPool *pool, const char *str, size_t len) {
  // Empty-string fast path — StrId{0} by convention.
  if (len == 0)
    return (StrId){.idx = 0};

  // Load-factor check (~70%). Resize before insertion so we never
  // probe into a stretched table.
  if ((pool->slot_used + 1) * 10 > pool->slot_count * 7) {
    slots_resize(pool, pool->slot_count * 2);
  }

  uint32_t h = hash_bytes(str, len);
  uint32_t existing_id;
  size_t slot = find_slot(pool, str, len, h, &existing_id);

  if (existing_id != POOL_EMPTY) {
    return (StrId){.idx = existing_id};
  }

  // Insert: append [u32 len][bytes][\0] to the contiguous buffer. The
  // block's byte offset is the StrId. `offset` is captured before the
  // reserve (reserve changes buffer/cap, never len), so a moving
  // realloc is fine.
  uint32_t total = (uint32_t)len + sizeof(uint32_t) + 1;
  uint32_t offset = (uint32_t)pool->len;
  pool_reserve(pool, total);

  uint32_t u32_len = (uint32_t)len;
  char *ptr = pool->buffer + offset;
  memcpy(ptr, &u32_len, sizeof(uint32_t));
  char *dest = ptr + sizeof(uint32_t);
  memcpy(dest, str, len);
  dest[len] = '\0';
  pool->len += total;

  pool->slots[slot] = offset;
  pool->slot_used++;

  return (StrId){.idx = offset};
}

const char *pool_get(StringPool *pool, StrId id) {
  if (id.idx == 0)
    return ""; // Empty-string fast path.

  const char *block = block_at(pool, id.idx);
  return block + sizeof(uint32_t);
}
