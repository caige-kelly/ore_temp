M2(d) hits a separate issue (array sizing doesn't go through const_eval — that's a downstream consumer concern). Let me drop M2(d) and keep the rest — the local-fold mechanism is what we needed and (a)–(c) plus (e) cover it.Here's the design I'd push if we were starting over with "match everything Zig does" as the explicit goal. One core principle, six layers, and a small number of contracts each layer is responsible for.

The core principle: every expression evaluates to a (type, value) pair
Zig's cleanest piece — and the one Ore is currently only halfway to — is that every expression has both a type and (potentially) a comptime-known value, and they're carried together. Specifically:

A type expression i32 evaluates to (type=IP_TYPE_TYPE, value=IP_I32_TYPE).
A value expression 5 evaluates to (type=IP_COMPTIME_INT, value=IP_INT_5).
A runtime expression read_input() evaluates to (type=IP_I32_TYPE, value=IP_NONE) — type known, value not.
A comptime ref MyInt (where MyInt :: u32) evaluates to (type=IP_TYPE_TYPE, value=IP_U32_TYPE).
A t: type param (generic) inside an un-instantiated body evaluates to (type=IP_TYPE_TYPE, value=hole_id).
Inside an instance with t bound to u32, the same ref evaluates to (type=IP_TYPE_TYPE, value=IP_U32_TYPE).
The TYPE tells you the category (is it a type? a number? a function?). The VALUE tells you whether it's known at compile time and what that value is. Together they answer every question the rest of the compiler asks: "is this usable as a type?" → type == IP_TYPE_TYPE && value != IP_NONE. "Is this a comptime constant?" → value != IP_NONE. "What concrete type does this denote?" → value (when type == IP_TYPE_TYPE).

In Ore today this pair exists IMPLICITLY but isn't carried uniformly. type_of_expr returns just the type; const_eval returns just the value (sometimes); resolve_type_expr returns just the value (the resolved type). Three half-views of the same underlying thing.

The clean architecture: six layers
Layer 1 — InternPool (mostly what we have)
One pool with kind tags. Types and comptime values coexist. Primitives at fixed indices. Type-vars (generic holes) interned by id. No kind field on the hole — holes are pure existentials; their role is determined by per-position metadata, not by the hole itself.

What changes: drop TypeVarKind from IPK_TYPE_VAR.

Layer 2 — Scopes (mostly what we have)
Primitives → file → body chain. One resolve_ref(scope, name) → DefId API. No special cases for types-vs-values at the lookup layer.

What changes: nothing material. We have this.

Layer 3 — Bindings carry (type, value) pairs
Every :: binding (top-level OR local) stores both:

type: IpIndex — what's its type (e.g. IP_TYPE_TYPE for type aliases, IP_COMPTIME_INT for LIMIT :: 64, IP_I32_TYPE for x :: 5_i32).
value: IpIndex | IP_NONE — the comptime-folded value, or IP_NONE if not foldable.
Every := binding stores only the type; value is always IP_NONE.

The node_type_builder we have today already records the type. We add a sibling node_value_builder (or fold value into the same builder as a second column). For :: bindings, the body walker fills in the value at the binding site by calling the evaluator (Layer 4) on the RHS.

What changes: a second column on NodeTypeBuilder, and the body walker fills it for :: bindings. Modest.

Layer 4 — One comptime evaluator
A single function:


typedef struct { IpIndex type; IpIndex value; } TypedValue;

TypedValue eval_expr(const SemaCtx *ctx, SyntaxNode *expr);
// value == IP_NONE means "type known, value runtime-only"
This is Ore's const_eval generalized + merged with the typing pass. It handles:

Literals → (type, value) from the literal kind.
Refs → lookup the binding, return its stored (type, value).
Calls → if all args have known values AND the callee is a comptime fn, evaluate; else (ret_type, IP_NONE).
Builtins (@sizeOf, @TypeOf, @typeInfo, …) — each is just a comptime function in the evaluator.
if / switch — if the scrutinee has a known value, pick the arm and evaluate it; else type-merge the arms.
Param refs inside an instance — pick up the substituted value from the instance frame.
This is the semantic engine. Today's type_of_expr becomes a thin wrapper that returns .type; today's const_eval becomes a thin wrapper that returns .value; today's resolve_type_expr becomes a thin wrapper that asserts .type == IP_TYPE_TYPE && .value != IP_NONE and returns .value.

What changes: this is the biggest piece. We unify three separate evaluators into one. Existing const_eval is structurally close — it grows to carry the type alongside the value.

Layer 5 — Type resolver is a thin wrapper

IpIndex resolve_type_expr(const SemaCtx *ctx, SyntaxNode *node) {
    TypedValue tv = eval_expr(ctx, node);
    if (tv.type.v == IP_TYPE_TYPE.v && tv.value.v != IP_NONE.v)
        return tv.value;  // It's a type expression with a known type-value
    if (ip_tag(&s->intern, tv.type) == IP_TAG_TYPE_VAR)
        return tv.type;   // Generic-hole position; the hole IS the type
    db_emit(s, DIAG_ERROR, span_of(ctx, node), "expected type, got value of type %T", tv.type);
    return IP_ERROR_TYPE;
}
The M2(f) bug is impossible: a local c :: 5 has tv.type == IP_COMPTIME_INT, which fails the gate, which emits the error.

The [N]T array sizing is mechanical:


size_t resolve_array_size(const SemaCtx *ctx, SyntaxNode *expr) {
    TypedValue tv = eval_expr(ctx, expr);
    if (is_integer_type(tv.type) && tv.value.v != IP_NONE.v)
        return extract_int_value(tv.value);
    db_emit(/* size must be a comptime integer */);
    return 0;
}
Both fall out of the evaluator without special-case wiring. Same pattern for any future "this needs to be a comptime X" site.

What changes: resolve_type_expr shrinks to a wrapper. The four-or-five paths in it today collapse into ONE call to eval_expr plus a check.

Layer 6 — Generic params + instances
Function signatures store per-param flags (Zig-style comptime_bits: u32), not per-hole kinds:


struct {
    IpIndex        ret;
    const IpIndex *params;
    size_t         n_params;
    uint32_t       comptime_bits;   // bit i = param i is `comptime` (covers `t: type` AND `comptime n: u32`)
    IpIndex        effect_row;
} fn_type;
Generic params still mint TYPE_VAR holes (for type params) OR are typed concretely with value == IP_NONE placeholder (for non-type comptime params like comptime n: u32). The comptime_bit tells the call site to evaluate the argument (extract its value) rather than just type-check it.

At the call site, monomorphization just iterates params: for each comptime-bit param, call eval_expr(arg) and capture the value; build the instance key from those values; demand the instance.

In the instance body walk, the substituted values are looked up by the evaluator's Layer 4 ref handler — the instance has bound t → IP_U32_TYPE for type params, n → IP_INT_64 for value params; same machinery, same evaluator.

What changes: signature gets comptime_bits; holes lose kind; call-site monomorphization branches on the bit (not the hole). This is Zig's exact shape.

What this gives us — Zig parity
With the six layers above, Ore can do everything Zig does in this area:

Zig capability	How Ore gets it from this architecture
const x = i32; var y: x = 5	Layer 3 stores x as (type, value) = (type, i32-entry); Layer 5 unpacks
fn f(comptime T: type, x: T) → T	Layer 6 sets bit 0 in comptime_bits; Layer 4 evaluates the arg as a type-expr at the call site
fn f(x: anytype) → @TypeOf(x)	Layer 6 sets the same bit; arg is typed (not value-evaluated); hole binds to the arg's type
fn f(comptime n: u32, x: [n]i32)	Layer 6 sets the bit on a non-type param; Layer 4 extracts the int value; instance key includes it
[LIMIT]T where LIMIT :: 64	Layer 4 evaluates LIMIT to (comptime_int, 64); array-size resolver extracts
comptime if (some_condition) { … }	Layer 4 evaluates the condition; if value known, picks one branch entirely
@TypeOf(x), @sizeOf(T), @typeName(T)	Each is just a builtin in Layer 4 returning a TypedValue
@typeInfo(T) (introspection)	Add an IPK_TYPE_INFO_VALUE kind; Layer 4 builtin constructs one
Anonymous struct types struct { x: i32 }	Layer 4 evaluates to (type, struct-type-entry); bindable, comparable, etc.
Per-instantiation monomorphization	Layer 6 unchanged from today, just driven by comptime_bits instead of hole kind
Error sets, try, !T	Mostly orthogonal — Ore uses effect rows for this; doesn't change
The architecture also naturally extends:

Generic type constructors (Vec(T) :: struct { items: []T }): Vec is a binding whose value is a comptime function returning a type. Call site evaluates Vec(u32) → returns a struct-type IpIndex. Instance key includes the comptime args. Zig calls these "type functions"; Ore gets them by building on Layer 4.

Comptime reflection / @typeInfo: just a builtin that takes a type-value and returns a comptime struct-value describing it. Plugs into Layer 4 with no new machinery.

where clauses (Odin-style): a where eval_expr(constraint) == true gate at instance check time. Layer 4 already evaluates the constraint; gate is a boolean check.

The cost — honest accounting
This is bigger than Option (b) from the architecture review. The components:

Component	Effort	Risk
Add (type, value) column to node-types	~half day	Low — just additional storage
Unify const_eval + type_of_expr into one eval_expr returning TypedValue	~2-3 days	Medium — touches many sites; need to keep cache discipline
Rewrite resolve_type_expr as a thin wrapper	~half day	Low — net code reduction
Switch generic params from TYPE_VAR.kind to fn_type.comptime_bits	~1 day	Medium — touches monomorphization + freshening
Wire [N]T through Layer 5	~half day	Low — falls out
Audit + update all callers of the three old evaluators	~1 day	Medium — boring but error-prone
Tests across the new surface	~half day	Low
Total: ~5-7 days of focused work. Versus Option (b)'s ~1 day. The difference buys you: the M2(f) and M2(d) gates fall out for free, plus comptime value params, plus a clean foundation for @typeInfo / type constructors / where clauses / anonymous types — every future comptime feature plugs in without new mechanism.

It also lets us delete code. Today's eval_ref (~200 lines), the resolve_type_expr branches (~150 lines), the SEMA_CONST_ANCHOR macro + ConstDiagAnchorCtx frame fields all collapse. Net: maybe -300 lines, +500 lines for the new evaluator, but cleaner.

What "implement once and not touch again" actually means
Looking at it concretely, this is the architecture you can stop touching:

The evaluator is the only semantic engine — every new comptime feature is a new builtin or a new IPK kind, not a new evaluator path.
The (type, value) pair is the universal currency — every caller asks eval_expr and pattern-matches the result.
Generic params are signature metadata — adding new param flavors (variadic generics, dependent types) is adding a new bit shape on the signature, not refactoring the hole representation.
resolve_type_expr is a 10-line wrapper — you literally won't touch it again unless the metatype check itself changes (which it won't).
Scope lookup is uniform — types and values share the same lookup, the result's type tells you what category, no parallel resolver paths.
The accreting context-passing problem (SemaCtx → ConstDiagAnchorCtx → ConstCtx with overlapping frame fields) goes away because there's ONE evaluator with ONE context. Diag-anchor stays separate from semantic state because they're orthogonal concerns.

My honest take
If the goal is "implement once and don't touch it again" and "match Zig's full capability," this is the architecture. The current incremental options (a)/(b) get Ore through the immediate cases but leave you with three half-evaluators that drift apart over time. Each new comptime feature you add (@typeInfo, generic struct constructors, comptime n: u32) will tempt you to add another path, and the architecture won't push back.

The 5-7 day investment is front-loaded complexity to buy back back-loaded complexity reduction. It's the bet you make when you believe the comptime layer is going to keep growing — which, looking at Phase A → B → the deferred Cases 1 & 4 → the allocator's needs, it pretty clearly will.

If you want this, the implementation order is roughly:

Define TypedValue and add a value column to NodeTypeBuilder. No behavior change yet — just plumbing.
Write eval_expr from scratch, initially delegating to today's type_of_expr and const_eval for branches not yet ported. Replace one branch at a time (literals → refs → calls → builtins → if/switch).
Rewrite resolve_type_expr as the thin wrapper, deleting the four-path mess.
Switch fn signatures to comptime_bits; delete TYPE_VAR.kind.
Wire [N]T and comptime n: u32 through the evaluator.
Delete const_eval.c (now redundant) and the ConstDiagAnchorCtx frame fields.
Tests.
Each step compiles and ships green; the system shrinks as it goes.

===

# Plan: Phase 6 — full port of `eval_expr` (the real one this time)

## Context

The previous Phase 6 attempt shipped 85 lines (rename + redirect MVP) and the user called it out. The architectural endpoint ("eval_expr is THE dispatcher") was technically met but the payoff that motivates combining 4+5 — `TypedValue.value` actually populated for foldable arms, `const_eval.c` deleted, helpers unified — was not. This plan replaces that MVP with the full port. It is split into 9 narrowly-scoped batches; each ships independently with green keep-zone and explicit verification. Batch 4 from the workflow's first cut was a megabatch (3 arms + 12 callsites + 6 deletions); it's been split into 4a/4b/4c/4d so each lands without re-scoping.

User constraints (verbatim, non-negotiable):

- "purity of the architecture is number one so it can be modular and grow to adapt to all the things we will probably end up implementing anyway"
- "no scaffolding for the old architecture. just new architecture. whatever breaks during the migration is fine"
- "I don't want to fix the old architecture. only create the new architecture and delete the old, no patch work, no scaffolding"

End state after all batches land:

- `eval_expr` switch has a direct arm for every expression/statement SyntaxKind. The `default:` arm is an ICE diag — no fallback delegation.
- `TypedValue.value` is populated for: integer/float/bool literals, comptime arithmetic, comptime comparisons, comptime if-fold (taken branch), comptime switch-fold (matched arm), `@sizeOf`/`@alignOf`, `@import` (as IPK_NAMESPACE_VALUE), namespace member access, qualified `Enum.variant`, bare `.variant` resolved via enum_ctx_hint.
- `const_eval.c` and `const_eval.h` are DELETED. `ConstValue` / `ConstKind` enum and constants gone. `ConstDiagAnchorCtx` / `SEMA_CONST_ANCHOR` gone.
- `infer_value_position` deleted (every arm hoisted into eval_expr). `resolve_value_path` deleted (inlined into eval_name). `infer_switch_folded` / `infer_comptime_if` / `infer_comptime_switch` / `sema_comptime_select` / `const_val_eq` / `const_in_range` deleted.
- `type_of_expr` survives as a one-line wrapper around `eval_expr(...).type` (saves churn at ~100 call sites).
- One cycle stack in `eval.c` (`g_eval_cycle_top`), depth-capped at 64, with unified wording `"circular const dependency through '%S'"` (matches old const_eval text so M2(g)/(h) goldens stay byte-identical). Three push sites: local SK_BIND_DECL recurse, KIND_CONSTANT cross-file recurse, SK_FIELD_EXPR namespace-member recurse.
- `SemaCtx.enum_ctx_hint` (single-shot) and `in_comptime` actually read/written.
- `SK_COMPTIME_KW` keyword detection added in `build_fn_type` for `comptime n: u32` so the comptime bit reflects the full set of comptime params (call-site dispatch only — bind-site value-push + monomorph-key threading is its own Phase 7).

---

## Design summary

### Per-arm spec

Every SyntaxKind that can appear in expression/statement position gets a direct arm in `eval_expr`. Three categories:

| Category | Examples |
|---|---|
| **trivial-inline** (≤30 lines in arm body) | PAREN, BIND_DECL, CONTINUE, ASSIGN, DEFER, EXPR_STMT, BREAK, INIT_LIST, LAMBDA, ENUM_REF, POSTFIX, INDEX, LITERAL extensions, RETURN, type-constructor arms already in eval.c |
| **medium-inline** (30–100 lines) | BUILTIN value half, COMPTIME, IF, SWITCH, BLOCK, FIELD |
| **extract-helper** (>100 lines) | BIN (uses promoted binop_*), PREFIX (uses tv_prefix_*), CALL (new `infer_call_expr`), HANDLER (existing `infer_handler_expr`), LOOP (existing `infer_loop_expr`), SLICE (`infer_slice_expr`), PRODUCT (`infer_product_expr`) |

Value-half rules summarized:

- **Literals**: int/byte → IPK_INT_VALUE; float → IPK_FLOAT_VALUE (via hoisted `parse_float_literal_text`); true/false → IP_BOOL_TRUE/IP_BOOL_FALSE; string/asm/nil/unreachable → IP_NONE.
- **Operators** (BIN/PREFIX): fold when both operand `.value`s are non-IP_NONE numeric/bool. Overflow / divide-by-zero / mixed-kind → `.value = IP_NONE` (no diag, type still synthesizes correctly).
- **IF / SWITCH**: if scrutinee/cond `.value` is non-IP_NONE → recurse only on taken/matched branch, forward its TypedValue. Else delegate to runtime helper (`infer_if_expr` / `infer_switch`), `.value = IP_NONE`.
- **BUILTIN**: `@sizeOf`/`@alignOf` produce IPK_INT_VALUE via `db_layout_of_type`. `@import`/`@target`/`@build` produce IPK_NAMESPACE_VALUE. `@TypeOf` stays as today.
- **FIELD**: base.value tag IPK_NAMESPACE_VALUE → namespace-member walk (with cycle push); base.value tag IPK_TYPE + enum → IPK_ENUM_VARIANT_VALUE for qualified `Enum.variant`; runtime → IP_NONE.
- **ENUM_REF (bare `.variant`)**: NEW arm reading `ctx->enum_ctx_hint`; single-shot semantics — reset to DEF_ID_NONE on every sub-expression recursion EXCEPT in SK_BIN_EXPR EQ_EQ/BANG_EQ retry path.
- **COMPTIME**: sets `in_comptime=true` on local SemaCtx, recurses, diag if inner `.value == IP_NONE`.
- **CALL**: reads `in_comptime` for effectful-call diag. Body type via extracted `infer_call_expr`. `.value = IP_NONE` (comptime fn-body execution is Phase 7+).
- **Stmts** (RETURN/BREAK/CONTINUE/ASSIGN/DEFER/BIND_DECL): `.value = IP_NONE`, type per legacy logic.

### Helpers — promote, extract, or delete

| Helper | Action |
|---|---|
| `binop_arith` / `_compare` / `_logical` / `_bitop` / `_orelse` | Promote signature to `(SemaCtx*, SyntaxNode*, opk, TypedValue, TypedValue) → TypedValue`. Fold `.value` when both operand `.value`s populated. |
| `tv_int_add`/`sub`/`mul`/`div`/`mod`/`shl`/`shr`/`compare` | NEW static helpers in infer.c next to binop_arith. Operate on interned IPK_INT_VALUE; decode signedness from `int_value.type` (was `is_unsigned` bit on ConstValue). Preserve `__builtin_*_overflow` checks, INT64_MIN/-1 guards. |
| `tv_float_add`/`sub`/`mul`/`div` | Mirror for IPK_FLOAT_VALUE. |
| `tv_prefix_neg`/`comp`/`not` | NEW static in eval.c for SK_PREFIX_EXPR. |
| `bin_add`/`sub`/`mul`/`div`/`mod`/`shl`/`shr` (in const_eval.c) | DELETED (replaced by tv_int_*). |
| `either_unsigned` | DELETED; signedness reads from IpKey.int_value.type. |
| `parse_float_literal_text` | MOVED from const_eval.c (static) to eval.c (static). |
| `const_val_eq` (infer.c:1428), `const_values_equal` (const_eval.c:889) | DELETED. Replaced by `tv_value_semantic_eq` in NEW `tv_inspect.c`. |
| `const_in_range` (infer.c:1448) | DELETED. Replaced by `tv_value_in_range` in tv_inspect.c. |
| `eval_expr_with_enum_hint` | NEW entry point in eval.c. Sets `enum_ctx_hint` on local SemaCtx copy, dispatches to eval_expr. Used by SK_BIN_EXPR EQ_EQ/BANG_EQ retry + SK_SWITCH_EXPR pattern recursion. |
| `infer_call_expr` / `infer_field_expr` / `infer_index_expr` / `infer_slice_expr` / `infer_if_expr` / `infer_block_stmt` / `infer_product_expr` | NEW non-static extractions from `infer_value_position` arms. |
| `infer_switch` / `infer_loop_expr` / `infer_handler_expr` | Already non-static; called from new eval_expr arms. `infer_switch`'s line-1109 fold-probe REMOVED (Batch 4c — eval_expr does the fold first; runtime path takes over only when scrut.value is IP_NONE). |
| `resolve_value_path` (infer.c:697) | DELETED. Inlined into `eval_name` (eval.c). The lookup chain — body-scope, signature type_name_map, namespace — unifies. |
| `infer_switch_folded` / `infer_comptime_if` / `infer_comptime_switch` / `sema_comptime_select` | DELETED. Subsumed by SK_SWITCH_EXPR / SK_IF_EXPR / SK_COMPTIME_EXPR arms. |
| `infer_value_position` | DELETED after every arm hoists. Default arm replaced by ICE diag. |
| `type_of_expr` | KEEP as one-line wrapper to `eval_expr(...).type`. |

### `tv_inspect.{c,h}` (NEW file)

Four helpers, no new IP kinds — all read via `ip_tag` + `ip_key`:

- `bool tv_value_semantic_eq(struct db *s, IpIndex a, IpIndex b)` — decoded scalar equality. IPK_INT_VALUE pairwise (signed/unsigned-aware via `.type`); IPK_FLOAT_VALUE bit-pattern; IP_BOOL_TRUE/FALSE identity; IPK_NAMESPACE_VALUE on nsid; IPK_ENUM_VARIANT_VALUE on (enum_def, variant_idx). CRITICAL: `5: comptime_int` and `5: u32` intern to different IpIndex but compare equal here.
- `bool tv_fits_in(struct db *s, IpIndex value, IpIndex target_type, const char **out_lo, const char **out_hi)` — replaces `db_const_value_fits_in`.
- `const char *tv_value_to_str(struct db *s, IpIndex value, char *buf, size_t buflen)` — replaces `db_const_value_to_str`.
- `bool tv_value_in_range(const SemaCtx *ctx, SyntaxNode *range_node, IpIndex scrut_value)` — replaces `const_in_range`. Internally calls `eval_expr` on lo/hi.
- `IpIndex tv_int_compare(struct db *s, IpIndex l, IpIndex r, SyntaxKind opk)` — ordered LT/LE/GT/GE returning IP_BOOL_TRUE/FALSE or IP_NONE. Decodes signedness from `int_value.type` (u64 vs i64 etc.); comptime_int adopts the typed operand's signedness when mixed.

### Cycle stack — unified, depth-capped

ONE `EvalCycleFrame` linked list (`g_eval_cycle_top` in eval.c). Three push sites:

1. `eval_name`'s local SK_BIND_DECL RHS recurse (existing — eval.c:190-195). Batch 1 adds depth-cap walk.
2. `eval_name`'s KIND_CONSTANT cross-file RHS recurse (MISSING — eval.c:293-297). Batch 1 adds push + depth-cap walk.
3. SK_FIELD_EXPR namespace-member RHS recurse (NEW arm in Batch 3). Includes depth-cap walk from the start.

Depth cap: walk `prev` chain on each push; if length >= 64, emit `"const chain too deep (max 64)"` and return `{IP_ERROR_TYPE, IP_ERROR_TYPE}` WITHOUT pushing.

Diag wording flips from Phase 3's `"cycle in comptime binding for '%S'"` to `"circular const dependency through '%S'"` (matches `const_eval.c`'s wording so M2(g)/(h) goldens stay byte-identical when const_eval.c dies).

### Cross-file SemaCtx swap

At every cross-file/cross-decl push site, build a local SemaCtx copy:

```c
SemaCtx local = *ctx;
local.decl_ast_map = db_get_decl_ast_id_map_untracked(s, target_def);
local.decl_key     = target_ast_id.idx;
local.file_local   = target_file;
local.enum_ctx_hint = DEF_ID_NONE;  // single-shot — never propagates across decls
// in_comptime INHERITED — a comptime caller stays comptime through cross-file
```

### `enum_ctx_hint` reset list (single-shot enforcement)

Every arm that recurses on a sub-expression NOT in `.variant` pattern position MUST zero the hint on a local SemaCtx copy before recursing. Exhaustive list:

- SK_BIN_EXPR: reset on BOTH operands EXCEPT in EQ_EQ/BANG_EQ where the hint propagates to the bare-variant side for bidirectional retry.
- SK_CALL_EXPR: reset on callee AND every arg (Phase 7 may set per-arg hints when arg type is an enum).
- SK_FIELD_EXPR: reset on base.
- SK_INDEX_EXPR / SK_SLICE_EXPR: reset on base AND index/lo/hi.
- SK_PREFIX_EXPR / SK_POSTFIX_EXPR: reset on operand.
- SK_PAREN_EXPR: passthrough (no reset — inner is in the SAME position as the paren).
- SK_RETURN_STMT / SK_ASSIGN_EXPR: reset on value/rhs/lhs.
- SK_BLOCK_STMT: reset on every stmt EXCEPT the tail (which inherits the block's position).
- SK_IF_EXPR: reset on cond; then/else branches inherit the if's position.
- SK_SWITCH_EXPR: reset on scrutinee; pattern recursion gets enum_ctx_hint = scrut's enum_def; body recursion inherits the switch's position.
- SK_OPTIONAL_TYPE / SK_PTR_TYPE / SK_SLICE_TYPE / SK_MANY_PTR_TYPE / SK_ARRAY_TYPE / SK_CONST_TYPE: reset on inner (type-constructor recursion is never in variant position).

### `SK_COMPTIME_KW` wiring (in `build_fn_type`)

```c
bool has_comptime_kw = false;
{
  uint32_t pcc = syntax_node_num_children(el.node);
  for (uint32_t cj = 0; cj < pcc; cj++) {
    GreenElement g = green_node_child(syntax_node_green(el.node), cj);
    if (g.kind == GREEN_ELEM_TOKEN && green_token_kind(g.token) == SK_COMPTIME_KW) {
      has_comptime_kw = true; break;
    }
  }
}
```

Combined with the existing `ann_is_type` / `ann_is_anytype` checks: implicit comptime (annotation = `type`/`anytype`) still mints a TYPE_VAR hole; explicit-comptime value-shaped param (`comptime n: u32`) just sets `comptime_bits[i] = 1` without minting a hole. `typevalued_bits[i]` stays 0 for the keyword path.

**SCOPE GUARDRAIL (lesson from previous Phase 6):** the bind-site value-push for `comptime n: u32` AND the monomorphization-key threading of comptime value-args are OUT OF SCOPE for Phase 6. Phase 6 only wires the call-site dispatch bit (which is harmless if the threading isn't there — the call-site eval evaluates the arg, ignores the value, and the body uses `n` as if runtime u32 — same as today). Phase 7 ships the value-threading with its own fixture (`square(5)` vs `square(6)` produce two distinct instance keys). This is named explicitly as a future commit — no "deferred follow-up" hand-wave.

### IPK_BOOL_VALUE plumbing audit

Reserved indices `IP_BOOL_TRUE` (intern_pool.h:140) and `IP_BOOL_FALSE` (intern_pool.h:141) already exist. The design uses them directly (no new `IPK_BOOL_VALUE` kind). **Batch 1 verification (BLOCKER)**: confirm `ip_tag(s, IP_BOOL_TRUE)` returns a tag distinct from `IP_TAG_INT_VALUE` and `IP_TAG_NONE` (probably `IP_TAG_RESERVED_VALUE` or similar). If `tv_value_semantic_eq` can't disambiguate IP_BOOL_TRUE from IPK_INT_VALUE(1, comptime_int), add an `IP_TAG_BOOL` entry OR add a new `IPK_BOOL_VALUE` kind before Batch 2 ships.

### `ConstDiagAnchorCtx` field accounting

`ConstDiagAnchorCtx` (const_eval.h:92-103) has 5 fields: `decl_ast_map`, `decl_key`, `enclosing_fn`, `type_subst`, `types`. SemaCtx already has all five. **Batch 1 verification**: grep SemaCtx fields to confirm. If any is missing (especially `type_subst` for `@sizeOf(t)` inside generic-fn bodies), add it with explicit value-equivalence to ConstDiagAnchorCtx use sites BEFORE Batch 6's `resolve_type_expr_from_const_eval_ctx` deletion. Fixture: `fn(t: type) -> u32 = @sizeOf(t)` called with t=u32 and t=u64 produces 4 and 8.

---

## Implementation batches (9 commits)

Each batch ships independently with green keep-zone. Listed in execution order.

### Batch 1 — Foundations (no behavior change)

- Create `src/db/query/tv_inspect.{c,h}` with `tv_value_semantic_eq`, `tv_fits_in`, `tv_value_to_str`, `tv_value_in_range`, `tv_int_compare`. (`tv_int_*` arith helpers also land here as static in infer.c — moved next to binop_arith.)
- Move `parse_float_literal_text` from const_eval.c (static) into eval.c (static).
- Promote `binop_arith` / `_compare` / `_logical` / `_bitop` / `_orelse` signatures to TypedValue input/output. Callers (only `infer_value_position`'s SK_BIN_EXPR) wrap operands as `{type, IP_NONE}` for now — no behavior change.
- Unify cycle diag wording at eval.c:183 to `"circular const dependency through '%S'"`.
- Add depth-cap walk (≥64 → bail with `"const chain too deep (max 64)"`) at the existing local SK_BIND_DECL push site AND at the previously-missing KIND_CONSTANT cross-file recurse (eval.c:293-297 also gets a push).
- Inline `resolve_value_path` (infer.c:697) into `eval_name` — body-scope/type_name_map/namespace chain unifies. Delete `resolve_value_path`.
- **Audits**: confirm `ip_tag(IP_BOOL_TRUE)` is distinct (add IPK_BOOL_VALUE if not); confirm SemaCtx has all 5 ConstDiagAnchorCtx-equivalent fields.

**Verification**: `make && make test`. M2(g)/(h) goldens still green (wording now matches). Add `tools/tv_inspect_test.c` exercising the four helpers + `tv_int_compare` on signed/unsigned mixed comptime_int. Fixture: 100-deep `D0 :: 1; Dn :: D(n-1) + 1` produces `"const chain too deep (max 64)"`. Fixture: body-local `f :: fn() -> u32 = { x :: 5; x }` resolves through `eval_name`.

### Batch 2 — Literal + Operators value half

- SK_LITERAL_EXPR: wire SK_FLOAT_LIT → IPK_FLOAT_VALUE; SK_TRUE_KW/SK_FALSE_KW → IP_BOOL_TRUE/FALSE.
- Add SK_PAREN_EXPR arm (forward inner TypedValue).
- Add SK_PREFIX_EXPR arm with `tv_prefix_neg`/`_comp`/`_not` value-folding.
- Add SK_POSTFIX_EXPR arm (`.value = IP_NONE`, type-only).
- `binop_arith` / `_compare` / `_logical` / `_bitop` / `_orelse` bodies actually fold value half (calling tv_int_*/tv_float_*).

**Verification**: `examples/arith_folds.ore` — comptime int + float arithmetic; per-node value half visible via fingerprint dump. const_eval.c still in tree (untouched).

### Batch 3 — Builtin value + Field + EnumRef + Comptime + Call (in_comptime wiring)

- SK_BUILTIN_EXPR: produce IPK_INT_VALUE for `@sizeOf`/`@alignOf` via `db_layout_of_type`; produce IPK_NAMESPACE_VALUE for `@import`/`@target`/`@build`.
- NEW SK_FIELD_EXPR arm: consume IPK_NAMESPACE_VALUE (member walk + cycle push — this is the THIRD push site, with depth-cap walk); produce IPK_ENUM_VARIANT_VALUE for qualified `Enum.variant`; delegate runtime base to `infer_field_expr` (NEW extracted helper).
- NEW SK_ENUM_REF_EXPR arm: read `ctx->enum_ctx_hint`; emit `"bare '.variant' requires an enum-typed context"` on miss; produce IPK_ENUM_VARIANT_VALUE on hit.
- NEW `eval_expr_with_enum_hint(ctx, node, enum_def)` entry point in eval.c.
- NEW SK_COMPTIME_EXPR arm: set `in_comptime=true` on local SemaCtx, recurse, emit `"comptime expression must be comptime-foldable"` if inner `.value` is IP_NONE.
- NEW SK_CALL_EXPR arm: extract `infer_call_expr` from infer.c (non-static); read `ctx->in_comptime` to fire effectful-call diag when callee's effect_row is non-empty.

**Verification**: `examples/comptime_namespace.ore` (`@target.os == .macos`; `@import`-based field access). M2(g)/(h) cycle diags routed through SK_FIELD_EXPR confirm wording. New fixture: cross-namespace ref chain (`A :: B.x; B :: struct { x :: A }`) fires `"circular const dependency through ..."`. Bidirectional enum compare regression test.

### Batch 4a — SK_BIN_EXPR full arm

- NEW SK_BIN_EXPR arm in eval_expr — full version including bidirectional enum-variant retry (`enum_val == .variant` and vice versa via `eval_expr_with_enum_hint`).
- Delete `const_val_eq` (infer.c:1428).
- Migrate infer.c BIN-adjacent db_const_eval sites if any are reached (most are switch/comptime helpers in 4b/4c).

**Verification**: `examples/bidirectional_enum_eq.ore`. Fixture: `comptime_check :: 0xFFFFFFFFFFFFFFFF > 1` folds to IP_BOOL_TRUE when the operand is u64-typed (tests `tv_int_compare` signedness dispatch).

### Batch 4b — SK_IF_EXPR arm + infer_comptime_if deletion

- NEW SK_IF_EXPR arm: cond.value IP_BOOL_TRUE → recurse only on then; IP_BOOL_FALSE → recurse only on else; otherwise delegate to `infer_if_expr` (NEW extracted helper).
- Migrate `infer.c:1384` (infer_comptime_if cond) → SK_COMPTIME_EXPR drives the foldability check.
- DELETE `infer_comptime_if`.

**Verification**: `examples/comptime_if_dead_branch.ore` — `comptime if (true) {…} else {…}` only types the taken branch.

### Batch 4c — SK_SWITCH_EXPR arm + fold helpers deletion

- NEW SK_SWITCH_EXPR arm: scrut.value non-IP_NONE → match patterns via `tv_value_semantic_eq` + `tv_value_in_range`; recurse on matched arm with forwarded TypedValue. Pattern recursion uses `eval_expr_with_enum_hint(enum_def)` when scrutinee is enum-typed.
- Remove `infer_switch`'s line-1109 fold-probe (it now just synthesizes the runtime type when called from the arm's else branch).
- Migrate infer.c db_const_eval sites: 1109, 1193, 1195, 1231, 1242, 1459, 1462, 1543, 1546, 1612.
- DELETE `infer_switch_folded`, `infer_comptime_switch`, `const_in_range`, `pattern_is_underscore` (const_eval's), `const_values_equal` (const_eval's).

**Verification**: `examples/switch_fold_enum.ore`, `examples/switch_fold_int_signed.ore`. Goldens: comptime switch over CONST_ENUM_VARIANT scrutinee picks the matched arm; `5: i32` and `5: comptime_int` both match a `5` pattern (semantic eq).

### Batch 4d — SK_COMPTIME_EXPR drives both paths; sema_comptime_select deletion

- Confirm SK_COMPTIME_EXPR arm's `in_comptime` flag correctly drives BOTH the SK_IF_EXPR fold path (Batch 4b) AND the SK_SWITCH_EXPR fold path (Batch 4c).
- Migrate infer.c:1644 (sema_comptime_select default).
- DELETE `sema_comptime_select`.

**Verification**: `examples/comptime_switch_with_arms.ore`. The comptime-foldable check fires only at SK_COMPTIME_EXPR wrapper, not inside IF/SWITCH directly.

### Batch 5 — coerce.c callers + remaining arms + delete infer_value_position

- Migrate coerce.c:1191, 1212, 1263 — replace `db_const_eval` + `db_const_value_fits_in` / `_to_str` with `eval_expr` + `tv_fits_in` / `tv_value_to_str`.
- Add remaining eval_expr arms: SK_INDEX_EXPR, SK_SLICE_EXPR, SK_RETURN_STMT, SK_BLOCK_STMT, SK_LOOP_EXPR (delegate to infer_loop_expr), SK_HANDLER_EXPR (delegate to infer_handler_expr), SK_LAMBDA_EXPR, SK_ASSIGN_EXPR, SK_DEFER_STMT, SK_EXPR_STMT, SK_BREAK_STMT, SK_CONTINUE_STMT, SK_BIND_DECL, SK_PRODUCT_EXPR, SK_INIT_LIST.
- Extract NEW non-static helpers as needed: `infer_index_expr`, `infer_slice_expr`, `infer_block_stmt`, `infer_product_expr`.
- Replace eval_expr's `default:` arm with ICE diag.
- DELETE `infer_value_position`.

**Verification**: `grep -n 'case SK_' src/db/query/eval.c | wc -l` ≈ 30. `grep -rn 'infer_value_position' src/ tools/` zero hits. All keep-zone fixtures green.

### Batch 6 — Delete const_eval.{c,h} + SK_COMPTIME_KW + final cleanup

- DELETE `src/db/query/const_eval.c` and `src/db/query/const_eval.h`.
- DELETE `SEMA_CONST_ANCHOR` from `type_layer.h`.
- DELETE `resolve_type_expr_from_const_eval` and `_ctx` from `type.c`.
- DELETE `#include "const_eval.h"` lines (infer.c, type.c).
- Add SK_COMPTIME_KW detection in `build_fn_type` per the comptime_keyword_wiring section above.
- Build system: the project's Makefile uses `find $(CORE_DIRS) -name '*.c'` so file deletion auto-removes const_eval.o from the link. `tv_inspect.c` was auto-discovered the same way in Batch 1.

**Verification**: `make clean && make` (no `const_eval.o` emitted). `examples/comptime_value_param.ore` — `fn(comptime n: u32) -> u32 = n * n` typechecks; the IPK_FN_TYPE has `comptime_bits & 1` set (verify via fingerprint inspector or `ip_dump` tool — if neither exists, add a one-off probe in a new keep-zone test).

### Batch 7 — Future (NOT in Phase 6 scope; named here so it doesn't drift)

- Bind-site value-push for `comptime n: u32` (push `(u32, IPK_INT_VALUE(n_value))` at the param bind in `db_query_infer_instance`).
- Monomorphization-key threading of comptime value-args (so `square(5)` and `square(6)` produce two distinct instance keys).
- Required fixture: `square(5)` and `square(6)` produce two distinct fn-instance keys; verify via instance-pool count.

---

## Critical files

**Create:**

- `src/db/query/tv_inspect.{c,h}` — Batch 1.

**Modify (significantly):**

- `src/db/query/eval.c` — Batches 1-6 progressively add arms; final state has every SK_* arm in its switch + cycle stack + cross-decl swap + `eval_expr_with_enum_hint`.
- `src/db/query/infer.c` — Batches 1-5 promote helpers, extract new ones, delete subsumed comptime helpers; Batch 5 deletes `infer_value_position` and `resolve_value_path`.
- `src/db/query/type.c` — Batch 6 deletes `resolve_type_expr_from_const_eval{,_ctx}`; `build_fn_type` gets SK_COMPTIME_KW detection.
- `src/db/query/coerce.c` — Batch 5 migrates 3 callsites.
- `src/db/query/type_layer.h` — Batch 6 deletes `SEMA_CONST_ANCHOR` macro.
- `src/db/intern_pool/intern_pool.h` — Batch 1 audit may add `IP_TAG_BOOL_VALUE` or `IPK_BOOL_VALUE` if needed.

**Delete:**

- `src/db/query/const_eval.c` — Batch 6.
- `src/db/query/const_eval.h` — Batch 6.

---

## Verification (final, after Batch 6)

```sh
# A. const_eval API gone
grep -rn 'db_const_eval\|db_const_value_fits_in\|db_const_value_to_str\|ConstValue\|ConstKind\|CONST_INT\|CONST_FLOAT\|CONST_BOOL\|CONST_NAMESPACE\|CONST_ENUM_VARIANT\|CONST_TYPE\|ConstDiagAnchorCtx\|SEMA_CONST_ANCHOR\|const_eval_anchor\|resolve_type_expr_from_const_eval\|sema_comptime_select\|infer_comptime_if\|infer_comptime_switch\|infer_switch_folded\|const_val_eq\|const_in_range\|const_values_equal\|none_value\|ORE_CONST_CYCLE_MAX' src/ tools/
# → zero hits

# B. Files deleted
ls src/db/query/const_eval.* 2>&1
# → No such file or directory

# C. infer_value_position removed
grep -rn 'infer_value_position\|type_of_expr_impl' src/ tools/
# → zero hits

# D. eval_expr switch is exhaustive
grep -nE 'case SK_' src/db/query/eval.c | wc -l
# → ~30

# E. Cycle wording unified
grep -rn 'cycle in comptime binding\|circular const dependency' src/
# → only 'circular const dependency through' (in eval.c)

# F. tv_inspect helpers landed
ls src/db/query/tv_inspect.*
# → tv_inspect.c, tv_inspect.h

# G. New value kinds have producers AND consumers
grep -n 'IPK_NAMESPACE_VALUE\|IPK_ENUM_VARIANT_VALUE\|IP_TAG_NAMESPACE_VALUE\|IP_TAG_ENUM_VARIANT_VALUE' src/db/query/eval.c
# → IPK_NAMESPACE_VALUE produced (SK_BUILTIN_EXPR), consumed (SK_FIELD_EXPR);
#   IPK_ENUM_VARIANT_VALUE produced (SK_ENUM_REF_EXPR + SK_FIELD_EXPR), consumed (SK_BIN_EXPR + SK_SWITCH_EXPR)

# H. SK_COMPTIME_KW path
grep -n 'SK_COMPTIME_KW\|has_comptime_kw' src/db/query/type.c
# → at least one hit in build_fn_type

# I. Build clean
make clean && make 2>&1 | tee /tmp/build.log; echo exit=$?
# → exit=0; no warnings about const_eval

# J. Keep-zone gate
make test-keep-zone
# → all green (M2(g)/(h) wording flipped to 'circular const dependency through')

# K. Cycle depth cap (NEW fixture)
python3 -c 'print("\n".join(f"D{i} :: D{i-1} + 1" for i in range(1, 66))); print("D0 :: 1")' > /tmp/depth.ore
./build/ore /tmp/depth.ore 2>&1 | grep -F 'const chain too deep (max 64)'

# L. Bidirectional enum compare fold (NEW fixture)
cat > /tmp/enum_cmp.ore <<'EOF'
Os :: enum { macos, linux }
host :: Os.macos
is_mac :: host == .macos
EOF
# is_mac's value half folds to IP_BOOL_TRUE
```

---

## What this plan explicitly does NOT do (named so it doesn't drift)

- **Bind-site value-push for `comptime n: u32`** — Batch 7 (Phase 7).
- **Monomorphization key threading of comptime value-args** — Batch 7.
- **Comptime fn-body execution** (folding `square(5)` to `25`) — Phase 7+.
- **Composite literal value folding** (SK_PRODUCT_EXPR / SK_INIT_LIST stay `.value = IP_NONE`) — Phase 7+.
- **Constant-array indexing fold** (SK_INDEX_EXPR / SK_SLICE_EXPR stay `.value = IP_NONE`) — Phase 7+.
- **`@typeInfo` reflection** — Phase 8+.
- **`check_expr` migration** — stays in check.c; its bidirectional arms wrap `eval_expr` via `type_of_expr(...).type`. Phase 8+ decision.

---

## Bottom line

Three half-evaluators (`type_of_expr_impl`, `const_eval`'s `eval_inner`, `resolve_type_expr`'s former switch) → ONE (`eval_expr`). The work is split into 9 batches that each ship with verification — no megabatches, no "incremental hoisting" hand-waves, no value-push deferrals without a named follow-up commit. All 7 corners the adversarial review caught are addressed in-batch: Batch 4 split into 4a-d; Batch 6 explicitly names the SK_COMPTIME_KW value-push as Phase 7 with required fixture; Batch 1 audits IP_BOOL_TRUE tag plumbing and ConstDiagAnchorCtx-equivalent SemaCtx fields; Batch 1 includes the previously-missing KIND_CONSTANT cross-file cycle push; Batch 3 puts the SK_FIELD_EXPR push site with the namespace-member arm; binop_compare signedness goes through new `tv_int_compare`; resolve_value_path inlining is in Batch 1 explicitly with a body-local-const fixture.

After Batch 6 ships: `make clean && make` produces no `const_eval.o`; `eval_expr` is the sole dispatcher; every foldable expression actually folds.