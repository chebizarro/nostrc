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
  /* The session relay's org.nostr.SessionRelay1.FederationState, probed
   * once on first use (UNKNOWN until then). */
  NostrPublishFederation       federation;
  /* Session bus, connected once on first use (one connection per share,
   * not one per call). */
  GDBusConnection             *bus;
  gboolean                     bus_tried;
} NsNet;

void   ns_net_init(NsNet *net);   /* detects the session relay socket */
void   ns_net_clear(NsNet *net);

/* $XDG_RUNTIME_DIR/nostr/relay.sock when it exists and is a socket. */
gchar *ns_session_relay_socket(void);

/* Whether the session relay forwards what it is given to the user's
 * upstream relays (nostrc-t24q): its FederationState on the session bus.
 * NOT_RUNNING without a socket or a session bus; a relay whose socket
 * exists but whose bus name has no owner is started (socket activation)
 * and asked again. Blocks for at most a few seconds; cached in @net. */
NostrPublishFederation ns_net_session_federation(NsNet *net);

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
  gchar   **write;            /* the user's write relays (NIP-34 `relays`
                               * tag); NULL when routed via the session
                               * relay and not asked for */
  gboolean  session_included;
  /* targets is just the session relay and it forwards upstream: the
   * verdict is the upstream delivery, not the local OK (nostrc-t24q). */
  gboolean  session_upstream;
  gchar    *write_source;     /* "kind 10002", "config home_relays", "group relay" */
  gchar    *session_note;     /* why the session relay is (not) used; nullable */
} NsTargets;

void ns_targets_clear(NsTargets *t);

/* @need_write: also resolve NsTargets.write when routing through the
 * session relay (a NIP-34 announcement lists the user's relays). */
gboolean ns_resolve_targets(const NsConfig    *cfg,
                            NsNet             *net,
                            const gchar       *pubkey_hex,
                            const NsRecipient *to,
                            gboolean           need_write,
                            NsTargets         *out,
                            GError           **error);

/* NIP-17 inbox of @pubkey_hex (nostrc-k95e): its verified kind-10050
 * event from the discovery relays, else from @pubkey_hex's own NIP-65
 * write relays (not under session_relay_only, which asks the session
 * relay only). FALSE (no error) when there is none: NIP-17 says not to
 * send then. @out_event_json is the event itself, for the session relay's
 * router. */
gboolean ns_resolve_inbox(const NsConfig *cfg,
                          NsNet          *net,
                          const gchar    *pubkey_hex,
                          gchar        ***out_relays,
                          gchar         **out_event_json,
                          gchar         **out_source);

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
  /* Routed through a forwarding session relay (NsTargets.session_upstream):
   * n_targets / n_accepted count the upstream relays it delivered to. */
  gboolean                 upstream;
  NostrPublishForwardState upstream_state;
} NsPublishReport;

void ns_publish_report_clear(NsPublishReport *r);

/* Publish an already-signed event. Returns TRUE when the upstream mode's
 * success rule is met (see ns-config.h). Otherwise FALSE with
 * NS_ERROR_PUBLISH, or NS_ERROR_QUEUED when a forwarding session relay
 * holds the event but has not confirmed delivery within ok_wait_sec (it
 * keeps delivering in the background). @report is filled either way. */
gboolean ns_net_publish(NsNet            *net,
                        const NsConfig   *cfg,
                        const gchar      *signed_json,
                        const NsTargets  *targets,
                        NsPublishReport  *report,
                        GError          **error);

G_END_DECLS

#endif /* NS_NET_H */
