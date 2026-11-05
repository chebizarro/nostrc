/* SPDX-License-Identifier: MIT */
#include "nss-ui.h"

static void opened(GObject *source, GAsyncResult *result, gpointer data)
{
  NssContext *ctx = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (!reply) nss_toast(ctx, "Grotto could not be opened. Install or start Grotto to make this change.");
  nss_context_unref(ctx);
}

void nss_grotto_open(NssContext *ctx, const gchar *section, const gchar *target)
{
  if (!ctx->bus) { nss_toast(ctx, "Grotto is unavailable without a session bus."); return; }
  /* Navigation only. No proposed mutation, decision, credential or key data. */
  GVariantBuilder args, platform;
  g_variant_builder_init(&args, G_VARIANT_TYPE("av"));
  g_variant_builder_add(&args, "v", g_variant_new("(ss)", section, target ? target : ""));
  g_variant_builder_init(&platform, G_VARIANT_TYPE_VARDICT);
  g_dbus_connection_call(ctx->bus, "org.nostr.Grotto", "/org/nostr/Grotto",
    "org.freedesktop.Application", "ActivateAction",
    g_variant_new("(s@av@a{sv})", "open-management", g_variant_builder_end(&args), g_variant_builder_end(&platform)),
    NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, opened, nss_context_ref(ctx));
}
static void toast(gpointer data, const char *message) { nss_toast(data, "%s", message); }
static void review(gpointer data, const char *id) { nss_grotto_open(data, "review", id); }
static void add_identity(gpointer data) { nss_grotto_open(data, "add-identity", ""); }
static void add_app(gpointer data) { nss_grotto_open(data, "apps", ""); }
static void backup(gpointer data, const char *npub) { nss_grotto_open(data, "backup", npub); }
static void remove_identity(gpointer data, const char *npub) { nss_grotto_open(data, "identity", npub); }
static void replace_identity(gpointer data, const char *npub) { nss_grotto_open(data, "identity", npub); }
static void open_signer(gpointer data, const char *target) { nss_grotto_open(data, "target", target); }

void nss_grotto_attach_host(GrottoPanel *panel, NssContext *ctx)
{
  /* No can_command/command_async/profile_async hooks: Settings is not a trusted
   * security/profile host. The service exposes only the manager's label RPC. */
  const GrottoPanelsHost host = {.show_toast = toast, .review_request = review,
    .add_identity = add_identity, .add_app = add_app, .open_backup = backup,
    .remove_identity = remove_identity, .replace_identity = replace_identity,
    .open_in_signer = open_signer};
  grotto_panel_set_host(panel, &host, nss_context_ref(ctx), (GDestroyNotify)nss_context_unref);
  g_object_set(panel, "show-window-controls", FALSE, NULL);
}
