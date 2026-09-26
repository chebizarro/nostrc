/* main.c - nostr-wallet-agent
 *
 * SPDX-License-Identifier: MIT
 *
 * A GApplication (id org.nostr.Wallet) that additionally owns the API name
 * org.nostr.Wallet1. Normally started by D-Bus activation through the
 * nostr-wallet-agent.service systemd user unit (--gapplication-service).
 *
 * `nostr-wallet-agent --open <uri>` (the .desktop scheme handler) calls
 * org.nostr.Wallet1.OpenUri, D-Bus-activating the agent if needed, so the
 * agent can name the application that opened the link. Only when no agent
 * is activatable (build tree) does this process become the agent.
 */
#include "nwa-service.h"

#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

#ifndef NWA_VERSION
#define NWA_VERSION "0.1.0"
#endif

typedef struct {
  NwaService *service;
  guint       name_id;
  gchar      *pending_uri;
} Agent;

static Agent agent;

static void
on_name_lost(GDBusConnection *bus, const gchar *name, gpointer data)
{
  GApplication *app = data;
  if (bus && g_dbus_connection_is_closed(bus))
    g_message("nostr-wallet-agent: session bus closed; exiting");
  else
    g_warning("nostr-wallet-agent: lost or could not own %s; exiting", name);
  g_application_quit(app);
}

static void
on_name_acquired(GDBusConnection *bus, const gchar *name, gpointer data)
{
  (void)bus; (void)data;
  g_message("nostr-wallet-agent: owning %s", name);
}

#define NWA_TYPE_AGENT_APP (nwa_agent_app_get_type())
G_DECLARE_FINAL_TYPE(NwaAgentApp, nwa_agent_app, NWA, AGENT_APP, GApplication)

struct _NwaAgentApp {
  GApplication parent_instance;
};

G_DEFINE_TYPE(NwaAgentApp, nwa_agent_app, G_TYPE_APPLICATION)

/* GApplication calls dbus_register in every process, including a
 * short-lived `--open` forwarder that turns out to be remote, so the service
 * itself is only created in startup (primary instance only). */
static void
agent_startup(GApplication *app)
{
  G_APPLICATION_CLASS(nwa_agent_app_parent_class)->startup(app);
  /* The agent is a daemon: stay alive without windows. */
  g_application_hold(app);

  GDBusConnection *bus = g_application_get_dbus_connection(app);
  if (!bus) {
    g_warning("nostr-wallet-agent: no session bus");
    g_application_quit(app);
    return;
  }
  GError *err = NULL;
  agent.service = nwa_service_new(bus, &err);
  if (!agent.service) {
    g_warning("nostr-wallet-agent: cannot export %s: %s", NWA_OBJECT_PATH, err->message);
    g_error_free(err);
    g_application_quit(app);
    return;
  }
  agent.name_id = g_bus_own_name_on_connection(bus, NWA_BUS_NAME,
                                               G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
                                               on_name_acquired, on_name_lost, app, NULL);
}

static void
agent_dbus_unregister(GApplication *app, GDBusConnection *bus, const gchar *path)
{
  if (agent.name_id) {
    g_bus_unown_name(agent.name_id);
    agent.name_id = 0;
  }
  g_clear_pointer(&agent.service, nwa_service_free);
  G_APPLICATION_CLASS(nwa_agent_app_parent_class)->dbus_unregister(app, bus, path);
}

static void
nwa_agent_app_class_init(NwaAgentAppClass *klass)
{
  GApplicationClass *ac = G_APPLICATION_CLASS(klass);
  ac->startup = agent_startup;
  ac->dbus_unregister = agent_dbus_unregister;
}

static void
nwa_agent_app_init(NwaAgentApp *self)
{
  (void)self;
}

static void
on_activate(GApplication *app, gpointer data)
{
  (void)app; (void)data;
  if (agent.pending_uri) {
    g_autofree gchar *uri = g_steal_pointer(&agent.pending_uri);
    if (agent.service) nwa_service_open_uri(agent.service, uri);
  }
}

static gint
on_handle_local_options(GApplication *app, GVariantDict *opts, gpointer data)
{
  (void)data;
  if (g_variant_dict_contains(opts, "version")) {
    printf("nostr-wallet-agent %s\n", NWA_VERSION);
    return 0;
  }
  const gchar *uri = NULL;
  if (!g_variant_dict_lookup(opts, "open", "&s", &uri))
    return -1;

  /* Normal path: hand the link to the (D-Bus activated) agent, which
   * identifies us — and therefore the application that opened the link —
   * from our bus credentials. */
  GError *err = NULL;
  g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (bus) {
    g_autoptr(GVariant) r = g_dbus_connection_call_sync(bus, NWA_BUS_NAME, NWA_OBJECT_PATH,
                                                        NWA_INTERFACE, "OpenUri",
                                                        g_variant_new("(s)", uri), NULL,
                                                        G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &err);
    if (r) return 0;
    if (!g_dbus_error_is_remote_error(err) ||
        (!g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) &&
         !g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER))) {
      g_printerr("nostr-wallet-agent: %s\n", err->message);
      g_error_free(err);
      return 1;
    }
    g_clear_error(&err);
  } else {
    g_printerr("nostr-wallet-agent: %s\n", err->message);
    g_error_free(err);
    return 1;
  }
  /* No activatable agent (e.g. running from a build tree): become it. */
  if (!g_application_register(app, NULL, &err)) {
    g_printerr("nostr-wallet-agent: %s\n", err->message);
    g_error_free(err);
    return 1;
  }
  if (g_application_get_is_remote(app)) {
    g_printerr("nostr-wallet-agent: another instance is starting; try again\n");
    return 1;
  }
  agent.pending_uri = g_strdup(uri);
  return -1;
}

int
main(int argc, char **argv)
{
  GApplication *app = g_object_new(NWA_TYPE_AGENT_APP,
                                   "application-id", "org.nostr.Wallet",
                                   "flags", G_APPLICATION_DEFAULT_FLAGS,
                                   NULL);

  static const GOptionEntry entries[] = {
    { "open", 0, 0, G_OPTION_ARG_STRING, NULL,
      "Handle a lightning:, bitcoin: or nostr+walletconnect: URI", "URI" },
    { "version", 0, 0, G_OPTION_ARG_NONE, NULL, "Print version and exit", NULL },
    { NULL, 0, 0, 0, NULL, NULL, NULL }
  };
  g_application_add_main_option_entries(app, entries);
  g_application_set_option_context_summary(app, "Nostr Wallet Connect agent (org.nostr.Wallet1)");

  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
  g_signal_connect(app, "handle-local-options", G_CALLBACK(on_handle_local_options), NULL);

  int rc = g_application_run(app, argc, argv);
  g_free(agent.pending_uri);
  g_object_unref(app);
  return rc;
}
