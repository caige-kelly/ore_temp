#include "./name_resolution.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================
// Error reporting
// ============================================================

static void resolver_error(struct Resolver* r, struct Span span, const char* fmt, ...) {
    struct ResolveError err = {0};
    err.span = span;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err.msg, sizeof(err.msg), fmt, ap);
    va_end(ap);
    vec_push(r->errors, &err);
    r->has_errors = true;
}

// ============================================================
// Scopes
// ============================================================

struct Scope* scope_new(struct Resolver* r, ScopeKind kind, struct Scope* parent) {
    struct Scope* s = arena_alloc(r->arena, sizeof(struct Scope));
    s->kind = kind;
    s->parent = parent;
    s->decls = vec_new_in(r->arena, sizeof(struct Decl*));
    s->children = vec_new_in(r->arena, sizeof(struct Scope*));
    if (parent) vec_push(parent->children, &s);
    return s;
}

// Local-only lookup — used for same-scope duplicate checking.
// Does NOT walk parent chain.
static struct Decl* scope_lookup_local(struct Scope* s, uint32_t string_id) {
    for (size_t i = 0; i < s->decls->count; i++) {
        struct Decl** d = (struct Decl**)vec_get(s->decls, i);
        if (d && *d && (*d)->name.string_id == string_id) {
            return *d;
        }
    }
    return NULL;
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
                             DeclKind kind, struct Identifier name,
                             struct Expr* node) {
    // Same-scope duplicate check.
    struct Decl* existing = scope_lookup_local(owner, name.string_id);
    if (existing) {
        const char* nm = pool_get(r->pool, name.string_id, 0);
        resolver_error(r, name.span,
            "'%s' is already defined in this scope", nm ? nm : "?");
        return NULL;
    }

    struct Decl* d = arena_alloc(r->arena, sizeof(struct Decl));
    d->kind = kind;
    d->name = name;
    d->name.resolved = d;       // self-reference for the canonical decl identifier
    d->node = node;
    d->owner = owner;
    d->child_scope = NULL;
    d->module = NULL;
    d->is_comptime = false;
    d->is_export = false;
    vec_push(owner->decls, &d);
    return d;
}

// ============================================================
// Primitives
// ============================================================

static void register_primitive(struct Resolver* r, const char* name) {
    struct Identifier id = {0};
    id.string_id = pool_intern(r->pool, name, strlen(name));
    id.span = (struct Span){0};
    struct Decl* d = decl_new(r, r->root, DECL_PRIMITIVE, id, NULL);
    if (d) d->is_comptime = true;
}

void register_primitives(struct Resolver* r) {
    static const char* prims[] = {
        "i8", "i16", "i32", "i64", "isize",
        "u8", "u16", "u32", "u64", "usize",
        "f32", "f64",
        "bool", "void", "noreturn",
        "type", "anytype",
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
            decl_new(r, scope, DECL_FIELD, op->bind.name, op);
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
            decl_new(r, scope, DECL_FIELD, m->field.name, NULL);
        } else if (m->kind == member_Union && m->union_def.variants) {
            for (size_t j = 0; j < m->union_def.variants->count; j++) {
                struct FieldDef* f = (struct FieldDef*)vec_get(m->union_def.variants, j);
                if (f) decl_new(r, scope, DECL_FIELD, f->name, NULL);
            }
        }
    }
}

// Add variant NAMES from an enum into its child_scope.
static void EnumExpr_seed_decls(struct Resolver* r, struct Scope* scope, struct EnumExpr* en) {
    if (!en || !en->variants) return;
    for (size_t i = 0; i < en->variants->count; i++) {
        struct EnumVariant* v = (struct EnumVariant*)vec_get(en->variants, i);
        if (v) decl_new(r, scope, DECL_FIELD, v->name, NULL);
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

    if (expr->kind == expr_Bind) {
        struct BindExpr* b = &expr->bind;
        DeclKind kind = DECL_USER;
        struct Decl* d = decl_new(r, r->current, kind, b->name, expr);
        if (!d) return;

        d->is_comptime = (b->kind == bind_Const);
        d->is_export = true;       // top-level: exported by default

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

    if (expr->kind == expr_DestructureBind) {
        // .{a, b} := expr at top level: add each pattern element as a Decl.
        struct DestructureBindExpr* db = &expr->destructure;
        if (!db->pattern || db->pattern->kind != expr_Product) return;
        struct ProductExpr* prod = &db->pattern->product;
        for (size_t i = 0; i < prod->Fields->count; i++) {
            struct ProductField* f = (struct ProductField*)vec_get(prod->Fields, i);
            if (!f || !f->value) continue;
            if (f->value->kind != expr_Ident) continue;
            struct Decl* d = decl_new(r, r->current, DECL_USER,
                                      f->value->ident, expr);
            if (d) {
                d->is_comptime = db->is_const;
                d->is_export = true;
            }
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

        if (e->kind == expr_Ident && e->ident.resolved &&
            e->ident.resolved->child_scope &&
            e->ident.resolved->child_scope->kind == SCOPE_EFFECT) {
            struct Scope* s = e->ident.resolved->child_scope;
            vec_push(out_scopes, &s);
            pushed++;
            continue;
        }

        switch (e->kind) {
            case expr_Bin:
                if (top + 2 <= 64) {
                    if (e->bin.Left)  stack[top++] = e->bin.Left;
                    if (e->bin.Right) stack[top++] = e->bin.Right;
                }
                break;
            case expr_Call:
                if (top + 1 <= 64 && e->call.callee) stack[top++] = e->call.callee;
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
        case expr_Break:
        case expr_Continue:
            // Leaves with no children to walk.
            return;

        case expr_Ident: {
            // Lookup walks the parent chain AND any active `with X`
            // effect-scope overlays so operations like `panic` resolve
            // when an enclosing `with Exn` is active.
            struct Decl* d = scope_lookup_with_overlays(r, expr->ident.string_id);
            if (d) {
                expr->ident.resolved = d;
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
            if (expr->builtin.args) {
                for (size_t i = 0; i < expr->builtin.args->count; i++) {
                    struct Expr** a = (struct Expr**)vec_get(expr->builtin.args, i);
                    if (a) resolve_expr(r, *a);
                }
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
                decl_new(r, cap_scope, DECL_USER,
                         expr->if_expr.capture, expr);
                resolve_expr(r, expr->if_expr.then_branch);
                r->current = saved;
            } else {
                resolve_expr(r, expr->if_expr.then_branch);
            }
            // else-branch never sees the capture.
            resolve_expr(r, expr->if_expr.else_branch);
            return;
        }

        case expr_For:
            // bindings are declarations; skip for now.
            resolve_expr(r, expr->for_expr.iter);
            resolve_expr(r, expr->for_expr.where_clause);
            resolve_expr(r, expr->for_expr.body);
            return;

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

            Vec* stmts = &expr->block.stmts;
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
                    Vec* stmts = &body->block.stmts;
                    for (size_t i = 0; i < stmts->count; i++) {
                        struct Expr** st = (struct Expr**)vec_get(stmts, i);
                        if (st) resolve_expr(r, *st);
                    }
                } else {
                    resolve_expr(r, body);
                }
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
                                      expr->bind.name, expr);
                if (local_decl) {
                    local_decl->is_comptime = true;
                    local_decl->is_export = false;
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
                                          expr->bind.name, expr);
                if (d) {
                    d->is_comptime = false;
                    d->is_export = false;
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
                                                   f->value->ident, expr);
                        if (dd) {
                            dd->is_comptime = expr->destructure.is_const;
                            dd->is_export = false;
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
            r->current = ctl_scope;

            if (expr->ctl.params) {
                for (size_t i = 0; i < expr->ctl.params->count; i++) {
                    struct Param* p = (struct Param*)vec_get(expr->ctl.params, i);
                    if (!p) continue;
                    resolve_expr(r, p->type_ann);
                    decl_new(r, ctl_scope, DECL_PARAM, p->name, expr);
                }
            }

            resolve_expr(r, expr->ctl.ret_type);
            resolve_expr(r, expr->ctl.body);

            r->current = saved;
            return;
        }

        case expr_With: {
            // Resolve the func first so its identifiers get back-linked
            // to their decls.
            resolve_expr(r, expr->with.func);

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
            if (expr->with.func && expr->with.func->kind == expr_Ident) {
                struct Decl* d = expr->with.func->ident.resolved;

                // Case 1.
                if (d && d->child_scope &&
                    d->child_scope->kind == SCOPE_EFFECT) {
                    effect_scope = d->child_scope;
                }

                // Case 2: handler function — walk its effect annotation.
                if (!effect_scope && d && d->node &&
                    d->node->kind == expr_Bind &&
                    d->node->bind.value &&
                    d->node->bind.value->kind == expr_Lambda) {
                    struct Expr* eff = d->node->bind.value->lambda.effect;
                    // Walk the effect annotation looking for any
                    // identifier whose decl has SCOPE_EFFECT child_scope.
                    // The annotation is typically Bin(Pipe, ...) of
                    // identifiers; just search recursively.
                    struct Expr* stack[16];
                    int top = 0;
                    if (eff) stack[top++] = eff;
                    while (top > 0) {
                        struct Expr* e2 = stack[--top];
                        if (!e2) continue;
                        if (e2->kind == expr_Ident && e2->ident.resolved &&
                            e2->ident.resolved->child_scope &&
                            e2->ident.resolved->child_scope->kind == SCOPE_EFFECT) {
                            effect_scope = e2->ident.resolved->child_scope;
                            break;
                        }
                        if (e2->kind == expr_Bin && top + 2 < 16) {
                            if (e2->bin.Left)  stack[top++] = e2->bin.Left;
                            if (e2->bin.Right) stack[top++] = e2->bin.Right;
                        } else if (e2->kind == expr_Call) {
                            // Allocator(s) parses as a Call with callee=Allocator.
                            if (e2->call.callee && top < 16) stack[top++] = e2->call.callee;
                        }
                    }
                }

                // Case 3: convention fallback — capitalize first letter.
                if (!effect_scope) {
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
            }

            // For handlers, the convention is:
            //   fn(action: fn(...) <handled> ret) <propagated> ret
            // The `with` body sees the HANDLED effects. So also collect
            // any effect references inside the first param's type
            // annotation (the action's signature).
            int extra_pushed = 0;
            if (expr->with.func && expr->with.func->kind == expr_Ident) {
                struct Decl* d = expr->with.func->ident.resolved;
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
            // Resolve the object only; the field name is not a scope ref.
            resolve_expr(r, expr->field.object);
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
                decl_new(r, loop_scope, DECL_USER,
                         expr->loop_expr.capture, expr);
            }

            resolve_expr(r, expr->loop_expr.body);

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
    r->current = fn_scope;

    if (L->params) {
        for (size_t i = 0; i < L->params->count; i++) {
            struct Param* p = (struct Param*)vec_get(L->params, i);
            if (!p) continue;
            resolve_expr(r, p->type_ann);
            decl_new(r, fn_scope, DECL_PARAM, p->name, lambda);
        }
    }

    // Effect annotation + return type resolve with params in scope.
    resolve_expr(r, L->effect);
    resolve_expr(r, L->ret_type);

    // Push declared effects as with-imports so operations like
    // `panic()` resolve inside this body. e.g. fn f() <Exn> ... can
    // reference panic() directly because Exn's operations are in scope.
    int pushed = collect_effect_scopes(L->effect, r->with_imports);

    // If the body is a Block, walk its stmts directly so the function
    // scope contains both params and locals (no extra SCOPE_BLOCK
    // wrapping the function body).
    if (L->body && L->body->kind == expr_Block) {
        Vec* stmts = &L->body->block.stmts;
        for (size_t i = 0; i < stmts->count; i++) {
            struct Expr** st = (struct Expr**)vec_get(stmts, i);
            if (st) resolve_expr(r, *st);
        }
    } else {
        resolve_expr(r, L->body);
    }

    // Pop the same number of overlays we pushed.
    for (int i = 0; i < pushed; i++) r->with_imports->count--;

    r->current = saved;
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
        case DECL_LOOP_LABEL: return "LOOP_LABEL";
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
        printf("decl %s %s%s%s%s%s\n",
               decl_kind_str((*d)->kind),
               nm ? nm : "?",
               (*d)->is_comptime ? " [comptime]" : "",
               (*d)->is_export   ? " [export]"   : "",
               (*d)->has_effects ? " [effects]"  : "",
               (*d)->child_scope ? " [+scope]"   : "");
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
        case expr_If:
            tally_expr(r, expr->if_expr.condition, s);
            tally_expr(r, expr->if_expr.then_branch, s);
            tally_expr(r, expr->if_expr.else_branch, s);
            return;
        case expr_For:
            tally_expr(r, expr->for_expr.iter, s);
            tally_expr(r, expr->for_expr.where_clause, s);
            tally_expr(r, expr->for_expr.body, s);
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
            Vec* stmts = &expr->block.stmts;
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

    // Tally identifier resolution coverage.
    struct RefStats stats = {0};
    stats.unresolved = vec_new_in(r->arena, sizeof(struct Identifier));
    stats.unresolved_cap_for_display = 30;
    stats.resolved_sample = vec_new_in(r->arena, sizeof(struct ResolvedSample));
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

    if (r->errors->count > 0) {
        printf("\n=== errors ===\n");
        for (size_t i = 0; i < r->errors->count; i++) {
            struct ResolveError* e = (struct ResolveError*)vec_get(r->errors, i);
            if (!e) continue;
            printf("  line %d col %d: %s\n", e->span.line, e->span.column, e->msg);
        }
    }
}

// ============================================================
// Resolver init + driver
// ============================================================

struct Resolver resolver_new(Vec* ast, StringPool* pool, Arena* arena) {
    struct Resolver r = {0};
    r.arena = arena;
    r.pool = pool;
    r.ast = ast;
    r.current = NULL;
    r.root = NULL;
    r.current_module = NULL;
    r.modules = vec_new_in(arena, sizeof(struct Module*));
    r.errors = vec_new_in(arena, sizeof(struct ResolveError));
    r.has_errors = false;
    r.with_imports = vec_new_in(arena, sizeof(struct Scope*));
    return r;
}

bool resolve(struct Resolver* r) {
    // Allocate the module + its root scope.
    struct Module* mod = arena_alloc(r->arena, sizeof(struct Module));
    mod->path_id = 0;
    mod->ast = r->ast;
    mod->resolved = false;
    mod->exports = vec_new_in(r->arena, sizeof(struct Decl*));

    r->root = scope_new(r, SCOPE_MODULE, NULL);
    mod->scope = r->root;
    r->current_module = mod;
    vec_push(r->modules, &mod);

    // Pre-populate primitive types.
    register_primitives(r);

    r->current = r->root;

    // Pass 1: collect top-level decls.
    for (size_t i = 0; i < r->ast->count; i++) {
        struct Expr** decl = (struct Expr**)vec_get(r->ast, i);
        if (decl && *decl) collect_decl(r, *decl);
    }

    // Pass 2: resolve references (stub — implemented in Step 2).
    for (size_t i = 0; i < r->ast->count; i++) {
        struct Expr** decl = (struct Expr**)vec_get(r->ast, i);
        if (decl && *decl) resolve_expr(r, *decl);
    }

    // Harvest exports.
    for (size_t i = 0; i < r->root->decls->count; i++) {
        struct Decl** d = (struct Decl**)vec_get(r->root->decls, i);
        if (d && *d && (*d)->is_export && (*d)->kind != DECL_PRIMITIVE) {
            vec_push(mod->exports, d);
        }
    }
    mod->resolved = true;

    return !r->has_errors;
}
