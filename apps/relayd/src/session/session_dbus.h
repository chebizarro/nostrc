/*
 * session_dbus.h — org.nostr.SessionRelay1 on nostr-session-relayd.
 *
 * Read-only statistics for settings UIs (gnome/nostr-settings) and the
 * `--stats` CLI. Interface: gnome/dbus/org.nostr.SessionRelay1.xml.
 *
 * The relay's event loop (libwebsockets, nostr_relay_server_run()) blocks
 * the main thread, so the bus connection lives on a dedicated thread with
 * its own GMainContext. Everything the handlers read is either immutable
 * after start (config, paths), an atomic metrics snapshot, or computed on
 * the D-Bus thread itself (disk usage, storage count through a read
 * transaction).
 *
 * Built only when GIO is available (NSR_HAVE_GDBUS); otherwise the stubs
 * below make start/stop no-ops and --stats reports it is unsupported.
 */
#ifndef NSR_SESSION_DBUS_H
#define NSR_SESSION_DBUS_H

#include "nostr-storage.h"
#include "relayd_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NSR_DBUS_NAME "org.nostr.SessionRelay1"
#define NSR_DBUS_PATH "/org/nostr/SessionRelay1"
#define NSR_DBUS_IFACE "org.nostr.SessionRelay1"

typedef struct {
  /* Borrowed; must outlive nsr_dbus_stop(). */
  const RelaydConfig *cfg;
  /* Borrowed, may be NULL (cache-less). Only vt->count is used, from the
   * D-Bus thread; nostrdb read transactions are thread-safe. */
  NostrStorage *storage;
  /* Driver actually opened ("nostrdb") or NULL when running cache-less. */
  const char *storage_backend;
  /* Driver the config asked for. */
  const char *requested_backend;
  const char *storage_dir;
  const char *version;
} NsrDbusInfo;

/* Spawn the D-Bus thread and request the bus name. Never fatal: a missing
 * session bus or a name already owned is logged and the relay carries on
 * without the stats interface. Copies the strings in @info. */
void nsr_dbus_start(const NsrDbusInfo *info);

/* Release the name and join the thread. Safe to call if start failed. */
void nsr_dbus_stop(void);

/* `nostr-session-relayd --stats`: query the running daemon over the
 * session bus and print one "key: value" line per statistic. Returns a
 * process exit status (0 ok, 1 not running / error). */
int nsr_dbus_print_stats(void);

/* Disk usage (allocated bytes) of every regular file under @dir. Exposed
 * for tests. Returns 0 for a missing directory. */
unsigned long long nsr_dir_allocated_bytes(const char *dir);

#ifdef __cplusplus
}
#endif

#endif /* NSR_SESSION_DBUS_H */
