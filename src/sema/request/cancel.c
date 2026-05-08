#include "cancel.h"

#include "../sema.h"

void sema_set_active_cancel(struct Sema *s, struct CancelToken *tok) {
  s->active_cancel = tok;
}

void sema_clear_active_cancel(struct Sema *s) {
  s->active_cancel = NULL;
}

bool sema_check_cancel(struct Sema *s) {
  return cancel_token_is_set(s->active_cancel);
}
