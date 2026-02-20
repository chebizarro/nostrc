/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-marmot-service.c - Marmot Protocol Service
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-marmot-service.h"
#include <marmot-gobject-1.0/marmot-gobject.h>

struct _GnMarmotService
{
  GObject parent_instance;

  MarmotGobjectClient  *client;
  MarmotGobjectStorage *storage;

  gchar *data_dir;
  gchar *user_pubkey_hex;
  gchar *user_secret_key_hex;
};

G_DEFINE_TYPE(GnMarmotService, gn_marmot_service, G_TYPE_OBJECT)

/* Signals */
enum {
  SIGNAL_GROUP_CREATED,
  SIGNAL_GROUP_JOINED,
  SIGNAL_MESSAGE_RECEIVED,
  SIGNAL_WELCOME_RECEIVED,
  SIGNAL_GROUP_UPDATED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

/* Singleton instance */
static GnMarmotService *default_service = NULL;

static void
gn_marmot_service_dispose(GObject *object)
{
  GnMarmotService *self = GN_MARMOT_SERVICE(object);

  g_clear_object(&self->client);
  g_clear_object(&self->storage);

  G_OBJECT_CLASS(gn_marmot_service_parent_class)->dispose(object);
}

static void
gn_marmot_service_finalize(GObject *object)
{
  GnMarmotService *self = GN_MARMOT_SERVICE(object);

  g_clear_pointer(&self->data_dir, g_free);
  g_clear_pointer(&self->user_pubkey_hex, g_free);

  if (self->user_secret_key_hex)
    {
      /* Securely wipe secret key */
      volatile char *p = (volatile char *)self->user_secret_key_hex;
      size_t len = strlen(self->user_secret_key_hex);
      for (size_t i = 0; i < len; i++)
        p[i] = 0;
      g_free(self->user_secret_key_hex);
      self->user_secret_key_hex = NULL;
    }

  G_OBJECT_CLASS(gn_marmot_service_parent_class)->finalize(object);
}

static void
gn_marmot_service_class_init(GnMarmotServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose  = gn_marmot_service_dispose;
  object_class->finalize = gn_marmot_service_finalize;

  /**
   * GnMarmotService::group-created:
   * @service: the service
   * @group: (transfer none): the created group
   */
  signals[SIGNAL_GROUP_CREATED] =
    g_signal_new("group-created",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1,
                 MARMOT_GOBJECT_TYPE_GROUP);

  /**
   * GnMarmotService::group-joined:
   * @service: the service
   * @group: (transfer none): the joined group
   */
  signals[SIGNAL_GROUP_JOINED] =
    g_signal_new("group-joined",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1,
                 MARMOT_GOBJECT_TYPE_GROUP);

  /**
   * GnMarmotService::message-received:
   * @service: the service
   * @group_id_hex: MLS group ID hex string
   * @inner_event_json: decrypted inner event JSON
   */
  signals[SIGNAL_MESSAGE_RECEIVED] =
    g_signal_new("message-received",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 2,
                 G_TYPE_STRING,
                 G_TYPE_STRING);

  /**
   * GnMarmotService::welcome-received:
   * @service: the service
   * @welcome: (transfer none): the welcome
   */
  signals[SIGNAL_WELCOME_RECEIVED] =
    g_signal_new("welcome-received",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1,
                 MARMOT_GOBJECT_TYPE_WELCOME);

  /**
   * GnMarmotService::group-updated:
   * @service: the service
   * @group: (transfer none): the updated group
   */
  signals[SIGNAL_GROUP_UPDATED] =
    g_signal_new("group-updated",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1,
                 MARMOT_GOBJECT_TYPE_GROUP);
}

static void
gn_marmot_service_init(GnMarmotService *self)
{
  self->client             = NULL;
  self->storage            = NULL;
  self->data_dir           = NULL;
  self->user_pubkey_hex    = NULL;
  self->user_secret_key_hex = NULL;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════ */

GnMarmotService *
gn_marmot_service_get_default(void)
{
  return default_service;
}

GnMarmotService *
gn_marmot_service_initialize(const gchar *data_dir,
                              GError     **error)
{
  g_return_val_if_fail(data_dir != NULL, NULL);

  if (default_service != NULL)
    {
      g_debug("MarmotService: already initialized");
      return default_service;
    }

  g_autoptr(GnMarmotService) self = g_object_new(GN_TYPE_MARMOT_SERVICE, NULL);

  self->data_dir = g_strdup(data_dir);

  /* Ensure data directory exists */
  g_autofree gchar *marmot_dir = g_build_filename(data_dir, "marmot", NULL);
  if (g_mkdir_with_parents(marmot_dir, 0700) != 0)
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "Failed to create marmot data directory: %s", marmot_dir);
      return NULL;
    }

  /* Create SQLite storage backend */
  g_autofree gchar *db_path = g_build_filename(marmot_dir, "marmot.db", NULL);
  self->storage = marmot_gobject_sqlite_storage_new(db_path, NULL, error);
  if (self->storage == NULL)
    {
      g_prefix_error(error, "Failed to create marmot storage: ");
      return NULL;
    }

  /* Create client */
  self->client = marmot_gobject_client_new(self->storage);
  if (self->client == NULL)
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "Failed to create marmot client");
      return NULL;
    }

  g_info("MarmotService: initialized with storage at %s", db_path);

  default_service = g_steal_pointer(&self);
  return default_service;
}

void
gn_marmot_service_shutdown(void)
{
  if (default_service == NULL)
    return;

  g_info("MarmotService: shutting down");
  g_clear_object(&default_service);
}

MarmotGobjectClient *
gn_marmot_service_get_client(GnMarmotService *self)
{
  g_return_val_if_fail(GN_IS_MARMOT_SERVICE(self), NULL);
  return self->client;
}

const gchar *
gn_marmot_service_get_user_pubkey_hex(GnMarmotService *self)
{
  g_return_val_if_fail(GN_IS_MARMOT_SERVICE(self), NULL);
  return self->user_pubkey_hex;
}

void
gn_marmot_service_set_user_identity(GnMarmotService *self,
                                     const gchar     *pubkey_hex,
                                     const gchar     *secret_key_hex)
{
  g_return_if_fail(GN_IS_MARMOT_SERVICE(self));
  g_return_if_fail(pubkey_hex != NULL);

  g_free(self->user_pubkey_hex);
  self->user_pubkey_hex = g_strdup(pubkey_hex);

  /* Securely wipe old secret key */
  if (self->user_secret_key_hex)
    {
      volatile char *p = (volatile char *)self->user_secret_key_hex;
      size_t len = strlen(self->user_secret_key_hex);
      for (size_t i = 0; i < len; i++)
        p[i] = 0;
      g_free(self->user_secret_key_hex);
      self->user_secret_key_hex = NULL;
    }

  if (secret_key_hex)
    self->user_secret_key_hex = g_strdup(secret_key_hex);

  g_info("MarmotService: user identity set (pubkey: %.16s…)", pubkey_hex);
}
