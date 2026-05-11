#ifndef ORE_SEMA_CANCEL_H
#define ORE_SEMA_CANCEL_H

#include <stdatomic.h>
#include <stdbool.h>

// Cancellation tokens — cooperative bail-out for long-running
// queries when the LSP server learns the user has typed again.
//
// Model: a CancelToken is a tiny struct holding an atomic flag.
// External code (the LSP shell) would create a token, attach it to
// the active request, and flip the flag when the request is
// superseded. The query engine checks the flag at every
// sema_query_begin and unwinds via QUERY_BEGIN_CANCELED.
//
// Today the LSP runs requests synchronously, so no external code
// installs a token — `Sema.active_cancel` stays NULL and the check
// at sema_query_begin is always a no-op. The infrastructure is kept
// because adding it back when the LSP grows async/cancellable
// request handling is a single field-flip plus an installer.

struct Sema;

struct CancelToken {
    _Atomic bool flag;
};

static inline bool cancel_token_is_set(const struct CancelToken *tok) {
    if (!tok)
        return false;
    return atomic_load(&tok->flag);
}

// Check whether the active request has been cancelled. Cheap;
// safe to call from any sema code path. Returns false when no
// token is installed.
bool sema_check_cancel(struct Sema *s);

#endif // ORE_SEMA_CANCEL_H
