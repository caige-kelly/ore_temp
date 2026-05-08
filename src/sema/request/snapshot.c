#include "snapshot.h"

#include "../sema.h"

struct Snapshot sema_snapshot_begin(struct Sema *s) {
  // Capture *prior* request_revision into the Snapshot so end()
  // can restore it for nested begin/end pairs. The captured
  // revision is the current_revision at this moment.
  struct Snapshot snap = {
      .s = s,
      .revision = s->request_revision,
  };
  s->request_revision = s->current_revision;
  return snap;
}

void sema_snapshot_end(struct Snapshot *snap) {
  if (!snap || !snap->s)
    return;
  // Restore the prior pinned revision (0 if outermost).
  snap->s->request_revision = snap->revision;
  snap->s = NULL;
}

uint64_t sema_effective_revision(struct Sema *s) {
  return s->request_revision != 0 ? s->request_revision
                                  : s->current_revision;
}
