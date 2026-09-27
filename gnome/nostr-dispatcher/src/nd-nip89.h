/*
 * nd-nip89 — NIP-89 handler *suggestions* for kinds no installed app
 * handles (nostrc-prqu.1).
 *
 * Trust model: a kind-31990 "handler information" event is signed by
 * anybody, so a discovered handler is only ever OFFERED — as a
 * notification button (daemon) or a stderr line (CLI) — and never
 * launched without the user clicking it. Ranking uses the only social
 * signal NIP-89 defines: kind-31989 recommendations for that kind by the
 * user and the people the user follows (kind 3). The offer always shows
 * the web handler's host; unrecommended handlers say so.
 *
 * Discovery (blocking; run in a worker thread):
 *   1. fresh cache ($XDG_CACHE_HOME/nostr-dispatcher/nip89/<kind>.json:
 *      24 h when something was found, 1 h when nothing was);
 *   2. the per-user session relay (local; no identity needed);
 *   3. unless [Dispatcher] fetch-relay-hints=false: the user's NIP-65 READ
 *      relays (kind 10002 found on the session relay), else the relays the
 *      user configured in the signer (org.nostr.Signer.GetRelays; never
 *      fetched from the network). At most ND_NIP89_MAX_RELAYS relays.
 *   The user's pubkey comes from org.nostr.Signer.GetPublicKey with
 *   NO_AUTO_START: discovery never starts a signer; without one it still
 *   searches the session relay, just without read relays or ranking.
 *   [Dispatcher] nip89-discovery=false disables all of it.
 *
 * Web templates: ["web", "https://…<bech32>…", "<entity>"?]. Only https://
 * URLs with a host and no userinfo are used; <bech32> is replaced by the
 * dispatcher's canonical NIP-19 re-encoding of the link. A template whose
 * entity marker names our entity (nevent / naddr / nprofile / npub / note)
 * wins over an unmarked one; other markers never match.
 *
 * Desktop hint: a ["flatpak", "<app-id>"] or ["linux", "<app-id>"] tag
 * with a valid reverse-DNS application id is shown as "also available as
 * a desktop app" with a button opening appstream://<app-id> (GNOME
 * Software) — again only on click.
 */
#ifndef ND_NIP89_H
#define ND_NIP89_H

#include <gio/gio.h>
#include "nd-uri.h"

G_BEGIN_DECLS

#define ND_NIP89_HANDLER_KIND 31990
#define ND_NIP89_RECOMMEND_KIND 31989
#define ND_NIP89_MAX_RELAYS 4
#define ND_NIP89_MAX_FOLLOWS 250
#define ND_NIP89_TTL_FOUND_S (24 * 3600)
#define ND_NIP89_TTL_NONE_S 3600

typedef struct {
  char *template_url;
  char *entity; /* nullable */
} NdNip89Web;

typedef struct {
  char *address;        /* "31990:<pubkey>:<d>" */
  char *pubkey_hex;
  char *name;           /* sanitised display name from the content, or NULL */
  GPtrArray *web;       /* NdNip89Web* */
  char *app_id;         /* flatpak/linux desktop hint, validated, or NULL */
  gint64 created_at;
  guint recommended_by; /* distinct recommenders among self + follows */
  char *event_json;     /* the validated 31990 (for the cache) */
} NdNip89Handler;

void nd_nip89_handler_free(gpointer h);

/* A validated kind-31990 event that declares ["k","<kind>"] and a d tag,
 * or NULL. */
NdNip89Handler *nd_nip89_handler_from_json(const char *event_json, guint32 kind);

/* https URL for @t via @h's best web template, or NULL. */
char *nd_nip89_web_url(const NdNip89Handler *h, const NdTarget *t);

/* Host of an https URL (for display), or NULL. */
char *nd_nip89_url_host(const char *url);

/* Count kind-31989 recommendations (@recs: NdEvent*, validated, d = kind)
 * into ->recommended_by: one per distinct author per "a" address. */
void nd_nip89_apply_recommendations(GPtrArray *handlers, GPtrArray *recs, guint32 kind);

/* Dedupe by address (newest wins), then sort: recommended_by desc, has a
 * web template desc, created_at desc. */
void nd_nip89_rank(GPtrArray *handlers);

/* Filters (NIP-01 JSON objects). */
char *nd_nip89_handlers_filter(guint32 kind);
char *nd_nip89_recommendations_filter(guint32 kind, const char *const *authors);
char *nd_nip89_replaceable_filter(gint kind, const char *author_hex);

/* Kind 10002 → read relays ("r" tags without a marker or marked "read"),
 * wss:// / loopback ws:// only. Kind 3 → followed hex pubkeys (at most
 * @max). Never NULL. */
char **nd_nip89_read_relays(const char *relay_list_json);
char **nd_nip89_follows(const char *contacts_json, guint max);

/* Newest event of @events (NdEvent*) by created_at, or NULL (borrowed). */
const char *nd_nip89_newest_json(GPtrArray *events);

/* Notification text for offering @h for @kind (@url may be NULL). */
char *nd_nip89_offer_body(const NdNip89Handler *h, guint32 kind, const char *url);
char *nd_nip89_offer_button(const NdNip89Handler *h, const char *url);

/* Cache. @dir NULL = $XDG_CACHE_HOME/nostr-dispatcher/nip89. load returns
 * NULL when absent/corrupt; *out_fresh tells whether it is within its TTL
 * at @now. @max_age_s > 0 overrides the TTL (used when acting on an offer
 * the user clicked). */
GPtrArray *nd_nip89_cache_load(const char *dir, guint32 kind, gint64 now,
                               gint64 max_age_s, gboolean *out_fresh);
gboolean nd_nip89_cache_store(const char *dir, guint32 kind, GPtrArray *handlers,
                              gint64 now, GError **error);

/* Offers the user was actually shown. A random 128-bit token names each;
 * the notification buttons carry only the token, so a session-bus peer
 * calling org.freedesktop.Application.ActivateAction cannot open anything
 * that was not presented (cached handlers are attacker-publishable).
 * Stored as key files in @dir (NULL: $XDG_RUNTIME_DIR/nostr-dispatcher/
 * nip89-offers, 0700; files 0600) so a click still works after the
 * service has exited on idle; valid for ND_NIP89_OFFER_TTL_S; single use. */
#define ND_NIP89_OFFER_TTL_S (24 * 3600)
char *nd_nip89_offer_store(const char *dir, guint32 kind, const char *address,
                           const char *uri, const char *app_id, gint64 now, GError **error);
gboolean nd_nip89_offer_take(const char *dir, const char *token, gint64 now, guint32 *kind,
                             char **address, char **uri, char **app_id);

typedef struct {
  const char *socket_path;     /* NULL: $XDG_RUNTIME_DIR/nostr/relay.sock */
  gboolean use_network;        /* fetch-relay-hints */
  const char *cache_dir;       /* NULL: default */
  const char *pubkey_hex;      /* NULL: ask org.nostr.Signer (no auto-start) */
  const char *const *relays;   /* NULL: NIP-65 read relays / signer GetRelays */
  gboolean no_signer;          /* tests: never touch the session bus */
  guint budget_ms;             /* 0: 8000 for everything network */
} NdNip89Options;

/* Ranked NdNip89Handler* (possibly empty; never NULL). Stores the cache. */
GPtrArray *nd_nip89_discover_sync(guint32 kind, const NdNip89Options *opts,
                                  GCancellable *cancellable);

G_END_DECLS

#endif /* ND_NIP89_H */
