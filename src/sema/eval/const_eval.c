#include "const_eval.h"
#include "../sema.h"
#include "../sema_internal.h"
#include "../../parser/ast.h"
#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// Helper: parse int literal test into a host int64_t.
// underscores striped
static bool parse_int_literal(const char *text, int64_t *out) {
    if (!text) return false;
    char buf[64];
    size_t j = 0;
    for (size_t i = 0; text[i] && j + 1 < sizeof(buf); i++){
        if (text[i] != '_') buf[j++] = text[i];
    }
    buf[j] = '\0';
    errno = 0;
    char *end;
    long long v = strtoll(buf, &end, 0); // base 0 -> autodetect 0x/0o/0b
    if (errno || *end != '\0') return false;
    *out = (int64_t)v;
    return true;
}

struct ConstValue query_const_eval(struct Sema *s, struct Expr *expr) {
    struct ConstValue none = { .kind = CONST_NONE };
    if (!s || !expr) return none;

    // Cache check: have we computed this already?
    void *cached = hashmap_get(&s->const_eval_cache, (uint64_t)(uintptr_t) expr);
    if (cached) {
        return *(struct ConstValue *) cached;
    }

    // Compute.
    struct ConstValue result = none;
    switch (expr->kind) {
        case expr_Lit :{
            if (expr->lit.kind == lit_Int ) {
                const char *text = pool_get(s->pool, expr->lit.string_id, 0);
                int64_t v;
                if (parse_int_literal(text, &v)) {
                    result.kind = CONST_INT;
                    result.int_val = v;
                }
            }
            break;
        }
        default:
            break;
    }

    // Cache the result. Allocate in the arena so it lives as long as Sema.
    struct ConstValue *stored = arena_alloc(s->arena, sizeof(*stored));
    *stored = result;
    hashmap_put(&s->const_eval_cache, (uint64_t)(uintptr_t)expr, stored);

    return result;
}