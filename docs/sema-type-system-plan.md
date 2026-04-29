# Sema And Type System Plan

This document captures the intended direction for Ore's semantic analysis, type checking, effect checking, and comptime machinery. The aim is to keep the compiler architecture explicit before the current `sema` skeleton grows into the real checker.

## High-Level Shape

Ore should use a small set of complementary strategies rather than one global inference system:

- Nominal core types for declared structs, enums, and effects.
- Bidirectional type checking for ordinary expressions.
- A targeted effect-constraint worklist for row-polymorphic effects.
- A comptime evaluator and specialization layer for generics, `anytype`, type-valued parameters, `@returnType`, and generated declarations.

The core pipeline should stay roughly:

```text
parse
  -> name resolution
  -> sema/type checking
      -> bidirectional expression checking
      -> comptime value evaluation when required
      -> effect collection and constraints
      -> specialization / generated declarations where required
  -> later IR/codegen
```

Comptime is not a second parser, resolver, and type checker. It should be an evaluator over resolved and checked compiler IR or typed AST facts, with controlled callbacks into sema when specialization creates new declarations or types.

## Source Layout

The sema implementation is split by stable responsibility, not by every future roadmap phase:

```text
src/sema/
  sema.h / sema.c          public pass entrypoint, Sema context, facts, dumps
  type.h / type.c          type constructors, primitive mapping, compatibility helpers
  checker.h / checker.c    expression inference/checking and AST walk phase
  decls.h / decls.c        declaration signature collection and decl-to-type mapping
  effects.h / effects.c    effect signature parsing/printing and future constraints
  comptime.h / comptime.c  comptime phase hook and future evaluator boundary
  sema_internal.h          internal diagnostics/fact-recording helpers
```

The initial split should preserve the current permissive behavior while making the future phases explicit. New checker behavior should be added behind these boundaries instead of growing another monolithic `sema.c`.

## Type Model

Declared structs, enums, and effects are nominal:

```ore
Buffer :: struct
    data : []u8
```

`Buffer` is a distinct declared type. Another struct with the same fields is not automatically interchangeable with `Buffer`.

Anonymous product literals are checked against expected types bidirectionally:

```ore
make_buffer :: fn(count: usize) <Allocator(s) | e> Buffer
    .{ .data = alloc(u8, count) }
```

In infer mode, `.{ .data = ... }` may be an anonymous product literal. In check mode with expected type `Buffer`, sema should check that `Buffer` is a struct, `.data` exists, and the value assigned to `.data` is compatible with the field type `[]u8`.

Structural or duck-typed behavior should be explicit and generic, mostly through `anytype` and comptime specialization. Ore should not be structurally typed by default.

## Bidirectional Checking

The checker should have two first-class operations:

```text
infer_expr(expr) -> Type*
check_expr(expr, expected_type) -> ok/error
```

Use `infer_expr` when context does not provide an expected type. Use `check_expr` when syntax or declarations do provide one:

- Annotated bindings: `x: T = value` checks `value` against `T`.
- Function bodies check against declared return types.
- Function arguments check against parameter types.
- Product literals check against expected nominal struct/product types.
- Array literals check against expected array/slice element types when available.
- Comptime parameters check against `type` or other comptime-compatible expected types.

Bidirectional checking should be the baseline. It is the simplest inference strategy that still lets the language feel expressive without needing a global type solver immediately.

## Effect Checking And Row Polymorphism

Effect checking should use targeted constraints rather than a global type-inference constraint solver.

At a high level:

```text
body_effects <= allowed_effect_signature
```

For:

```ore
make_buffer :: fn(count: usize) <Allocator(s) | e> Buffer
    .{ .data = alloc(u8, count) }
```

The body requires `Allocator(s)`. The annotation allows `Allocator(s)` plus row variable `e`, so the effect constraint succeeds.

The effect subsystem should eventually have a small worklist of constraints such as:

```text
EffectConstraint:
  required effects
  allowed signature / row
  source span
```

The worklist can initially be module-local or function-local. It should solve only effect rows and scoped effects, not all value types.

Useful first constraints:

- Required named effect must appear in the allowed signature or be captured by an open row.
- Required scoped effect must match both effect declaration and scope token.
- Closed signatures reject extra effects.
- Handlers subtract discharged effects from the body before comparing against the enclosing signature.

## Comptime And Specialization

Comptime should produce compile-time values and declarations from checked compiler representation.

Important compile-time values include:

- integers, bools, strings
- type values
- declaration references
- function references
- struct/product values
- effect signatures or type metadata later

Initial comptime use cases:

- Array sizes.
- `comptime t: type` parameters.
- Return types like `[]t` where `t` is a comptime type value.
- `@returnType(action)`.
- `anytype` specialization.

Later comptime use cases:

- Type-level functions.
- Generated declarations, such as creating methods/getters/setters/updaters for a type constructor like `Array u8`.
- Reflection over types and declarations.
- Compile-time loops and conditionals.

The important rule is that comptime evaluation should reuse the normal frontend. It should not own a duplicate parser, resolver, or typechecker.

## Responsibility Split

Name resolution should answer identity and scope questions:

- Which declaration does this identifier reference?
- Which module does this import alias reference?
- Which effect-row or scope-token declaration does this annotation introduce?
- Are lexical control-flow constructs placed in a valid scope?

Sema/type checking should answer meaning and compatibility questions:

- What type does this expression have?
- Is this expression compatible with an expected type?
- Is this callee callable, and do arguments match parameters?
- Does a function body match its declared return type?
- Does a field exist on the expected nominal type?
- Is an enum variant valid for the expected enum type?
- Which effects does this body require, and are they allowed?
- Which expressions must be evaluated at comptime?

Comptime should answer value-production questions:

- Can this checked expression be evaluated during compilation?
- What compile-time value does it produce?
- Does specialization produce new declarations or a new instantiated type?

## Roadmap

### Current State

The existing sema pass records facts and rough types but remains permissive. Many unsupported cases become `unknown_type` instead of diagnostics. Name resolution now provides a much better base: resolved identifiers, module imports, effect annotations, loop/control-flow validation, and unresolved-name diagnostics.

### Phase 1: Bidirectional Type Checker Baseline

Goal: turn sema from a fact collector into a real checker for ordinary functions and values.

Likely scope:

- Introduce a clear `infer_expr` / `check_expr` split.
- Add type equality and assignment compatibility helpers.
- Build stable function types from parameter and return annotations.
- Check function bodies against declared return types.
- Check simple function calls for callable callee, arity, argument types, and return type.
- Check annotated bindings against their declared type.
- Check expected-type product literals against nominal structs for basic field existence and field value types.
- Add focused positive and negative regression tests.

Phase 1 should avoid full effect row solving, full comptime execution, and `anytype` specialization. It should create the rails those later systems need.

### Phase 2: Nominal Field And Variant Checking

Goal: finish type-dependent member semantics.

Likely scope:

- Resolve/check `value.field` after the object type is known.
- Diagnose missing fields on nominal structs.
- Check enum variant references against expected enum types.
- Improve product literal diagnostics for unknown, missing, and duplicate fields.

### Phase 3: Comptime Type Values

Goal: support the first useful comptime semantics without arbitrary compile-time execution.

Likely scope:

- Represent `ComptimeValue` for type values and simple literals.
- Evaluate array sizes.
- Support `comptime t: type` function parameters.
- Substitute comptime type parameters into return and parameter types.
- Add early `@returnType` support where the argument function type is already known.

### Phase 4: Effect Collection

Goal: collect body effects precisely enough to check closed signatures.

Likely scope:

- Mark effect operations distinctly in sema facts or declarations.
- Collect effects required by calls inside a function body.
- Check closed signatures like `<Exn>` reject unlisted effects.
- Keep open rows permissive until Phase 5.

### Phase 5: Effect Row Worklist

Goal: solve row-polymorphic and scoped effect constraints.

Likely scope:

- Add effect constraint records and a worklist.
- Match named and scoped effects against allowed rows.
- Track row variables like `e` in `<Allocator(s) | e>`.
- Check scope-token compatibility for scoped effects.
- Add handler discharge constraints.

### Phase 6: Specialization And Generated Declarations

Goal: make comptime generics and generated APIs real.

Likely scope:

- Specialize functions for `anytype` and comptime parameters.
- Cache specializations.
- Allow comptime type functions to produce new nominal or anonymous types.
- Attach generated declarations to type/module scopes before later checks need them.

## Non-Goals For The First Typechecker Slice

Do not start with:

- A global Hindley-Milner style solver.
- Full arbitrary comptime function execution.
- Full row-polymorphic effect solving.
- Generic specialization caching.
- Borrow/escape checking.
- Code generation concerns.

Those are important, but they should sit on top of a working bidirectional checker.

## Open Questions

- What exact syntax should type constructors like `Array u8` use, and do they produce nominal types, anonymous types, or specialized declarations attached to a generic nominal family?
- How explicit should `anytype` specialization be in diagnostics and dumps?
- Should product literals without expected types be anonymous structural values, or should they require expected context except in limited cases?
- How much compile-time reflection should be supported before arbitrary comptime function execution?
- Should effect constraints be solved per function, per module, or globally with module-local batches?
