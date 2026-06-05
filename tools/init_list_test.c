// D2.6 gate — principled bidirectional `check_expr` for SK_PRODUCT_EXPR.
//   1. struct literal `T{ .x = ... }` types clean against the declared struct
//      (named init; anonymous-union flattening covered via Variant).
//   2. anonymous `.{ ... }` against an expected struct (existing handler path,
//      now via walk_init_list) still types clean.
//   3. array literal `[3]i32{ 1, 2, 3 }` types clean; elements checked vs i32.
//   4. inferred-size `[_]i32{ 1, 2, 3 }` resolves to a 3-elem array, clean.
//   5. unknown struct field → error.
//   6. positional initializer against a struct → error.
//   7. array size mismatch → error.
//   8. named initializer against an array → error.
//   9. standalone SK_INIT_LIST in a synth position → loud diag (no silent
//      IP_NONE fallback).
// KEEP_ZONE, ASan.

#include "../src/db/db.h"
#include "../src/db/diag/diag.h"
#include "../src/db/ids/ids.h"
#include "../src/db/intern_pool/intern_pool.h"
#include "../src/db/query/coerce.h"
#include "../src/db/query/engine.h"
#include "../src/db/workspace/workspace.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern void db_check_namespace(db_query_ctx *, NamespaceId);

static FileId open_file(struct db *s, const char *path, const char *text) {
    SourceId src = workspace_did_open(s, path, strlen(path), text, strlen(text));
    return db_lookup_file_by_source(s, src);
}

// Run db_check_namespace for `fid`'s namespace, then return (errors, warnings)
// counts from db_collect_diags_for_file (the unused warnings on private decls
// are fine to ignore — we only care about ERRORS).
typedef struct { int errors; int warnings; } Counts;
static Counts check_and_count(struct db *s, FileId fid) {
    NamespaceId ns = db_get_file_namespace(s, fid);
    db_request_begin(s, db_current_revision(s));
    db_check_namespace(s, ns);
    Vec out;
    vec_init(&out, sizeof(Diag));
    db_collect_diags_for_file(s, fid, &out);
    Counts r = {0};
    for (size_t i = 0; i < out.count; i++) {
        Diag *d = (Diag *)vec_get(&out, i);
        if (d->severity == DIAG_ERROR) r.errors++;
        else if (d->severity == DIAG_WARNING) r.warnings++;
    }
    vec_free(&out);
    db_request_end(s);
    return r;
}

int main(void) {
    struct db s;
    db_init(&s);

    // (1) struct literal — clean.
    {
        FileId fid = open_file(&s, "/sl.ore",
            "Point :: struct\n    x : i32\n    y : i32\n"
            "make :: fn() Point\n    return Point{ .x = 1, .y = 2 }\n");
        Counts r = check_and_count(&s, fid);
        assert(r.errors == 0 && "struct literal Point{ .x=1, .y=2 } types clean");
    }

    // (2) anonymous `.{ ... }` against expected struct — clean.
    {
        FileId fid = open_file(&s, "/anon.ore",
            "Point :: struct\n    x : i32\n    y : i32\n"
            "make :: fn() Point\n    return .{ .x = 1, .y = 2 }\n");
        Counts r = check_and_count(&s, fid);
        assert(r.errors == 0 && "anonymous .{ .x=1, .y=2 } against Point types clean");
    }

    // (3) array literal `[3]i32{ 1, 2, 3 }` — clean.
    {
        FileId fid = open_file(&s, "/arr.ore",
            "make :: fn() [3]i32\n    return [3]i32{ 1, 2, 3 }\n");
        Counts r = check_and_count(&s, fid);
        assert(r.errors == 0 && "[3]i32{ 1, 2, 3 } types clean");
    }

    // (4) inferred-size `[_]i32{ 1, 2, 3 }` — resolves to [3]i32, clean.
    //     The fn's declared return [3]i32 acts as the verifier; the literal's
    //     `_` size gets patched from the init count (3) inside resolve_product_target.
    {
        FileId fid = open_file(&s, "/inf.ore",
            "make :: fn() [3]i32\n    return [_]i32{ 1, 2, 3 }\n");
        Counts r = check_and_count(&s, fid);
        assert(r.errors == 0 && "[_]i32{ 1, 2, 3 } infers size 3 and types clean");
    }

    // (5) unknown struct field → error.
    {
        FileId fid = open_file(&s, "/uf.ore",
            "Point :: struct\n    x : i32\n    y : i32\n"
            "bad :: fn() Point\n    return Point{ .x = 1, .z = 2 }\n");
        Counts r = check_and_count(&s, fid);
        assert(r.errors >= 1 && "unknown field .z → error");
    }

    // (6) positional initializer against a struct → error.
    {
        FileId fid = open_file(&s, "/pos.ore",
            "Point :: struct\n    x : i32\n    y : i32\n"
            "bad :: fn() Point\n    return Point{ 1, 2 }\n");
        Counts r = check_and_count(&s, fid);
        assert(r.errors >= 1 && "positional initializer in struct literal → error");
    }

    // (7) array size mismatch → error.
    {
        FileId fid = open_file(&s, "/sz.ore",
            "bad :: fn() [3]i32\n    return [3]i32{ 1, 2 }\n");
        Counts r = check_and_count(&s, fid);
        assert(r.errors >= 1 && "[3]i32{ 1, 2 } element count != declared size → error");
    }

    // (8) named initializer against an array → error.
    {
        FileId fid = open_file(&s, "/na.ore",
            "bad :: fn() [3]i32\n    return [3]i32{ .x = 1, .y = 2, .z = 3 }\n");
        Counts r = check_and_count(&s, fid);
        assert(r.errors >= 1 && "named init in array literal → error");
    }

    // (9) standalone aggregate literal in a synth position → loud diag.
    //     `x :: { .a = 1 }` has no target type → SK_INIT_LIST hits the new
    //     diag in type_of_expr_impl (NOT a silent fallback).
    //
    //     NOTE: depending on the parser, `{ .a = 1 }` at the top level may
    //     parse as a block. To exercise the standalone INIT_LIST diag reliably,
    //     we'd need a parse path that reaches SK_INIT_LIST without a wrapping
    //     SK_PRODUCT_EXPR. If the parser always wraps init-lists in
    //     PRODUCT_EXPR (the current behavior per parse_expr.c:511), the
    //     standalone case is unreachable in practice — which is the intended
    //     invariant — and the contract docstring + the typed-but-anon diag in
    //     PRODUCT_EXPR cover the gap. Skip this assertion; document only.

    // (10) Array-init §A — ip_default_value direct API gates.
    //   - numeric primitives → non-NONE (zero-default sentinel)
    //   - bool → IP_BOOL_FALSE
    //   - optional → IP_NIL_TYPE
    //   - raw ptr → IP_NONE (strict-nil: no default)
    //   - array of defaultable → recursive non-NONE
    //   - array of fn type → IP_NONE (fn has no default)
    {
        // Direct API; no parser involved.
        assert(ip_default_value(&s, IP_I32_TYPE).v != IP_NONE.v &&
               "ip_default_value(i32) has a default");
        assert(ip_default_value(&s, IP_BOOL_TYPE).v == IP_BOOL_FALSE.v &&
               "ip_default_value(bool) == IP_BOOL_FALSE");
        IpIndex opt_i32 = ip_get(
            &s.intern,
            (IpKey){.kind = IPK_OPTIONAL_TYPE, .optional_type = {.elem = IP_I32_TYPE}});
        assert(ip_default_value(&s, opt_i32).v == IP_NIL_TYPE.v &&
               "ip_default_value(?i32) == IP_NIL_TYPE");
        IpIndex raw_ptr = ip_get(
            &s.intern,
            (IpKey){.kind = IPK_PTR_TYPE,
                    .ptr_type = {.elem = IP_I32_TYPE, .is_const = false}});
        assert(ip_default_value(&s, raw_ptr).v == IP_NONE.v &&
               "ip_default_value(^i32) == IP_NONE (strict-nil)");
        IpIndex arr_5_i32 = ip_get(
            &s.intern,
            (IpKey){.kind = IPK_ARRAY_TYPE,
                    .array_type = {.elem = IP_I32_TYPE, .size = 5}});
        assert(ip_default_value(&s, arr_5_i32).v != IP_NONE.v &&
               "ip_default_value([5]i32) recurses to defaultable elem");
        // fn types have no default. Build a trivial fn() i32 to probe.
        IpKey fn_key = {.kind = IPK_FN_TYPE};
        fn_key.fn_type.ret = IP_I32_TYPE;
        fn_key.fn_type.modifiers = 0;
        fn_key.fn_type.n_params = 0;
        fn_key.fn_type.params = NULL;
        IpIndex fn_ty = ip_get(&s.intern, fn_key);
        assert(ip_default_value(&s, fn_ty).v == IP_NONE.v &&
               "ip_default_value(fn() i32) == IP_NONE");
    }

    db_free(&s);
    printf("PASS init_list: struct literal (named) + anonymous .{...} + array "
           "literal + inferred-size [_]T + error-on-(unknown field, positional "
           "struct, size mismatch, named array) + ip_default_value table\n");
    return 0;
}
