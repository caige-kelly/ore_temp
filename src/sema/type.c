#include "type.h"

#include <stdio.h>
#include <string.h>

#include "sema.h"

// Returns true iff `v` (a comptime numeric value) fits in `target`'s range.
// For non-comptime or non-numeric `v`, returns true (defers to type-level check).
bool sema_value_fits_type(struct ConstValue v, struct Type* target) {
    if (!target) return true;

    if (v.kind == CONST_INT) {
        int64_t val = v.int_val;
        switch (target->kind) {
            case TYPE_U8:    return val >= 0 && val <= 255;
            case TYPE_U16:   return val >= 0 && val <= 65535;
            case TYPE_U32:   return val >= 0 && val <= 4294967295LL;
            case TYPE_U64:   return val >= 0;     // int64_t max ≤ uint64_t max
            case TYPE_USIZE: {
                // Target-dependent. Use the target info on Sema/Compiler.
                // For 64-bit targets: same as u64. For 32-bit: same as u32.
                // Conservative: require non-negative and fit in u32 unless 64-bit.
                return val >= 0;
            }
            case TYPE_I8:    return val >= -128       && val <= 127;
            case TYPE_I16:   return val >= -32768     && val <= 32767;
            case TYPE_I32:   return val >= -2147483648LL && val <= 2147483647LL;
            case TYPE_I64:   return true;     // val IS int64_t; always fits
            case TYPE_ISIZE: return true;     // see usize note above
            // Floats: comptime_int can always be represented exactly in float
            // up to 2^53 (f64) or 2^24 (f32). Above that, precision loss.
            // For v1, allow all comptime_int → float and skip precision checks.
            case TYPE_F32:   return true;
            case TYPE_F64:   return true;
            case TYPE_COMPTIME_INT: return true;   // staying comptime is always fine
            default: return true;
        }
    }

    if (v.kind == CONST_FLOAT) {
        // Skip float-range checking for v1; rare in practice.
        return true;
    }

    return true;
}

struct Type* sema_type_new(struct Sema* s, TypeKind kind) {
    if (!s || !s->arena) return NULL;
    struct Type* type = arena_alloc(s->arena, sizeof(struct Type));
    if (!type) return NULL;
    type->kind = kind;
    type->name_id = 0;
    type->decl = NULL;
    type->elem = NULL;
    type->ret = NULL;
    type->params = vec_new_in(s->arena, sizeof(struct Type*));
    type->effects = vec_new_in(s->arena, sizeof(struct EffectSig*));
    type->effect_sig = NULL;
    type->region_id = 0;
    sema_query_slot_init(&type->layout_query, QUERY_LAYOUT_OF_TYPE);
    type->layout = (struct TypeLayout){0};
    type->is_optional = false;
    type->is_const = false;
    type->array_length = -1;
    return type;
}

struct Type* sema_optional_type(struct Sema* s, struct Type* inner) {
    if (!inner) return NULL;
    if (inner->is_optional) return inner;
    if (sema_type_is_errorish(inner)) return inner;
    // Shallow clone so the optional and non-optional versions stay distinct.
    struct Type* opt = arena_alloc(s->arena, sizeof(struct Type));
    if (!opt) return inner;
    *opt = *inner;
    opt->is_optional = true;
    // The optional carries its own layout cache (a future fix can specialize
    // payload+tag for non-pointer optionals; pointer optionals use null as
    // the sentinel, matching the underlying layout).
    sema_query_slot_init(&opt->layout_query, QUERY_LAYOUT_OF_TYPE);
    opt->layout = (struct TypeLayout){0};
    return opt;
}

struct Type* sema_unwrap_optional(struct Sema* s, struct Type* type) {
    if (!type || !type->is_optional) return type;
    if (!s || !s->arena) return type;
    struct Type* out = arena_alloc(s->arena, sizeof(struct Type));
    if (!out) return type;
    *out = *type;
    out->is_optional = false;
    sema_query_slot_init(&out->layout_query, QUERY_LAYOUT_OF_TYPE);
    out->layout = (struct TypeLayout){0};
    return out;
}

struct Type* sema_const_qualified_type(struct Sema* s, struct Type* inner) {
    if (!inner) return NULL;
    if (inner->is_const) return inner;
    if (sema_type_is_errorish(inner)) return inner;
    if (!s || !s->arena) return inner;
    // Shallow clone so the const and non-const versions stay distinct in
    // equality / cache keys. Layout is identical (const is a usage marker,
    // not a representation change), but resetting the query slot keeps the
    // cache key unique — matches how `sema_optional_type` handles its
    // distinct-but-equivalent-layout clone.
    struct Type* out = arena_alloc(s->arena, sizeof(struct Type));
    if (!out) return inner;
    *out = *inner;
    out->is_const = true;
    sema_query_slot_init(&out->layout_query, QUERY_LAYOUT_OF_TYPE);
    out->layout = (struct TypeLayout){0};
    return out;
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

struct Type* sema_handler_type(struct Sema* s, struct Decl* effect_decl, struct Type* ret) {
    struct Type* t = sema_type_new(s, TYPE_HANDLER);
    if (!t) return NULL;
    // Reuse Type.decl for the effect E (every HandlerOf<E,_> is keyed
    // by its E decl) and Type.ret for R. Equality compares both.
    t->decl = effect_decl;
    t->ret = ret;
    return t;
}

const char* sema_type_kind_str(TypeKind kind) {
    switch (kind) {
        case TYPE_UNKNOWN:        return "unknown";
        case TYPE_ERROR:          return "error";
        case TYPE_VOID:           return "void";
        case TYPE_NORETURN:       return "noreturn";
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
        case TYPE_F32:            return "f32";
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
        case TYPE_HANDLER:        return "handler";
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

struct Type* sema_numeric_join(struct Sema* s, struct Type* a, struct Type* b) {
    bool a_int = sema_type_is_integer(a), a_flt = sema_type_is_float(a);
    bool b_int = sema_type_is_integer(b), b_flt = sema_type_is_float(b);
    if ((a_int && b_flt) || (a_flt && b_int)) return NULL;   // caller diagnoses

    if (a->kind == b->kind) return a;
    if (a->kind == TYPE_COMPTIME_INT   && b_int) return b;
    if (b->kind == TYPE_COMPTIME_INT   && a_int) return a;
    if (a->kind == TYPE_COMPTIME_FLOAT && b_flt) return b;
    if (b->kind == TYPE_COMPTIME_FLOAT && a_flt) return a;
    return NULL;   // two unequal concretes — also an error, no widening
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

bool sema_type_is_integer(struct Type* type) {
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
        type->kind == TYPE_ISIZE 
    );
}

bool sema_type_is_float(struct Type* type) {
    return type && (
        type->kind == TYPE_F64          ||
        type->kind == TYPE_F32          ||
        type->kind == TYPE_COMPTIME_FLOAT
    );
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
    // Single hashmap lookup against the table built in `sema_new`.
    // Replaced a 22-arm `sema_name_is` chain (each call cost a
    // pool_get + strcmp). Misses fall back to `unknown_type` to
    // preserve the prior contract (callers always get a non-NULL Type).
    if (!s) return NULL;
    struct Type* t = (struct Type*)hashmap_get(&s->primitive_types, (uint64_t)id);
    return t ? t : s->unknown_type;
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

    // Optional prefix wraps any kind: `?T`, `?^T`, `?[]T`, etc.
    if (type->is_optional) {
        struct Type tmp = *type;
        tmp.is_optional = false;
        char inner[128];
        snprintf(buffer, buffer_size, "?%s",
            sema_type_display_name(s, &tmp, inner, sizeof(inner)));
        return buffer;
    }

    // `const` lives just inside the optional layer. For bare const types
    // (`const i32`) it shows here as "const i32"; for wrapped const elements
    // (`[]const u8` / `^const T`) the wrapper arm recurses into the elem,
    // which carries `is_const`, and that recursion enters this branch to
    // render "const u8" / "const T" — composes into "[]const u8" / "*const T"
    // naturally without per-wrapper-arm code.
    if (type->is_const) {
        struct Type tmp = *type;
        tmp.is_const = false;
        char inner[128];
        snprintf(buffer, buffer_size, "const %s",
            sema_type_display_name(s, &tmp, inner, sizeof(inner)));
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
            sema_type_display_name(s, type->elem, elem_name, sizeof(elem_name));
            if (type->array_length >= 0) {
                snprintf(buffer, buffer_size, "[%lld]%s",
                    (long long)type->array_length, elem_name);
            } else {
                snprintf(buffer, buffer_size, "[?]%s", elem_name);
            }
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
            append_text(buffer, buffer_size, &used, ")");
            // Effect row: render `<E>` / `<E1, E2>` / `<E | row>` / `<| row>`
            // when present so a "type mismatch" between two `fn() i32`s with
            // different effects doesn't print as the tautological
            // "expected fn() i32 but found fn() i32".
            struct EffectSig* sig = type->effect_sig;
            bool has_effect = sig && (
                (sig->terms && sig->terms->count > 0) || sig->is_open);
            if (has_effect) {
                append_text(buffer, buffer_size, &used, " <");
                bool first = true;
                if (sig->terms) {
                    for (size_t i = 0; i < sig->terms->count; i++) {
                        struct EffectTerm* t = (struct EffectTerm*)vec_get(sig->terms, i);
                        if (!t) continue;
                        if (!first) append_text(buffer, buffer_size, &used, ", ");
                        const char* nm = pool_get(s->pool, t->name_id, 0);
                        append_text(buffer, buffer_size, &used, nm ? nm : "?");
                        first = false;
                    }
                }
                if (sig->is_open) {
                    if (!first) append_text(buffer, buffer_size, &used, " | ");
                    else        append_text(buffer, buffer_size, &used, "| ");
                    const char* row = sig->row_name_id
                        ? pool_get(s->pool, sig->row_name_id, 0) : NULL;
                    append_text(buffer, buffer_size, &used, row ? row : "?");
                }
                append_text(buffer, buffer_size, &used, ">");
            }
            append_text(buffer, buffer_size, &used, " -> ");
            char ret_name[128];
            append_text(buffer, buffer_size, &used,
                sema_type_display_name(s, type->ret, ret_name, sizeof(ret_name)));
            return buffer;
        }
        case TYPE_HANDLER: {
            const char* eff_name = (type->decl && type->decl->name.string_id)
                ? pool_get(s->pool, type->decl->name.string_id, 0) : "?";
            char ret_name[128];
            if (type->ret) {
                snprintf(buffer, buffer_size, "HandlerOf<%s, %s>",
                    eff_name ? eff_name : "?",
                    sema_type_display_name(s, type->ret, ret_name, sizeof(ret_name)));
            } else {
                snprintf(buffer, buffer_size, "HandlerOf<%s>",
                    eff_name ? eff_name : "?");
            }
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
        left->scope_token_id == right->scope_token_id;
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
    if (left->is_optional != right->is_optional) return false;
    if (left->is_const != right->is_const) return false;
    // Nominal types (struct/enum/effect/module/...) are identified by
    // their decl pointer — same name+layout but defined twice are not
    // the same type. Structural types (function, pointer, slice, array)
    // compare by content even when one carries a back-pointer to a
    // decl (e.g. a typed `fn(...) -> T` lambda decl) and the other does
    // not (e.g. an inline `fn(...) -> T` annotation in a parameter).
    if (sema_type_is_nominal(left) || sema_type_is_nominal(right)) {
        if (left->decl || right->decl) return left->decl == right->decl;
    }

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
        case TYPE_HANDLER:
            // HandlerOf<E, R>: E is the effect Decl (nominal — pointer
            // equality), R is structural. NULL R = "polymorphic R, will
            // be pinned at call site" — treat NULL as a wildcard.
            if (left->decl != right->decl) return false;
            if (!left->ret || !right->ret) return true;
            return sema_type_equal(left->ret, right->ret);
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
    // void is the universal sink: any expression can flow into a void
    // slot with its value discarded. Centralizing the rule here means
    // every check (function returns, statement-position uses, handler
    // op bodies, ...) gets the same behavior automatically — no
    // call-site special-casing needed.
    if (expected->kind == TYPE_VOID) return true;
    // noreturn is the bottom type — flows into any expected type. A call to
    // a noreturn function is dead-end control, so its "value" can stand in
    // for anything the surrounding context wanted.
    if (actual->kind == TYPE_NORETURN) return true;
    if (expected->kind == TYPE_TYPE && sema_type_is_type_value(actual)) return true;
    // nil flows into any optional, plus the legacy nil-into-pointer/slice path
    // (kept until all pointer/slice types are properly marked optional).
    if (actual->kind == TYPE_NIL) {
        if (expected->is_optional) return true;
        return expected->kind == TYPE_NIL || expected->kind == TYPE_POINTER || expected->kind == TYPE_SLICE;
    }
    // T → ?T : a non-optional value flows into an optional binding.
    if (expected->is_optional && !actual->is_optional) {
        struct Type tmp = *expected;
        tmp.is_optional = false;
        return sema_type_assignable(&tmp, actual);
    }
    // ?T → T is NOT allowed without explicit unwrap. Fall through to equal.
    // T → const T : a mutable value flows into a const-qualified slot
    // (drop write capability is always safe). The reverse — const T → T —
    // is rejected because equality (which sema_type_assignable falls
    // through to) treats `is_const` as a distinguishing bit. The
    // pointer/slice/array elem-recursion below picks this up per-level so
    // `^T → ^const T` works the same way for the pointee.
    if (expected->is_const && !actual->is_const) {
        struct Type tmp = *actual;
        tmp.is_const = true;
        return sema_type_assignable(expected, &tmp);
    }
    if (actual->kind == TYPE_COMPTIME_INT && sema_type_is_numeric(expected)) return true;
    if (actual->kind == TYPE_COMPTIME_FLOAT && sema_type_is_float(expected)) return true;

    if (expected->kind == actual->kind && expected->is_optional == actual->is_optional) {
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