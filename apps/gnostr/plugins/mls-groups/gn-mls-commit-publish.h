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

/*
 * MIP-03: a Commit is applied only once at least one relay confirmed it.
 * Publish the signed @event_json to @relay_urls one after another until one
 * answers OK.  Finishes with that relay's URL, or with the last relay's
 * error (G_IO_ERROR_NOT_FOUND when there are no relays): the caller then
 * merges or clears its pending Commit.
 */
void   gn_mls_publish_until_ack_async(GnMlsAckPublishFunc  publish,
                                      GnMlsAckFinishFunc   finish,
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
