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
#include "ids/ids.h"
#include "scope/scope.h"
#include "query/query.h"
#include "type/target.h"
#include "type/type.h"
#include "type/effects.h"
#include "type/decls.h"
#include "type/const_eval.h"

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
    struct Compiler* compiler;
    Arena* arena;
    StringPool* pool;
    struct DiagBag* diags;
    Vec* bodies;               // Vec of CheckedBody*
    struct CheckedBody* current_body;
    Vec* instantiations;       // Vec of Instantiation* (insertion order, for iteration)
    HashMap instantiation_buckets; // Decl* (uint64_t) -> Vec<Instantiation*>* (per-decl bucket)
    HashMap decl_info;         // Decl* (uint64_t) -> SemaDeclInfo*: per-Decl sema cache
    struct ComptimeEnv* current_env;
    struct EvidenceVector* current_evidence; // active handler stack during checker walk
    HashMap effect_sig_cache;  // Expr* (uint64_t) -> EffectSig* — interning by source annotation
    Vec* query_stack;          // Vec of QueryFrame for cycle/debug context
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
    struct Type* anytype_type;
    struct Type* module_type;
    struct Type* effect_type;
    struct Type* effect_row_type;
    struct Type* scope_token_type;

    // Pre-interned name IDs for keyword-like names compared in hot paths
    // (sema_infer_expr's expr_Builtin switch, const_eval's eval_builtin
    // and target-field chain). Each one removes a per-call
    // strcmp(pool_get(id), "name") via `sema_name_is`. Compute once at
    // sema_new; compare uint32_t ids thereafter.
    uint32_t name_import;
    uint32_t name_sizeOf;
    uint32_t name_alignOf;
    uint32_t name_intCast;
    uint32_t name_TypeOf;
    uint32_t name_target;
    uint32_t name_true;
    uint32_t name_false;
    uint32_t name_returnType;

    // string_id (uint64_t) -> struct Type* for primitive names
    // (i32, bool, void, comptime_int, ...). Replaces a 22-arm strcmp
    // chain in `sema_primitive_type_for_name` with one hashmap lookup.
    HashMap primitive_types;

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
    // prelude_module: synthetic module (ModuleId{1}) holding builtin
    // primitive types. Every user module's internal_scope parents to
    // its export_scope so primitive names resolve without per-module
    // boilerplate. Populated by prelude_init() during sema_new.
    HashMap module_by_path;
    ModuleId prelude_module;

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

    // Layer 4 — scope index + resolution caches.
    //
    // node_to_scope: NodeId.id -> ScopeId.idx packed. Module-level
    // nodes (the top-level Bind nodes, type annotations sitting at
    // module scope) record here. Nodes inside a fn body live in
    // that fn's per-fn `node_to_scope` (in fn_scope_index_cache).
    //
    // resolved_refs: NodeId.id -> DefId.idx packed. Memoizes
    // query_resolve_ref so each Ident is resolved at most once.
    //
    // effect_ops_cache: DefId.idx -> Vec<DefId>* of ops visible
    // inside that function decl's body via its `<E>` annotation.
    HashMap node_to_scope;
    HashMap resolved_refs;
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
};

struct Sema sema_new(struct Compiler* compiler);
bool sema_check(struct Sema* sema);
// Assemble per-module HirModule wrappers + per-instantiation HirFns
// from the HirInstrs sema produced during the checker walk. Stores
// results in `sema->module_hir` (Module* -> HirModule*) and
// `sema->decl_hir` (Decl* -> HirFn*). Must be called after a
// successful `sema_check`. Idempotent — safe to call once.
void sema_lower_modules(struct Sema* sema);
struct Type* sema_type_of(struct Sema* sema, struct Expr* expr);
SemanticKind sema_semantic_of(struct Sema* sema, struct Expr* expr);
uint32_t sema_region_of(struct Sema* sema, struct Expr* expr);
struct EffectSig* sema_effect_sig_of(struct Sema* sema, struct Expr* expr);
void dump_sema(struct Sema* sema);
void dump_sema_effects(struct Sema* sema);
void dump_sema_evidence(struct Sema* sema);
void dump_tyck(struct Sema* sema);
void sema_record_call_value(struct Sema* s, struct Expr* call_expr, struct ConstValue v);

#endif // SEMA_H
