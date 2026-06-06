#include "./parser.h"

// Forward declarations for the per-area parsers. Bodies live in
// parse_decl.c / parse_stmt.c / parse_expr.c.
void parse_top_level_decls(Parser *p);

// =====================================================================
// Cursor primitives
// =====================================================================
//
// INVARIANT: after every mutating cursor call returns, p->pos points
// at a non-trivia token, or one-past-the-end. The skip_trivia() helper
// emits any pending trivia tokens to the green builder as it advances.
// Lookahead helpers (p_peek_at) walk over trivia without emitting.
//
// Trivia is therefore a child of whichever node is currently open in
// the GreenBuilder at the moment the trivia is consumed — leading
// trivia of token T ends up inside the node that owns T. Initial
// trivia is flushed once in parse_file_green after SOURCE_FILE opens.

// Advance past any run of trivia at p->pos, emitting each token to
// the green builder. Restores the cursor invariant.
static void skip_trivia(Parser *p) {
  const Token *toks = (const Token *)p->tokens->data;
  while (p->pos < p->tokens->count) {
    const Token *t = &toks[p->pos];
    if (!token_is_trivia(t->kind))
      return;
    if (p->gb) {
      uint32_t len = token_len(t);
      const char *text = p->source ? p->source + t->start : "";
      green_builder_token(p->gb, t->kind, text, len);
    }
    p->pos++;
  }
}

bool p_is_eof(const Parser *p) { return p_peek(p) == SK_EOF; }

const Token *p_current(const Parser *p) {
  // Cursor invariant: pos is at non-trivia or one-past-end.
  const Token *toks = (const Token *)p->tokens->data;
  if (p->pos >= p->tokens->count) {
    if (p->tokens->count == 0)
      return NULL;
    return &toks[p->tokens->count - 1];
  }
  return &toks[p->pos];
}

SyntaxKind p_peek(const Parser *p) {
  const Token *t = p_current(p);
  return t ? t->kind : SK_EOF;
}

// Walk `offset` non-trivia tokens forward from pos without emitting.
// pos itself is non-trivia by invariant, so offset==0 returns pos.
// Returns the index of the offset-th non-trivia, or tokens->count if past end.
static uint32_t p_logical_index(const Parser *p, uint32_t offset) {
  const Token *toks = (const Token *)p->tokens->data;
  uint32_t i = p->pos;
  while (offset > 0 && i < p->tokens->count) {
    i++;
    while (i < p->tokens->count && token_is_trivia(toks[i].kind))
      i++;
    offset--;
  }
  return i;
}

SyntaxKind p_peek_at(const Parser *p, uint32_t offset) {
  const Token *toks = (const Token *)p->tokens->data;
  uint32_t i = p_logical_index(p, offset);
  if (i >= p->tokens->count) {
    if (p->tokens->count == 0)
      return SK_EOF;
    return toks[p->tokens->count - 1].kind;
  }
  return toks[i].kind;
}

const Token *p_token_at(const Parser *p, uint32_t offset) {
  const Token *toks = (const Token *)p->tokens->data;
  uint32_t i = p_logical_index(p, offset);
  if (i >= p->tokens->count)
    return NULL;
  return &toks[i];
}

const Token *p_advance(Parser *p) {
  const Token *t = p_current(p);
  if (p_is_eof(p))
    return t;

  // By invariant, pos is at non-trivia. Emit it to the builder.
  // Virtual layout tokens (SK_VIRTUAL_LBRACE/RBRACE/SEMI) have zero
  // text width; they emit with text_len == 0 (token_len returns 0
  // because start == byte_end for virtuals).
  if (t && p->gb) {
    uint32_t len = token_len(t);
    const char *text = p->source ? p->source + t->start : "";
    green_builder_token(p->gb, t->kind, text, len);
  }

  p->pos++;
  // Restore invariant: emit any trivia immediately following the
  // just-consumed token as children of the currently-open node.
  skip_trivia(p);
  return t;
}

// The brace/semi pairs are the ONLY places where a kind ask implies
// "accept the virtual sibling too". Centralized here so both p_match
// and p_consume share one rule.
static inline bool kind_matches_with_virtual(SyntaxKind asked, SyntaxKind cur) {
  if (asked == cur)
    return true;
  if (asked == SK_LBRACE && cur == SK_VIRTUAL_LBRACE)
    return true;
  if (asked == SK_RBRACE && cur == SK_VIRTUAL_RBRACE)
    return true;
  if (asked == SK_SEMI && cur == SK_VIRTUAL_SEMI)
    return true;
  return false;
}

bool p_match(Parser *p, SyntaxKind kind) {
  if (kind_matches_with_virtual(kind, p_peek(p))) {
    p_advance(p);
    return true;
  }
  return false;
}

const Token *p_consume(Parser *p, SyntaxKind kind, const char *err_msg) {
  if (kind_matches_with_virtual(kind, p_peek(p))) {
    return p_advance(p);
  }
  p_error(p, err_msg);
  return NULL;
}

bool p_check(const Parser *p, SyntaxKind kind) {
  return kind_matches_with_virtual(kind, p_peek(p));
}

void p_error(Parser *p, const char *msg) {
  ParseError e = {.tok_pos = p->pos, .msg = msg};
  vec_push(&p->errors, &e);
}

// Global ceiling on tokens consumed by recovery across one parse. The
// per-call forced-advance already guarantees termination; this only stops
// adversarial input from re-diagnosing forever (past it, recovery goes
// silent — see p_recover).
#define PARSE_FIX_CAP 1000

// True if the cursor sits on a token that terminates a recovery skip for
// `sync`. The SEMI/RBRACE bits go through p_check so they also stop on the
// layout-emitted virtual boundaries (handles continuation-suppressed semis).
static bool at_sync(const Parser *p, SyncSet sync) {
  if ((sync & SYNC_SEMI) && p_check(p, SK_SEMI))
    return true;
  if ((sync & SYNC_RBRACE) && p_check(p, SK_RBRACE))
    return true;
  SyntaxKind k = p_peek(p);
  if ((sync & SYNC_RPAREN) && k == SK_RPAREN)
    return true;
  if ((sync & SYNC_RBRACKET) && k == SK_RBRACKET)
    return true;
  if ((sync & SYNC_GT) && k == SK_GT)
    return true;
  if ((sync & SYNC_COMMA) && k == SK_COMMA)
    return true;
  if ((sync & SYNC_PIPE) && k == SK_PIPE)
    return true;
  return false;
}

void p_recover(Parser *p, SyncSet sync, const char *msg) {
  // One diagnostic at the first bad token, before advancing. Past the cap,
  // stay silent (recovery still runs, just no new diag).
  if (p->fix_count < PARSE_FIX_CAP)
    p_error(p, msg);

  p_start_node(p, SK_ERROR_NODE);
  uint32_t start = p->pos;
  // Skip the unexpected run up to (not including) the first sync token / EOF.
  // Each skipped token flows into the open SK_ERROR_NODE (lossless).
  while (!p_is_eof(p) && !at_sync(p, sync) && p->fix_count < PARSE_FIX_CAP) {
    p_advance(p);
    p->fix_count++;
  }
  // Guaranteed progress + non-empty node: if we stopped immediately because
  // the stuck token IS a sync token (e.g. a stray separator), wrap exactly
  // that one token so the caller's loop can't spin.
  if (p->pos == start && !p_is_eof(p)) {
    p_advance(p);
    p->fix_count++;
  }
  p_finish_node(p);
}

// =====================================================================
// Public entry point
// =====================================================================

// Pre-intern every contextual keyword into ParserKws so contextual-
// keyword checks are a single u32 compare against `t->string_id.idx`.
// Helper for parse_file_green; trivially expressible as one macro line
// per keyword. Order doesn't matter — StringPool dedups.
static void init_parser_kws(Parser *p) {
#define KW(field, lit) p->kws.field = pool_intern(p->pool, (lit), sizeof(lit) - 1)
  KW(named,      "named");
  KW(scoped,     "scoped");
  KW(pub,        "pub");
  KW(pvt,        "pvt");
  KW(abstract_,  "abstract");
  KW(distinct_,  "distinct");
  KW(linear,     "linear");
  KW(ctl,        "ctl");
  KW(val,        "val");
  KW(final_,     "final");
  KW(final_ctl,  "final-ctl");
  KW(behind,     "behind");
  KW(in_,        "in");
#undef KW
}

GreenNode *parse_file_green(const Vec *tokens, const char *source,
                            StringPool *pool, NodeCache *cache,
                            Vec *out_errors) {
  Parser p = {
      .gb = green_builder_new(cache),
      .tokens = tokens,
      .source = source,
      .pool = pool,
      .pos = 0,
      .parsing_type = false,
      .fix_count = 0,
  };
  init_parser_kws(&p);
  p.errors = *out_errors; // caller initialized; we operate in place

  p_start_node(&p, SK_SOURCE_FILE);
  // Bootstrap the cursor invariant: pos may start on trivia (leading
  // file comment, blank lines). Emit it as a child of SOURCE_FILE
  // before any real token is consumed.
  skip_trivia(&p);
  parse_top_level_decls(&p);
  // Flush trailing trivia at EOF (final newline, comment-at-EOF) so
  // it lands inside SOURCE_FILE rather than being silently dropped.
  skip_trivia(&p);
  p_finish_node(&p);

  GreenNode *root = green_builder_finish(p.gb);
  green_builder_destroy(p.gb);

  // Move the errors back to the caller's slot. The Vec is value-typed
  // (data + count + cap), so this is safe — no aliasing concerns.
  *out_errors = p.errors;

  return root;
}
