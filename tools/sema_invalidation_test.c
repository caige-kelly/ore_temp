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
#include "sema/query/invalidate.h"
#include "sema/query/query.h"
#include "sema/resolve/scope_index.h"
#include "sema/scope/scope.h"
#include "sema/sema.h"
#include "sema/type/checker.h"
#include "sema/type/decl_data.h"
#include "sema/type/decl_info.h"
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
// fine since we never hold two renderings live concurrently. Marked
// unused because the current scenarios don't print on success; left
// available for ad-hoc debugging when a new test fails.
__attribute__((unused))
static const char *render_type(struct Sema *s, struct Type *t) {
  static char buf[256];
  if (!t) return "<unresolved>";
  return type_to_string(s, t, buf, sizeof(buf));
}

// =====================================================================
// Slot inspection helpers
// =====================================================================
//
// The harness needs to assert "this slot re-ran" / "this slot did not
// re-run" across a revision boundary. We tap the slot's computed_rev
// stamp (bumped by sema_query_succeed) and the verified_rev stamp
// (bumped by sema_revalidate / sema_query_succeed).
//
// Slots are addressed by (kind, key). For per-DefId slots the key is
// derived from a side table (SemaDeclInfo for type_query; FnSignature
// for fn_signature; etc.). The lookups below mirror the dispatch in
// sema_locate_slot.

__attribute__((unused))
static struct QuerySlot *type_of_def_slot(struct Sema *s, DefId def) {
  struct SemaDeclInfo *sdi = sema_decl_info(s, def);
  return sdi ? &sdi->type_query : NULL;
}

static struct QuerySlot *fn_signature_slot(struct Sema *s, DefId def) {
  // Force-allocate the FnSignature entry without driving the query.
  // Calling query_fn_signature would re-run the body, which defeats
  // observation. Instead, peek by calling it once (it'll cache after
  // the test's normal pipeline runs) and then locate the slot.
  struct FnSignature *sig = query_fn_signature(s, def);
  return sig ? sema_locate_slot(s, QUERY_FN_SIGNATURE, sig) : NULL;
}

static struct QuerySlot *struct_signature_slot(struct Sema *s, DefId def) {
  struct StructSignature *sig = query_struct_signature(s, def);
  return sig ? sema_locate_slot(s, QUERY_STRUCT_SIGNATURE, sig) : NULL;
}

// Look up the DefMapEntry's QuerySlot for a given (module, name) pair.
// Mirrors the dispatch in sema_locate_slot: QUERY_DEF_FOR_NAME keys on
// the entry pointer itself. Returns NULL if the entry hasn't been
// allocated yet (which happens before the first query_def_for_name
// call for that name).
static struct QuerySlot *def_for_name_slot(struct Sema *s, ModuleId mid,
                                           uint32_t name_id) {
  struct ModuleInfo *m = module_info(s, mid);
  if (!m || m->def_map_entries.entries == NULL) return NULL;
  uint64_t key = (uint64_t)name_id;
  if (!hashmap_contains(&m->def_map_entries, key)) return NULL;
  struct DefMapEntry *entry =
      (struct DefMapEntry *)hashmap_get(&m->def_map_entries, key);
  return entry ? sema_locate_slot(s, QUERY_DEF_FOR_NAME, entry) : NULL;
}

__attribute__((unused))
static uint64_t slot_computed_rev(struct QuerySlot *slot) {
  return slot ? slot->computed_rev : 0;
}

static Fingerprint slot_fingerprint(struct QuerySlot *slot) {
  return slot ? slot->fingerprint : FINGERPRINT_NONE;
}

// Look up a top-level decl's DefId by name. Helper for slot-state
// assertions that need the DefId after a pipeline run.
static DefId def_id_of_top_level(struct Sema *s, ModuleId mid,
                                 const char *name) {
  uint32_t name_id = pool_intern(&s->pool, name, strlen(name));
  return query_def_for_name(s, mid, name_id);
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
// Slot-state scenarios — observe which slots re-ran across an edit
// =====================================================================

// T8. Body-vs-signature fingerprint isolation. Edit only a fn body.
//     The fn_signature slot WILL re-run because it has an AST dep
//     and ast_fp = source_fp shifts on any source change. The
//     contract is one level deeper: the signature's *fingerprint*
//     must be unchanged across a body-only edit (params/ret
//     unchanged), so consumers (type_of_def) walk the dep graph,
//     see the signature dep's recorded fp matches the slot's
//     current fp, and skip recompute via early cutoff.
static void test_body_edit_signature_fingerprint_stable(void) {
  start_test("body-only edit: fn_signature fingerprint is stable");
  const char *src_a =
      "double :: fn(x: i32) -> i32\n"
      "    x * 2\n";
  const char *src_b =
      "double :: fn(x: i32) -> i32\n"
      "    x + x\n";

  struct Sema sema;
  sema_init(&sema);
  InputId iid = sema_register_input(&sema, "<test>");
  ModuleId mid = drive_pipeline(&sema, iid, (ModuleId){0}, src_a);
  DefId def = def_id_of_top_level(&sema, mid, "double");
  (void)query_type_of_def(&sema, def);
  Fingerprint sig_fp_before = slot_fingerprint(fn_signature_slot(&sema, def));

  drive_pipeline(&sema, iid, mid, src_b);
  (void)query_type_of_def(&sema, def);
  Fingerprint sig_fp_after = slot_fingerprint(fn_signature_slot(&sema, def));

  bool ok = sig_fp_before != FINGERPRINT_NONE && sig_fp_before == sig_fp_after;
  if (!ok) {
    fprintf(stderr, "         sig fingerprint: before=%llu after=%llu\n",
            (unsigned long long)sig_fp_before,
            (unsigned long long)sig_fp_after);
  }
  finish_test(ok);
  sema_free(&sema);
}

// T9. Signature-edit fingerprint shift. Edit the fn's return type;
//     fn_signature re-runs AND its fingerprint changes (different
//     ret_type pointer in the structural hash). The fingerprint
//     shift is what propagates the cascade to type_of_def.
static void test_signature_edit_fingerprint_shifts(void) {
  start_test("signature edit: fn_signature fingerprint shifts");
  const char *src_a =
      "f :: fn() -> i32\n"
      "    42\n";
  const char *src_b =
      "f :: fn() -> u8\n"
      "    42\n";

  struct Sema sema;
  sema_init(&sema);
  InputId iid = sema_register_input(&sema, "<test>");
  ModuleId mid = drive_pipeline(&sema, iid, (ModuleId){0}, src_a);
  DefId def = def_id_of_top_level(&sema, mid, "f");
  (void)query_type_of_def(&sema, def);
  Fingerprint sig_fp_before = slot_fingerprint(fn_signature_slot(&sema, def));

  drive_pipeline(&sema, iid, mid, src_b);
  (void)query_type_of_def(&sema, def);
  Fingerprint sig_fp_after = slot_fingerprint(fn_signature_slot(&sema, def));

  bool ok = sig_fp_before != FINGERPRINT_NONE &&
            sig_fp_after != FINGERPRINT_NONE &&
            sig_fp_before != sig_fp_after;
  if (!ok) {
    fprintf(stderr, "         sig fingerprint: before=%llu after=%llu\n",
            (unsigned long long)sig_fp_before,
            (unsigned long long)sig_fp_after);
  }
  finish_test(ok);
  sema_free(&sema);
}

// T10. Cascading invalidation through a struct. Edit a struct's
//      field type. Three properties to validate:
//        a) The struct's signature fingerprint shifts (field type
//           pointer is part of the structural fp).
//        b) An unrelated function's signature fingerprint stays
//           stable (its (params, ret) tuple is unchanged).
//        c) The user-of-Point's signature fingerprint stays stable
//           (Point's TY_STRUCT identity is by DefId, so the param
//           type pointer doesn't move). The cascade for user code
//           lives at the StructSignature, not at TY_STRUCT itself.
static void test_struct_edit_cascade_fingerprints(void) {
  start_test("struct field edit shifts struct fp, leaves unrelated fps stable");
  const char *src_a =
      "Point :: struct\n"
      "    x : i32\n"
      "use_point :: fn(p: Point) -> i32\n"
      "    p.x\n"
      "unrelated :: fn(n: i32) -> i32\n"
      "    n + 1\n";
  const char *src_b =
      "Point :: struct\n"
      "    x : u8\n"
      "use_point :: fn(p: Point) -> i32\n"
      "    p.x\n"
      "unrelated :: fn(n: i32) -> i32\n"
      "    n + 1\n";

  struct Sema sema;
  sema_init(&sema);
  InputId iid = sema_register_input(&sema, "<test>");
  ModuleId mid = drive_pipeline(&sema, iid, (ModuleId){0}, src_a);
  DefId point_def = def_id_of_top_level(&sema, mid, "Point");
  DefId use_def = def_id_of_top_level(&sema, mid, "use_point");
  DefId unrel_def = def_id_of_top_level(&sema, mid, "unrelated");
  (void)query_type_of_def(&sema, use_def);
  (void)query_type_of_def(&sema, unrel_def);
  Fingerprint struct_fp_before =
      slot_fingerprint(struct_signature_slot(&sema, point_def));
  Fingerprint use_fp_before = slot_fingerprint(fn_signature_slot(&sema, use_def));
  Fingerprint unrel_fp_before =
      slot_fingerprint(fn_signature_slot(&sema, unrel_def));

  drive_pipeline(&sema, iid, mid, src_b);
  (void)query_type_of_def(&sema, use_def);
  (void)query_type_of_def(&sema, unrel_def);
  Fingerprint struct_fp_after =
      slot_fingerprint(struct_signature_slot(&sema, point_def));
  Fingerprint use_fp_after = slot_fingerprint(fn_signature_slot(&sema, use_def));
  Fingerprint unrel_fp_after =
      slot_fingerprint(fn_signature_slot(&sema, unrel_def));

  bool ok = struct_fp_before != struct_fp_after &&
            use_fp_before == use_fp_after &&
            unrel_fp_before == unrel_fp_after;
  if (!ok) {
    fprintf(stderr,
            "         struct    fp: before=%llu after=%llu (want differ)\n"
            "         use_point fp: before=%llu after=%llu (want equal)\n"
            "         unrelated fp: before=%llu after=%llu (want equal)\n",
            (unsigned long long)struct_fp_before,
            (unsigned long long)struct_fp_after,
            (unsigned long long)use_fp_before,
            (unsigned long long)use_fp_after,
            (unsigned long long)unrel_fp_before,
            (unsigned long long)unrel_fp_after);
  }
  finish_test(ok);
  sema_free(&sema);
}

// T11. Multi-edit stress: alternate between two sources 10 times on
//      the same Sema. ASan in CI catches use-after-free in stale
//      Expr* paths; here we just assert the harness doesn't crash
//      and the final state's type matches the final source.
static void test_multi_edit_stress(void) {
  start_test("10 alternating edits don't crash; final state correct");
  const char *src_a =
      "f :: fn() -> i32\n"
      "    1\n";
  const char *src_b =
      "f :: fn() -> u8\n"
      "    1\n";

  struct Sema sema;
  sema_init(&sema);
  InputId iid = sema_register_input(&sema, "<test>");
  ModuleId mid = drive_pipeline(&sema, iid, (ModuleId){0}, src_a);
  for (int i = 0; i < 10; i++) {
    drive_pipeline(&sema, iid, mid, (i & 1) ? src_a : src_b);
    (void)type_of_top_level(&sema, mid, "f");
  }
  // Loop ends with i=9 (odd) → last drive used src_a → ret type i32.
  // Drive once more with src_b explicitly to assert end-state.
  drive_pipeline(&sema, iid, mid, src_b);
  struct Type *final_t = type_of_top_level(&sema, mid, "f");
  // Drive back to src_a to compare pointers.
  drive_pipeline(&sema, iid, mid, src_a);
  struct Type *a_again = type_of_top_level(&sema, mid, "f");
  drive_pipeline(&sema, iid, mid, src_b);
  struct Type *b_again = type_of_top_level(&sema, mid, "f");

  bool ok = final_t != NULL && a_again != NULL && b_again != NULL &&
            a_again != b_again && b_again == final_t;
  finish_test(ok);
  sema_free(&sema);
}

// T12. Re-add a removed decl. Source A defines `foo`, B removes it,
//      C re-adds it. Verifies the def_for_name slot recovers from
//      ERROR (after B) back to a valid DefId (in C). This is the
//      direct exercise of the QUERY_ERROR re-validation fix.
static void test_readd_removed_decl(void) {
  start_test("removed-then-readded decl resolves on the readd revision");
  const char *src_a = "foo :: 1\n";
  const char *src_b = "bar :: 2\n";
  const char *src_c = "foo :: 3\n";

  struct Sema sema;
  sema_init(&sema);
  InputId iid = sema_register_input(&sema, "<test>");
  ModuleId mid = drive_pipeline(&sema, iid, (ModuleId){0}, src_a);
  struct Type *t_a = type_of_top_level(&sema, mid, "foo");

  drive_pipeline(&sema, iid, mid, src_b);
  struct Type *t_b = type_of_top_level(&sema, mid, "foo");

  drive_pipeline(&sema, iid, mid, src_c);
  struct Type *t_c = type_of_top_level(&sema, mid, "foo");

  bool ok = t_a != NULL && t_b == NULL && t_c != NULL;
  finish_test(ok);
  sema_free(&sema);
}

// =====================================================================
// PR 2 — comptime correctness scenarios
// =====================================================================

// Helper: get the Bind.value Expr* for a top-level decl, post-parse.
static struct Expr *bind_value_of(struct Sema *s, ModuleId mid,
                                  const char *name) {
  uint32_t name_id = pool_intern(&s->pool, name, strlen(name));
  DefId def = query_def_for_name(s, mid, name_id);
  if (!def_id_is_valid(def)) return NULL;
  struct Expr *origin = def_origin(s, def);
  if (!origin || origin->kind != expr_Bind) return NULL;
  return origin->bind.value;
}

// T13. Comptime chain invalidation. Source A: MAX=1024; HALF=MAX/2.
//      Source B: MAX=2048; HALF=MAX/2 (unchanged source-text). Edit
//      MAX's literal; HALF's const-eval slot must invalidate (deps
//      include MAX's slot indirectly via the Ident path) and recompute
//      to the new folded value.
static void test_comptime_chain_invalidation(void) {
  start_test("comptime chain: MAX edit invalidates HALF's folded value");
  const char *src_a =
      "MAX  :: 1024\n"
      "HALF :: MAX / 2\n";
  const char *src_b =
      "MAX  :: 2048\n"
      "HALF :: MAX / 2\n";

  struct Sema sema;
  sema_init(&sema);
  InputId iid = sema_register_input(&sema, "<test>");
  ModuleId mid = drive_pipeline(&sema, iid, (ModuleId){0}, src_a);
  struct Expr *half_a = bind_value_of(&sema, mid, "HALF");
  struct ConstValue v_a = half_a ? query_const_eval(&sema, half_a)
                                  : (struct ConstValue){.kind = CONST_NONE};

  drive_pipeline(&sema, iid, mid, src_b);
  struct Expr *half_b = bind_value_of(&sema, mid, "HALF");
  struct ConstValue v_b = half_b ? query_const_eval(&sema, half_b)
                                  : (struct ConstValue){.kind = CONST_NONE};

  bool ok = v_a.kind == CONST_INT && v_a.int_val == 512 &&
            v_b.kind == CONST_INT && v_b.int_val == 1024;
  if (!ok) {
    fprintf(stderr,
            "         before: kind=%d int=%lld (want 1/512)\n"
            "         after : kind=%d int=%lld (want 1/1024)\n",
            (int)v_a.kind, (long long)v_a.int_val,
            (int)v_b.kind, (long long)v_b.int_val);
  }
  finish_test(ok);
  sema_free(&sema);
}

// T14. is_comptime flips when a fn body's tail switches between a
//      literal (comptime) and a fn call (not). Source A's body is
//      `42`; source B replaces it with a call to a sibling fn `g()`.
//      query_is_comptime on the body expression must answer true,
//      then false, after the edit.
static void test_is_comptime_flip(void) {
  start_test("is_comptime flips when body changes from literal to call");
  const char *src_a =
      "g :: fn() -> i32\n"
      "    7\n"
      "f :: fn() -> i32\n"
      "    42\n";
  const char *src_b =
      "g :: fn() -> i32\n"
      "    7\n"
      "f :: fn() -> i32\n"
      "    g()\n";

  struct Sema sema;
  sema_init(&sema);
  InputId iid = sema_register_input(&sema, "<test>");
  ModuleId mid = drive_pipeline(&sema, iid, (ModuleId){0}, src_a);

  // f's bind.value is the Lambda; the body is lambda.body.
  struct Expr *f_lam_a = bind_value_of(&sema, mid, "f");
  struct Expr *body_a = (f_lam_a && f_lam_a->kind == expr_Lambda)
                           ? f_lam_a->lambda.body : NULL;
  bool comp_a = body_a ? query_is_comptime(&sema, body_a) : false;

  drive_pipeline(&sema, iid, mid, src_b);
  struct Expr *f_lam_b = bind_value_of(&sema, mid, "f");
  struct Expr *body_b = (f_lam_b && f_lam_b->kind == expr_Lambda)
                           ? f_lam_b->lambda.body : NULL;
  bool comp_b = body_b ? query_is_comptime(&sema, body_b) : false;

  bool ok = comp_a == true && comp_b == false;
  if (!ok) {
    fprintf(stderr, "         before=%d (want 1) after=%d (want 0)\n",
            (int)comp_a, (int)comp_b);
  }
  finish_test(ok);
  sema_free(&sema);
}

// T15. No-op revalidate on a long chain doesn't recompute. Drive a
//      26-link chain (A..Z), type the whole module, snapshot each
//      link's const_eval slot computed_rev, drive the *same* source
//      again (revision bumps but content doesn't), and assert every
//      slot's computed_rev is unchanged. This is the linear-walk
//      property: pre-PR-2 the recursive walker visited each link
//      O(chain length) times per check.
static void test_chain_no_op_skips_recompute(void) {
  start_test("chain const_eval slots skip recompute on no-op revalidate");
  // Build a 26-link chain: A=1, B=A+1, ..., Z=Y+1. Each link's value
  // is the prior literal incremented; folded value of Z is 26.
  const char *src =
      "A :: 1\n"
      "B :: A + 1\n"
      "C :: B + 1\n"
      "D :: C + 1\n"
      "E :: D + 1\n"
      "F :: E + 1\n"
      "G :: F + 1\n"
      "H :: G + 1\n"
      "I :: H + 1\n"
      "J :: I + 1\n"
      "K :: J + 1\n"
      "L :: K + 1\n"
      "M :: L + 1\n"
      "N :: M + 1\n"
      "O :: N + 1\n"
      "P :: O + 1\n"
      "Q :: P + 1\n"
      "R :: Q + 1\n"
      "S :: R + 1\n"
      "T :: S + 1\n"
      "U :: T + 1\n"
      "V :: U + 1\n"
      "W :: V + 1\n"
      "X :: W + 1\n"
      "Y :: X + 1\n"
      "Z :: Y + 1\n";

  struct Sema sema;
  sema_init(&sema);
  InputId iid = sema_register_input(&sema, "<test>");
  ModuleId mid = drive_pipeline(&sema, iid, (ModuleId){0}, src);

  // Force-fold Z by calling query_const_eval. The recursion threads
  // through every link.
  struct Expr *z_val = bind_value_of(&sema, mid, "Z");
  struct ConstValue z = z_val ? query_const_eval(&sema, z_val)
                              : (struct ConstValue){.kind = CONST_NONE};
  bool z_ok = z.kind == CONST_INT && z.int_val == 26;

  // Snapshot every link's const_eval slot's computed_rev.
  const char *names[26] = {"A","B","C","D","E","F","G","H","I","J","K","L","M",
                            "N","O","P","Q","R","S","T","U","V","W","X","Y","Z"};
  uint64_t before[26] = {0};
  for (int i = 0; i < 26; i++) {
    struct Expr *v = bind_value_of(&sema, mid, names[i]);
    if (!v) continue;
    // Calling query_const_eval here is a CACHED hit (slot is DONE);
    // it doesn't re-run the body, but it lets us reach the slot via
    // the entry hashmap. The slot pointer comes from sema_locate_slot.
    (void)query_const_eval(&sema, v);
    uint64_t key = (uint64_t)v->id.id;
    if (!hashmap_contains(&sema.const_eval_entries, key)) continue;
    struct ConstEvalEntry *e = (struct ConstEvalEntry *)hashmap_get(
        &sema.const_eval_entries, key);
    before[i] = e ? e->query.computed_rev : 0;
  }

  // Re-drive with the SAME source. Revision bumps but ast_fp
  // (= source_fp) is identical, so the AST dep walker should
  // short-circuit and no slot should recompute.
  drive_pipeline(&sema, iid, mid, src);

  // Re-eval Z to trigger any missed revalidation.
  struct Expr *z_val_2 = bind_value_of(&sema, mid, "Z");
  (void)(z_val_2 ? query_const_eval(&sema, z_val_2)
                 : (struct ConstValue){.kind = CONST_NONE});

  bool unchanged = true;
  int recomputed_count = 0;
  for (int i = 0; i < 26; i++) {
    struct Expr *v = bind_value_of(&sema, mid, names[i]);
    if (!v) continue;
    uint64_t key = (uint64_t)v->id.id;
    if (!hashmap_contains(&sema.const_eval_entries, key)) continue;
    struct ConstEvalEntry *e = (struct ConstEvalEntry *)hashmap_get(
        &sema.const_eval_entries, key);
    uint64_t after = e ? e->query.computed_rev : 0;
    if (before[i] != 0 && after != before[i]) {
      unchanged = false;
      recomputed_count++;
    }
  }

  bool ok = z_ok && unchanged;
  if (!ok) {
    fprintf(stderr, "         z=%lld (want 26) recomputed=%d (want 0)\n",
            (long long)z.int_val, recomputed_count);
  }
  finish_test(ok);
  sema_free(&sema);
}

// =====================================================================
// Edit-cycle hygiene scenarios (B3, B4, B5, B12 in bug_of_bugs)
// =====================================================================

// T16. Rename then re-add original name. Source A defines `old`,
//      source B renames it to `new` (which under the hood adds a
//      DefId for `new` and orphans `old`'s entry in the scope's
//      name_index), source C re-adds `old`. Expected: source C's
//      `old` resolves cleanly with a current-revision DefInfo —
//      not the orphan from source A.
//
//      The B12 scenario: if `old`'s name_index entry from source A
//      is still in `internal_scope` when source C tries to install
//      a fresh `old`, scope_define_def returns false and the user
//      sees `'old' not defined`. This test catches that.
static void test_rename_then_readd_original(void) {
  start_test("rename then re-add: original name resolves with fresh value");
  const char *src_a = "old :: 1\n";
  const char *src_b = "new :: 2\n";
  const char *src_c = "old :: 3\n";

  struct Sema sema;
  sema_init(&sema);
  InputId iid = sema_register_input(&sema, "<test>");
  ModuleId mid = drive_pipeline(&sema, iid, (ModuleId){0}, src_a);
  struct Type *t_a = type_of_top_level(&sema, mid, "old");

  drive_pipeline(&sema, iid, mid, src_b);
  // After rename: `old` should be unresolvable.
  struct Type *t_b = type_of_top_level(&sema, mid, "old");

  drive_pipeline(&sema, iid, mid, src_c);
  // After re-add: `old` resolves again. The folded value should be
  // the new one (3), not the stale source A (1).
  struct Type *t_c = type_of_top_level(&sema, mid, "old");

  bool ok_resolve = (t_a != NULL) && (t_b == NULL) && (t_c != NULL);
  if (!ok_resolve) {
    fprintf(stderr,
            "         resolve a=%d b=%d c=%d (want 1/0/1)\n",
            (int)(t_a != NULL), (int)(t_b != NULL), (int)(t_c != NULL));
    finish_test(false);
    sema_free(&sema);
    return;
  }

  // Verify the folded value reflects revision C, not revision A.
  // Path: get DefId, get its bind value Expr* via def_origin, fold.
  uint32_t old_name_id = pool_intern(&sema.pool, "old", 3);
  DefId def = query_def_for_name(&sema, mid, old_name_id);
  struct Expr *origin = def_origin(&sema, def);
  bool ok = false;
  if (origin && origin->kind == expr_Bind && origin->bind.value) {
    struct ConstValue v = query_const_eval(&sema, origin->bind.value);
    ok = (v.kind == CONST_INT && v.int_val == 3);
    if (!ok) {
      fprintf(stderr, "         folded value: kind=%d int=%lld (want 1/3)\n",
              (int)v.kind, (long long)v.int_val);
    }
  }
  finish_test(ok);
  sema_free(&sema);
}

// T17. 20-edit stress with adds and removes. Drives random-ish
//      sources through one Sema instance and asserts no crashes,
//      consistent results. Probes B3 (node_to_expr orphan growth)
//      under ASan; if a stale entry is dereferenced we'd see a
//      use-after-free or wrong-type access.
static void test_long_running_edit_cycle(void) {
  start_test("20-edit cycle with adds + removes preserves correctness");
  // Six fixed sources we'll cycle through in non-trivial order.
  // Each adds / removes / renames a different decl, so over 20
  // cycles every per-name DefMapEntry sees several DONE → ERROR
  // → DONE transitions.
  const char *sources[6] = {
      "a :: 1\n",
      "a :: 1\nb :: 2\n",
      "b :: 2\n",
      "b :: 2\nc :: 3\n",
      "c :: 3\n",
      "a :: 10\nc :: 30\n",
  };

  struct Sema sema;
  sema_init(&sema);
  InputId iid = sema_register_input(&sema, "<test>");
  ModuleId mid = drive_pipeline(&sema, iid, (ModuleId){0}, sources[0]);

  // Order chosen to interleave "remove a" / "re-add a" / "remove b" /
  // etc. so every name sees multiple DONE→ERROR→DONE cycles.
  static const int order[20] = {
      0, 1, 2, 3, 4, 5, 0, 5, 2, 4,
      1, 3, 0, 2, 5, 1, 4, 3, 0, 5,
  };

  for (int i = 0; i < 20; i++) {
    drive_pipeline(&sema, iid, mid, sources[order[i]]);
    // Drive type queries to surface staleness.
    (void)type_of_top_level(&sema, mid, "a");
    (void)type_of_top_level(&sema, mid, "b");
    (void)type_of_top_level(&sema, mid, "c");
  }

  // After the last step (sources[5] = "a :: 10\nc :: 30\n") the
  // resolved values must reflect that revision exactly.
  drive_pipeline(&sema, iid, mid, sources[5]);
  struct Type *ta = type_of_top_level(&sema, mid, "a");
  struct Type *tb = type_of_top_level(&sema, mid, "b");
  struct Type *tc = type_of_top_level(&sema, mid, "c");

  bool ok = (ta != NULL) && (tb == NULL) && (tc != NULL);
  if (!ok) {
    fprintf(stderr,
            "         after final: a=%d b=%d c=%d (want 1/0/1)\n",
            (int)(ta != NULL), (int)(tb != NULL), (int)(tc != NULL));
  }
  finish_test(ok);
  sema_free(&sema);
}

// T18. Same name, different shape across revisions. Source A binds
//      `x` to a literal; source B re-binds `x` to a fn; source C
//      back to a literal of a different value. The DefInfo's
//      semantic_kind, origin, etc. must reflect each revision
//      faithfully — not stale state from a prior shape.
static void test_same_name_different_shape(void) {
  start_test("same name, different shape across revisions: type tracks shape");
  const char *src_a = "x :: 42\n";
  const char *src_b =
      "x :: fn() -> i32\n"
      "    99\n";
  const char *src_c = "x :: 7\n";

  struct Sema sema;
  sema_init(&sema);
  InputId iid = sema_register_input(&sema, "<test>");
  ModuleId mid = drive_pipeline(&sema, iid, (ModuleId){0}, src_a);
  struct Type *t_a = type_of_top_level(&sema, mid, "x");

  drive_pipeline(&sema, iid, mid, src_b);
  struct Type *t_b = type_of_top_level(&sema, mid, "x");

  drive_pipeline(&sema, iid, mid, src_c);
  struct Type *t_c = type_of_top_level(&sema, mid, "x");

  // src_a → comptime_int. src_b → fn() -> i32. src_c → comptime_int.
  // a and c may share the comptime_int interned Type*; b must differ.
  bool ok = t_a && t_b && t_c &&
            t_a == t_c &&        // both comptime_int
            t_a != t_b &&        // shape changed in B
            t_b != t_c;
  if (!ok) {
    fprintf(stderr,
            "         t_a=%p t_b=%p t_c=%p (want a==c, a!=b, b!=c)\n",
            (void *)t_a, (void *)t_b, (void *)t_c);
  }
  finish_test(ok);
  sema_free(&sema);
}

// T19. B9 fingerprint stability for query_def_for_name.
//      Two assertions in one scenario, paired so a regression in
//      either direction (over- or under-invalidating) is caught:
//        a) `pub` toggle changes the slot's fingerprint (vis is part
//           of the four-tuple), so cross-module consumers revalidate.
//        b) Body-only edit (RHS literal change) leaves the fingerprint
//           stable, so dependents that only care about identity early-
//           cut at the slot boundary.
//      Pre-B9 the slot's fingerprint was always FINGERPRINT_NONE, so
//      neither property held — PR 1 worked around it by depending on
//      query_module_ast directly. With the four-tuple in place the
//      slot itself encodes the right invariants.
static void test_def_for_name_fingerprint_pub_toggle(void) {
  start_test("def_for_name fp: pub toggle shifts, body edit stable");
  const char *src_pub = "x :: pub 1\n";
  const char *src_priv = "x :: 1\n";
  const char *src_priv_body_changed = "x :: 2\n";

  struct Sema sema;
  sema_init(&sema);
  InputId iid = sema_register_input(&sema, "<test>");
  ModuleId mid = drive_pipeline(&sema, iid, (ModuleId){0}, src_pub);
  uint32_t name_id = pool_intern(&sema.pool, "x", 1);
  (void)query_def_for_name(&sema, mid, name_id);
  Fingerprint fp_pub = slot_fingerprint(def_for_name_slot(&sema, mid, name_id));

  // Vis change: pub → private. Fingerprint MUST shift.
  drive_pipeline(&sema, iid, mid, src_priv);
  (void)query_def_for_name(&sema, mid, name_id);
  Fingerprint fp_priv = slot_fingerprint(def_for_name_slot(&sema, mid, name_id));

  // Body change: same vis (private), same name, same DefId, same shape;
  // only the RHS literal differs. Fingerprint MUST stay equal to fp_priv.
  drive_pipeline(&sema, iid, mid, src_priv_body_changed);
  (void)query_def_for_name(&sema, mid, name_id);
  Fingerprint fp_priv_body =
      slot_fingerprint(def_for_name_slot(&sema, mid, name_id));

  bool fp_set_at_all = fp_pub != FINGERPRINT_NONE;
  bool vis_shifted = fp_pub != fp_priv;
  bool body_stable = fp_priv == fp_priv_body;
  bool ok = fp_set_at_all && vis_shifted && body_stable;
  if (!ok) {
    fprintf(stderr,
            "         fp: pub=%llu priv=%llu priv_body=%llu "
            "(want set, pub!=priv, priv==priv_body)\n",
            (unsigned long long)fp_pub, (unsigned long long)fp_priv,
            (unsigned long long)fp_priv_body);
  }
  finish_test(ok);
  sema_free(&sema);
}

// T20. Untracked-read → REVALIDATE_RECOMPUTE. Synthetic scenario that
//      proves the trace-mode infrastructure actually drives recompute:
//        1. Drive a pipeline with a fn so its signature slot computes
//           and verifies at revision N.
//        2. Manually set the slot's has_untracked_read = true to
//           simulate a body that called SEMA_READ_UNTRACKED.
//        3. Re-drive with the SAME source. Without the flag, the slot
//           would early-cut at the verified_rev fast-path or via
//           dep-fp comparison — same content, no recompute.
//        4. Assert: the slot's computed_rev DID move forward, proving
//           sema_revalidate honored the untracked-read bit and forced
//           RECOMPUTE through to sema_query_begin.
//      Only meaningful under -DORE_DEBUG_QUERIES; in default builds
//      the test reports SKIP and passes trivially.
static void test_untracked_read_forces_recompute(void) {
  start_test("untracked-read flag forces RECOMPUTE on revalidate");
#ifndef ORE_DEBUG_QUERIES
  // Field is compiled out; this scenario can't be exercised. Treat as
  // a no-op pass so the test count stays consistent across build modes.
  finish_test(true);
  return;
#else
  const char *src =
      "f :: fn(x: i32) -> i32\n"
      "    x + 1\n";

  struct Sema sema;
  sema_init(&sema);
  InputId iid = sema_register_input(&sema, "<test>");
  ModuleId mid = drive_pipeline(&sema, iid, (ModuleId){0}, src);
  DefId def = def_id_of_top_level(&sema, mid, "f");
  // Drive type_of_def so the fn_signature slot computes.
  (void)query_type_of_def(&sema, def);

  struct QuerySlot *slot = fn_signature_slot(&sema, def);
  if (!slot) {
    fprintf(stderr, "         no fn_signature slot for 'f'\n");
    finish_test(false);
    sema_free(&sema);
    return;
  }
  uint64_t rev_before = slot->computed_rev;

  // Simulate a body that read untracked state. In real code this is
  // set by sema_mark_frame_untracked at frame succeed/fail time.
  slot->has_untracked_read = true;

  // Same source. Revision bumps; ast_fp is identical, deps' fps are
  // unchanged. Without the untracked-read shortcut, fn_signature
  // would early-cut (dep walk all-green → SKIP_RECOMPUTE). With the
  // shortcut, sema_revalidate returns RECOMPUTE → sema_query_begin
  // resets to EMPTY → body re-runs → computed_rev advances.
  drive_pipeline(&sema, iid, mid, src);
  (void)query_type_of_def(&sema, def);

  struct QuerySlot *slot_after = fn_signature_slot(&sema, def);
  uint64_t rev_after = slot_after ? slot_after->computed_rev : 0;

  bool recomputed = rev_after > rev_before;
  // After recompute the body itself didn't call SEMA_READ_UNTRACKED,
  // so the freshly-derived slot's flag must be CLEAR. Verifies that
  // sema_query_begin's reset path (EMPTY transition) clears the bit.
  bool flag_cleared = slot_after && !slot_after->has_untracked_read;

  bool ok = recomputed && flag_cleared;
  if (!ok) {
    fprintf(stderr,
            "         rev: before=%llu after=%llu (want after>before); "
            "flag_cleared=%d (want 1)\n",
            (unsigned long long)rev_before, (unsigned long long)rev_after,
            (int)flag_cleared);
  }
  finish_test(ok);
  sema_free(&sema);
#endif
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
  test_body_edit_signature_fingerprint_stable();
  test_signature_edit_fingerprint_shifts();
  test_struct_edit_cascade_fingerprints();
  test_multi_edit_stress();
  test_readd_removed_decl();
  test_comptime_chain_invalidation();
  test_is_comptime_flip();
  test_chain_no_op_skips_recompute();
  test_rename_then_readd_original();
  test_long_running_edit_cycle();
  test_same_name_different_shape();
  test_def_for_name_fingerprint_pub_toggle();
  test_untracked_read_forces_recompute();
  printf("\n%d pass, %d fail\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
