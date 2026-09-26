/*
 * nd-dispatch — orchestrates one "open" request:
 *
 *   parse -> (kind known?  choose handler
 *                          -> handler running + Handler1? fetch event for
 *                             handoff, else launch the canonical URI)
 *            (kind unknown? fetch event (session relay, then hints)
 *                          -> kind from the validated event -> choose
 *                          -> fetch failed: fallback handler, URI launch)
 *
 * The URI handed to a handler is always our canonical re-encoding, never
 * the caller's raw string. Launch is GDesktopAppInfo (argv, no shell) for
 * installed desktop entries only.
 */
#ifndef ND_DISPATCH_H
#define ND_DISPATCH_H

#include <gio/gio.h>
#include "nd-registry.h"

G_BEGIN_DECLS

typedef struct {
  char *activation_token; /* XDG activation token / startup id, or NULL */
  gboolean dry_run;       /* resolve only: never launch, never hand off */
  gboolean no_handoff;    /* never try org.nostr.Handler1 */
} NdOpenOptions;

typedef struct {
  gint kind;           /* -1 if still unknown */
  char *desktop_id;    /* chosen handler */
  NdSource source;
  gboolean handed_off; /* delivered via org.nostr.Handler1.OpenEvent */
  char *launched_uri;  /* canonical URI (or file URI) passed to the app */
} NdOpenResult;

void nd_open_result_free(NdOpenResult *r);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(NdOpenResult, nd_open_result_free)

/* @reg: transfer full. @uri: nostr:/web+nostr: URI or legacy form. */
void nd_dispatch_open_uri_async(NdRegistry *reg, const char *uri,
                                const NdOpenOptions *opts, GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer user_data);

/* @reg: transfer full. @event_json: event object (e.g. a .nostr file);
 * @relays: optional hints; @file_uri: the file it came from, or NULL. */
void nd_dispatch_open_event_async(NdRegistry *reg, const char *event_json,
                                  const char *const *relays, const char *file_uri,
                                  const NdOpenOptions *opts, GCancellable *cancellable,
                                  GAsyncReadyCallback callback, gpointer user_data);

NdOpenResult *nd_dispatch_open_finish(GAsyncResult *res, GError **error);

/* D-Bus bus name / object path a handler desktop id would own under the
 * GApplication convention ("org.example.App.desktop" -> "org.example.App",
 * "/org/example/App"). NULL if the id is not a valid well-known name. */
char *nd_handler_bus_name(const char *desktop_id);
char *nd_handler_object_path(const char *bus_name);

G_END_DECLS

#endif /* ND_DISPATCH_H */
