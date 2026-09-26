/*
 * nostr-notify-daemon — NIP-29 + NIP-17 background notifier for the GNOME
 * session (§3.3 Piece B).
 *
 * Design summary:
 *
 *   - Owns GApplication ID `org.nostr.NotifyDaemon` (Finding 4: cannot
 *     share GNostr's ID or the notification actions and suppression
 *     break simultaneously).
 *   - Reads the account pubkey once from the signer's session-bus method
 *     `org.nostr.Signer.GetPublicKey`. On `NotFound` (no account yet) the
 *     daemon idles cleanly.
 *   - Loads home_relays from ~/.config/nostr-notify/nostr-notify.conf.
 *     The signer's `GetRelays` is preferred when available; the config
 *     is the fallback.
 *   - Suppression edge (§3.3 D5 revised, Finding 12): watches session
 *     bus for `org.gnostr.Client` name-owner-changed. Any transition
 *     bumps the generation guard so in-flight callbacks drop silently.
 *
 * Not in v1:
 *   - Actual Unix-socket connection to $XDG_RUNTIME_DIR/nostr/relay.sock
 *     (libnostr's ws client speaks ws://+wss:// only; adding unix:// is
 *     tracked separately). The daemon prints a note and falls back to
 *     direct home_relays. See notify_subs.c for the mode handling.
 *   - Coalescing summary ("N new messages") — v1 replaces the visible
 *     notification per (kind, thread-key) via a stable withdraw_id.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib-unix.h>

#include "nostr/nip19/nip19.h"  /* nostr_nip19_decode_npub */

#include "notify_gnotification.h"
#include "notify_subs.h"
#include "notify_suppress.h"

/* Forward decl from notify_subs.c; internal, keeps the main file dumb. */
void nostr_notify_subs_withdraw_all(void);

/* Daemon-wide singleton state. GApplication runs on the main thread. */
typedef struct {
  GApplication *app;
  NostrNotifySuppressGuard guard;

  /* Config. Loaded once at startup. `home_relays` is a NULL-terminated
   * array of newly-allocated URL strings; freed at shutdown. */
  char *user_pubkey_hex; /* 64 hex chars + \0; NULL if no account */
  char **home_relays;    /* NULL-terminated */
  size_t home_relays_count;
  NostrNotifyUpstreamMode upstream_mode;

  /* Grace timer for resuming subscriptions after GNostr vanishes. */
  guint resume_grace_source_id;

  /* Session bus watcher for org.gnostr.Client. */
  guint gnostr_name_watch_id;
  gboolean gnostr_present;

  /* Whether subscription drivers are currently armed. */
  gboolean subs_running;

  /* Subs context handed to notify_subs.[ch]. */
  NostrNotifySubsCtx subs_ctx;
} NostrNotifyDaemon;

static NostrNotifyDaemon g_daemon;

/* ------------------------------------------------------------------- */
/* Config loading                                                      */
/* ------------------------------------------------------------------- */

static char *config_path(void) {
  const char *xcfg = getenv("XDG_CONFIG_HOME");
  const char *home = getenv("HOME");
  if (xcfg && xcfg[0] == '/')
    return g_strdup_printf("%s/nostr-notify/nostr-notify.conf", xcfg);
  if (home && home[0] == '/')
    return g_strdup_printf("%s/.config/nostr-notify/nostr-notify.conf", home);
  return NULL;
}

static void load_config(NostrNotifyDaemon *d) {
  d->upstream_mode = NSN_UPSTREAM_DIRECT;

  g_autofree char *path = config_path();
  if (!path) return;
  g_autoptr(GKeyFile) kf = g_key_file_new();
  GError *e = NULL;
  if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &e)) {
    /* Missing config is fine — we then require the signer's GetRelays. */
    g_clear_error(&e);
    return;
  }

  gchar *mode = g_key_file_get_string(kf, "notify", "upstream_mode", NULL);
  if (mode) {
    if (g_ascii_strcasecmp(mode, "session_relay") == 0)
      d->upstream_mode = NSN_UPSTREAM_SESSION_RELAY;
    g_free(mode);
  }

  gsize count = 0;
  gchar **relays = g_key_file_get_string_list(kf, "notify", "home_relays",
                                              &count, NULL);
  if (relays && count > 0) {
    d->home_relays_count = count;
    d->home_relays = g_new0(char *, count + 1);
    for (gsize i = 0; i < count; i++)
      d->home_relays[i] = g_strdup(relays[i]);
    g_strfreev(relays);
  } else if (relays) {
    g_strfreev(relays);
  }
}

/* ------------------------------------------------------------------- */
/* Signer bridge                                                       */
/* ------------------------------------------------------------------- */

/*
 * Resolve the account pubkey via `org.nostr.Signer.GetPublicKey`. Returns
 * a newly-allocated 64-char lowercase hex string, or NULL on:
 *   - Signer not available (bus not there / daemon not started)
 *   - NotFound (no account configured — daemon idles)
 *   - Any decode error
 */
static char *fetch_pubkey_hex_from_signer(void) {
  g_autoptr(GError) err = NULL;
  g_autoptr(GDBusConnection) bus =
      g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus) return NULL;

  g_autoptr(GVariant) ret = g_dbus_connection_call_sync(
      bus, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer",
      "GetPublicKey", NULL, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE,
      3000, NULL, &err);
  if (!ret) return NULL;

  const gchar *npub = NULL;
  g_variant_get(ret, "(&s)", &npub);
  if (!npub || !*npub) return NULL;

  uint8_t raw[32];
  if (nostr_nip19_decode_npub(npub, raw) != 0) return NULL;

  static const char *hex = "0123456789abcdef";
  char *out = g_malloc(65);
  for (int i = 0; i < 32; i++) {
    out[i * 2]     = hex[(raw[i] >> 4) & 0xF];
    out[i * 2 + 1] = hex[raw[i] & 0xF];
  }
  out[64] = '\0';
  return out;
}

/*
 * If the signer exposes `GetRelays`, prefer that over the config file's
 * home_relays. Returns TRUE if home_relays was overwritten.
 */
static gboolean maybe_use_signer_relays(NostrNotifyDaemon *d) {
  g_autoptr(GError) err = NULL;
  g_autoptr(GDBusConnection) bus =
      g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus) return FALSE;

  g_autoptr(GVariant) ret = g_dbus_connection_call_sync(
      bus, "org.nostr.Signer", "/org/nostr/signer", "org.nostr.Signer",
      "GetRelays", NULL, G_VARIANT_TYPE("(a{sa{sb}})"),
      G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err);
  if (!ret) return FALSE;

  g_autoptr(GVariant) map = g_variant_get_child_value(ret, 0);
  GVariantIter it;
  g_variant_iter_init(&it, map);
  const gchar *url = NULL;
  GVariant *policy = NULL;
  GPtrArray *found = g_ptr_array_new_with_free_func(g_free);
  while (g_variant_iter_loop(&it, "{&s@a{sb}}", &url, &policy)) {
    /* Consume as-is — the daemon subscribes only, so every relay is
     * candidate for our read-side subs. */
    if (url && *url) g_ptr_array_add(found, g_strdup(url));
  }
  if (found->len == 0) {
    g_ptr_array_free(found, TRUE);
    return FALSE;
  }
  /* Replace config-loaded relays. */
  if (d->home_relays) {
    for (size_t i = 0; d->home_relays[i]; i++) g_free(d->home_relays[i]);
    g_free(d->home_relays);
  }
  d->home_relays_count = found->len;
  d->home_relays = g_new0(char *, found->len + 1);
  for (guint i = 0; i < found->len; i++)
    d->home_relays[i] = g_strdup((const char *)g_ptr_array_index(found, i));
  g_ptr_array_free(found, TRUE);
  return TRUE;
}

/* ------------------------------------------------------------------- */
/* Suppression edges                                                   */
/* ------------------------------------------------------------------- */

static void start_subs(NostrNotifyDaemon *d) {
  if (d->subs_running) return;
  if (!d->user_pubkey_hex || !d->home_relays_count) return;

  memset(&d->subs_ctx, 0, sizeof d->subs_ctx);
  strncpy(d->subs_ctx.user_pubkey_hex, d->user_pubkey_hex,
          sizeof(d->subs_ctx.user_pubkey_hex) - 1);
  d->subs_ctx.mode = d->upstream_mode;
  d->subs_ctx.home_relays = (const char **)d->home_relays;
  d->subs_ctx.home_relays_count = d->home_relays_count;
  d->subs_ctx.guard = &d->guard;
  d->subs_ctx.app = d->app;

  if (nostr_notify_subs_start(&d->subs_ctx)) {
    d->subs_running = TRUE;
    g_message("nostr-notify: subscriptions armed on %zu relay(s)",
              d->home_relays_count);
  }
}

static void stop_subs(NostrNotifyDaemon *d) {
  if (!d->subs_running) return;
  nostr_notify_subs_stop(&d->subs_ctx);
  d->subs_running = FALSE;
}

static gboolean resume_grace_expired(gpointer user_data) {
  NostrNotifyDaemon *d = user_data;
  d->resume_grace_source_id = 0;
  if (!d->gnostr_present) {
    /* Bump generation as we leave the transition state; any callback that
     * snapshot the pre-vanish generation stays dropped, but new events
     * from this point onward are permitted. */
    (void)nsn_guard_bump(&d->guard);
    nsn_guard_set_suppressed(&d->guard, false);
    start_subs(d);
  }
  return G_SOURCE_REMOVE;
}

static void on_gnostr_name_appeared(GDBusConnection *bus, const gchar *name,
                                    const gchar *name_owner, gpointer u) {
  (void)bus; (void)name; (void)name_owner;
  NostrNotifyDaemon *d = u;
  g_message("nostr-notify: %s appeared; suppressing", name);
  d->gnostr_present = TRUE;

  /* Close the gate BEFORE invalidating snapshots (§Oracle review Q2). If
   * the order were reversed, a connector that snapshotted between bump
   * and set_suppressed(true) would see (new_gen, suppressed==false), pass
   * its cheap producer-side check, and advance the cursor for an event
   * that the main-thread final check will then correctly drop — wasted
   * work at best, a lost notification at worst if the daemon crashes
   * before the drop is recorded. Setting suppressed first makes every
   * intermediate observable state fail closed. */
  nsn_guard_set_suppressed(&d->guard, true);
  (void)nsn_guard_bump(&d->guard);

  /* Cancel grace timer if a name-vanish is being un-done. */
  if (d->resume_grace_source_id) {
    g_source_remove(d->resume_grace_source_id);
    d->resume_grace_source_id = 0;
  }

  /* Withdraw all live notifications; the user is looking at GNostr. */
  nostr_notify_subs_withdraw_all();

  /* Stop the subscriptions so we don't burn upstream credit while
   * suppressed. New events queued in the connector thread's channel
   * will be discarded on the main-thread re-check. */
  stop_subs(d);
}

static void on_gnostr_name_vanished(GDBusConnection *bus, const gchar *name,
                                    gpointer u) {
  (void)bus; (void)name;
  NostrNotifyDaemon *d = u;
  g_message("nostr-notify: %s vanished; 30s grace before resume", name);
  d->gnostr_present = FALSE;

  /* Bump generation on the vanish edge too. In-flight callbacks that
   * snapshotted while suppressed drop regardless (suppressed field is
   * still true until we clear it in resume_grace_expired). */
  (void)nsn_guard_bump(&d->guard);

  if (d->resume_grace_source_id) g_source_remove(d->resume_grace_source_id);
  d->resume_grace_source_id =
      g_timeout_add_seconds(30, resume_grace_expired, d);
}

/* ------------------------------------------------------------------- */
/* app.open-in-gnostr action                                           */
/* ------------------------------------------------------------------- */

static void on_open_in_gnostr(GSimpleAction *action, GVariant *param,
                              gpointer user_data) {
  (void)action; (void)user_data;
  if (!param || !g_variant_is_of_type(param, G_VARIANT_TYPE_STRING)) return;
  const gchar *uri = g_variant_get_string(param, NULL);
  if (!uri || !*uri) return;
  (void)nostr_notify_activate_deep_link(uri);
}

/* ------------------------------------------------------------------- */
/* GApplication plumbing                                               */
/* ------------------------------------------------------------------- */

static void on_startup(GApplication *app, gpointer user_data) {
  NostrNotifyDaemon *d = user_data;
  d->app = app;
  nsn_guard_init(&d->guard);

  /* Load config first so we know upstream_mode + fallback relays. */
  load_config(d);

  /* Resolve pubkey. NotFound → idle. */
  d->user_pubkey_hex = fetch_pubkey_hex_from_signer();
  if (!d->user_pubkey_hex) {
    g_message("nostr-notify: signer has no account (GetPublicKey NotFound "
              "or Signer bus not available); daemon idle.");
  }

  /* Prefer signer's relay list if available. */
  (void)maybe_use_signer_relays(d);

  /* Register the app-level action so a GNotification default action
   * `app.open-in-gnostr` with a string target can be activated. */
  GSimpleAction *act = g_simple_action_new("open-in-gnostr",
                                           G_VARIANT_TYPE_STRING);
  g_signal_connect(act, "activate", G_CALLBACK(on_open_in_gnostr), d);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(act));
  g_object_unref(act);

  /* Watch GNostr's bus name. `_FLAG_NONE` so we do NOT autostart GNostr;
   * the notifier's job is background observation. */
  d->gnostr_name_watch_id = g_bus_watch_name(
      G_BUS_TYPE_SESSION, "org.gnostr.Client", G_BUS_NAME_WATCHER_FLAGS_NONE,
      on_gnostr_name_appeared, on_gnostr_name_vanished, d, NULL);

  /* Arm subscriptions if we have both a pubkey and a relay list. */
  start_subs(d);

  /* g_application_hold() keeps the app alive with no windows. */
  g_application_hold(app);
}

static void on_shutdown(GApplication *app, gpointer user_data) {
  (void)app;
  NostrNotifyDaemon *d = user_data;
  stop_subs(d);
  if (d->gnostr_name_watch_id) {
    g_bus_unwatch_name(d->gnostr_name_watch_id);
    d->gnostr_name_watch_id = 0;
  }
  if (d->resume_grace_source_id) {
    g_source_remove(d->resume_grace_source_id);
    d->resume_grace_source_id = 0;
  }
  g_free(d->user_pubkey_hex);
  d->user_pubkey_hex = NULL;
  if (d->home_relays) {
    for (size_t i = 0; d->home_relays[i]; i++) g_free(d->home_relays[i]);
    g_free(d->home_relays);
    d->home_relays = NULL;
  }
  d->home_relays_count = 0;
}

/* ------------------------------------------------------------------- */
/* Signal wiring                                                       */
/* ------------------------------------------------------------------- */

static gboolean on_sigterm(gpointer user_data) {
  GApplication *app = user_data;
  g_application_release(app);
  return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
  memset(&g_daemon, 0, sizeof g_daemon);

  /* Ignore SIGPIPE — a relay disconnect during write must not kill us. */
  signal(SIGPIPE, SIG_IGN);

  GApplication *app =
      g_application_new("org.nostr.NotifyDaemon", G_APPLICATION_IS_SERVICE);
  g_signal_connect(app, "startup", G_CALLBACK(on_startup), &g_daemon);
  g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), &g_daemon);

  g_unix_signal_add(SIGTERM, on_sigterm, app);
  g_unix_signal_add(SIGINT, on_sigterm, app);

  int rc = g_application_run(app, argc, argv);
  g_object_unref(app);
  return rc;
}
