#ifndef ORE_SEMA_SCOPE_H
#define ORE_SEMA_SCOPE_H

#include <stdbool.h>
#include <stdint.h>

#include "../../../support/common/hashmap.h"
#include "../../../support/common/vec.h"
#include "../../../parser/ast.h"
#include "../../../db/ids/ids.h"
#include "../../../support/common/stringpool.h"

// Scopes and definitions — the data shapes the rest of sema operates on.
//
// Two principles:
//   1. Every entity is named by an opaque ID (DefId, ScopeId), not a
//      pointer. Pointers move with arena resets and don't serialize;
//      IDs are stable for a Sema's lifetime and are the future
//      incremental key.
//   2. The shapes here are pure data. The algorithms (build the
//      DefMap, walk the parent chain, do a path lookup) live in their
//      respective layers — modules/, resolve/. This keeps scope.h a
//      narrow definition surface that every other layer can include
//      without dragging in algorithm code.
//
// Names are interned via the StringPool elsewhere in the compiler;
// `name_id` is the pool's u32 handle. Within a single ScopeInfo,
// names are unique (the resolver emits a duplicate-decl diagnostic on
// collision); across ScopeInfos, a name may appear many times.

struct Sema;
struct Expr;

// === ScopeKind ===
//
// Every scope has a kind that affects shadowing and lookup rules.
// Module scopes allow forward references to all decls (mutual
// recursion); block scopes only post-declaration. Effect / Struct /
// Enum scopes hold member decls for path resolution. Handler scopes
// hold operation-impl decls. Loop scopes are break/continue targets.
typedef enum {
    SCOPE_MODULE,
    SCOPE_FUNCTION,
    SCOPE_BLOCK,
    SCOPE_STRUCT,
    SCOPE_ENUM,
    SCOPE_EFFECT,
    SCOPE_HANDLER,
    SCOPE_LOOP,
    SCOPE_COMPTIME,
    SCOPE_PRIMITIVES,    // synthetic root holding compiler-built-in primitives
} ScopeKind;

// === DeclKind ===
//
// PRIMITIVE/USER are the common cases. IMPORT names a module brought
// in by @import. PARAM/FIELD are syntactic positions. SCOPE_PARAM is
// the comptime effect-scope token (e.g. `<s>`); EFFECT_ROW is an
// effect-row variable in `<|e>` annotations. LOOP_LABEL anchors
// break/continue.
typedef enum {
    DECL_PRIMITIVE,
    DECL_USER,
    DECL_IMPORT,
    DECL_PARAM,
    DECL_FIELD,
    DECL_VARIANT,            // enum variant (Stage E.3)
    DECL_SCOPE_PARAM,
    DECL_EFFECT_ROW,
    DECL_LOOP_LABEL,
} DeclKind;

// === SemanticKind ===
//
// The "what is this referring to" classifier used at lookup sites.
// Distinct from DeclKind because a single DeclKind (DECL_USER) can
// be a value, a type, an effect, etc.
typedef enum {
    SEM_UNKNOWN,
    SEM_VALUE,
    SEM_TYPE,
    SEM_EFFECT,
    SEM_MODULE,
    SEM_SCOPE_TOKEN,
    SEM_EFFECT_ROW,
} SemanticKind;

// === Namespace ===
//
// Resolution dispatches per namespace so a value and a type with the
// same name don't collide. The mapping from SemanticKind → Namespace
// is many-to-one: SEM_VALUE → NS_VALUE, SEM_TYPE → NS_TYPE,
// SEM_EFFECT → NS_EFFECT, SEM_SCOPE_TOKEN/SEM_EFFECT_ROW → NS_OP
// (since they only appear in op/effect-row positions).
typedef enum {
    NS_VALUE,
    NS_TYPE,
    NS_EFFECT,
    NS_OP,
    // Conjunctive "either value or type is acceptable here" — used by
    // expression-position references that can legitimately resolve to
    // a type (e.g. `Point{ ... }` struct construction, `@sizeOf(T)`,
    // `@TypeOf(x)`). Pre-B6 these sites called query_resolve_ref twice
    // (NS_VALUE then NS_TYPE on miss), generating 2× slots, 2× GUARD
    // evaluations, and 2× record_dep_on_parent calls per Ident. With
    // NS_VALUE_OR_TYPE the resolver does the prefer-value-fall-back-
    // to-type logic inside a single slot.
    NS_VALUE_OR_TYPE,
} Namespace;

// === DefInfo ===
//
// One per declaration anywhere in the program. Lives in Sema's `defs`
// table at index `DefId.idx`. Allocated and populated by the layer
// that introduced the decl (def_map.c for top-level, scope_index.c
// for locals, primitives.c for builtins).
//
// `origin_id` is the introducing AST node's NodeId, used by
// `def_origin` for local DECL_USER lookups (top-level lookups go
// through the module's top-level index keyed by name). 0 for
// synthetic decls like primitives.
//
// `DefInfo` is the *thin identity* for a decl — kind, name, scope
// position. Per-kind details (parameter types, signatures, field
// defaults, ...) live in side tables on Sema, populated by per-kind
// queries. AST-derived data (span, visibility, semantic_kind, origin
// Expr*) is re-derived on demand via `def_span` / `def_visibility` /
// `def_semantic_kind` / `def_origin` — see scope.h's per-def
// accessor section. See `sema/type/decl_data.h` for the current
// data structs (FnSignature, ParamLocator) and the comment block in
// sema.h for the architectural intent.
//
// Migration status of "detail-shaped" fields:
//   - is_comptime, has_effects   → migrated to FnSignature
//   - type_ann (params)          → migrated to FnSignature.param_types
//                                   (accessed via ParamLocator)
//   - child_scope                → DELETED. Member lookup for nominal
//     types (struct/enum) goes through `struct_find_field_def` /
//     `enum_find_variant_def` against the signature query.
//   - span, vis, semantic_kind, origin → DELETED. AST-derived data is
//     re-derived on access via def_span / def_visibility /
//     def_semantic_kind / def_origin. These rebuild from the current
//     AST (via top-level index for top-level defs, via origin_id +
//     node_to_expr for local/nested defs). Storing them on DefInfo
//     was the source of the LSP "stale di->origin after didChange"
//     class of bug — identity records must not cache derived state.
//     Matches rust-analyzer's `FunctionLoc` / `StructLoc` pattern:
//     pure identity, all derived data lives in tracked queries.
//   - imported_module, scope_token_id → DELETED. Both were dead
//     state: no live code created a DECL_IMPORT def with a valid
//     imported_module, and no live code ever read scope_token_id.
//     When `@import` and scope-param features land, the rust-
//     analyzer-faithful home is a per-kind side table (e.g.
//     `Sema.import_data: DefId.idx → ImportData*` analogous to
//     `fn_signatures` / `struct_signatures`) — not a kind-specific
//     field on the generic identity record.
//
// `ast_id` is the stable per-module identity for top-level DECL_USER
// items (and any future module-scope kinds: DECL_IMPORT, DECL_EFFECT,
// ...). Computed as hash((kind, name)) at parse time and stored on
// the def at first allocation. `def_origin` uses ast_id to look up
// the current revision's Bind expr via the module's AstIdMap —
// stable across inserting/removing sibling decls because the hash
// doesn't depend on byte position. Zero for non-top-level defs
// (locals, fields, variants, params, primitives).
//
// `origin_id` is the AST NodeId. For local DECL_USER defs (let-binds
// inside fn bodies) it's the current parse's NodeId for the Bind
// expression — fresh per parse because scope_index_build_module
// recreates the local DefInfo on every revision. For top-level defs
// it's zero — ast_id is the proper handle.
struct DefInfo {
    DeclKind kind;
    StrId name_id;                // StringPool handle
    AstId ast_id;                 // stable identity for module-scope items
    struct NodeId origin_id;      // AST node id; {0} for top-level / synthetic
    ExprId origin_expr_id;        // R8 — stable ExprId for the origin Bind expr;
                                  // populated for local DECL_USER defs by
                                  // scope_index_build_module via expr_to_id.
                                  // Used by def_origin's local path. Zero
                                  // (EXPR_ID_NONE) for top-level defs (use
                                  // ast_id) and synthetic defs.
    ScopeId owner_scope;          // the scope this def lives in
};

// === ScopeInfo ===
//
// One per lexical scope. Stored in Sema's `scopes` table at
// ScopeId.idx. Every scope has a parent (SCOPE_ID_INVALID for the
// primitives module's scopes); user lookup chains bottom out at
// the primitives module's export_scope.
//
// `defs` is the ordered insertion list (used for stable iteration in
// dumps and member-walk paths like struct fields). `name_index` is a
// keyed lookup mirror for O(1) local resolve. `children` is the
// reverse — every scope this scope introduced — for tree walks. All
// three are arena-allocated.
struct ScopeInfo {
    ScopeKind kind;
    ScopeId parent;
    ModuleId owner_module;        // module this scope belongs to
    Vec *defs;                    // Vec<DefId>
    HashMap name_index;           // name_id (uint64_t) -> DefId encoded as ((uint64_t)id.idx)
    Vec *children;                // Vec<ScopeId>
};

// === Helpers ===
//
// Map a SemanticKind to its resolution namespace.
Namespace ns_for_semantic(SemanticKind sem);

// Return true if `vis` allows access to a def from outside its
// owner_module. Used by path resolution at module boundaries.
bool visibility_allows_external(Visibility vis);

// Allocate and register a new ScopeInfo of the given kind, parented
// under `parent` and owned by `owner_module`. Returns the assigned
// ScopeId. The new scope's `defs`, `name_index`, and `children`
// vectors are initialized; insertion is via scope_insert_def.
ScopeId scope_create(struct Sema *s, ScopeKind kind, ScopeId parent,
                     ModuleId owner_module);

// Allocate and register a new DefInfo. Caller passes a populated
// stack-built struct; the function arena-copies it, registers it, and
// returns the assigned DefId. The def is NOT yet inserted into any
// scope's name_index — call scope_insert_def for that. (Decoupled so
// builders can populate then insert as a single atomic step.)
DefId def_create(struct Sema *s, struct DefInfo proto);

// Define `def` at `scope` as its canonical home.
//
// Sets `def->owner_scope = scope`, inserts into the scope's defs vec
// and name_index. Returns false if the name already exists in this
// scope (caller owns diagnostic emission); on false return, the def
// is not added to either vec or index.
//
// Use this for the *first* (canonical) place a def lives. Public
// top-level binds use `scope_mirror_def` for their export-scope
// reflection.
bool scope_define_def(struct Sema *s, ScopeId scope, DefId def);

// Mirror `def` into `scope` so name lookups in this scope find it,
// without changing `def->owner_scope`. Used to project a public
// internal-scope def into the export scope so external path
// resolution (`module.name`) resolves cleanly.
//
// Returns false if the name already exists in this scope; otherwise
// true. The def is unchanged on either return.
bool scope_mirror_def(struct Sema *s, ScopeId scope, DefId def);

// Look up `name_id` in the immediate decls of `scope` only — no
// parent walk. Returns DEF_ID_INVALID on miss. The walking variant
// lives in the resolve layer.
DefId scope_lookup_local(struct Sema *s, ScopeId scope, StrId name_id);

// === Per-def AST-derived accessors ===
//
// These functions read AST-derived data from the CURRENT revision's
// AST rather than from a cached field on DefInfo. The DefInfo holds
// only pure identity (kind + name + scope position); span, visibility,
// and semantic_kind are properties of where the def's source sits in
// the current AST, so they're re-derived on every read.
//
// This matches rust-analyzer's pattern: `StructLoc` and `FunctionLoc`
// carry only identity; `Visibility`, source location, etc. are queries
// that read the AST through ast_id_map + parse_or_expand each call.
//
// For DECL_USER / DECL_IMPORT the lookup goes through
// query_top_level_index. For nested kinds (DECL_FIELD, DECL_VARIANT,
// DECL_PARAM), it dispatches to the relevant per-kind data
// (StructSignature / EnumSignature / FnSignature). For DECL_PRIMITIVE
// it returns kind-appropriate defaults.

// Visibility for a def. For DECL_USER / DECL_IMPORT reads the bind's
// `pub` modifier from the current AST. For DECL_FIELD reads the
// field's vis from the parent struct's signature. Variants and
// primitives are always Visibility_public; params and synthesized
// defs are Visibility_private.
Visibility def_visibility(struct Sema *s, DefId def);

// Source span for a def. For top-level binds the bind's name span;
// for fields/variants the declaration span recorded on the signature;
// for params the param ident's span; for primitives a zeroed span.
struct Span def_span(struct Sema *s, DefId def);

// Semantic kind classifies which namespace the def belongs to. For
// DECL_USER it's derived from the bind value's expression kind
// (struct/enum → SEM_TYPE, fn → SEM_VALUE, decl_Effect → SEM_EFFECT,
// otherwise SEM_VALUE). Kind-specific for the rest.
SemanticKind def_semantic_kind(struct Sema *s, DefId def);

#endif // ORE_SEMA_SCOPE_H
