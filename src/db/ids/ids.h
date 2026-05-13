#ifndef ORE_DB_IDS_H
#define ORE_DB_IDS_H

#include <stdbool.h>
#include <stdint.h>


/* Kinds of Ore */
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

/* THE PRIMARY KEYS (The Handles) */

typedef struct { uint32_t idx; } SourceId;
typedef struct { uint32_t idx; } ModuleId;
typedef struct { uint32_t idx; } DefId;
typedef struct { uint32_t idx; } TypeId;
typedef struct { uint32_t idx; } ScopeId;
typedef struct { uint32_t idx; } FileId;
typedef struct { uint32_t idx; } TargetId;
typedef struct { uint32_t idx; } ValueId;
typedef struct { uint32_t idx; } KindId;

// Identifies a specific syntax node within a single module.
typedef struct { uint32_t idx; } AstNodeId;

// Strongly-typed handle to an index in the extra_data bucket.
typedef struct { uint32_t raw; } AstExtraDataIdx;

// Identifies an entry in the String Pool.
typedef struct { uint32_t idx; } StrId;

/* String functions */
#define ID_EQ(a, b) ((a).idx == (b).idx)
#define ID_VALID(a) ((a).idx != 0)

/* SENTINELS (The NULLs) */

#define SOURCE_ID_NONE   ((SourceId){0})
#define MODULE_ID_NONE   ((ModuleId){0})
#define DEF_ID_NONE      ((DefId){0})
#define TYPE_ID_NONE     ((TypeId){0})
#define SCOPE_ID_NONE    ((ScopeId){0})
#define AST_ID_NONE      ((AstNodeId){0})
#define STR_ID_NONE      ((StrId){0})
#define FILE_ID_NONE     ((FileId){0})
#define TARGET_ID_NONE   ((TargetId){0})
#define VALUE_ID_NONE    ((ValueId){0})
#define ASTSTORE_ID_NONE ((ASTStore){0})
#define KIND_ID_NONE     ((DefKind){0})
#define EFFECT_ID_NONE   ((EffectId){0})
#define HANDLER_ID_NONE  ((HandlerId){0})
#define ASTEXTRA_ID_NONE ((AstExtraDataIdx){0})
#define ASTNODE_ID_NONE  ((AstNodeId){0})


// EFFECTS 
typedef struct { uint32_t idx; } EffectId;    // The 'ability'
typedef struct { uint32_t idx; } HandlerId;   // The implementation

/* COMPOSITE KEYS (The Joins) */

// GlobalNodeId: A universally unique pointer to a syntax node.
// Packs (ModuleId | AstNodeId) into 64 bits.
typedef struct { uint64_t raw; } GlobalNodeId;

static inline GlobalNodeId global_node_id(ModuleId m, AstNodeId n) {
    return (GlobalNodeId){ .raw = ((uint64_t)m.idx << 32) | n.idx };
}

static inline ModuleId  gn_module(GlobalNodeId g) { return (ModuleId){(uint32_t)(g.raw >> 32)}; }
static inline AstNodeId gn_node(GlobalNodeId g)   { return (AstNodeId){(uint32_t)(g.raw & 0xFFFFFFFF)}; }

// ExprId: A stable identity for a body expression.
// Decoupled from position-based AstNodeId.
typedef struct { 
    DefId decl;      // The function/const owning this body
    uint32_t local;  // Index in the deterministic body walk
} ExprId;

#define EXPR_ID_NONE ((ExprId){.decl = {0}, .local = 0})

/* 4. UTILITIES */

#define ID_EQ(a, b) ((a).idx == (b).idx)
#define ID_VALID(a) ((a).idx != 0)

static inline bool expr_id_eq(ExprId a, ExprId b) {
    return a.decl.idx == b.decl.idx && a.local == b.local;
}

static inline bool expr_id_valid(ExprId id) {
    return id.decl.idx != 0 && id.local != 0;
}

/* HASH KEYS */
// Used when an ID needs to be a key in a sparse HashMap.
// Packs the 32-bit ID with a 4-bit type tag to prevent accidental collisions.

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