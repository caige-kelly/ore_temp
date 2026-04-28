Yes — you have enough. Here's the build order with a validation gate after each step:

Step 1: collect_decl (top-level only).
Walk top-level expressions. For each Bind, allocate a Decl and push into module scope. Same-scope duplicate check.

Validate: dump the module scope. Should see one decl per top-level definition in malloc.ore (Page, Header, validate_heap, alloc, etc.) plus the primitives.
Stop and fix if any decl is missing or any duplicate slips through.
Step 2: Identifier resolution in pass 2 (no scope traversal yet).
Walk every expression. For each Identifier (not Field, not EnumRef), call scope_lookup against r->root only. Set ident->resolved.

Validate: dump_resolution shows each identifier's resolution. Top-level references work (u8 in a struct field type, Header in Page->next's type). Function-internal references will fail — that's fine for now.
Stop and fix if known-good top-level references show as unresolved.
Step 3: Function scopes — params + body.
When resolve_expr encounters a Bind whose value is a Lambda, allocate a SCOPE_FUNCTION, add params as DECL_PARAM, push the scope, walk the body, pop the scope. Walk effect annotations and return type before pushing (they resolve in the enclosing scope).

Validate: references to params inside function bodies now resolve. References to other top-level decls still resolve (parent walk works). The first real test: malloc.ore's validate_page should have every identifier resolved.
Step 4: Block scopes for If, Loop, Switch arms, With body.
Each pushes a SCOPE_BLOCK (or SCOPE_LOOP for loop bodies). Locals declared inside don't leak.

Validate: a := inside an if body is unreachable from outside. Test by dumping scopes — a local in an if body should be in a nested scope, not the function scope.
Step 5: Capture bindings.
if (x) |val|, loop x |val| — push a fresh SCOPE_BLOCK for the body, declare val as a DECL_USER in it, then walk the body.

Validate: val resolves inside the then-branch / loop body. After the branch, val is not in scope.
Step 6: Struct/Enum/Effect bodies.
When Bind's value is Struct/Enum/Effect, allocate the matching child_scope on the decl, walk fields/variants/operations as DECL_FIELD. Field types resolve against the enclosing scope (so ^Page finds Page at module level).

Validate: Page :: struct { next: ?^Page } — the Page in the field type resolves to the same decl that owns the struct (self-reference works).
Step 7: Comptime guard.
Track depth via r->comptime_depth or a SCOPE_COMPTIME push. When resolving a Call whose callee resolves to a function with effect annotations, error if depth > 0.

Validate: artificially write comptime alloc(u8, 10) somewhere; resolution errors. Remove comptime; resolution succeeds.
Step 8: dump_resolution polish + unresolved references list.
Once steps 1–7 pass, make the dump self-validating: the "unresolved references" section must be empty for malloc.ore and test.ore.

Validate: full run, zero unresolved.
Step 9: Negative test.
Copy test.ore to test_typo.ore, change one identifier (Vec3 → Vec4). Confirm:

resolver.has_errors == true
The error message names Vec4 and gives the correct line.
Other parts of the file still resolve (the error doesn't cascade).
Phase A done. At this point malloc.ore and test.ore both resolve cleanly. Phase B (modules) starts on a known-good foundation.

Each step is small and has a clear pass/fail signal. If you build them in order and gate on validation, you'll catch regressions immediately and the bug surface stays bounded. The riskiest step is #6 (struct self-reference) because it stresses the two-pass design — that's why it's late in the order, after simpler cases are working.

Start with step 1