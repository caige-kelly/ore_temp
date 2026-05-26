# Ore

## Introduction

Ore is a procedural systems programming language with effects and compile time metaprogramming

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
