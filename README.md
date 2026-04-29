Phase 1: Query Kernel
Add src/sema/query.h / query.c.

Core pieces:

typedef enum {
    QUERY_EMPTY,
    QUERY_RUNNING,
    QUERY_DONE,
    QUERY_ERROR,
} QueryState;

typedef enum {
    QUERY_TYPE_OF_DECL,
    QUERY_LAYOUT_OF_TYPE,
    QUERY_CONST_EVAL,
    QUERY_INSTANTIATE_DECL,
    QUERY_EFFECT_SIG,
    QUERY_BODY_EFFECTS,
} QueryKind;

struct QuerySlot {
    QueryState state;
    bool cycle_reported;
};
Add helpers:

bool sema_query_begin(struct Sema*, struct QuerySlot*, QueryKind, struct Span);
void sema_query_done(struct QuerySlot*);
void sema_query_error(struct QuerySlot*);
Also add a small query stack to Sema for debugging cycles.

First use: adapt existing Decl.state / Decl.type into this pattern without changing behavior much.

Phase 2: Rename Around Queries
Keep sema_type_from_decl() for compatibility, but introduce clearer APIs:

struct Type* sema_type_of_decl(struct Sema*, struct Decl*);
struct Type* sema_signature_of_decl(struct Sema*, struct Decl*);
Eventually callers should use these instead of deriving from facts.

Files:

decls.c (line 280)
decls.h (line 9)
Phase 3: layout_of_type
This is the first real new query because of @sizeOf / @alignOf.

Add src/sema/layout.h / layout.c.

struct TypeLayout {
    uint64_t size;
    uint64_t align;
    bool complete;
};
Query:

struct TypeLayout sema_layout_of_type(struct Sema*, struct Type*);
This handles:

primitive sizes
pointer/slice layout
struct field layout
target ABI
illegal by-value recursive structs
allowed pointer-recursive structs
This supports malloc.ore lines like @sizeOf(Header) and @alignOf(t).

Phase 4: const_eval_expr
Add src/sema/const_eval.h / const_eval.c.

Needed for:

constants like PAGE_SIZE
comptime switch @target.os
@sizeOf, @alignOf
build options
later @typeName, @tagName, @returnType
Key point: const eval needs an environment.

struct ConstValue sema_const_eval_expr(struct Sema*, struct Expr*, struct ComptimeEnv*);
Do not cache by Expr* alone.

Phase 5: Instantiation
Add src/sema/instantiate.h / instantiate.c.

Needed for:

alloc :: fn(comptime t: type, count: usize) []t
free  :: fn(slice: anytype) void
Core API:

struct Instantiation* sema_instantiate_decl(
    struct Sema*,
    struct Decl* generic_decl,
    struct ComptimeArgTuple args
);
This should own:

substitutions for comptime params
specialized function type
specialized effect signature
checked body/facts for that instantiation
Phase 6: Effect Queries + Solver
Keep effect annotations simple at first, then add:

struct EffectSig* sema_effect_sig_of_callable(...);
struct EffectSet* sema_body_effects_of(...);
bool sema_solve_effect_rows(...);
Effect row solving does not disappear. Queries give it a home and cache the result.

This is where <Allocator(s) | e> and handlers become tractable.

Phase 7: Facts Become Body-Local
Eventually replace global facts with:

struct CheckedBody {
    struct Decl* decl;
    struct Instantiation* instantiation;
    Vec* facts;
};
A fact should mean:

this Expr has this Type in this checked body / instantiation
not globally.

Implementation Order
I’d land it like this:

Add query kernel, no behavior change.
Wrap current Decl type resolution with query helpers.
Add layout_of_type() for primitives, pointers, structs.
Make @sizeOf / @alignOf call layout query.
Add const_eval_expr() for constants and target/build values.
Add instantiation cache for comptime t: type.
Move expression facts under checked body/instantiation.
Add effect-row solving queries.
First implementation slice should be small: query kernel + existing Decl.type resolution using it, with all current tests still green.





6:14 PM
ok please plan phase 1 if needed and implement
6:15 PM



Worked for 2m 55s
Implemented Phase 1.

Plan executed:

Added a small typed query kernel.
Added a sema query stack for cycle/debug context.
Replaced the bespoke Decl resolution state with QuerySlot.
Kept current behavior and diagnostics intact.
Key changes:

Added query.h (line 10) and query.c (line 28) with QUERY_EMPTY/RUNNING/DONE/ERROR, QueryKind, QuerySlot, and sema_query_begin/succeed/fail.
Added query_stack to Sema (line 34), initialized in sema.c (line 84).
Changed Decl to own type_query in name_resolution.h (line 71).
sema_type_from_decl() / declaration signature resolution now use the query lifecycle in decls.c (line 285).
Existing self-cycle diagnostic still reports circular definition of 'bad'.
Verification: make test passes, 35 passed, 0 failed.

Note: ore.dSYM/Contents/Resources/DWARF/ore remains modified