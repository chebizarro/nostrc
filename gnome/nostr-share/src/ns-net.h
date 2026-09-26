/* ns-net.h - Signer, relay discovery and publishing for nostr-share
 *
 * SPDX-License-Identifier: MIT
 *
 * Every function here is synchronous: it pushes a private GMainContext,
 * drives libnostr-publish transports on it and returns when done. The
 * CLI calls them directly; the dialog calls them from a GTask thread.
 */
#ifndef NS_NET_H
#define NS_NET_H

#include <glib.h>
#include <nostr-publish/nostr-publish.h>

#include "ns-config.h"
#include "ns-event.h"

G_BEGIN_DECLS

#define NS_APP_ID            "org.nostr.Share"
/* The session relay speaks NIP-01 over WebSocket on a Unix socket; this
 * URL is only the HTTP handshake target and the relay's identity in the
 * publisher. */
#define NS_SESSION_RELAY_URL "ws://localhost/"

typedef struct {
  /* Test seam: when set, every transport comes from here. */
  NostrPublishTransportFactory factory;
  gpointer                     factory_data;
  gchar                       *session_socket;   /* nullable */
} NsNet;

void   ns_net_init(NsNet *net);   /* detects the session relay socket */
void   ns_net_clear(NsNet *net);

/* $XDG_RUNTIME_DIR/nostr/relay.sock when it exists and is a socket. */
gchar *ns_session_relay_socket(void);

NostrPublishTransport *ns_net_transport_new(NsNet *net, const gchar *url);

/* Connect to org.nostr.Signer on the session bus. Fails with
 * NS_ERROR_NO_SIGNER (clear message) when no signer is reachable. */
NostrPublishSigner *ns_signer_connect(gchar **out_pubkey_hex, GError **error);

/* Latest verified replaceable event (@kind, @author_hex) across @relays,
 * or NULL (no error) when none of them has one. Events whose id or
 * signature do not verify are ignored. */
gchar *ns_net_fetch_replaceable(NsNet              *net,
                                const gchar *const *relays,
                                gint                kind,
                                const gchar        *author_hex,
                                guint               timeout_ms,
                                GError            **error);

typedef struct {
  gchar   **targets;          /* publish set, in order */
  gchar   **direct;           /* subset: non-session relays */
  gboolean  session_included;
  gchar    *write_source;     /* "kind 10002", "config home_relays", "group relay" */
} NsTargets;

void ns_targets_clear(NsTargets *t);

gboolean ns_resolve_targets(const NsConfig    *cfg,
                            NsNet             *net,
                            const gchar       *pubkey_hex,
                            const NsRecipient *to,
                            NsTargets         *out,
                            GError           **error);

/* Blossom servers: kind 10063 (BUD-03) first, config fallback. */
gchar **ns_resolve_blossom_servers(const NsConfig *cfg,
                                   NsNet          *net,
                                   const gchar    *pubkey_hex,
                                   gchar         **out_source,
                                   GError        **error);

typedef struct {
  guint      n_targets;
  guint      n_accepted;
  guint      n_direct_accepted;
  gboolean   session_accepted;
  GPtrArray *lines;          /* "url: accepted" / "url: rejected (reason)" */
} NsPublishReport;

void ns_publish_report_clear(NsPublishReport *r);

/* Publish an already-signed event. Returns TRUE when the upstream mode's
 * success rule is met (see ns-config.h), FALSE with NS_ERROR_PUBLISH
 * otherwise; @report is filled either way. */
gboolean ns_net_publish(NsNet            *net,
                        const NsConfig   *cfg,
                        const gchar      *signed_json,
                        const NsTargets  *targets,
                        NsPublishReport  *report,
                        GError          **error);

G_END_DECLS

#endif /* NS_NET_H */
