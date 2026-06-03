// Phase 6 — effect-row representation + helper invariants (keep-zone).
//
// Direct API gate on the intern-pool shape (`IpKey.effect_row.tail`,
// `IpKey.fn_type.effect_row`) plus the public coerce helpers
// (`row_union`, `row_unify`, `instantiate_fn_for_call_site`,
// `ground_unbound_row_vars`). Pure data-structure asserts — no parser,
// no infer_body, no fixtures.

#include "../src/db/db.h"
#include "../src/db/intern_pool/intern_pool.h"
#include "../src/db/query/coerce.h"
#include "../src/db/query/type_layer.h"
#include "../src/support/data_structure/hashmap.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPECT(cond)                                                           \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
      abort();                                                                 \
    }                                                                          \
  } while (0)

static SemaCtx make_ctx(struct db *s, HashMap *row_subst) {
  return (SemaCtx){.s = s,
                   .file_green_root = NULL,
                   .nsid = (NamespaceId){0},
                   .enclosing_fn = DEF_ID_NONE,
                   .file_local = (FileId){0},
                   .types = NULL,
                   .decl_ast_map = NULL,
                   .decl_key = 0,
                   .row_subst = row_subst,
                   .body_effect_row = NULL,
                   .row_name_map = NULL};
}

// --- 1. Empty row identity ------------------------------------------------
static void test_empty_row(struct db *s) {
  IpKey k = ip_key(&s->intern, IP_EMPTY_EFFECT_ROW);
  EXPECT(k.kind == IPK_EFFECT_ROW);
  EXPECT(k.effect_row.n_labels == 0);
  // Empty row's tail self-references — the sentinel pattern.
  EXPECT(k.effect_row.tail.v == IP_EMPTY_EFFECT_ROW.v);
  printf("  ok  empty effect row interns at IP_EMPTY_EFFECT_ROW with self-tail\n");
}

// --- 2. Fn type defaults to pure when effect_row is zero-init -------------
static void test_fn_pure_default(struct db *s) {
  // Designated initializer leaves effect_row unset (.v=0). The intern
  // pool MUST normalize that to IP_EMPTY_EFFECT_ROW so every existing
  // call site stays pure-by-default.
  IpKey k = {.kind = IPK_FN_TYPE};
  k.fn_type.ret = IP_I32_TYPE;
  k.fn_type.modifiers = 0;
  k.fn_type.n_params = 0;
  k.fn_type.params = NULL;
  // k.fn_type.effect_row left zero-init.
  IpIndex t = ip_get(&s->intern, k);
  IpKey r = ip_key(&s->intern, t);
  EXPECT(r.fn_type.effect_row.v == IP_EMPTY_EFFECT_ROW.v);
  printf("  ok  fn_type with zero-init effect_row normalizes to IP_EMPTY_EFFECT_ROW\n");
}

// --- 3. row_union of empty + concrete = concrete --------------------------
static void test_row_union_empty(struct db *s) {
  HashMap subst;
  hashmap_init(&subst);
  SemaCtx ctx = make_ctx(s, &subst);
  DefId labels[1] = {{42}};
  IpIndex r = row_intern(s, labels, 1, IP_EMPTY_EFFECT_ROW);
  IpIndex u1 = row_union(&ctx, IP_EMPTY_EFFECT_ROW, r, NULL);
  EXPECT(u1.v == r.v);
  IpIndex u2 = row_union(&ctx, r, IP_EMPTY_EFFECT_ROW, NULL);
  EXPECT(u2.v == r.v);
  hashmap_free(&subst);
  printf("  ok  row_union with empty returns the other side\n");
}

// --- 4. row_union of two concrete rows MERGES + preserves duplicates ------
static void test_row_union_merge(struct db *s) {
  HashMap subst;
  hashmap_init(&subst);
  SemaCtx ctx = make_ctx(s, &subst);
  DefId a_labels[2] = {{3}, {7}};
  DefId b_labels[2] = {{5}, {7}};
  IpIndex a = row_intern(s, a_labels, 2, IP_EMPTY_EFFECT_ROW);
  IpIndex b = row_intern(s, b_labels, 2, IP_EMPTY_EFFECT_ROW);
  IpIndex u = row_union(&ctx, a, b, NULL);
  IpKey k = ip_key(&s->intern, u);
  EXPECT(k.kind == IPK_EFFECT_ROW);
  // Koka semantics: duplicates allowed; <3,7> ∪ <5,7> = <3,5,7,7>.
  EXPECT(k.effect_row.n_labels == 4);
  EXPECT(k.effect_row.labels[0].idx == 3);
  EXPECT(k.effect_row.labels[1].idx == 5);
  EXPECT(k.effect_row.labels[2].idx == 7);
  EXPECT(k.effect_row.labels[3].idx == 7);
  EXPECT(k.effect_row.tail.v == IP_EMPTY_EFFECT_ROW.v);
  hashmap_free(&subst);
  printf("  ok  row_union merges sorted, preserves duplicates (Koka semantics)\n");
}

// --- 5. row_unify binds row vars ------------------------------------------
static void test_row_unify_binds_var(struct db *s) {
  HashMap subst;
  hashmap_init(&subst);
  SemaCtx ctx = make_ctx(s, &subst);
  IpIndex mu = ip_fresh_row_var(&s->intern);
  DefId labels[1] = {{11}};
  IpIndex closed = row_intern(s, labels, 1, IP_EMPTY_EFFECT_ROW);
  IpIndex open = row_intern(s, NULL, 0, mu); // <..mu>
  EXPECT(row_unify(&ctx, closed, open, NULL));
  // After unify, mu should resolve to <11> (the closed row's labels).
  IpIndex flat = row_flatten(&ctx, open);
  IpKey k = ip_key(&s->intern, flat);
  EXPECT(k.effect_row.n_labels == 1);
  EXPECT(k.effect_row.labels[0].idx == 11);
  EXPECT(k.effect_row.tail.v == IP_EMPTY_EFFECT_ROW.v);
  hashmap_free(&subst);
  printf("  ok  row_unify binds row var to closed labels; row_flatten resolves\n");
}

// --- 6. ground_unbound_row_vars grounds dangling vars to empty ------------
static void test_grounding(struct db *s) {
  HashMap subst;
  hashmap_init(&subst);
  SemaCtx ctx = make_ctx(s, &subst);
  IpIndex mu = ip_fresh_row_var(&s->intern);
  IpIndex open = row_intern(s, NULL, 0, mu); // <..mu>, mu unbound
  ground_unbound_row_vars(&ctx, open);
  IpIndex flat = row_flatten(&ctx, open);
  EXPECT(flat.v == IP_EMPTY_EFFECT_ROW.v);
  hashmap_free(&subst);
  printf("  ok  ground_unbound_row_vars grounds dangling row var → IP_EMPTY\n");
}

// --- 7. instantiate_fn_for_call_site mints fresh row var per call ---------
static void test_instantiate(struct db *s) {
  HashMap subst;
  hashmap_init(&subst);
  (void)make_ctx(s, &subst); // instantiate doesn't need a SemaCtx
  // Build apply :: fn(f: Fn() <..e> i32) <..e> i32.
  IpIndex mu = ip_fresh_row_var(&s->intern);
  IpIndex open_row = row_intern(s, NULL, 0, mu);
  // Inner fn() <..e> i32.
  IpKey ik = {.kind = IPK_FN_TYPE};
  ik.fn_type.ret = IP_I32_TYPE;
  ik.fn_type.modifiers = 0;
  ik.fn_type.n_params = 0;
  ik.fn_type.params = NULL;
  ik.fn_type.effect_row = open_row;
  IpIndex inner_fn = ip_get(&s->intern, ik);
  // Outer fn(inner_fn) <..e> i32 — same mu shared (name-scoping).
  IpIndex params[1] = {inner_fn};
  IpKey ok = {.kind = IPK_FN_TYPE};
  ok.fn_type.ret = IP_I32_TYPE;
  ok.fn_type.modifiers = 0;
  ok.fn_type.n_params = 1;
  ok.fn_type.params = params;
  ok.fn_type.effect_row = open_row;
  IpIndex outer_fn = ip_get(&s->intern, ok);

  // Instantiate twice — each instance should differ from the original
  // and from each other (fresh mu per call).
  IpIndex inst1 = instantiate_fn_for_call_site(s, outer_fn);
  IpIndex inst2 = instantiate_fn_for_call_site(s, outer_fn);
  EXPECT(inst1.v != outer_fn.v);
  EXPECT(inst2.v != outer_fn.v);
  EXPECT(inst1.v != inst2.v);

  // Within a single instance, the SAME fresh mu must appear in both
  // the param's effect_row tail AND the outer's effect_row tail.
  IpKey k1 = ip_key(&s->intern, inst1);
  IpKey k1_inner = ip_key(&s->intern, k1.fn_type.params[0]);
  IpIndex outer_tail = ip_key(&s->intern, k1.fn_type.effect_row).effect_row.tail;
  IpIndex param_tail = ip_key(&s->intern, k1_inner.fn_type.effect_row).effect_row.tail;
  EXPECT(outer_tail.v == param_tail.v);
  hashmap_free(&subst);
  printf("  ok  instantiate_fn_for_call_site: fresh mu per call, name-scope preserved\n");
}

int main(void) {
  printf("[effect_test]\n");
  struct db s;
  db_init(&s);
  test_empty_row(&s);
  test_fn_pure_default(&s);
  test_row_union_empty(&s);
  test_row_union_merge(&s);
  test_row_unify_binds_var(&s);
  test_grounding(&s);
  test_instantiate(&s);
  db_free(&s);
  printf("PASS effect: 7 invariants\n");
  return 0;
}
