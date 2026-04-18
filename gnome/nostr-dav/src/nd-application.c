/* nd-application.c - NostrDav application lifecycle
 *
 * SPDX-License-Identifier: MIT
 *
 * GApplication subclass that owns the DAV server and token store.
 * Holds the application so the main loop keeps running (daemon-style),
 * similar to the blossom-cache pattern.
 */

#include "nd-application.h"
#include "nd-dav-server.h"
#include "nd-token-store.h"

#include <glib.h>
#include <gio/gio.h>
#include <stdlib.h>

#define ND_DEFAULT_PORT    7680
#define ND_DEFAULT_ADDRESS "127.0.0.1"

struct _NdApplication {
  GApplication   parent_instance;

  NdTokenStore  *token_store;
  NdDavServer   *dav_server;
};

G_DEFINE_TYPE(NdApplication, nd_application, G_TYPE_APPLICATION)

/* ---- Lifecycle ---- */

static void
nd_application_activate(GApplication *app)
{
  NdApplication *self = ND_APPLICATION(app);

  g_message("nostr-dav: activating…");

  /* 1. Token store */
  self->token_store = nd_token_store_new();
  if (self->token_store == NULL) {
    g_critical("nostr-dav: failed to create token store");
    g_application_quit(app);
    return;
  }

  /* 2. Generate/retrieve default token for display */
  GError *token_err = NULL;
  g_autofree gchar *token = nd_token_store_ensure_token(
    self->token_store, "default", &token_err);
  if (token != NULL) {
    g_message("nostr-dav: bearer token for WebDAV client: %s", token);
    g_message("nostr-dav: use this as the password when adding a WebDAV account");
  } else {
    g_warning("nostr-dav: token generation failed: %s",
              token_err ? token_err->message : "unknown");
    g_clear_error(&token_err);
  }

  /* 3. DAV server */
  self->dav_server = nd_dav_server_new(self->token_store);

  /* Parse listen address/port from environment or use defaults */
  const gchar *addr_env = g_getenv("NOSTR_DAV_ADDRESS");
  const gchar *port_env = g_getenv("NOSTR_DAV_PORT");

  const gchar *address = (addr_env && addr_env[0]) ? addr_env : ND_DEFAULT_ADDRESS;
  guint port = ND_DEFAULT_PORT;
  if (port_env && port_env[0]) {
    gint64 p = g_ascii_strtoll(port_env, NULL, 10);
    if (p > 0 && p <= 65535)
      port = (guint)p;
  }

  GError *srv_err = NULL;
  if (!nd_dav_server_start(self->dav_server, address, port, &srv_err)) {
    g_critical("nostr-dav: failed to start on %s:%u: %s",
               address, port, srv_err->message);
    g_clear_error(&srv_err);
    g_application_quit(app);
    return;
  }

  g_message("nostr-dav: ready at http://%s:%u", address, port);
  g_message("nostr-dav: CalDAV: http://%s:%u/.well-known/caldav", address, port);
  g_message("nostr-dav: CardDAV: http://%s:%u/.well-known/carddav", address, port);

  /* Hold so the daemon keeps running */
  g_application_hold(app);
}

static void
nd_application_shutdown(GApplication *app)
{
  NdApplication *self = ND_APPLICATION(app);

  g_message("nostr-dav: shutting down…");

  if (self->dav_server) {
    nd_dav_server_stop(self->dav_server);
  }

  g_clear_object(&self->dav_server);
  nd_token_store_free(self->token_store);
  self->token_store = NULL;

  G_APPLICATION_CLASS(nd_application_parent_class)->shutdown(app);
}

static void
nd_application_startup(GApplication *app)
{
  G_APPLICATION_CLASS(nd_application_parent_class)->startup(app);
  g_message("nostr-dav: startup");
}

/* ---- GObject plumbing ---- */

static void
nd_application_finalize(GObject *obj)
{
  NdApplication *self = ND_APPLICATION(obj);

  g_clear_object(&self->dav_server);
  if (self->token_store) {
    nd_token_store_free(self->token_store);
    self->token_store = NULL;
  }

  G_OBJECT_CLASS(nd_application_parent_class)->finalize(obj);
}

static void
nd_application_class_init(NdApplicationClass *klass)
{
  GObjectClass      *object_class = G_OBJECT_CLASS(klass);
  GApplicationClass *app_class    = G_APPLICATION_CLASS(klass);

  object_class->finalize = nd_application_finalize;
  app_class->startup     = nd_application_startup;
  app_class->activate    = nd_application_activate;
  app_class->shutdown    = nd_application_shutdown;
}

static void
nd_application_init(NdApplication *self)
{
  (void)self;
}

NdApplication *
nd_application_new(void)
{
  return g_object_new(ND_TYPE_APPLICATION,
                      "application-id", "org.nostr.Dav",
                      "flags", G_APPLICATION_NON_UNIQUE,
                      NULL);
}
