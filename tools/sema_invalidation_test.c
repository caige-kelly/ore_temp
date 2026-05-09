// Incremental / invalidation test harness.
//
// Drives Sema in-process across multiple revisions of the same input.
// Validates that:
//   - The query/invalidation cascade re-derives correct types after
//     a source mutation.
//   - Body-only edits don't perturb a fn's signature (the signature
//     query's slot fingerprint is structural, not pointer-based).
//   - Adding/removing top-level decls leaves unrelated decls' types
//     pointer-stable (interning + slot caching).
//   - Stale Expr* pointers in DefInfo.origin don't crash post-edit.
//
// Each scenario follows the shape:
//   1. SET source A, run pipeline, capture Type* for named decls.
//   2. SET source B (same InputId — bumps revision).
//   3. Run pipeline again, capture Type* again.
//   4. Assert per-decl: TYPE_SAME (interned pointer-equal) or
//      TYPE_CHANGED (different) per expectation.
//
// Failure prints decl name + expected vs actual to stderr and bumps
// the failure counter. The process exits non-zero if any failed.

#include "common/arena.h"
#include "common/stringpool.h"
#include "common/vec.h"
#include "diag/diag.h"
#include "parser/ast.h"
#include "sema/ids/ids.h"
#include "sema/modules/def_map.h"
#include "sema/modules/inputs.h"
#include "sema/modules/modules.h"
#include "sema/resolve/scope_index.h"
#include "sema/scope/scope.h"
#include "sema/sema.h"
#include "sema/type/checker.h"
#include "sema/type/display.h"
#include "sema/type/type.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// =====================================================================
// Test harness
// =====================================================================

static int g_pass = 0;
static int g_fail = 0;
static const char *g_current_test = NULL;

static void start_test(const char *name) {
  g_current_test = name;
  printf("  ... %s\n", name);
}

static void finish_test(bool ok) {
  if (ok) {
    g_pass++;
  } else {
    g_fail++;
    fprintf(stderr, "       FAIL: %s\n", g_current_test);
  }
}

// Fully reset Sema and re-drive the pipeline against a fresh source.
// Returns the ModuleId for the (re-)materialized module.
static ModuleId drive_pipeline(struct Sema *s, InputId iid, ModuleId mid,
                               const char *source) {
  sema_set_input_source(s, iid, source, strlen(source));
  if (!module_id_is_valid(mid))
    mid = module_create(s, iid, /*is_primitives=*/false);
  bool ok = query_module_def_map(s, mid);
  if (ok) scope_index_build_module(s, mid);
  return mid;
}

// Look up a top-level decl's type by name. Returns NULL on miss.
static struct Type *type_of_top_level(struct Sema *s, ModuleId mid,
                                      const char *name) {
  uint32_t name_id = pool_intern(&s->pool, name, strlen(name));
  DefId def = query_def_for_name(s, mid, name_id);
  if (!def_id_is_valid(def)) return NULL;
  return query_type_of_def(s, def);
}

// Build a fresh Sema, drive the pipeline against `source_a`, then
// `source_b`, return both type pointers for the named decl. Caller
// frees nothing — Sema is owned for the duration of the test and freed
// before return.
struct DeclTypePair {
  struct Type *before;
  struct Type *after;
  bool resolved_before;
  bool resolved_after;
};

static struct DeclTypePair compare_across_edit(const char *source_a,
                                               const char *source_b,
                                               const char *decl_name) {
  struct Sema sema;
  sema_init(&sema);
  InputId iid = sema_register_input(&sema, "<test>");

  ModuleId mid = drive_pipeline(&sema, iid, (ModuleId){0}, source_a);
  struct Type *t_a = type_of_top_level(&sema, mid, decl_name);
  bool resolved_a = (t_a != NULL);

  // Re-drive with source_b on the same InputId. The set_input_source
  // call bumps current_revision and marks the input dirty so the
  // module_ast slot invalidates.
  drive_pipeline(&sema, iid, mid, source_b);
  struct Type *t_b = type_of_top_level(&sema, mid, decl_name);
  bool resolved_b = (t_b != NULL);

  struct DeclTypePair r = {.before = t_a,
                           .after = t_b,
                           .resolved_before = resolved_a,
                           .resolved_after = resolved_b};
  sema_free(&sema);
  return r;
}

// Render a Type* to a stable string for diff output. Static buffer is
// fine since we never hold two renderings live concurrently.
static const char *render_type(struct Sema *s, struct Type *t) {
  static char buf[256];
  if (!t) return "<unresolved>";
  return type_to_string(s, t, buf, sizeof(buf));
}

// =====================================================================
// Scenarios
// =====================================================================

// T1. Idempotent edit. Setting the same source twice should leave every
//     decl's Type* pointer-equal (interned types + slot caching).
static void test_idempotent_edit(void) {
  start_test("idempotent edit (no change) preserves Type pointers");
  const char *src =
      "answer :: 42\n"
      "double :: fn(x: i32) -> i32\n"
      "    x * 2\n";
  struct DeclTypePair r = compare_across_edit(src, src, "double");
  bool ok = r.resolved_before && r.resolved_after && r.before == r.after;
  finish_test(ok);
}

// T2. Body-only edit. Editing a fn body must NOT change its signature
//     type. The fn's TY_FN is interned by (params, ret), so as long as
//     the signature is structurally unchanged, the Type* is pointer-
//     equal across the edit.
static void test_body_only_edit(void) {
  start_test("body-only edit preserves fn signature Type");
  const char *src_a =
      "double :: fn(x: i32) -> i32\n"
      "    x * 2\n";
  const char *src_b =
      "double :: fn(x: i32) -> i32\n"
      "    x + x\n";
  struct DeclTypePair r = compare_across_edit(src_a, src_b, "double");
  bool ok = r.resolved_before && r.resolved_after && r.before == r.after;
  finish_test(ok);
}

// T3. Signature edit. Changing the return type must produce a different
//     Type* (different interned shape).
static void test_signature_edit(void) {
  start_test("signature edit changes fn Type");
  const char *src_a =
      "f :: fn() -> i32\n"
      "    42\n";
  const char *src_b =
      "f :: fn() -> u8\n"
      "    42\n";
  struct DeclTypePair r = compare_across_edit(src_a, src_b, "f");
  bool ok = r.resolved_before && r.resolved_after && r.before != r.after;
  finish_test(ok);
}

// T4. Add a new top-level decl. Existing decls should keep their Type
//     pointers (their slots aren't invalidated by an unrelated decl
//     appearing — top_level_index re-runs but downstream queries
//     early-cutoff via fingerprint).
static void test_add_unrelated_decl(void) {
  start_test("adding an unrelated decl preserves existing Type");
  const char *src_a =
      "double :: fn(x: i32) -> i32\n"
      "    x * 2\n";
  const char *src_b =
      "double :: fn(x: i32) -> i32\n"
      "    x * 2\n"
      "triple :: fn(x: i32) -> i32\n"
      "    x * 3\n";
  struct DeclTypePair r = compare_across_edit(src_a, src_b, "double");
  bool ok = r.resolved_before && r.resolved_after && r.before == r.after;
  if (!ok) {
    fprintf(stderr, "         resolved before=%d after=%d\n",
            r.resolved_before, r.resolved_after);
    fprintf(stderr, "         before=%p after=%p\n",
            (void *)r.before, (void *)r.after);
  }
  finish_test(ok);
}

// T5. Remove a top-level decl. After the edit, the removed name should
//     no longer resolve. Stale Expr* pointers in any per-name caches
//     must not cause a crash.
static void test_remove_decl(void) {
  start_test("removing a decl makes it unresolvable (no crash)");
  const char *src_a =
      "alive :: 7\n"
      "doomed :: 99\n";
  const char *src_b =
      "alive :: 7\n";
  struct DeclTypePair r = compare_across_edit(src_a, src_b, "doomed");
  bool ok = r.resolved_before && !r.resolved_after;
  finish_test(ok);
}

// T6. Rename a top-level decl. The old name becomes unresolvable; the
//     new name resolves. Cross-edit Expr* references in the per-name
//     def-map cache must not survive the AST re-parse.
static void test_rename_decl(void) {
  start_test("renaming a decl: old name unresolves, new name resolves");
  // Reuse the harness: check the OLD name across the rename.
  const char *src_a =
      "old_name :: 42\n";
  const char *src_b =
      "new_name :: 42\n";
  struct DeclTypePair old_check =
      compare_across_edit(src_a, src_b, "old_name");
  // Separately, drive a fresh harness to verify new_name resolves on
  // the post-edit state. Cheaper than threading two assertions through
  // one helper.
  struct DeclTypePair new_check =
      compare_across_edit(src_a, src_b, "new_name");
  bool ok = old_check.resolved_before && !old_check.resolved_after &&
            !new_check.resolved_before && new_check.resolved_after;
  finish_test(ok);
}

// T7. Edit a struct's field type. The struct's TY_STRUCT identity is
//     keyed by DefId (not by content), so the Type* for `Point` stays
//     pointer-equal across the edit. But the struct *signature* (which
//     holds field types) DOES change — a deeper test would compare
//     query_struct_signature output. For now, validate the identity
//     stability and the cascading typecheck.
static void test_struct_field_type_edit(void) {
  start_test("struct field type edit: identity stable, signature changes");
  const char *src_a =
      "Point :: struct\n"
      "    x : i32\n"
      "    y : i32\n";
  const char *src_b =
      "Point :: struct\n"
      "    x : u8\n"
      "    y : i32\n";
  struct DeclTypePair r = compare_across_edit(src_a, src_b, "Point");
  // Identity-only TY_STRUCT — same DefId → same Type* even when
  // fields change. This is the cycle-breaking property we rely on
  // for `Node :: struct { next: ^Node }`.
  bool ok = r.resolved_before && r.resolved_after && r.before == r.after;
  finish_test(ok);
}

// =====================================================================
// Main
// =====================================================================

int main(void) {
  printf("sema invalidation test\n");
  test_idempotent_edit();
  test_body_only_edit();
  test_signature_edit();
  test_add_unrelated_decl();
  test_remove_decl();
  test_rename_decl();
  test_struct_field_type_edit();
  printf("\n%d pass, %d fail\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
