#include "./layout.h"
#include "token.h"

#include <assert.h>

// =====================================================================
// Line/Column Resolution
// =====================================================================

static uint32_t get_column(uint32_t byte_start, const uint32_t *line_starts,
                           size_t n) {
  if (n == 0)
    return 1;
  size_t low = 0;
  size_t high = n - 1;
  size_t best = 0;
  while (low <= high) {
    size_t mid = low + (high - low) / 2;
    if (line_starts[mid] <= byte_start) {
      best = mid;
      low = mid + 1;
    } else {
      if (mid == 0)
        break;
      high = mid - 1;
    }
  }
  return byte_start - line_starts[best] + 1;
}

// =====================================================================
// Synthetic-token construction
// =====================================================================
//
// Synthetics carry the FROZEN trivia offset (the value from the last real
// emit), not the current trivia.count. This gives synthetics an empty
// preceding-trivia range and lets the trivia flow past them to attach to
// the next real source token (or to EOF for trailing trivia). Without
// this freezing, trivia preceding a real token would attribute to a
// synthetic that the layout pass injected in between — positionally
// wrong (synthetics live at prev_byte_end, before the trivia).

static void emit_synthetic(Vec *out_real, Vec *out_offsets, TokenKind kind,
                           uint32_t pos, uint32_t frozen_offset) {
  Token t = {
      .kind = kind,
      ._pad0 = 0,
      .string_id = STR_ID_NONE,
      .start = pos,
      .byte_end = pos,
  };
  vec_push(out_real, &t);
  vec_push(out_offsets, &frozen_offset);
}

static bool should_emit_semicolon(TokenKind prev) {
  switch (prev) {
  case TK_LBRACE:
  case TK_SEMI:
  case TK_EOF:
    return false;
  default:
    return true;
  }
}

// =====================================================================
// Continuation predicates
// =====================================================================

// Token classification as a precomputed flag table instead of three
// switches. Each predicate is now one L1 load + AND + setcc —
// branchless and constant-time — versus a compiler-lowered jump table
// or compare-chain (data-dependent indirect branch → mispredicts) hit
// once per token in the layout pass. One table also keeps the whole
// classification in a single place, and bakes the old runtime
// composition (start/end were `is_binary_op_token(k) || ...`) straight
// into the bits.
//
// Behavior is identical to the prior switches. Unlisted kinds default
// to 0 (C zero-fills the rest of a designated-initialized static).
enum {
  TF_BINOP = 1u << 0, // continues a line as an infix operator
  TF_START = 1u << 1, // legal at the START of a continuation line
  TF_END = 1u << 2,   // legal at the END (prev token) of a continued line
};

// Every binary-op token was also a start- AND end-continuation under the
// old `if (is_binary_op_token(k)) return true;` prefix, so they get all
// three bits.
#define TF_OP (TF_BINOP | TF_START | TF_END)

static const uint8_t tok_flags[TK_COUNT] = {
    // Binary / assignment operators (orelse: `a orelse\n b` continues).
    [TK_PLUS] = TF_OP,     [TK_MINUS] = TF_OP,    [TK_STAR] = TF_OP,
    [TK_STAR_STAR] = TF_OP,[TK_SLASH] = TF_OP,    [TK_PERCENT] = TF_OP,
    [TK_EQ_EQ] = TF_OP,    [TK_BANG_EQ] = TF_OP,  [TK_LE] = TF_OP,
    [TK_GE] = TF_OP,       [TK_AMP_AMP] = TF_OP,  [TK_PIPE_PIPE] = TF_OP,
    [TK_AMP] = TF_OP,      [TK_PIPE] = TF_OP,     [TK_CARET] = TF_OP,
    [TK_SHL] = TF_OP,      [TK_SHR] = TF_OP,      [TK_PLUS_EQ] = TF_OP,
    [TK_MINUS_EQ] = TF_OP, [TK_STAR_EQ] = TF_OP,  [TK_SLASH_EQ] = TF_OP,
    [TK_PERCENT_EQ] = TF_OP,[TK_EQ] = TF_OP,      [TK_AMP_EQ] = TF_OP,
    [TK_PIPE_EQ] = TF_OP,  [TK_CARET_EQ] = TF_OP, [TK_ORELSE] = TF_OP,

    // Start- and end-continuation (non-operator).
    [TK_COMMA] = TF_START | TF_END,
    [TK_LBRACE] = TF_START | TF_END,

    // Start-continuation only.
    [TK_RPAREN] = TF_START,   [TK_RBRACKET] = TF_START,
    [TK_RBRACE] = TF_START,   [TK_ELSE] = TF_START,
    [TK_ELIF] = TF_START,     [TK_RARROW] = TF_START,
    [TK_COLON] = TF_START,    [TK_DOT_DOT] = TF_START,
    [TK_COLON_EQ] = TF_START, [TK_GT] = TF_START,

    // End-continuation only.
    [TK_LPAREN] = TF_END,  [TK_LBRACKET] = TF_END,
    [TK_DOT] = TF_END,     [TK_LT] = TF_END,
};

#undef TF_OP

static inline bool is_binary_op_token(TokenKind k) {
  return (tok_flags[k] & TF_BINOP) != 0;
}

static inline bool is_start_continuation(TokenKind k) {
  return (tok_flags[k] & TF_START) != 0;
}

static inline bool is_end_continuation(TokenKind k) {
  return (tok_flags[k] & TF_END) != 0;
}

static bool is_expr_continuation(TokenKind prev, TokenKind cur) {
  return is_start_continuation(cur) || is_end_continuation(prev);
}

// =====================================================================
// Layout state
// =====================================================================

typedef enum { FRAME_ROOT, FRAME_LAYOUT, FRAME_EXPLICIT } FrameKind;

typedef struct {
  uint32_t indent;
  FrameKind kind;
} LayoutFrame;

// Layout-frame stack depth. 256 levels covers any sane code (typical
// max is 8-10 deep; even pathological codegen rarely exceeds ~50).
// Overflow asserts loudly — it's a signal that something upstream is
// wrong, not a case to silently grow the buffer.
#define LAYOUT_MAX_FRAMES 256

// =====================================================================
// Fused streaming driver
// =====================================================================
//
// This is the single layout driver. It was derived as a faithful
// transliteration of the prior two-pass layout() (which walked a
// pre-built Vec<Token>); the two-pass version was deleted once this
// proved byte-identical over every example. The only structural
// differences from that algorithm, both forced by the pull model and
// proven behaviorally identical:
//
//   * The `for (i over raw_tokens)` walk becomes a `lex_next()` pull
//     loop holding ONE token. Each old `i--; continue;` (which
//     re-examined the same raw token after a frame push/pop) becomes a
//     `continue` of the INNER loop on the same held token — no new
//     pull. This reproduces multi-synthetic-per-position (dedent
//     unwinding K frames = K inner iterations) exactly.
//
//   * Case (3) `{` cannot forward-scan future tokens for the explicit
//     block's column. Instead the frame is opened with indent =
//     INDENT_UNRESOLVED and the column is filled in when the next
//     non-trivia token is reached (deferred-indent). current.indent is
//     never read between the `{` and that token (only trivia
//     intervene, and trivia handling never reads it), so this yields
//     the same value the forward-scan would have, at the same time it
//     is first needed. If EOF arrives first, the value is never read
//     (EOF-close ignores current.indent) — matching the original's
//     unused next_indent default.
//
// All guard logic, predicates, EMIT_REAL/last_real_offset freezing and
// TriviaSpan recording are verbatim from that algorithm.

#define INDENT_UNRESOLVED 0u // real columns are 1-based; 0 == "not set"

void layout_stream(LexCursor *lc, const Vec *line_starts,
                   Vec *out_real_tokens, Vec *out_trivia_tokens,
                   Vec *out_trivia_offsets) {

  vec_clear(out_real_tokens);
  vec_clear(out_trivia_tokens);
  vec_clear(out_trivia_offsets);

  // Initial offset for the first token
  uint32_t initial_offset = 0;
  vec_push(out_trivia_offsets, &initial_offset);

  LayoutFrame frames[LAYOUT_MAX_FRAMES];
  size_t frames_count = 0;

  LayoutFrame current = {.indent = 1, .kind = FRAME_ROOT};

  TokenKind prev_kind = TK_EOF;
  uint32_t prev_byte_end = 0;

  bool newline_seen = false;
  uint32_t last_real_offset = 0;

#define EMIT_REAL(tok_ptr)                                                     \
  do {                                                                         \
    vec_push(out_real_tokens, (tok_ptr));                                      \
    last_real_offset = out_trivia_tokens->count;                               \
    vec_push(out_trivia_offsets, &last_real_offset);                           \
  } while (0)

  // Mirror layout()'s init quirk: prev_byte_end = first raw token's
  // start, regardless of whether that token is trivia.
  Token held = lex_next(lc);
  prev_byte_end = held.start;

  for (;;) {
    if (token_is_trivia(held.kind)) {
      if (held.kind == TK_NEWLINE)
        newline_seen = true;
      // Zero-copy: record the source range, not a 16-byte Token copy.
      TriviaSpan ts = {held.start, held.byte_end};
      vec_push(out_trivia_tokens, &ts);
      held = lex_next(lc);
      continue;
    }

    // Deferred-indent: the first non-trivia token after an explicit `{`
    // determines that frame's column (== layout()'s forward-scan).
    if (current.kind == FRAME_EXPLICIT && current.indent == INDENT_UNRESOLVED)
      current.indent =
          get_column(held.start, (const uint32_t *)line_starts->data,
                     line_starts->count);

    if (held.kind == TK_EOF) {
      // EOF: close all open layouts. Synthetics inherit
      // last_real_offset so any trailing trivia stays attached to EOF
      // below, not to these.
      if (should_emit_semicolon(prev_kind)) {
        emit_synthetic(out_real_tokens, out_trivia_offsets, TK_SEMI,
                       prev_byte_end, last_real_offset);
      }

      while (frames_count > 0) {
        emit_synthetic(out_real_tokens, out_trivia_offsets, TK_RBRACE,
                       prev_byte_end, last_real_offset);
        emit_synthetic(out_real_tokens, out_trivia_offsets, TK_SEMI,
                       prev_byte_end, last_real_offset);
        current = frames[--frames_count];
      }

      // EOF emit: advances last_real_offset so trailing trivia between
      // the last real source token and EOF gets attributed to EOF's
      // preceding range.
      EMIT_REAL(&held);
      break;
    }

    if (held.kind == TK_ERROR) {
      EMIT_REAL(&held);
      prev_kind = held.kind;
      prev_byte_end = held.byte_end;
      newline_seen = false;
      held = lex_next(lc);
      continue;
    }

    // Re-examination loop: each `continue` re-evaluates the guards on
    // the SAME held token (== layout()'s `i--; continue;`); each
    // `break` advances to the next raw token.
    for (;;) {
      uint32_t indent =
          get_column(held.start, (const uint32_t *)line_starts->data,
                     line_starts->count);
      uint32_t layout_col = current.indent;

      // ---- (1) Insert `{` and push layout ---------------------------
      if (newline_seen && indent > layout_col &&
          !is_expr_continuation(prev_kind, held.kind)) {
        emit_synthetic(out_real_tokens, out_trivia_offsets, TK_LBRACE,
                       prev_byte_end, last_real_offset);

        assert(frames_count < LAYOUT_MAX_FRAMES &&
               "layout frame stack overflow");
        frames[frames_count++] = current;
        current.indent = indent;
        current.kind = FRAME_LAYOUT;

        prev_kind = TK_LBRACE;
        newline_seen = false;
        continue; // re-examine same held token
      }

      // ---- (2) Insert `;` and `}` and pop layout --------------------
      if (newline_seen && indent < layout_col &&
          !(held.kind == TK_RBRACE && current.kind == FRAME_EXPLICIT)) {
        if (should_emit_semicolon(prev_kind)) {
          emit_synthetic(out_real_tokens, out_trivia_offsets, TK_SEMI,
                         prev_byte_end, last_real_offset);
        }
        emit_synthetic(out_real_tokens, out_trivia_offsets, TK_RBRACE,
                       prev_byte_end, last_real_offset);

        if (frames_count > 0)
          current = frames[--frames_count];

        prev_kind = TK_RBRACE;
        newline_seen = false;
        continue; // re-examine same held token
      }

      // ---- (3) Push layout on `{` -----------------------------------
      if (held.kind == TK_LBRACE) {
        EMIT_REAL(&held);

        assert(frames_count < LAYOUT_MAX_FRAMES &&
               "layout frame stack overflow");
        frames[frames_count++] = current;
        current.indent = INDENT_UNRESOLVED; // resolved at next non-trivia
        current.kind = FRAME_EXPLICIT;

        prev_kind = held.kind;
        prev_byte_end = held.byte_end;
        newline_seen = false;
        break;
      }

      // ---- (4) Pop layout on `}` ------------------------------------
      if (held.kind == TK_RBRACE) {
        while (current.kind == FRAME_LAYOUT && frames_count > 0) {
          emit_synthetic(out_real_tokens, out_trivia_offsets, TK_RBRACE,
                         prev_byte_end, last_real_offset);
          current = frames[--frames_count];
        }

        if (should_emit_semicolon(prev_kind)) {
          emit_synthetic(out_real_tokens, out_trivia_offsets, TK_SEMI,
                         prev_byte_end, last_real_offset);
        }

        EMIT_REAL(&held);

        if (frames_count > 0)
          current = frames[--frames_count];

        prev_kind = held.kind;
        prev_byte_end = held.byte_end;
        newline_seen = false;
        break;
      }

      // ---- (5) Insert `;` between siblings --------------------------
      if (newline_seen && indent == layout_col &&
          !is_expr_continuation(prev_kind, held.kind)) {
        if (should_emit_semicolon(prev_kind)) {
          emit_synthetic(out_real_tokens, out_trivia_offsets, TK_SEMI,
                         prev_byte_end, last_real_offset);
        }

        EMIT_REAL(&held);

        prev_kind = held.kind;
        prev_byte_end = held.byte_end;
        newline_seen = false;
        break;
      }

      // ---- (6) Pass through -----------------------------------------
      EMIT_REAL(&held);
      prev_kind = held.kind;
      prev_byte_end = held.byte_end;
      newline_seen = false;
      break;
    }

    held = lex_next(lc);
  }

#undef EMIT_REAL
}

#undef INDENT_UNRESOLVED
