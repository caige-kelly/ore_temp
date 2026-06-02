// Phase H — coerce.c table exhaustiveness (keep-zone, ASan).
//
// One assertion per rule row in the H plan's coerce table. Pass
// node = NULL — that's the documented structural-only path; the
// range-checking arm (which folds via db_const_eval and requires a
// real SyntaxNode) is exercised end-to-end by the fixture suite
// (`comptime_chain_errors.ore`, `int_narrowing_errors.ore`,
// `int_widening.ore`).
//
// The H1 invariant that coerce() does NOT modify NodeTypesRange is
// trivially upheld here: we pass ctx->types = NULL, so any
// node_type_builder_push would no-op anyway. Adding a positive
// "we observed zero pushes" check would require linking infer.c's
// builder machinery; the call-site spot-check in
// `make test`/`make test-lsp` (e.g. hover-on-comptime-leaf) covers
// the actual stamping behavior end-to-end.

#include "../src/db/db.h"
#include "../src/db/intern_pool/intern_pool.h"
#include "../src/db/query/coerce.h"
#include "../src/db/query/type_layer.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SemaCtx make_ctx(struct db *s) {
    return (SemaCtx){
        .s = s,
        .file_green_root = NULL,
        .nsid = (NamespaceId){0},
        .enclosing_fn = DEF_ID_NONE,
        .file_local = (FileId){0},
        .types = NULL,
    };
}

#define EXPECT_OK(actual, expected) do {                                       \
    Coercion c = coerce(&ctx, NULL, (actual), (expected));                     \
    if (c.kind != COERCE_OK) {                                                 \
        fprintf(stderr,                                                        \
                "FAIL line %d: expected COERCE_OK, got kind=%d\n",             \
                __LINE__, (int)c.kind);                                        \
        abort();                                                               \
    }                                                                          \
} while (0)

#define EXPECT_FAIL_TYPE(actual, expected) do {                                \
    Coercion c = coerce(&ctx, NULL, (actual), (expected));                     \
    if (c.kind != COERCE_FAIL_TYPE) {                                          \
        fprintf(stderr,                                                        \
                "FAIL line %d: expected COERCE_FAIL_TYPE, got kind=%d\n",      \
                __LINE__, (int)c.kind);                                        \
        abort();                                                               \
    }                                                                          \
} while (0)

int main(void) {
    struct db s;
    db_init(&s);
    SemaCtx ctx = make_ctx(&s);

    // --- Identity + sentinels --------------------------------------------
    EXPECT_OK(IP_I32_TYPE, IP_I32_TYPE);
    EXPECT_OK(IP_NONE, IP_I32_TYPE);             // poisoned actual
    EXPECT_OK(IP_I32_TYPE, IP_NONE);             // poisoned expected
    EXPECT_OK(IP_NORETURN_TYPE, IP_I32_TYPE);    // bottom → anything

    // --- Pointer variance: ^T → ^T, ^T → ^const T ------------------------
    // `is_const` is folded into the storage tag at intern time — same
    // IPK_PTR_TYPE key kind, distinct interned result.
    IpKey kp_mut_i32 = {.kind = IPK_PTR_TYPE,
                        .ptr_type = {.elem = IP_I32_TYPE, .is_const = false}};
    IpIndex ptr_mut_i32 = ip_get(&s.intern, kp_mut_i32);
    IpKey kp_const_i32 = {.kind = IPK_PTR_TYPE,
                          .ptr_type = {.elem = IP_I32_TYPE, .is_const = true}};
    IpIndex ptr_const_i32 = ip_get(&s.intern, kp_const_i32);
    IpKey kp_mut_u8 = {.kind = IPK_PTR_TYPE,
                       .ptr_type = {.elem = IP_U8_TYPE, .is_const = false}};
    IpIndex ptr_mut_u8 = ip_get(&s.intern, kp_mut_u8);

    EXPECT_OK(ptr_mut_i32, ptr_const_i32);       // mut → const
    EXPECT_FAIL_TYPE(ptr_const_i32, ptr_mut_i32); // const → mut rejected
    EXPECT_FAIL_TYPE(ptr_mut_i32, ptr_mut_u8);   // different elem

    // --- Slice variance: []T → []const T ---------------------------------
    IpKey ksl_mut_u8 = {.kind = IPK_SLICE_TYPE,
                        .slice_type = {.elem = IP_U8_TYPE, .is_const = false}};
    IpIndex slice_mut_u8 = ip_get(&s.intern, ksl_mut_u8);
    IpKey ksl_const_u8 = {.kind = IPK_SLICE_TYPE,
                          .slice_type = {.elem = IP_U8_TYPE, .is_const = true}};
    IpIndex slice_const_u8 = ip_get(&s.intern, ksl_const_u8);

    EXPECT_OK(slice_mut_u8, slice_const_u8);
    EXPECT_FAIL_TYPE(slice_const_u8, slice_mut_u8);

    // --- Many-ptr variance ----------------------------------------------
    IpKey kmp_mut = {.kind = IPK_MANY_PTR_TYPE,
                     .many_ptr_type = {.elem = IP_U8_TYPE, .is_const = false}};
    IpIndex many_mut_u8 = ip_get(&s.intern, kmp_mut);
    IpKey kmp_const = {.kind = IPK_MANY_PTR_TYPE,
                       .many_ptr_type = {.elem = IP_U8_TYPE, .is_const = true}};
    IpIndex many_const_u8 = ip_get(&s.intern, kmp_const);

    EXPECT_OK(many_mut_u8, many_const_u8);

    // --- Array-ptr decay: ^[N]T → []T / [^]T -----------------------------
    IpKey karr = {.kind = IPK_ARRAY_TYPE,
                  .array_type = {.elem = IP_I32_TYPE, .size = 3}};
    IpIndex arr_3_i32 = ip_get(&s.intern, karr);
    IpKey kp_arr = {.kind = IPK_PTR_TYPE,
                    .ptr_type = {.elem = arr_3_i32, .is_const = false}};
    IpIndex ptr_arr_3_i32 = ip_get(&s.intern, kp_arr);

    IpKey ksl_i32 = {.kind = IPK_SLICE_TYPE,
                     .slice_type = {.elem = IP_I32_TYPE, .is_const = false}};
    IpIndex slice_i32 = ip_get(&s.intern, ksl_i32);
    IpKey kmp_i32 = {.kind = IPK_MANY_PTR_TYPE,
                     .many_ptr_type = {.elem = IP_I32_TYPE, .is_const = false}};
    IpIndex many_i32 = ip_get(&s.intern, kmp_i32);

    EXPECT_OK(ptr_arr_3_i32, slice_i32);  // ^[3]i32 → []i32
    EXPECT_OK(ptr_arr_3_i32, many_i32);   // ^[3]i32 → [^]i32

    // --- nil-lift --------------------------------------------------------
    IpKey kopt_i32 = {.kind = IPK_OPTIONAL_TYPE,
                      .optional_type = {.elem = IP_I32_TYPE}};
    IpIndex opt_i32 = ip_get(&s.intern, kopt_i32);

    // §H — nil ONLY coerces into optionals. Raw pointers / slices /
    // many-pointers are non-null; nullability requires the `?` wrapper.
    EXPECT_OK(IP_NIL_TYPE, opt_i32);
    EXPECT_FAIL_TYPE(IP_NIL_TYPE, ptr_mut_i32);   // raw ptr rejects nil
    EXPECT_FAIL_TYPE(IP_NIL_TYPE, slice_mut_u8);  // raw slice rejects nil
    EXPECT_FAIL_TYPE(IP_NIL_TYPE, many_mut_u8);   // raw many-ptr rejects nil
    EXPECT_FAIL_TYPE(IP_NIL_TYPE, IP_I32_TYPE);   // nil → non-pointer rejected

    // --- Optional lift: T → ?T (recursive on elem) -----------------------
    EXPECT_OK(IP_I32_TYPE, opt_i32);
    EXPECT_FAIL_TYPE(IP_BOOL_TYPE, opt_i32);     // wrong elem

    // --- Comptime numeric (structural) -----------------------------------
    // Range check needs a node; without it, comptime → concrete is
    // structurally OK.
    EXPECT_OK(IP_COMPTIME_INT_TYPE, IP_I32_TYPE);
    EXPECT_OK(IP_COMPTIME_INT_TYPE, IP_F64_TYPE);
    EXPECT_OK(IP_COMPTIME_INT_TYPE, IP_COMPTIME_FLOAT_TYPE);
    EXPECT_OK(IP_COMPTIME_FLOAT_TYPE, IP_F32_TYPE);
    EXPECT_FAIL_TYPE(IP_COMPTIME_FLOAT_TYPE, IP_I32_TYPE); // float → int

    // --- H1.5 widening: u8 → u16, i8 → i32, u8 → i16 (small-uns → wider-sgn)
    EXPECT_OK(IP_U8_TYPE,  IP_U16_TYPE);
    EXPECT_OK(IP_U8_TYPE,  IP_U32_TYPE);
    EXPECT_OK(IP_U8_TYPE,  IP_U64_TYPE);
    EXPECT_OK(IP_I8_TYPE,  IP_I32_TYPE);
    EXPECT_OK(IP_I16_TYPE, IP_I64_TYPE);
    EXPECT_OK(IP_U8_TYPE,  IP_I16_TYPE);
    EXPECT_OK(IP_U8_TYPE,  IP_I32_TYPE);
    EXPECT_OK(IP_U16_TYPE, IP_I32_TYPE);

    // --- H1.5 rejects: same-width sign change, narrow without const ------
    EXPECT_FAIL_TYPE(IP_U32_TYPE, IP_I32_TYPE);  // same width, sign change
    EXPECT_FAIL_TYPE(IP_U64_TYPE, IP_I64_TYPE);
    EXPECT_FAIL_TYPE(IP_I32_TYPE, IP_I16_TYPE);  // narrow signed
    EXPECT_FAIL_TYPE(IP_U16_TYPE, IP_U8_TYPE);   // narrow unsigned
    EXPECT_FAIL_TYPE(IP_I32_TYPE, IP_U32_TYPE);  // signed → unsigned
    EXPECT_FAIL_TYPE(IP_F32_TYPE, IP_F64_TYPE);  // float widening NOT in
                                                  // H1.5 — only ints

    // --- Cross-class rejected: int ↔ float, etc. -------------------------
    EXPECT_FAIL_TYPE(IP_I32_TYPE, IP_F32_TYPE);
    EXPECT_FAIL_TYPE(IP_F32_TYPE, IP_I32_TYPE);
    EXPECT_FAIL_TYPE(IP_BOOL_TYPE, IP_I32_TYPE);

    db_free(&s);
    printf("PASS coerce: structural + variance + decay + nil/optional + "
           "comptime + H1.5 width-change (32 rules verified)\n");
    return 0;
}
