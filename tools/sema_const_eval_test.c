#include "sema/const_eval.h"
#include "sema/layout.h"
#include "sema/sema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Stubs: const_eval.c and layout.c reference helpers that live in checker.c /
// decls.c / sema.c. The unit test only exercises code paths that don't hit
// those calls, so abort if they are invoked unexpectedly.
void sema_error(struct Sema* sema, struct Span span, const char* fmt, ...) {
    (void)sema; (void)span; (void)fmt;
    fprintf(stderr, "sema_error called unexpectedly in unit test\n");
    abort();
}

struct Type* sema_infer_type_expr(struct Sema* sema, struct Expr* expr) {
    (void)sema; (void)expr;
    fprintf(stderr, "sema_infer_type_expr called unexpectedly in unit test\n");
    abort();
}

struct Type* sema_type_of_decl(struct Sema* sema, struct Decl* decl) {
    (void)sema; (void)decl;
    fprintf(stderr, "sema_type_of_decl called unexpectedly in unit test\n");
    abort();
}

struct SemaDeclInfo* sema_decl_info(struct Sema* sema, struct Decl* decl) {
    (void)sema; (void)decl;
    fprintf(stderr, "sema_decl_info called unexpectedly in unit test\n");
    abort();
}

struct Type* sema_decl_type(struct Sema* sema, struct Decl* decl) {
    (void)sema; (void)decl;
    return NULL;  // unit test doesn't exercise field-typed structs
}

static int setup_sema(struct Sema* sema, Arena* arena, StringPool* pool) {
    arena_init(arena, 4096);
    pool_init(pool, 1024);

    *sema = (struct Sema){0};
    sema->arena = arena;
    sema->pool = pool;
    // No Compiler attached; layout/const_eval fall back to host target.
    sema->bodies = vec_new_in(arena, sizeof(struct CheckedBody*));
    sema->current_body = NULL;
    hashmap_init_in(&sema->effect_sig_cache, arena);
    sema->query_stack = vec_new_in(arena, sizeof(struct QueryFrame));

    sema->unknown_type = sema_type_new(sema, TYPE_UNKNOWN);
    sema->error_type = sema_type_new(sema, TYPE_ERROR);
    sema->void_type = sema_type_new(sema, TYPE_VOID);
    sema->noreturn_type = sema_type_new(sema, TYPE_NORETURN);
    sema->bool_type = sema_type_new(sema, TYPE_BOOL);
    sema->comptime_int_type = sema_type_new(sema, TYPE_COMPTIME_INT);
    sema->comptime_float_type = sema_type_new(sema, TYPE_COMPTIME_FLOAT);
    sema->u8_type = sema_type_new(sema, TYPE_U8);
    sema->u16_type = sema_type_new(sema, TYPE_U16);
    sema->u32_type = sema_type_new(sema, TYPE_U32);
    sema->u64_type = sema_type_new(sema, TYPE_U64);
    sema->usize_type = sema_type_new(sema, TYPE_USIZE);
    sema->i8_type = sema_type_new(sema, TYPE_I8);
    sema->i16_type = sema_type_new(sema, TYPE_I16);
    sema->i32_type = sema_type_new(sema, TYPE_I32);
    sema->i64_type = sema_type_new(sema, TYPE_I64);
    sema->isize_type = sema_type_new(sema, TYPE_ISIZE);
    sema->f32_type = sema_type_new(sema, TYPE_F32);
    sema->f64_type = sema_type_new(sema, TYPE_F64);
    sema->string_type = sema_type_new(sema, TYPE_STRING);
    sema->nil_type = sema_type_new(sema, TYPE_NIL);
    sema->type_type = sema_type_new(sema, TYPE_TYPE);
    sema->anytype_type = sema_type_new(sema, TYPE_ANYTYPE);
    sema->module_type = sema_type_new(sema, TYPE_MODULE);
    sema->effect_type = sema_type_new(sema, TYPE_EFFECT);
    sema->effect_row_type = sema_type_new(sema, TYPE_EFFECT_ROW);
    sema->scope_token_type = sema_type_new(sema, TYPE_SCOPE_TOKEN);
    return 0;
}

int main(void) {
    Arena arena;
    StringPool pool;
    struct Sema sema;
    int rc = 0;
    if (setup_sema(&sema, &arena, &pool) != 0) return 1;

    // ConstValue helpers behave correctly.
    struct ConstValue four = sema_const_int(4);
    struct ConstValue four2 = sema_const_int(4);
    struct ConstValue five = sema_const_int(5);
    struct ConstValue tru = sema_const_bool(true);

    if (!sema_const_value_is_valid(four)) { rc = 2; goto out; }
    if (sema_const_value_is_valid(sema_const_invalid())) { rc = 3; goto out; }
    if (!sema_const_value_equal(four, four2)) { rc = 4; goto out; }
    if (sema_const_value_equal(four, five)) { rc = 5; goto out; }
    if (sema_const_value_equal(four, tru)) { rc = 6; goto out; }

    // Layout query yields concrete sizes for primitives.
    struct TypeLayout u32_layout = sema_layout_of_type(&sema, sema.u32_type);
    if (!u32_layout.complete || u32_layout.size != 4 || u32_layout.align != 4) { rc = 7; goto out; }

    struct TargetInfo host = target_default_host();
    struct TypeLayout usize_layout = sema_layout_of_type(&sema, sema.usize_type);
    if (!usize_layout.complete || usize_layout.size != host.usize_size) { rc = 8; goto out; }

    struct TypeLayout ptr_layout = sema_layout_of_type(&sema,
        sema_pointer_type(&sema, sema.u8_type));
    if (!ptr_layout.complete || ptr_layout.size != host.pointer_size) { rc = 9; goto out; }

    struct TypeLayout slice_layout = sema_layout_of_type(&sema,
        sema_slice_type(&sema, sema.u8_type));
    if (!slice_layout.complete || slice_layout.size != host.pointer_size * 2) { rc = 10; goto out; }

    // ComptimeEnv: bind a Decl* to a ConstValue and look it up across frames.
    struct Decl outer_decl = {0};
    struct Decl inner_decl = {0};
    struct ComptimeEnv* outer = sema_comptime_env_new(&sema, NULL);
    sema_comptime_env_bind(&sema, outer, &outer_decl, sema_const_int(7));

    struct ComptimeEnv* inner = sema_comptime_env_new(&sema, outer);
    sema_comptime_env_bind(&sema, inner, &inner_decl, sema_const_int(99));

    struct ConstValue v = {0};
    if (!sema_comptime_env_lookup(inner, &outer_decl, &v) || v.int_val != 7) { rc = 11; goto out; }
    if (!sema_comptime_env_lookup(inner, &inner_decl, &v) || v.int_val != 99) { rc = 12; goto out; }
    if (sema_comptime_env_lookup(outer, &inner_decl, &v)) { rc = 13; goto out; }

    // Inner shadows outer.
    sema_comptime_env_bind(&sema, inner, &outer_decl, sema_const_int(42));
    if (!sema_comptime_env_lookup(inner, &outer_decl, &v) || v.int_val != 42) { rc = 14; goto out; }
    if (!sema_comptime_env_lookup(outer, &outer_decl, &v) || v.int_val != 7) { rc = 15; goto out; }

    //  ---- Cell based bindings + assign ----

    // Create a fresh decl and a fresh env (no parent - start clean)
    struct Decl mut_decl = {0};
    struct ComptimeEnv* env = sema_comptime_env_new(&sema, NULL);

    // Bind mut decl to 10. The new bind allocates a call, put 10 in it.
    sema_comptime_env_bind(&sema, env, &mut_decl, sema_const_int(10));

    // Lookup should give us 10. Proves bind-then-lookup works
    if (!sema_comptime_env_lookup(env, &mut_decl, &v) || v.int_val != 10) {
        rc = 16;
        goto out;
    }

    // Assign mut_decl to 20. This walks the env, finds the binding, and mutates in place.
    sema_comptime_env_assign(&sema, env, &mut_decl, sema_const_int(20));

    // Lookup again. must give 20 not 10. Proves assignment mutates the cell and lookup reads current val.
    if (!sema_comptime_env_lookup(env, &mut_decl, &v) || v.int_val != 20) {
        rc = 17;
        goto out;
    }

    // Prove assign walks parent frames, like lookup. Key for closures. callee's env can mutate the binding
    // that lives in the caller's env. 
    struct Decl shared_decl = {0};
    struct ComptimeEnv* parent = sema_comptime_env_new(&sema, NULL);
    sema_comptime_env_bind(&sema, parent, &shared_decl, sema_const_int(100));

    struct ComptimeEnv* child = sema_comptime_env_new(&sema, parent);

    // Child has no binding for shard_decl, but lookup walks parent.
    if (!sema_comptime_env_lookup(child, &shared_decl, &v) || v.int_val != 100) {
        rc = 18;
        goto out;
    }

    // Assign through the child - should walk parent and mutate parent's cell.
    sema_comptime_env_assign(&sema, child, &shared_decl, sema_const_int(200));

    // Lookup through the parent directly - should see the new value.
    // If fails, it means assign didn't actually mutate.
    if (!sema_comptime_env_lookup(parent, &shared_decl, &v) || v.int_val != 200) {
        rc = 19;
        goto out;
    }

out:
    pool_free(&pool);
    arena_free(&arena);
    if (rc != 0) fprintf(stderr, "sema_const_eval_test failed at %d\n", rc);
    return rc;
}
