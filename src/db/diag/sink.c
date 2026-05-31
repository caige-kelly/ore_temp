// Phase P P7.1.2 — DiagSink emit path + cleanup helpers.
//
// This file implements the new pure-query diag path. It coexists with
// src/db/inputs/diag.c (the legacy side-channel store) throughout the
// P7.1–P7.5 rollout; the legacy file is deleted in P7.5 once every
// emitting query has migrated.
//
// Provides:
//   - diag_bundle_free        (P2.b — manual heap discipline)
//   - diag_index_remove_ref   (P2.a — R7 stale-index guard; stub
//                              until P7.1.3 introduces the index)
//   - diag_sink_emit          (P3 — sink-routed variadic emit)
//   - diag_sink_emit_tagged   (P3 + N2 — tagged variant)

#include "diag.h"
#include "../db.h"

#include "../../support/data_structure/stringpool.h"
#include "../../support/data_structure/arena.h"
#include "../../support/data_structure/vec.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
// diag_bundle_free  (P2.b)
// ============================================================================

void diag_bundle_free(DiagBundle *b) {
  if (!b)
    return;
  vec_free(&b->items);
  arena_free(&b->args_arena);
  b->items = (Vec){0};
  b->args_arena = (Arena){0};
}

void diag_bundle_reset(DiagBundle *b) {
  if (!b)
    return;
  // First-recompute path: lazy-init if the slot has never held a
  // bundle (zeroed by paged_push_zero). Subsequent recomputes reuse
  // capacity via vec_clear + arena_reset.
  if (b->items.element_size == 0)
    vec_init(&b->items, sizeof(Diag));
  else
    vec_clear(&b->items);
  if (b->args_arena.generation == 0)
    arena_init(&b->args_arena, 0); // default chunk capacity
  else
    arena_reset(&b->args_arena);
}

// ============================================================================
// diag_index_remove_ref  (P2.a — stub for P7.1.2; populated in P7.1.3)
// ============================================================================
//
// Becomes the per-file index removal helper once
// db.diag_bundles_by_file / db.diag_bundles_multi_file land in P7.1.3.
// Stubbed here so prototypes resolve during the incremental rollout.

void diag_index_remove_ref(struct db *s, DiagBundleRef ref) {
  (void)s;
  (void)ref;
  // TODO(Phase P P7.1.3): remove ref from s->diag_bundles_by_file or
  // s->diag_bundles_multi_file and clear the diag_decl_file_cache
  // entry for body-anchored columns.
}

// ============================================================================
// build_template_and_args  (shared between legacy emit + sink emit)
// ============================================================================
//
// Duplicated from src/db/inputs/diag.c during the migration window;
// the legacy file goes away at P7.5 and this becomes the only copy.
// Walks `fmt`, replaces every `%X` specifier with `{N}` and pushes the
// matching DiagArg into args_out[]. Same format specifier set as the
// legacy db_emit:
//   %S StrId    %s C-string    %T IpIndex   %d int   %c char   %P DiagAnchor

static size_t build_template_and_args(struct db *s, const char *fmt,
                                      va_list ap, char *tmpl_out,
                                      size_t tmpl_cap, DiagArg *args_out,
                                      size_t args_cap) {
  size_t t = 0;
  size_t n = 0;
  size_t cap = tmpl_cap - 1;

  for (size_t i = 0; fmt[i] && t < cap; i++) {
    char c = fmt[i];
    if (c != '%') {
      tmpl_out[t++] = c;
      continue;
    }
    c = fmt[++i];
    if (c == '\0') {
      if (t < cap)
        tmpl_out[t++] = '%';
      break;
    }
    if (c == '%') {
      if (t < cap)
        tmpl_out[t++] = '%';
      continue;
    }

    if (n >= args_cap)
      break;
    switch (c) {
    case 'S':
      args_out[n].kind = DIAG_ARG_STR_ID;
      args_out[n].str = va_arg(ap, StrId);
      break;
    case 's': {
      const char *cs = va_arg(ap, const char *);
      args_out[n].kind = DIAG_ARG_STR_ID;
      args_out[n].str =
          pool_intern(&s->strings, cs ? cs : "(null)", cs ? strlen(cs) : 6);
      break;
    }
    case 'T':
      args_out[n].kind = DIAG_ARG_TYPE;
      args_out[n].type = va_arg(ap, IpIndex);
      break;
    case 'd':
      args_out[n].kind = DIAG_ARG_INT;
      args_out[n].i = va_arg(ap, int);
      break;
    case 'c':
      args_out[n].kind = DIAG_ARG_CHAR;
      args_out[n].ch = va_arg(ap, unsigned int);
      break;
    case 'P':
      args_out[n].kind = DIAG_ARG_SPAN;
      args_out[n].span = va_arg(ap, DiagAnchor);
      break;
    default:
      if (t + 1 < cap) {
        tmpl_out[t++] = '%';
        tmpl_out[t++] = c;
      }
      continue;
    }
    int wrote = snprintf(tmpl_out + t, cap - t, "{%zu}", n);
    if (wrote > 0)
      t += (size_t)wrote > cap - t ? cap - t : (size_t)wrote;
    n++;
  }
  tmpl_out[t] = '\0';
  return n;
}

// ============================================================================
// diag_sink_emit{,_tagged}  (P3 — main emit path)
// ============================================================================

// Default first-chunk size for a sink's args arena. Match the legacy
// per-DiagList arena size so memory patterns stay comparable.
#define ORE_SINK_ARENA_DEFAULT_CHUNK_CAP (1 * 1024)

static void sink_emit_impl(DiagSink *sink, struct db *s, DiagSeverity severity,
                           DiagTag tag, uint8_t owner_kind, DiagAnchor anchor,
                           const char *fmt, va_list ap) {
  // Gap #2: every emit asserts the sink is wired. A NULL sink means
  // the caller forgot to construct one (driver-level path) or a
  // pass-through frame's parent never set one up. Either is a bug.
  assert(sink != NULL &&
         "diag_sink_emit: sink is NULL — pass-through frame's parent "
         "didn't push a sink, or driver forgot to construct one");
  assert(sink->items != NULL && "diag_sink_emit: sink->items is NULL");
  assert(sink->args_arena != NULL && "diag_sink_emit: sink->args_arena is NULL");

  char tmpl[512];
  DiagArg args[8];
  size_t n = build_template_and_args(s, fmt, ap, tmpl, sizeof tmpl, args, 8);

  StrId tid = pool_intern(&s->strings, tmpl, strlen(tmpl));

  // Copy args into the sink's arena so the per-row Diag.args pointer
  // stays valid until the bundle is reset.
  const DiagArg *args_copied = NULL;
  if (n > 0) {
    DiagArg *dst =
        (DiagArg *)arena_alloc_raw(sink->args_arena, sizeof(DiagArg) * n);
    memcpy(dst, args, sizeof(DiagArg) * n);
    args_copied = dst;
  }

  Diag d = {
      .anchor = anchor,
      .template_id = tid,
      .args = args_copied,
      .code = 0,
      .n_args = (uint8_t)n,
      .severity = severity,
      .tag = (uint8_t)tag,
      .owner_kind = owner_kind, // stamped by caller (see P7 for migration policy)
  };
  vec_push(sink->items, &d);

  // TODO(Phase P P7.1.3): on first emit per bundle per recompute,
  // insert sink->ref into s->diag_bundles_by_file[anchor's file].
  // Stubbed until the index lands.
  (void)anchor;
}

void diag_sink_emit(struct db *s, DiagSink *sink, DiagSeverity severity,
                    DiagAnchor anchor, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  // owner_kind is 0 here; callers that need the parse-vs-sema gate
  // (Phase O's has_parse_errors) use diag_sink_emit_tagged or the
  // explicit-owner_kind variant added in P7.1.8 when the INFER_BODY
  // migration lands. For now this matches the surface of db_emit.
  sink_emit_impl(sink, s, severity, DIAG_TAG_NONE, /*owner_kind=*/0, anchor,
                 fmt, ap);
  va_end(ap);
}

void diag_sink_emit_tagged(struct db *s, DiagSink *sink, DiagSeverity severity,
                           DiagTag tag, DiagAnchor anchor, const char *fmt,
                           ...) {
  va_list ap;
  va_start(ap, fmt);
  sink_emit_impl(sink, s, severity, tag, /*owner_kind=*/0, anchor, fmt, ap);
  va_end(ap);
}
