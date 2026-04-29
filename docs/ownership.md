# Compiler Ownership And Lifetimes

This compiler uses explicit ownership rules so compiler passes can share state without hidden pointer hazards.

## Compiler-Owned State

`Compiler` owns the cross-cutting state for one compilation:

- `pool`: interned strings. Store string IDs in long-lived structures, not raw `pool_get()` pointers.
- `arena`: long-lived compiler arena.
- `pass_arena`: reset at `compiler_begin_pass()`.
- `scratch_arena`: reset manually around short-lived helper work.
- `source_map`: file records and source text ownership.
- `diags`: append-only diagnostics for the compilation.
- `modules`, `module_stack`, and `next_file_id`: import/module state.

## Long-Lived Arena

Allocate from `compiler.arena` when the value must survive multiple passes:

- AST nodes and AST-owned child vectors.
- Module records and module export tables.
- Scopes and declarations.
- Diagnostics and source map records.
- Semantic facts, types, and effect signatures.

The arena is chunked, so `arena_alloc()` growth does not move earlier allocations.

## Temporary Arenas

Use `compiler.pass_arena` for values that only need to live during one compiler pass:

- Lexer token vectors.
- Layout normalizer output and frame vectors.
- Resolver pass-local helper vectors such as `with_imports`.

Use `compiler.scratch_arena` for helper values that are discarded within a pass:

- Import path candidate strings before canonicalization.
- Dump/stat temporary vectors.

`arena_reset()` keeps the first chunk, frees overflow chunks, and reuses the first chunk from offset zero.

## Heap-Owned State

Use heap allocation when external APIs or explicit destruction make ownership clearer:

- Source buffers returned by `ore_read_file_to_string()` are heap-owned until transferred to `SourceMap`.
- `SourceMap` frees source buffers with `sourcemap_free_sources()`.
- Canonical paths returned by `realpath()` are heap-owned and must be freed after interning or use.
- Malloc-backed `Vec` values must be released with `vec_free()` and `free()` where applicable.

## Borrowed Pointers

`pool_get()` returns a borrowed pointer. Store the string ID for long-lived references. A later `pool_intern()` may grow the pool and invalidate raw pointers returned by earlier `pool_get()` calls.

`vec_get()` returns a borrowed element pointer. Do not keep it across `vec_push()` on the same vector, because vector growth can move the backing storage.

## Current Audit Notes

- Existing `pool_get()` uses are immediate formatting, comparison, keyword lookup, or short helper inputs. They are not stored in long-lived structures.
- Existing retained compiler structures store interned string IDs rather than raw string pointers.
- Source buffers intentionally remain heap-owned because diagnostics need source text until rendering/free.