/*
 * bc-application.c - BcApplication implementation
 *
 * SPDX-License-Identifier: MIT
 *
 * Owns the lifecycle of the blob store, cache manager, upstream client,
 * and HTTP server. Reads configuration from GSettings and wires up all
 * components during activate.
 */

#include "bc-application.h"
#include "bc-blob-store.h"
#include "bc-db-backend.h"
#include "bc-http-server.h"
#include "bc-cache-manager.h"
#include "bc-upstream-client.h"

#include <glib.h>
#include <gio/gio.h>
#include <stdlib.h>

struct _BcApplication {
  GApplication      parent_instance;

  GSettings        *settings;
  BcBlobStore      *store;
  BcUpstreamClient *upstream;
  BcCacheManager   *cache_mgr;
  BcHttpServer     *http_server;
};

G_DEFINE_TYPE(BcApplication, bc_application, G_TYPE_APPLICATION)

/* ---------- GSettings helpers ---------- */

static gchar *
bc_application_resolve_storage_dir(BcApplication *self)
{
  g_autofree gchar *configured = NULL;

  if (self->settings) {
    configured = g_settings_get_string(self->settings, "storage-path");
  }

  if (configured != NULL && configured[0] != '\0') {
    return g_steal_pointer(&configured);
  }

  return g_build_filename(g_get_user_data_dir(), "blossom-cache", NULL);
}

static gchar **
bc_application_get_upstream_servers(BcApplication *self)
{
  if (self->settings) {
    return g_settings_get_strv(self->settings, "upstream-servers");
  }

  const gchar *defaults[] = { "https://blossom.primal.net", NULL };
  return g_strdupv((gchar **)defaults);
}

/* ---------- Lifecycle ---------- */

static void
bc_application_activate(GApplication *app)
{
  BcApplication *self = BC_APPLICATION(app);

  g_message("blossom-cache: activating…");

  /* 1. Resolve storage directory */
  g_autofree gchar *storage_dir = bc_application_resolve_storage_dir(self);

  /* 2. Open blob store */
  GError *error = NULL;
  /* Select metadata backend: LMDB if available and configured, else SQLite */
  g_autofree gchar *db_backend_name = NULL;
  if (self->settings) {
    db_backend_name = g_settings_get_string(self->settings, "db-backend");
  }

  gboolean use_lmdb = (db_backend_name != NULL && g_str_equal(db_backend_name, "lmdb"));

  if (use_lmdb) {
    g_autofree gchar *lmdb_dir = g_build_filename(storage_dir, "metadata.lmdb", NULL);
    BcDbBackend *backend = bc_db_backend_lmdb_new(lmdb_dir, 0, &error);
    if (backend != NULL) {
      self->store = bc_blob_store_new(storage_dir, backend, &error);
    } else {
      g_warning("blossom-cache: LMDB backend failed (%s), falling back to SQLite",
                error ? error->message : "unknown");
      g_clear_error(&error);
      use_lmdb = FALSE;
    }
  }

  if (!use_lmdb) {
    self->store = bc_blob_store_new_sqlite(storage_dir, &error);
  }
  if (self->store == NULL) {
    g_critical("blossom-cache: failed to open blob store at %s: %s",
               storage_dir, error ? error->message : "unknown error");
    g_clear_error(&error);
    g_application_quit(app);
    return;
  }

  g_message("blossom-cache: blob store at %s (%u blobs, %" G_GINT64_FORMAT " bytes)",
            storage_dir,
            bc_blob_store_get_blob_count(self->store),
            bc_blob_store_get_total_size(self->store));

  /* 3. Upstream client */
  g_auto(GStrv) servers = bc_application_get_upstream_servers(self);
  self->upstream = bc_upstream_client_new((const gchar * const *)servers);

  /* 4. Cache manager */
  guint max_cache_mb = 2048;
  guint max_blob_mb  = 100;
  gboolean verify    = TRUE;

  if (self->settings) {
    max_cache_mb = g_settings_get_uint(self->settings, "max-cache-size-mb");
    max_blob_mb  = g_settings_get_uint(self->settings, "max-blob-size-mb");
    verify       = g_settings_get_boolean(self->settings, "verify-sha256");
  }

  self->cache_mgr = bc_cache_manager_new(
    self->store,
    self->upstream,
    (gint64)max_cache_mb * 1024 * 1024,
    (gint64)max_blob_mb  * 1024 * 1024,
    verify);

  /* 5. Run initial eviction sweep */
  GError *evict_err = NULL;
  gint evicted = bc_cache_manager_run_eviction(self->cache_mgr, &evict_err);
  if (evicted > 0) {
    g_message("blossom-cache: evicted %d blobs on startup", evicted);
  } else if (evicted < 0) {
    g_warning("blossom-cache: eviction error: %s",
              evict_err ? evict_err->message : "unknown");
    g_clear_error(&evict_err);
  }

  /* 6. Start HTTP server */
  gchar *listen_addr = NULL;
  guint port = 24242;

  if (self->settings) {
    listen_addr = g_settings_get_string(self->settings, "listen-address");
    port        = g_settings_get_uint(self->settings, "listen-port");
  }
  if (listen_addr == NULL || listen_addr[0] == '\0') {
    g_free(listen_addr);
    listen_addr = g_strdup("127.0.0.1");
  }

  self->http_server = bc_http_server_new(self->store, self->cache_mgr);

  GError *srv_err = NULL;
  if (!bc_http_server_start(self->http_server, listen_addr, port, &srv_err)) {
    g_critical("blossom-cache: HTTP server failed to start on %s:%u: %s",
               listen_addr, port, srv_err->message);
    g_free(listen_addr);
    g_clear_error(&srv_err);
    g_application_quit(app);
    return;
  }

  g_message("blossom-cache: listening on http://%s:%u", listen_addr, port);
  g_free(listen_addr);

  /* Hold the application so the main loop keeps running (daemon-style) */
  g_application_hold(app);
}

static void
bc_application_shutdown(GApplication *app)
{
  BcApplication *self = BC_APPLICATION(app);

  g_message("blossom-cache: shutting down…");

  if (self->http_server) {
    bc_http_server_stop(self->http_server);
  }

  g_clear_object(&self->http_server);
  g_clear_object(&self->cache_mgr);
  g_clear_object(&self->upstream);
  g_clear_object(&self->store);
  g_clear_object(&self->settings);

  G_APPLICATION_CLASS(bc_application_parent_class)->shutdown(app);
}

static void
bc_application_startup(GApplication *app)
{
  BcApplication *self = BC_APPLICATION(app);

  G_APPLICATION_CLASS(bc_application_parent_class)->startup(app);

  /* Try to load GSettings schema. Not fatal if unavailable. */
  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  if (source != NULL) {
    g_autoptr(GSettingsSchema) schema =
      g_settings_schema_source_lookup(source, "org.gnostr.BlossomCache", TRUE);
    if (schema != NULL) {
      self->settings = g_settings_new("org.gnostr.BlossomCache");
      g_debug("blossom-cache: GSettings schema loaded");
    } else {
      g_info("blossom-cache: GSettings schema not installed — using defaults");
    }
  }
}

/* ---------- GObject plumbing ---------- */

static void
bc_application_finalize(GObject *obj)
{
  BcApplication *self = BC_APPLICATION(obj);

  g_clear_object(&self->http_server);
  g_clear_object(&self->cache_mgr);
  g_clear_object(&self->upstream);
  g_clear_object(&self->store);
  g_clear_object(&self->settings);

  G_OBJECT_CLASS(bc_application_parent_class)->finalize(obj);
}

static void
bc_application_class_init(BcApplicationClass *klass)
{
  GObjectClass      *object_class = G_OBJECT_CLASS(klass);
  GApplicationClass *app_class    = G_APPLICATION_CLASS(klass);

  object_class->finalize = bc_application_finalize;
  app_class->startup     = bc_application_startup;
  app_class->activate    = bc_application_activate;
  app_class->shutdown    = bc_application_shutdown;
}

static void
bc_application_init(BcApplication *self)
{
  (void)self;
}

BcApplication *
bc_application_new(void)
{
  return g_object_new(BC_TYPE_APPLICATION,
                      "application-id", "org.gnostr.BlossomCache",
                      "flags", G_APPLICATION_NON_UNIQUE,
                      NULL);
}
