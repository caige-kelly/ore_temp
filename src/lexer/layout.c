#include "./layout.h"
#include "token.h"

// =====================================================================
// Synthetic-token construction
// =====================================================================

static struct Token layout_token(enum TokenKind kind, struct Span *span,
                                 StringPool *pool) {
  struct Token t = {.kind = kind,
                    .string_id = pool_intern(pool, "", 0),
                    .string_len = 0,
                    .span = *span,
                    .origin = Layout};
  return t;
}

// Build a zero-width span at the END of `prev`. Used to position synthetic
// LBrace/RBrace/Semicolon tokens just after the previous source token.
static struct Span span_after(struct Span s) {
  s.start = s.end;
  s.line = s.line_end;
  s.column = s.column_end;
  return s;
}

static void emit_synthetic(Vec *out, enum TokenKind kind, struct Span prev_span,
                           StringPool *pool) {
  struct Span s = span_after(prev_span);
  struct Token t = layout_token(kind, &s, pool);
  vec_push(out, &t);
}

// =====================================================================
// Continuation predicates
// =====================================================================

// Binary operators EXCEPT `<` and `>`. The exclusion is deliberate:
// `<` and `>` double as type-angle delimiters, so they need asymmetric
// treatment (Less is only an end-continuation; Greater is only a
// start-continuation). LessEqual/GreaterEqual stay because they're
// unambiguously comparison ops.
static bool is_binary_op_token(enum TokenKind k) {
  switch (k) {
  case Plus: case Minus: case Star: case StarStar:
  case ForwardSlash: case Percent:
  case EqualEqual: case BangEqual:
  case LessEqual: case GreaterEqual:
  case AmpersandAmpersand: case PipePipe:
  case Ampersand: case Pipe: case Caret:
  case ShiftLeft: case ShiftRight:
  case PlusEqual: case MinusEqual: case StarEqual: case ForwardSlashEqual:
  case PercentEqual: case Equal:
  case AmpersandEqual: case PipeEqual: case CaretEqual:
    return true;
  default:
    return false;
  }
}

static bool is_start_continuation(enum TokenKind k) {
  if (is_binary_op_token(k)) return true;
  switch (k) {
  case RParen: case RBracket: case Comma:
  case LBrace: case RBrace:
  case Else: case Elif:
  case RightArrow: case Colon: case DotDot: case ColonEqual:
  case Greater:                    // closer of a multi-line `<...>` angle
    return true;
  default:
    return false;
  }
  // Note: `Less` deliberately excluded — `<` at line start opens a fresh
  // construct (e.g. `<E>` annotation), not a continuation.
}

static bool is_end_continuation(enum TokenKind k) {
  if (is_binary_op_token(k)) return true;
  switch (k) {
  case LParen: case LBracket: case Comma:
  case LBrace: case Dot:
  case Less:                        // opener of a `<...>` angle, line incomplete
    return true;
  default:
    return false;
  }
  // Note: `Greater` deliberately excluded — `>` at line end closes a unit,
  // line is complete.
}

static bool is_expr_continuation(enum TokenKind prev, enum TokenKind cur) {
  return is_start_continuation(cur) || is_end_continuation(prev);
}

// =====================================================================
// Pipeline stage: check_comments
// =====================================================================

void check_comments(Vec *tokens, struct DiagBag *diags) {
  int prev_end_line = 0;
  bool have_comment = false;
  struct Span comment_span = {0};

  for (size_t i = 0; i < tokens->count; i++) {
    struct Token *t = (struct Token *)vec_get(tokens, i);

    if (t->kind == Comment) {
      have_comment = true;
      comment_span = t->span;
      continue;
    }

    if (t->kind == NewLine || t->kind == Space) continue;

    if (have_comment &&
        t->span.line > prev_end_line &&
        t->span.line == comment_span.line_end &&
        comment_span.column_end > 1) {
      if (diags) {
        diag_error(diags, comment_span,
                   "comments cannot be placed in the indentation of a line");
      }
    }

    prev_end_line = t->span.line_end;
  }
}

// =====================================================================
// Pipeline stage: remove_whitespace / remove_comments
// =====================================================================

Vec *remove_whitespace(Vec *tokens, Arena *arena) {
  Vec *out = vec_new_in(arena, sizeof(struct Token));
  for (size_t i = 0; i < tokens->count; i++) {
    struct Token *t = (struct Token *)vec_get(tokens, i);
    if (t->kind == Space || t->kind == NewLine) continue;
    vec_push(out, t);
  }
  return out;
}

Vec *remove_comments(Vec *tokens, Arena *arena) {
  Vec *out = vec_new_in(arena, sizeof(struct Token));
  for (size_t i = 0; i < tokens->count; i++) {
    struct Token *t = (struct Token *)vec_get(tokens, i);
    if (t->kind == Comment) continue;
    vec_push(out, t);
  }
  return out;
}

// =====================================================================
// Pipeline stage: indent_layout
// =====================================================================

static bool should_emit_semicolon(enum TokenKind prev) {
    switch (prev) {
        case LBrace:
        case Semicolon:
        case Eof:
            return false;
        default:
            return true;
    }
}

static Vec *indent_layout(Vec *tokens, StringPool *pool, Arena *arena,
                          struct DiagBag *diags) {
  Vec *output = vec_new_in(arena, sizeof(struct Token));
  Vec *frames = vec_new_in(arena, sizeof(struct LayoutFrame));

  if (tokens->count == 0) return output;

  // Initial layout: column 1, root kind. No outer frames stacked.
  struct LayoutFrame current = {.indent = 1, .kind = framekind_Root};

  // Synthetic prev placed at the first token's start position. Ensures
  // newline = false on iteration 0 (so the first token never triggers
  // guard 1 / 2 / 5).
  struct Token *first = (struct Token *)vec_get(tokens, 0);
  struct Token prev = {.kind = Eof, .span = first->span};
  prev.span.end = prev.span.start;
  prev.span.line_end = prev.span.line;
  prev.span.column_end = prev.span.column;

  size_t i = 0;
  while (i < tokens->count) {
    struct Token *cur = (struct Token *)vec_get(tokens, i);

    // Eof: stop, close layouts below.
    if (cur->kind == Eof) break;

    // Errors pass through unchanged; the lexer already diagnosed them.
    if (cur->kind == Error) {
      vec_push(output, cur);
      prev = *cur;
      i++;
      continue;
    }

    bool newline = prev.span.line_end < cur->span.line;
    int  indent  = cur->span.column;
    int  layoutCol = (int)current.indent;

    int nextIndent = 1;
    if (i + 1 < tokens->count) {
      struct Token *next = (struct Token *)vec_get(tokens, i + 1);
      nextIndent = next->span.column;
    }

    // ---- (1) Insert `{` and push layout ---------------------------
    if (newline && indent > layoutCol &&
        !is_expr_continuation(prev.kind, cur->kind)) {
      emit_synthetic(output, LBrace, prev.span, pool);
      vec_push(frames, &current);
      current.indent = (size_t)indent;
      current.kind   = framekind_Layout;
      // prev = the synthetic LBrace we just emitted, with span at
      // after-of-original-prev. LBrace is an end-continuation, so the
      // next iteration won't fire guard 5 between `{` and cur.
      struct Span synth_span = span_after(prev.span);
      prev.kind = LBrace;
      prev.span = synth_span;
      prev.origin = Layout;
      continue;                       // re-examine same cur
    }

    // ---- (2) Insert `;` and `}` and pop layout --------------------
    if (newline && indent < layoutCol &&
        !(cur->kind == RBrace && current.kind == framekind_Explicit)) {
      if (should_emit_semicolon(prev.kind)) {
        emit_synthetic(output, Semicolon, prev.span, pool);
      }
      
      emit_synthetic(output, RBrace, prev.span, pool);

      if (current.kind == framekind_Explicit && diags) {
        diag_error(diags, cur->span,
                   "layout: explicit '{' is matched by implicit '}' "
                   "due to dedent");
      }
      
      if (frames->count > 0) {
        struct LayoutFrame *outer =
            (struct LayoutFrame *)vec_get(frames, frames->count - 1);
        current = *outer;
        frames->count--;
      }

      struct Span synth_span = span_after(prev.span);
      prev.kind = RBrace;
      prev.span = synth_span;
      prev.origin = Layout;
      continue;                       // re-examine same cur
    }

    // ---- (3) Push layout on `{` -----------------------------------
    if (cur->kind == LBrace) {
      vec_push(output, cur);
      vec_push(frames, &current);
      current.indent = (size_t)nextIndent;
      current.kind = (cur->origin == Layout) ? framekind_Layout
                                              : framekind_Explicit;
      if (nextIndent <= layoutCol) {
        if (diags) {
          diag_error(diags, cur->span,
                     "layout: line must be indented more than the "
                     "enclosing layout context (column %d)", layoutCol);
        }
      }
      prev = *cur;
      i++;
      continue;
    }

    // ---- (4) Pop layout on `}` ------------------------------------
    if (cur->kind == RBrace) {
      while (current.kind == framekind_Layout && frames->count > 0) {
        emit_synthetic(output, RBrace, prev.span, pool);
        struct LayoutFrame *outer =
            (struct LayoutFrame *)vec_get(frames, frames->count - 1);
        current = *outer;
        frames->count--;
      }
      
      if (should_emit_semicolon(prev.kind)) {
        emit_synthetic(output, Semicolon, prev.span, pool);
      }
      vec_push(output, cur);

      if (frames->count > 0) {
        struct LayoutFrame *outer =
            (struct LayoutFrame *)vec_get(frames, frames->count - 1);
        current = *outer;
        frames->count--;
      } else {
        if (diags) {
          diag_error(diags, cur->span, "unmatched closing brace '}'");
        }
      }
      prev = *cur;
      i++;
      continue;
    }

    // ---- (5) Insert `;` between siblings --------------------------
    if (newline && indent == layoutCol &&
        !is_expr_continuation(prev.kind, cur->kind)) {
      if (should_emit_semicolon(prev.kind)) {
        emit_synthetic(output, Semicolon, prev.span, pool);
      }
      vec_push(output, cur);
      prev = *cur;
      i++;
      continue;
    }

    // ---- (6) Pass through -----------------------------------------
    vec_push(output, cur);
    prev = *cur;
    i++;
  }

  // EOF: close all open layouts.
  if (should_emit_semicolon(prev.kind)) {
    emit_synthetic(output, Semicolon, prev.span, pool);
  }

  while (frames->count > 0) {
    emit_synthetic(output, RBrace, prev.span, pool);
    emit_synthetic(output, Semicolon, prev.span, pool);

    if (current.kind == framekind_Explicit && diags) {
      diag_error(diags, prev.span,
                 "layout: explicit '{' matched by implicit '}' "
                 "at end of input");
    }
    struct LayoutFrame *outer =
        (struct LayoutFrame *)vec_get(frames, frames->count - 1);
    current = *outer;
    frames->count--;
  }

  // Append the actual Eof token (it's the last input we skipped over).
  if (tokens->count > 0) {
    struct Token *last = (struct Token *)vec_get(tokens, tokens->count - 1);
    if (last->kind == Eof) {
      vec_push(output, last);
    }
  }

  return output;
}

// =====================================================================
// Pipeline driver
// =====================================================================

Vec *normalizer_in(Vec *tokens, StringPool *pool, Arena *arena,
                   struct DiagBag *diags) {
  check_comments(tokens, diags);
  Vec *no_ws   = remove_whitespace(tokens, arena);
  Vec *no_cmts = remove_comments(no_ws, arena);
  return indent_layout(no_cmts, pool, arena, diags);
}
