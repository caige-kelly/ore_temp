#include "./layout.h"
#include "token.h"
#include <stdlib.h>
#include <stdio.h>

struct LayoutNormalizer normalizer_new_in(Vec *tokens, Arena *arena) {
  // The compiler always supplies an arena; the prior heap fallback was
  // dead code (and the `normalizer()` wrapper that consumed it was
  // unreferenced). Requiring an arena keeps lifetime ownership obvious.
  Vec *output_vec = vec_new_in(arena, sizeof(struct Token));
  Vec *frames_vec = vec_new_in(arena, sizeof(struct LayoutFrame));

  struct LayoutNormalizer ln = {
      .tokens = tokens, // The input token stream.
      .current = 0,
      .output = output_vec, // The output token stream.
      .frames = frames_vec, // The stack of layout frames.
      .expecting_brace_body = false,
      .line_last_sig = Eof, // A safe default, any non-significant token works.
      .current_line_indent = 0,
      .delimiter_depth = 0,
      .brace_frame_depths = 0,
      .at_line_start = true,
  };

  return ln;
}

static enum TokenKind peek_next_kind(struct LayoutNormalizer *ln) {
  struct Token *next = (struct Token *)vec_get(ln->tokens, ln->current + 1);
  return next ? next->kind : Eof;
}

static struct Token layout_token(enum TokenKind kind, struct Span *span,
                                 StringPool *pool) {
  struct Token t = {.kind = kind,
                    .string_id = pool_intern(pool, "", 0),
                    .string_len = 0,
                    .span = *span,
                    .origin = Layout};
  return t;
}

// Does this token at the START of a line suppress layout?
static bool is_start_continuation(enum TokenKind kind, enum TokenKind next) {

  switch (kind) {
    case RParen:
    case Greater:
    case RBracket:
    case Comma:
    case LBrace:
    case RBrace:
    case Else:
    case Elif:
    case RightArrow:
    case Equal:
    case Pipe:
    case Colon:
    case Dot:
    case DotDot:
    case ColonEqual:
    case !Less:
    return true;
    default: return false;
  }
}

// Does this token at the END of a line suppress layout?
static bool is_end_continuation(enum TokenKind kind) {
  switch (kind) {
    case LParen:
    case Less:
    case LBracket:
    case Comma:
    case LBrace:
    case Dot:
    return true;
    default: return false;
  }
}

static void insert_semicolon(struct LayoutNormalizer *ln,
                                   struct Token *token, StringPool *pool) {

  // Emit synthetic semicolon
  struct Token semi = layout_token(Semicolon, &token->span, pool);
  vec_push(ln->output, &semi);
}

static void handle_line_start(struct LayoutNormalizer *ln, struct Token *token, StringPool *pool) {
  size_t spaces = token->span.column - 1;
  ln->at_line_start = false;
  ln->current_line_indent = spaces;

  // Get current indentation level from top frame
  struct LayoutFrame *top =
      (struct LayoutFrame *)vec_get(ln->frames, ln->frames->count - 1);
  size_t current_level = top ? top->indent : 0;

  if (spaces > current_level) {
    // Indent increase
    if (ln->expecting_brace_body) {
      // Explicit { on previous line — push Explicit frame
      struct LayoutFrame frame = {.indent = spaces, .kind = framekind_Explicit};
      vec_push(ln->frames, &frame);
      ln->expecting_brace_body = false;
    } else if (is_end_continuation(ln->line_last_sig) ||
               is_start_continuation(token->kind, peek_next_kind(ln))) {
      // Continuation — do nothing
    } else {
      // New layout block
      struct LayoutFrame frame = {.indent = spaces, .kind = framekind_Layout};
      vec_push(ln->frames, &frame);
      struct Token lbrace = layout_token(LBrace, &token->span, pool);
      vec_push(ln->output, &lbrace);
    }
  } else if (spaces < current_level) {
    // Dedent — close frames
    ln->expecting_brace_body = false;
    while (ln->frames->count > 1) {
      struct LayoutFrame *f =
          (struct LayoutFrame *)vec_get(ln->frames, ln->frames->count - 1);
      if (f->indent <= spaces)
        break;
      // Pop frame, emit } if it was a Layout frame
      if (f->kind == framekind_Layout) {
        struct Token rbrace = layout_token(RBrace, &token->span, pool);
        vec_push(ln->output, &rbrace);
      }
      ln->frames->count--; // pop
    }
    insert_semicolon(ln, token, pool);
  } 

  // Reset for new line
  ln->line_last_sig = Eof;
}

static void handle_open_brace(struct LayoutNormalizer *ln,
                              struct Token *token) {
  // Remember how deep the frame stack is, so close_brace
  // knows how many layout frames to unwind
  vec_push(ln->output, token);

  // Check if next token is a NewLine (brace at end of line)
  struct Token *next = (struct Token *)vec_get(ln->tokens, ln->current);
  if (next && next->kind == NewLine) {
    ln->expecting_brace_body = true;
  }
  // Store current frame depth for this brace
  ln->brace_frame_depths = ln->frames->count;
}

static void handle_close_brace(struct LayoutNormalizer *ln, struct Token *token,
                               StringPool *pool) {
  // Close any layout frames opened inside this brace pair
  size_t target_depth = ln->brace_frame_depths;
  while (ln->frames->count > target_depth) {
    struct LayoutFrame *f =
        (struct LayoutFrame *)vec_get(ln->frames, ln->frames->count - 1);
    if (f->kind == framekind_Layout) {
      struct Token rbrace = layout_token(RBrace, &token->span, pool);
      vec_push(ln->output, &rbrace);
    }
    ln->frames->count--;
  }
  ln->expecting_brace_body = false;
  vec_push(ln->output, token);
}

static Vec *normalizer_run(struct LayoutNormalizer *ln, StringPool *pool) {

  // Push root frame
  struct LayoutFrame root_frame = {.indent = 0, .kind = framekind_Root};
  vec_push(ln->frames, &root_frame);

  while (ln->current < ln->tokens->count) {
    struct Token *token = (struct Token *)vec_get(ln->tokens, ln->current);
    if (token->kind == Eof)
      break;

    handle_line_start(ln, token, pool);

    // Consume the token
    ln->current++;

    switch (token->kind) {
    case NewLine:
      ln->at_line_start = true;
      // Don't emit newlines — they're consumed by layout
      break;
    case LParen:
    case LBracket:
      vec_push(ln->output, token);
      ln->delimiter_depth++;
      break;
    case RParen:
    case RBracket:
      if (ln->delimiter_depth > 0)
        ln->delimiter_depth--;
      vec_push(ln->output, token);
      break;
    case LBrace:
      handle_open_brace(ln, token);
      break;
    case RBrace:
      handle_close_brace(ln, token, pool);
      break;
    default:
      vec_push(ln->output, token);
      break;
    }

    // Track last significant token for continuation checks
    if (token->kind != NewLine) {
      ln->line_last_sig = token->kind;
    }
  }

  // Close all remaining frames
  struct Token *eof = (struct Token *)vec_get(ln->tokens, ln->current);
  while (ln->frames->count > 1) {
    struct LayoutFrame *f =
        (struct LayoutFrame *)vec_get(ln->frames, ln->frames->count - 1);
    if (f->kind == framekind_Layout) {
      struct Token rbrace = layout_token(RBrace, &eof->span, pool);
      vec_push(ln->output, &rbrace);
    }
    ln->frames->count--;
  }
  vec_push(ln->output, eof);

  return ln->output;
}

// Translation of Koka's checkComments.
//
// Flags exactly one pattern: a comment that ENDS mid-line, with new code
// starting on that same line. For example:
//
//     val a = 1
//        /* foo */ val b = 2     // <-- comment in the indentation
//
// Does NOT flag standalone indented comments between code lines:
//
//     val a = 1
//        # ordinary indented documentation
//     val b = 2                  // <-- fine, comment is on its own line
//
// Errors flow through diag_error (non-fatal); pass continues.


void check_comments(Vec *tokens) {
  int prev_end_line = 0;            // end line of the last code token seen
  bool have_comment = false;        // do we have a tracked comment?
  struct Span comment_span = {0};   // span of the most recent non-doc comment

  for (size_t i = 0; i < tokens->count; i++) {
    struct Token *t = (struct Token *)vec_get(tokens, i);

    if (t->kind == Comment) {
      have_comment = true;
      comment_span = t->span;
      continue;
    }

    if (t->kind == NewLine) {
      // Whitespace passes through without touching either piece of state.
      continue;
    }

    // Real code token — apply the rule, then advance prev_end_line.
    if (have_comment &&
        t->span.line > prev_end_line &&             // (a) on a NEW line
        t->span.line == comment_span.end &&    // (b) same line as comment's end
        comment_span.column > 1) {              // (c) comment ended past col 1
        printf("comment error");
        exit(1);
    }

    prev_end_line = t->span.line;
  }
}

Vec *normalizer_in(Vec *tokens, StringPool *pool, Arena *arena) {
  struct LayoutNormalizer ln = normalizer_new_in(tokens, arena);
  check_comments(tokens);
  return normalizer_run(&ln, pool);
}
