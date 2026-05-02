#include "./name_resolution.h"
#include "../compiler/compiler.h"
#include "../project/module_loader.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================
// Error reporting
// ============================================================

static void resolver_error(struct Resolver* r, struct Span span, const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (r->diags) {
        diag_error(r->diags, span, "%s", msg);
    }
    r->has_errors = true;
}

// ============================================================
// Scopes
// ============================================================

void scope_add_decl(struct Scope* scope, struct Decl* decl) {
    if (!scope || !decl) return;
    vec_push(scope->decls, &decl);
    hashmap_put(&scope->name_index, (uint64_t)decl->name.string_id, decl);
}

struct Scope* scope_new(struct Resolver* r, ScopeKind kind, struct Scope* parent) {
    struct Scope* s = arena_alloc(r->arena, sizeof(struct Scope));
    s->kind = kind;
    s->parent = parent;
    s->decls = vec_new_in(r->arena, sizeof(struct Decl*));
    hashmap_init_in(&s->name_index, r->arena);
    s->children = vec_new_in(r->arena, sizeof(struct Scope*));
    if (parent) vec_push(parent->children, &s);
    return s;
}

// Local-only lookup — used for same-scope duplicate checking.
// Does NOT walk parent chain.
static struct Decl* scope_lookup_local(struct Scope* s, uint32_t string_id) {
    if (!s) return NULL;
    return (struct Decl*)hashmap_get(&s->name_index, (uint64_t)string_id);
}

// Full lookup walks current scope and all parents.
struct Decl* scope_lookup(struct Scope* s, uint32_t string_id) {
    for (struct Scope* cur = s; cur != NULL; cur = cur->parent) {
        struct Decl* d = scope_lookup_local(cur, string_id);
        if (d) return d;
    }
    return NULL;
}

// Lookup that ALSO checks any active `with X` overlays. Overlays are
// effect scopes pushed by `with` blocks; their decls become callable
// without qualification for the duration of the `with` body.
//
// Search order: walk parent chain first (closest binding wins), then
// most-recently-pushed overlay, then older overlays. This matches user
// intuition: a local declaration shadows an effect operation of the
// same name.
static struct Decl* scope_lookup_with_overlays(struct Resolver* r, uint32_t string_id) {
    struct Decl* d = scope_lookup(r->current, string_id);
    if (d) return d;

    if (r->with_imports) {
        for (size_t i = r->with_imports->count; i > 0; i--) {
            struct Scope** sp = (struct Scope**)vec_get(r->with_imports, i - 1);
            if (!sp || !*sp) continue;
            d = scope_lookup_local(*sp, string_id);
            if (d) return d;
        }
    }
    return NULL;
}

// ============================================================
// Decl allocation
// ============================================================

// Allocate a new Decl wired into the given scope.
// Returns NULL on duplicate (and emits an error).
static struct Decl* decl_new(struct Resolver* r, struct Scope* owner,
                             DeclKind kind, struct Identifier* name,
                             struct Expr* node) {
    if (!name) return NULL;

    // Same-scope duplicate check.
    struct Decl* existing = scope_lookup_local(owner, name->string_id);
    if (existing) {
        const char* nm = pool_get(r->pool, name->string_id, 0);
        resolver_error(r, name->span,
            "'%s' is already defined in this scope", nm ? nm : "?");
        return NULL;
    }

    struct Decl* d = arena_alloc(r->arena, sizeof(struct Decl));
    d->kind = kind;
    d->semantic_kind = SEM_UNKNOWN;
    d->name = *name;
    d->name.resolved = d;       // self-reference for the canonical decl identifier
    name->resolved = d;
    d->node = node;
    d->owner = owner;
    d->child_scope = NULL;
    d->module = NULL;
    // Sema-only fields (type, effect_sig, query slots) now live on
    // Sema.decl_info, populated lazily on first sema lookup.
    d->is_comptime = false;
    d->is_export = false;
    d->has_effects = false;
    d->scope_token_id = 0;
    scope_add_decl(owner, d);
    if (r->handler_body_depth > 0 && r->compiler) {
        // Mark this decl as a handler-op implementation. Sema later reads
        // this set to skip the standard sig-vs-body effect check on it.
        // Sentinel value: any non-NULL pointer (we use the decl itself).
        hashmap_put(&r->compiler->handler_impl_decls,
            (uint64_t)(uintptr_t)d, (void*)d);
    }
    return d;
}

static SemanticKind semantic_kind_for_decl_value(struct Expr* value, DeclKind decl_kind) {
    if (decl_kind == DECL_IMPORT) return SEM_MODULE;
    if (!value) return SEM_VALUE;
    switch (value->kind) {
        case expr_Effect: return SEM_EFFECT;
        case expr_Struct:
        case expr_Enum:
            return SEM_TYPE;
        default:
            return SEM_VALUE;
    }
}

static struct Decl* ensure_implicit_decl(struct Resolver* r, struct Scope* owner,
                                         DeclKind kind, SemanticKind semantic_kind,
                                         struct Identifier* id, struct Expr* node) {
    if (!id || id->string_id == 0) return NULL;

    struct Decl* existing = scope_lookup_local(owner, id->string_id);
    if (existing) {
        if (existing->semantic_kind != semantic_kind) {
            const char* nm = pool_get(r->pool, id->string_id, 0);
            resolver_error(r, id->span,
                "'%s' is already bound with an incompatible semantic kind",
                nm ? nm : "?");
        }
        id->resolved = existing;
        return existing;
    }

    struct Decl* d = decl_new(r, owner, kind, id, node);
    if (!d) return NULL;
    d->semantic_kind = semantic_kind;
    d->is_comptime = true;
    d->is_export = false;
    if (kind == DECL_SCOPE_PARAM) {
        d->scope_token_id = r->next_scope_token_id++;
    }
    id->resolved = d;
    return d;
}

// ============================================================
// Module/import helpers
// ============================================================

static bool is_builtin_named(struct Resolver* r, struct Expr* expr, const char* name) {
    if (!expr || expr->kind != expr_Builtin) return false;
    uint32_t id = pool_intern(r->pool, name, strlen(name));
    return expr->builtin.name_id == id;
}

static bool is_import_expr(struct Resolver* r, struct Expr* expr) {
    return is_builtin_named(r, expr, "import");
}

static struct Expr* import_path_arg(struct Resolver* r, struct Expr* expr) {
    if (!is_import_expr(r, expr)) return NULL;
    if (!expr->builtin.args || expr->builtin.args->count != 1) {
        resolver_error(r, expr->span, "@import expects exactly one string literal path");
        return NULL;
    }

    struct Expr** arg = (struct Expr**)vec_get(expr->builtin.args, 0);
    if (!arg || !*arg || (*arg)->kind != expr_Lit || (*arg)->lit.kind != lit_String) {
        resolver_error(r, expr->span, "@import path must be a string literal");
        return NULL;
    }
    return *arg;
}

static struct Module* module_find(struct Resolver* r, uint32_t path_id) {
    if (!r->compiler || !r->compiler->module_map) return NULL;
    return hashmap_get(r->compiler->module_map, path_id);
}

static struct Module* module_new(struct Resolver* r, uint32_t path_id, Vec* ast) {
    struct Module* mod = arena_alloc(r->arena, sizeof(struct Module));
    mod->path_id = path_id;
    mod->scope = scope_new(r, SCOPE_MODULE, NULL);
    mod->exports = vec_new_in(r->arena, sizeof(struct Decl*));
    mod->ast = ast;
    mod->resolving = false;
    mod->resolved = false;
    vec_push(r->compiler->modules, &mod);
    hashmap_put(r->compiler->module_map, path_id, mod);
    return mod;
}

static void report_import_cycle(struct Resolver* r, struct Module* repeated, struct Span span) {
    char msg[256];
    const char* path = pool_get(r->pool, repeated->path_id, 0);
    snprintf(msg, sizeof(msg), "circular import involving %s", path ? path : "<unknown>");
    resolver_error(r, span, "%s", msg);
}

static bool resolve_module(struct Resolver* r, struct Module* mod);

static struct Module* load_imported_module(struct Resolver* r, struct Expr* import_expr) {
    struct Expr* path_arg = import_path_arg(r, import_expr);
    if (!path_arg) return NULL;

    const char* raw_path = pool_get(r->pool, path_arg->lit.string_id, 0);
    const char* importer_path = r->current_module
        ? pool_get(r->pool, r->current_module->path_id, 0)
        : pool_get(r->pool, r->root_path_id, 0);
    Arena* path_arena = r->compiler ? &r->compiler->scratch_arena : NULL;
    ArenaMark path_mark = path_arena ? arena_mark(path_arena) : (ArenaMark){0};
    char* canonical_path = ore_resolve_import_path_in(path_arena, importer_path, raw_path);
    if (!canonical_path) {
        if (path_arena) arena_reset_to(path_arena, path_mark);
        resolver_error(r, import_expr->span,
            "could not resolve import path '%s'", raw_path ? raw_path : "?");
        return NULL;
    }
    if (path_arena) arena_reset_to(path_arena, path_mark);

    uint32_t path_id = pool_intern(r->pool, canonical_path, strlen(canonical_path));
    struct Module* mod = module_find(r, path_id);
    if (!mod) {
        Vec* ast = ore_parse_file(r->compiler, canonical_path, r->compiler->next_file_id++);
        if (!ast) {
            resolver_error(r, import_expr->span,
                "could not parse imported module '%s'", canonical_path);
            free(canonical_path);
            return NULL;
        }
        mod = module_new(r, path_id, ast);
    }

    free(canonical_path);
    if (mod->resolving) {
        report_import_cycle(r, mod, import_expr->span);
        return NULL;
    }
    if (!resolve_module(r, mod)) return NULL;
    return mod;
}

static bool resolve_import_binding(struct Resolver* r, struct Decl* decl, struct Expr* bind_expr) {
    if (!decl || !bind_expr || bind_expr->kind != expr_Bind) return false;
    struct Expr* import_expr = bind_expr->bind.value;
    if (!is_import_expr(r, import_expr)) return false;

    struct Module* mod = load_imported_module(r, import_expr);
    if (!mod) return false;

    decl->module = mod;
    decl->child_scope = mod->scope;
    return true;
}

// ============================================================
// Primitives
// ============================================================

static void register_primitive(struct Resolver* r, const char* name) {
    struct Identifier id = {0};
    id.string_id = pool_intern(r->pool, name, strlen(name));
    id.span = (struct Span){0};
    struct Decl* d = decl_new(r, r->root, DECL_PRIMITIVE, &id, NULL);
    if (d) {
        d->is_comptime = true;
        d->semantic_kind = (strcmp(name, "true") == 0 ||
                            strcmp(name, "false") == 0 ||
                            strcmp(name, "nil") == 0)
            ? SEM_VALUE
            : SEM_TYPE;
    }
}

void register_primitives(struct Resolver* r) {
    static const char* prims[] = {
        "i8", "i16", "i32", "i64", "isize",
        "u8", "u16", "u32", "u64", "usize",
        "f32", "f64",
        "bool", "void", "noreturn",
        "type", "anytype", "Scope",
        "true", "false", "nil",
    };
    for (size_t i = 0; i < sizeof(prims) / sizeof(prims[0]); i++) {
        register_primitive(r, prims[i]);
    }
}

// ============================================================
// Pass 1: collect top-level decls
// ============================================================

// Add operation NAMES from an effect declaration into its child_scope.
// Type expressions are not resolved here — Pass 2 walks them.
static void EffectExpr_seed_decls(struct Resolver* r, struct Scope* scope, struct EffectExpr* eff) {
    if (!eff || !eff->operations) return;
    for (size_t i = 0; i < eff->operations->count; i++) {
        struct Expr** op_p = (struct Expr**)vec_get(eff->operations, i);
        if (!op_p || !*op_p) continue;
        struct Expr* op = *op_p;
        if (op->kind == expr_Bind) {
            struct Decl* d = decl_new(r, scope, DECL_FIELD, &op->bind.name, op);
            if (d) d->semantic_kind = SEM_VALUE;
        }
    }
}

// Add field/union-variant NAMES from a struct decl into its child_scope.
static void StructExpr_seed_decls(struct Resolver* r, struct Scope* scope, struct StructExpr* st) {
    if (!st || !st->members) return;
    for (size_t i = 0; i < st->members->count; i++) {
        struct StructMember* m = (struct StructMember*)vec_get(st->members, i);
        if (!m) continue;
        if (m->kind == member_Field) {
            struct Decl* d = decl_new(r, scope, DECL_FIELD, &m->field.name, NULL);
            if (d) d->semantic_kind = SEM_VALUE;
        } else if (m->kind == member_Union && m->union_def.variants) {
            for (size_t j = 0; j < m->union_def.variants->count; j++) {
                struct FieldDef* f = (struct FieldDef*)vec_get(m->union_def.variants, j);
                if (f) {
                    struct Decl* d = decl_new(r, scope, DECL_FIELD, &f->name, NULL);
                    if (d) d->semantic_kind = SEM_TYPE;
                }
            }
        }
    }
}

// Add variant NAMES from an enum into its child_scope.
static void EnumExpr_seed_decls(struct Resolver* r, struct Scope* scope, struct EnumExpr* en) {
    if (!en || !en->variants) return;
    for (size_t i = 0; i < en->variants->count; i++) {
        struct EnumVariant* v = (struct EnumVariant*)vec_get(en->variants, i);
        if (v) {
            struct Decl* d = decl_new(r, scope, DECL_FIELD, &v->name, NULL);
            if (d) d->semantic_kind = SEM_VALUE;
        }
    }
}

// Determine the scope kind that a top-level value introduces.
// Returns SCOPE_MODULE (i.e. "no child scope") if the bind doesn't
// introduce a new scope.
static ScopeKind child_scope_kind_for(struct Expr* value) {
    if (!value) return SCOPE_MODULE;
    switch (value->kind) {
        case expr_Lambda: return SCOPE_FUNCTION;
        case expr_Struct: return SCOPE_STRUCT;
        case expr_Enum:   return SCOPE_ENUM;
        case expr_Effect: return SCOPE_EFFECT;
        default: return SCOPE_MODULE;
    }
}

// Walk a top-level AST expression and register module-level decls.
// Only handles Bind and DestructureBind at the top level — anything
// else is silently ignored (the parser shouldn't emit it at module
// scope, but we don't error so we stay robust).
void collect_decl(struct Resolver* r, struct Expr* expr) {
    if (!expr) return;

    if (expr->kind == expr_DestructureBind) {
        // `.{a, b} :: rhs` at module scope — declare each pattern name as
        // a top-level user decl. Pass 2's expr_DestructureBind handler
        // gates on `is_local`, so without this collection step the names
        // would silently never enter scope.
        struct Expr* pattern = expr->destructure.pattern;
        if (!pattern || pattern->kind != expr_Product) return;
        struct ProductExpr* prod = &pattern->product;
        if (!prod->Fields) return;
        for (size_t i = 0; i < prod->Fields->count; i++) {
            struct ProductField* f = (struct ProductField*)vec_get(prod->Fields, i);
            if (!f || !f->value || f->value->kind != expr_Ident) continue;
            struct Decl* d = decl_new(r, r->current, DECL_USER,
                                      &f->value->ident, expr);
            if (!d) continue;
            d->is_comptime = expr->destructure.is_const;
            d->is_export = true;  // top-level: exported by default (until `pub`)
            d->semantic_kind = SEM_VALUE;
        }
        return;
    }

    if (expr->kind == expr_Bind) {
        struct BindExpr* b = &expr->bind;
        bool import_binding = is_import_expr(r, b->value);
        DeclKind kind = import_binding ? DECL_IMPORT : DECL_USER;
        struct Decl* d = decl_new(r, r->current, kind, &b->name, expr);
        if (!d) return;

        d->is_comptime = (b->kind == bind_Const);
        // TODO(pub): when we flip to "private by default", set
        //   d->is_export = b->is_pub;
        // For now the `pub` keyword is recorded on the AST but every
        // top-level is exported. b->is_pub is consulted by module-graph
        // code that already wants the future-correct behavior.
        d->is_export = true;
        d->semantic_kind = semantic_kind_for_decl_value(b->value, kind);

        if (import_binding) {
            return;
        }

        // Detect effects: if value is a Lambda with a non-NULL effect
        // annotation, mark the decl. The comptime guard reads this.
        if (b->value && b->value->kind == expr_Lambda &&
            b->value->lambda.effect != NULL) {
            d->has_effects = true;
        }

        // Allocate the child scope eagerly when the value introduces one.
        ScopeKind k = child_scope_kind_for(b->value);
        if (k != SCOPE_MODULE) {
            d->child_scope = scope_new(r, k, r->current);
        }

        // Pre-populate type bodies so their members are visible to ALL
        // Pass 2 references regardless of source order. Without this,
        // a function declared above an effect that references its
        // operations (via `with X` / overlay imports) would see the
        // effect's child_scope as empty.
        if (b->value && b->value->kind == expr_Effect && d->child_scope) {
            EffectExpr_seed_decls(r, d->child_scope, &b->value->effect_expr);
        } else if (b->value && b->value->kind == expr_Struct && d->child_scope) {
            StructExpr_seed_decls(r, d->child_scope, &b->value->struct_expr);
        } else if (b->value && b->value->kind == expr_Enum && d->child_scope) {
            EnumExpr_seed_decls(r, d->child_scope, &b->value->enum_expr);
        }
        return;
    }

    // Anything else at module top level (calls, assignments, etc.)
    // is not a declaration. Pass 2 will still walk it for diagnostics.
}

// ============================================================
// Pass 2: resolve references
// ============================================================
//
// This walks the entire AST and, for each Identifier reference, sets
// ident->resolved via scope_lookup against the current scope chain.
//
// Step 2 does NOT push function/struct/block scopes yet — all lookups
// hit the module scope only. References to function params and locals
// are expected to remain unresolved at this stage. They become
// resolvable in Steps 3-6 as scope-pushing is added.
//
// Skip rules:
//   - Builtin.name_id  — not a scope name (e.g. @sizeOf, @target)
//   - Field.field      — member name, resolved later through types
//   - EnumRef.name     — variant name, deferred to type checking
//   - Bind.name        — declaration site, not a reference
//   - Lambda Param.name — declaration site
//   - Capture idents   — declaration site for unwrap captures

static void resolve_lambda_into(struct Resolver* r, struct Expr* lambda, struct Scope* fn_scope);

static struct Decl* resolved_decl_for_expr(struct Expr* expr) {
    if (!expr) return NULL;
    switch (expr->kind) {
        case expr_Ident:
            return expr->ident.resolved;
        case expr_Field:
            return expr->field.field.resolved;
        default:
            return NULL;
    }
}

static bool decl_is_scoped_effect(struct Decl* d) {
    if (!d || d->semantic_kind != SEM_EFFECT || !d->node) return false;
    if (d->node->kind != expr_Bind || !d->node->bind.value) return false;
    struct Expr* value = d->node->bind.value;
    return value->kind == expr_Effect && value->effect_expr.is_scoped;
}

static void declare_effect_annotation_params(struct Resolver* r, struct Scope* owner,
                                             struct Expr* expr, struct Expr* node) {
    if (!expr || !owner) return;

    switch (expr->kind) {
        case expr_EffectRow:
            ensure_implicit_decl(r, owner, DECL_EFFECT_ROW, SEM_EFFECT_ROW,
                &expr->effect_row.row, node);
            declare_effect_annotation_params(r, owner, expr->effect_row.head, node);
            return;
        case expr_Call:
            // Scoped effect instantiation: Allocator(s). The effect callee
            // resolves normally; bare identifier args become fresh comptime
            // Scope tokens local to this signature.
            if (expr->call.args) {
                for (size_t i = 0; i < expr->call.args->count; i++) {
                    struct Expr** arg = (struct Expr**)vec_get(expr->call.args, i);
                    if (!arg || !*arg) continue;
                    if ((*arg)->kind == expr_Ident) {
                        ensure_implicit_decl(r, owner, DECL_SCOPE_PARAM,
                            SEM_SCOPE_TOKEN, &(*arg)->ident, node);
                    } else {
                        declare_effect_annotation_params(r, owner, *arg, node);
                    }
                }
            }
            declare_effect_annotation_params(r, owner, expr->call.callee, node);
            return;
        case expr_Bin:
            declare_effect_annotation_params(r, owner, expr->bin.Left, node);
            declare_effect_annotation_params(r, owner, expr->bin.Right, node);
            return;
        case expr_Field:
            declare_effect_annotation_params(r, owner, expr->field.object, node);
            return;
        default:
            return;
    }
}

static void validate_effect_annotation_expr(struct Resolver* r, struct Expr* expr) {
    if (!expr) return;

    switch (expr->kind) {
        case expr_EffectRow:
            if (expr->effect_row.row.string_id != 0) {
                struct Decl* row = expr->effect_row.row.resolved;
                if (!row || row->semantic_kind != SEM_EFFECT_ROW) {
                    const char* nm = pool_get(r->pool, expr->effect_row.row.string_id, 0);
                    resolver_error(r, expr->effect_row.row.span,
                        "'%s' must be an effect-row variable", nm ? nm : "?");
                }
            }
            validate_effect_annotation_expr(r, expr->effect_row.head);
            return;
        case expr_Ident:
        case expr_Field: {
            struct Decl* d = resolved_decl_for_expr(expr);
            if (d && d->semantic_kind != SEM_EFFECT &&
                d->semantic_kind != SEM_EFFECT_ROW) {
                const char* nm = pool_get(r->pool, d->name.string_id, 0);
                resolver_error(r, expr->span,
                    "'%s' is not an effect", nm ? nm : "?");
            }
            return;
        }
        case expr_Call: {
            validate_effect_annotation_expr(r, expr->call.callee);
            struct Decl* callee = resolved_decl_for_expr(expr->call.callee);
            if (callee && callee->semantic_kind == SEM_EFFECT &&
                !decl_is_scoped_effect(callee)) {
                const char* nm = pool_get(r->pool, callee->name.string_id, 0);
                resolver_error(r, expr->span,
                    "effect '%s' is not scoped and cannot take a Scope token",
                    nm ? nm : "?");
            }
            if (expr->call.args) {
                for (size_t i = 0; i < expr->call.args->count; i++) {
                    struct Expr** arg = (struct Expr**)vec_get(expr->call.args, i);
                    if (!arg || !*arg) continue;
                    struct Decl* ad = resolved_decl_for_expr(*arg);
                    if (!ad || ad->semantic_kind != SEM_SCOPE_TOKEN) {
                        resolver_error(r, (*arg)->span,
                            "scoped effect argument must be a comptime Scope token");
                    }
                }
            }
            return;
        }
        case expr_Bin:
            validate_effect_annotation_expr(r, expr->bin.Left);
            validate_effect_annotation_expr(r, expr->bin.Right);
            return;
        default:
            return;
    }
}

static void resolve_effect_annotation(struct Resolver* r, struct Scope* owner,
                                      struct Expr* effect, struct Expr* node) {
    if (!effect) return;
    declare_effect_annotation_params(r, owner, effect, node);
    r->effect_annotation_depth++;
    resolve_expr(r, effect);
    r->effect_annotation_depth--;
    validate_effect_annotation_expr(r, effect);
}

// Walk a type/effect-annotation expression collecting any referenced
// effect's child_scope. Pushes onto out_scopes (Vec of struct Scope*).
// Returns the count pushed so callers can pop the same number.
//
// Recurses into nested Lambda type expressions so a param's type like
// `fn(void) <Allocator(s) | e>` reaches the inner effect row.
static int collect_effect_scopes(struct Expr* eff, Vec* out_scopes) {
    if (!eff || !out_scopes) return 0;
    int pushed = 0;
    struct Expr* stack[64];
    int top = 0;
    stack[top++] = eff;
    while (top > 0) {
        struct Expr* e = stack[--top];
        if (!e) continue;

        struct Decl* resolved = resolved_decl_for_expr(e);
        if (resolved && resolved->child_scope &&
            resolved->child_scope->kind == SCOPE_EFFECT) {
            struct Scope* s = resolved->child_scope;
            vec_push(out_scopes, &s);
            pushed++;
            continue;
        }

        switch (e->kind) {
            case expr_EffectRow:
                if (top + 1 <= 64 && e->effect_row.head) stack[top++] = e->effect_row.head;
                break;
            case expr_Bin:
                if (top + 2 <= 64) {
                    if (e->bin.Left)  stack[top++] = e->bin.Left;
                    if (e->bin.Right) stack[top++] = e->bin.Right;
                }
                break;
            case expr_Call:
                if (top + 1 <= 64 && e->call.callee) stack[top++] = e->call.callee;
                break;
            case expr_Field:
                if (top + 1 <= 64 && e->field.object) stack[top++] = e->field.object;
                break;
            case expr_Lambda:
                // Recurse into the inner lambda's effect annotation
                // and ALSO each param's type_ann (effects can be on
                // either side in handler signatures).
                if (e->lambda.effect && top + 1 <= 64) stack[top++] = e->lambda.effect;
                if (e->lambda.params) {
                    for (size_t i = 0; i < e->lambda.params->count; i++) {
                        struct Param* p = (struct Param*)vec_get(e->lambda.params, i);
                        if (p && p->type_ann && top + 1 <= 64) stack[top++] = p->type_ann;
                    }
                }
                if (e->lambda.ret_type && top + 1 <= 64) stack[top++] = e->lambda.ret_type;
                break;
            default:
                break;
        }
    }
    return pushed;
}

// Pre-interned ids for Bind names that are actually keywords in
// disguise (the parser wraps `initially` / `finally` blocks in fake
// Binds so they ride through resolve_expr like everything else).
static uint32_t INITIALLY_ID = (uint32_t)-1;
static uint32_t FINALLY_ID   = (uint32_t)-1;

static bool is_handler_lifecycle_bind(struct Resolver* r, uint32_t string_id) {
    if (INITIALLY_ID == (uint32_t)-1) {
        INITIALLY_ID = pool_intern(r->pool, "initially", 9);
        FINALLY_ID   = pool_intern(r->pool, "finally", 7);
    }
    return string_id == INITIALLY_ID || string_id == FINALLY_ID;
}

static void resolve_expr_inner(struct Resolver* r, struct Expr* expr) {
    switch (expr->kind) {
        case expr_Lit:
        case expr_Asm:
        case expr_Wildcard:
            // Leaves with no children to walk. Wildcard introduces no
            // binding and resolves to nothing — it's a pattern-position
            // placeholder.
            return;

        case expr_Break:
            if (r->loop_body_depth == 0) {
                resolver_error(r, expr->span, "break used outside of a loop");
            }
            return;

        case expr_Continue:
            if (r->loop_body_depth == 0) {
                resolver_error(r, expr->span, "continue used outside of a loop");
            }
            return;

        case expr_Ident: {
            // Lookup walks the parent chain AND any active `with X`
            // effect-scope overlays so operations like `panic` resolve
            // when an enclosing `with Exn` is active.
            struct Decl* d = scope_lookup_with_overlays(r, expr->ident.string_id);
            if (d) {
                expr->ident.resolved = d;
                if ((d->semantic_kind == SEM_SCOPE_TOKEN ||
                     d->semantic_kind == SEM_EFFECT_ROW) &&
                    r->effect_annotation_depth == 0) {
                    const char* nm = pool_get(r->pool, d->name.string_id, 0);
                    resolver_error(r, expr->span,
                        "'%s' is a comptime effect token and cannot be used as a runtime value",
                        nm ? nm : "?");
                }
                // Comptime guard: effectful function references are
                // forbidden inside comptime expressions.
                if (r->comptime_depth > 0 && d->has_effects) {
                    const char* nm = pool_get(r->pool, d->name.string_id, 0);
                    resolver_error(r, expr->span,
                        "effectful function '%s' is not allowed in comptime context",
                        nm ? nm : "?");
                }
            }
            // Note: we don't error on miss yet. Final pass will treat
            // residual misses as errors.
            return;
        }

        case expr_Bin:
            resolve_expr(r, expr->bin.Left);
            resolve_expr(r, expr->bin.Right);
            return;

        case expr_Assign:
            resolve_expr(r, expr->assign.target);
            resolve_expr(r, expr->assign.value);
            return;

        case expr_Unary:
            resolve_expr(r, expr->unary.operand);
            return;

        case expr_Call:
            resolve_expr(r, expr->call.callee);
            if (expr->call.args) {
                for (size_t i = 0; i < expr->call.args->count; i++) {
                    struct Expr** a = (struct Expr**)vec_get(expr->call.args, i);
                    if (a) resolve_expr(r, *a);
                }
            }
            return;

        case expr_Builtin:
            // The builtin name itself is not a scope reference. Just walk args.
            if (is_import_expr(r, expr)) {
                resolver_error(r, expr->span,
                    "@import must be used as a module-level alias, e.g. name :: @import(\"file.ore\")");
                return;
            }
            if (expr->builtin.args) {
                for (size_t i = 0; i < expr->builtin.args->count; i++) {
                    struct Expr** a = (struct Expr**)vec_get(expr->builtin.args, i);
                    if (a) resolve_expr(r, *a);
                }
            }
            return;

        case expr_EffectRow:
            resolve_expr(r, expr->effect_row.head);
            if (expr->effect_row.row.string_id != 0 &&
                expr->effect_row.row.resolved == NULL) {
                expr->effect_row.row.resolved = scope_lookup(r->current,
                    expr->effect_row.row.string_id);
            }
            return;

        case expr_If: {
            // Condition resolves in current scope.
            resolve_expr(r, expr->if_expr.condition);

            // If there's an unwrap capture, push a SCOPE_BLOCK and
            // declare the capture name. The then-branch sees it; the
            // else-branch does not.
            if (expr->if_expr.capture.string_id != 0) {
                struct Scope* cap_scope = scope_new(r, SCOPE_BLOCK, r->current);
                struct Scope* saved = r->current;
                r->current = cap_scope;
                struct Decl* cap_decl = decl_new(r, cap_scope, DECL_USER,
                    &expr->if_expr.capture, expr);
                if (cap_decl) cap_decl->semantic_kind = SEM_VALUE;
                resolve_expr(r, expr->if_expr.then_branch);
                r->current = saved;
            } else {
                resolve_expr(r, expr->if_expr.then_branch);
            }
            // else-branch never sees the capture.
            resolve_expr(r, expr->if_expr.else_branch);
            return;
        }

        case expr_Switch:
            resolve_expr(r, expr->switch_expr.scrutinee);
            if (expr->switch_expr.arms) {
                for (size_t i = 0; i < expr->switch_expr.arms->count; i++) {
                    struct SwitchArm* arm = (struct SwitchArm*)vec_get(expr->switch_expr.arms, i);
                    if (!arm) continue;
                    if (arm->patterns) {
                        for (size_t j = 0; j < arm->patterns->count; j++) {
                            struct Expr** p = (struct Expr**)vec_get(arm->patterns, j);
                            if (p) resolve_expr(r, *p);
                        }
                    }
                    resolve_expr(r, arm->body);
                }
            }
            return;

        case expr_Block: {
            // Push a fresh SCOPE_BLOCK so locals declared inside don't
            // leak. Lambda bodies bypass this case via resolve_lambda_into.
            struct Scope* block_scope = scope_new(r, SCOPE_BLOCK, r->current);
            struct Scope* saved = r->current;
            r->current = block_scope;

            Vec* stmts = expr->block.stmts;
            for (size_t i = 0; i < stmts->count; i++) {
                struct Expr** s = (struct Expr**)vec_get(stmts, i);
                if (s) resolve_expr(r, *s);
            }

            r->current = saved;
            return;
        }

        case expr_Product:
            resolve_expr(r, expr->product.type_expr);
            if (expr->product.Fields) {
                for (size_t i = 0; i < expr->product.Fields->count; i++) {
                    struct ProductField* f = (struct ProductField*)vec_get(expr->product.Fields, i);
                    if (f) resolve_expr(r, f->value);
                }
            }
            return;

        case expr_Bind: {
            // initially/finally are not real Binds — the parser wraps
            // each as one so it rides through here. Walk the body's
            // stmts inline in the current (handler) scope so locals
            // declared in `initially` are visible in `finally`.
            if (is_handler_lifecycle_bind(r, expr->bind.name.string_id)) {
                struct Expr* body = expr->bind.value;
                if (body && body->kind == expr_Block) {
                    Vec* stmts = body->block.stmts;
                    for (size_t i = 0; i < stmts->count; i++) {
                        struct Expr** st = (struct Expr**)vec_get(stmts, i);
                        if (st) resolve_expr(r, *st);
                    }
                } else {
                    resolve_expr(r, body);
                }
                return;
            }

            if (r->current == r->root && is_import_expr(r, expr->bind.value)) {
                struct Decl* import_decl = scope_lookup_local(r->current, expr->bind.name.string_id);
                if (!import_decl || import_decl->kind != DECL_IMPORT) {
                    resolver_error(r, expr->span, "@import must be bound to a module-level alias");
                    return;
                }
                resolve_import_binding(r, import_decl, expr);
                return;
            }

            // Type annotation always resolves in the current scope.
            resolve_expr(r, expr->bind.type_ann);

            // Inner Binds (not at module root) introduce a new local
            // Decl in the current scope. Top-level Binds were already
            // collected by collect_decl, so we don't re-declare them.
            // Note: order matters with self-reference. For bind_Const
            // (functions allowed to recurse), declare BEFORE resolving
            // the value. For bind_Var, declare AFTER (so `x := x + 1`
            // doesn't reference itself recursively).
            bool is_local = (r->current != r->root);
            struct Decl* local_decl = NULL;
            if (is_local && expr->bind.kind == bind_Const) {
                local_decl = decl_new(r, r->current, DECL_USER,
                                      &expr->bind.name, expr);
                if (local_decl) {
                    local_decl->is_comptime = true;
                    local_decl->is_export = false;
                    local_decl->semantic_kind = semantic_kind_for_decl_value(expr->bind.value, DECL_USER);
                }
            }

            // Walk the value. Several kinds (Lambda, Struct, Enum,
            // Effect) own a child_scope that we push before recursing.
            struct Decl* owning_decl = local_decl
                ? local_decl
                : scope_lookup_local(r->current, expr->bind.name.string_id);

            if (expr->bind.value && expr->bind.value->kind == expr_Lambda) {
                struct Scope* fn_scope = owning_decl ? owning_decl->child_scope : NULL;
                if (!fn_scope) {
                    fn_scope = scope_new(r, SCOPE_FUNCTION, r->current);
                    if (owning_decl) owning_decl->child_scope = fn_scope;
                }
                resolve_lambda_into(r, expr->bind.value, fn_scope);
            } else if (expr->bind.value && (expr->bind.value->kind == expr_Struct ||
                                             expr->bind.value->kind == expr_Enum ||
                                             expr->bind.value->kind == expr_Effect)) {
                // Push the type's child_scope and walk members in it.
                ScopeKind sk = expr->bind.value->kind == expr_Struct ? SCOPE_STRUCT
                            : expr->bind.value->kind == expr_Enum   ? SCOPE_ENUM
                            : SCOPE_EFFECT;
                struct Scope* type_scope = owning_decl ? owning_decl->child_scope : NULL;
                if (!type_scope) {
                    type_scope = scope_new(r, sk, r->current);
                    if (owning_decl) owning_decl->child_scope = type_scope;
                }
                struct Scope* saved = r->current;
                r->current = type_scope;
                resolve_expr(r, expr->bind.value);
                r->current = saved;
            } else {
                resolve_expr(r, expr->bind.value);
            }

            // bind_Var (and bind_Typed without :: form) declares AFTER
            // RHS so self-reference fails.
            if (is_local && expr->bind.kind != bind_Const) {
                struct Decl* d = decl_new(r, r->current, DECL_USER,
                                          &expr->bind.name, expr);
                if (d) {
                    d->is_comptime = false;
                    d->is_export = false;
                    d->semantic_kind = semantic_kind_for_decl_value(expr->bind.value, DECL_USER);
                }
            }
            return;
        }

        case expr_DestructureBind: {
            // Walk the value first so RHS doesn't see the new names.
            resolve_expr(r, expr->destructure.value);

            // Top-level destructures are already collected. For local
            // (non-root) destructures, declare each pattern name in
            // the current scope.
            bool is_local = (r->current != r->root);
            if (is_local && expr->destructure.pattern &&
                expr->destructure.pattern->kind == expr_Product) {
                struct ProductExpr* prod = &expr->destructure.pattern->product;
                if (prod->Fields) {
                    for (size_t i = 0; i < prod->Fields->count; i++) {
                        struct ProductField* f = (struct ProductField*)vec_get(prod->Fields, i);
                        if (!f || !f->value) continue;
                        if (f->value->kind != expr_Ident) continue;
                        struct Decl* dd = decl_new(r, r->current, DECL_USER,
                                                   &f->value->ident, expr);
                        if (dd) {
                            dd->is_comptime = expr->destructure.is_const;
                            dd->is_export = false;
                            dd->semantic_kind = SEM_VALUE;
                        }
                    }
                }
            }
            return;
        }

        case expr_Ctl: {
            // Push a fresh function-like scope; walk params
            // left-to-right so dependent types resolve correctly; then
            // walk ret_type and body in that scope.
            struct Scope* ctl_scope = scope_new(r, SCOPE_FUNCTION, r->current);
            struct Scope* saved = r->current;
            int saved_loop_body_depth = r->loop_body_depth;
            r->current = ctl_scope;
            r->loop_body_depth = 0;

            if (expr->ctl.params) {
                for (size_t i = 0; i < expr->ctl.params->count; i++) {
                    struct Param* p = (struct Param*)vec_get(expr->ctl.params, i);
                    if (!p) continue;
                    resolve_expr(r, p->type_ann);
                    struct Decl* pd = decl_new(r, ctl_scope, DECL_PARAM, &p->name, expr);
                    if (pd) {
                        pd->semantic_kind = SEM_VALUE;
                        pd->is_comptime = (p->kind != PARAM_RUNTIME);
                    }
                }
            }

            resolve_expr(r, expr->ctl.ret_type);
            resolve_expr(r, expr->ctl.body);

            r->loop_body_depth = saved_loop_body_depth;
            r->current = saved;
            return;
        }

        case expr_With: {
            // Resolve the func first so its identifiers get back-linked
            // to their decls. With `with handler { ... }` the parser puts
            // the handler-impl block in with.func, so any decls introduced
            // here are op implementations.
            r->handler_body_depth++;
            resolve_expr(r, expr->with.func);
            r->handler_body_depth--;

            // Try to discover an effect scope to import for the body.
            // Order:
            //   1. If func is an Ident whose decl has a SCOPE_EFFECT
            //      child_scope, use that directly. (e.g. `with Exn`)
            //   2. If the decl is a function (Lambda), walk its effect
            //      annotation looking for any reference whose resolved
            //      decl has a SCOPE_EFFECT child_scope.
            //   3. Convention fallback: try the capitalized form of the
            //      identifier name and use its effect child_scope.
            struct Scope* effect_scope = NULL;
            struct Decl* d = resolved_decl_for_expr(expr->with.func);

            // Case 1.
            if (d && d->child_scope &&
                d->child_scope->kind == SCOPE_EFFECT) {
                effect_scope = d->child_scope;
            }

            // Case 2: handler function — find which effect is *handled* by
            // walking the action parameter's annotation. Convention:
            //   handler :: fn(action: fn(...) <H> R) <propagated> R
            // so the *handled* effect H lives in the action's `<...>`. Action
            // may not be the first param (e.g. when an inferred scope token
            // precedes it: `fn(comptime s: Scope, action: fn() <H(s)> R) R`).
            // Walk every param's annotation looking for a Lambda type.
            if (!effect_scope && d && d->node &&
                d->node->kind == expr_Bind &&
                d->node->bind.value &&
                d->node->bind.value->kind == expr_Lambda) {
                struct Expr* eff = NULL;
                Vec* params = d->node->bind.value->lambda.params;
                if (params) {
                    for (size_t pi = 0; pi < params->count && !eff; pi++) {
                        struct Param* p = (struct Param*)vec_get(params, pi);
                        if (p && p->type_ann && p->type_ann->kind == expr_Lambda) {
                            eff = p->type_ann->lambda.effect;
                        }
                    }
                }
                if (!eff) eff = d->node->bind.value->lambda.effect;
                // Walk the effect annotation looking for any reference
                // whose decl has SCOPE_EFFECT child_scope. The annotation
                // is typically Bin(Pipe, ...) of identifiers; field nodes
                // support namespace-qualified effects from imports.
                struct Expr* stack[16];
                int top = 0;
                if (eff) stack[top++] = eff;
                while (top > 0) {
                    struct Expr* e2 = stack[--top];
                    if (!e2) continue;
                    struct Decl* ed = resolved_decl_for_expr(e2);
                    if (ed && ed->child_scope &&
                        ed->child_scope->kind == SCOPE_EFFECT) {
                        effect_scope = ed->child_scope;
                        break;
                    }
                    if (e2->kind == expr_Bin && top + 2 < 16) {
                        if (e2->bin.Left)  stack[top++] = e2->bin.Left;
                        if (e2->bin.Right) stack[top++] = e2->bin.Right;
                    } else if (e2->kind == expr_Call) {
                        // Allocator(s) parses as a Call with callee=Allocator.
                        if (e2->call.callee && top < 16) stack[top++] = e2->call.callee;
                    } else if (e2->kind == expr_Field) {
                        if (e2->field.object && top < 16) stack[top++] = e2->field.object;
                    } else if (e2->kind == expr_EffectRow) {
                        // <H | e> parses as EffectRow(head=H, row=e). Walk head.
                        if (e2->effect_row.head && top < 16) stack[top++] = e2->effect_row.head;
                    }
                }
            }

            // Case 3: convention fallback — capitalize first letter.
            if (!effect_scope && expr->with.func && expr->with.func->kind == expr_Ident) {
                const char* nm = pool_get(r->pool,
                    expr->with.func->ident.string_id, 0);
                if (nm && nm[0] >= 'a' && nm[0] <= 'z') {
                    char buf[256];
                    size_t len = strlen(nm);
                    if (len < sizeof(buf)) {
                        memcpy(buf, nm, len + 1);
                        buf[0] = (char)(buf[0] - 'a' + 'A');
                        uint32_t cap_id = pool_intern(r->pool, buf, len);
                        struct Decl* cd = scope_lookup(r->current, cap_id);
                        if (cd && cd->child_scope &&
                            cd->child_scope->kind == SCOPE_EFFECT) {
                            effect_scope = cd->child_scope;
                        }
                    }
                }
            }

            // For handlers, the convention is:
            //   fn(action: fn(...) <handled> ret) <propagated> ret
            // The `with` body sees the HANDLED effects. So also collect
            // any effect references inside the first param's type
            // annotation (the action's signature).
            int extra_pushed = 0;
            if (d && d->node && d->node->kind == expr_Bind &&
                d->node->bind.value &&
                d->node->bind.value->kind == expr_Lambda) {
                Vec* params = d->node->bind.value->lambda.params;
                if (params && params->count > 0) {
                    struct Param* p = (struct Param*)vec_get(params, 0);
                    if (p && p->type_ann) {
                        extra_pushed = collect_effect_scopes(p->type_ann, r->with_imports);
                    }
                }
            }

            // Case 4: with.func is a handler block (`handler { alloc :: ... }`)
            // — it parses as an expr_Block whose binds name the ops being
            // implemented. Match the first op's name to an effect in scope
            // whose child_scope declares a field with that name.
            if (!effect_scope && expr->with.func &&
                expr->with.func->kind == expr_Block &&
                expr->with.func->block.stmts->count > 0) {
                struct Expr** first_p = (struct Expr**)vec_get(expr->with.func->block.stmts, 0);
                struct Expr* first = first_p ? *first_p : NULL;
                if (first && first->kind == expr_Bind) {
                    uint32_t op_name = first->bind.name.string_id;
                    for (struct Scope* cur = r->current; cur && !effect_scope; cur = cur->parent) {
                        if (!cur->decls) continue;
                        for (size_t i = 0; i < cur->decls->count && !effect_scope; i++) {
                            struct Decl** dp = (struct Decl**)vec_get(cur->decls, i);
                            struct Decl* dd = dp ? *dp : NULL;
                            if (!dd || dd->semantic_kind != SEM_EFFECT) continue;
                            if (!dd->child_scope) continue;
                            if (hashmap_get(&dd->child_scope->name_index, (uint64_t)op_name)) {
                                effect_scope = dd->child_scope;
                            }
                        }
                    }
                }
            }

            // Cache the resolved handled effect on the AST node so sema can
            // read it without re-running this search. Walk the parent of the
            // effect scope to find the effect's Decl*.
            if (effect_scope && effect_scope->parent && effect_scope->parent->decls) {
                Vec* siblings = effect_scope->parent->decls;
                for (size_t i = 0; i < siblings->count; i++) {
                    struct Decl** dp = (struct Decl**)vec_get(siblings, i);
                    struct Decl* dd = dp ? *dp : NULL;
                    if (dd && dd->child_scope == effect_scope &&
                        dd->semantic_kind == SEM_EFFECT) {
                        expr->with.handled_effect = dd;
                        break;
                    }
                }
            }

            // Push the effect scope as an overlay (if found), walk body, pop.
            if (effect_scope) {
                vec_push(r->with_imports, &effect_scope);
            }
            resolve_expr(r, expr->with.body);
            if (effect_scope) {
                r->with_imports->count--;
            }
            for (int i = 0; i < extra_pushed; i++) r->with_imports->count--;
            return;
        }

        case expr_Field:
            // Resolve the object first. If it names something with a
            // child scope (notably DECL_IMPORT aliases, but also struct,
            // enum, and effect declarations), resolve the field against
            // that scope. Ordinary value fields still defer to type
            // checking because params/locals don't carry type scopes yet.
            resolve_expr(r, expr->field.object);
            {
                struct Decl* object_decl = resolved_decl_for_expr(expr->field.object);
                if (object_decl && object_decl->child_scope) {
                    struct Decl* field_decl = scope_lookup_local(object_decl->child_scope,
                        expr->field.field.string_id);
                    if (field_decl) {
                        expr->field.field.resolved = field_decl;
                    } else if (object_decl->kind == DECL_IMPORT) {
                        const char* obj_nm = pool_get(r->pool, object_decl->name.string_id, 0);
                        const char* field_nm = pool_get(r->pool, expr->field.field.string_id, 0);
                        resolver_error(r, expr->field.field.span,
                            "module '%s' has no member '%s'",
                            obj_nm ? obj_nm : "?", field_nm ? field_nm : "?");
                    }
                }
            }
            return;

        case expr_Index:
            resolve_expr(r, expr->index.object);
            resolve_expr(r, expr->index.index);
            return;

        case expr_Lambda: {
            // Anonymous Lambda — no enclosing Bind owns it, so allocate
            // a fresh function scope and dispatch to the helper.
            struct Scope* fn_scope = scope_new(r, SCOPE_FUNCTION, r->current);
            resolve_lambda_into(r, expr, fn_scope);
            return;
        }

        case expr_Loop: {
            // C-style loop init/cond/step run in a fresh SCOPE_LOOP so
            // the loop variable doesn't leak. Capture also lives there.
            struct Scope* loop_scope = scope_new(r, SCOPE_LOOP, r->current);
            struct Scope* saved = r->current;
            r->current = loop_scope;

            // Walk init first so a `loop (i := 0; ...)` introduces `i`
            // into the loop scope (handled by expr_Bind which will see
            // current != root).
            resolve_expr(r, expr->loop_expr.init);
            resolve_expr(r, expr->loop_expr.condition);
            resolve_expr(r, expr->loop_expr.step);

            // Optional unwrap capture on the body.
            if (expr->loop_expr.capture.string_id != 0) {
                struct Decl* cap_decl = decl_new(r, loop_scope, DECL_USER,
                    &expr->loop_expr.capture, expr);
                if (cap_decl) cap_decl->semantic_kind = SEM_VALUE;
            }

            int saved_loop_body_depth = r->loop_body_depth;
            r->loop_body_depth++;
            resolve_expr(r, expr->loop_expr.body);
            r->loop_body_depth = saved_loop_body_depth;

            r->current = saved;
            return;
        }

        case expr_Struct:
            // Member NAMES already registered by collect_decl pass.
            // Walk types and defaults for resolution.
            if (expr->struct_expr.members) {
                for (size_t i = 0; i < expr->struct_expr.members->count; i++) {
                    struct StructMember* m = (struct StructMember*)vec_get(expr->struct_expr.members, i);
                    if (!m) continue;
                    if (m->kind == member_Field) {
                        resolve_expr(r, m->field.type);
                        resolve_expr(r, m->field.default_value);
                    } else if (m->kind == member_Union && m->union_def.variants) {
                        for (size_t j = 0; j < m->union_def.variants->count; j++) {
                            struct FieldDef* f = (struct FieldDef*)vec_get(m->union_def.variants, j);
                            if (!f) continue;
                            resolve_expr(r, f->type);
                            resolve_expr(r, f->default_value);
                        }
                    }
                }
            }
            return;

        case expr_Enum:
            // Variant names already registered. Walk explicit values.
            if (expr->enum_expr.variants) {
                for (size_t i = 0; i < expr->enum_expr.variants->count; i++) {
                    struct EnumVariant* v = (struct EnumVariant*)vec_get(expr->enum_expr.variants, i);
                    if (v) resolve_expr(r, v->explicit_value);
                }
            }
            return;

        case expr_EnumRef:
            // Variant references defer to type checking.
            return;

        case expr_Effect:
            // Operation NAMES already registered. Walk each operation's
            // value (a Ctl signature) for param-type resolution.
            if (expr->effect_expr.scope_param.string_id != 0) {
                ensure_implicit_decl(r, r->current, DECL_SCOPE_PARAM,
                    SEM_SCOPE_TOKEN, &expr->effect_expr.scope_param, expr);
            } else if (expr->effect_expr.is_scoped) {
                resolver_error(r, expr->span,
                    "scoped effect must declare a Scope token parameter like <s>");
            }
            if (expr->effect_expr.operations) {
                for (size_t i = 0; i < expr->effect_expr.operations->count; i++) {
                    struct Expr** op_p = (struct Expr**)vec_get(expr->effect_expr.operations, i);
                    if (!op_p || !*op_p) continue;
                    struct Expr* op = *op_p;
                    if (op->kind == expr_Bind) {
                        resolve_expr(r, op->bind.value);
                    } else {
                        resolve_expr(r, op);
                    }
                }
            }
            return;

        case expr_Return:
            resolve_expr(r, expr->return_expr.value);
            return;

        case expr_Defer:
            resolve_expr(r, expr->defer_expr.value);
            return;

        case expr_ArrayType:
            resolve_expr(r, expr->array_type.size);
            resolve_expr(r, expr->array_type.elem);
            return;

        case expr_ArrayLit:
            resolve_expr(r, expr->array_lit.size);
            resolve_expr(r, expr->array_lit.elem_type);
            resolve_expr(r, expr->array_lit.initializer);
            return;

        case expr_SliceType:
            resolve_expr(r, expr->slice_type.elem);
            return;

        case expr_ManyPtrType:
            resolve_expr(r, expr->many_ptr_type.elem);
            return;
    }
}

// Public entry point: tracks comptime depth around the inner walker so
// every recursive call automatically picks it up.
void resolve_expr(struct Resolver* r, struct Expr* expr) {
    if (!expr) return;
    bool entered = expr->is_comptime;
    if (entered) r->comptime_depth++;
    resolve_expr_inner(r, expr);
    if (entered) r->comptime_depth--;
}

// Resolve a Lambda using the supplied function scope.
//   - Push fn_scope first.
//   - Walk params left-to-right: resolve each param's type (which can
//     reference EARLIER params plus the enclosing scope through the
//     scope chain), then declare the param's name as DECL_PARAM.
//   - Effect annotation and return type resolve with all params in
//     scope (e.g. @returnType(action)).
//   - Body resolves in fn_scope.
static void resolve_lambda_into(struct Resolver* r, struct Expr* lambda, struct Scope* fn_scope) {
    if (!lambda) return;
    struct LambdaExpr* L = &lambda->lambda;

    // Push function scope BEFORE walking params so dependent types
    // (where a later param's type references an earlier param) work.
    struct Scope* saved = r->current;
    int saved_loop_body_depth = r->loop_body_depth;
    r->current = fn_scope;
    r->loop_body_depth = 0;

    if (L->params) {
        for (size_t i = 0; i < L->params->count; i++) {
            struct Param* p = (struct Param*)vec_get(L->params, i);
            if (!p) continue;
            resolve_expr(r, p->type_ann);
            struct Decl* pd = decl_new(r, fn_scope, DECL_PARAM, &p->name, lambda);
            if (pd) {
                pd->is_comptime = (p->kind != PARAM_RUNTIME);
                if (p->kind == PARAM_INFERRED_COMPTIME) {
                    // `comptime s: Scope` — register as a scope token so
                    // `<Allocator(s)>` annotations in the same signature
                    // resolve to this decl rather than introducing a
                    // conflicting fresh token.
                    pd->semantic_kind = SEM_SCOPE_TOKEN;
                    pd->scope_token_id = r->next_scope_token_id++;
                } else {
                    pd->semantic_kind = SEM_VALUE;
                }
            }
        }
    }

    // Effect annotation + return type resolve with params in scope.
    // Effect rows introduce comptime-only tokens (`e`, `s`) into this
    // function scope. Later borrow/escape analysis can treat those
    // tokens as region colors on references while ordinary resolution
    // prevents them from escaping as runtime values.
    resolve_effect_annotation(r, fn_scope, L->effect, lambda);
    resolve_expr(r, L->ret_type);

    // Push declared effects as with-imports so operations like
    // `panic()` resolve inside this body. e.g. fn f() <Exn> ... can
    // reference panic() directly because Exn's operations are in scope.
    int pushed = collect_effect_scopes(L->effect, r->with_imports);

    // If the body is a Block, walk its stmts directly so the function
    // scope contains both params and locals (no extra SCOPE_BLOCK
    // wrapping the function body).
    if (L->body && L->body->kind == expr_Block) {
        Vec* stmts = L->body->block.stmts;
        for (size_t i = 0; i < stmts->count; i++) {
            struct Expr** st = (struct Expr**)vec_get(stmts, i);
            if (st) resolve_expr(r, *st);
        }
    } else {
        resolve_expr(r, L->body);
    }

    // Pop the same number of overlays we pushed.
    for (int i = 0; i < pushed; i++) r->with_imports->count--;

    r->loop_body_depth = saved_loop_body_depth;
    r->current = saved;
}

// ============================================================
// Pass 3: validate unresolved references
// ============================================================

static void validate_expr_identifiers(struct Resolver* r, struct Expr* expr);

static void validate_identifier_reference(struct Resolver* r, struct Identifier id) {
    if (id.string_id == 0 || id.resolved) return;
    const char* nm = pool_get(r->pool, id.string_id, 0);
    resolver_error(r, id.span,
        "'%s' is not defined in any accessible scope", nm ? nm : "?");
}

static void validate_effect_row_reference(struct Resolver* r, struct Identifier id) {
    if (id.string_id == 0 || id.resolved) return;
    const char* nm = pool_get(r->pool, id.string_id, 0);
    resolver_error(r, id.span,
        "'%s' must be an effect-row variable in scope", nm ? nm : "?");
}

static void validate_field_def_identifiers(struct Resolver* r, struct FieldDef* f) {
    if (!f) return;
    validate_expr_identifiers(r, f->type);
    validate_expr_identifiers(r, f->default_value);
}

static void validate_struct_member_identifiers(struct Resolver* r, struct StructMember* m) {
    if (!m) return;
    if (m->kind == member_Field) {
        validate_field_def_identifiers(r, &m->field);
    } else if (m->kind == member_Union && m->union_def.variants) {
        for (size_t i = 0; i < m->union_def.variants->count; i++) {
            validate_field_def_identifiers(r,
                (struct FieldDef*)vec_get(m->union_def.variants, i));
        }
    }
}

static void validate_expr_identifiers(struct Resolver* r, struct Expr* expr) {
    if (!expr) return;
    switch (expr->kind) {
        case expr_Lit:
        case expr_Asm:
        case expr_Break:
        case expr_Continue:
        case expr_Wildcard:
            return;
        case expr_Ident:
            validate_identifier_reference(r, expr->ident);
            return;
        case expr_Bin:
            validate_expr_identifiers(r, expr->bin.Left);
            validate_expr_identifiers(r, expr->bin.Right);
            return;
        case expr_Assign:
            validate_expr_identifiers(r, expr->assign.target);
            validate_expr_identifiers(r, expr->assign.value);
            return;
        case expr_Unary:
            validate_expr_identifiers(r, expr->unary.operand);
            return;
        case expr_Call:
            validate_expr_identifiers(r, expr->call.callee);
            if (expr->call.args) {
                for (size_t i = 0; i < expr->call.args->count; i++) {
                    struct Expr** a = (struct Expr**)vec_get(expr->call.args, i);
                    if (a) validate_expr_identifiers(r, *a);
                }
            }
            return;
        case expr_Builtin:
            if (expr->builtin.args) {
                for (size_t i = 0; i < expr->builtin.args->count; i++) {
                    struct Expr** a = (struct Expr**)vec_get(expr->builtin.args, i);
                    if (a) validate_expr_identifiers(r, *a);
                }
            }
            return;
        case expr_EffectRow:
            validate_expr_identifiers(r, expr->effect_row.head);
            validate_effect_row_reference(r, expr->effect_row.row);
            return;
        case expr_If:
            validate_expr_identifiers(r, expr->if_expr.condition);
            validate_expr_identifiers(r, expr->if_expr.then_branch);
            validate_expr_identifiers(r, expr->if_expr.else_branch);
            return;
        case expr_Switch:
            validate_expr_identifiers(r, expr->switch_expr.scrutinee);
            if (expr->switch_expr.arms) {
                for (size_t i = 0; i < expr->switch_expr.arms->count; i++) {
                    struct SwitchArm* arm = (struct SwitchArm*)vec_get(expr->switch_expr.arms, i);
                    if (!arm) continue;
                    if (arm->patterns) {
                        for (size_t j = 0; j < arm->patterns->count; j++) {
                            struct Expr** p = (struct Expr**)vec_get(arm->patterns, j);
                            if (p) validate_expr_identifiers(r, *p);
                        }
                    }
                    validate_expr_identifiers(r, arm->body);
                }
            }
            return;
        case expr_Block: {
            Vec* stmts = expr->block.stmts;
            for (size_t i = 0; i < stmts->count; i++) {
                struct Expr** st = (struct Expr**)vec_get(stmts, i);
                if (st) validate_expr_identifiers(r, *st);
            }
            return;
        }
        case expr_Product:
            validate_expr_identifiers(r, expr->product.type_expr);
            if (expr->product.Fields) {
                for (size_t i = 0; i < expr->product.Fields->count; i++) {
                    struct ProductField* f = (struct ProductField*)vec_get(expr->product.Fields, i);
                    if (f) validate_expr_identifiers(r, f->value);
                }
            }
            return;
        case expr_Bind:
            validate_expr_identifiers(r, expr->bind.type_ann);
            validate_expr_identifiers(r, expr->bind.value);
            return;
        case expr_DestructureBind:
            validate_expr_identifiers(r, expr->destructure.value);
            return;
        case expr_Ctl:
            if (expr->ctl.params) {
                for (size_t i = 0; i < expr->ctl.params->count; i++) {
                    struct Param* p = (struct Param*)vec_get(expr->ctl.params, i);
                    if (p) validate_expr_identifiers(r, p->type_ann);
                }
            }
            validate_expr_identifiers(r, expr->ctl.ret_type);
            validate_expr_identifiers(r, expr->ctl.body);
            return;
        case expr_With:
            validate_expr_identifiers(r, expr->with.func);
            validate_expr_identifiers(r, expr->with.body);
            return;
        case expr_Field:
            validate_expr_identifiers(r, expr->field.object);
            return;
        case expr_Index:
            validate_expr_identifiers(r, expr->index.object);
            validate_expr_identifiers(r, expr->index.index);
            return;
        case expr_Lambda:
            if (expr->lambda.params) {
                for (size_t i = 0; i < expr->lambda.params->count; i++) {
                    struct Param* p = (struct Param*)vec_get(expr->lambda.params, i);
                    if (p) validate_expr_identifiers(r, p->type_ann);
                }
            }
            validate_expr_identifiers(r, expr->lambda.effect);
            validate_expr_identifiers(r, expr->lambda.ret_type);
            validate_expr_identifiers(r, expr->lambda.body);
            return;
        case expr_Loop:
            validate_expr_identifiers(r, expr->loop_expr.init);
            validate_expr_identifiers(r, expr->loop_expr.condition);
            validate_expr_identifiers(r, expr->loop_expr.step);
            validate_expr_identifiers(r, expr->loop_expr.body);
            return;
        case expr_Struct:
            if (expr->struct_expr.members) {
                for (size_t i = 0; i < expr->struct_expr.members->count; i++) {
                    validate_struct_member_identifiers(r,
                        (struct StructMember*)vec_get(expr->struct_expr.members, i));
                }
            }
            return;
        case expr_Enum:
            if (expr->enum_expr.variants) {
                for (size_t i = 0; i < expr->enum_expr.variants->count; i++) {
                    struct EnumVariant* v = (struct EnumVariant*)vec_get(expr->enum_expr.variants, i);
                    if (v) validate_expr_identifiers(r, v->explicit_value);
                }
            }
            return;
        case expr_EnumRef:
            return;
        case expr_Effect:
            if (expr->effect_expr.operations) {
                for (size_t i = 0; i < expr->effect_expr.operations->count; i++) {
                    struct Expr** op = (struct Expr**)vec_get(expr->effect_expr.operations, i);
                    if (op) validate_expr_identifiers(r, *op);
                }
            }
            return;
        case expr_Return:
            validate_expr_identifiers(r, expr->return_expr.value);
            return;
        case expr_Defer:
            validate_expr_identifiers(r, expr->defer_expr.value);
            return;
        case expr_ArrayType:
            validate_expr_identifiers(r, expr->array_type.size);
            validate_expr_identifiers(r, expr->array_type.elem);
            return;
        case expr_ArrayLit:
            validate_expr_identifiers(r, expr->array_lit.size);
            validate_expr_identifiers(r, expr->array_lit.elem_type);
            validate_expr_identifiers(r, expr->array_lit.initializer);
            return;
        case expr_SliceType:
            validate_expr_identifiers(r, expr->slice_type.elem);
            return;
        case expr_ManyPtrType:
            validate_expr_identifiers(r, expr->many_ptr_type.elem);
            return;
    }
}

static void validate_resolved_identifiers(struct Resolver* r) {
    if (!r || !r->ast) return;
    for (size_t i = 0; i < r->ast->count; i++) {
        struct Expr** expr = (struct Expr**)vec_get(r->ast, i);
        if (expr) validate_expr_identifiers(r, *expr);
    }
}

// ============================================================
// dump_resolution — validation harness
// ============================================================

static const char* scope_kind_str(ScopeKind k) {
    switch (k) {
        case SCOPE_MODULE:   return "module";
        case SCOPE_FUNCTION: return "fn";
        case SCOPE_BLOCK:    return "block";
        case SCOPE_STRUCT:   return "struct";
        case SCOPE_ENUM:     return "enum";
        case SCOPE_EFFECT:   return "effect";
        case SCOPE_HANDLER:  return "handler";
        case SCOPE_LOOP:     return "loop";
        case SCOPE_COMPTIME: return "comptime";
    }
    return "?";
}

static const char* decl_kind_str(DeclKind k) {
    switch (k) {
        case DECL_PRIMITIVE:  return "PRIMITIVE";
        case DECL_USER:       return "USER";
        case DECL_IMPORT:     return "IMPORT";
        case DECL_PARAM:      return "PARAM";
        case DECL_FIELD:      return "FIELD";
        case DECL_SCOPE_PARAM:return "SCOPE_PARAM";
        case DECL_EFFECT_ROW: return "EFFECT_ROW";
        case DECL_LOOP_LABEL: return "LOOP_LABEL";
    }
    return "?";
}

static const char* semantic_kind_str(SemanticKind k) {
    switch (k) {
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

static void dump_scope(struct Resolver* r, struct Scope* s, int depth) {
    for (int i = 0; i < depth; i++) printf("  ");
    printf("[scope %s, %zu decls]\n", scope_kind_str(s->kind), s->decls->count);
    for (size_t i = 0; i < s->decls->count; i++) {
        struct Decl** d = (struct Decl**)vec_get(s->decls, i);
        if (!d || !*d) continue;
        for (int j = 0; j < depth + 1; j++) printf("  ");
        const char* nm = pool_get(r->pool, (*d)->name.string_id, 0);
         bool handler_impl = r->compiler && hashmap_contains(
             &r->compiler->handler_impl_decls, (uint64_t)(uintptr_t)*d);
         bool is_pub = false;
         if ((*d)->node) {
             if ((*d)->node->kind == expr_Bind) is_pub = (*d)->node->bind.is_pub;
             else if ((*d)->node->kind == expr_DestructureBind) is_pub = (*d)->node->destructure.is_pub;
         }
         printf("decl %s %s <%s>%s%s%s%s%s%s%s",
               decl_kind_str((*d)->kind),
             nm ? nm : "?",
             semantic_kind_str((*d)->semantic_kind),
             (*d)->is_comptime ? " [comptime]" : "",
             is_pub             ? " [pub]"      : "",
             (*d)->is_export    ? " [export]"   : "",
             (*d)->has_effects  ? " [effects]"  : "",
             handler_impl       ? " [handler-impl]" : "",
             (*d)->child_scope  ? " [+scope]"   : "",
             (*d)->scope_token_id ? " [scope-token]" : "");
         if ((*d)->scope_token_id) printf("#%u", (*d)->scope_token_id);
         printf("\n");
    }
    for (size_t i = 0; i < s->children->count; i++) {
        struct Scope** c = (struct Scope**)vec_get(s->children, i);
        if (c && *c) dump_scope(r, *c, depth + 1);
    }
}

// ----- Reference tally (Step 2 verification harness) -----

struct ResolvedSample {
    struct Identifier ref;     // the use site
    struct Decl* decl;         // what it resolved to
};

struct RefStats {
    size_t total;
    size_t resolved;
    Vec* unresolved;   // Vec of struct Identifier — first N for display
    size_t unresolved_cap_for_display;
    Vec* resolved_sample;  // Vec of ResolvedSample — first N successful
    size_t resolved_cap_for_display;
};

static void tally_expr(struct Resolver* r, struct Expr* expr, struct RefStats* s);

static void tally_field_def(struct Resolver* r, struct FieldDef* f, struct RefStats* s) {
    if (!f) return;
    tally_expr(r, f->type, s);
    tally_expr(r, f->default_value, s);
}

static void tally_struct_member(struct Resolver* r, struct StructMember* m, struct RefStats* s) {
    if (!m) return;
    if (m->kind == member_Field) {
        tally_field_def(r, &m->field, s);
    } else if (m->kind == member_Union) {
        if (m->union_def.variants) {
            for (size_t i = 0; i < m->union_def.variants->count; i++) {
                tally_field_def(r, (struct FieldDef*)vec_get(m->union_def.variants, i), s);
            }
        }
    }
}

static void tally_expr(struct Resolver* r, struct Expr* expr, struct RefStats* s) {
    if (!expr) return;
    switch (expr->kind) {
        case expr_Lit: case expr_Asm: case expr_Break: case expr_Continue:
        case expr_Wildcard:
            return;
        case expr_Ident:
            s->total++;
            if (expr->ident.resolved) {
                s->resolved++;
                if (s->resolved_sample->count < s->resolved_cap_for_display) {
                    struct ResolvedSample sample = {
                        .ref = expr->ident,
                        .decl = expr->ident.resolved,
                    };
                    vec_push(s->resolved_sample, &sample);
                }
            } else if (s->unresolved->count < s->unresolved_cap_for_display) {
                vec_push(s->unresolved, &expr->ident);
            }
            return;
        case expr_Bin:
            tally_expr(r, expr->bin.Left, s); tally_expr(r, expr->bin.Right, s); return;
        case expr_Assign:
            tally_expr(r, expr->assign.target, s); tally_expr(r, expr->assign.value, s); return;
        case expr_Unary:
            tally_expr(r, expr->unary.operand, s); return;
        case expr_Call:
            tally_expr(r, expr->call.callee, s);
            if (expr->call.args)
                for (size_t i = 0; i < expr->call.args->count; i++) {
                    struct Expr** a = (struct Expr**)vec_get(expr->call.args, i);
                    if (a) tally_expr(r, *a, s);
                }
            return;
        case expr_Builtin:
            if (expr->builtin.args)
                for (size_t i = 0; i < expr->builtin.args->count; i++) {
                    struct Expr** a = (struct Expr**)vec_get(expr->builtin.args, i);
                    if (a) tally_expr(r, *a, s);
                }
            return;
        case expr_EffectRow:
            tally_expr(r, expr->effect_row.head, s);
            if (expr->effect_row.row.string_id != 0) {
                s->total++;
                if (expr->effect_row.row.resolved) {
                    s->resolved++;
                    if (s->resolved_sample->count < s->resolved_cap_for_display) {
                        struct ResolvedSample sample = {
                            .ref = expr->effect_row.row,
                            .decl = expr->effect_row.row.resolved,
                        };
                        vec_push(s->resolved_sample, &sample);
                    }
                } else if (s->unresolved->count < s->unresolved_cap_for_display) {
                    vec_push(s->unresolved, &expr->effect_row.row);
                }
            }
            return;
        case expr_If:
            tally_expr(r, expr->if_expr.condition, s);
            tally_expr(r, expr->if_expr.then_branch, s);
            tally_expr(r, expr->if_expr.else_branch, s);
            return;
        case expr_Switch:
            tally_expr(r, expr->switch_expr.scrutinee, s);
            if (expr->switch_expr.arms)
                for (size_t i = 0; i < expr->switch_expr.arms->count; i++) {
                    struct SwitchArm* arm = (struct SwitchArm*)vec_get(expr->switch_expr.arms, i);
                    if (!arm) continue;
                    if (arm->patterns)
                        for (size_t j = 0; j < arm->patterns->count; j++) {
                            struct Expr** p = (struct Expr**)vec_get(arm->patterns, j);
                            if (p) tally_expr(r, *p, s);
                        }
                    tally_expr(r, arm->body, s);
                }
            return;
        case expr_Block: {
            Vec* stmts = expr->block.stmts;
            for (size_t i = 0; i < stmts->count; i++) {
                struct Expr** st = (struct Expr**)vec_get(stmts, i);
                if (st) tally_expr(r, *st, s);
            }
            return;
        }
        case expr_Product:
            tally_expr(r, expr->product.type_expr, s);
            if (expr->product.Fields)
                for (size_t i = 0; i < expr->product.Fields->count; i++) {
                    struct ProductField* f = (struct ProductField*)vec_get(expr->product.Fields, i);
                    if (f) tally_expr(r, f->value, s);
                }
            return;
        case expr_Bind:
            tally_expr(r, expr->bind.type_ann, s);
            tally_expr(r, expr->bind.value, s);
            return;
        case expr_DestructureBind:
            tally_expr(r, expr->destructure.value, s);
            return;
        case expr_Ctl:
            if (expr->ctl.params)
                for (size_t i = 0; i < expr->ctl.params->count; i++) {
                    struct Param* p = (struct Param*)vec_get(expr->ctl.params, i);
                    if (p) tally_expr(r, p->type_ann, s);
                }
            tally_expr(r, expr->ctl.ret_type, s);
            tally_expr(r, expr->ctl.body, s);
            return;
        case expr_With:
            tally_expr(r, expr->with.func, s);
            tally_expr(r, expr->with.body, s);
            return;
        case expr_Field:
            tally_expr(r, expr->field.object, s);
            return;
        case expr_Index:
            tally_expr(r, expr->index.object, s);
            tally_expr(r, expr->index.index, s);
            return;
        case expr_Lambda:
            if (expr->lambda.params)
                for (size_t i = 0; i < expr->lambda.params->count; i++) {
                    struct Param* p = (struct Param*)vec_get(expr->lambda.params, i);
                    if (p) tally_expr(r, p->type_ann, s);
                }
            tally_expr(r, expr->lambda.effect, s);
            tally_expr(r, expr->lambda.ret_type, s);
            tally_expr(r, expr->lambda.body, s);
            return;
        case expr_Loop:
            tally_expr(r, expr->loop_expr.init, s);
            tally_expr(r, expr->loop_expr.condition, s);
            tally_expr(r, expr->loop_expr.step, s);
            tally_expr(r, expr->loop_expr.body, s);
            return;
        case expr_Struct:
            if (expr->struct_expr.members)
                for (size_t i = 0; i < expr->struct_expr.members->count; i++) {
                    struct StructMember* m = (struct StructMember*)vec_get(expr->struct_expr.members, i);
                    if (m) tally_struct_member(r, m, s);
                }
            return;
        case expr_Enum:
            if (expr->enum_expr.variants)
                for (size_t i = 0; i < expr->enum_expr.variants->count; i++) {
                    struct EnumVariant* v = (struct EnumVariant*)vec_get(expr->enum_expr.variants, i);
                    if (v) tally_expr(r, v->explicit_value, s);
                }
            return;
        case expr_EnumRef:
            return;
        case expr_Effect:
            if (expr->effect_expr.operations)
                for (size_t i = 0; i < expr->effect_expr.operations->count; i++) {
                    struct Expr** op = (struct Expr**)vec_get(expr->effect_expr.operations, i);
                    if (op) tally_expr(r, *op, s);
                }
            return;
        case expr_Return: tally_expr(r, expr->return_expr.value, s); return;
        case expr_Defer:  tally_expr(r, expr->defer_expr.value, s); return;
        case expr_ArrayType:
            tally_expr(r, expr->array_type.size, s);
            tally_expr(r, expr->array_type.elem, s);
            return;
        case expr_ArrayLit:
            tally_expr(r, expr->array_lit.size, s);
            tally_expr(r, expr->array_lit.elem_type, s);
            tally_expr(r, expr->array_lit.initializer, s);
            return;
        case expr_SliceType:    tally_expr(r, expr->slice_type.elem, s); return;
        case expr_ManyPtrType:  tally_expr(r, expr->many_ptr_type.elem, s); return;
    }
}

void dump_resolution(struct Resolver* r) {
    printf("\n=== resolution dump ===\n");
    if (r->root) dump_scope(r, r->root, 0);
    printf("(generic instantiations are created during sema and not shown "
           "here — see --dump-evidence for per-instantiation bodies.)\n");

    // Tally identifier resolution coverage.
    Arena* stats_arena = r->arena;
    if (r->compiler) {
        compiler_reset_scratch(r->compiler);
        stats_arena = &r->compiler->scratch_arena;
    }

    struct RefStats stats = {0};
    stats.unresolved = vec_new_in(stats_arena, sizeof(struct Identifier));
    stats.unresolved_cap_for_display = 30;
    stats.resolved_sample = vec_new_in(stats_arena, sizeof(struct ResolvedSample));
    stats.resolved_cap_for_display = 20;
    for (size_t i = 0; i < r->ast->count; i++) {
        struct Expr** e = (struct Expr**)vec_get(r->ast, i);
        if (e && *e) tally_expr(r, *e, &stats);
    }

    printf("\n=== identifier references ===\n");
    printf("  total:     %zu\n", stats.total);
    printf("  resolved:  %zu\n", stats.resolved);
    printf("  unresolved: %zu\n", stats.total - stats.resolved);

    if (stats.resolved_sample->count > 0) {
        printf("  first %zu resolved (use -> decl):\n", stats.resolved_sample->count);
        for (size_t i = 0; i < stats.resolved_sample->count; i++) {
            struct ResolvedSample* rs = (struct ResolvedSample*)vec_get(stats.resolved_sample, i);
            const char* use_nm = pool_get(r->pool, rs->ref.string_id, 0);
            const char* decl_nm = pool_get(r->pool, rs->decl->name.string_id, 0);
            printf("    %s @ %d:%d -> %s %s (decl @ %d:%d)\n",
                   use_nm ? use_nm : "?",
                   rs->ref.span.line, rs->ref.span.column,
                   decl_kind_str(rs->decl->kind),
                   decl_nm ? decl_nm : "?",
                   rs->decl->name.span.line, rs->decl->name.span.column);
        }
    }
    if (stats.unresolved->count > 0) {
        printf("  first %zu unresolved:\n", stats.unresolved->count);
        for (size_t i = 0; i < stats.unresolved->count; i++) {
            struct Identifier* id = (struct Identifier*)vec_get(stats.unresolved, i);
            const char* nm = pool_get(r->pool, id->string_id, 0);
            printf("    line %d col %d: '%s'\n",
                   id->span.line, id->span.column, nm ? nm : "?");
        }
    }

    if (r->compiler) compiler_reset_scratch(r->compiler);
}

// ============================================================
// Resolver init + driver
// ============================================================

struct Resolver resolver_new(struct Compiler* compiler, Vec* ast) {
    struct Resolver r = {0};
    if (!compiler) return r;

    r.compiler = compiler;
    r.arena = &compiler->arena;
    r.pool = &compiler->pool;
    r.ast = ast;
    r.current = NULL;
    r.root = NULL;
    r.current_module = NULL;
    r.root_path_id = compiler->root_path
        ? pool_intern(&compiler->pool, compiler->root_path, strlen(compiler->root_path))
        : 0;
    r.source_map = &compiler->source_map;
    r.diags = &compiler->diags;
    r.has_errors = false;
    r.effect_annotation_depth = 0;
    r.loop_body_depth = 0;
    r.next_scope_token_id = 1;
    r.with_imports = vec_new_in(&compiler->pass_arena, sizeof(struct Scope*));
    return r;
}

static bool resolve_module(struct Resolver* r, struct Module* mod) {
    if (!mod) return false;
    if (mod->resolved) return true;
    if (mod->resolving) {
        report_import_cycle(r, mod, (struct Span){0});
        return false;
    }

    size_t error_count_before = r->diags ? r->diags->error_count : 0;

    struct Scope* saved_root = r->root;
    struct Scope* saved_current = r->current;
    struct Module* saved_module = r->current_module;
    Vec* saved_ast = r->ast;

    mod->resolving = true;
    vec_push(r->compiler->module_stack, &mod);

    r->root = mod->scope;
    r->current = mod->scope;
    r->current_module = mod;
    r->ast = mod->ast;

    // Pre-populate primitive types per module. They are excluded from
    // exports, so a file's anonymous struct only exposes user decls.
    register_primitives(r);

    // Pass 1: collect top-level decls.
    for (size_t i = 0; i < mod->ast->count; i++) {
        struct Expr** decl = (struct Expr**)vec_get(mod->ast, i);
        if (decl && *decl) collect_decl(r, *decl);
    }

    // Pass 2: resolve references and recursively load imports.
    for (size_t i = 0; i < mod->ast->count; i++) {
        struct Expr** decl = (struct Expr**)vec_get(mod->ast, i);
        if (decl && *decl) resolve_expr(r, *decl);
    }

    // Pass 3: residual unresolved references become diagnostics.
    validate_resolved_identifiers(r);

    size_t error_count_after = r->diags ? r->diags->error_count : error_count_before;

    // Harvest exports.
    for (size_t i = 0; i < mod->scope->decls->count; i++) {
        struct Decl** d = (struct Decl**)vec_get(mod->scope->decls, i);
        if (d && *d && (*d)->is_export && (*d)->kind != DECL_PRIMITIVE) {
            vec_push(mod->exports, d);
        }
    }
    mod->resolved = true;
    mod->resolving = false;
    if (r->compiler->module_stack && r->compiler->module_stack->count > 0) {
        r->compiler->module_stack->count--;
    }

    r->root = saved_root;
    r->current = saved_current;
    r->current_module = saved_module;
    r->ast = saved_ast;

    return error_count_after == error_count_before;
}

bool resolve(struct Resolver* r) {
    struct Module* mod = module_new(r, r->root_path_id, r->ast);
    bool ok = resolve_module(r, mod);

    r->root = mod->scope;
    r->current = mod->scope;
    r->current_module = mod;
    r->ast = mod->ast;

    return ok;
}
