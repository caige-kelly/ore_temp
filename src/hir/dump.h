// HIR dumping — `--dump-hir` output.
//
// Structured pretty-printer over the HirModule/HirFn/HirInstr tree.
// Mirrors `dump_resolution` / `dump_sema`'s shape so the existing
// dump-flag idiom is consistent.
//
// Phase C1.2: prints one block per function — name, signature line,
// then the body's instructions. Per-instr formatting uses
// `hir_kind_str` plus kind-specific detail (decl name, literal value,
// type display). Type info comes from each HirInstr's own `type`
// field, populated during lowering.

#ifndef ORE_HIR_DUMP_H
#define ORE_HIR_DUMP_H

struct Sema;

void dump_hir(struct Sema* sema);

#endif // ORE_HIR_DUMP_H
