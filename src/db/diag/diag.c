// Diag struct + DiagArg + DiagSeverity types live in diag.h.
// Implementations split across:
//   src/db/setters/diag.c   — db_emit_* (writes Diag into the active slot)
//   src/db/getters/diag.c   — db_collect_diags_*, db_format_diag,
//                             db_print_diag, db_resolve_span
// This file is empty by design — kept to avoid breaking the build
// glob (find -name '*.c'). Once we're confident nothing references it,
// it can be deleted.
