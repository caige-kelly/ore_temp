#include "sema/sema.h"

#include <string.h>

static int setup_sema(struct Sema* sema, Arena* arena, StringPool* pool) {
    arena_init(arena, 1024);
    pool_init(pool, 1024);

    *sema = (struct Sema){0};
    sema->arena = arena;
    sema->pool = pool;

    sema->unknown_type = sema_type_new(sema, TYPE_UNKNOWN);
    sema->error_type = sema_type_new(sema, TYPE_ERROR);
    sema->void_type = sema_type_new(sema, TYPE_VOID);
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
    sema->string_type = sema_type_new(sema, TYPE_STRING);
    sema->nil_type = sema_type_new(sema, TYPE_NIL);
    sema->type_type = sema_type_new(sema, TYPE_TYPE);
    sema->anytype_type = sema_type_new(sema, TYPE_ANYTYPE);
    sema->module_type = sema_type_new(sema, TYPE_MODULE);
    sema->effect_type = sema_type_new(sema, TYPE_EFFECT);
    sema->effect_row_type = sema_type_new(sema, TYPE_EFFECT_ROW);
    sema->scope_token_type = sema_type_new(sema, TYPE_SCOPE_TOKEN);

    return sema->scope_token_type ? 0 : 1;
}

int main(void) {
    Arena arena;
    StringPool pool;
    struct Sema sema;
    int rc = 0;
    if (setup_sema(&sema, &arena, &pool) != 0) return 1;

    pool_intern(&pool, "_", 1);
    uint32_t buffer_name = pool_intern(&pool, "Buffer", 6);

    struct Decl buffer_decl = {0};
    buffer_decl.name.string_id = buffer_name;
    buffer_decl.semantic_kind = SEM_TYPE;

    struct Decl other_buffer_decl = {0};
    other_buffer_decl.name.string_id = buffer_name;
    other_buffer_decl.semantic_kind = SEM_TYPE;

    struct Type* buffer_type = sema_named_type(&sema, TYPE_STRUCT, buffer_name, &buffer_decl);
    struct Type* same_buffer_type = sema_named_type(&sema, TYPE_STRUCT, buffer_name, &buffer_decl);
    struct Type* other_buffer_type = sema_named_type(&sema, TYPE_STRUCT, buffer_name, &other_buffer_decl);

    if (!sema_type_equal(buffer_type, same_buffer_type))      { rc = 2; goto out; }
    if (sema_type_equal(buffer_type, other_buffer_type))      { rc = 3; goto out; }

    struct Type* buffer_ptr = sema_pointer_type(&sema, buffer_type);
    struct Type* same_buffer_ptr = sema_pointer_type(&sema, same_buffer_type);
    struct Type* other_buffer_ptr = sema_pointer_type(&sema, other_buffer_type);

    if (!sema_type_equal(buffer_ptr, same_buffer_ptr))                 { rc = 4;  goto out; }
    if (sema_type_equal(buffer_ptr, other_buffer_ptr))                 { rc = 5;  goto out; }
    if (!sema_type_assignable(buffer_ptr, sema.nil_type))              { rc = 6;  goto out; }
    if (sema_type_assignable(sema.comptime_int_type, sema.nil_type))   { rc = 7;  goto out; }
    if (!sema_type_assignable(sema.unknown_type, sema.bool_type))      { rc = 8;  goto out; }
    if (!sema_type_assignable(sema.type_type, buffer_type))            { rc = 9;  goto out; }
    if (sema_type_assignable(sema.bool_type, sema.comptime_int_type))  { rc = 10; goto out; }

    struct Type* fn_type = sema_function_type(&sema);
    vec_push(fn_type->params, &sema.comptime_int_type);
    fn_type->ret = sema.bool_type;

    if (!sema_type_is_callable(fn_type)) { rc = 11; goto out; }

    char name_buf[128];
    const char* display_name = sema_type_display_name(&sema, buffer_ptr, name_buf, sizeof(name_buf));
    if (strcmp(display_name, "*Buffer") != 0) { rc = 12; goto out; }

    display_name = sema_type_display_name(&sema, fn_type, name_buf, sizeof(name_buf));
    if (strcmp(display_name, "fn(comptimeInt) bool") != 0) { rc = 13; goto out; }

out:
    pool_free(&pool);
    arena_free(&arena);
    return rc;
}