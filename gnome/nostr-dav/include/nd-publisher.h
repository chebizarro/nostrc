/* nd-publisher.h - Outbox worker for nostr-dav DAV writes
 *
 * SPDX-License-Identifier: MIT
 *
 * Plan Track 2 D5, revised per maintainer Q4 (NIP-51/65 outbox model):
 * the publisher drains the `publish_state='pending'` rows out of the
 * SQLite outbox, calls the Track-1 signer to obtain a signed event
 * JSON, publishes it to every relay in the resolved target set, and
 * transitions the row to `published` only after ALL of them have
 * ACK'd. Anything less keeps the row `pending` with exponential
 * backoff (60 s -> 60 min).
 *
 * Failure classes (mapped to NdSignerError + relay OK reasons):
 *   * DENIED / MALFORMED / permanent relay reject
 *       -> `publish_state='failed_permanent'`, fires a single deduped
 *          %ND_PUBLISHER_NOTIFICATION_FAILED_PERMANENT.
 *   * Transient (signer down, relay disconnected, socket EAGAIN)
 *       -> row stays `pending`, publish_attempts incremented, retries
 *          via nd_publisher_tick() on the next scheduled poll.
 *
 * Since bead nostrc-tmsc the relay side (EVENT fan-out, OK
 * classification, quorum verdict, OK-wait deadline, backoff curve) is
 * libnostr-publish's NostrPublisher; this module keeps the SQLite outbox,
 * tombstones, publish_log, DAV event builders and notifications.
 *
 * Through a forwarding session relay (nostrc-t24q) the relay's OK only
 * means "held locally and queued upstream": the row stays `pending`,
 * records the event id in `upstream_event_id`, and settles on the
 * relay's upstream report (nd_publisher_record_upstream()).
 *
 * The worker itself is deterministic and single-threaded for testing:
 * nd_publisher_tick() picks up any pending rows whose `publish_next_ts`
 * is <= now, drives them one step, and returns. Production wraps it in
 * a GSource on the main context. Tests drive tick() directly.
 */
#ifndef ND_PUBLISHER_H
#define ND_PUBLISHER_H

#include <glib.h>
#include <nostr-publish/nostr-publish-session-relay.h>

#include "nd-config.h"
#include "nd-relay-transport.h"
#include "nd-signer.h"
#include "nd-store-db.h"

G_BEGIN_DECLS

#define ND_PUBLISHER_ERROR (nd_publisher_error_quark())
GQuark nd_publisher_error_quark(void);

typedef enum {
  ND_PUBLISHER_ERROR_SIGNER = 1,
  ND_PUBLISHER_ERROR_TRANSPORT,
  ND_PUBLISHER_ERROR_STORE
} NdPublisherError;

typedef struct _NdPublisher NdPublisher;

/**
 * NdPublisherNotifyKind:
 * @ND_PUBLISHER_NOTIFICATION_FAILED_PERMANENT: fired at most once per
 *   outbox row whose publish transitions to `failed_permanent`. The
 *   receiver is expected to deduplicate on @row_id inside its own
 *   deliver-once bookkeeping.
 */
typedef enum {
  ND_PUBLISHER_NOTIFICATION_FAILED_PERMANENT
} NdPublisherNotifyKind;

typedef void (*NdPublisherNotifyCallback)(NdPublisher          *self,
                                          NdPublisherNotifyKind kind,
                                          NdStoreCollection     collection,
                                          const gchar          *row_id,
                                          const gchar          *reason,
                                          gpointer              user_data);

/**
 * nd_publisher_new:
 * @db: (transfer none): store database; the publisher takes a ref
 * @signer: (transfer none): signer for outbound events; the publisher
 *   takes a ref
 * @factory: transport factory used to reach relays in the target set;
 *   the same factory used by NdRelaySync is expected here so a single
 *   transport instance per URL is shared. May be NULL only in tests
 *   that stage rows and never call nd_publisher_tick().
 * @factory_data: opaque pointer passed to @factory
 */
NdPublisher *nd_publisher_new(NdStoreDb              *db,
                              NdSigner               *signer,
                              NdRelayTransportFactory factory,
                              gpointer                factory_data);

void nd_publisher_free(NdPublisher *self);

/** Configures the home relays and the quorum. The actual target set is
 *  home_relays filtered by the upstream mode (nd_publisher_set_upstream()). */
void nd_publisher_configure(NdPublisher      *self,
                            const gchar      *account_pubkey,
                            const GStrv       home_relays,
                            NdPublishQuorum   quorum);

/**
 * nd_publisher_set_upstream:
 * @mode: nostr_dav_upstream_mode (default SESSION_RELAY_OR_DIRECT)
 * @session_relay_url: (nullable): ND_SESSION_RELAY_URL when the session
 *   relay socket exists *and the relay forwards upstream*
 *   (FederationState active / waiting-for-account, nostrc-t24q), else
 *   NULL. A relay that does not forward must not be passed: its OK would
 *   keep the event on this machine.
 *
 * Enforces the upstream mode on publishes (nostrc-862u) through
 * nostr_publish_policy_select_targets(). SESSION_RELAY_ONLY never
 * publishes to home relays — not even for rows staged earlier under a
 * wider mode — and with no session relay the publisher is *held*: rows
 * stay pending locally, nd_publisher_tick() dispatches nothing and no
 * failure is reported, until a later call supplies the session relay.
 * A row staged for the session relay is never sent to it while no
 * (forwarding) session relay is supplied: it goes to the current targets.
 */
void nd_publisher_set_upstream(NdPublisher    *self,
                               NdUpstreamMode  mode,
                               const gchar    *session_relay_url);

/** TRUE while held (see nd_publisher_set_upstream()). */
gboolean nd_publisher_is_held(NdPublisher *self);

/* ---- Upstream delivery through the session relay (nostrc-t24q) ---- */

/** Asks the session relay for @event_id's upstream state
 *  (GetEventUpstream); the answer comes back through
 *  nd_publisher_record_upstream(). */
typedef void (*NdPublisherUpstreamQueryFunc)(NdPublisher *self,
                                             const gchar *event_id,
                                             gpointer     user_data);

void nd_publisher_set_upstream_query_func(NdPublisher                 *self,
                                          NdPublisherUpstreamQueryFunc func,
                                          gpointer                     user_data);

/**
 * nd_publisher_record_upstream:
 * @update: an UpstreamStatusChanged signal or GetEventUpstream reply
 *
 * Settles the row awaiting @update->event_id: forwarded / partial ->
 * `published`; failed / skipped / unknown (never queued, or pruned) ->
 * `failed_permanent` with one notification; superseded / cancelled ->
 * `superseded` (tombstones: `published`); new / pending / unroutable
 * leave it waiting. Per-relay transitions are written to publish_log.
 * Updates for ids no row awaits are ignored.
 */
void nd_publisher_record_upstream(NdPublisher                     *self,
                                  const NostrPublishForwardUpdate *update);

/** Queries (through the query function) every row awaiting an upstream
 *  verdict: after start-up or when the session relay (re)appears, since
 *  signals sent while nobody listened are lost. */
void nd_publisher_resync_upstream(NdPublisher *self);

/** Returns every row awaiting an upstream verdict to plain `pending`, to
 *  be published to the current targets: the session relay stopped
 *  forwarding and the mode allows direct publishing. The signed event is
 *  unchanged, so a late upstream copy is a harmless duplicate.
 *  Returns: the number of rows released. */
guint nd_publisher_release_upstream(NdPublisher *self);

void nd_publisher_set_notify_callback(NdPublisher              *self,
                                      NdPublisherNotifyCallback cb,
                                      gpointer                  user_data);

/**
 * nd_publisher_stage_calendar_put:
 *
 * Called by the DAV PUT handler after the local write has been staged
 * in the calendar store. Prepares an outbox row (publish_state=pending,
 * publish_next_ts=now) so the next tick will pick it up.
 *
 * The row's target set is the caller-supplied one (from config today,
 * from NIP-65 discovery when that lands). It is serialised as a JSON
 * array in the outbox so a later NIP-65 change does not invalidate a
 * pending retry.
 */
gboolean nd_publisher_stage_calendar_put(NdPublisher     *self,
                                          const gchar     *uid,
                                          GError         **error);

gboolean nd_publisher_stage_contact_put (NdPublisher     *self,
                                          const gchar     *uid,
                                          GError         **error);

/**
 * nd_publisher_stage_tombstone:
 * @self: the publisher
 * @target_kind: kind of the addressable event being deleted (e.g. 31922
 *   for a NIP-52 date-based event, 30085 for a contact, 1063 for a file)
 * @target_pubkey_hex: 64-hex x-only pubkey of the addressable event's
 *   author — the account the publisher is configured for. NULL falls
 *   back to the publisher's configured account_pubkey.
 * @target_uid: the addressable event's `d`-tag value (its UID / path)
 * @error: (out) (optional): location for error
 *
 * Called by the DAV DELETE handlers after the local row has been
 * removed. Inserts a row in the `tombstones` outbox; the next
 * nd_publisher_tick() will build a NIP-09 kind-5 event with an
 * `[\"a\", \"<kind>:<pubkey>:<uid>\"]` tag, sign it via the configured
 * signer, and publish it to the publisher's home relay set.
 *
 * Returns: TRUE on success; FALSE with @error set on a SQLite failure.
 */
gboolean nd_publisher_stage_tombstone(NdPublisher  *self,
                                      int           target_kind,
                                      const gchar  *target_pubkey_hex,
                                      const gchar  *target_uid,
                                      GError      **error);

/**
 * nd_publisher_tick:
 * @now_ts: wall-clock time in unix seconds; tests pass a synthetic
 *   value to drive backoff deterministically
 *
 * Drains every eligible pending row once. Return value is TRUE if the
 * outbox has more work to do (either pending rows waiting on backoff or
 * work left after a tick), FALSE if it is empty.
 */
gboolean nd_publisher_tick(NdPublisher *self, gint64 now_ts);

/** Shares a transport (owned by the sync layer, or a test) for a URL so
 *  tick() can send to it. The publisher takes its own ref; the caller
 *  keeps routing the transport's OK frames into nd_publisher_record_ok().
 *  Passing @transport = NULL removes the binding. */
void nd_publisher_bind_transport(NdPublisher      *self,
                                 const gchar      *relay_url,
                                 NdRelayTransport *transport);

/**
 * nd_publisher_record_ok:
 *
 * Feed an OK envelope in for a given relay. Real transports invoke
 * this from their listener; tests drive it directly. The publisher
 * matches @event_id + @relay_url against pending rows and advances
 * their per-relay ACK state.
 */
void nd_publisher_record_ok(NdPublisher *self,
                            const gchar *relay_url,
                            const gchar *event_id,
                            gboolean     ok,
                            const gchar *reason);

G_END_DECLS
#endif /* ND_PUBLISHER_H */
