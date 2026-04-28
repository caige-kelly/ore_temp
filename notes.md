Yes — before deeper type/effect/comptime work, the best housekeeping is to centralize diagnostics, make arena ownership explicit, and add a small regression harness.

Recommended housekeeping before harder compiler work:

1. Use one unified diagnostics module
Do this. In C, each pass can still create pass-specific errors, but they should all flow into one central diagnostics API and one renderer.

Best shape:

SourceMap: owns file paths, source text, line-start tables, file IDs.
Span: should carry file_id plus byte/line/column info.
Diag: severity, message, code, primary span, secondary labels, notes, suggestions.
DiagBag: append-only collection owned by the compiler context.
diag_render(): the only place that pretty-prints rustc-style errors.
Then parser/name resolution/sema should not print directly. They should call helpers like diag_error(...), and main.c should render all diagnostics once.

For this codebase, that means eventually replacing/pass-through wrapping:

struct ResolveError in name_resolution.h
struct SemaError in sema.h
ad-hoc fprintf(stderr, ...) in main.c
Recommendation: centralize the storage and rendering, but let each pass keep small local helper functions like resolver_error() and sema_error() that build diagnostics with pass-specific wording.

2. Add a real Compiler context
Right now, objects are passed separately: StringPool, Arena, resolver, sema, etc. That is okay early, but it will get messy.

Add a central context later:

Compiler
StringPool*
SourceMap*
DiagBag*
long-lived arenas
options/flags
module cache
target/build config later
Then passes take either Compiler* or a pass context containing it. This makes imports, diagnostics, source lookup, and build-runner integration much cleaner.

3. Do not keep one giant arena forever
One giant arena is fine for prototyping, but not ideal long-term.

Better arena layout:

Long-lived arena:

AST
module table
declarations/scopes
stable semantic facts
types/HIR eventually
Source arena or heap-owned source files:

source text
paths
line tables
Diagnostics arena:

diagnostic messages, labels, notes
Scratch/temp arena:

reset after each file/pass/function
effect constraint temp structures
parser/layout temporary buffers if possible
Optional pass arenas:

name-resolution temp arena
type-check/effect-solving arena
comptime-eval arena
Important C-specific point: your current Arena uses realloc growth, which can invalidate every pointer already returned by the arena. That is dangerous for a compiler. Either keep overallocating for now, or soon replace it with a chunked arena where old chunks never move.

Best next arena improvement: implement chunked Arena blocks or arena_mark() / arena_reset_to(mark) for scratch usage.

4. Add ownership conventions
C needs explicit ownership rules because the language will not enforce them.

Document conventions like:

AST nodes live for the whole compilation.
Decl, Scope, Module, Type, SemaFact, and EffectSig are arena-owned.
Source buffers live at least as long as diagnostics can render.
char* returned by path helpers is heap-owned and must be freed.
Vec* created with vec_new_in() is arena-owned.
Vec created with vec_init() must be vec_free()d.
This will prevent subtle leaks and use-after-free bugs once comptime/build runner work starts.

5. Add a small regression test harness
Before effect solving/comptime, add a simple shell or C test runner:

positive examples must exit 0
negative examples must exit nonzero
optionally grep expected diagnostic text
maybe store expected diagnostics in comments later
This should cover imports, scoped effects, missing fields, import cycles, sema skeleton, and old examples.

This will save a lot of time once the compiler gets complicated.

6. Add CLI dump flags
Right now the compiler dumps a lot unconditionally. Before the project grows, make dumps explicit:

--dump-ast
--dump-resolve
--dump-sema
--dump-effects
--no-color
maybe --quiet
This matters because diagnostics should become the default user-facing output, not all internal dumps.

7. Improve source/file tracking now
This is tightly connected to diagnostics.

Currently errors are mostly line/column only. For imports and build systems, every span needs a file ID. Otherwise diagnostics across modules become confusing.

Priority:

Add file_id to Span if not already reliable everywhere.
Add SourceMap.
Make lexer assign file IDs from SourceMap.
Render diagnostics as path:line:col.
8. Use hash maps soon, but not immediately
Scope.decls is currently a linear Vec. That is fine for tiny examples, but name resolution will get slow and duplicate handling gets awkward.

Soon-ish:

interned string ID → Decl* map for each scope
path ID → Module* map for module cache
Expr* → SemaFact* map for sema queries
For now, linear lookup is okay. The API boundary matters more than the implementation.

Recommended order
I would do housekeeping in this order:

Central diagnostics + SourceMap
CLI dump flags
Test harness
Chunked arena or scratch arena marks
Compiler context
Hash maps behind existing lookup APIs
If you want the most useful next implementation, start with diagnostics + source map. That unlocks reliable imports, better negative tests, effect/type errors, and eventually build-runner messages