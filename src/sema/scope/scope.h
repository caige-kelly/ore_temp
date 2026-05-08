#ifndef ORE_SEMA_SCOPE_H
#define ORE_SEMA_SCOPE_H

#include <stdbool.h>
#include <stdint.h>

#include "../../common/hashmap.h"
#include "../../common/vec.h"
#include "../../parser/ast.h"
#include "../ids/ids.h"

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
    SCOPE_PRELUDE,    // synthetic root holding builtins
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
} Namespace;

// === DefInfo ===
//
// One per declaration anywhere in the program. Lives in Sema's `defs`
// table at index `DefId.idx`. Allocated and populated by the layer
// that introduced the decl (def_map.c for top-level, scope_index.c
// for locals, prelude.c for builtins).
//
// `origin` is the AST node that introduced this decl, borrowed from
// the parser arena (which outlives Sema). NULL for synthetic decls
// like primitives. `origin_id` is the same node's stable NodeId, used
// when the def is referenced from caches keyed by NodeId.
struct DefInfo {
    DeclKind kind;
    SemanticKind semantic_kind;
    uint32_t name_id;             // StringPool handle
    struct Span span;             // canonical span for diagnostics
    struct NodeId origin_id;      // AST node id; {0} for synthetic
    struct Expr *origin;          // borrowed from parser arena; NULL ok
    ScopeId owner_scope;          // the scope this def lives in
    ScopeId child_scope;          // scope this def introduces (fn body,
                                  // type members, module exports);
                                  // SCOPE_ID_INVALID otherwise
    ModuleId imported_module;     // for DECL_IMPORT, points to the
                                  // imported module; INVALID otherwise
    Visibility vis;
    uint32_t scope_token_id;      // DECL_SCOPE_PARAM only; 0 otherwise
    bool is_comptime;
    bool has_effects;             // function carries `<E>` annotation
};

// === ScopeInfo ===
//
// One per lexical scope. Stored in Sema's `scopes` table at
// ScopeId.idx. Every scope has a parent (SCOPE_ID_INVALID for the
// prelude); the chain bottoms out at the prelude.
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

// Insert `def` into `scope`'s defs vec and name_index. Returns false
// if the name already exists in this scope; the caller owns
// diagnostic emission. On false return, the def is not added to either
// vec or index.
bool scope_insert_def(struct Sema *s, ScopeId scope, DefId def);

// Look up `name_id` in the immediate decls of `scope` only — no
// parent walk. Returns DEF_ID_INVALID on miss. The walking variant
// lives in the resolve layer.
DefId scope_lookup_local(struct Sema *s, ScopeId scope, uint32_t name_id);

#endif // ORE_SEMA_SCOPE_H
