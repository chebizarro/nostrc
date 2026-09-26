/* nss-net.h — fetch the user's replaceable lists and publish new ones.
 * SPDX-License-Identifier: MIT
 *
 * Synchronous: each call pushes a private GMainContext, drives
 * libnostr-publish transports on it and returns when done — the UI calls
 * these from a GTask worker thread. Tests inject fixture transports and a
 * vtable signer through NssNet.
 */
#ifndef NSS_NET_H
#define NSS_NET_H

#include <glib.h>
#include <nostr-publish/nostr-publish.h>

G_BEGIN_DECLS

#define NSS_APP_ID "org.nostr.Settings"
/* The session relay speaks WebSocket over $XDG_RUNTIME_DIR/nostr/relay.sock;
 * this URL is only the HTTP handshake target (same as nostr-share). */
#define NSS_SESSION_RELAY_URL "ws://localhost/"

#define NSS_NET_ERROR (nss_net_error_quark())
GQuark nss_net_error_quark(void);
typedef enum {
  NSS_NET_ERROR_NO_SIGNER = 1,
  NSS_NET_ERROR_NO_RELAYS,
  NSS_NET_ERROR_PUBLISH,
} NssNetError;

typedef struct {
  NostrPublishTransportFactory factory;   /* test seam; NULL = real sockets */
  gpointer                     factory_data;
  gchar                       *session_socket;  /* NULL when absent */
} NssNet;

void   nss_net_init(NssNet *net);   /* detects relay.sock */
void   nss_net_clear(NssNet *net);

/* Newest verified (id + signature) event of @kind by @author_hex across
 * @relays (the session relay URL is served over relay.sock), or NULL when
 * none has one. @out_source (nullable) receives the relay it came from. */
gchar *nss_net_fetch_replaceable(NssNet *net, const gchar *const *relays, gint kind,
                                 const gchar *author_hex, guint timeout_ms,
                                 gchar **out_source);

typedef struct {
  gchar   *url;
  gboolean accepted;
  gchar   *detail;   /* relay reason / "unreachable" / "timed out" */
} NssRelayResult;

void nss_relay_result_free(NssRelayResult *r);

typedef struct {
  gchar     *signed_json;
  GPtrArray *results;     /* NssRelayResult*, one per target */
  guint      n_required;  /* relays in @required */
  guint      n_required_ok;
} NssPublishReport;

void nss_publish_report_clear(NssPublishReport *r);

/* Sign @unsigned_json with @signer and publish to @targets. Succeeds only
 * when every URL in @required (a subset of @targets — the user's write
 * relays) accepted: a relay list that only some of your write relays hold
 * resolves differently depending on where a client looks (NIP-65 outbox).
 * @report is filled either way (on failure too) when signing succeeded. */
gboolean nss_net_publish(NssNet *net, NostrPublishSigner *signer,
                         const gchar *unsigned_json, const gchar *const *targets,
                         const gchar *const *required, guint ok_wait_sec,
                         NssPublishReport *report, GError **error);

/* Session-bus signer + the active identity's hex pubkey. */
NostrPublishSigner *nss_signer_connect(GDBusConnection *bus, gchar **out_pubkey_hex,
                                       GError **error);

G_END_DECLS

#endif /* NSS_NET_H */
