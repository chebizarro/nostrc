/**
 * @file neg-client.h
 * @brief NIP-77 Negentropy sync client for range-based event reconciliation
 *
 * Builds local state fingerprints from NostrDB and runs the negentropy
 * protocol (NEG-OPEN/NEG-MSG/NEG-CLOSE) with relays to efficiently detect
 * and resolve event set differences for specific kinds.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NEG_CLIENT_H
#define NEG_CLIENT_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define GNOSTR_NEG_ERROR (gnostr_neg_error_quark())

GQuark gnostr_neg_error_quark(void);

typedef enum {
  GNOSTR_NEG_ERROR_CONNECTION,
  GNOSTR_NEG_ERROR_PROTOCOL,
  GNOSTR_NEG_ERROR_UNSUPPORTED,
  GNOSTR_NEG_ERROR_TIMEOUT,
  GNOSTR_NEG_ERROR_CANCELLED,
  GNOSTR_NEG_ERROR_LOCAL
} GnostrNegErrorCode;

typedef struct {
  guint local_count;    /* Events in local NDB for synced kinds */
  guint rounds;         /* Negentropy protocol rounds completed */
  guint events_fetched; /* Events fetched from relay (0 if in_sync) */
  gboolean in_sync;     /* TRUE if local and remote fingerprints match */
} GnostrNegSyncStats;

/**
 * gnostr_neg_sync_kinds_async:
 * @relay_url: WebSocket URL of the relay to sync with
 * @kinds: (array length=kind_count): event kinds to sync (e.g., 3 for contacts)
 * @kind_count: number of elements in @kinds
 * @cancellable: (nullable): optional cancellable
 * @callback: async callback
 * @user_data: user data for callback
 *
 * Run a negentropy sync session for the specified event kinds.
 * Builds a local fingerprint from NostrDB, opens a relay connection,
 * and runs the NEG-OPEN/NEG-MSG protocol to detect differences.
 *
 * Note: One sync session at a time (V1 limitation).
 */
void gnostr_neg_sync_kinds_async(const char *relay_url,
                                  const int *kinds,
                                  size_t kind_count,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data);

/**
 * gnostr_neg_sync_kinds_finish:
 * @result: async result from callback
 * @stats_out: (out) (optional): sync statistics
 * @error: (out): error location
 *
 * Complete a negentropy sync operation.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_neg_sync_kinds_finish(GAsyncResult *result,
                                       GnostrNegSyncStats *stats_out,
                                       GError **error);

G_END_DECLS

#endif /* NEG_CLIENT_H */
