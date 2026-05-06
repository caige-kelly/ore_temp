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
//
// Mirrors Koka's isStartContinuationToken / isEndContinuationToken
// (Syntax/Layout.hs). Koka's "all operators except `<`/`>`" rule is
// expressed here via a single is_binary_op_token umbrella, with `Less`
// and `Greater` deliberately omitted from the start/end lists.
//
// Why `<`/`>` are special: in Koka and Ore they double as type-angle
// delimiters (effect rows, generics). A `<` at start-of-line should NOT
// continue the previous line — it's beginning a fresh statement that
// happens to start with an angle. Symmetrically, `>` at end-of-line
// shouldn't suppress layout.
// =====================================================================

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
  if (is_binary_op_token(k)) return true;       // all binary ops except `<`
  switch (k) {
  case RParen: case Greater: case RBracket: case Comma:
  case LBrace: case RBrace:
  case Else: case Elif:
  case RightArrow: case Colon: case DotDot: case ColonEqual:
    return true;
  default:
    return false;
  }
}

static bool is_end_continuation(enum TokenKind k) {
  if (is_binary_op_token(k)) return true;       // all binary ops except `>`
  switch (k) {
  case LParen: case Less: case LBracket: case Comma:
  case LBrace: case Dot:
    return true;
  default:
    return false;
  }
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
//
// Direct port of Koka's `brace` (Syntax/Layout.hs). Inserts synthetic
// LBrace/RBrace/Semicolon tokens based on indentation. Five guards in
// priority order:
//
//   (1) newline + indent > layoutCol + !continuation  → emit `{`,
//       push a new layout at this column, re-examine cur.
//   (2) newline + indent < layoutCol + !(cur=`}` matching explicit `{`)
//       → emit `;` (if needed), emit `}`, pop layout, re-examine cur.
//   (3) cur == `{`  → emit cur, push new layout at nextIndent.
//   (4) cur == `}`  → emit `;` (if needed) then cur, pop layout.
//   (5) newline + indent == layoutCol + !continuation  → emit `;`,
//       then cur.
//   (6) otherwise   → emit cur.
//
// Guards (1) and (2) "re-examine" by emitting their synthetic token(s),
// updating the layout state, setting prev to the synthetic, and
// continuing without advancing the input pointer. This matches Koka's
// `insertLCurly prev ++ lexemes` recursion: the next iteration will see
// the same source token but in a new layout context.
// =====================================================================

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
      // Before closing the block, terminate the preceding statement.
      if (prev.kind != LBrace && prev.kind != Semicolon) {
        emit_synthetic(output, Semicolon, prev.span, pool);
      }
      
      // Always emit a synthetic `}` for the popped frame.
      emit_synthetic(output, RBrace, prev.span, pool);

      // If the popped frame was opened by an explicit `{`, the synthetic
      // close means the user's brace pair is now mismatched — flag it.
      if (current.kind == framekind_Explicit && diags) {
        diag_error(diags, cur->span,
                   "layout: explicit '{' is matched by implicit '}' "
                   "due to dedent");
      }
      // Pop.
      if (frames->count > 0) {
        struct LayoutFrame *outer =
            (struct LayoutFrame *)vec_get(frames, frames->count - 1);
        current = *outer;
        frames->count--;
      }
      // Update prev to a synthetic `}` for the re-examination.
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
      // Distinguish synthetic `{` (already from layout) vs source `{`.
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
      // Cascade-close any Layout frames sitting between us and the
      // matching Explicit. This is the same-line analog of guard 2's
      // iteration: a source `}` that arrives without a prior dedent
      // (e.g. closing on the same line as enclosed code) still needs
      // any layout-pushed frames closed first so the brace pairing
      // matches the user's intent.
      while (current.kind == framekind_Layout && frames->count > 0) {
        emit_synthetic(output, RBrace, prev.span, pool);
        struct LayoutFrame *outer =
            (struct LayoutFrame *)vec_get(frames, frames->count - 1);
        current = *outer;
        frames->count--;
      }
      // Now consume the source `}` for the (presumed) Explicit frame.
      if (prev.kind != LBrace && prev.kind != Semicolon) {
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
      if (prev.kind != Semicolon) {
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

  // EOF: close all open layouts. Insert one trailing `;` if needed.
  if (prev.kind != LBrace && prev.kind != Semicolon && prev.kind != Eof) {
    emit_synthetic(output, Semicolon, prev.span, pool);
  }
  while (frames->count > 0) {
    // Always emit a synthetic `}` for the closed frame. If the frame was
    // opened by an explicit `{` that the user never closed, also flag it.
    emit_synthetic(output, RBrace, prev.span, pool);
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
// Pipeline driver — mirrors Koka's `layout` (Syntax/Layout.hs).
// =====================================================================
//
//   1. check_comments        — annotate problematic comment placement
//   2. combine_line_comments — SKIPPED (Koka HTML-doc cosmetic only)
//   3. remove_whitespace     — strip Space + NewLine
//   4. associate_comments    — SKIPPED (Koka doc-string attachment, n/a)
//   5. remove_comments       — strip Comment
//   6. check_ids             — SKIPPED (Koka @-id check, n/a)
//   7. indent_layout         — main layout pass
Vec *normalizer_in(Vec *tokens, StringPool *pool, Arena *arena,
                   struct DiagBag *diags) {
  check_comments(tokens, diags);
  Vec *no_ws   = remove_whitespace(tokens, arena);
  Vec *no_cmts = remove_comments(no_ws, arena);
  return indent_layout(no_cmts, pool, arena, diags);
}
