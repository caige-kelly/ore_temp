#include "type.h"

#include <stdio.h>
#include <string.h>

#include "sema.h"

struct Type* sema_type_new(struct Sema* s, TypeKind kind) {
    if (!s || !s->arena) return NULL;
    struct Type* type = arena_alloc(s->arena, sizeof(struct Type));
    if (!type) return NULL;
    type->kind = kind;
    type->params = vec_new_in(s->arena, sizeof(struct Type*));
    type->effects = vec_new_in(s->arena, sizeof(struct EffectSig*));
    return type;
}

struct Type* sema_named_type(struct Sema* s, TypeKind kind, uint32_t name_id, struct Decl* decl) {
    struct Type* type = sema_type_new(s, kind);
    if (!type) return NULL;
    type->name_id = name_id;
    type->decl = decl;
    return type;
}

struct Type* sema_pointer_type(struct Sema* s, struct Type* elem) {
    struct Type* type = sema_type_new(s, TYPE_POINTER);
    if (!type) return NULL;
    type->elem = elem;
    type->region_id = elem ? elem->region_id : 0;
    return type;
}

struct Type* sema_slice_type(struct Sema* s, struct Type* elem) {
    struct Type* type = sema_type_new(s, TYPE_SLICE);
    if (!type) return NULL;
    type->elem = elem;
    return type;
}

struct Type* sema_array_type(struct Sema* s, struct Type* elem) {
    struct Type* type = sema_type_new(s, TYPE_ARRAY);
    if (!type) return NULL;
    type->elem = elem;
    return type;
}

struct Type* sema_function_type(struct Sema* s) {
    return sema_type_new(s, TYPE_FUNCTION);
}

const char* sema_type_kind_str(TypeKind kind) {
    switch (kind) {
        case TYPE_UNKNOWN:        return "unknown";
        case TYPE_ERROR:          return "error";
        case TYPE_VOID:           return "void";
        case TYPE_BOOL:           return "bool";
        case TYPE_COMPTIME_INT:   return "comptimeInt";
        case TYPE_U8:             return "u8";
        case TYPE_U16:            return "u16";
        case TYPE_U32:            return "u32";
        case TYPE_U64:            return "u64";
        case TYPE_USIZE:          return "usize";
        case TYPE_I8:             return "i8";
        case TYPE_I16:            return "i16";
        case TYPE_I32:            return "i32";
        case TYPE_I64:            return "i64";
        case TYPE_ISIZE:          return "isize";
        case TYPE_COMPTIME_FLOAT: return "comptimeFloat";              
        case TYPE_F64:            return "f64";
        case TYPE_F32:            return "f64";
        case TYPE_STRING:         return "[]const u8";
        case TYPE_NIL:            return "nil";
        case TYPE_TYPE:           return "type";
        case TYPE_ANYTYPE:        return "anytype";
        case TYPE_MODULE:         return "module";
        case TYPE_STRUCT:         return "struct";
        case TYPE_ENUM:           return "enum";
        case TYPE_EFFECT:         return "effect";
        case TYPE_EFFECT_ROW:     return "effect-row";
        case TYPE_SCOPE_TOKEN:    return "scope-token";
        case TYPE_FUNCTION:       return "function";
        case TYPE_POINTER:        return "pointer";
        case TYPE_SLICE:          return "slice";
        case TYPE_ARRAY:          return "array";
        case TYPE_PRODUCT:        return "product";
    }
    return "?";
}

const char* sema_semantic_kind_str(SemanticKind kind) {
    switch (kind) {
        case SEM_UNKNOWN:     return "unknown";
        case SEM_VALUE:       return "value";
        case SEM_TYPE:        return "type";
        case SEM_EFFECT:      return "effect";
        case SEM_MODULE:      return "module";
        case SEM_SCOPE_TOKEN: return "scope-token";
        case SEM_EFFECT_ROW:  return "effect-row";
    }
    return "?";
}

bool sema_name_is(struct Sema* s, uint32_t id, const char* name) {
    const char* got = s && s->pool ? pool_get(s->pool, id, 0) : NULL;
    return got && strcmp(got, name) == 0;
}

bool sema_type_is_errorish(struct Type* type) {
    return !type || type->kind == TYPE_UNKNOWN || type->kind == TYPE_ERROR;
}

bool sema_type_is_nominal(struct Type* type) {
    if (!type) return false;
    switch (type->kind) {
        case TYPE_MODULE:
        case TYPE_STRUCT:
        case TYPE_ENUM:
        case TYPE_EFFECT:
        case TYPE_EFFECT_ROW:
        case TYPE_SCOPE_TOKEN:
            return type->decl != NULL;
        default:
            return false;
    }
}

bool sema_type_is_type_value(struct Type* type) {
    if (!type) return false;
    switch (type->kind) {
        case TYPE_TYPE:
        case TYPE_ANYTYPE:
        case TYPE_STRUCT:
        case TYPE_ENUM:
        case TYPE_EFFECT:
        case TYPE_EFFECT_ROW:
        case TYPE_SCOPE_TOKEN:
        case TYPE_FUNCTION:
        case TYPE_POINTER:
        case TYPE_SLICE:
        case TYPE_ARRAY:
        case TYPE_PRODUCT:
            return true;
        default:
            return false;
    }
}

bool sema_type_is_numeric(struct Type* type) {
    return type && (
        type->kind == TYPE_COMPTIME_INT || 
        type->kind == TYPE_U8           ||
        type->kind == TYPE_U16          ||
        type->kind == TYPE_U32          ||
        type->kind == TYPE_U64          ||
        type->kind == TYPE_USIZE        ||
        type->kind == TYPE_I8           ||
        type->kind == TYPE_I16          ||
        type->kind == TYPE_I32          ||
        type->kind == TYPE_I64          ||
        type->kind == TYPE_ISIZE        ||
        type->kind == TYPE_F64          ||
        type->kind == TYPE_F32          ||
        type->kind == TYPE_COMPTIME_FLOAT
    );
}

bool sema_type_is_callable(struct Type* type) {
    return type && type->kind == TYPE_FUNCTION;
}

struct Type* sema_primitive_type_for_name(struct Sema* s, uint32_t id) {
    if (sema_name_is(s, id, "void")) return s->void_type;
    if (sema_name_is(s, id, "bool")) return s->bool_type;
    if (sema_name_is(s, id, "type")) return s->type_type;
    if (sema_name_is(s, id, "anytype")) return s->anytype_type;
    if (sema_name_is(s, id, "Scope")) return s->type_type;
    if (sema_name_is(s, id, "true") || sema_name_is(s, id, "false")) return s->bool_type;
    if (sema_name_is(s, id, "nil")) return s->nil_type;

    const char* name = s && s->pool ? pool_get(s->pool, id, 0) : NULL;
    if (!name) return s ? s->unknown_type : NULL;
    if (strcmp(name, "u8")) return s->u8_type;
    if (strcmp(name, "u16")) return s->u16_type;
    if (strcmp(name, "u32")) return s->u32_type;
    if (strcmp(name, "u64")) return s->u64_type;
    if (strcmp(name, "usize")) return s->usize_type;
    if (strcmp(name, "i8")) return s->i8_type;
    if (strcmp(name, "i16")) return s->i16_type;
    if (strcmp(name, "i32")) return s->i32_type;
    if (strcmp(name, "i64")) return s->i64_type;
    if (strcmp(name, "isize")) return s->isize_type;
    if (strcmp(name, "f32")) return s->f32_type;
    if (strcmp(name, "f64")) return s->f64_type;
    if (strcmp(name, "comptime_int")) return s->comptime_int_type;
    if (strcmp(name, "comptime_float")) return s->comptime_float_type;
    return s->unknown_type;
}

SemanticKind sema_semantic_for_type(struct Type* type) {
    if (!type) return SEM_UNKNOWN;
    switch (type->kind) {
        case TYPE_MODULE:      return SEM_MODULE;
        case TYPE_STRUCT:
        case TYPE_ENUM:
        case TYPE_TYPE:
        case TYPE_ANYTYPE:     return SEM_TYPE;
        case TYPE_EFFECT:      return SEM_EFFECT;
        case TYPE_EFFECT_ROW:  return SEM_EFFECT_ROW;
        case TYPE_SCOPE_TOKEN: return SEM_SCOPE_TOKEN;
        case TYPE_UNKNOWN:
        case TYPE_ERROR:       return SEM_UNKNOWN;
        default:               return SEM_VALUE;
    }
}

static void append_text(char* buffer, size_t buffer_size, size_t* used, const char* text) {
    if (!buffer || buffer_size == 0 || !used || !text) return;
    if (*used >= buffer_size) return;

    int written = snprintf(buffer + *used, buffer_size - *used, "%s", text);
    if (written < 0) return;

    size_t amount = (size_t)written;
    if (amount >= buffer_size - *used) {
        *used = buffer_size - 1;
    } else {
        *used += amount;
    }
}

const char* sema_type_display_name(struct Sema* s, struct Type* type, char* buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) return "?";
    buffer[0] = '\0';

    if (!type) {
        snprintf(buffer, buffer_size, "<null>");
        return buffer;
    }

    if (type->name_id != 0 && s && s->pool) {
        const char* name = pool_get(s->pool, type->name_id, 0);
        if (name) return name;
    }

    switch (type->kind) {
        case TYPE_POINTER: {
            char elem_name[128];
            snprintf(buffer, buffer_size, "*%s",
                sema_type_display_name(s, type->elem, elem_name, sizeof(elem_name)));
            return buffer;
        }
        case TYPE_SLICE: {
            char elem_name[128];
            snprintf(buffer, buffer_size, "[]%s",
                sema_type_display_name(s, type->elem, elem_name, sizeof(elem_name)));
            return buffer;
        }
        case TYPE_ARRAY: {
            char elem_name[128];
            snprintf(buffer, buffer_size, "[_]%s",
                sema_type_display_name(s, type->elem, elem_name, sizeof(elem_name)));
            return buffer;
        }
        case TYPE_FUNCTION: {
            size_t used = 0;
            append_text(buffer, buffer_size, &used, "fn(");
            if (type->params) {
                for (size_t i = 0; i < type->params->count; i++) {
                    struct Type** param = (struct Type**)vec_get(type->params, i);
                    char param_name[128];
                    if (i > 0) append_text(buffer, buffer_size, &used, ", ");
                    append_text(buffer, buffer_size, &used,
                        sema_type_display_name(s, param ? *param : NULL, param_name, sizeof(param_name)));
                }
            }
            append_text(buffer, buffer_size, &used, ") ");
            char ret_name[128];
            append_text(buffer, buffer_size, &used,
                sema_type_display_name(s, type->ret, ret_name, sizeof(ret_name)));
            return buffer;
        }
        default:
            snprintf(buffer, buffer_size, "%s", sema_type_kind_str(type->kind));
            return buffer;
    }
}

static bool effect_term_equal(struct EffectTerm* left, struct EffectTerm* right) {
    if (left == right) return true;
    if (!left || !right) return false;
    if (left->kind != right->kind) return false;
    if (left->decl || right->decl) return left->decl == right->decl;
    return left->name_id == right->name_id &&
        left->scope_token_id == right->scope_token_id &&
        left->row_name_id == right->row_name_id;
}

static bool effect_sig_equal(struct EffectSig* left, struct EffectSig* right) {
    if (left == right) return true;
    if (!left || !right) return false;
    if (left->is_open != right->is_open) return false;
    if (left->row_decl || right->row_decl) {
        if (left->row_decl != right->row_decl) return false;
    } else if (left->row_name_id != right->row_name_id) {
        return false;
    }
    if (left->terms == right->terms) return true;
    if (!left->terms || !right->terms || left->terms->count != right->terms->count) return false;
    for (size_t i = 0; i < left->terms->count; i++) {
        struct EffectTerm* left_term = (struct EffectTerm*)vec_get(left->terms, i);
        struct EffectTerm* right_term = (struct EffectTerm*)vec_get(right->terms, i);
        if (!effect_term_equal(left_term, right_term)) return false;
    }
    return true;
}

static bool effect_vec_equal(Vec* left, Vec* right) {
    if (left == right) return true;
    if (!left || !right || left->count != right->count) return false;
    for (size_t i = 0; i < left->count; i++) {
        struct EffectSig** left_sig = (struct EffectSig**)vec_get(left, i);
        struct EffectSig** right_sig = (struct EffectSig**)vec_get(right, i);
        if (!left_sig || !right_sig) return false;
        if (!effect_sig_equal(*left_sig, *right_sig)) return false;
    }
    return true;
}

static bool type_vec_equal(Vec* left, Vec* right) {
    if (left == right) return true;
    if (!left || !right || left->count != right->count) return false;
    for (size_t i = 0; i < left->count; i++) {
        struct Type** left_type = (struct Type**)vec_get(left, i);
        struct Type** right_type = (struct Type**)vec_get(right, i);
        if (!left_type || !right_type) return false;
        if (!sema_type_equal(*left_type, *right_type)) return false;
    }
    return true;
}

bool sema_type_equal(struct Type* left, struct Type* right) {
    if (left == right) return true;
    if (!left || !right) return false;
    if (left->kind != right->kind) return false;
    if (left->decl || right->decl) return left->decl == right->decl;

    switch (left->kind) {
        case TYPE_POINTER:
        case TYPE_SLICE:
        case TYPE_ARRAY:
            return sema_type_equal(left->elem, right->elem);
        case TYPE_FUNCTION:
            return type_vec_equal(left->params, right->params) &&
                sema_type_equal(left->ret, right->ret) &&
                effect_sig_equal(left->effect_sig, right->effect_sig) &&
                effect_vec_equal(left->effects, right->effects);
        case TYPE_SCOPE_TOKEN:
            if (left->region_id || right->region_id) return left->region_id == right->region_id;
            return left->name_id == right->name_id;
        default:
            if (left->name_id || right->name_id) return left->name_id == right->name_id;
            return true;
    }
}

bool sema_type_assignable(struct Type* expected, struct Type* actual) {
    if (!expected || !actual) return true;
    if (sema_type_is_errorish(expected) || sema_type_is_errorish(actual)) return true;
    if (expected->kind == TYPE_ANYTYPE || actual->kind == TYPE_ANYTYPE) return true;
    if (expected->kind == TYPE_TYPE && sema_type_is_type_value(actual)) return true;
    if (actual->kind == TYPE_NIL) {
        return expected->kind == TYPE_NIL || expected->kind == TYPE_POINTER || expected->kind == TYPE_SLICE;
    }
    if (expected->kind == actual->kind) {
        switch (expected->kind) {
            case TYPE_POINTER:
            case TYPE_SLICE:
            case TYPE_ARRAY:
                return sema_type_assignable(expected->elem, actual->elem);
            default:
                break;
        }
    }
    return sema_type_equal(expected, actual);
}