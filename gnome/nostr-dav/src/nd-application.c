/* nd-application.c - NostrDav application lifecycle
 *
 * SPDX-License-Identifier: MIT
 *
 * GApplication service (G_APPLICATION_IS_SERVICE) that owns the DAV
 * server, token store, and store database. The systemd user unit is the
 * single activation authority; D-Bus activation of org.nostr.Dav is
 * delegated to that unit, and owning the bus name gives single-instance
 * semantics ($XDG_RUNTIME_DIR/nostr-dav/instance.lock backs this up; the
 * unit's RuntimeDirectory= makes that directory writable under
 * ProtectSystem=strict).
 *
 * Startup is fail-closed and ordered so that binding the socket is the
 * last step: lock → config → token → store → account → listen.
 */

#include "nd-application.h"
#include "nd-config.h"
#include "nd-dav-server.h"
#include "nd-publisher.h"
#include "nd-relay-sync.h"
#include "nd-relay-transport.h"
#include "nd-signer.h"
#include "nd-calendar-store.h"
#include "nd-contact-store.h"
#include "nd-store-db.h"
#include "nd-token-store.h"

#include <glib.h>
#include <gio/gio.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

/* Bind policy is compile-time: loopback only, fixed port, no
 * environment or command-line override. For LAN access put a TLS
 * reverse proxy in front (see docs/QUICKSTART.md). */
#define ND_LISTEN_ADDRESS "127.0.0.1"
#define ND_LISTEN_PORT    7680

#define ND_ACCOUNT_ID     "default"
#define ND_DAV_USERNAME   "nostr"

/* How long a replacement instance waits for its predecessor to exit. */
#define ND_LOCK_WAIT_STEPS    50
#define ND_LOCK_WAIT_STEP_US  (100 * 1000)

struct _NdApplication {
  GApplication      parent_instance;

  NdConfig          config;
  NdTokenStore     *token_store;
  NdStoreDb        *db;
  NdDavServer      *dav_server;
  NdSigner         *signer;
  NdPublisher      *publisher;
  NdRelaySync      *relay_sync;
  NdCalendarStore  *cal_store;
  NdContactStore   *contact_store;
  int               lock_fd;
  int               exit_status;
};

G_DEFINE_TYPE(NdApplication, nd_application, G_TYPE_APPLICATION)

static const GOptionEntry nd_option_entries[] = {
  { "show-credentials", 0, 0, G_OPTION_ARG_NONE, NULL,
    "Print the WebDAV server URL, username and bearer token, then exit",
    NULL },
  { NULL, 0, 0, 0, NULL, NULL, NULL }
};

/* ---- Helpers ---- */

static gboolean
acquire_instance_lock(NdApplication *self, GError **error)
{
  g_autofree gchar *dir =
    g_build_filename(g_get_user_runtime_dir(), "nostr-dav", NULL);
  g_autofree gchar *path = g_build_filename(dir, "instance.lock", NULL);

  if (g_mkdir_with_parents(dir, 0700) != 0) {
    int saved = errno;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved),
                "Cannot create %s: %s", dir, g_strerror(saved));
    return FALSE;
  }

  int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    int saved = errno;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved),
                "Cannot open lock file %s: %s", path, g_strerror(saved));
    return FALSE;
  }

  /* Bounded wait so a --gapplication-replace successor can take over
   * once its predecessor has released the port and exited. */
  for (int i = 0; i < ND_LOCK_WAIT_STEPS; i++) {
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
      self->lock_fd = fd;
      return TRUE;
    }
    if (errno != EWOULDBLOCK && errno != EINTR)
      break;
    g_usleep(ND_LOCK_WAIT_STEP_US);
  }

  close(fd);
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
              "Another nostr-dav instance holds %s; refusing to start a "
              "second instance", path);
  return FALSE;
}

static NdRelayTransport *
session_transport_factory(const gchar *relay_url, gpointer user_data)
{
  (void)user_data;
  /* Real WebSocket wiring is a follow-up bead; the factory hands out a
   * scaffold transport so the sync + publisher plumbing is testable in
   * a running daemon (connects will fail loud, staged rows stay
   * pending and retry). */
  return nd_relay_transport_new_websocket(relay_url);
}

static gboolean
bring_up(NdApplication *self, GError **error)
{
  /* 1. Single instance (defense in depth behind the bus name). */
  if (!acquire_instance_lock(self, error))
    return FALSE;

  /* 2. Configuration. */
  g_autofree gchar *config_path = nd_config_default_path();
  if (!nd_config_load(config_path, &self->config, error))
    return FALSE;

  /* 3. Token: load or mint; wrong permissions are fatal. */
  g_autofree gchar *token_dir = nd_token_store_default_dir();
  self->token_store = nd_token_store_new(token_dir, TRUE);
  g_autofree gchar *token =
    nd_token_store_ensure_token(self->token_store, ND_ACCOUNT_ID, error);
  if (token == NULL)
    return FALSE;
  memset(token, 0, strlen(token));

  /* 4. Persistent store. */
  g_autofree gchar *db_path = nd_store_db_default_path();
  self->db = nd_store_db_open(db_path, error);
  if (self->db == NULL)
    return FALSE;

  /* 5. Account, then 6. listen — binding is the last step. */
  self->dav_server = nd_dav_server_new(self->token_store, self->db);
  nd_dav_server_set_account_id(self->dav_server, ND_ACCOUNT_ID);

  /* 5a. Signer + publisher: opt-in via `enable_publish` and best-effort
   *     on top of that. If the session bus is unreachable (headless
   *     build, tests) or the signer proxy fails to build, the local
   *     store still serves DAV — publishes just cannot be attempted.
   *     The gate defaults to OFF while the WebSocket transport is a
   *     scaffold (see nd-relay-transport.c) so the outbox does not spin
   *     against a `NOT_SUPPORTED` backend. */
  if (!self->config.enable_publish) {
    g_message("nostr-dav: publish + relay-sync disabled "
              "(enable_publish is off in %s)", config_path);
    goto listen;
  }
  GDBusConnection *bus =
    g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  if (bus != NULL) {
    GError *signer_err = NULL;
    self->signer = nd_signer_new_dbus(bus, &signer_err);
    if (self->signer == NULL) {
      g_warning("nostr-dav: signer proxy unavailable: %s",
                signer_err ? signer_err->message : "unknown");
      g_clear_error(&signer_err);
    }
    g_object_unref(bus);
  } else {
    g_message("nostr-dav: session bus unreachable; publish disabled");
  }

  if (self->signer != NULL) {
    self->cal_store     = nd_calendar_store_new(self->db);
    self->contact_store = nd_contact_store_new(self->db);
    self->publisher = nd_publisher_new(self->db, self->signer,
                                        session_transport_factory, self);
    nd_publisher_configure(self->publisher,
                           self->config.account_pubkey,
                           self->config.home_relays,
                           self->config.publish_quorum);
    nd_dav_server_set_publisher(self->dav_server, self->publisher);

    self->relay_sync = nd_relay_sync_new(self->db, self->cal_store,
                                          self->contact_store,
                                          session_transport_factory, self);
    nd_relay_sync_configure(self->relay_sync,
                            self->config.account_pubkey,
                            self->config.home_relays,
                            self->config.upstream_mode);
    nd_relay_sync_start(self->relay_sync);
  }

listen:
  if (!nd_dav_server_start(self->dav_server, ND_LISTEN_ADDRESS,
                           ND_LISTEN_PORT, error))
    return FALSE;

  g_message("nostr-dav: ready at http://%s:%u/ (store %s)",
            ND_LISTEN_ADDRESS, ND_LISTEN_PORT, db_path);
  g_message("nostr-dav: bearer token file: %s (run `nostr-dav "
            "--show-credentials` to display it)",
            nd_token_store_get_path(self->token_store));
  g_message("nostr-dav: relay upstream mode: %s",
            nd_upstream_mode_to_string(self->config.upstream_mode));
  return TRUE;
}

static void
tear_down(NdApplication *self)
{
  if (self->dav_server)
    nd_dav_server_stop(self->dav_server);
  g_clear_object(&self->dav_server);
  if (self->relay_sync != NULL) {
    nd_relay_sync_free(self->relay_sync);
    self->relay_sync = NULL;
  }
  if (self->publisher != NULL) {
    nd_publisher_free(self->publisher);
    self->publisher = NULL;
  }
  g_clear_pointer(&self->signer, nd_signer_unref);
  if (self->cal_store != NULL) {
    nd_calendar_store_free(self->cal_store);
    self->cal_store = NULL;
  }
  if (self->contact_store != NULL) {
    nd_contact_store_free(self->contact_store);
    self->contact_store = NULL;
  }
  g_clear_pointer(&self->db, nd_store_db_unref);
  g_clear_pointer(&self->token_store, nd_token_store_free);
  if (self->lock_fd >= 0) {
    close(self->lock_fd);
    self->lock_fd = -1;
  }
  nd_config_clear(&self->config);
}

static int
show_credentials(void)
{
  g_autofree gchar *dir = nd_token_store_default_dir();
  NdTokenStore *store = nd_token_store_new(dir, TRUE);
  GError *err = NULL;
  g_autofree gchar *token = nd_token_store_ensure_token(store, ND_ACCOUNT_ID,
                                                        &err);
  nd_token_store_free(store);

  if (token == NULL) {
    g_printerr("nostr-dav: %s\n", err ? err->message : "cannot load token");
    g_clear_error(&err);
    return 1;
  }

  g_print("Server:   http://%s:%u/\n"
          "Username: %s\n"
          "Password: %s\n",
          ND_LISTEN_ADDRESS, ND_LISTEN_PORT, ND_DAV_USERNAME, token);
  memset(token, 0, strlen(token));
  return 0;
}

/* ---- GApplication vfuncs ---- */

static gint
nd_application_handle_local_options(GApplication *app, GVariantDict *options)
{
  (void)app;
  if (g_variant_dict_contains(options, "show-credentials"))
    return show_credentials();
  return -1;
}

static void
nd_application_startup(GApplication *app)
{
  NdApplication *self = ND_APPLICATION(app);

  G_APPLICATION_CLASS(nd_application_parent_class)->startup(app);

  GError *err = NULL;
  if (!bring_up(self, &err)) {
    g_warning("nostr-dav: refusing to start: %s",
              err ? err->message : "unknown error");
    g_clear_error(&err);
    self->exit_status = 1;
    tear_down(self);
    g_application_quit(app);
    return;
  }

  /* Service mode never emits ::activate on its own; hold so the daemon
   * keeps running. */
  g_application_hold(app);
}

static void
nd_application_activate(GApplication *app)
{
  (void)app;
  /* D-Bus Activate() on an already running service: nothing to do. */
  g_debug("nostr-dav: activate");
}

static void
nd_application_shutdown(GApplication *app)
{
  NdApplication *self = ND_APPLICATION(app);

  g_message("nostr-dav: shutting down");
  tear_down(self);

  G_APPLICATION_CLASS(nd_application_parent_class)->shutdown(app);
}

/* ---- GObject plumbing ---- */

static void
nd_application_finalize(GObject *obj)
{
  tear_down(ND_APPLICATION(obj));
  G_OBJECT_CLASS(nd_application_parent_class)->finalize(obj);
}

static void
nd_application_class_init(NdApplicationClass *klass)
{
  GObjectClass      *object_class = G_OBJECT_CLASS(klass);
  GApplicationClass *app_class    = G_APPLICATION_CLASS(klass);

  object_class->finalize        = nd_application_finalize;
  app_class->handle_local_options = nd_application_handle_local_options;
  app_class->startup            = nd_application_startup;
  app_class->activate           = nd_application_activate;
  app_class->shutdown           = nd_application_shutdown;
}

static void
nd_application_init(NdApplication *self)
{
  nd_config_init_defaults(&self->config);
  self->lock_fd = -1;
  self->exit_status = 0;
  g_application_add_main_option_entries(G_APPLICATION(self),
                                        nd_option_entries);
}

NdApplication *
nd_application_new(void)
{
  return g_object_new(ND_TYPE_APPLICATION,
                      "application-id", "org.nostr.Dav",
                      "flags", G_APPLICATION_IS_SERVICE |
                               G_APPLICATION_ALLOW_REPLACEMENT,
                      NULL);
}

int
nd_application_get_exit_status(NdApplication *self)
{
  g_return_val_if_fail(ND_IS_APPLICATION(self), 1);
  return self->exit_status;
}
