#ifndef ORE_DB_IDS_H
#define ORE_DB_IDS_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

/*
    Typed identity handles.

    Every long-lived entity in db/ — sources, modules, defs, types, scopes,
    effects, handlers — is keyed by a u32 wrapped in a one-field struct. The
    wrapping forces the compiler to reject passing a DefId where a ModuleId
    is expected — that type safety is the load-bearing invariant the SoA
    columns in struct db rely on.

    Slot 0 of every id-column is reserved as the NONE sentinel. Valid ids
    start at 1, so "is this id present?" is `id.idx != 0`.

    Conventions:
    - Simple handles use `.idx`. Always.
    - Composite/packed keys (e.g. GlobalNodeId joining ModuleId+AstNodeId)
        use `.raw` for the packed integer and provide inline accessors for
        the fields they carry.
    - Sentinels are #define'd to compound literals so they can appear in
        constant-expression contexts (static initializers, etc.). The named
        convention (TYPE_NONE) is the type discipline; the compiler enforces
        it at use site via the wrapped struct type.
*/


// Def kind enumeration. Stored in defs.kinds; one DefKind per DefId.

typedef enum : uint8_t {
    KIND_NONE = 0,
    KIND_FUNCTION,
    KIND_STRUCT,
    KIND_UNION,
    KIND_ENUM,
    KIND_EFFECT,
    KIND_HANDLER,
    KIND_VARIABLE,
    KIND_CONSTANT,
} DefKind;

// Primary keys — typed handles into the database's SoA columns.

typedef struct { uint32_t idx; } SourceId;
typedef struct { uint32_t idx; } ModuleId;
typedef struct { uint32_t idx; } DefId;
typedef struct { uint32_t idx; } TypeId;
typedef struct { uint32_t idx; } ScopeId;
typedef struct { uint32_t idx; } FileId;
typedef struct { uint32_t idx; } TargetId;
typedef struct { uint32_t idx; } ValueId;
typedef struct { uint32_t idx; } EffectId;        // The 'ability'
typedef struct { uint32_t idx; } HandlerId;       // The implementation
typedef struct { uint32_t idx; } StrId;           // Entry in the StringPool

// Position-based handle into a module's ASTStore arrays. Stable only within
// a single parse — every reparse may assign a different AstNodeId to the
// same logical syntax. For stable cross-reparse identity, use AstId below.
typedef struct { uint32_t idx; } AstNodeId;

// Strongly-typed handle into the AST extra_data side-bucket.
typedef struct { uint32_t idx; } AstExtraDataIdx;

// Reparse-stable identity for a top-level AST item. Computed from
// (item kind, name) so adding a sibling decl earlier in the file does NOT
// shift other items' AstIds — that's the property AstNodeId can't promise.
// Load-bearing for LSP perf: without it, every reparse churns every
// top-level decl's query slot.
typedef struct { uint32_t idx; } AstId;

// Pin the wire size. Any accidental field addition triggers a compile
// error rather than silently breaking column-parallel layouts.
static_assert(sizeof(SourceId)        == 4, "id sizes must stay u32");
static_assert(sizeof(ModuleId)        == 4, "id sizes must stay u32");
static_assert(sizeof(DefId)           == 4, "id sizes must stay u32");
static_assert(sizeof(TypeId)          == 4, "id sizes must stay u32");
static_assert(sizeof(ScopeId)         == 4, "id sizes must stay u32");
static_assert(sizeof(FileId)          == 4, "id sizes must stay u32");
static_assert(sizeof(TargetId)        == 4, "id sizes must stay u32");
static_assert(sizeof(ValueId)         == 4, "id sizes must stay u32");
static_assert(sizeof(EffectId)        == 4, "id sizes must stay u32");
static_assert(sizeof(HandlerId)       == 4, "id sizes must stay u32");
static_assert(sizeof(StrId)           == 4, "id sizes must stay u32");
static_assert(sizeof(AstNodeId)       == 4, "id sizes must stay u32");
static_assert(sizeof(AstExtraDataIdx) == 4, "id sizes must stay u32");
static_assert(sizeof(AstId)           == 4, "id sizes must stay u32");


// Sentinels — slot 0 of every id column is reserved as NONE.

#define SOURCE_ID_NONE          ((SourceId){0})
#define MODULE_ID_NONE          ((ModuleId){0})
#define DEF_ID_NONE             ((DefId){0})
#define TYPE_ID_NONE            ((TypeId){0})
#define SCOPE_ID_NONE           ((ScopeId){0})
#define FILE_ID_NONE            ((FileId){0})
#define TARGET_ID_NONE          ((TargetId){0})
#define VALUE_ID_NONE           ((ValueId){0})
#define EFFECT_ID_NONE          ((EffectId){0})
#define HANDLER_ID_NONE         ((HandlerId){0})
#define STR_ID_NONE             ((StrId){0})
#define AST_NODE_ID_NONE        ((AstNodeId){0})
#define AST_EXTRA_DATA_IDX_NONE ((AstExtraDataIdx){0})
#define AST_ID_NONE             ((AstId){0})

// Composite keys.

// Universally-unique pointer to a syntax node across the workspace.
// Packs (ModuleId, AstNodeId) into 64 bits: high 32 = module, low 32 = node.
typedef struct { uint64_t raw; } GlobalNodeId;

#define GLOBAL_NODE_ID_NONE ((GlobalNodeId){0})

static inline GlobalNodeId global_node_id(ModuleId m, AstNodeId n) {
    return (GlobalNodeId){ .raw = ((uint64_t)m.idx << 32) | (uint64_t)n.idx };
}
static inline ModuleId  global_node_module(GlobalNodeId g) {
    return (ModuleId){ .idx = (uint32_t)(g.raw >> 32) };
}
static inline AstNodeId global_node_node(GlobalNodeId g) {
    return (AstNodeId){ .idx = (uint32_t)(g.raw & 0xFFFFFFFFu) };
}

// Stable identity for a body-level expression: (owning decl, local index
// in the decl's deterministic body walk). Slot 0 of the walk is reserved
// as the NULL sentinel — see body_store — so local==0 means "no expr."
typedef struct {
    DefId    decl;
    uint32_t local;
} ExprId;

#define EXPR_ID_NONE ((ExprId){ .decl = {0}, .local = 0 })

/*
    Per-type accessors.
    These generate one inline pair per type so the type checker catches misuse.
*/

#define ORE_ID_HELPERS(T, prefix)                                       \
    static inline bool prefix##_eq(T a, T b) { return a.idx == b.idx; } \
    static inline bool prefix##_valid(T a)   { return a.idx != 0;     }

ORE_ID_HELPERS(SourceId,        source_id)
ORE_ID_HELPERS(ModuleId,        module_id)
ORE_ID_HELPERS(DefId,           def_id)
ORE_ID_HELPERS(TypeId,          type_id)
ORE_ID_HELPERS(ScopeId,         scope_id)
ORE_ID_HELPERS(FileId,          file_id)
ORE_ID_HELPERS(TargetId,        target_id)
ORE_ID_HELPERS(ValueId,         value_id)
ORE_ID_HELPERS(EffectId,        effect_id)
ORE_ID_HELPERS(HandlerId,       handler_id)
ORE_ID_HELPERS(StrId,           str_id)
ORE_ID_HELPERS(AstNodeId,       ast_node_id)
ORE_ID_HELPERS(AstExtraDataIdx, ast_extra_data_idx)
ORE_ID_HELPERS(AstId,           ast_id)

#undef ORE_ID_HELPERS

static inline bool expr_id_eq(ExprId a, ExprId b) {
    return a.decl.idx == b.decl.idx && a.local == b.local;
}
static inline bool expr_id_valid(ExprId a) {
    // local==0 is the NULL sentinel slot seated by body_store init;
    // decl==0 means "no owning decl recorded." Either is invalid.
    return a.decl.idx != 0 && a.local != 0;
}

static inline bool global_node_id_eq(GlobalNodeId a, GlobalNodeId b) {
    return a.raw == b.raw;
}
static inline bool global_node_id_valid(GlobalNodeId a) {
    return a.raw != 0;
}

/*
    Tagged hashmap keys.

    When an id needs to live in a HashMap that holds mixed kinds (e.g. one
    keyed by either DefId or ModuleId), tag it with a 4-bit kind
    discriminator at the high bits so accidental cross-type lookups can't
    collide. For type-specific maps (`HashMap<DefId, X>`), hash the bare
    idx — no tag needed.
*/

#define TAG_DEF    0x1
#define TAG_MOD    0x2
#define TAG_TYPE   0x3
#define TAG_SCOPE  0x4
#define TAG_INPUT  0x5
#define TAG_FILE   0x6
#define TAG_VALUE  0x7

static inline uint64_t id_key(uint32_t idx, uint8_t tag) {
    return ((uint64_t)tag << 60) | (uint64_t)idx;
}

#endif
