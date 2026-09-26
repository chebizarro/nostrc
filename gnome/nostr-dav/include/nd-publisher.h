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
 * The worker itself is deterministic and single-threaded for testing:
 * nd_publisher_tick() picks up any pending rows whose `publish_next_ts`
 * is <= now, drives them one step, and returns. Production wraps it in
 * a GSource on the main context. Tests drive tick() directly.
 */
#ifndef ND_PUBLISHER_H
#define ND_PUBLISHER_H

#include <glib.h>

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

/** Configures the target set (v1: reuses home_relays) and the quorum. */
void nd_publisher_configure(NdPublisher      *self,
                            const gchar      *account_pubkey,
                            const GStrv       home_relays,
                            NdPublishQuorum   quorum);

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

/** Test-only: bind an already-constructed transport to a URL so tick()
 *  will use it instead of asking the factory. Ownership retained by
 *  caller. Passing @transport = NULL removes the binding. */
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
