#include "cancel.h"

#include "../sema.h"

bool sema_check_cancel(struct Sema *s) {
  return cancel_token_is_set(s->active_cancel);
}
