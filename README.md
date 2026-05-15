# Ore

## Introduction

Ore is a systems programming language designed for ultimate modularity. By combining Algebraic Effects (Koka-style) with Compile-time Metaprogramming, Ore allows you to define not just how your code runs, but how it interacts with the system at a fundamental level.

## Key Features

Algebraic Effects: Replace brittle error handling and global state with scoped, type-safe handlers.

Comptime Power: A full execution engine at compile time for DSLs and code generation.

Zero-Dependency Core: The compiler provides the primitives; the community provides the structures.

## Code at a glance

```rust
allocator :: @import("allocator.ore")
a         :: allocator.Allocator

main :: fn(void) i32
    comptime if (@build.mode == .debug)
        with @build.handlers.exn
        with @build.handlers.allocator

    r1 := alloc(u8, 1024)
    defer free(r1)

    0
```

## Build System Example

Ore has utilizing a build system written in Ore.

```rust
b :: @import("./build.ore")

build :: pub fn(void) void
    exe := b.add_executable(.{
        .name = "allocator_test", 
        .root = b.root(.{
            .source_file = b.path("main.ore"),
            .target = .APPLESILICON,
        })
        .handlers = b.default_handlers(.{
            .{ .name = "allocator", .fn = @import("allocator.ore").debug_allocator }
            .{ .name = "exn", .fn = @import("exn.ore").default_exn }
        })
    })

    builder.install_artifact(exe)
```

## TODO

Minor observations / not bugs
2. Comments interned. lex_line_comment and lex_block_comment call emit_interned. Comment text rarely repeats across files, so the pool just grows for no dedup benefit. Could be emit_plain — consumers (formatter, doc-on-hover) get text via source[start..byte_end] walk. Marginal optimization; can defer.

3. Numeric literals interned. Same shape as comments — 123 lexed, interned, then parser converts to value. Could just convert from source[start..byte_end]. But numerics dedup more (e.g. 0, 1, 100 are common), and pool entries are short. Less clear-cut than comments. Probably keep.

4. isdigit / isxdigit from ctype.h are locale-dependent. For C source files we want strictly ASCII. Best practice is inline ASCII range checks. In practice almost always runs in C locale, so latent. Worth flagging, not urgent.

5. lex_error is a no-op stub. Comments say "TODO(diag): wire up against the new diag subsystem." TK_ERROR tokens carry byte ranges but no diagnostic message attached anywhere. When the diag system is wired into lex (presumably via the query body that invokes lex), the messages connect. Until then, error positions are visible but messages are lost.

6. Malformed numeric edge cases. 0x with no hex digits emits a TK_INT_LIT covering just "0x". The conversion would fail downstream. Could lex_error on empty digit sequence, but it's a minor recovery decision.


