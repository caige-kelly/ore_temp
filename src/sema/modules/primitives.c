#include "../../common/stringpool.h"
#include "../scope/scope.h"
#include "../sema.h"
#include "modules.h"

// Primitives module — the synthetic ModuleId{1} that holds the
// language's compiler-built-in type names (`u8`, `i32`, `bool`,
// `f32`, ...). Every user module's internal_scope parents to the
// primitives module's export_scope so bare primitive names resolve
// without any user-side import.
//
// This is NOT a "prelude" in the Rust/Haskell sense — there's no
// auto-imported stdlib here. Ore's stdlib goes through explicit
// `@import("std/...")`. The primitives module exists only because
// type names like `u8` aren't user-writable; they're language facts
// the compiler injects. Same shape as Zig's primitives.
//
// IMMUTABILITY CONTRACT: this module is populated exactly once,
// during `primitives_init`, and is read-only thereafter. No other
// code path may add, remove, or modify entries. Queries that depend
// on the primitives module record a (no-op) dep on its def_map_query
// — the slot fingerprint never changes, so dependents are never
// invalidated by primitive-set changes (because there are none).
// If primitives ever become configurable (target-arch-specific
// intrinsics, etc.), this module would be promoted to a real Salsa
// input with mutator API; for now it's a fixed compiler fact.
//
// Visibility model: every primitive is `Visibility_public`. The
// internal_scope and export_scope hold identical contents — there's
// no "private" primitives. Same as Zig.
//
// Adding a primitive is a one-liner in the PRIM list below. The
// cached Type* on Sema (e.g. `s->i32_type`) is the canonical type
// representation; the DefId registered here is what
// `query_resolve_ref` returns when looking up `i32`. The Type→Def
// link is established by `sema_types_init` populating
// `Sema.primitive_types` so `query_type_of_decl` can map a
// resolved primitive DefId back to its Type*.

static DefId register_primitive(struct Sema *s, ModuleId mid, ScopeId internal,
                                ScopeId export_scope, const char *name,
                                size_t name_len) {
  StrId name_id = pool_intern(&s->pool, name, name_len);
  struct DefInfo proto = {
      .kind = DECL_PRIMITIVE,
      .semantic_kind = SEM_TYPE,
      .name_id = name_id,
      .span = (struct Span){0},
      .origin_id = (struct NodeId){0},
      .origin = NULL,
      .owner_scope = internal,
      .imported_module = MODULE_ID_INVALID,
      .vis = Visibility_public,
      .scope_token_id = 0,
  };
  DefId def = def_create(s, proto);
  // Internal is the canonical home (matches owner_scope in proto);
  // export_scope is a public mirror so cross-module lookups find it.
  scope_define_def(s, internal, def);
  scope_mirror_def(s, export_scope, def);
  (void)mid;
  return def;
}

#define PRIM(s, mid, internal, export_scope, lit)                              \
  register_primitive((s), (mid), (internal), (export_scope), (lit),            \
                     sizeof(lit) - 1)

// Initialize the primitives module. Called exactly once from
// `sema_init` (before any user modules are processed). Stamps
// `s->primitives_module`, materializes its scopes via
// `query_module_def_map` (a no-op walk, since the module has no
// AST), and inserts each primitive's DefInfo into both internal
// and export scopes.
//
// Re-calls are no-ops (early return on valid module). After this
// returns, the primitives module is sealed — see the immutability
// contract at the top of this file.
//
// Registration order is stable so DefIds are deterministic across
// runs — useful when comparing `--dump-resolve` outputs.
void primitives_init(struct Sema *s) {
  if (module_id_is_valid(s->primitives_module))
    return;

  ModuleId pid = module_create(s, INPUT_ID_INVALID, /*is_primitives=*/true);
  s->primitives_module = pid;

  // Run def_map to materialize the primitives module's scopes. The
  // walk is a no-op (NULL ast) but the slot transitions to DONE
  // and the internal/export scopes get allocated.
  query_module_def_map(s, pid);

  struct ModuleInfo *m = module_info(s, pid);
  if (!m)
    return;
  ScopeId internal = m->internal_scope;
  ScopeId export_scope = m->export_scope;

  PRIM(s, pid, internal, export_scope, "void");
  PRIM(s, pid, internal, export_scope, "bool");
  PRIM(s, pid, internal, export_scope, "noreturn");

  PRIM(s, pid, internal, export_scope, "i8");
  PRIM(s, pid, internal, export_scope, "i16");
  PRIM(s, pid, internal, export_scope, "i32");
  PRIM(s, pid, internal, export_scope, "i64");
  PRIM(s, pid, internal, export_scope, "isize");

  PRIM(s, pid, internal, export_scope, "u8");
  PRIM(s, pid, internal, export_scope, "u16");
  PRIM(s, pid, internal, export_scope, "u32");
  PRIM(s, pid, internal, export_scope, "u64");
  PRIM(s, pid, internal, export_scope, "usize");

  PRIM(s, pid, internal, export_scope, "f32");
  PRIM(s, pid, internal, export_scope, "f64");

  PRIM(s, pid, internal, export_scope, "type");
  PRIM(s, pid, internal, export_scope, "anytype");
  PRIM(s, pid, internal, export_scope, "string");
  PRIM(s, pid, internal, export_scope, "nil");

  PRIM(s, pid, internal, export_scope, "comptime_int");
  PRIM(s, pid, internal, export_scope, "comptime_float");
}

#undef PRIM
