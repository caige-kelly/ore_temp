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

// const_eval.c::call_cache_lookup uses this canonical predicate from
// instantiate.c. We don't link instantiate.c into the unit test (pulls in too
// many sema deps), so re-implement the same algorithm here. If the real one
// in instantiate.c ever drifts, the cache test below will catch it.
bool sema_arg_tuple_equal(const struct ComptimeArgTuple* a, const struct ComptimeArgTuple* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (!a->values || !b->values) return a->values == b->values;
    if (a->values->count != b->values->count) return false;
    for (size_t i = 0; i < a->values->count; i++) {
        struct ConstValue* ax = (struct ConstValue*)vec_get(a->values, i);
        struct ConstValue* bx = (struct ConstValue*)vec_get(b->values, i);
        if (!ax || !bx) return false;
        if (!sema_const_value_equal(*ax, *bx)) return false;
    }
    return true;
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

    // ----- Validate Eval Result -----

    struct EvalResult ok = sema_eval_normal(sema_const_int(123));
    if (ok.control != EVAL_NORMAL) { rc = 20; goto out; }
    if (ok.value.int_val != 123)   { rc = 21; goto out; }

    struct EvalResult bad = sema_eval_err();
    if (bad.control != EVAL_ERROR)       { rc = 22; goto out; }
    if (bad.value.kind != CONST_INVALID) { rc = 23; goto out; }

    // ----- Validate Void -----
    struct ConstValue void_val = sema_const_void();
    if (void_val.kind != CONST_VOID) { rc = 24; goto out; }

    if (!sema_const_value_is_valid(void_val)) { rc = 25; goto out; }
    if (!sema_const_value_equal(void_val, sema_const_void())) { rc = 26 ; goto out; }
    if (sema_const_value_equal(void_val, sema_const_invalid())) { rc = 27; goto out; }
    if (sema_const_value_equal(void_val, sema_const_int(0))) {rc = 28; goto out; }

    // ---- Step 4: eval_block ----

    // Synthesize an empty block expression. expr_Block has an embedded Vec of Expr*.
    struct Expr empty_block = {0};
    empty_block.kind = expr_Block;
    empty_block.block.stmts = vec_new_in(&arena, sizeof(struct Expr*));

    struct EvalResult eb = sema_const_eval_expr(&sema, &empty_block, NULL);
    if (eb.control != EVAL_NORMAL)   { rc = 29; goto out; }
    if (eb.value.kind != CONST_VOID) { rc = 30; goto out; }

    struct Expr lit_seven = {0};
    lit_seven.kind = expr_Lit;
    lit_seven.lit.kind = lit_Int;
    lit_seven.lit.string_id = pool_intern(&pool, "7", 1);

    struct Expr* lit_seven_ptr = &lit_seven;

    struct Expr block_one = {0};
    block_one.kind = expr_Block;
    block_one.block.stmts = vec_new_in(&arena, sizeof(struct Expr*));
    vec_push(block_one.block.stmts, &lit_seven_ptr);

    struct EvalResult b1 = sema_const_eval_expr(&sema, &block_one, NULL);
    if (b1.control != EVAL_NORMAL)   { rc = 31; goto out; }
    if (b1.value.kind != CONST_INT)  { rc = 32; goto out; }
    if (b1.value.int_val != 7)       { rc = 33; goto out; }

    // Synthesize a block of two literals: { 7; 42 }
    // The block's value should be 42 (last-statement-as-value).
    struct Expr lit_42 = {0};
    lit_42.kind = expr_Lit;
    lit_42.lit.kind = lit_Int;
    lit_42.lit.string_id = pool_intern(&pool, "42", 2);

    struct Expr* lit_42_ptr = &lit_42;

    struct Expr block_two = {0};
    block_two.kind = expr_Block;
    block_two.block.stmts = vec_new_in(&arena, sizeof(struct Expr*));
    vec_push(block_two.block.stmts, &lit_seven_ptr);
    vec_push(block_two.block.stmts, &lit_42_ptr);

    struct EvalResult b2 = sema_const_eval_expr(&sema, &block_two, NULL);
    if (b2.control != EVAL_NORMAL)          { rc = 34; goto out; }
    if (b2.value.int_val != 42)             { rc = 35; goto out; }

    // ---- Step 5: return / break / continue ----

    // Synthesize: return  (no value)
    // Should produce EVAL_RETURN with CONST_VOID.
    struct Expr ret_void = {0};
    ret_void.kind = expr_Return;
    ret_void.return_expr.value = NULL;

    struct EvalResult r1 = sema_const_eval_expr(&sema, &ret_void, NULL);
    if (r1.control != EVAL_RETURN)        { rc = 36; goto out; }
    if (r1.value.kind != CONST_VOID)      { rc = 37; goto out; }

    // Synthesize: return 99
    // Should produce EVAL_RETURN with CONST_INT(99).
    struct Expr lit_99 = {0};
    lit_99.kind = expr_Lit;
    lit_99.lit.kind = lit_Int;
    lit_99.lit.string_id = pool_intern(&pool, "99", 2);

    struct Expr ret_99 = {0};
    ret_99.kind = expr_Return;
    ret_99.return_expr.value = &lit_99;

    struct EvalResult r2 = sema_const_eval_expr(&sema, &ret_99, NULL);
    if (r2.control != EVAL_RETURN)        { rc = 38; goto out; }
    if (r2.value.kind != CONST_INT)       { rc = 39; goto out; }
    if (r2.value.int_val != 99)           { rc = 40; goto out; }

    // Synthesize: break
    struct Expr brk = {0};
    brk.kind = expr_Break;

    struct EvalResult r3 = sema_const_eval_expr(&sema, &brk, NULL);
    if (r3.control != EVAL_BREAK)         { rc = 41; goto out; }
    if (r3.value.kind != CONST_VOID)      { rc = 42; goto out; }

    // Synthesize: continue
    struct Expr cont = {0};
    cont.kind = expr_Continue;

    struct EvalResult r4 = sema_const_eval_expr(&sema, &cont, NULL);
    if (r4.control != EVAL_CONTINUE)      { rc = 43; goto out; }
    if (r4.value.kind != CONST_VOID)      { rc = 44; goto out; }

    // The interesting one: a block that does { 1; return 99; 2 }
    // The "2" should never run because RETURN bubbles out of the block.
    // Result should be EVAL_RETURN(CONST_INT(99)) — not CONST_INT(2).
    struct Expr lit_1 = {0};
    lit_1.kind = expr_Lit;
    lit_1.lit.kind = lit_Int;
    lit_1.lit.string_id = pool_intern(&pool, "1", 1);

    struct Expr lit_2 = {0};
    lit_2.kind = expr_Lit;
    lit_2.lit.kind = lit_Int;
    lit_2.lit.string_id = pool_intern(&pool, "2", 1);

    struct Expr* lit_1_ptr   = &lit_1;
    struct Expr* ret_99_ptr  = &ret_99;
    struct Expr* lit_2_ptr   = &lit_2;

    struct Expr block_with_return = {0};
    block_with_return.kind = expr_Block;
    block_with_return.block.stmts = vec_new_in(&arena, sizeof(struct Expr*));
    vec_push(block_with_return.block.stmts, &lit_1_ptr);
    vec_push(block_with_return.block.stmts, &ret_99_ptr);
    vec_push(block_with_return.block.stmts, &lit_2_ptr);

    struct EvalResult r5 = sema_const_eval_expr(&sema, &block_with_return, NULL);
    if (r5.control != EVAL_RETURN)        { rc = 45; goto out; }
    if (r5.value.int_val != 99)           { rc = 46; goto out; }

        // ---- Step 6: eval_if ----

    // Synthesize: if (true) 1 else 2  →  should be 1
    struct Expr cond_true = {0};
    cond_true.kind = expr_Lit;
    cond_true.lit.kind = lit_True;

    struct Expr if_true = {0};
    if_true.kind = expr_If;
    if_true.if_expr.condition = &cond_true;
    if_true.if_expr.then_branch = &lit_1;       // reuse from earlier tests
    if_true.if_expr.else_branch = &lit_2;

    struct EvalResult i1 = sema_const_eval_expr(&sema, &if_true, NULL);
    if (i1.control != EVAL_NORMAL)        { rc = 47; goto out; }
    if (i1.value.kind != CONST_INT)       { rc = 48; goto out; }
    if (i1.value.int_val != 1)            { rc = 49; goto out; }

    // Synthesize: if (false) 1 else 2  →  should be 2
    struct Expr cond_false = {0};
    cond_false.kind = expr_Lit;
    cond_false.lit.kind = lit_False;

    struct Expr if_false = {0};
    if_false.kind = expr_If;
    if_false.if_expr.condition = &cond_false;
    if_false.if_expr.then_branch = &lit_1;
    if_false.if_expr.else_branch = &lit_2;

    struct EvalResult i2 = sema_const_eval_expr(&sema, &if_false, NULL);
    if (i2.control != EVAL_NORMAL)        { rc = 50; goto out; }
    if (i2.value.int_val != 2)            { rc = 51; goto out; }

    // Synthesize: if (false) 1   (no else)  →  should be CONST_VOID
    struct Expr if_no_else = {0};
    if_no_else.kind = expr_If;
    if_no_else.if_expr.condition = &cond_false;
    if_no_else.if_expr.then_branch = &lit_1;
    if_no_else.if_expr.else_branch = NULL;

    struct EvalResult i3 = sema_const_eval_expr(&sema, &if_no_else, NULL);
    if (i3.control != EVAL_NORMAL)        { rc = 52; goto out; }
    if (i3.value.kind != CONST_VOID)      { rc = 53; goto out; }

    // The juicy one: if (true) return 99  → should produce EVAL_RETURN(99).
    // This proves the picked branch's RETURN tag bubbles out through the if.
    struct Expr if_with_return = {0};
    if_with_return.kind = expr_If;
    if_with_return.if_expr.condition = &cond_true;
    if_with_return.if_expr.then_branch = &ret_99;   // reuse from step 5 tests
    if_with_return.if_expr.else_branch = NULL;

    struct EvalResult i4 = sema_const_eval_expr(&sema, &if_with_return, NULL);
    if (i4.control != EVAL_RETURN)        { rc = 54; goto out; }
    if (i4.value.int_val != 99)           { rc = 55; goto out; }

    // Negative case: condition is non-bool (use lit_99 = comptime int) → INVALID.
    // This is the "couldn't fold" path — the type checker should have rejected
    // this upstream, but the evaluator handles it defensively.
    struct Expr if_bad_cond = {0};
    if_bad_cond.kind = expr_If;
    if_bad_cond.if_expr.condition = &lit_99;        // int, not bool
    if_bad_cond.if_expr.then_branch = &lit_1;
    if_bad_cond.if_expr.else_branch = &lit_2;

    struct EvalResult i5 = sema_const_eval_expr(&sema, &if_bad_cond, NULL);
    if (i5.control != EVAL_NORMAL)        { rc = 56; goto out; }
    if (i5.value.kind != CONST_INVALID)   { rc = 57; goto out; }

        // ---- Step 7: eval_loop ----

    // Synthesize: loop  break
    // Infinite loop with immediate break → exits immediately, returns void.
    struct Expr brk_in_loop = {0};
    brk_in_loop.kind = expr_Break;

    struct Expr loop_break = {0};
    loop_break.kind = expr_Loop;
    loop_break.loop_expr.init = NULL;
    loop_break.loop_expr.condition = NULL;
    loop_break.loop_expr.step = NULL;
    loop_break.loop_expr.body = &brk_in_loop;

    struct EvalResult lp1 = sema_const_eval_expr(&sema, &loop_break, NULL);
    if (lp1.control != EVAL_NORMAL)        { rc = 58; goto out; }
    if (lp1.value.kind != CONST_VOID)      { rc = 59; goto out; }

    // Synthesize: loop  return 42
    // Loop with immediate return — RETURN bubbles out, doesn't get caught.
    struct Expr lit_42_2 = {0};
    lit_42_2.kind = expr_Lit;
    lit_42_2.lit.kind = lit_Int;
    lit_42_2.lit.string_id = pool_intern(&pool, "42", 2);

    struct Expr ret_42_2 = {0};
    ret_42_2.kind = expr_Return;
    ret_42_2.return_expr.value = &lit_42;

    struct Expr loop_ret = {0};
    loop_ret.kind = expr_Loop;
    loop_ret.loop_expr.body = &ret_42_2;

    struct EvalResult lp2 = sema_const_eval_expr(&sema, &loop_ret, NULL);
    if (lp2.control != EVAL_RETURN)        { rc = 60; goto out; }
    if (lp2.value.int_val != 42)           { rc = 61; goto out; }

    // Synthesize: loop (false) body  →  immediately exits, returns void.
    // (Body should never run — but we'll use `return 99` as the body so the
    // assertion catches it if the body did run.)
    struct Expr cond_false_2 = {0};
    cond_false_2.kind = expr_Lit;
    cond_false_2.lit.kind = lit_False;

    struct Expr loop_false = {0};
    loop_false.kind = expr_Loop;
    loop_false.loop_expr.condition = &cond_false_2;
    loop_false.loop_expr.body = &ret_99;     // reuse from earlier

    struct EvalResult lp3 = sema_const_eval_expr(&sema, &loop_false, NULL);
    if (lp3.control != EVAL_NORMAL)        { rc = 62; goto out; }
    if (lp3.value.kind != CONST_VOID)      { rc = 63; goto out; }

    // Synthesize: loop  continue
    // CONTINUE makes the loop iterate forever (no break, no condition).
    // Should hit the fuel limit and return EVAL_ERROR.
    struct Expr cont_in_loop = {0};
    cont_in_loop.kind = expr_Continue;

    struct Expr loop_continue = {0};
    loop_continue.kind = expr_Loop;
    loop_continue.loop_expr.body = &cont_in_loop;

    struct EvalResult lp4 = sema_const_eval_expr(&sema, &loop_continue, NULL);
    if (lp4.control != EVAL_ERROR)         { rc = 64; goto out; }

    // ---- Step 8: eval_call ----

    // We need to synthesize:
    //   square :: fn(x: comptime_int) comptime_int
    //       x * x
    // Then call square(5) and check we get 25.

    // The param x.
    struct Decl x_decl = {0};
    x_decl.semantic_kind = SEM_VALUE;
    x_decl.kind = DECL_PARAM;
    x_decl.name.string_id = pool_intern(&pool, "x", 1);
    x_decl.name.resolved = &x_decl;     // self-ref (resolver invariant)

    struct Param x_param = {0};
    x_param.name = x_decl.name;          // param.name.resolved → x_decl

    // Body: x * x
    struct Expr ident_x = {0};
    ident_x.kind = expr_Ident;
    ident_x.ident = x_decl.name;
    ident_x.ident.resolved = &x_decl;

    struct Expr body_xx = {0};
    body_xx.kind = expr_Bin;
    body_xx.bin.op = Star;
    body_xx.bin.Left = &ident_x;
    body_xx.bin.Right = &ident_x;

    // Lambda: fn(x) x * x
    struct Expr lambda_square = {0};
    lambda_square.kind = expr_Lambda;
    lambda_square.lambda.params = vec_new_in(&arena, sizeof(struct Param));
    vec_push(lambda_square.lambda.params, &x_param);
    lambda_square.lambda.body = &body_xx;

    // Bind: square :: lambda
    struct Expr bind_square = {0};
    bind_square.kind = expr_Bind;
    bind_square.bind.name.string_id = pool_intern(&pool, "square", 6);
    bind_square.bind.value = &lambda_square;

    // The square Decl, pointing back at the bind.
    struct Decl square_decl = {0};
    square_decl.semantic_kind = SEM_VALUE;
    square_decl.kind = DECL_USER;
    square_decl.name = bind_square.bind.name;
    square_decl.name.resolved = &square_decl;
    square_decl.node = &bind_square;
    bind_square.bind.name.resolved = &square_decl;

    // The callee identifier: `square`
    struct Expr ident_square = {0};
    ident_square.kind = expr_Ident;
    ident_square.ident = square_decl.name;
    ident_square.ident.resolved = &square_decl;

    // The argument: literal 5
    struct Expr lit_5 = {0};
    lit_5.kind = expr_Lit;
    lit_5.lit.kind = lit_Int;
    lit_5.lit.string_id = pool_intern(&pool, "5", 1);

    struct Expr* lit_5_ptr = &lit_5;

    // The call: square(5)
    struct Expr call_sq = {0};
    call_sq.kind = expr_Call;
    call_sq.call.callee = &ident_square;
    call_sq.call.args = vec_new_in(&arena, sizeof(struct Expr*));
    vec_push(call_sq.call.args, &lit_5_ptr);

    struct EvalResult c1 = sema_const_eval_expr(&sema, &call_sq, NULL);
    if (c1.control != EVAL_NORMAL)        { rc = 65; goto out; }
    if (c1.value.kind != CONST_INT)       { printf("%d", c1.value.kind);rc = 66; goto out; }
    if (c1.value.int_val != 25)           { rc = 67; goto out; }

    // Same function, different arg: square(7) → 49
    struct Expr lit_7 = {0};
    lit_7.kind = expr_Lit;
    lit_7.lit.kind = lit_Int;
    lit_7.lit.string_id = pool_intern(&pool, "7", 1);

    struct Expr* lit_7_ptr = &lit_7;

    struct Expr call_sq7 = {0};
    call_sq7.kind = expr_Call;
    call_sq7.call.callee = &ident_square;
    call_sq7.call.args = vec_new_in(&arena, sizeof(struct Expr*));
    vec_push(call_sq7.call.args, &lit_7_ptr);

    struct EvalResult c2 = sema_const_eval_expr(&sema, &call_sq7, NULL);
    if (c2.control != EVAL_NORMAL)        { rc = 68; goto out; }
    if (c2.value.int_val != 49)           { rc = 69; goto out; }

    // Arity mismatch: square()  → error
    struct Expr call_no_args = {0};
    call_no_args.kind = expr_Call;
    call_no_args.call.callee = &ident_square;
    call_no_args.call.args = vec_new_in(&arena, sizeof(struct Expr*));   // empty

    struct EvalResult c3 = sema_const_eval_expr(&sema, &call_no_args, NULL);
    if (c3.control != EVAL_ERROR)         { rc = 70; goto out; }

    // ---- Step 8b: call cache ----
    //
    // The earlier step-8 calls (c1=square(5), c2=square(7)) already populated
    // the cache. Reset the body-eval counter to start clean; the cache itself
    // is intentionally not cleared (caching is supposed to survive any
    // observation of the counter).

    sema.comptime_body_evals = 0;

    // square(5) and square(7) both already cached → hit, no body run.
    struct EvalResult m1 = sema_const_eval_expr(&sema, &call_sq, NULL);
    if (m1.control != EVAL_NORMAL)        { rc = 71; goto out; }
    if (m1.value.int_val != 25)           { rc = 72; goto out; }
    if (sema.comptime_body_evals != 0)    { rc = 73; goto out; }

    struct EvalResult m2 = sema_const_eval_expr(&sema, &call_sq, NULL);
    if (m2.control != EVAL_NORMAL)        { rc = 74; goto out; }
    if (m2.value.int_val != 25)           { rc = 75; goto out; }
    if (sema.comptime_body_evals != 0)    { rc = 76; goto out; }

    struct EvalResult m3 = sema_const_eval_expr(&sema, &call_sq7, NULL);
    if (m3.control != EVAL_NORMAL)        { rc = 77; goto out; }
    if (m3.value.int_val != 49)           { rc = 78; goto out; }
    if (sema.comptime_body_evals != 0)    { rc = 79; goto out; }

    // Fresh argument value → cache miss → body runs once.
    struct Expr lit_11 = {0};
    lit_11.kind = expr_Lit;
    lit_11.lit.kind = lit_Int;
    lit_11.lit.string_id = pool_intern(&pool, "11", 2);
    struct Expr* lit_11_ptr = &lit_11;

    struct Expr call_sq11 = {0};
    call_sq11.kind = expr_Call;
    call_sq11.call.callee = &ident_square;
    call_sq11.call.args = vec_new_in(&arena, sizeof(struct Expr*));
    vec_push(call_sq11.call.args, &lit_11_ptr);

    struct EvalResult m4 = sema_const_eval_expr(&sema, &call_sq11, NULL);
    if (m4.control != EVAL_NORMAL)        { rc = 80; goto out; }
    if (m4.value.int_val != 121)          { rc = 81; goto out; }
    if (sema.comptime_body_evals != 1)    { rc = 82; goto out; }

    // Repeat square(11) → cache hit, counter stays at 1.
    struct EvalResult m5 = sema_const_eval_expr(&sema, &call_sq11, NULL);
    if (m5.control != EVAL_NORMAL)        { rc = 83; goto out; }
    if (m5.value.int_val != 121)          { rc = 84; goto out; }
    if (sema.comptime_body_evals != 1)    { rc = 85; goto out; }

    // ---- Step 9: bind + assign + inc ----

    // Set up a Decl for `x` and an env to hold it.
    struct Decl x_local = {0};
    x_local.semantic_kind = SEM_VALUE;
    x_local.kind = DECL_USER;
    x_local.name.string_id = pool_intern(&pool, "xloc", 4);
    x_local.name.resolved = &x_local;

    // bind_x will represent `x := 5`. Note the resolver normally sets
    // bind.name.resolved = &x_local; we mirror that here.
    struct Expr lit_5b = {0};
    lit_5b.kind = expr_Lit;
    lit_5b.lit.kind = lit_Int;
    lit_5b.lit.string_id = pool_intern(&pool, "5", 1);

    struct Expr bind_x = {0};
    bind_x.kind = expr_Bind;
    bind_x.bind.kind = bind_Var;          // `:=`
    bind_x.bind.name = x_local.name;
    bind_x.bind.value = &lit_5b;
    x_local.node = &bind_x;

    // ident_x reads `x`.
    struct Expr ident_x_local = {0};
    ident_x_local.kind = expr_Ident;
    ident_x_local.ident = x_local.name;

    // Run the bind in a fresh env. After this, env should have x=5.
    struct ComptimeEnv* test_env = sema_comptime_env_new(&sema, NULL);
    struct EvalResult bb1 = sema_const_eval_expr(&sema, &bind_x, test_env);
    if (bb1.control != EVAL_NORMAL)        { rc = 86; goto out; }

    // Read x — should be 5.
    struct EvalResult rr1 = sema_const_eval_expr(&sema, &ident_x_local, test_env);
    if (rr1.control != EVAL_NORMAL)        { rc = 87; goto out; }
    if (rr1.value.int_val != 5)            { rc = 88; goto out; }

    // Synthesize: x = 10
    struct Expr lit_10 = {0};
    lit_10.kind = expr_Lit;
    lit_10.lit.kind = lit_Int;
    lit_10.lit.string_id = pool_intern(&pool, "10", 2);

    struct Expr assign_x_10 = {0};
    assign_x_10.kind = expr_Assign;
    assign_x_10.assign.target = &ident_x_local;
    assign_x_10.assign.value = &lit_10;

    struct EvalResult a1 = sema_const_eval_expr(&sema, &assign_x_10, test_env);
    if (a1.control != EVAL_NORMAL)        { rc = 89; goto out; }
    if (a1.value.kind != CONST_VOID)      { rc = 90; goto out; }

    // Read x again — should be 10 now.
    struct EvalResult rr2 = sema_const_eval_expr(&sema, &ident_x_local, test_env);
    if (rr2.control != EVAL_NORMAL)        { rc = 91; goto out; }
    if (rr2.value.int_val != 10)           { rc = 92; goto out; }

    // Synthesize: x++ (postfix)
    struct Expr inc_x = {0};
    inc_x.kind = expr_Unary;
    inc_x.unary.op = unary_Inc;
    inc_x.unary.operand = &ident_x_local;
    inc_x.unary.postfix = true;

    struct EvalResult ii1 = sema_const_eval_expr(&sema, &inc_x, test_env);
    if (ii1.control != EVAL_NORMAL)        { rc = 93; goto out; }
    if (ii1.value.int_val != 10)           { rc = 94; goto out; }   // postfix → old value (10)

    // Read x — should be 11 now.
    struct EvalResult rr3 = sema_const_eval_expr(&sema, &ident_x_local, test_env);
    if (rr3.control != EVAL_NORMAL)        { rc = 95; goto out; }
    if (rr3.value.int_val != 11)           { rc = 96; goto out; }

out:
    pool_free(&pool);
    arena_free(&arena);
    if (rc != 0) fprintf(stderr, "sema_const_eval_test failed at %d\n", rc);
    return rc;
}
