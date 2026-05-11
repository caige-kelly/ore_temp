#include "snapshot.h"

#include "../sema.h"

uint64_t sema_effective_revision(struct Sema *s) {
  return s->request_revision != 0 ? s->request_revision : s->current_revision;
}
