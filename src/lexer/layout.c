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

static bool is_binary_op_token(TokenKind k) {
  switch (k) {
  case TK_PLUS:
  case TK_MINUS:
  case TK_STAR:
  case TK_STAR_STAR:
  case TK_SLASH:
  case TK_PERCENT:
  case TK_EQ_EQ:
  case TK_BANG_EQ:
  case TK_LE:
  case TK_GE:
  case TK_AMP_AMP:
  case TK_PIPE_PIPE:
  case TK_AMP:
  case TK_PIPE:
  case TK_CARET:
  case TK_SHL:
  case TK_SHR:
  case TK_PLUS_EQ:
  case TK_MINUS_EQ:
  case TK_STAR_EQ:
  case TK_SLASH_EQ:
  case TK_PERCENT_EQ:
  case TK_EQ:
  case TK_AMP_EQ:
  case TK_PIPE_EQ:
  case TK_CARET_EQ:
  case TK_ORELSE: // `a orelse\n b` — nullable-coalescing infix, line continues
    return true;
  default:
    return false;
  }
}

static bool is_start_continuation(TokenKind k) {
  if (is_binary_op_token(k))
    return true;
  switch (k) {
  case TK_RPAREN:
  case TK_RBRACKET:
  case TK_COMMA:
  case TK_LBRACE:
  case TK_RBRACE:
  case TK_ELSE:
  case TK_ELIF:
  case TK_RARROW:
  case TK_COLON:
  case TK_DOT_DOT:
  case TK_COLON_EQ:
  case TK_GT:
    return true;
  default:
    return false;
  }
}

static bool is_end_continuation(TokenKind k) {
  if (is_binary_op_token(k))
    return true;
  switch (k) {
  case TK_LPAREN:
  case TK_LBRACKET:
  case TK_COMMA:
  case TK_LBRACE:
  case TK_DOT:
  case TK_LT:
    return true;
  default:
    return false;
  }
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

// =====================================================================
// Pipeline driver (Single-Pass)
// =====================================================================

void layout(const Vec *in_raw_tokens, const uint32_t *line_starts,
            size_t n_line_starts, Vec *out_real_tokens, Vec *out_trivia_tokens,
            Vec *out_trivia_offsets) {

  vec_clear(out_real_tokens);
  vec_clear(out_trivia_tokens);
  vec_clear(out_trivia_offsets);

  // Initial offset for the first token
  uint32_t initial_offset = 0;
  vec_push(out_trivia_offsets, &initial_offset);

  if (in_raw_tokens->count == 0)
    return;

// Layout-frame stack. 256 levels covers any sane code (typical max is
// 8-10 deep; even pathological codegen rarely exceeds ~50). Overflow
// asserts loudly — it's a signal that something upstream is wrong,
// not a case to silently grow the buffer.
#define LAYOUT_MAX_FRAMES 256
  LayoutFrame frames[LAYOUT_MAX_FRAMES];
  size_t frames_count = 0;

  LayoutFrame current = {.indent = 1, .kind = FRAME_ROOT};

  TokenKind prev_kind = TK_EOF;
  uint32_t prev_byte_end = 0;

  if (in_raw_tokens->count > 0) {
    Token *first = vec_get((Vec *)in_raw_tokens, 0);
    prev_byte_end = first->start;
  }

  bool newline_seen = false;

  // Trivia-attribution cursor. Updated when a REAL source token is
  // emitted (and at EOF); used as the offset value for synthetics so
  // they always have empty preceding-trivia ranges. Trivia flows
  // past synthetics to attach to the next real source token (or to
  // EOF for trailing trivia).
  uint32_t last_real_offset = 0;

// Helper macro for real-token emission. Records the token + its
// offset (= current trivia count) and updates the cursor.
#define EMIT_REAL(tok_ptr)                                                     \
  do {                                                                         \
    vec_push(out_real_tokens, (tok_ptr));                                      \
    last_real_offset = out_trivia_tokens->count;                               \
    vec_push(out_trivia_offsets, &last_real_offset);                           \
  } while (0)

  for (size_t i = 0; i < in_raw_tokens->count; i++) {
    Token *cur = vec_get((Vec *)in_raw_tokens, i);

    if (cur->kind == TK_EOF)
      break;

    if (token_is_trivia(cur->kind)) {
      if (cur->kind == TK_NEWLINE)
        newline_seen = true;
      vec_push(out_trivia_tokens, cur);
      continue;
    }

    if (cur->kind == TK_ERROR) {
      EMIT_REAL(cur);
      prev_kind = cur->kind;
      prev_byte_end = cur->byte_end;
      newline_seen = false;
      continue;
    }

    uint32_t indent = get_column(cur->start, line_starts, n_line_starts);
    uint32_t layout_col = current.indent;

    // ---- (1) Insert `{` and push layout ---------------------------
    if (newline_seen && indent > layout_col &&
        !is_expr_continuation(prev_kind, cur->kind)) {
      emit_synthetic(out_real_tokens, out_trivia_offsets, TK_LBRACE,
                     prev_byte_end, last_real_offset);

      assert(frames_count < LAYOUT_MAX_FRAMES && "layout frame stack overflow");
      frames[frames_count++] = current;
      current.indent = indent;
      current.kind = FRAME_LAYOUT;

      prev_kind = TK_LBRACE;
      newline_seen = false;
      i--; // re-examine
      continue;
    }

    // ---- (2) Insert `;` and `}` and pop layout --------------------
    if (newline_seen && indent < layout_col &&
        !(cur->kind == TK_RBRACE && current.kind == FRAME_EXPLICIT)) {
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
      i--; // re-examine
      continue;
    }

    // ---- (3) Push layout on `{` -----------------------------------
    if (cur->kind == TK_LBRACE) {
      // Scan forward for the next real token's column — only needed
      // here, to set the indent of the explicit block we're opening.
      uint32_t next_indent = 1;
      for (size_t j = i + 1; j < in_raw_tokens->count; j++) {
        Token *next = vec_get((Vec *)in_raw_tokens, j);
        if (!token_is_trivia(next->kind)) {
          next_indent = get_column(next->start, line_starts, n_line_starts);
          break;
        }
      }

      EMIT_REAL(cur);

      assert(frames_count < LAYOUT_MAX_FRAMES && "layout frame stack overflow");
      frames[frames_count++] = current;
      current.indent = next_indent;
      current.kind = FRAME_EXPLICIT;

      prev_kind = cur->kind;
      prev_byte_end = cur->byte_end;
      newline_seen = false;
      continue;
    }

    // ---- (4) Pop layout on `}` ------------------------------------
    if (cur->kind == TK_RBRACE) {
      while (current.kind == FRAME_LAYOUT && frames_count > 0) {
        emit_synthetic(out_real_tokens, out_trivia_offsets, TK_RBRACE,
                       prev_byte_end, last_real_offset);
        current = frames[--frames_count];
      }

      if (should_emit_semicolon(prev_kind)) {
        emit_synthetic(out_real_tokens, out_trivia_offsets, TK_SEMI,
                       prev_byte_end, last_real_offset);
      }

      EMIT_REAL(cur);

      if (frames_count > 0)
        current = frames[--frames_count];

      prev_kind = cur->kind;
      prev_byte_end = cur->byte_end;
      newline_seen = false;
      continue;
    }

    // ---- (5) Insert `;` between siblings --------------------------
    if (newline_seen && indent == layout_col &&
        !is_expr_continuation(prev_kind, cur->kind)) {
      if (should_emit_semicolon(prev_kind)) {
        emit_synthetic(out_real_tokens, out_trivia_offsets, TK_SEMI,
                       prev_byte_end, last_real_offset);
      }

      EMIT_REAL(cur);

      prev_kind = cur->kind;
      prev_byte_end = cur->byte_end;
      newline_seen = false;
      continue;
    }

    // ---- (6) Pass through -----------------------------------------
    EMIT_REAL(cur);
    prev_kind = cur->kind;
    prev_byte_end = cur->byte_end;
    newline_seen = false;
  }

  // EOF: close all open layouts. Synthetics inherit last_real_offset
  // so any trailing trivia stays attached to EOF below, not to these.
  if (should_emit_semicolon(prev_kind)) {
    emit_synthetic(out_real_tokens, out_trivia_offsets, TK_SEMI, prev_byte_end,
                   last_real_offset);
  }

  while (frames_count > 0) {
    emit_synthetic(out_real_tokens, out_trivia_offsets, TK_RBRACE,
                   prev_byte_end, last_real_offset);
    emit_synthetic(out_real_tokens, out_trivia_offsets, TK_SEMI, prev_byte_end,
                   last_real_offset);
    current = frames[--frames_count];
  }

  // EOF emit: advances last_real_offset so trailing trivia between the
  // last real source token and EOF gets attributed to EOF's preceding
  // range. EOF acts as the natural anchor for end-of-file whitespace.
  if (in_raw_tokens->count > 0) {
    Token *last = vec_get((Vec *)in_raw_tokens, in_raw_tokens->count - 1);
    if (last->kind == TK_EOF) {
      EMIT_REAL(last);
    }
  }

#undef EMIT_REAL
}
