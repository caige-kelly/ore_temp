#ifndef SEMA_H
#define SEMA_H

#include <stdbool.h>
#include <stdint.h>

#include "../common/arena.h"
#include "../common/hashmap.h"
#include "../common/stringpool.h"
#include "../common/vec.h"
#include "../parser/ast.h"
#include "../diag/diag.h"
#include "../diag/sourcemap.h"
#include "ids/ids.h"
#include "scope/scope.h"
#include "query/query.h"
#include "eval/const_eval.h"

struct Compiler;
struct Instantiation;
struct CancelToken;     // src/sema/request/cancel.h

struct ComptimeArgTuple {
    Vec* values;  // Vec of ConstValue
};

// A CheckedBody owns the HirInstrs derived from one type-checked unit:
//   - top-level expressions in a module
//   - a function/handler body (one body per Decl)
//   - a specialized function body (one body per Instantiation)
//
// HirInstrs are not global: "this Expr has this HirInstr" is only meaningful
// inside the checked body that produced it — the same generic Expr nodes
// get distinct HirInstrs in distinct instantiation walks. The module body
// acts as the catch-all for top-level expressions.
struct CheckedBody {
    struct Decl* decl;                  // owning decl, NULL for the module body
    struct Module* module;              // module this body lives in
    struct Instantiation* instantiation;// non-NULL when the body is a generic specialization
    struct EvidenceVector* entry_evidence; // evidence stack on body entry
    HashMap call_evidence;              // Expr* (uint64_t) -> EvidenceVector*: per-call snapshot
    // Per-body HIR map. Each Expr in this body's walk gets a HirInstr
    // stored here (allocated by body_record_hir). Per-body keying
    // handles per-instantiation correctly: the same generic Expr nodes
    // get different HirInstrs in different instantiation walks, each
    // in its own CheckedBody.
    HashMap expr_hir;                   // Expr* (uint64_t) -> HirInstr*
};

struct Sema {
    Arena arena;
    Arena pass_arena;          // lexer scratch space; reset per parse pass
    StringPool pool;
    struct DiagBag diags;
    struct SourceMap source_map;
    Vec* bodies;               // Vec of CheckedBody*
    struct CheckedBody* current_body;
    Vec* instantiations;       // Vec of Instantiation* (insertion order, for iteration)
    HashMap instantiation_buckets; // Decl* (uint64_t) -> Vec<Instantiation*>* (per-decl bucket)
    HashMap decl_info;         // Decl* (uint64_t) -> SemaDeclInfo*: per-Decl sema cache
    struct ComptimeEnv* current_env;
    struct EvidenceVector* current_evidence; // active handler stack during checker walk
    HashMap effect_sig_cache;  // Expr* (uint64_t) -> EffectSig* — interning by source annotation
    Vec* query_stack;          // Vec of QueryFrame for cycle/debug context
#ifdef ORE_DEBUG_QUERIES
    // Per-QueryKind telemetry. Indexed by `(int)kind`; sized by
    // QUERY_KIND_COUNT. Bumped throughout query.c / invalidate.c
    // and dumped by --dump-query-stats. See bug_of_bugs.md B14.
    struct QueryStats query_stats[QUERY_KIND_COUNT];
#endif
    int comptime_call_depth;   // guard against infinite comptime recursion
    HashMap call_cache;       // Decl* → Vec<ComptimeCallCacheEntry*>
    int64_t comptime_body_evals;   // instrumentation: how many times we've actually run a body
    bool has_errors;

    struct Type* unknown_type;
    struct Type* error_type;
    struct Type* void_type;
    struct Type* noreturn_type;
    struct Type* bool_type;
    struct Type* comptime_int_type;
    struct Type* comptime_float_type;
    struct Type* u8_type;
    // Cached `const u8` for hot paths — string indexing returns this since
    // strings are conceptually `[]const u8`. Built lazily during sema init.
    struct Type* const_u8_type;
    struct Type* u16_type;
    struct Type* u32_type;
    struct Type* u64_type;
    struct Type* i8_type;
    struct Type* i16_type;
    struct Type* i32_type;
    struct Type* i64_type;
    struct Type* usize_type;
    struct Type* isize_type;
    struct Type* f64_type;
    struct Type* f32_type;
    struct Type* string_type;
    struct Type* nil_type;
    struct Type* type_type;
    struct Type* module_type;

    // Pre-interned name IDs for builtin dispatch hot paths. Each one
    // turns `name_is(s, id, "sizeOf")` (pool_get + char-by-char compare)
    // into a single uint32_t equality. Initialized in sema_init.
    //
    // Only names with an active dispatch site are pre-interned. Earlier
    // versions had name_import / name_target / name_true / name_false /
    // name_returnType reserved for a future migration that never
    // materialized; they're removed until a real consumer appears.
    StrId name_sizeOf;
    StrId name_alignOf;
    StrId name_intCast;
    StrId name_TypeOf;
    StrId name_typeName;

    // string_id (uint64_t) -> struct Type* for primitive names
    // (i32, bool, void, comptime_int, ...). Replaces a 22-arm strcmp
    // chain in `sema_primitive_type_for_name` with one hashmap lookup.
    HashMap primitive_types;

    // Compound-type interners (Stage E.2). Each maps a content hash
    // → Vec<struct Type*>. Lookup walks the bucket and structural-
    // compares; collisions fall through to a fresh allocation.
    // Keep these in sync with src/sema/type/intern.c.
    HashMap fn_types;       // hash(params, ret)         → Vec<Type*>
    HashMap ptr_types;      // hash(elem, is_const)      → Vec<Type*>
    HashMap many_ptr_types; // hash(elem, is_const)      → Vec<Type*>
    HashMap slice_types;    // hash(elem, is_const)      → Vec<Type*>
    HashMap array_types;    // hash(elem, size)          → Vec<Type*>
    HashMap optional_types; // hash(elem)                → Vec<Type*>

    // Stage E.3 nominal interners. Keyed directly by DefId.idx — no
    // bucket/structural-eq dance needed since the type IS the def.
    HashMap struct_types; // DefId.idx                 → struct Type*
    HashMap enum_types;   // DefId.idx                 → struct Type*

    // Per-Expression type cache (Stage E.2). NodeId.id → struct
    // TypeOfExprEntry* (defined in type/expr_check.h). Each entry
    // owns a query slot so query_type_of_expr participates in the
    // standard cycle-detection / dep-tracking / fingerprint
    // machinery.
    HashMap type_of_expr_entries;

    // === Per-kind decl detail tables (Stage E.2+) ===
    //
    // DefInfo is the *thin identity* for any decl — kind, name, span,
    // scope position. Per-kind details (type annotations, parameter
    // info, field defaults, fn signatures, ...) live in side tables
    // keyed by DefId. Mirrors rust-analyzer's per-kind data queries
    // (`function_data`, `struct_data`, ...). Adding a new field to
    // a per-kind data struct doesn't bloat DefInfo for unrelated
    // kinds, and the population path is local to whichever query
    // produces that data.
    //
    // Today's per-kind data tables:
    //
    //   fn_signatures  — per-fn-DefId. Holds resolved param types,
    //                    param kinds, ret type, and modifiers.
    //                    Computed by query_fn_signature.
    //
    //   param_locators — per-param-DefId. Records (parent_fn, index)
    //                    so query_type_of_def(param) can index into
    //                    the parent fn's signature.
    //
    // Pre-existing detail fields still on DefInfo (`imported_module`,
    // `scope_token_id`) migrate to their own data structs when
    // naturally exercised by feature work.
    HashMap fn_signatures;       // DefId.idx → struct FnSignature*
    HashMap param_locators;      // DefId.idx → struct ParamLocator*

    // Stage E.3 — struct/enum side tables.
    //
    //   struct_signatures — per-struct-DefId. Holds the resolved
    //                        FieldData arena, with C-style anonymous
    //                        union arms flattened (union_group != 0).
    //                        Computed by query_struct_signature.
    //   field_locators    — per-field-DefId. Records (parent_struct,
    //                        index) so query_type_of_def(field) can
    //                        index into the parent's signature.
    //   enum_signatures   — per-enum-DefId. Holds VariantData arena
    //                        with const-evaluated values.
    //   variant_locators  — per-variant-DefId. Records (parent_enum,
    //                        index).
    HashMap struct_signatures;   // DefId.idx → struct StructSignature*
    HashMap field_locators;      // DefId.idx → struct FieldLocator*
    HashMap enum_signatures;     // DefId.idx → struct EnumSignature*
    HashMap variant_locators;    // DefId.idx → struct VariantLocator*

    // Module* (uint64_t) -> struct HirModule*. Populated by
    // `sema_lower_modules` after `sema_check` completes; consumers
    // (`--dump-hir`, future codegen, post-C4 effect solver) read from
    // here. NULL entries mean lowering hasn't run yet for that module.
    HashMap module_hir;

    // Decl* (uint64_t) -> struct HirFn*. Decl-level shortcut into the
    // module's HirModule, populated alongside module_hir. Used by
    // sema_body_effects_of and codegen to find the HirFn for a given
    // function decl in O(1) without scanning the module's function vec.
    HashMap decl_hir;

    // Layer 1 — stable opaque ID tables. Each holds void* pointers to
    // the layer-owned info struct (DefInfo lives in resolve/scope.h,
    // ModuleInfo in modules/modules.h, etc.). Slot 0 of each is a NULL
    // sentinel so the *_INVALID IDs dereference to NULL via the
    // accessors in ids/ids.h. Initialized by sema_ids_init().
    Vec* defs_table;
    Vec* scopes_table;
    Vec* modules_table;
    Vec* bodies_table;

    // Layer 2 — query engine revision counter. Bumps when an input
    // changes. Layer 7.5 (invalidation walker) compares
    // slot.verified_rev against this value to decide whether a
    // cached slot needs re-validation. See src/sema/query/query.h.
    uint64_t current_revision;

    // Layer 7.5 — invalidation gating.
    //
    // Off during the initial build (the first compilation pass before
    // any input has been mutated). Switches on the moment a
    // sema_set_input_source / sema_invalidate_input call bumps the
    // revision. Keeping it off during cold start avoids the per-query
    // revalidate overhead when there's nothing to invalidate.
    bool invalidation_enabled;

    // Layer 3 — module system.
    //
    // module_by_path: path_id (uint64_t) -> ModuleId.idx packed in a
    // void* slot. Lets query_module_for_path dedupe by canonical path.
    //
    // primitives_module: synthetic module (ModuleId{1}) holding builtin
    // primitive types. Every user module's internal_scope parents to
    // its export_scope so primitive names resolve without per-module
    // boilerplate. Populated by primitives_init() during sema_new.
    HashMap module_by_path;
    ModuleId primitives_module;

    // Layer 7.1 — inputs (source-text-as-input).
    //
    // inputs_table: InputId.idx -> struct InputInfo*. Slot 0 is a NULL
    // sentinel so INPUT_ID_INVALID dereferences cleanly via input_info.
    //
    // inputs_by_path: path_id (uint64_t) -> InputId.idx packed.
    // Idempotent registration — re-calling sema_register_input on the
    // same path returns the same InputId.
    //
    // The input layer is the bottom of the lazy query chain: source
    // text mutations bump current_revision, and downstream queries
    // (parse, def_map, resolve, typecheck) cascade-invalidate via
    // fingerprint comparison. See src/sema/modules/inputs.h.
    Vec *inputs_table;
    HashMap inputs_by_path;

    // Layer 4 — resolution caches.
    //
    // resolve_ref_entries: (NodeId<<4)|NS -> struct ResolveRefEntry*
    // (defined in resolve/resolve.h). Each entry owns a query slot
    // so resolve_ref participates in the standard cycle-detection /
    // dep-tracking / fingerprint machinery. Distinct entries per
    // (node, namespace) pair: the same Ident queried in NS_VALUE
    // vs NS_TYPE doesn't share cache state.
    //
    // resolve_path_entries: (root NodeId<<4)|NS -> struct
    // ResolvePathEntry*. Same shape as resolve_ref_entries but for
    // dotted paths. The root_node is the path's head AST node.
    //
    // effect_ops_cache: DefId.idx -> Vec<DefId>* of ops visible
    // inside that function decl's body via its `<E>` annotation.
    HashMap resolve_ref_entries;
    HashMap resolve_path_entries;
    HashMap effect_ops_cache;

    // Layer 7.4 — per-fn lazy scope index.
    //
    // node_to_decl: NodeId.id -> DefId.idx packed. Built lazily
    // per-module via query_node_to_decl_index — every NodeId in a
    // module's subtree maps to its enclosing top-level DefId.
    // The first stop in query_scope_for_node's lookup chain.
    //
    // fn_scope_index_cache: DefId.idx -> struct ScopeIndexResult*.
    // The per-fn scope construction result. Each fn's scopes,
    // local DefIds, and per-fn node_to_scope live here. Built
    // on-demand by query_fn_scope_index; a body-only edit
    // invalidates exactly this entry.
    HashMap node_to_decl;
    HashMap fn_scope_index_cache;

    // Layer 7.6 — reverse indices + position queries.
    //
    // node_to_expr: NodeId.id -> struct Expr* — the AST node for
    // each NodeId. Populated by decl_walk in scope_index.c (which
    // visits every reachable node). Used by query_def_at_position
    // to convert a positional NodeId back to an AST expression
    // for resolution.
    HashMap node_to_expr;

    // refs_to_def: DefId.idx -> Vec<NodeId>* — every NodeId that
    // resolved to this DefId via query_resolve_ref. Populated as
    // a side-effect of resolution; consumed by
    // query_references_of for LSP "find references."
    //
    // span_index_by_module: ModuleId.idx -> Vec<SpanIndexEntry>*
    // — per-module sorted span index for O(log N) position
    // lookup. Built lazily by query_node_at_position. Rebuilt on
    // AST re-parse via the invalidation walker (Layer 7.5).
    HashMap refs_to_def;
    HashMap span_index_by_module;

    // Layer 7.7 — request scope and resource control.
    //
    // active_cancel: pointer to the cancellation token for the
    // currently-running request, or NULL when no cancellable
    // request is active. Every sema_query_begin checks this and
    // returns QUERY_BEGIN_CANCELED if the flag has been set.
    //
    // request_revision: pinned revision for the active request.
    // Zero means "use current_revision." When a request handler
    // captures a snapshot, it stamps this so all queries during
    // the request see a consistent revision (LSP correctness
    // when edits arrive mid-request).
    //
    // slot_count / slot_budget: bounded-memory policy hooks. Each
    // QuerySlot creation increments slot_count. When the LRU
    // walker is wired (currently a stub), exceeding slot_budget
    // triggers eviction of the least-recently-accessed slots.
    struct CancelToken *active_cancel;
    uint64_t request_revision;
    size_t slot_count;
    size_t slot_budget;

    // Per-Expr const-eval entries. Keyed by NodeId.id; values are
    // struct ConstEvalEntry* (defined in eval/const_eval.h). The
    // entry owns its query slot — same lazy/cycle/invalidate pattern
    // as every other query.
    HashMap const_eval_entries;

    // Per-Expr "is this comptime-evaluable?" cache. Keyed by NodeId.id;
    // values are struct IsComptimeEntry*. Replaces an old recursive
    // walker that bypassed the dep graph (cleanup.md #3). Composed via
    // its own slot so editing a transitively-referenced const-bind
    // properly invalidates every dependent predicate result.
    HashMap is_comptime_entries;

    // Per-Type layout cache. Keyed by Type* (interned, so pointer
    // identity = type identity); values are struct LayoutEntry*. The
    // entry owns its query slot — by-value cycles like
    // `Bad :: struct { self: Bad }` are caught via the standard
    // RUNNING-state cycle handling and produce a precise diagnostic.
    // Used by query_layout_of_type (PR 3.5 R5) and consumed by
    // @sizeOf / @alignOf for aggregate operands.
    HashMap layout_of_type;
};

// Lifecycle. Sema owns its arenas, string pool, diagnostics bag,
// and source map. `sema_init` brings up the entire database; the
// LSP shell and the CLI driver both call it.
void sema_init(struct Sema* s);
void sema_free(struct Sema* s);

#endif // SEMA_H
