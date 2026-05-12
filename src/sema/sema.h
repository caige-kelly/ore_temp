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
#include "intern_pool/intern_pool.h"

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

    // R4 — unified type+value intern pool. The single source of
    // truth for type identity post-cleanup. Every `struct Type *`
    // produced by `type_ptr` / `type_slice` / `type_struct` / etc.
    // carries a valid `ip` pointing into this pool.
    InternPool intern_pool;

    // R4 — reverse lookup IpIndex → struct Type*. Indexed by
    // IpIndex.v; sparse (entries for non-Type IpIndices stay NULL).
    // type_of_ip / ip_of_type are the bridge helpers.
    //
    // (Pre-cleanup this lived alongside eight per-kind HashMap
    // bucket interners — fn_types, ptr_types, many_ptr_types,
    // slice_types, array_types, optional_types, struct_types,
    // enum_types — that were used as fallback during the
    // incremental Steps 3a-g migration. Cleanup PR deleted them
    // all once every type kind was routed through the pool.)
    Vec* types_by_ip;

    // Per-Expression type cache (Stage E.2). NodeId.id → struct
    // TypeOfExprEntry* (defined in type/expr_check.h). Each entry
    // owns a query slot so query_type_of_expr participates in the
    // standard cycle-detection / dep-tracking / fingerprint
    // machinery.
    HashMap type_of_expr_entries;

    // === Per-kind decl detail tables (Stage E.2+) ===
    //
    // DefInfo is the *thin identity* for any decl — kind, name,
    // scope position. Per-kind details (type annotations, parameter
    // info, field defaults, fn signatures, ...) live in side tables
    // keyed by DefId. Mirrors rust-analyzer's per-kind data queries
    // (`function_data`, `struct_data`, ...). Adding a new field to
    // a per-kind data struct doesn't bloat DefInfo for unrelated
    // kinds, and the population path is local to whichever query
    // produces that data. AST-derived data (span, vis, origin Expr*,
    // semantic_kind) is re-derived on demand via the per-def
    // accessors in scope.h — DefInfo holds no AST pointers.
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

    // Idempotent allocators for nominal-member DefIds. Keyed by
    // (parent.idx << 32) | local_index — same shape as rust-analyzer's
    // `FieldId { parent, local_id }`. The same (parent, index) always
    // returns the same DefId, so re-running a signature query produces
    // stable DefIds for unchanged field/variant positions. Re-shape
    // edits (insert in middle, reorder) shift indices and thus shift
    // DefIds for affected positions — same identity model as
    // rust-analyzer's salsa-tracked `VariantFields::Arena<FieldData>`.
    // Values are `DefId.idx` packed into the `void*` slot.
    HashMap struct_field_defs;   // (parent.idx << 32) | index → DefId.idx
    HashMap enum_variant_defs;   // (parent.idx << 32) | index → DefId.idx

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
    // (R8: removed `node_to_expr` HashMap. Position queries now walk
    // the AST directly via find_expr_at_position in index/position.c
    // — returns Expr* without an indirection. def_origin uses
    // AstIdMap for top-level defs and DefInfo.origin_expr_id +
    // id_to_expr for local defs.)

    // span_index_by_module: ModuleId.idx -> Vec<SpanIndexEntry>*
    // — per-module sorted span index for O(log N) position
    // lookup. Built lazily by query_node_at_position. Rebuilt on
    // AST re-parse via the invalidation walker (Layer 7.5).
    //
    // (Previously: a refs_to_def HashMap maintained as a side
    // effect of query_resolve_ref. Removed in favor of RA-style
    // scan-on-demand — see src/sema/index/refs.h. The maintained
    // index couldn't handle source-delete edits without slot
    // eviction support, and find-references is human-paced so a
    // scan is fine.)
    HashMap span_index_by_module;

    // Layer 7.7 — request scope.
    //
    // active_cancel: pointer to the cancellation token for the
    // currently-running request, or NULL when no cancellable
    // request is active. Every sema_query_begin checks this and
    // returns QUERY_BEGIN_CANCELED if the flag has been set.
    // Today the LSP runs requests synchronously so this stays
    // NULL — see request/cancel.h.
    //
    // request_revision: pinned revision for the active request.
    // Zero means "use current_revision." See request/snapshot.h.
    struct CancelToken *active_cancel;
    uint64_t request_revision;

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

    // R8 — per-decl body stores. DefId.idx (uint64_t) -> struct
    // BodyStore*. Each entry assigns stable ExprId identities to
    // every body-level Expr reachable from the decl's body root.
    // Populated lazily by query_body_store; see
    // sema/body/body_store.h.
    HashMap body_stores;
};

// Lifecycle. Sema owns its arenas, string pool, diagnostics bag,
// and source map. `sema_init` brings up the entire database; the
// LSP shell and the CLI driver both call it.
void sema_init(struct Sema* s);
void sema_free(struct Sema* s);

#endif // SEMA_H
