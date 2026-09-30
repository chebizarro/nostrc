#ifndef GH_RELAY_SCOPE_H
#define GH_RELAY_SCOPE_H

#include <glib.h>
#include <nostr-filter.h>

#include "gh-relay-auth.h"

G_BEGIN_DECLS

/*
 * NIP-42 in a scope: AUTH identity is chosen per URL by the caller
 * (gh_relay_scope_set_url_auth()); see gh-relay-auth.h for the policy.
 *
 *  - NONE, the default for every URL: nothing is signed. An AUTH challenge
 *    is reported as GH_RELAY_NOTICE_AUTH and a CLOSED "auth-required:" as
 *    GH_RELAY_NOTICE_CLOSED, exactly as without AUTH support.
 *  - EPHEMERAL: a throwaway key, new for each connection's AUTH and wiped
 *    after signing. For URLs that must not learn the account (discovery,
 *    other people's relays, MLS routing).
 *  - ACCOUNT: the scope's account signer (gh_relay_scope_set_account_signer(),
 *    bound to the scope's generation). Only for the account's own inbox and
 *    list relays and NIP-29 group relays; never for someone else's inbox.
 *
 * For a URL with EPHEMERAL or ACCOUNT, a CLOSED "auth-required:" on a
 * connection that has sent a challenge is held back while Groundhog signs,
 * verifies and sends one AUTH for that challenge; on the relay's OK true
 * for the AUTH event the REQ is re-issued once on the same connection (a
 * fresh EOSE boundary follows). If signing is refused, the signed event
 * fails local verification, the relay answers the AUTH with OK false, or the
 * retried REQ is refused again for the same challenge, the held CLOSED is
 * reported with the relay's reason; a refused EPHEMERAL AUTH is never
 * retried as the account. A CLOSED that arrives before any challenge is
 * reported at once, and a challenge that arrives later still triggers the
 * single authenticated retry. OKs for Groundhog's own AUTH events are never
 * reported. A lost connection, cancellation, signer revocation or a change
 * of the URL's identity choice drops a pending AUTH; nothing is ever signed
 * as the account for a stale generation.
 */

/*
 * Overflow (nostrc-5rfp). The GNostrRelay transport's subscriptions deliver
 * every event, up to a hard backlog ceiling far above any legitimate
 * backfill (GNostrSubscription:max-backlog-events). A relay that outpaces
 * the main loop past it ends that REQ with a CLOSED whose detail starts
 * with GH_RELAY_CLOSED_OVERFLOW_PREFIX, after every event received before
 * it. The scope reports that CLOSED like any other (the URL's backfill is
 * incomplete: no EOSE follows for that REQ), then re-issues the REQ once on
 * the same connection, from an idle, when the transport can resubscribe:
 * the answer starts again from the caller's filters (its durable cursor),
 * with a fresh EOSE boundary. A second overflow on the same connection is
 * only reported; the next connection may retry once again.
 */
#define GH_RELAY_CLOSED_OVERFLOW_PREFIX "overflow:"
#define GH_RELAY_SCOPE_OVERFLOW_RETRIES 1

typedef struct _GhRelayScope GhRelayScope;

typedef enum {
  GH_RELAY_NOTICE_EVENT,
  GH_RELAY_NOTICE_EOSE,
  GH_RELAY_NOTICE_CLOSED,
  GH_RELAY_NOTICE_AUTH,
  GH_RELAY_NOTICE_OK,
  GH_RELAY_NOTICE_DISCONNECTED,
  GH_RELAY_NOTICE_ERROR
} GhRelayNotice;

typedef struct {
  GhRelayNotice notice;
  const gchar *url;
  const gchar *event_json; /* EVENT only; valid only during callback */
  const gchar *event_id;   /* EVENT or OK; valid only during callback */
  const gchar *detail;     /* CLOSED, AUTH, OK or ERROR */
  gboolean accepted;       /* relay-local OK, never upstream delivery */
  gboolean backfill;       /* EVENT received before this URL's EOSE */
} GhRelayUpdate;

typedef void (*GhRelayScopeFunc)(GhRelayScope *scope,
                                 const GhRelayUpdate *update,
                                 gpointer user_data);

/* Internal transport seam. open must issue a REQ only to url; close must stop
 * callbacks before returning. A scope never hands it URLs outside its set. */
typedef struct {
  gpointer (*open)(GhRelayScope *scope, const gchar *url,
                   const NostrFilters *filters, gpointer user_data,
                   GError **error);
  void (*close)(gpointer handle, gpointer user_data);
} GhRelayTransport;

/* Optional NIP-42 half of the transport seam, on the same handles and
 * user_data. send_auth writes ["AUTH",signed_event_json] on the handle's
 * current connection and reports a later write failure as
 * GH_RELAY_NOTICE_ERROR; resubscribe replaces the handle's REQ with a new one
 * on the same connection. gh_relay_scope_new() installs the GNostrRelay one;
 * a scope with neither never authenticates. */
typedef struct {
  gboolean (*send_auth)(gpointer handle, const gchar *signed_event_json,
                        gpointer user_data, GError **error);
  void (*resubscribe)(gpointer handle, gpointer user_data);
} GhRelayAuthTransport;

GhRelayScope *gh_relay_scope_new(guint64 account_generation,
                                 NostrFilters *filters,
                                 GhRelayScopeFunc callback,
                                 gpointer user_data);
GhRelayScope *gh_relay_scope_new_with_transport(guint64 account_generation,
                                                NostrFilters *filters,
                                                const GhRelayTransport *transport,
                                                gpointer transport_data,
                                                GhRelayScopeFunc callback,
                                                gpointer user_data);
GhRelayScope *gh_relay_scope_ref(GhRelayScope *scope);
void gh_relay_scope_unref(GhRelayScope *scope);
/* Whether a CLOSED detail reports a subscription ended by its backlog
 * ceiling (see "Overflow" above), not by the relay. */
gboolean gh_relay_closed_is_overflow(const gchar *detail);
/* The relay URL rule shared by scopes and publishes: ws or wss, a non-empty
 * host, no userinfo. Sets G_IO_ERROR_INVALID_ARGUMENT otherwise. */
gboolean gh_relay_url_validate(const gchar *url, GError **error);
/* At most 16 distinct ws(s) URLs. A URL added after start receives its own
 * live REQ and independent EOSE boundary. */
gboolean gh_relay_scope_add_url(GhRelayScope *scope, const gchar *url,
                                GError **error);
void gh_relay_scope_start(GhRelayScope *scope);
/* Revoke generation before closing transports; no later callback is admitted. */
void gh_relay_scope_cancel(GhRelayScope *scope);
guint64 gh_relay_scope_get_generation(const GhRelayScope *scope);
/* The account signer used by ACCOUNT URLs only. Before start. Refuses
 * (G_IO_ERROR_PERMISSION_DENIED) a signer bound to another account
 * generation or already revoked; NULL clears (refused while a URL is set to
 * ACCOUNT). Setting it enables account AUTH on no URL by itself. */
gboolean gh_relay_scope_set_account_signer(GhRelayScope *scope,
                                           GhRelayAuthSigner *signer,
                                           GError **error);
/* The AUTH identity for one URL already added to the scope; may change at
 * any time, dropping an AUTH in flight. ACCOUNT requires the account signer
 * (G_IO_ERROR_PERMISSION_DENIED); an unknown URL is G_IO_ERROR_NOT_FOUND. */
gboolean gh_relay_scope_set_url_auth(GhRelayScope *scope, const gchar *url,
                                     GhRelayAuthMode mode, GError **error);
/* Internal, before start; both functions or NULL. */
void gh_relay_scope_set_auth_transport(GhRelayScope *scope,
                                       const GhRelayAuthTransport *auth);

/* The GNostrRelay (libnostr/libwebsockets) transport: direct connections
 * only, with local DNS. */
extern const GhRelayTransport gh_relay_gnostr_transport;
extern const GhRelayAuthTransport gh_relay_gnostr_auth_transport;

/* The transport gh_relay_scope_new() gives every scope. Until an app-wide
 * default is installed it is the GNostrRelay one; the network session
 * (src/net/gh-relay-net.h, G09) installs its network-mode dispatcher, which
 * sends Tor mode through SOCKS5 and never falls back to a direct connection.
 * transport NULL restores GNostrRelay. auth is optional. Install before any
 * scope is made; thread-safe. */
void gh_relay_scope_set_default_transport(const GhRelayTransport *transport,
                                          const GhRelayAuthTransport *auth,
                                          gpointer transport_data);

/* Tor stream isolation (privacy charter §4.3, PD-6, NT-6): a label shared by
 * all of this scope's connections, and only by them. A transport that
 * proxies derives its SOCKS credentials from it and the generation, so each
 * scope gets its own Tor circuits while its reconnects keep them. The
 * default is a random label made with the scope; a caller may set a stable
 * one (e.g. "inbox") before start. Never a URL or key: it only needs to be
 * distinct. */
void gh_relay_scope_set_isolation(GhRelayScope *scope, const gchar *isolation);
const gchar *gh_relay_scope_get_isolation(const GhRelayScope *scope);

/* Transport-to-scope delivery. Unknown URLs and cancelled generations are
 * discarded. EVENT validates signed NIP-01 JSON and deduplicates 4096 IDs. */
void gh_relay_scope_event(GhRelayScope *scope, const gchar *url,
                          const gchar *event_json);
void gh_relay_scope_eose(GhRelayScope *scope, const gchar *url);
void gh_relay_scope_notice(GhRelayScope *scope, const gchar *url,
                           GhRelayNotice notice, const gchar *event_id,
                           gboolean accepted, const gchar *detail);
/* A NIP-42 challenge on url's current connection: reported as
 * GH_RELAY_NOTICE_AUTH and remembered for a later auth-required CLOSED. A
 * DISCONNECTED notice forgets it. */
void gh_relay_scope_auth_challenge(GhRelayScope *scope, const gchar *url,
                                   const gchar *challenge);

G_END_DECLS
#endif
