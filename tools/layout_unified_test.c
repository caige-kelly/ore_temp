// Layout single-stream verification test (Phase A.0.5).
//
// Asserts four invariants of the post-A.0.3 layout_stream output:
//
//   1. Source-bytes roundtrip — every byte of the source appears in
//      exactly one (non-virtual, non-EOF) token, in order. Captures:
//      trivia is in the stream, no bytes lost, ordering correct.
//   2. Synthetic-ness invariant — SK_VIRTUAL_* kinds always have
//      zero-width range; non-virtual tokens (excluding EOF) have
//      width >= 1.
//   3. Document-order property — adjacent tokens satisfy
//      t[i].byte_end <= t[i+1].start (with equality at zero-width
//      boundaries: synthetic between two real tokens at the same byte).
//   4. Token struct invariant — sizeof(Token) == 16, runtime-echoed.
//
// Links against src/lexer/{layout,lexer,token}, src/parser/syntax_kind,
// src/support/data_structure/{stringpool,arena,vec}. Standalone — no db.

#include "../src/lexer/layout.h"
#include "../src/lexer/lexer.h"
#include "../src/lexer/token.h"
#include "../src/syntax/syntax_kind.h"
#include "../src/support/data_structure/stringpool.h"
#include "../src/support/data_structure/vec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIE(...)                                                               \
  do {                                                                         \
    fprintf(stderr, "layout_unified_test: " __VA_ARGS__);                      \
    fprintf(stderr, "\n");                                                     \
    exit(1);                                                                   \
  } while (0)

// ---- Test harness --------------------------------------------------

typedef struct {
  Vec        tokens;       // Vec<Token>
  Vec        line_starts;  // Vec<uint32_t>
  StringPool pool;
} LayoutRun;

static void run_layout(LayoutRun *out, const char *src) {
  vec_init(&out->tokens, sizeof(Token));
  vec_init(&out->line_starts, sizeof(uint32_t));
  pool_init(&out->pool, 64);

  LexCursor lc;
  lex_begin(&lc, src, (uint32_t)strlen(src), &out->pool, &out->line_starts);
  layout_stream(&lc, &out->line_starts, &out->tokens);
}

static void free_layout(LayoutRun *r) {
  vec_free(&r->tokens);
  vec_free(&r->line_starts);
  pool_free(&r->pool);
}

// ---- Test 1: Token struct stays 16 bytes (runtime echo) -----------
static void test_token_struct_size(void) {
  if (sizeof(Token) != 16)
    DIE("sizeof(Token) = %zu, want 16", sizeof(Token));
  fprintf(stderr, "  test_token_struct_size: OK\n");
}

// ---- Test 2: Source-bytes roundtrip (simple block) ----------------
//
// "x\n  y\n" — expect (excluding virtuals + EOF):
//   IDENT("x"), NEWLINE("\n"), WHITESPACE("  "), IDENT("y"), NEWLINE("\n")
// concatenating their text recovers the source.
static void test_roundtrip_simple_block(void) {
  LayoutRun r;
  const char *src = "x\n  y\n";
  run_layout(&r, src);

  // Concatenate non-virtual, non-EOF token bytes; verify == src.
  char buf[64] = {0};
  size_t pos = 0;
  for (size_t i = 0; i < r.tokens.count; i++) {
    Token *t = (Token *)vec_get(&r.tokens, i);
    if (t->kind == SK_EOF) continue;
    if (token_is_synthetic(t)) continue;
    uint32_t len = token_len(t);
    if (pos + len >= sizeof(buf)) DIE("buffer overflow");
    memcpy(buf + pos, src + t->start, len);
    pos += len;
  }
  buf[pos] = '\0';

  if (strcmp(buf, src) != 0)
    DIE("roundtrip mismatch: got \"%s\", want \"%s\"", buf, src);
  free_layout(&r);
  fprintf(stderr, "  test_roundtrip_simple_block: OK\n");
}

// ---- Test 3: Synthetic-ness invariants ----------------------------
//
// Indented-block source forces layout to emit virtual braces + semis.
// Verify: every SK_VIRTUAL_* has start == byte_end; every other
// non-EOF token has width >= 1.
static void test_synthetic_invariants(void) {
  LayoutRun r;
  const char *src = "x\n  y\n  z\n";
  run_layout(&r, src);

  bool saw_virtual = false;
  for (size_t i = 0; i < r.tokens.count; i++) {
    Token *t = (Token *)vec_get(&r.tokens, i);
    bool is_v = ore_kind_is_virtual_layout((OreSyntaxKind)t->kind);
    if (is_v) {
      saw_virtual = true;
      if (t->start != t->byte_end)
        DIE("virtual token at idx %zu has non-zero width [%u..%u)", i,
            t->start, t->byte_end);
    } else if (t->kind != SK_EOF) {
      if (t->start >= t->byte_end)
        DIE("non-virtual non-EOF token at idx %zu has zero width", i);
    }
  }
  if (!saw_virtual)
    DIE("expected at least one virtual token in indented-block source");
  free_layout(&r);
  fprintf(stderr, "  test_synthetic_invariants: OK\n");
}

// ---- Test 4: Document order is monotonic --------------------------
//
// For every adjacent pair: t[i].byte_end <= t[i+1].start. Equality
// happens at zero-width boundaries (a virtual sits at the previous
// real token's byte_end == the trivia's start). Strict-less is the
// normal case.
static void test_document_order(void) {
  LayoutRun r;
  const char *src = "a + b\nc * d\n";
  run_layout(&r, src);

  for (size_t i = 1; i < r.tokens.count; i++) {
    Token *prev = (Token *)vec_get(&r.tokens, i - 1);
    Token *cur = (Token *)vec_get(&r.tokens, i);
    if (prev->byte_end > cur->start)
      DIE("out-of-order at idx %zu: prev.byte_end=%u > cur.start=%u",
          i, prev->byte_end, cur->start);
  }
  free_layout(&r);
  fprintf(stderr, "  test_document_order: OK\n");
}

// ---- Test 5: Virtual tokens appear at layout-decided positions ----
//
// Source forces layout to insert at least one VIRTUAL_LBRACE
// (indented block opens implicit block) and one VIRTUAL_SEMI
// (between sibling statements at same indent).
static void test_virtuals_at_expected_positions(void) {
  LayoutRun r;
  const char *src = "f =\n  a\n  b\n";
  run_layout(&r, src);

  bool saw_lbrace = false, saw_semi = false, saw_rbrace = false;
  for (size_t i = 0; i < r.tokens.count; i++) {
    Token *t = (Token *)vec_get(&r.tokens, i);
    if (t->kind == SK_VIRTUAL_LBRACE) saw_lbrace = true;
    if (t->kind == SK_VIRTUAL_SEMI) saw_semi = true;
    if (t->kind == SK_VIRTUAL_RBRACE) saw_rbrace = true;
  }
  if (!saw_lbrace) DIE("expected SK_VIRTUAL_LBRACE for indented block");
  if (!saw_semi) DIE("expected SK_VIRTUAL_SEMI between siblings");
  if (!saw_rbrace) DIE("expected SK_VIRTUAL_RBRACE on dedent / EOF close");
  free_layout(&r);
  fprintf(stderr, "  test_virtuals_at_expected_positions: OK\n");
}

int main(void) {
  fprintf(stderr, "layout_unified_test: starting\n");
  test_token_struct_size();
  test_roundtrip_simple_block();
  test_synthetic_invariants();
  test_document_order();
  test_virtuals_at_expected_positions();

  // Cleanup the global runs from each test — done via auto-free in the
  // helpers above; nothing leaks because each LayoutRun lives only
  // inside its test function.
  fprintf(stderr, "layout_unified_test: all PASS\n");
  return 0;
}
