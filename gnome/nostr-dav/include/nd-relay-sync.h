/* nd-relay-sync.h - Inbound relay subscription + fold/upsert
 *
 * SPDX-License-Identifier: MIT
 *
 * Plan Track 2 D4: opens REQ subscriptions to the account's home_relays
 * (per nd-config) with filters `{kinds:[31922,31923,30085,5],
 * authors:[account_pubkey]}`, folds each incoming EVENT into the local
 * SQLite stores (calendar/contact) via the existing parsers, and applies
 * NIP-09 kind-5 tombstones by deleting the matching addressable rows.
 *
 * Reconnect backoff is doubled 60 s -> 60 min per relay. The last
 * ingested `created_at` is persisted per relay in the `relay_cursor`
 * table so a restart resumes without re-processing history.
 *
 * NdRelaySync is intentionally small at the top: nd_relay_sync_start()
 * wires up transports, and nd_relay_sync_ingest_event() is exposed so
 * tests can hand it decoded EVENT envelopes without dragging in a
 * transport.
 */
#ifndef ND_RELAY_SYNC_H
#define ND_RELAY_SYNC_H

#include <glib.h>

#include "nd-config.h"
#include "nd-calendar-store.h"
#include "nd-contact-store.h"
#include "nd-relay-transport.h"
#include "nd-store-db.h"

G_BEGIN_DECLS

#define ND_RELAY_SYNC_ERROR (nd_relay_sync_error_quark())
GQuark nd_relay_sync_error_quark(void);

typedef enum {
  ND_RELAY_SYNC_ERROR_MALFORMED = 1,   /* invalid EVENT envelope */
  ND_RELAY_SYNC_ERROR_UNSUPPORTED,     /* kind we do not fold */
  ND_RELAY_SYNC_ERROR_STORE            /* SQLite failure */
} NdRelaySyncError;

typedef struct _NdRelaySync NdRelaySync;

/**
 * nd_relay_sync_new:
 * @db: (transfer none): store database; the sync layer takes a ref
 * @cal_store: (transfer none): calendar store (kind 31922/31923 fold)
 * @contact_store: (transfer none): contact store (kind 30085 fold)
 * @factory: transport factory; every relay URL is materialised through it
 * @factory_data: opaque pointer passed to @factory on every call
 *
 * Returns: (transfer full): a new sync layer; call nd_relay_sync_start()
 *   to open subscriptions.
 */
NdRelaySync *nd_relay_sync_new(NdStoreDb              *db,
                                NdCalendarStore        *cal_store,
                                NdContactStore         *contact_store,
                                NdRelayTransportFactory factory,
                                gpointer                factory_data);

void nd_relay_sync_free(NdRelaySync *self);

/**
 * nd_relay_sync_configure:
 * @account_pubkey: (nullable): the account's hex pubkey; the REQ filter
 *   uses this as `authors[]`. NULL disables the author filter — v1 lets
 *   the caller drive `#p`-addressed variants only in that case.
 * @home_relays: (nullable): NULL-terminated relay URL list. NULL is a
 *   valid "no relays configured yet" state.
 * @upstream_mode: policy from nd-config (currently informational —
 *   session_relay wiring lands with Track 3 Piece A).
 *
 * May be called before or after nd_relay_sync_start(); reconfiguration
 * disconnects transports whose URL is no longer in the list and starts
 * fresh transports for newly added URLs.
 */
void nd_relay_sync_configure(NdRelaySync    *self,
                             const gchar    *account_pubkey,
                             const GStrv     home_relays,
                             NdUpstreamMode  upstream_mode);

void nd_relay_sync_start(NdRelaySync *self);
void nd_relay_sync_stop (NdRelaySync *self);

/**
 * nd_relay_sync_ingest_event:
 * @relay_url: (nullable): source relay for cursor accounting; NULL skips
 *   cursor persistence (used from fold-only tests)
 * @event_object_json: the inner event object JSON (the third slot of an
 *   `["EVENT","sub-id",{...}]` envelope)
 *
 * Returns: TRUE when the event was folded into the store (or was an
 *   already-known event ignored on purpose); FALSE with @error set for
 *   malformed input or a store failure.
 */
gboolean nd_relay_sync_ingest_event(NdRelaySync *self,
                                    const gchar *relay_url,
                                    const gchar *event_object_json,
                                    GError     **error);

/**
 * nd_relay_sync_handle_envelope:
 * @envelope_json: a complete NIP-01 relay envelope
 *
 * Convenience for tests / the transport listener: parses the envelope,
 * dispatches EVENT frames through nd_relay_sync_ingest_event(), and
 * silently ignores everything else (EOSE/OK/NOTICE — those steer the
 * subscription state machine, not the store).
 *
 * Returns: TRUE if the envelope was recognised and processed without
 *   error; FALSE with @error set otherwise.
 */
gboolean nd_relay_sync_handle_envelope(NdRelaySync *self,
                                       const gchar *relay_url,
                                       const gchar *envelope_json,
                                       GError     **error);

G_END_DECLS
#endif /* ND_RELAY_SYNC_H */
