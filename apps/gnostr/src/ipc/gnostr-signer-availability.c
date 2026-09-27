/* gnostr-signer-availability — see gnostr-signer-availability.h (nostrc-e5nz). */
#include "gnostr-signer-availability.h"
#include "../util/keystore.h"

#include <glib/gi18n.h>

#define DBUS_NAME "org.freedesktop.DBus"
#define DBUS_PATH "/org/freedesktop/DBus"
#define DBUS_IFACE "org.freedesktop.DBus"

static gboolean name_has_owner(GDBusConnection *bus, const char *name,
                               GCancellable *cancellable) {
  GVariant *r = g_dbus_connection_call_sync(bus, DBUS_NAME, DBUS_PATH, DBUS_IFACE,
                                            "NameHasOwner", g_variant_new("(s)", name),
                                            G_VARIANT_TYPE("(b)"),
                                            G_DBUS_CALL_FLAGS_NONE, 5000,
                                            cancellable, NULL);
  gboolean owned = FALSE;
  if (r) {
    g_variant_get(r, "(b)", &owned);
    g_variant_unref(r);
  }
  return owned;
}

/* Plain D-Bus daemon calls: they never auto-start the signer (unlike a
 * method call on a proxy for org.nostr.Signer). */
static GnostrSignerPresence query_presence(GCancellable *cancellable,
                                           gboolean *out_approver) {
  *out_approver = FALSE;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, cancellable, NULL);
  if (!bus) return GNOSTR_SIGNER_PRESENCE_NO_BUS;

  GnostrSignerPresence presence = GNOSTR_SIGNER_PRESENCE_NOT_INSTALLED;
  GVariant *r = NULL;
  *out_approver = name_has_owner(bus, GNOSTR_SIGNER_APPROVER_BUS_NAME, cancellable);
  if (name_has_owner(bus, GNOSTR_SIGNER_BUS_NAME, cancellable)) {
    presence = GNOSTR_SIGNER_PRESENCE_RUNNING;
  } else {
    r = g_dbus_connection_call_sync(bus, DBUS_NAME, DBUS_PATH, DBUS_IFACE,
                                    "ListActivatableNames", NULL,
                                    G_VARIANT_TYPE("(as)"),
                                    G_DBUS_CALL_FLAGS_NONE, 5000, cancellable, NULL);
    if (r) {
      g_autofree const gchar **names = NULL;
      g_variant_get(r, "(^a&s)", &names);
      for (gsize i = 0; names && names[i]; i++) {
        if (g_strcmp0(names[i], GNOSTR_SIGNER_BUS_NAME) == 0) {
          presence = GNOSTR_SIGNER_PRESENCE_ACTIVATABLE;
          break;
        }
      }
      g_variant_unref(r);
    }
  }
  g_object_unref(bus);
  return presence;
}

static void status_query_thread(GTask *task, gpointer source, gpointer data,
                                GCancellable *cancellable) {
  (void)source;
  (void)data;
  GnostrSignerStatus *st = g_new0(GnostrSignerStatus, 1);
  st->presence = query_presence(cancellable, &st->approver_running);
  /* Attribute-only: never unlocks the keyring or loads a secret. */
  GList *legacy = gnostr_keystore_list_legacy_keys(NULL);
  st->legacy_keys = g_list_length(legacy);
  g_list_free_full(legacy, (GDestroyNotify)gnostr_key_info_free);
  st->legacy_auto_migrates = gnostr_keystore_legacy_migrates_automatically();
  /* nostrc-jppi: how many identities the signer holds, from the same
   * attribute-only view - asking the signer itself could raise a prompt. */
  st->signer_keys = -1;
  if (gnostr_keystore_available()) {
    GError *kerr = NULL;
    GList *keys = gnostr_keystore_list_keys(&kerr);
    if (!kerr)
      st->signer_keys = (gint)g_list_length(keys);
    g_list_free_full(keys, (GDestroyNotify)gnostr_key_info_free);
    g_clear_error(&kerr);
  }
  g_task_return_pointer(task, st, g_free);
}

void gnostr_signer_status_query_async(GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data) {
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(task, gnostr_signer_status_query_async);
  g_task_run_in_thread(task, status_query_thread);
  g_object_unref(task);
}

gboolean gnostr_signer_status_query_finish(GAsyncResult *result,
                                           GnostrSignerStatus *out,
                                           GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, NULL), FALSE);
  GnostrSignerStatus *st = g_task_propagate_pointer(G_TASK(result), error);
  if (!st) return FALSE;
  if (out) *out = *st;
  g_free(st);
  return TRUE;
}

static void on_start_reply(GObject *source, GAsyncResult *res, gpointer user_data) {
  GTask *task = user_data;
  GError *error = NULL;
  GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
  if (r) {
    g_variant_unref(r);
    g_task_return_boolean(task, TRUE);
  } else {
    g_task_return_error(task, error);
  }
  g_object_unref(task);
}

static void on_start_bus(GObject *source, GAsyncResult *res, gpointer user_data) {
  (void)source;
  GTask *task = user_data;
  GError *error = NULL;
  GDBusConnection *bus = g_bus_get_finish(res, &error);
  if (!bus) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  /* Activation may have to wait for a keyring unlock prompt; allow it. */
  g_dbus_connection_call(bus, DBUS_NAME, DBUS_PATH, DBUS_IFACE, "StartServiceByName",
                         g_variant_new("(su)", GNOSTR_SIGNER_BUS_NAME, 0u),
                         G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 60000,
                         g_task_get_cancellable(task), on_start_reply, task);
  g_object_unref(bus);
}

void gnostr_signer_start_async(GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data) {
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(task, gnostr_signer_start_async);
  g_bus_get(G_BUS_TYPE_SESSION, cancellable, on_start_bus, task);
}

gboolean gnostr_signer_start_finish(GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, NULL), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

gboolean gnostr_signer_open_app(GError **error) {
  g_autofree char *program = g_find_program_in_path("gnostr-signer");
  if (!program) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        _("GNostr Signer is not installed"));
    return FALSE;
  }
  char *argv[] = { program, NULL };
  return g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, error);
}

/* ---- Copy ---- */

gboolean gnostr_signer_status_can_start(const GnostrSignerStatus *status) {
  return status && status->presence == GNOSTR_SIGNER_PRESENCE_ACTIVATABLE;
}

gboolean gnostr_signer_status_can_open(const GnostrSignerStatus *status) {
  return status && status->presence == GNOSTR_SIGNER_PRESENCE_RUNNING &&
         !status->approver_running;
}

char *gnostr_signer_status_login_text(const GnostrSignerStatus *status) {
  g_return_val_if_fail(status != NULL, NULL);
  switch (status->presence) {
    case GNOSTR_SIGNER_PRESENCE_RUNNING:
      if (status->signer_keys == 0)
        return g_strdup(_("GNostr Signer is running but holds no key yet. Create or "
                          "import one in GNostr Signer."));
      if (!status->approver_running)
        return g_strdup(_("GNostr Signer is running, but its window is closed. Signing "
                          "in asks for your approval there: open GNostr Signer first."));
      return NULL;
    case GNOSTR_SIGNER_PRESENCE_ACTIVATABLE:
      return g_strdup(_("GNostr Signer is installed but not running. Start it to "
                        "sign in with a key on this computer."));
    case GNOSTR_SIGNER_PRESENCE_NOT_INSTALLED:
      return g_strdup(_("GNostr Signer is not installed. Install it to keep your key "
                        "on this computer, or use a remote signer."));
    case GNOSTR_SIGNER_PRESENCE_NO_BUS:
    default:
      return g_strdup(_("No desktop session bus, so GNostr Signer cannot be reached. "
                        "Use a remote signer."));
  }
}

char *gnostr_signer_status_legacy_text(const GnostrSignerStatus *status) {
  g_return_val_if_fail(status != NULL, NULL);
  if (status->legacy_keys == 0) return NULL;
  const char *head = g_dngettext(NULL,
      "An older version of GNostr saved %u private key itself. GNostr no longer "
      "keeps keys; GNostr Signer does.",
      "An older version of GNostr saved %u private keys itself. GNostr no longer "
      "keeps keys; GNostr Signer does.",
      status->legacy_keys);
  const char *tail;
  if (!status->legacy_auto_migrates)
    tail = _("Import them in GNostr Signer, then delete the old “org.gnostr.Client” "
             "items from your keychain.");
#ifdef __APPLE__
  /* nostrc-de9h: the daemon reads each old item once; the Keychain may ask
   * to allow that, and a dismissed prompt only postpones the item. */
  else if (status->presence == GNOSTR_SIGNER_PRESENCE_RUNNING)
    tail = _("GNostr Signer moves them into its own Keychain items when it starts; if "
             "they are still listed, restart GNostr Signer and allow its Keychain "
             "access.");
  else if (status->presence == GNOSTR_SIGNER_PRESENCE_ACTIVATABLE)
    tail = _("Start GNostr Signer to move them into its own Keychain items (the "
             "Keychain may ask to allow this).");
  else
    tail = _("Install GNostr Signer; it moves them into its own Keychain items when it "
             "first starts.");
#else
  else if (status->presence == GNOSTR_SIGNER_PRESENCE_RUNNING)
    tail = _("GNostr Signer moves them into its keyring when it starts; if they are "
             "still listed, unlock your keyring and restart GNostr Signer.");
  else if (status->presence == GNOSTR_SIGNER_PRESENCE_ACTIVATABLE)
    tail = _("Start GNostr Signer to move them into its keyring (your keyring may ask "
             "to be unlocked).");
  else
    tail = _("Install GNostr Signer; it moves them into its keyring when it first starts.");
#endif
  g_autofree char *first = g_strdup_printf(head, status->legacy_keys);
  return g_strdup_printf("%s %s", first, tail);
}

gboolean gnostr_signer_status_is_read_only(const GnostrSignerStatus *status,
                                           GnostrSignerNeed need) {
  g_return_val_if_fail(status != NULL, FALSE);
  return need == GNOSTR_SIGNER_NEED_ACTIVE && status->presence != GNOSTR_SIGNER_PRESENCE_RUNNING;
}

char *gnostr_signer_status_banner_text(const GnostrSignerStatus *status,
                                       GnostrSignerNeed need) {
  g_return_val_if_fail(status != NULL, NULL);
  if (status->presence == GNOSTR_SIGNER_PRESENCE_RUNNING) return NULL;
  gboolean startable = status->presence == GNOSTR_SIGNER_PRESENCE_ACTIVATABLE;
  if (need == GNOSTR_SIGNER_NEED_ACTIVE) {
    /* Signing resumes by itself once the signer is back. */
    return g_strdup(startable
        ? _("GNostr Signer is not running. GNostr keeps no keys of its own, so it is "
            "read-only until the signer runs.")
        : _("GNostr Signer is not available. GNostr keeps no keys of its own, so it is "
            "read-only: install GNostr Signer or sign in with a remote signer."));
  }
  if (need == GNOSTR_SIGNER_NEED_SIGNED_OUT) {
    /* nostrc-vuwu: once the signer runs, the session resumes by itself if
     * the signer holds this account; otherwise the user signs in again. */
    return g_strdup(startable
        ? _("You are signed out: GNostr keeps no keys of its own and GNostr Signer is "
            "not running. Start it to continue; if it uses a different account, sign "
            "in again.")
        : _("You are signed out: GNostr keeps no keys of its own and GNostr Signer is "
            "not available. Install it, or sign in with a remote signer."));
  }
  if (status->legacy_keys > 0 && status->legacy_auto_migrates) {
    return g_strdup(status->presence == GNOSTR_SIGNER_PRESENCE_ACTIVATABLE
        ? _("Keys saved by an older GNostr are waiting for GNostr Signer. Start it to "
            "move them into its key store.")
        : _("Keys saved by an older GNostr are waiting for GNostr Signer. Install it to "
            "move them into its key store."));
  }
  return NULL;
}

char *gnostr_signer_status_approval_text(const GnostrSignerStatus *status,
                                         GnostrSignerApproval approval) {
  g_return_val_if_fail(status != NULL, NULL);
  if (status->presence != GNOSTR_SIGNER_PRESENCE_RUNNING)
    return NULL;
  switch (approval) {
    case GNOSTR_SIGNER_APPROVAL_NO_APPROVER:
      if (status->approver_running)
        return NULL;
      return g_strdup(_("GNostr Signer needs your approval for a request from GNostr, "
                        "but its window is closed. Open GNostr Signer to answer it."));
    case GNOSTR_SIGNER_APPROVAL_REFUSED:
      return g_strdup(_("GNostr Signer is set to refuse some of GNostr’s requests. "
                        "Change that in GNostr Signer to use them again."));
    case GNOSTR_SIGNER_APPROVAL_OK:
    default:
      return NULL;
  }
}
