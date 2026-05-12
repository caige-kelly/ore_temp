#include "db.h"

#include "uri.h"

#include "../common/hashmap.h"
#include "../diag/diag.h"
#include "../sema/modules/modules.h"
#include "../sema/query/query.h"
#include "../sema/resolve/scope_index.h"
#include "../sema/type/checker.h"

#include <stdio.h>
#include <stdlib.h>

void oredb_init(struct OreDb *db) {
  sema_init(&db->sema);
  vec_init(&db->drafts, sizeof(struct Draft));
}

void oredb_free(struct OreDb *db) {
  vec_free(&db->drafts);
  sema_free(&db->sema);
}

// Grow the drafts vec so `drafts[iid.idx]` is accessible. New
// slots are zero-initialized — lsp_synced=false, version=0 —
// which matches the "never opened" semantics.
static struct Draft *ensure_draft_slot(struct OreDb *db, InputId iid) {
  while (db->drafts.count <= iid.idx) {
    struct Draft zero = {0};
    vec_push(&db->drafts, &zero);
  }
  return (struct Draft *)vec_get(&db->drafts, iid.idx);
}

// Canonicalize URI and register/lookup the InputId. Returns
// INPUT_ID_INVALID on URI parse failure (non-file:// scheme,
// malformed). On success the returned path string is freed
// before return — caller doesn't need it.
static InputId resolve_uri_to_input(struct OreDb *db, const char *uri) {
  char *path = lsp_uri_to_path(uri);
  if (!path) {
    fprintf(stderr, "lsp: ignoring URI with unsupported scheme: %s\n", uri);
    return INPUT_ID_INVALID;
  }
  InputId iid = sema_register_input(&db->sema, path);
  free(path);
  return iid;
}

InputId oredb_did_open(struct OreDb *db, const char *uri, int32_t version,
                       const char *text, size_t text_len) {
  InputId iid = resolve_uri_to_input(db, uri);
  if (!input_id_is_valid(iid))
    return INPUT_ID_INVALID;

  sema_set_input_source(&db->sema, iid, text, text_len);

  struct Draft *d = ensure_draft_slot(db, iid);
  d->lsp_synced = true;
  d->version = version;
  return iid;
}

InputId oredb_did_change(struct OreDb *db, const char *uri, int32_t version,
                         const char *text, size_t text_len) {
  InputId iid = resolve_uri_to_input(db, uri);
  if (!input_id_is_valid(iid))
    return INPUT_ID_INVALID;

  struct Draft *d = ensure_draft_slot(db, iid);

  // Per LSP spec, didChange version must be > the last version
  // for this document. Tolerate equal (some clients re-send the
  // same version on no-op edits); drop strictly older. The "<"
  // case is the failure mode we care about.
  if (d->lsp_synced && version < d->version) {
    fprintf(stderr, "lsp: dropping stale didChange (version %d < %d) for %s\n",
            version, d->version, uri);
    return INPUT_ID_INVALID;
  }

  sema_set_input_source(&db->sema, iid, text, text_len);
  d->lsp_synced = true;
  d->version = version;
  return iid;
}

// Look up the ModuleId already associated with this input via the
// path_id cache module_create populates. Returns MODULE_ID_INVALID
// if no module exists yet. Avoids the double-allocation hazard of
// calling module_create twice for the same input (each call
// allocates a fresh ModuleInfo and overwrites the path cache).
static ModuleId module_for_input(struct Sema *s, InputId iid) {
  if (!input_id_is_valid(iid))
    return MODULE_ID_INVALID;
  struct InputInfo *ii = input_info(s, iid);
  if (!ii || ii->path_id.v == 0)
    return MODULE_ID_INVALID;
  if (!hashmap_contains(&s->module_by_path, (uint64_t)ii->path_id.v))
    return MODULE_ID_INVALID;
  void *slot = hashmap_get(&s->module_by_path, (uint64_t)ii->path_id.v);
  return (ModuleId){(uint32_t)(uintptr_t)slot};
}

ModuleId oredb_typecheck(struct OreDb *db, InputId iid) {
  if (!input_id_is_valid(iid) || iid.idx >= db->drafts.count)
    return MODULE_ID_INVALID;
  struct Draft *d = (struct Draft *)vec_get(&db->drafts, iid.idx);

  // First typecheck for this input: allocate (or recover an
  // existing) ModuleId. Subsequent passes reuse the cached one;
  // the query system handles revalidation via the revision bump
  // sema_set_input_source already performed.
  if (!module_id_is_valid(d->mid)) {
    d->mid = module_for_input(&db->sema, iid);
    if (!module_id_is_valid(d->mid))
      d->mid = module_create(&db->sema, iid, /*is_primitives=*/false);
  }

  // Reset diagnostics so the publishDiagnostics payload describes
  // only the current revision. Cross-input diagnostics aren't a
  // concern yet (single-file projects); when multi-file lands,
  // this needs to become a per-file filter instead of a blanket
  // clear.
  diag_bag_clear(&db->sema.diags);

  bool ok = query_module_def_map(&db->sema, d->mid);
  if (ok)
    scope_index_build_module(&db->sema, d->mid);
  if (ok)
    sema_check_module(&db->sema, d->mid);

  return d->mid;
}

bool oredb_did_close(struct OreDb *db, const char *uri) {
  // didClose on an unknown URI is a no-op. We do NOT want to
  // allocate a fresh InputId here just to flag-then-discard it.
  // The simplest path is still to resolve via sema_register_input
  // — it's idempotent for known paths and the spurious new
  // allocation for unknown paths is harmless (just an empty,
  // never-touched slot).
  InputId iid = resolve_uri_to_input(db, uri);
  if (!input_id_is_valid(iid))
    return false;

  if (iid.idx >= db->drafts.count) {
    // Never opened — nothing to flip.
    return false;
  }
  struct Draft *d = (struct Draft *)vec_get(&db->drafts, iid.idx);
  if (!d->lsp_synced)
    return false; // already closed or never opened

  d->lsp_synced = false;
  return true;
}
