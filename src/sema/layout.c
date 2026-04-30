#include "layout.h"

#include "sema.h"
#include "sema_internal.h"
#include "target.h"
#include "type.h"
#include "../compiler/compiler.h"
#include "../parser/ast.h"

// Read the active target. Falls back to the host's defaults when called from
// a unit-test Sema that lacks a Compiler.
static struct TargetInfo target_for(struct Sema* s) {
    if (s && s->compiler) return s->compiler->target;
    return target_default_host();
}

static struct TypeLayout layout_unknown(void) {
    return (struct TypeLayout){.size = 0, .align = 0, .complete = false};
}

static struct TypeLayout layout_complete(uint64_t size, uint64_t align) {
    return (struct TypeLayout){.size = size, .align = align, .complete = true};
}

static struct TypeLayout primitive_layout(struct Sema* s, struct Type* type) {
    switch (type->kind) {
        case TYPE_BOOL: return layout_complete(1, 1);
        case TYPE_U8:
        case TYPE_I8:   return layout_complete(1, 1);
        case TYPE_U16:
        case TYPE_I16:  return layout_complete(2, 2);
        case TYPE_U32:
        case TYPE_I32:
        case TYPE_F32:  return layout_complete(4, 4);
        case TYPE_U64:
        case TYPE_I64:
        case TYPE_F64:  return layout_complete(8, 8);
        case TYPE_USIZE:
        case TYPE_ISIZE: return layout_complete(target_for(s).usize_size, target_for(s).usize_align);
        case TYPE_VOID:  return layout_complete(0, 1);
        case TYPE_NIL:   return layout_complete(target_for(s).pointer_size, target_for(s).pointer_align);
        default: return layout_unknown();
    }
}

static uint64_t align_up(uint64_t value, uint64_t align) {
    if (align <= 1) return value;
    return (value + (align - 1)) & ~(align - 1);
}

static struct TypeLayout layout_from_field_decls(struct Sema* s, struct Decl* owner) {
    struct Scope* scope = owner ? owner->child_scope : NULL;
    if (!scope || !scope->decls) return layout_complete(0, 1);

    uint64_t offset = 0;
    uint64_t struct_align = 1;
    bool any_field = false;

    for (size_t i = 0; i < scope->decls->count; i++) {
        struct Decl** field_p = (struct Decl**)vec_get(scope->decls, i);
        struct Decl* field = field_p ? *field_p : NULL;
        if (!field) continue;
        if (field->kind != DECL_FIELD) continue;
        if (field->semantic_kind != SEM_VALUE) continue;

        struct Type* field_type = sema_decl_type(s, field);
        if (!field_type) {
            // Field type not yet resolved; report and bail.
            return layout_unknown();
        }

        struct TypeLayout field_layout = sema_layout_of_type(s, field_type);
        if (!field_layout.complete) return layout_unknown();

        offset = align_up(offset, field_layout.align);
        offset += field_layout.size;
        if (field_layout.align > struct_align) struct_align = field_layout.align;
        any_field = true;
    }

    if (!any_field) return layout_complete(0, 1);

    uint64_t total = align_up(offset, struct_align);
    return layout_complete(total, struct_align);
}

static struct TypeLayout layout_struct(struct Sema* s, struct Type* type) {
    if (!type || !type->decl) return layout_unknown();
    return layout_from_field_decls(s, type->decl);
}

static struct TypeLayout layout_enum(struct Sema* s, struct Type* type) {
    (void)s; (void)type;
    return layout_complete(4, 4);
}

static struct TypeLayout compute_layout(struct Sema* s, struct Type* type) {
    if (!type) return layout_unknown();

    switch (type->kind) {
        case TYPE_BOOL:
        case TYPE_U8: case TYPE_U16: case TYPE_U32: case TYPE_U64:
        case TYPE_I8: case TYPE_I16: case TYPE_I32: case TYPE_I64:
        case TYPE_USIZE: case TYPE_ISIZE:
        case TYPE_F32: case TYPE_F64:
        case TYPE_VOID:
        case TYPE_NIL:
            return primitive_layout(s, type);

        case TYPE_POINTER:
            // Pointer layout is independent of pointee — that is what allows
            // pointer-recursive structs (Header { next : ?^Header }) to work.
            return layout_complete(target_for(s).pointer_size, target_for(s).pointer_align);

        case TYPE_SLICE:
            // {ptr, len}
            return layout_complete(target_for(s).pointer_size * 2, target_for(s).pointer_align);

        case TYPE_STRING:
            // []const u8 — same shape as a slice.
            return layout_complete(target_for(s).pointer_size * 2, target_for(s).pointer_align);

        case TYPE_ARRAY:
            // No element count tracked yet; layout incomplete until a length lands.
            return layout_unknown();

        case TYPE_STRUCT:
            return layout_struct(s, type);

        case TYPE_ENUM:
            return layout_enum(s, type);

        case TYPE_FUNCTION:
            // Functions are addresses.
            return layout_complete(target_for(s).pointer_size, target_for(s).pointer_align);

        case TYPE_SCOPE_TOKEN:
            // Comptime-only erased token; no runtime size.
            return layout_complete(0, 1);

        case TYPE_COMPTIME_INT:
        case TYPE_COMPTIME_FLOAT:
        case TYPE_TYPE:
        case TYPE_ANYTYPE:
        case TYPE_MODULE:
        case TYPE_EFFECT:
        case TYPE_EFFECT_ROW:
        case TYPE_PRODUCT:
        case TYPE_UNKNOWN:
        case TYPE_ERROR:
            return layout_unknown();
    }
    return layout_unknown();
}

static const char* type_label(struct Sema* s, struct Type* type, char* buf, size_t n) {
    return sema_type_display_name(s, type, buf, n);
}

static struct Span span_for_type(struct Type* type, struct Span fallback) {
    if (type && type->decl) return type->decl->name.span;
    return fallback;
}

struct TypeLayout sema_layout_of_type_at(struct Sema* s, struct Type* type, struct Span ref_span) {
    if (!s || !type) return layout_unknown();
    if (sema_type_is_errorish(type)) return layout_unknown();

    struct Span span = span_for_type(type, ref_span);
    QueryBeginResult begin = sema_query_begin(s, &type->layout_query,
        QUERY_LAYOUT_OF_TYPE, type, span);

    switch (begin) {
        case QUERY_BEGIN_CACHED:
            return type->layout;
        case QUERY_BEGIN_ERROR:
            return layout_unknown();
        case QUERY_BEGIN_CYCLE: {
            char name[128];
            sema_error(s, span,
                "type '%s' contains itself by value (use a pointer to break the cycle)",
                type_label(s, type, name, sizeof(name)));
            return layout_unknown();
        }
        case QUERY_BEGIN_COMPUTE:
            break;
    }

    struct TypeLayout layout = compute_layout(s, type);
    type->layout = layout;
    if (layout.complete) {
        sema_query_succeed(s, &type->layout_query);
    } else {
        sema_query_fail(s, &type->layout_query);
    }
    return layout;
}

struct TypeLayout sema_layout_of_type(struct Sema* s, struct Type* type) {
    struct Span span = {0};
    return sema_layout_of_type_at(s, type, span);
}
