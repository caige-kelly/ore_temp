#include "syntax.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

// =====================================================================
// SyntaxText — lazy text view over a subtree.
//
// Implementation: iterates the subtree's tokens via SyntaxPreorder
// (Enter events on tokens), clips each token's text against the
// SyntaxText's range, and invokes the visitor with the clipped chunk.
//
// All ops are O(tokens-in-subtree) worst case. No allocation; no
// refcounting on the SyntaxText itself.
// =====================================================================

SyntaxText syntax_text_of(SyntaxNode *n) {
    return (SyntaxText){.node = n, .range = syntax_node_text_range(n)};
}

SyntaxText syntax_text_slice(const SyntaxText *src, TextRange range) {
    assert(text_range_contains(src->range, range) &&
           "syntax_text_slice: range outside source");
    return (SyntaxText){.node = src->node, .range = range};
}

uint32_t syntax_text_len(const SyntaxText *st) { return st->range.length; }
bool     syntax_text_is_empty(const SyntaxText *st) { return st->range.length == 0; }


// Internal: iterate every leaf token in the subtree (via preorder),
// clipping each against `st->range`, calling `visitor` with the
// clipped slice. Returns false if visitor returned false (early exit).
bool syntax_text_for_each_chunk(const SyntaxText *st,
                                 SyntaxTextChunkVisitor visitor, void *user) {
    if (st->range.length == 0) return true;

    uint32_t window_start = st->range.start;
    uint32_t window_end   = text_range_end(st->range);

    SyntaxPreorder po;
    syntax_preorder_init(&po, st->node);
    bool ok = true;

    for (;;) {
        SyntaxWalkEvent ev = syntax_preorder_next(&po);
        if (syntax_walk_event_is_none(ev)) break;
        if (ev.kind != SYNTAX_WALK_ENTER) {
            SYN_ELEM_RELEASE(ev.element);
            continue;
        }
        if (ev.element.kind != SYNTAX_ELEM_TOKEN) {
            SYN_ELEM_RELEASE(ev.element);
            continue;
        }

        SyntaxToken *t = ev.element.token;
        TextRange tr = syntax_token_text_range(t);
        uint32_t t_start = tr.start;
        uint32_t t_end   = t_start + tr.length;

        // Skip tokens entirely outside the window.
        if (t_end <= window_start || t_start >= window_end) {
            SYN_ELEM_RELEASE(ev.element);
            continue;
        }

        // Compute clipped range within this token.
        const char *text = syntax_token_text(t);
        uint32_t clip_lo = (t_start < window_start) ? (window_start - t_start) : 0;
        uint32_t clip_hi = (t_end > window_end) ? (tr.length - (t_end - window_end))
                                                : tr.length;
        uint32_t clip_len = clip_hi - clip_lo;

        if (clip_len > 0) {
            if (!visitor(text + clip_lo, clip_len, user)) {
                ok = false;
                SYN_ELEM_RELEASE(ev.element);
                break;
            }
        }

        SYN_ELEM_RELEASE(ev.element);
    }
    syntax_preorder_free(&po);
    return ok;
}


// ---- to_cstr ------------------------------------------------------

typedef struct {
    char *buf;
    size_t cap;       // total buffer capacity (bufcap)
    size_t pos;       // bytes written so far (may exceed cap-1 to signal trunc)
} ToCstrCtx;

static bool to_cstr_visit(const char *text, uint32_t len, void *user) {
    ToCstrCtx *ctx = (ToCstrCtx *)user;
    if (ctx->cap > 0) {
        size_t space = ctx->cap - 1;            // reserve 1 for NUL
        size_t pos = ctx->pos < space ? ctx->pos : space;
        size_t to_copy = (pos + len <= space) ? len : (space - pos);
        if (to_copy > 0 && ctx->buf)
            memcpy(ctx->buf + pos, text, to_copy);
    }
    ctx->pos += len;
    return true;
}

size_t syntax_text_to_cstr(const SyntaxText *st, char *buf, size_t bufcap) {
    ToCstrCtx ctx = {.buf = buf, .cap = bufcap, .pos = 0};
    syntax_text_for_each_chunk(st, to_cstr_visit, &ctx);
    if (buf && bufcap > 0) {
        size_t pos = ctx.pos < bufcap - 1 ? ctx.pos : bufcap - 1;
        buf[pos] = '\0';
    }
    return ctx.pos;
}


// ---- byte_at ------------------------------------------------------

typedef struct {
    uint32_t target;   // absolute offset we want
    uint32_t cursor;   // current absolute offset (updated per chunk)
    int      result;   // -1 = not found
} ByteAtCtx;

static bool byte_at_visit(const char *text, uint32_t len, void *user) {
    ByteAtCtx *ctx = (ByteAtCtx *)user;
    uint32_t chunk_end = ctx->cursor + len;
    if (ctx->target >= ctx->cursor && ctx->target < chunk_end) {
        ctx->result = (unsigned char)text[ctx->target - ctx->cursor];
        return false;  // early exit
    }
    ctx->cursor = chunk_end;
    return true;
}

int syntax_text_byte_at(const SyntaxText *st, uint32_t offset) {
    if (offset < st->range.start) return -1;
    if (offset >= text_range_end(st->range)) return -1;
    ByteAtCtx ctx = {.target = offset, .cursor = st->range.start, .result = -1};
    syntax_text_for_each_chunk(st, byte_at_visit, &ctx);
    return ctx.result;
}


// ---- contains_byte / find_byte ------------------------------------

typedef struct {
    char     needle;
    uint32_t cursor;   // current absolute offset
    uint32_t found_at; // UINT32_MAX = miss
} FindByteCtx;

static bool find_byte_visit(const char *text, uint32_t len, void *user) {
    FindByteCtx *ctx = (FindByteCtx *)user;
    for (uint32_t i = 0; i < len; i++) {
        if (text[i] == ctx->needle) {
            ctx->found_at = ctx->cursor + i;
            return false;
        }
    }
    ctx->cursor += len;
    return true;
}

uint32_t syntax_text_find_byte(const SyntaxText *st, char c) {
    FindByteCtx ctx = {.needle = c, .cursor = st->range.start,
                       .found_at = UINT32_MAX};
    syntax_text_for_each_chunk(st, find_byte_visit, &ctx);
    return ctx.found_at;
}

bool syntax_text_contains_byte(const SyntaxText *st, char c) {
    return syntax_text_find_byte(st, c) != UINT32_MAX;
}


// ---- eq_cstr ------------------------------------------------------

typedef struct {
    const char *p;        // current position in the cstr we're comparing
    size_t      remaining;
    bool        mismatch;
} EqCstrCtx;

static bool eq_cstr_visit(const char *text, uint32_t len, void *user) {
    EqCstrCtx *ctx = (EqCstrCtx *)user;
    if (len > ctx->remaining) { ctx->mismatch = true; return false; }
    if (memcmp(text, ctx->p, len) != 0) { ctx->mismatch = true; return false; }
    ctx->p += len;
    ctx->remaining -= len;
    return true;
}

bool syntax_text_eq_cstr(const SyntaxText *st, const char *cstr) {
    size_t want = strlen(cstr);
    if (want != st->range.length) return false;
    EqCstrCtx ctx = {.p = cstr, .remaining = want, .mismatch = false};
    syntax_text_for_each_chunk(st, eq_cstr_visit, &ctx);
    if (ctx.mismatch) return false;
    return ctx.remaining == 0;
}


// =====================================================================
// UTF-8 char-level API (Phase 4f items 10, 11)
// =====================================================================
//
// Decoder reads 1–4 bytes per code point per the UTF-8 standard. The
// chunk visitor pattern means a multi-byte sequence MAY span chunk
// boundaries (because chunks correspond to GreenToken boundaries, not
// UTF-8 boundaries). The visitor maintains a small carry-over buffer.

// Decode the lead byte at `*p` (where `len` bytes remain). Returns the
// expected total length (1-4) or -1 if invalid. Code point output via
// *out_cp; only valid for the leading byte. Subsequent continuation
// bytes accumulate into *out_cp as bits are shifted in.
static int utf8_seq_len(uint8_t lead) {
    if ((lead & 0x80) == 0x00) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return -1;
}

// Decode one UTF-8 sequence starting at p; returns the code point and
// sets *consumed to the byte length, OR returns -1 if invalid.
// Reads up to 4 bytes from p; caller must ensure that many are
// available (or pass a smaller `len` and accept -1).
static int32_t utf8_decode(const uint8_t *p, uint32_t len, uint32_t *consumed) {
    if (len == 0) return -1;
    int seq = utf8_seq_len(p[0]);
    if (seq < 0 || (uint32_t)seq > len) return -1;
    int32_t cp;
    switch (seq) {
        case 1: cp = p[0]; break;
        case 2:
            cp = (p[0] & 0x1F) << 6 | (p[1] & 0x3F);
            if ((p[1] & 0xC0) != 0x80) return -1;
            break;
        case 3:
            cp = (p[0] & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F);
            if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return -1;
            break;
        case 4:
            cp = (p[0] & 0x07) << 18 | (p[1] & 0x3F) << 12 |
                 (p[2] & 0x3F) << 6  | (p[3] & 0x3F);
            if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 ||
                (p[3] & 0xC0) != 0x80) return -1;
            break;
        default: return -1;
    }
    *consumed = (uint32_t)seq;
    return cp;
}

// Encode a code point into 1–4 UTF-8 bytes. Returns the byte count
// (1-4) or 0 if `c` is out of range.
static int utf8_encode(int32_t c, uint8_t out[4]) {
    if (c < 0) return 0;
    if (c < 0x80) {
        out[0] = (uint8_t)c;
        return 1;
    } else if (c < 0x800) {
        out[0] = 0xC0 | ((c >> 6) & 0x1F);
        out[1] = 0x80 | (c & 0x3F);
        return 2;
    } else if (c < 0x10000) {
        out[0] = 0xE0 | ((c >> 12) & 0x0F);
        out[1] = 0x80 | ((c >> 6) & 0x3F);
        out[2] = 0x80 | (c & 0x3F);
        return 3;
    } else if (c < 0x110000) {
        out[0] = 0xF0 | ((c >> 18) & 0x07);
        out[1] = 0x80 | ((c >> 12) & 0x3F);
        out[2] = 0x80 | ((c >> 6) & 0x3F);
        out[3] = 0x80 | (c & 0x3F);
        return 4;
    }
    return 0;
}


// ---- char_at ------------------------------------------------------

typedef struct {
    uint32_t target;
    uint32_t cursor;     // current absolute offset
    uint8_t  carry[4];   // bytes from previous chunk that didn't form a complete sequence
    uint32_t carry_len;
    int32_t  result;     // -1 = not found / invalid
    bool     found;
} CharAtCtx;

static bool char_at_visit(const char *text, uint32_t len, void *user) {
    CharAtCtx *ctx = (CharAtCtx *)user;
    const uint8_t *p = (const uint8_t *)text;
    uint32_t i = 0;

    // First, drain carry if any.
    while (ctx->carry_len > 0 && i < len) {
        if (ctx->carry_len < 4) {
            ctx->carry[ctx->carry_len++] = p[i++];
        }
        int needed = utf8_seq_len(ctx->carry[0]);
        if (needed < 0) {
            // Invalid; advance and reset.
            ctx->cursor += 1;
            ctx->carry_len = 0;
            if (ctx->cursor > ctx->target) { ctx->found = true; return false; }
            continue;
        }
        if ((uint32_t)needed > ctx->carry_len) continue;  // need more
        uint32_t consumed;
        int32_t cp = utf8_decode(ctx->carry, ctx->carry_len, &consumed);
        if (ctx->target == ctx->cursor) {
            ctx->result = cp;
            ctx->found = true;
            return false;
        }
        ctx->cursor += consumed;
        ctx->carry_len = 0;
        if (ctx->cursor > ctx->target) {
            // Target was inside a multi-byte sequence — invalid boundary.
            ctx->result = -1;
            ctx->found = true;
            return false;
        }
    }

    while (i < len) {
        int needed = utf8_seq_len(p[i]);
        if (needed < 0) {
            if (ctx->target == ctx->cursor) {
                ctx->result = -1;
                ctx->found = true;
                return false;
            }
            ctx->cursor += 1;
            i += 1;
            continue;
        }
        if (i + (uint32_t)needed > len) {
            // Carry-over to next chunk.
            for (uint32_t k = 0; k < len - i; k++) ctx->carry[k] = p[i + k];
            ctx->carry_len = len - i;
            return true;
        }
        uint32_t consumed;
        int32_t cp = utf8_decode(p + i, len - i, &consumed);
        if (ctx->target == ctx->cursor) {
            ctx->result = cp;
            ctx->found = true;
            return false;
        }
        ctx->cursor += consumed;
        i += consumed;
        if (ctx->cursor > ctx->target) {
            ctx->result = -1;  // mid-sequence
            ctx->found = true;
            return false;
        }
    }
    return true;
}

int32_t syntax_text_char_at(const SyntaxText *st, uint32_t offset) {
    if (offset < st->range.start) return -1;
    if (offset >= text_range_end(st->range)) return -1;
    CharAtCtx ctx = {.target = offset, .cursor = st->range.start,
                     .carry_len = 0, .result = -1, .found = false};
    syntax_text_for_each_chunk(st, char_at_visit, &ctx);
    return ctx.found ? ctx.result : -1;
}


// ---- find_char / contains_char ------------------------------------

typedef struct {
    const uint8_t *needle;
    uint32_t       needle_len;
    uint32_t       cursor;      // absolute offset
    uint32_t       found_at;
    uint8_t        carry[8];    // up to 4 lookback for boundary match
    uint32_t       carry_len;
} FindCharCtx;

// Naive byte-match across (carry || text). For typical 1-4-byte needles
// on chunks of 1-8 bytes (typical tokens), this is plenty fast.
static bool find_char_visit(const char *text, uint32_t len, void *user) {
    FindCharCtx *ctx = (FindCharCtx *)user;
    const uint8_t *p = (const uint8_t *)text;

    // Stitch carry + first few bytes for boundary-spanning match.
    if (ctx->carry_len > 0) {
        for (uint32_t i = 0; i < len && ctx->carry_len < 8; i++) {
            ctx->carry[ctx->carry_len++] = p[i];
        }
        // Scan within the stitched buffer (excluding the final
        // ctx->needle_len-1 bytes — those might extend into next chunk).
        uint32_t stitched_scannable = ctx->carry_len >= ctx->needle_len
                                          ? ctx->carry_len - ctx->needle_len + 1
                                          : 0;
        for (uint32_t i = 0; i < stitched_scannable; i++) {
            if (memcmp(ctx->carry + i, ctx->needle, ctx->needle_len) == 0) {
                ctx->found_at = ctx->cursor + i;
                return false;
            }
        }
        // Drop bytes that were processed from the original carry.
        // The remaining carry is the trailing (needle_len - 1) bytes for
        // possible boundary match on the next chunk. But also we've
        // consumed some bytes from this chunk's start. Reset.
        ctx->cursor += stitched_scannable;
        ctx->carry_len = 0;
    }

    if (len + 0u < ctx->needle_len) {
        // Whole chunk smaller than needle — accumulate into carry.
        for (uint32_t i = 0; i < len; i++) ctx->carry[ctx->carry_len++] = p[i];
        return true;
    }

    // Find offset within this chunk where scanning should start.
    // Without carry, that's 0; with carry already consumed above,
    // cursor was advanced past the stitched portion, but our chunk
    // position is still 0. We need to align: re-scan from chunk start
    // where the leftover-from-carry overlap ended.
    // Simpler: scan the whole chunk for matches not crossing the
    // chunk's tail boundary.
    uint32_t scan_end = len - (ctx->needle_len - 1);
    for (uint32_t i = 0; i < scan_end; i++) {
        if (memcmp(p + i, ctx->needle, ctx->needle_len) == 0) {
            ctx->found_at = ctx->cursor + i;
            return false;
        }
    }
    // Save the trailing (needle_len - 1) bytes as carry for next chunk.
    ctx->cursor += scan_end;
    uint32_t tail = ctx->needle_len - 1;
    for (uint32_t i = 0; i < tail; i++) {
        ctx->carry[i] = p[len - tail + i];
    }
    ctx->carry_len = tail;
    return true;
}

uint32_t syntax_text_find_char(const SyntaxText *st, int32_t c) {
    uint8_t enc[4];
    int nlen = utf8_encode(c, enc);
    if (nlen <= 0) return UINT32_MAX;
    if (nlen == 1) {
        // Fast path: equivalent to find_byte for ASCII.
        return syntax_text_find_byte(st, (char)enc[0]);
    }
    FindCharCtx ctx = {.needle = enc, .needle_len = (uint32_t)nlen,
                       .cursor = st->range.start, .found_at = UINT32_MAX,
                       .carry_len = 0};
    syntax_text_for_each_chunk(st, find_char_visit, &ctx);
    return ctx.found_at;
}

bool syntax_text_contains_char(const SyntaxText *st, int32_t c) {
    return syntax_text_find_char(st, c) != UINT32_MAX;
}


// ---- try_fold_chunks ----------------------------------------------

typedef struct {
    SyntaxTextChunkFoldFn fn;
    void                 *acc;
    void                 *user;
    bool                  aborted;
} FoldCtx;

static bool fold_visit(const char *text, uint32_t len, void *user) {
    FoldCtx *ctx = (FoldCtx *)user;
    if (!ctx->fn(text, len, ctx->acc, ctx->user)) {
        ctx->aborted = true;
        return false;
    }
    return true;
}

bool syntax_text_try_fold_chunks(const SyntaxText *st, SyntaxTextChunkFoldFn fn,
                                  void *acc, void *user) {
    FoldCtx ctx = {.fn = fn, .acc = acc, .user = user, .aborted = false};
    syntax_text_for_each_chunk(st, fold_visit, &ctx);
    return !ctx.aborted;
}
