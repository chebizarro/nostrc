/*
 * nd-fetch — obtain the validated event a NdTarget points at.
 *
 * Order: the per-user session relay ($XDG_RUNTIME_DIR/nostr/relay.sock,
 * WebSocket over AF_UNIX via libsoup's remote-connectable — libnostr's
 * relay client only speaks ws:// / wss://), then the target's relay hints
 * via libnostr's relay client (worker thread, sequential, shared budget).
 *
 * Every candidate is admitted only if nd_event_parse() validated it AND
 * nd_event_matches_target() holds, so a hostile relay can neither forge an
 * event nor substitute a different kind to steer routing.
 */
#ifndef ND_FETCH_H
#define ND_FETCH_H

#include <gio/gio.h>
#include "nd-event.h"
#include "nd-uri.h"

G_BEGIN_DECLS

typedef struct {
  gboolean use_local;
  gboolean use_hints;
  const char *socket_path;   /* NULL: $XDG_RUNTIME_DIR/nostr/relay.sock */
  guint local_timeout_ms;    /* 0: default 1500 */
  guint hints_budget_ms;     /* 0: default 4000 (all hints together) */
} NdFetchOptions;

char *nd_fetch_default_socket_path(void);

/* REQ filter JSON for @t (exposed for tests). */
char *nd_fetch_filter_json(const NdTarget *t);

void nd_fetch_event_async(const NdTarget *t, const NdFetchOptions *opts,
                          GCancellable *cancellable,
                          GAsyncReadyCallback callback, gpointer user_data);
NdEvent *nd_fetch_event_finish(GAsyncResult *res, GError **error);

G_END_DECLS

#endif /* ND_FETCH_H */
