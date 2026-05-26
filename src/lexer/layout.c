#include "./layout.h"
#include "./token.h"

#include <assert.h>

// =====================================================================
// Line/Column Resolution
// =====================================================================

// Column of `byte_start` (1-based) = byte_start - (start of its line).
// `*cur` is a monotone forward cursor into line_starts: layout_stream
// processes tokens in source order, so the line index only ever moves
// forward — amortized O(1) per token instead of get_column's O(log L)
// binary search per token (it was the hot per-token cost in
// layout_stream). Byte-identical to the old binary search for
// source-ordered byte_start (the only inputs here: real tokens; the
// re-exam loop re-passes the SAME held.start, which is idempotent).
static inline uint32_t col_advance(uint32_t byte_start,
                                   const uint32_t *line_starts, size_t n,
                                   size_t *cur) {
  if (n == 0)
    return 1;
  size_t c = *cur;
  while (c + 1 < n && line_starts[c + 1] <= byte_start)
    c++;
  *cur = c;
  return byte_start - line_starts[c] + 1;
}

// =====================================================================
// Synthetic-token construction
// =====================================================================
//
// All synthetics are zero-width tokens placed at `prev_byte_end` (the
// end of the previous real source token). They sit BEFORE any trivia
// that follows the previous real token, preserving positional order
// (zero-width at offset X comes before non-zero-width starting at X).
//
// Synthetics use the SK_VIRTUAL_* kinds (distinct from explicit
// SK_LBRACE/SK_RBRACE/SK_SEMI) so downstream consumers — formatter,
// syntax highlighter, refactor tools — can tell user-typed vs
// layout-inserted apart.

static void emit_synthetic(Vec *out_tokens, SyntaxKind kind, uint32_t pos) {
  Token t = {
      .kind = kind,
      ._pad0 = 0,
      .string_id = STR_ID_NONE,
      .start = pos,
      .byte_end = pos,
  };
  *(Token *)vec_push_slot(out_tokens) = t;
}

// Treat both explicit and layout-inserted block openers / statement
// separators as "already terminating," so we don't double-emit semis.
static bool should_emit_semicolon(SyntaxKind prev) {
  switch (prev) {
  case SK_LBRACE:
  case SK_VIRTUAL_LBRACE:
  case SK_SEMI:
  case SK_VIRTUAL_SEMI:
  case SK_EOF:
    return false;
  default:
    return true;
  }
}

// =====================================================================
// Continuation predicates
// =====================================================================
//
// Token classification as a precomputed flag table. Each predicate is
// one L1 load + AND + setcc — branchless and constant-time.

enum {
  TF_START = 1u << 0, // legal at the START of a continuation line
  TF_END = 1u << 1,   // legal at the END (prev token) of a continued line
};

// Every binary-op token also marks as start- AND end-continuation.
#define TF_OP (TF_START | TF_END)

static const uint8_t tok_flags[SK_LAST_TOKEN_KIND] = {
    // Binary / assignment operators (orelse: `a orelse\n b` continues).
    [SK_PLUS] = TF_OP,
    [SK_MINUS] = TF_OP,
    [SK_STAR] = TF_OP,
    [SK_STAR_STAR] = TF_OP,
    [SK_SLASH] = TF_OP,
    [SK_PERCENT] = TF_OP,
    [SK_EQ_EQ] = TF_OP,
    [SK_BANG_EQ] = TF_OP,
    [SK_LE] = TF_OP,
    [SK_GE] = TF_OP,
    [SK_AMP_AMP] = TF_OP,
    [SK_PIPE_PIPE] = TF_OP,
    [SK_AMP] = TF_OP,
    [SK_PIPE] = TF_OP,
    [SK_CARET] = TF_OP,
    [SK_SHL] = TF_OP,
    [SK_SHR] = TF_OP,
    [SK_PLUS_EQ] = TF_OP,
    [SK_MINUS_EQ] = TF_OP,
    [SK_STAR_EQ] = TF_OP,
    [SK_SLASH_EQ] = TF_OP,
    [SK_PERCENT_EQ] = TF_OP,
    [SK_EQ] = TF_OP,
    [SK_AMP_EQ] = TF_OP,
    [SK_PIPE_EQ] = TF_OP,
    [SK_TILDE_EQ] = TF_OP,
    [SK_ORELSE_KW] = TF_OP,

    // Start- and end-continuation (non-operator). Both explicit and
    // virtual LBRACE count: `... = {\n ... }` continues, regardless of
    // whether the `{` came from source or layout (rare in practice but
    // semantically consistent).
    [SK_COMMA] = TF_START | TF_END,
    [SK_LBRACE] = TF_START | TF_END,
    [SK_VIRTUAL_LBRACE] = TF_START | TF_END,

    // Start-continuation only.
    [SK_RPAREN] = TF_START,
    [SK_RBRACKET] = TF_START,
    [SK_RBRACE] = TF_START,
    [SK_VIRTUAL_RBRACE] = TF_START,
    [SK_ELSE_KW] = TF_START,
    [SK_ELIF_KW] = TF_START,
    [SK_RARROW] = TF_START,
    [SK_COLON] = TF_START,
    [SK_DOT_DOT] = TF_START,
    [SK_COLON_EQ] = TF_START,
    [SK_GT] = TF_START,

    // End-continuation only.
    [SK_LPAREN] = TF_END,
    [SK_LBRACKET] = TF_END,
    [SK_DOT] = TF_END,
    [SK_LT] = TF_END,
};

#undef TF_OP

static inline bool is_start_continuation(SyntaxKind k) {
  return (tok_flags[k] & TF_START) != 0;
}

static inline bool is_end_continuation(SyntaxKind k) {
  return (tok_flags[k] & TF_END) != 0;
}

static bool is_expr_continuation(SyntaxKind prev, SyntaxKind cur) {
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

#define LAYOUT_MAX_FRAMES 256

// Pending-trivia scratch buffer. Trivia from the lexer accumulates here
// while we wait to decide whether to emit a synthetic at the previous
// real token's byte_end (which sits BEFORE the trivia positionally).
// On EMIT_REAL the buffer flushes to out_tokens, then the real token
// is pushed. This is the ONE place where layout deviates from a pure
// "emit-as-you-see" stream — necessary because the synthetic decision
// depends on the next non-trivia token.
//
// 256 trivia tokens between two real tokens is generous; pathological
// files (giant comment blocks) trip the assert as a loud signal.
#define LAYOUT_TRIVIA_SCRATCH 256

// =====================================================================
// Fused streaming driver
// =====================================================================
//
// Outputs ONE Vec<Token> in document order with trivia + virtual
// layout + real tokens all distinguishable by kind. The state machine
// (frame stack, indent rules, continuation predicates) is byte-for-
// byte identical to the prior 3-Vec version.

#define INDENT_UNRESOLVED 0u // real columns are 1-based; 0 == "not set"

void layout_stream(LexCursor *lc, const Vec *line_starts, Vec *out_tokens) {
  vec_clear(out_tokens);

  LayoutFrame frames[LAYOUT_MAX_FRAMES];
  size_t frames_count = 0;

  LayoutFrame current = {.indent = 1, .kind = FRAME_ROOT};

  SyntaxKind prev_kind = SK_EOF;
  uint32_t prev_byte_end = 0;

  bool newline_seen = false;

  // Monotone forward cursor into line_starts for col_advance — tokens
  // arrive in source order, so this only moves forward.
  size_t line_cur = 0;

  // Pending trivia between the previous real token and the next.
  // Flushed by EMIT_REAL before pushing the real token.
  Token trivia_buf[LAYOUT_TRIVIA_SCRATCH];
  size_t trivia_buf_count = 0;

#define FLUSH_TRIVIA()                                                         \
  do {                                                                         \
    for (size_t _i = 0; _i < trivia_buf_count; _i++)                           \
      *(Token *)vec_push_slot(out_tokens) = trivia_buf[_i];                    \
    trivia_buf_count = 0;                                                      \
  } while (0)

#define EMIT_REAL(tok_ptr)                                                     \
  do {                                                                         \
    FLUSH_TRIVIA();                                                            \
    *(Token *)vec_push_slot(out_tokens) = *(tok_ptr);                          \
  } while (0)

  // Mirror the historical init quirk: prev_byte_end = first raw token's
  // start, regardless of whether that token is trivia.
  Token held = lex_next(lc);
  prev_byte_end = held.start;

  for (;;) {
    if (token_is_trivia(held.kind)) {
      if (held.kind == SK_NEWLINE)
        newline_seen = true;
      assert(trivia_buf_count < LAYOUT_TRIVIA_SCRATCH &&
             "layout trivia scratch overflow");
      trivia_buf[trivia_buf_count++] = held;
      held = lex_next(lc);
      continue;
    }

    // Deferred-indent: the first non-trivia token after an explicit `{`
    // determines that frame's column (matches the old forward-scan).
    if (current.kind == FRAME_EXPLICIT && current.indent == INDENT_UNRESOLVED)
      current.indent =
          col_advance(held.start, (const uint32_t *)line_starts->data,
                      line_starts->count, &line_cur);

    if (held.kind == SK_EOF) {
      // EOF: close all open layouts. Synthetics live at prev_byte_end
      // (before any trailing trivia in the buffer).
      if (should_emit_semicolon(prev_kind)) {
        emit_synthetic(out_tokens, SK_VIRTUAL_SEMI, prev_byte_end);
      }

      while (frames_count > 0) {
        emit_synthetic(out_tokens, SK_VIRTUAL_RBRACE, prev_byte_end);
        emit_synthetic(out_tokens, SK_VIRTUAL_SEMI, prev_byte_end);
        current = frames[--frames_count];
      }

      // Flush any trailing trivia, then emit EOF.
      EMIT_REAL(&held);
      break;
    }

    if (held.kind == SK_LEX_ERROR) {
      EMIT_REAL(&held);
      prev_kind = held.kind;
      prev_byte_end = held.byte_end;
      newline_seen = false;
      held = lex_next(lc);
      continue;
    }

    // Re-examination loop: each `continue` re-evaluates the guards on
    // the SAME held token; each `break` advances to the next raw token.
    for (;;) {
      uint32_t indent =
          col_advance(held.start, (const uint32_t *)line_starts->data,
                      line_starts->count, &line_cur);
      uint32_t layout_col = current.indent;

      // ---- (1) Insert virtual `{` and push layout -------------------
      if (newline_seen && indent > layout_col &&
          !is_expr_continuation(prev_kind, held.kind)) {
        emit_synthetic(out_tokens, SK_VIRTUAL_LBRACE, prev_byte_end);

        assert(frames_count < LAYOUT_MAX_FRAMES &&
               "layout frame stack overflow");
        frames[frames_count++] = current;
        current.indent = indent;
        current.kind = FRAME_LAYOUT;

        prev_kind = SK_VIRTUAL_LBRACE;
        newline_seen = false;
        continue; // re-examine same held token
      }

      // ---- (2) Insert `;` and `}` and pop layout --------------------
      if (newline_seen && indent < layout_col &&
          !(held.kind == SK_RBRACE && current.kind == FRAME_EXPLICIT)) {
        if (should_emit_semicolon(prev_kind)) {
          emit_synthetic(out_tokens, SK_VIRTUAL_SEMI, prev_byte_end);
        }
        emit_synthetic(out_tokens, SK_VIRTUAL_RBRACE, prev_byte_end);

        if (frames_count > 0)
          current = frames[--frames_count];

        prev_kind = SK_VIRTUAL_RBRACE;
        continue; // re-examine same held token
      }

      // ---- (3) Push layout on `{` -----------------------------------
      if (held.kind == SK_LBRACE) {
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
      if (held.kind == SK_RBRACE) {
        while (current.kind == FRAME_LAYOUT && frames_count > 0) {
          emit_synthetic(out_tokens, SK_VIRTUAL_RBRACE, prev_byte_end);
          current = frames[--frames_count];
        }

        if (should_emit_semicolon(prev_kind)) {
          emit_synthetic(out_tokens, SK_VIRTUAL_SEMI, prev_byte_end);
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
          emit_synthetic(out_tokens, SK_VIRTUAL_SEMI, prev_byte_end);
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
#undef FLUSH_TRIVIA
}

#undef INDENT_UNRESOLVED
