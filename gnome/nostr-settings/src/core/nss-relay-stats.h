/* nss-relay-stats.h — client side of org.nostr.SessionRelay1
 * (gnome/dbus/org.nostr.SessionRelay1.xml) and the strings the Relays
 * page shows for it.
 * SPDX-License-Identifier: MIT
 *
 * The name exists only while the daemon process runs (socket activation:
 * a listening-but-idle relay has no owner). Callers watch the name
 * (g_bus_watch_name, never auto-starting) and only poll while it is owned.
 */
#ifndef NSS_RELAY_STATS_H
#define NSS_RELAY_STATS_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define NSS_RELAY_BUS_NAME "org.nostr.SessionRelay1"
#define NSS_RELAY_OBJ_PATH "/org/nostr/SessionRelay1"
#define NSS_RELAY_IFACE    "org.nostr.SessionRelay1"

typedef struct {
  gint64   event_count;        /* -1 unknown */
  guint64  storage_bytes;
  guint32  connected_clients;
  guint64  uptime;
  gchar   *storage_backend;    /* "nostrdb" | "none" */
  gchar   *requested_backend;
  gchar   *storage_path;
  GArray  *supported_nips;     /* guint32 */
  gboolean retention_supported;
  guint64  connections_total;
  guint32  subscriptions;
  gchar   *version;
} NssRelayStats;

void     nss_relay_stats_clear(NssRelayStats *s);
/* Parse GetStats' a{sv}; missing keys keep neutral defaults, unknown keys
 * are ignored (additive evolution). */
void     nss_relay_stats_from_variant(NssRelayStats *out, GVariant *dict);
gboolean nss_relay_stats_has_storage(const NssRelayStats *s);

/* Sync GetStats with NO_AUTO_START. FALSE + G_DBUS_ERROR_SERVICE_UNKNOWN /
 * NAME_HAS_NO_OWNER when the relay is not running. */
gboolean nss_relay_stats_fetch(GDBusConnection *bus, NssRelayStats *out, GError **error);
void     nss_relay_stats_fetch_async(GDBusConnection *bus, GCancellable *c,
                                     GAsyncReadyCallback cb, gpointer user_data);
gboolean nss_relay_stats_fetch_finish(GAsyncResult *res, NssRelayStats *out, GError **error);

/* ── Presentation (pure) ───────────────────────────────────────────────── */
/* "nostrdb" / "No storage — this relay keeps nothing (nostrdb not built in)" */
gchar *nss_format_storage_title(const NssRelayStats *s);
/* "12.3 MB on disk · 3,456 events" / "12.3 MB on disk · event count unavailable" */
gchar *nss_format_storage_detail(const NssRelayStats *s);
/* "3 h 12 min", "45 s", "2 d 4 h" */
gchar *nss_format_uptime(guint64 seconds);
gchar *nss_format_nips(const GArray *nips);   /* guint32 array → "1, 11, 42" */
/* Disk usage (like du) of @path; 0 when missing. Used when the relay is idle. */
guint64 nss_disk_usage(const gchar *path);
gchar  *nss_session_relay_storage_dir(void);

G_END_DECLS

#endif /* NSS_RELAY_STATS_H */
