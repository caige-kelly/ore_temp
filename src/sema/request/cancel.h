#ifndef ORE_SEMA_CANCEL_H
#define ORE_SEMA_CANCEL_H

#include <stdatomic.h>
#include <stdbool.h>

// Cancellation tokens — cooperative bail-out for long-running
// queries when the LSP server learns the user has typed again.
//
// Model: a CancelToken is a tiny struct holding an atomic flag.
// External code (the LSP shell) creates a token, attaches it to
// the active request via sema_set_active_cancel, and flips the
// flag when the request is superseded. The query engine checks
// the flag at every sema_query_begin and unwinds via the new
// QUERY_BEGIN_CANCELED state.
//
// Atomic so a future multi-threaded compiler can flip the flag
// from a different thread than the query is running on. Today
// the flag is set by the same thread between requests; the
// _Atomic costs nothing on uncontended access.
//
// Cancellation is *cooperative*: the engine doesn't preempt
// in-flight C code. Granularity is one query. Long bodies that
// never call into the engine won't observe cancellation —
// that's an acceptable tradeoff because most Ore queries are
// short and engine-heavy.

struct Sema;

struct CancelToken {
    _Atomic bool flag;
};

static inline void cancel_token_init(struct CancelToken *tok) {
    atomic_store(&tok->flag, false);
}

static inline void cancel_token_signal(struct CancelToken *tok) {
    if (tok)
        atomic_store(&tok->flag, true);
}

static inline bool cancel_token_is_set(const struct CancelToken *tok) {
    if (!tok)
        return false;
    return atomic_load(&tok->flag);
}

// Install / uninstall the active cancel token on `s`. Called by the
// request handler at request boundaries. The query engine's per-
// query check reads `s->active_cancel`.
void sema_set_active_cancel(struct Sema *s, struct CancelToken *tok);
void sema_clear_active_cancel(struct Sema *s);

// Check whether the active request has been cancelled. Cheap;
// safe to call from any sema code path. Returns false when no
// token is installed.
bool sema_check_cancel(struct Sema *s);

#endif // ORE_SEMA_CANCEL_H
