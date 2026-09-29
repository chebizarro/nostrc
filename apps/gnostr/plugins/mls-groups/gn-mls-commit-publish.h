/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-mls-commit-publish.h - Publish a Commit until a relay accepts it
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_MLS_COMMIT_PUBLISH_H
#define GN_MLS_COMMIT_PUBLISH_H

#include <gio/gio.h>

G_BEGIN_DECLS

/*
 * One relay attempt that completes on the relay's NIP-01 OK (TRUE) or its
 * refusal/unreachability (FALSE + error), e.g.
 * gnostr_plugin_context_publish_event_to_relay_ack_async().
 */
typedef void     (*GnMlsAckPublishFunc)(gpointer             target,
                                        const char          *event_json,
                                        const char          *relay_url,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data);
typedef gboolean (*GnMlsAckFinishFunc)(gpointer       target,
                                       GAsyncResult  *result,
                                       GError       **error);
/* TRUE when @error is a definite refusal: the relay answered NIP-01
 * `OK false` and so did not store the event.  Anything else (timeout,
 * disconnect, unreachable, no OK) leaves open whether it was stored. */
typedef gboolean (*GnMlsAckIsRejectionFunc)(const GError *error);

#define GN_MLS_PUBLISH_ERROR (gn_mls_publish_error_quark())
GQuark gn_mls_publish_error_quark(void);

/*
 * How a publish ended when no relay accepted it:
 * @GN_MLS_PUBLISH_REJECTED: every relay refused (`OK false`): the event is
 *   certainly not published, so the pending Commit may be cleared.
 * @GN_MLS_PUBLISH_UNCERTAIN: some relay may have stored it (lost OK,
 *   timeout, disconnect): keep the Commit pending and retry; its echo from a
 *   relay merges it.
 * @GN_MLS_PUBLISH_NO_RELAYS: nothing to publish to (nothing was sent).
 */
typedef enum {
  GN_MLS_PUBLISH_REJECTED,
  GN_MLS_PUBLISH_UNCERTAIN,
  GN_MLS_PUBLISH_NO_RELAYS,
} GnMlsPublishError;

/*
 * MIP-03: a Commit is applied only once at least one relay confirmed it.
 * Publish the signed @event_json to @relay_urls one after another until one
 * answers OK.  Finishes with that relay's URL, or with a GN_MLS_PUBLISH_ERROR
 * (the message names the last relay's error): merge on success, clear only
 * on GN_MLS_PUBLISH_REJECTED / _NO_RELAYS, keep the Commit pending on
 * GN_MLS_PUBLISH_UNCERTAIN (review R2).
 */
void   gn_mls_publish_until_ack_async(GnMlsAckPublishFunc  publish,
                                      GnMlsAckFinishFunc   finish,
                                      GnMlsAckIsRejectionFunc is_rejection,
                                      gpointer             target,
                                      const char          *event_json,
                                      const char * const  *relay_urls,
                                      GCancellable        *cancellable,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data);
gchar *gn_mls_publish_until_ack_finish(GAsyncResult  *result,
                                       GError       **error);

G_END_DECLS

#endif /* GN_MLS_COMMIT_PUBLISH_H */
