/*
 * session_federation — upstream federation client of nostr-session-relayd
 * (bead nostrc-7d96): store-and-forward of events local apps write to
 * relay.sock, to the relays the user's own data names (see
 * session_fed_policy.h for the contract, session_outbox.h for the
 * durability model).
 *
 * Threading. The relay's libwebsockets loop owns the main thread; the
 * engine runs on its own thread with a private GMainContext. The only
 * calls made from other threads are nsr_federation_offer()/_retract()
 * (relay loop, through the storage tee), nsr_federation_wake(), and the
 * read-only status accessors (D-Bus thread). Observer callbacks run on the
 * engine thread.
 *
 * Outbound engine. libnostr-publish's NostrPublishTransport (libsoup-3
 * WebSocket, NIP-01 envelope parsing), NostrPublishSigner
 * (org.nostr.Signer; the relay holds no keys) and its OK / NIP-65 helpers.
 * NostrPublisher's request-level aggregation is deliberately not used: the
 * outbox tracks each (event, relay) pair durably and retries relays
 * independently, a permanent rejection by one relay must not abandon the
 * others, and `auth-required:` is answered with NIP-42 AUTH and a resend
 * rather than treated as final.
 */
#ifndef NSR_SESSION_FEDERATION_H
#define NSR_SESSION_FEDERATION_H

#include <glib.h>

#include <nostr-publish/nostr-publish-signer.h>

#include "nostr-event.h"
#include "nostr-storage.h"
#include "session_fed_policy.h"
#include "session_outbox.h"

G_BEGIN_DECLS

#define NSR_FED_DEFAULT_APP_ID "nostr-session-relay"

typedef struct NsrFederation NsrFederation;

typedef struct {
  const NsrFedConfig *cfg;     /* copied */
  NsrOutbox *outbox;           /* borrowed; must outlive the engine */
  /* Borrowed, nullable: fallback lookups of relay-list events stored before
   * the outbox existed. Read from the engine thread (nostrdb read
   * transactions are thread-safe). */
  NostrStorage *storage;
  /* Nullable (ref taken): signs NIP-42 AUTH. When NULL and @dbus_signer,
   * the engine connects to org.nostr.Signer on the session bus lazily. */
  NostrPublishSigner *signer;
  /* Use org.nostr.Signer: AUTH signing (if @signer is NULL) and
   * GetPublicKey to learn the local account when cfg has no
   * federation_accounts. */
  gboolean dbus_signer;
  const char *app_id;          /* signer app id; NULL = NSR_FED_DEFAULT_APP_ID */
} NsrFederationInit;

/* Per-target progress, engine thread. @relay is "" for event-level
 * transitions (skipped / unroutable / failed before any target existed). */
typedef void (*NsrFedObserver)(const char *event_id, const char *relay,
                               const char *target_state, const char *reason,
                               const char *event_state, void *user_data);

NsrFederation *nsr_federation_new(const NsrFederationInit *init);
/* Before nsr_federation_start(). */
void nsr_federation_set_observer(NsrFederation *fed, NsrFedObserver cb, void *user_data);
gboolean nsr_federation_start(NsrFederation *fed, GError **error);
/* Stop the engine thread: live connections are dropped, unfinished
 * attempts stay pending in the outbox (due again at once on the next
 * start). No observer call happens after it returns; the status accessors
 * stay usable until nsr_federation_free(). Idempotent. */
void nsr_federation_stop(NsrFederation *fed);
/* nsr_federation_stop() + free. NULL-safe. */
void nsr_federation_free(NsrFederation *fed);

/* Storage-tee hook (relay loop thread), called after the local store
 * accepted @ev: durably queue it if the contract allows forwarding it, and
 * remember relay-list kinds for routing. 0 on success or when nothing had
 * to be written, -1 when the outbox write failed (the tee then fails the
 * EVENT so the client retries). */
int nsr_federation_offer(NsrFederation *fed, NostrEvent *ev);
void nsr_federation_wake(NsrFederation *fed);

typedef struct {
  const char *state;  /* "active" | "waiting-for-account" (static) */
  char *detail;       /* g_free */
  NsrOutboxStats outbox;
  guint relays_connected;
} NsrFedStatus;
void nsr_federation_status(NsrFederation *fed, NsrFedStatus *out);
void nsr_federation_status_clear(NsrFedStatus *s);

typedef struct {
  char *url;
  gboolean connected;     /* either lane */
  gboolean authed;        /* identified lane completed NIP-42 AUTH */
  guint pending;
  guint64 acked, failed;  /* rows still in the outbox */
  char *last_error;       /* "" if none */
  gint64 last_ok_at;      /* unix s of the last accepted OK, 0 = never */
} NsrFedRelayInfo;
void nsr_fed_relay_info_free(gpointer p);
/* Known upstream relays (outbox rows ∪ live connections), sorted by URL. */
GPtrArray *nsr_federation_relays(NsrFederation *fed);

NsrOutbox *nsr_federation_get_outbox(NsrFederation *fed);

G_END_DECLS

#endif /* NSR_SESSION_FEDERATION_H */
