# Ore

## Introduction

Ore is a systems programming language designed for ultimate modularity. By combining Algebraic Effects (Koka-style) with Compile-time Metaprogramming, Ore allows you to define not just how your code runs, but how it interacts with the system at a fundamental level.

## Key Features

Algebraic Effects: Replace brittle error handling and global state with scoped, type-safe handlers.

Comptime Power: A full execution engine at compile time for DSLs and code generation.

Zero-Dependency Core: The compiler provides the primitives; the community provides the structures.

## Code at a glance

```
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
