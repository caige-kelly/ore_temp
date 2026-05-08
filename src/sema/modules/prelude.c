#include "../../common/stringpool.h"
#include "../scope/scope.h"
#include "../sema.h"
#include "modules.h"

// Prelude — the synthetic ModuleId{1} that holds builtin primitive
// type names. Every user module's internal_scope parents to the
// prelude's export_scope so bare primitive names (`i32`, `u8`, etc.)
// resolve without per-module setup.
//
// Visibility model: every primitive is `Visibility_public`. The
// prelude's internal_scope and export_scope hold identical contents
// — there's no "private" prelude. This matches Zig and Rust: the
// prelude is fundamentally a public namespace.
//
// Adding a primitive is a one-liner in `register_primitives` below.
// The cached Type* on Sema (e.g. `s->i32_type`) is the canonical
// type representation; the DefId we register here is what
// `query_resolve_ref` returns when it looks up `i32`. The Type→Def
// link is established during sema_check when type resolution maps
// the resolved DefId back to a Type*.

static DefId register_primitive(struct Sema *s, ModuleId mid,
                                ScopeId internal, ScopeId export_scope,
                                const char *name, size_t name_len) {
  uint32_t name_id = pool_intern(s->pool, name, name_len);
  struct DefInfo proto = {
      .kind = DECL_PRIMITIVE,
      .semantic_kind = SEM_TYPE,
      .name_id = name_id,
      .span = (struct Span){0},
      .origin_id = (struct NodeId){0},
      .origin = NULL,
      .owner_scope = internal,
      .child_scope = SCOPE_ID_INVALID,
      .imported_module = MODULE_ID_INVALID,
      .vis = Visibility_public,
      .scope_token_id = 0,
      .is_comptime = false,
      .has_effects = false,
  };
  DefId def = def_create(s, proto);
  scope_insert_def(s, internal, def);
  scope_insert_def(s, export_scope, def);
  (void)mid;
  return def;
}

#define PRIM(s, mid, internal, export_scope, lit) \
  register_primitive((s), (mid), (internal), (export_scope), (lit), \
                     sizeof(lit) - 1)

// Initialize the prelude module. Called once from sema_new (before
// any user modules are processed). Stamps `s->prelude_module` and
// populates the scope tables with primitive type defs.
//
// Registration order is stable so DefIds are deterministic across
// runs — useful when comparing `--dump-resolve` outputs.
void prelude_init(struct Sema *s) {
  if (module_id_is_valid(s->prelude_module))
    return;

  ModuleId pid = module_create(s, INPUT_ID_INVALID, /*is_prelude=*/true);
  s->prelude_module = pid;

  // Run def_map to materialize the prelude's scopes. The walk is a
  // no-op (NULL ast) but the slot transitions to DONE and the
  // scopes get allocated.
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
