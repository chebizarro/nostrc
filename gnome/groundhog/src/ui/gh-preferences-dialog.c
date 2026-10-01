#include "gh-preferences-dialog.h"

#include <glib/gi18n.h>
#include <string.h>

/* A relay scope takes at most 16 URLs (gh-relay-scope.h); the attachment
 * server list gets the same bound. */
#define GH_PREFERENCES_MAX_URLS 16

/* What every gated row says (charter §7.1: every disabled control says why). */
#define NOT_AVAILABLE N_("Not available in this version yet")

typedef enum {
  BINDING_CHOICE,  /* AdwComboRow <-> one of a fixed list of values */
  BINDING_LIST,    /* GtkListBox of URLs + AdwEntryRow to add one <-> as */
  BINDING_ADDRESS, /* AdwEntryRow, written on apply <-> s */
} BindingKind;

typedef struct {
  BindingKind kind;
  GhPreferencesDialog *self; /* borrowed: the dialog owns its bindings */
  const gchar *key;
  gboolean syncing;
  /* BINDING_CHOICE */
  AdwComboRow *row;
  GtkStringList *model;       /* the row's model: the choices, then any custom item */
  GPtrArray *values;          /* GVariant, one per choice */
  gboolean has_custom;        /* the key holds a value no choice has */
  gchar *(*custom_label)(GVariant *value);
  /* BINDING_LIST and BINDING_ADDRESS */
  AdwEntryRow *entry;
  GtkLabel *error;
  GtkListBox *list;           /* BINDING_LIST */
  GtkStringList *urls;        /* BINDING_LIST: the key's value, bound to list */
  gchar *(*normalize)(const gchar *text, gboolean allow_onion, GError **error);
  gboolean ordered;           /* BINDING_LIST: rows move up and down (attachment servers) */
} Binding;

struct _GhPreferencesDialog {
  AdwPreferencesDialog parent_instance;

  AdwSwitchRow *notifications_row;
  AdwComboRow *notification_privacy_row;
  AdwSwitchRow *sound_row;
  GtkLabel *notifications_note;
  AdwSwitchRow *remote_images_row;
  AdwSwitchRow *link_previews_row;
  AdwSwitchRow *profile_pictures_row;
  GtkLabel *web_note;
  AdwSwitchRow *filter_unknown_senders_row;
  AdwSwitchRow *verified_mls_groups_row;
  AdwSwitchRow *message_previews_row;
  AdwSwitchRow *enter_sends_row;
  AdwPreferencesGroup *disappearing_group;
  AdwComboRow *disappearing_row;
  AdwComboRow *retention_row;
  AdwComboRow *network_mode_row;
  AdwEntryRow *tor_address_row;
  GtkLabel *tor_address_error;
  AdwActionRow *tor_status_row;
  GtkLabel *proxy_note;
  GtkLabel *tor_unavailable_note;
  GtkLabel *tor_note;
  GtkListBox *discovery_list;
  AdwEntryRow *discovery_entry;
  GtkLabel *discovery_error;
  AdwPreferencesGroup *attachments_group;
  GtkListBox *blossom_list;
  AdwEntryRow *blossom_entry;
  GtkLabel *blossom_error;
  GtkListBox *attachment_info_list;
  AdwActionRow *attachment_limit_row;
  AdwActionRow *attachment_cache_row;
  GtkButton *attachment_clear_button;
  AdwAlertDialog *clear_attachments_dialog;
  AdwActionRow *no_account_row;
  AdwActionRow *account_row;
  AdwAvatar *account_avatar;
  AdwComboRow *signer_row;
  AdwSwitchRow *run_in_background_row;
  AdwPreferencesGroup *delete_group;
  AdwAlertDialog *delete_all_dialog;

  GSettings *settings;
  GhPreferencesFeatures features;
  GhPreferencesTorStatus tor_status;
  GHashTable *key_widgets; /* key -> borrowed widget */
  GPtrArray *bindings;     /* Binding */
  gulong settings_changed;
  gchar *account_npub;
  GhPreferencesForgetAsyncFunc forget_async;
  GhPreferencesForgetFinishFunc forget_finish;
  GObject *forget_target;
  gboolean forgetting;
  gboolean disposed; /* template children are gone; a forget may still finish */
  gchar *last_toast;
  /* G22: the account's attachment cache and upload consents. */
  GhPreferencesAttachments attachments;
  gboolean has_attachments;
  gpointer attachments_data;
  GDestroyNotify attachments_destroy;
  Binding *blossom_binding;
};

G_DEFINE_FINAL_TYPE(GhPreferencesDialog, gh_preferences_dialog, ADW_TYPE_PREFERENCES_DIALOG)

enum { PROP_0, PROP_SETTINGS, PROP_FEATURES, N_PROPS };
static GParamSpec *properties[N_PROPS];

/* The one table of rows that need a feature this build may lack
 * (gh-preferences-dialog.h). Every other row is always live.
 * tests/check_privacy.py (preference-consumers) reads it with
 * src/app/gh-features.h: a key is gated while one of its features is 0
 * there, and must have a consumer outside this dialog otherwise. */
static const struct {
  const gchar *key;
  GhPreferencesFeatures needs;
} key_features[] = {
  { "notifications-enabled", GH_PREFERENCES_FEATURE_NOTIFICATIONS },
  { "notification-privacy", GH_PREFERENCES_FEATURE_NOTIFICATIONS },
  { "sound-enabled", GH_PREFERENCES_FEATURE_NOTIFICATIONS },
  { "load-remote-images", GH_PREFERENCES_FEATURE_REMOTE_IMAGES },
  { "link-previews", GH_PREFERENCES_FEATURE_LINK_PREVIEWS },
  { "load-profile-pictures", GH_PREFERENCES_FEATURE_PROFILE_PICTURES },
  { "filter-unknown-senders", GH_PREFERENCES_FEATURE_REQUEST_FILTER },
  { "enter-sends", GH_PREFERENCES_FEATURE_COMPOSER },
  { "default-disappearing-seconds",
    GH_PREFERENCES_FEATURE_COMPOSER | GH_PREFERENCES_FEATURE_EXPIRY },
  { "retention-days", GH_PREFERENCES_FEATURE_EXPIRY },
  { "blossom-servers", GH_PREFERENCES_FEATURE_ATTACHMENTS },
  { "tor-socks-address", GH_PREFERENCES_FEATURE_TOR },
  { "only-join-verified-mls-groups", GH_PREFERENCES_FEATURE_ENCRYPTED_GROUPS },
};

static gboolean
key_available(GhPreferencesDialog *self, const gchar *key)
{
  for (guint i = 0; i < G_N_ELEMENTS(key_features); i++)
    if (g_str_equal(key_features[i].key, key))
      return (self->features & key_features[i].needs) == key_features[i].needs;
  return TRUE;
}

/* A row this build can't honour: unbound, insensitive, and saying why. */
static void
gate_row(AdwActionRow *row, const gchar *reason)
{
  adw_action_row_set_subtitle(row, reason);
  gtk_widget_set_sensitive(GTK_WIDGET(row), FALSE);
}

const gchar *
gh_preferences_dialog_not_available_text(void)
{
  return _(NOT_AVAILABLE);
}

/* The boolean keys and their switch rows. */
static const struct {
  const gchar *key;
  gsize offset;
} switch_rows[] = {
  { "notifications-enabled", G_STRUCT_OFFSET(GhPreferencesDialog, notifications_row) },
  { "sound-enabled", G_STRUCT_OFFSET(GhPreferencesDialog, sound_row) },
  { "load-remote-images", G_STRUCT_OFFSET(GhPreferencesDialog, remote_images_row) },
  { "link-previews", G_STRUCT_OFFSET(GhPreferencesDialog, link_previews_row) },
  { "load-profile-pictures", G_STRUCT_OFFSET(GhPreferencesDialog, profile_pictures_row) },
  { "filter-unknown-senders",
    G_STRUCT_OFFSET(GhPreferencesDialog, filter_unknown_senders_row) },
  { "only-join-verified-mls-groups",
    G_STRUCT_OFFSET(GhPreferencesDialog, verified_mls_groups_row) },
  { "show-message-previews", G_STRUCT_OFFSET(GhPreferencesDialog, message_previews_row) },
  { "enter-sends", G_STRUCT_OFFSET(GhPreferencesDialog, enter_sends_row) },
  { "run-in-background", G_STRUCT_OFFSET(GhPreferencesDialog, run_in_background_row) },
};

/* ---- URL rules ---------------------------------------------------------------- */

static gboolean
host_is_loopback(const gchar *host)
{
  if (g_str_equal(host, "localhost"))
    return TRUE;
  g_autoptr(GInetAddress) address = g_inet_address_new_from_string(host);
  return address && g_inet_address_get_is_loopback(address);
}

static gpointer
invalid(GError **error, const gchar *message)
{
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, message);
  return NULL;
}

/* secure is the required scheme; plain (its unencrypted twin) is allowed only
 * for loopback when plain_loopback, or for a .onion host when allow_onion. */
static gchar *
normalize_url(const gchar *text, const gchar *secure, const gchar *plain,
              gboolean plain_loopback, gboolean allow_onion, const gchar *scheme_message,
              const gchar *plain_message, GError **error)
{
  g_autofree gchar *trimmed = g_strstrip(g_strdup(text ? text : ""));
  if (!*trimmed)
    return invalid(error, scheme_message);
  g_autoptr(GUri) uri = g_uri_parse(trimmed, G_URI_FLAGS_NONE, NULL);
  const gchar *scheme = uri ? g_uri_get_scheme(uri) : NULL;
  if (g_strcmp0(scheme, secure) != 0 && g_strcmp0(scheme, plain) != 0)
    return invalid(error, scheme_message);
  if (g_uri_get_userinfo(uri))
    return invalid(error, _("Addresses can't contain a user name or password"));
  const gchar *raw_host = g_uri_get_host(uri);
  if (!raw_host || !*raw_host)
    return invalid(error, _("The address has no host name"));
  if (g_uri_get_query(uri) || g_uri_get_fragment(uri))
    return invalid(error, _("Addresses can't contain “?” or “#” parts"));
  g_autofree gchar *host = g_ascii_strdown(raw_host, -1);
  if (g_str_equal(scheme, plain) && !(plain_loopback && host_is_loopback(host)) &&
      !(allow_onion && g_str_has_suffix(host, ".onion")))
    return invalid(error, plain_message);
  g_autofree gchar *path = g_strdup(g_uri_get_path(uri));
  gsize len = strlen(path);
  while (len > 0 && path[len - 1] == '/')
    path[--len] = '\0';
  return g_uri_join(G_URI_FLAGS_NONE, scheme, NULL, host, g_uri_get_port(uri), path, NULL,
                    NULL);
}

gchar *
gh_preferences_normalize_relay_url(const gchar *url, gboolean allow_onion, GError **error)
{
  return normalize_url(
    url, "wss", "ws", TRUE, allow_onion, _("Relay addresses start with wss://"),
    _("Use a wss:// address: unencrypted ws:// is only allowed for this computer"), error);
}

gchar *
gh_preferences_normalize_server_url(const gchar *url, gboolean allow_onion, GError **error)
{
  return normalize_url(url, "https", "http", FALSE, allow_onion,
                       _("Server addresses start with https://"),
                       _("Use an https:// address: unencrypted http:// isn't allowed"), error);
}

gboolean
gh_preferences_validate_socks_address(const gchar *address, GError **error)
{
  g_autofree gchar *trimmed = g_strstrip(g_strdup(address ? address : ""));
  g_autoptr(GSocketConnectable) parsed =
    *trimmed && !strstr(trimmed, "://") ? g_network_address_parse(trimmed, 0, NULL) : NULL;
  if (!parsed || g_network_address_get_port(G_NETWORK_ADDRESS(parsed)) == 0 ||
      !*g_network_address_get_hostname(G_NETWORK_ADDRESS(parsed))) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        _("Enter the Tor proxy as host:port, such as 127.0.0.1:9050"));
    return FALSE;
  }
  return TRUE;
}

/* ---- helpers ---------------------------------------------------------------------- */

/* Sets an apply-button entry's text without offering to apply it. */
static void
entry_set_text_quietly(AdwEntryRow *entry, const gchar *text)
{
  adw_entry_row_set_show_apply_button(entry, FALSE);
  gtk_editable_set_text(GTK_EDITABLE(entry), text);
  adw_entry_row_set_show_apply_button(entry, TRUE);
}

static void
show_entry_error(Binding *binding, const gchar *message)
{
  gtk_label_set_text(binding->error, message);
  gtk_widget_set_visible(GTK_WIDGET(binding->error), TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(binding->entry), "error");
  gtk_accessible_update_property(GTK_ACCESSIBLE(binding->entry),
                                 GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, message, -1);
  gtk_accessible_announce(GTK_ACCESSIBLE(binding->entry), message,
                          GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
}

static void
clear_entry_error(Binding *binding)
{
  if (!gtk_widget_get_visible(GTK_WIDGET(binding->error)))
    return;
  gtk_widget_set_visible(GTK_WIDGET(binding->error), FALSE);
  gtk_widget_remove_css_class(GTK_WIDGET(binding->entry), "error");
  gtk_accessible_reset_property(GTK_ACCESSIBLE(binding->entry),
                                GTK_ACCESSIBLE_PROPERTY_DESCRIPTION);
}

static void
binding_free(gpointer data)
{
  Binding *binding = data;
  g_clear_pointer(&binding->values, g_ptr_array_unref);
  g_clear_object(&binding->urls);
  g_free(binding);
}

static Binding *
add_binding(GhPreferencesDialog *self, BindingKind kind, const gchar *key, GtkWidget *widget)
{
  Binding *binding = g_new0(Binding, 1);
  binding->kind = kind;
  binding->self = self;
  binding->key = key;
  g_ptr_array_add(self->bindings, binding);
  g_hash_table_insert(self->key_widgets, (gpointer)key, widget);
  return binding;
}

/* ---- choice rows ------------------------------------------------------------------ */

static void
choice_sync(Binding *binding)
{
  g_autoptr(GVariant) value = g_settings_get_value(binding->self->settings, binding->key);
  guint n_choices = binding->values->len;
  guint index = GTK_INVALID_LIST_POSITION;
  for (guint i = 0; i < n_choices && index == GTK_INVALID_LIST_POSITION; i++)
    if (g_variant_equal(g_ptr_array_index(binding->values, i), value))
      index = i;
  binding->syncing = TRUE;
  if (index == GTK_INVALID_LIST_POSITION) {
    g_autofree gchar *label = binding->custom_label(value);
    const gchar *const additions[] = { label, NULL };
    gtk_string_list_splice(binding->model, n_choices, binding->has_custom ? 1 : 0, additions);
    binding->has_custom = TRUE;
    index = n_choices;
  } else if (binding->has_custom) {
    gtk_string_list_remove(binding->model, n_choices);
    binding->has_custom = FALSE;
  }
  adw_combo_row_set_selected(binding->row, index);
  binding->syncing = FALSE;
}

static void
on_choice_selected(AdwComboRow *row, GParamSpec *pspec, Binding *binding)
{
  (void)pspec;
  if (binding->syncing)
    return;
  guint index = adw_combo_row_get_selected(row);
  /* The custom item only shows the key's value; choosing it changes nothing. */
  if (index >= binding->values->len)
    return;
  if (!g_settings_set_value(binding->self->settings, binding->key,
                            g_ptr_array_index(binding->values, index)))
    choice_sync(binding); /* not writable: show the value it kept */
}

/* values: floating or not, one per item of the row's GtkStringList model. A
 * row whose feature is missing shows its first choice (what the build does:
 * no notifications, no timer, keep everything) and binds nothing. */
static void
bind_choice(GhPreferencesDialog *self, AdwComboRow *row, const gchar *key,
            GVariant *const *values, guint n_values, gchar *(*custom_label)(GVariant *value))
{
  if (!key_available(self, key)) {
    for (guint i = 0; i < n_values; i++)
      g_variant_unref(g_variant_ref_sink(values[i]));
    g_hash_table_insert(self->key_widgets, (gpointer)key, row);
    adw_combo_row_set_selected(row, 0);
    gate_row(ADW_ACTION_ROW(row), _(NOT_AVAILABLE));
    return;
  }
  Binding *binding = add_binding(self, BINDING_CHOICE, key, GTK_WIDGET(row));
  binding->row = row;
  binding->model = GTK_STRING_LIST(adw_combo_row_get_model(row));
  binding->custom_label = custom_label;
  binding->values = g_ptr_array_new_with_free_func((GDestroyNotify)g_variant_unref);
  for (guint i = 0; i < n_values; i++)
    g_ptr_array_add(binding->values, g_variant_ref_sink(values[i]));
  g_return_if_fail(g_list_model_get_n_items(G_LIST_MODEL(binding->model)) == n_values);
  choice_sync(binding);
  g_signal_connect(row, "notify::selected", G_CALLBACK(on_choice_selected), binding);
}

/* A value set outside Groundhog that no choice has: shown as it is. */
static gchar *
unsupported_label(GVariant *value)
{
  if (!g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
    g_autofree gchar *text = g_variant_print(value, FALSE);
    return g_strdup_printf(_("Unsupported (%s)"), text);
  }
  const gchar *text = g_variant_get_string(value, NULL);
  if (g_str_equal(text, "tor")) /* network-mode without G09 */
    return g_strdup(_("Tor (not available in this version)"));
  return g_strdup_printf(_("Unsupported (“%s”)"), text);
}

static gchar *
days_label(GVariant *value)
{
  gint32 days = g_variant_get_int32(value);
  if (days < 0)
    return unsupported_label(value);
  return g_strdup_printf(ngettext("%d Day", "%d Days", days), days);
}

static gchar *
duration_label(GVariant *value)
{
  gint32 seconds = g_variant_get_int32(value);
  if (seconds <= 0)
    return unsupported_label(value);
  if (seconds % 86400 == 0)
    return g_strdup_printf(ngettext("%d Day", "%d Days", seconds / 86400), seconds / 86400);
  if (seconds % 3600 == 0)
    return g_strdup_printf(ngettext("%d Hour", "%d Hours", seconds / 3600), seconds / 3600);
  if (seconds % 60 == 0)
    return g_strdup_printf(ngettext("%d Minute", "%d Minutes", seconds / 60), seconds / 60);
  return g_strdup_printf(ngettext("%d Second", "%d Seconds", seconds), seconds);
}

/* ---- URL lists ---------------------------------------------------------------------- */

/* The list rules shared by both editors and gh_preferences_server_list_add(). */
static GStrv
url_list_add(const gchar *const *current, const gchar *text,
             gchar *(*normalize)(const gchar *, gboolean, GError **), gboolean allow_onion,
             GError **error)
{
  g_autofree gchar *url = normalize(text, allow_onion, error);
  if (!url)
    return NULL;
  guint n = 0;
  for (; current && current[n]; n++) {
    g_autofree gchar *normalized = normalize(current[n], TRUE, NULL);
    if (g_str_equal(normalized ? normalized : current[n], url)) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                          _("This address is already in the list"));
      return NULL;
    }
  }
  if (n >= GH_PREFERENCES_MAX_URLS) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                ngettext("Groundhog uses at most %d address here",
                         "Groundhog uses at most %d addresses here", GH_PREFERENCES_MAX_URLS),
                GH_PREFERENCES_MAX_URLS);
    return NULL;
  }
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
  if (current)
    g_strv_builder_addv(builder, (const gchar **)current);
  g_strv_builder_add(builder, url);
  return g_strv_builder_end(builder);
}

GStrv
gh_preferences_server_list_add(const gchar *const *servers, const gchar *url,
                               gboolean allow_onion, GError **error)
{
  return url_list_add(servers, url, gh_preferences_normalize_server_url, allow_onion, error);
}

GStrv
gh_preferences_server_list_remove(const gchar *const *servers, const gchar *url)
{
  g_autoptr(GStrvBuilder) kept = g_strv_builder_new();
  for (guint i = 0; servers && servers[i]; i++)
    if (g_strcmp0(servers[i], url) != 0)
      g_strv_builder_add(kept, servers[i]);
  return g_strv_builder_end(kept);
}

GStrv
gh_preferences_server_list_move(const gchar *const *servers, guint index, gint delta)
{
  GStrv moved = g_strdupv((gchar **)servers);
  if (!moved)
    return g_new0(gchar *, 1);
  guint n = g_strv_length(moved);
  gint64 to = (gint64)index + delta;
  if (index < n && to >= 0 && to < n) {
    gchar *item = moved[index];
    moved[index] = moved[to];
    moved[to] = item;
  }
  return moved;
}

static void
on_remove_url(GtkButton *button, Binding *binding)
{
  const gchar *url = g_object_get_data(G_OBJECT(button), "gh-url");
  g_auto(GStrv) current = g_settings_get_strv(binding->self->settings, binding->key);
  g_auto(GStrv) urls = gh_preferences_server_list_remove((const gchar *const *)current, url);
  g_settings_set_strv(binding->self->settings, binding->key, (const gchar *const *)urls);
}

/* The position of url in the key's list, or -1. */
static gint
url_index(const gchar *url, gchar **urls)
{
  for (guint i = 0; urls[i]; i++)
    if (g_str_equal(urls[i], url))
      return (gint)i;
  return -1;
}

static void
on_move_url(GtkButton *button, Binding *binding)
{
  const gchar *url = g_object_get_data(G_OBJECT(button), "gh-url");
  gint delta = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "gh-delta"));
  g_auto(GStrv) current = g_settings_get_strv(binding->self->settings, binding->key);
  gint index = url_index(url, current);
  if (index < 0)
    return;
  g_auto(GStrv) urls = gh_preferences_server_list_move((const gchar *const *)current,
                                                       (guint)index, delta);
  g_settings_set_strv(binding->self->settings, binding->key, (const gchar *const *)urls);
  /* The rows are made again: keyboard focus follows the moved server. */
  GtkListBoxRow *row = gtk_list_box_get_row_at_index(binding->list, index + delta);
  if (row)
    gtk_widget_grab_focus(GTK_WIDGET(row));
}

static void
on_revoke_consent(GtkButton *button, Binding *binding)
{
  GhPreferencesDialog *self = binding->self;
  const gchar *url = g_object_get_data(G_OBJECT(button), "gh-url");
  g_autoptr(GError) error = NULL;
  if (!self->has_attachments || !self->attachments.revoke_consent)
    return;
  if (!self->attachments.revoke_consent(self->attachments_data, url, &error)) {
    g_message("Groundhog could not take back an upload consent: %s", error->message);
    show_entry_error(binding, _("This can't be changed while the account's message storage "
                                "isn't open"));
    return;
  }
  /* The row loses its note and button. */
  gh_preferences_dialog_refresh_attachments(self);
}

static GtkWidget *
icon_button(const gchar *icon, const gchar *tooltip, const gchar *label, const gchar *url)
{
  GtkWidget *button = gtk_button_new_from_icon_name(icon);
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_set_tooltip_text(button, tooltip);
  gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, label,
                                 -1);
  g_object_set_data_full(G_OBJECT(button), "gh-url", g_strdup(url), g_free);
  return button;
}

static GtkWidget *
create_url_row(gpointer item, gpointer data)
{
  Binding *binding = data;
  GhPreferencesDialog *self = binding->self;
  const gchar *url = gtk_string_object_get_string(GTK_STRING_OBJECT(item));
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), url);
  adw_preferences_row_set_title_selectable(ADW_PREFERENCES_ROW(row), TRUE);
  gboolean editable = key_available(self, binding->key);

  if (binding->ordered && editable) {
    /* Attachment servers are tried in this order (G22). */
    guint n = g_list_model_get_n_items(G_LIST_MODEL(binding->urls));
    gint index = -1;
    for (guint i = 0; i < n && index < 0; i++)
      if (g_str_equal(gtk_string_list_get_string(binding->urls, i), url))
        index = (gint)i;
    if (self->has_attachments && self->attachments.get_consent &&
        self->attachments.get_consent(self->attachments_data, url)) {
      adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
                                  _("Uploads here are signed by your account"));
      g_autofree gchar *label = g_strdup_printf(_("Stop uploading to %s as your account"), url);
      GtkWidget *revoke = gtk_button_new_with_mnemonic(_("_Revoke"));
      gtk_widget_set_valign(revoke, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class(revoke, "flat");
      gtk_widget_set_tooltip_text(revoke, _("Stop uploading here as your account"));
      gtk_accessible_update_property(GTK_ACCESSIBLE(revoke), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                     label, -1);
      g_object_set_data_full(G_OBJECT(revoke), "gh-url", g_strdup(url), g_free);
      g_signal_connect(revoke, "clicked", G_CALLBACK(on_revoke_consent), binding);
      adw_action_row_add_suffix(ADW_ACTION_ROW(row), revoke);
    }
    g_autofree gchar *up_label = g_strdup_printf(_("Move %s up"), url);
    GtkWidget *up = icon_button("go-up-symbolic", _("Move Up"), up_label, url);
    g_object_set_data(G_OBJECT(up), "gh-delta", GINT_TO_POINTER(-1));
    gtk_widget_set_sensitive(up, index > 0);
    g_signal_connect(up, "clicked", G_CALLBACK(on_move_url), binding);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), up);
    g_autofree gchar *down_label = g_strdup_printf(_("Move %s down"), url);
    GtkWidget *down = icon_button("go-down-symbolic", _("Move Down"), down_label, url);
    g_object_set_data(G_OBJECT(down), "gh-delta", GINT_TO_POINTER(1));
    gtk_widget_set_sensitive(down, index >= 0 && (guint)index + 1 < n);
    g_signal_connect(down, "clicked", G_CALLBACK(on_move_url), binding);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), down);
  }

  g_autofree gchar *label = g_strdup_printf(_("Remove %s"), url);
  GtkWidget *remove = icon_button("user-trash-symbolic", _("Remove"), label, url);
  g_signal_connect(remove, "clicked", G_CALLBACK(on_remove_url), binding);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove);
  return row;
}

/* force: make every row again (e.g. a consent changed), keyboard focus or
 * not. */
static void
list_sync_full(Binding *binding, gboolean force)
{
  g_auto(GStrv) urls = g_settings_get_strv(binding->self->settings, binding->key);
  GListModel *model = G_LIST_MODEL(binding->urls);
  guint n = g_list_model_get_n_items(model);
  gboolean same = n == g_strv_length(urls);
  for (guint i = 0; same && i < n; i++)
    same = g_str_equal(gtk_string_list_get_string(binding->urls, i), urls[i]);
  if (!same || force) /* rebuilding the rows would move keyboard focus for nothing */
    gtk_string_list_splice(binding->urls, 0, n, (const gchar *const *)urls);
}

static void
list_sync(Binding *binding)
{
  list_sync_full(binding, FALSE);
}

static void
on_list_apply(AdwEntryRow *entry, Binding *binding)
{
  g_autoptr(GError) error = NULL;
  GhPreferencesDialog *self = binding->self;
  g_auto(GStrv) current = g_settings_get_strv(self->settings, binding->key);
  g_auto(GStrv) urls =
    url_list_add((const gchar *const *)current, gtk_editable_get_text(GTK_EDITABLE(entry)),
                 binding->normalize, (self->features & GH_PREFERENCES_FEATURE_TOR) != 0, &error);
  if (!urls) {
    show_entry_error(binding, error->message);
    return;
  }
  if (!g_settings_set_strv(self->settings, binding->key, (const gchar *const *)urls)) {
    show_entry_error(binding, _("This setting can't be changed"));
    return;
  }
  clear_entry_error(binding);
  entry_set_text_quietly(entry, "");
}

static void
on_entry_changed(GtkEditable *editable, Binding *binding)
{
  (void)editable;
  clear_entry_error(binding);
}

static Binding *
bind_list(GhPreferencesDialog *self, const gchar *key, GtkListBox *list, AdwEntryRow *entry,
          GtkLabel *error, gchar *(*normalize)(const gchar *, gboolean, GError **),
          gboolean ordered)
{
  Binding *binding = add_binding(self, BINDING_LIST, key, GTK_WIDGET(list));
  binding->list = list;
  binding->entry = entry;
  binding->error = error;
  binding->normalize = normalize;
  binding->ordered = ordered;
  binding->urls = gtk_string_list_new(NULL);
  gtk_list_box_bind_model(list, G_LIST_MODEL(binding->urls), create_url_row, binding, NULL);
  list_sync(binding);
  g_signal_connect(entry, "apply", G_CALLBACK(on_list_apply), binding);
  g_signal_connect(entry, "changed", G_CALLBACK(on_entry_changed), binding);
  /* Without its feature the list shows what is stored and nothing edits it
   * (its group says why). */
  if (!key_available(self, key)) {
    gtk_widget_set_sensitive(GTK_WIDGET(list), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(entry), FALSE);
  }
  return binding;
}

/* ---- Tor address ------------------------------------------------------------------- */

static void
address_sync(Binding *binding)
{
  g_autofree gchar *address = g_settings_get_string(binding->self->settings, binding->key);
  clear_entry_error(binding);
  entry_set_text_quietly(binding->entry, address);
}

static void
on_address_apply(AdwEntryRow *entry, Binding *binding)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *address = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry))));
  if (!gh_preferences_validate_socks_address(address, &error)) {
    show_entry_error(binding, error->message);
    return;
  }
  if (!g_settings_set_string(binding->self->settings, binding->key, address))
    show_entry_error(binding, _("This setting can't be changed"));
}

static void
bind_address(GhPreferencesDialog *self, const gchar *key, AdwEntryRow *entry, GtkLabel *error)
{
  Binding *binding = add_binding(self, BINDING_ADDRESS, key, GTK_WIDGET(entry));
  binding->entry = entry;
  binding->error = error;
  address_sync(binding);
  g_signal_connect(entry, "apply", G_CALLBACK(on_address_apply), binding);
  g_signal_connect(entry, "changed", G_CALLBACK(on_entry_changed), binding);
}

/* ---- network, account, delete ----------------------------------------------------- */

/* Tor rows exist only with G09 (charter §7.11: no fake support). With it the
 * page says, for the chosen mode, who can see the IP address (§4.2): relay
 * connections honour no proxy outside Tor mode (libwebsockets has none).
 * Without it a stored "tor", or a mode this build does not know, connects
 * to nothing (gh-relay-guard.h), and the page says so instead. */
static void
sync_network(GhPreferencesDialog *self)
{
  gboolean tor_available = (self->features & GH_PREFERENCES_FEATURE_TOR) != 0;
  g_autofree gchar *mode = g_settings_get_string(self->settings, "network-mode");
  gboolean tor = tor_available && g_str_equal(mode, "tor");
  gboolean refused = !tor_available && !g_str_equal(mode, "system") &&
                     !g_str_equal(mode, "none");
  gtk_widget_set_visible(GTK_WIDGET(self->tor_address_row), tor);
  if (!tor)
    gtk_widget_set_visible(GTK_WIDGET(self->tor_address_error), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(self->tor_note), tor);
  gtk_widget_set_visible(GTK_WIDGET(self->tor_unavailable_note), refused);
  gtk_widget_set_visible(GTK_WIDGET(self->proxy_note), !tor && !refused);
  if (tor_available && !tor)
    gtk_label_set_text(self->proxy_note,
                       g_str_equal(mode, "system")
                         ? _("Groundhog connects to relays directly, so relays and your "
                             "network can see your IP address. Only web lookups, such as "
                             "checking an address, use the system's proxy settings. Choose Tor "
                             "to hide your IP address.")
                         : _("Groundhog connects directly, so relays, websites and your "
                             "network can see your IP address. Choose Tor to hide it."));
  gboolean status = tor && self->tor_status != GH_PREFERENCES_TOR_STATUS_UNKNOWN;
  gtk_widget_set_visible(GTK_WIDGET(self->tor_status_row), status);
  if (!status)
    return;
  g_autofree gchar *address = g_settings_get_string(self->settings, "tor-socks-address");
  g_autofree gchar *subtitle = NULL;
  switch (self->tor_status) {
  case GH_PREFERENCES_TOR_STATUS_CHECKING:
    subtitle = g_strdup(_("Checking…"));
    break;
  case GH_PREFERENCES_TOR_STATUS_REACHABLE:
    subtitle = g_strdup_printf(_("Tor is reachable at %s"), address);
    break;
  case GH_PREFERENCES_TOR_STATUS_UNREACHABLE:
  default:
    subtitle = g_strdup_printf(_("Can't reach Tor at %s — Groundhog won't connect without it"),
                               address);
    break;
  }
  adw_action_row_set_subtitle(self->tor_status_row, subtitle);
}

void
gh_preferences_dialog_set_tor_status(GhPreferencesDialog *self, GhPreferencesTorStatus status)
{
  g_return_if_fail(GH_IS_PREFERENCES_DIALOG(self));
  self->tor_status = status;
  if (!self->disposed)
    sync_network(self);
}

/* Content and sound exist only with notifications, and only matter while
 * they are on. */
static void
sync_notifications(GhPreferencesDialog *self)
{
  gboolean available = key_available(self, "notifications-enabled");
  gboolean on = available && adw_switch_row_get_active(self->notifications_row);
  gtk_widget_set_visible(GTK_WIDGET(self->notification_privacy_row), available);
  gtk_widget_set_visible(GTK_WIDGET(self->sound_row), available);
  gtk_widget_set_visible(GTK_WIDGET(self->notifications_note), available);
  if (available) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->notification_privacy_row), on);
    gtk_widget_set_sensitive(GTK_WIDGET(self->sound_row), on);
  }
}

/* The copy says only what this build does. */
static void
sync_copy(GhPreferencesDialog *self)
{
  gboolean web = key_available(self, "load-remote-images") ||
                 key_available(self, "link-previews") ||
                 key_available(self, "load-profile-pictures");
  gtk_widget_set_visible(GTK_WIDGET(self->web_note), web);
  gtk_label_set_text(self->web_note,
                     self->features & GH_PREFERENCES_FEATURE_TOR
                       ? _("Loading anything from the web shows your IP address to that "
                           "website unless you use Tor.")
                       : _("Loading anything from the web shows your IP address to that "
                           "website."));
  /* G07: expired messages leave the store and the model; a sent message's
   * seal and wrap carry expirations up to a day later (NIP-40, PT-7). */
  const gchar *disappearing = NULL;
  if (self->features & GH_PREFERENCES_FEATURE_EXPIRY)
    disappearing = self->features & GH_PREFERENCES_FEATURE_COMPOSER
      ? _("Messages that carry a timer are deleted from this device when they expire. "
          "Messages you send also ask relays to delete them within about a day after that; "
          "relays and other people's apps may not honour this.")
      : _("Messages that carry a timer are deleted from this device when they expire.");
  adw_preferences_group_set_description(self->disappearing_group, disappearing);
  /* G22: what an attachment server learns, as this build does it. */
  const gchar *attachments;
  if (!key_available(self, "blossom-servers"))
    attachments = _("Encrypted attachments are stored on these servers. None are set by "
                    "default, and sending attachments isn't available in this version yet. A "
                    "server can see your IP address, when you upload or download, and the size "
                    "of each file.");
  else if (self->features & GH_PREFERENCES_FEATURE_TOR)
    attachments = _("Files are encrypted on this device, then uploaded to the first server in "
                    "this list that accepts them. None is set by default: Groundhog asks when "
                    "you first send a file. A server can see when you upload or download and "
                    "the size of each file, and your IP address unless you use Tor.");
  else
    attachments = _("Files are encrypted on this device, then uploaded to the first server in "
                    "this list that accepts them. None is set by default: Groundhog asks when "
                    "you first send a file. A server can see your IP address, when you upload "
                    "or download, and the size of each file.");
  adw_preferences_group_set_description(self->attachments_group, attachments);
}

/* ---- attachments (G22) -------------------------------------------------------------- */

static void
sync_attachment_rows(GhPreferencesDialog *self)
{
  gboolean shown = self->has_attachments && key_available(self, "blossom-servers");
  gtk_widget_set_visible(GTK_WIDGET(self->attachment_info_list), shown);
  if (!shown) {
    gtk_widget_action_set_enabled(GTK_WIDGET(self), "prefs.clear-attachments", FALSE);
    return;
  }
  guint64 max = self->attachments.max_file_size;
  gtk_widget_set_visible(GTK_WIDGET(self->attachment_limit_row), max > 0);
  if (max > 0) {
    g_autofree gchar *size = g_format_size(max);
    adw_action_row_set_subtitle(self->attachment_limit_row, size);
  }
  gint64 bytes = 0;
  g_autoptr(GError) error = NULL;
  gboolean known = self->attachments.get_cache_size &&
                   self->attachments.get_cache_size(self->attachments_data, &bytes, &error);
  g_autofree gchar *subtitle = NULL;
  if (!known)
    subtitle = g_strdup(_("Shown while an account's message storage is open"));
  else if (bytes == 0)
    subtitle = g_strdup(_("None on this device"));
  else {
    g_autofree gchar *size = g_format_size((guint64)bytes);
    /* TRANSLATORS: e.g. "12.3 MB, encrypted on this device". */
    subtitle = g_strdup_printf(_("%s, encrypted on this device"), size);
  }
  adw_action_row_set_subtitle(self->attachment_cache_row, subtitle);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "prefs.clear-attachments",
                                known && bytes > 0 && self->attachments.clear_cache);
}

static void
on_clear_attachments_response(AdwAlertDialog *dialog, const gchar *response,
                              GhPreferencesDialog *self)
{
  (void)dialog;
  if (g_strcmp0(response, "clear") != 0 || !self->has_attachments ||
      !self->attachments.clear_cache)
    return;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *message = NULL;
  if (self->attachments.clear_cache(self->attachments_data, &error))
    message = g_strdup(_("Downloaded files were deleted from this device"));
  else
    message = g_strdup_printf(_("Couldn't delete the downloaded files: %s"), error->message);
  g_free(self->last_toast);
  self->last_toast = g_strdup(message);
  adw_preferences_dialog_add_toast(ADW_PREFERENCES_DIALOG(self), adw_toast_new(message));
  sync_attachment_rows(self);
}

static void
clear_attachments_activated(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  GhPreferencesDialog *self = GH_PREFERENCES_DIALOG(widget);
  (void)action;
  (void)parameter;
  if (self->has_attachments && self->attachments.clear_cache)
    adw_dialog_present(ADW_DIALOG(self->clear_attachments_dialog), widget);
}

void
gh_preferences_dialog_set_attachments(GhPreferencesDialog *self,
                                      const GhPreferencesAttachments *attachments,
                                      gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail(GH_IS_PREFERENCES_DIALOG(self));
  if (self->attachments_destroy)
    self->attachments_destroy(self->attachments_data);
  self->attachments_destroy = NULL;
  self->attachments_data = NULL;
  memset(&self->attachments, 0, sizeof self->attachments);
  self->has_attachments = attachments != NULL;
  if (attachments) {
    self->attachments = *attachments;
    self->attachments_data = data;
    self->attachments_destroy = destroy;
  }
  if (self->disposed) {
    if (self->attachments_destroy)
      self->attachments_destroy(self->attachments_data);
    self->attachments_destroy = NULL;
    self->has_attachments = FALSE;
    return;
  }
  gh_preferences_dialog_refresh_attachments(self);
}

void
gh_preferences_dialog_refresh_attachments(GhPreferencesDialog *self)
{
  g_return_if_fail(GH_IS_PREFERENCES_DIALOG(self));
  if (self->disposed)
    return;
  sync_attachment_rows(self);
  if (self->blossom_binding)
    list_sync_full(self->blossom_binding, TRUE);
}

static void
on_settings_changed(GSettings *settings, const gchar *key, GhPreferencesDialog *self)
{
  (void)settings;
  for (guint i = 0; i < self->bindings->len; i++) {
    Binding *binding = g_ptr_array_index(self->bindings, i);
    if (!g_str_equal(binding->key, key))
      continue;
    switch (binding->kind) {
    case BINDING_CHOICE:
      choice_sync(binding);
      break;
    case BINDING_LIST:
      list_sync(binding);
      break;
    case BINDING_ADDRESS:
      address_sync(binding);
      break;
    default:
      g_assert_not_reached();
    }
  }
  if (g_str_equal(key, "network-mode") || g_str_equal(key, "tor-socks-address"))
    sync_network(self);
}

static gboolean
can_delete(GhPreferencesDialog *self)
{
  return self->account_npub && self->forget_async && !self->forgetting;
}

static void
sync_delete(GhPreferencesDialog *self)
{
  gtk_widget_set_visible(GTK_WIDGET(self->delete_group),
                         self->account_npub && self->forget_async);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "prefs.delete-all", can_delete(self));
}

typedef struct {
  GhPreferencesDialog *self;
  GObject *target;
  GhPreferencesForgetFinishFunc finish;
} ForgetOp;

static void
forget_done(GObject *source, GAsyncResult *result, gpointer data)
{
  ForgetOp *op = data;
  GhPreferencesDialog *self = op->self;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *message = NULL;
  (void)source;
  GhPreferencesForgetResult done = op->finish(op->target, result, &error);
  self->forgetting = FALSE;
  if (done != GH_PREFERENCES_FORGET_DELETED)
    g_message("Groundhog could not delete all messages and their key: %s",
              error ? error->message : "unknown error");
  if (self->disposed) /* e.g. the window was destroyed at quit */
    goto out;
  adw_dialog_set_can_close(ADW_DIALOG(self), TRUE);
  sync_delete(self);
  switch (done) {
  case GH_PREFERENCES_FORGET_DELETED:
    message = g_strdup(_("All messages of the account were deleted from this device"));
    break;
  case GH_PREFERENCES_FORGET_KEY_KEPT:
    /* The files are gone; only the (now useless) key item stayed. */
    message = g_strdup(_("Messages deleted; the storage key couldn't be removed from the "
                         "keyring"));
    break;
  case GH_PREFERENCES_FORGET_FAILED:
  default:
    message = g_strdup_printf(_("Couldn't delete all messages: %s"),
                              error ? error->message : "");
    break;
  }
  g_free(self->last_toast);
  self->last_toast = g_strdup(message);
  adw_preferences_dialog_add_toast(ADW_PREFERENCES_DIALOG(self), adw_toast_new(message));
  gtk_accessible_announce(GTK_ACCESSIBLE(self), message,
                          done == GH_PREFERENCES_FORGET_FAILED
                            ? GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_HIGH
                            : GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
out:
  g_object_unref(op->target);
  g_object_unref(op->self);
  g_free(op);
}

static void
on_delete_all_response(AdwAlertDialog *dialog, const gchar *response, GhPreferencesDialog *self)
{
  (void)dialog;
  if (g_strcmp0(response, "delete") != 0 || !can_delete(self))
    return;
  ForgetOp *op = g_new0(ForgetOp, 1);
  op->self = g_object_ref(self);
  op->target = g_object_ref(self->forget_target);
  op->finish = self->forget_finish;
  self->forgetting = TRUE;
  /* The user confirmed: the deletion runs to its end, so the dialog stays
   * open to report it. */
  adw_dialog_set_can_close(ADW_DIALOG(self), FALSE);
  sync_delete(self);
  self->forget_async(op->target, self->account_npub, NULL, forget_done, op);
}

static void
delete_all_activated(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  GhPreferencesDialog *self = GH_PREFERENCES_DIALOG(widget);
  (void)action;
  (void)parameter;
  if (can_delete(self))
    adw_dialog_present(ADW_DIALOG(self->delete_all_dialog), widget);
}

static gchar *
short_npub(const gchar *npub)
{
  gsize len = strlen(npub);
  return len > 16 ? g_strdup_printf("%.10s…%s", npub, npub + len - 6) : g_strdup(npub);
}

/* ---- public ----------------------------------------------------------------------- */

void
gh_preferences_dialog_set_account(GhPreferencesDialog *self, const gchar *npub,
                                  const gchar *label)
{
  g_return_if_fail(GH_IS_PREFERENCES_DIALOG(self));
  if (self->disposed)
    return;
  g_free(self->account_npub);
  self->account_npub = npub && *npub ? g_strdup(npub) : NULL;
  gboolean active = self->account_npub != NULL;
  gtk_widget_set_visible(GTK_WIDGET(self->no_account_row), !active);
  gtk_widget_set_visible(GTK_WIDGET(self->account_row), active);
  if (active) {
    g_autofree gchar *title = label && *label ? g_strdup(label) : short_npub(npub);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->account_row), title);
    adw_action_row_set_subtitle(self->account_row, npub);
    adw_avatar_set_text(self->account_avatar, title);
  }
  sync_delete(self);
}

void
gh_preferences_dialog_set_forget_func(GhPreferencesDialog *self,
                                      GhPreferencesForgetAsyncFunc forget_async,
                                      GhPreferencesForgetFinishFunc forget_finish,
                                      GObject *target)
{
  g_return_if_fail(GH_IS_PREFERENCES_DIALOG(self));
  g_return_if_fail((forget_async == NULL) == (forget_finish == NULL));
  g_return_if_fail(!forget_async || G_IS_OBJECT(target));
  if (self->disposed)
    return;
  self->forget_async = forget_async;
  self->forget_finish = forget_finish;
  g_set_object(&self->forget_target, forget_async ? target : NULL);
  sync_delete(self);
}

GtkWidget *
gh_preferences_dialog_get_key_widget(GhPreferencesDialog *self, const gchar *key)
{
  g_return_val_if_fail(GH_IS_PREFERENCES_DIALOG(self), NULL);
  return key && !self->disposed ? g_hash_table_lookup(self->key_widgets, key) : NULL;
}

const gchar *
gh_preferences_dialog_get_last_toast(GhPreferencesDialog *self)
{
  g_return_val_if_fail(GH_IS_PREFERENCES_DIALOG(self), NULL);
  return self->last_toast;
}

gboolean
gh_preferences_dialog_get_key_available(GhPreferencesDialog *self, const gchar *key)
{
  g_return_val_if_fail(GH_IS_PREFERENCES_DIALOG(self), FALSE);
  return key && g_hash_table_contains(self->key_widgets, key) && key_available(self, key);
}

GhPreferencesDialog *
gh_preferences_dialog_new(GSettings *settings, GhPreferencesFeatures features)
{
  g_return_val_if_fail(G_IS_SETTINGS(settings), NULL);
  return g_object_new(GH_TYPE_PREFERENCES_DIALOG, "settings", settings, "features",
                      (guint)features, NULL);
}

/* ---- GObject ---------------------------------------------------------------------- */

static void
gh_preferences_dialog_constructed(GObject *object)
{
  GhPreferencesDialog *self = GH_PREFERENCES_DIALOG(object);
  G_OBJECT_CLASS(gh_preferences_dialog_parent_class)->constructed(object);
  g_return_if_fail(G_IS_SETTINGS(self->settings));

  /* Writability is not reflected in sensitivity: availability and the
   * Notifications switch decide it. */
  for (guint i = 0; i < G_N_ELEMENTS(switch_rows); i++) {
    const gchar *key = switch_rows[i].key;
    GtkWidget *row = G_STRUCT_MEMBER(GtkWidget *, self, switch_rows[i].offset);
    g_hash_table_insert(self->key_widgets, (gpointer)key, row);
    if (key_available(self, key)) {
      g_settings_bind(self->settings, key, row, "active",
                      G_SETTINGS_BIND_DEFAULT | G_SETTINGS_BIND_NO_SENSITIVITY);
      continue;
    }
    /* Unbound: the row shows what this build does. Message requests are
     * always kept apart (charter §7.9), so that filter is simply on. */
    gboolean always_on = g_str_equal(key, "filter-unknown-senders");
    adw_switch_row_set_active(ADW_SWITCH_ROW(row), always_on);
    gate_row(ADW_ACTION_ROW(row),
             always_on ? _("Always on in this version: messages from people you haven't "
                           "accepted go to Message Requests, without profile lookups")
                       : _(NOT_AVAILABLE));
  }
  /* Scoped to the dialog: a row may outlive it (its settings binding still
   * following the key) and must never call back into it. */
  g_signal_connect_object(self->notifications_row, "notify::active",
                          G_CALLBACK(sync_notifications), self, G_CONNECT_SWAPPED);

  GVariant *levels[] = { g_variant_new_string("hidden"), g_variant_new_string("sender"),
                         g_variant_new_string("preview") };
  bind_choice(self, self->notification_privacy_row, "notification-privacy", levels,
              G_N_ELEMENTS(levels), unsupported_label);
  /* Charter §3.7: off, 1 day, 1 week, 4 weeks. */
  GVariant *timers[] = { g_variant_new_int32(0), g_variant_new_int32(86400),
                         g_variant_new_int32(604800), g_variant_new_int32(2419200) };
  bind_choice(self, self->disappearing_row, "default-disappearing-seconds", timers,
              G_N_ELEMENTS(timers), duration_label);
  /* D10: forever, 1 year, 30 days. */
  GVariant *retention[] = { g_variant_new_int32(0), g_variant_new_int32(365),
                            g_variant_new_int32(30) };
  bind_choice(self, self->retention_row, "retention-days", retention, G_N_ELEMENTS(retention),
              days_label);
  GVariant *modes[] = { g_variant_new_string("system"), g_variant_new_string("none"),
                        g_variant_new_string("tor") };
  guint n_modes = G_N_ELEMENTS(modes);
  if (!(self->features & GH_PREFERENCES_FEATURE_TOR)) {
    GtkStringList *model = GTK_STRING_LIST(adw_combo_row_get_model(self->network_mode_row));
    gtk_string_list_remove(model, --n_modes);
    g_variant_unref(g_variant_ref_sink(modes[n_modes]));
  }
  bind_choice(self, self->network_mode_row, "network-mode", modes, n_modes, unsupported_label);
  GVariant *signers[] = { g_variant_new_string("auto"), g_variant_new_string("local"),
                          g_variant_new_string("nip55l"), g_variant_new_string("nip46") };
  bind_choice(self, self->signer_row, "signer-method", signers, G_N_ELEMENTS(signers),
              unsupported_label);

  bind_address(self, "tor-socks-address", self->tor_address_row, self->tor_address_error);
  bind_list(self, "discovery-relays", self->discovery_list, self->discovery_entry,
            self->discovery_error, gh_preferences_normalize_relay_url, FALSE);
  self->blossom_binding = bind_list(self, "blossom-servers", self->blossom_list,
                                    self->blossom_entry, self->blossom_error,
                                    gh_preferences_normalize_server_url, TRUE);

  self->settings_changed = g_signal_connect(self->settings, "changed",
                                            G_CALLBACK(on_settings_changed), self);
  sync_network(self);
  sync_notifications(self);
  sync_copy(self);
  sync_delete(self);
  sync_attachment_rows(self);
}

static void
gh_preferences_dialog_set_property(GObject *object, guint prop_id, const GValue *value,
                                   GParamSpec *pspec)
{
  GhPreferencesDialog *self = GH_PREFERENCES_DIALOG(object);
  switch (prop_id) {
  case PROP_SETTINGS:
    self->settings = g_value_dup_object(value);
    break;
  case PROP_FEATURES:
    self->features = g_value_get_uint(value) & GH_PREFERENCES_FEATURES_ALL;
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
gh_preferences_dialog_get_property(GObject *object, guint prop_id, GValue *value,
                                   GParamSpec *pspec)
{
  GhPreferencesDialog *self = GH_PREFERENCES_DIALOG(object);
  switch (prop_id) {
  case PROP_SETTINGS:
    g_value_set_object(value, self->settings);
    break;
  case PROP_FEATURES:
    g_value_set_uint(value, self->features);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
gh_preferences_dialog_dispose(GObject *object)
{
  GhPreferencesDialog *self = GH_PREFERENCES_DIALOG(object);
  self->disposed = TRUE;
  if (self->settings_changed) {
    g_signal_handler_disconnect(self->settings, self->settings_changed);
    self->settings_changed = 0;
  }
  if (self->delete_all_dialog)
    g_signal_handlers_disconnect_by_func(self->delete_all_dialog, on_delete_all_response, self);
  if (self->clear_attachments_dialog)
    g_signal_handlers_disconnect_by_func(self->clear_attachments_dialog,
                                         on_clear_attachments_response, self);
  if (self->notifications_row)
    g_signal_handlers_disconnect_by_func(self->notifications_row, sync_notifications, self);
  /* The rows' handlers point into the bindings: drop the rows first. */
  if (self->discovery_list)
    gtk_list_box_bind_model(self->discovery_list, NULL, NULL, NULL, NULL);
  if (self->blossom_list)
    gtk_list_box_bind_model(self->blossom_list, NULL, NULL, NULL, NULL);
  gtk_widget_dispose_template(GTK_WIDGET(self), GH_TYPE_PREFERENCES_DIALOG);
  self->blossom_binding = NULL;
  g_clear_pointer(&self->bindings, g_ptr_array_unref);
  g_clear_object(&self->forget_target);
  if (self->attachments_destroy)
    self->attachments_destroy(self->attachments_data);
  self->attachments_destroy = NULL;
  self->attachments_data = NULL;
  self->has_attachments = FALSE;
  G_OBJECT_CLASS(gh_preferences_dialog_parent_class)->dispose(object);
}

static void
gh_preferences_dialog_finalize(GObject *object)
{
  GhPreferencesDialog *self = GH_PREFERENCES_DIALOG(object);
  g_clear_pointer(&self->key_widgets, g_hash_table_unref);
  g_clear_object(&self->settings);
  g_free(self->account_npub);
  g_free(self->last_toast);
  G_OBJECT_CLASS(gh_preferences_dialog_parent_class)->finalize(object);
}

static void
gh_preferences_dialog_class_init(GhPreferencesDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->constructed = gh_preferences_dialog_constructed;
  object_class->set_property = gh_preferences_dialog_set_property;
  object_class->get_property = gh_preferences_dialog_get_property;
  object_class->dispose = gh_preferences_dialog_dispose;
  object_class->finalize = gh_preferences_dialog_finalize;

  properties[PROP_SETTINGS] =
    g_param_spec_object("settings", NULL, NULL, G_TYPE_SETTINGS,
                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  /* GhPreferencesFeatures. */
  properties[PROP_FEATURES] =
    g_param_spec_uint("features", NULL, NULL, 0, GH_PREFERENCES_FEATURES_ALL, 0,
                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, N_PROPS, properties);

  gtk_widget_class_install_action(widget_class, "prefs.delete-all", NULL, delete_all_activated);
  gtk_widget_class_install_action(widget_class, "prefs.clear-attachments", NULL,
                                  clear_attachments_activated);

  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-preferences-dialog.ui");
#define BIND(name) gtk_widget_class_bind_template_child(widget_class, GhPreferencesDialog, name)
  BIND(notifications_row);
  BIND(notification_privacy_row);
  BIND(sound_row);
  BIND(notifications_note);
  BIND(remote_images_row);
  BIND(link_previews_row);
  BIND(profile_pictures_row);
  BIND(web_note);
  BIND(filter_unknown_senders_row);
  BIND(verified_mls_groups_row);
  BIND(message_previews_row);
  BIND(enter_sends_row);
  BIND(disappearing_group);
  BIND(disappearing_row);
  BIND(retention_row);
  BIND(network_mode_row);
  BIND(tor_address_row);
  BIND(tor_address_error);
  BIND(tor_status_row);
  BIND(proxy_note);
  BIND(tor_unavailable_note);
  BIND(tor_note);
  BIND(discovery_list);
  BIND(discovery_entry);
  BIND(discovery_error);
  BIND(attachments_group);
  BIND(blossom_list);
  BIND(blossom_entry);
  BIND(blossom_error);
  BIND(attachment_info_list);
  BIND(attachment_limit_row);
  BIND(attachment_cache_row);
  BIND(attachment_clear_button);
  BIND(clear_attachments_dialog);
  BIND(no_account_row);
  BIND(account_row);
  BIND(account_avatar);
  BIND(signer_row);
  BIND(run_in_background_row);
  BIND(delete_group);
  BIND(delete_all_dialog);
#undef BIND
  /* Privacy › Blocked Conversations, shown by gh_blocked_page_attach(). */
  gtk_widget_class_bind_template_child_full(widget_class, "blocked_group", FALSE, 0);
  gtk_widget_class_bind_template_child_full(widget_class, "blocked_row", FALSE, 0);
}

static void
gh_preferences_dialog_init(GhPreferencesDialog *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  self->key_widgets = g_hash_table_new(g_str_hash, g_str_equal);
  self->bindings = g_ptr_array_new_with_free_func(binding_free);
  g_signal_connect(self->delete_all_dialog, "response", G_CALLBACK(on_delete_all_response),
                   self);
  g_signal_connect(self->clear_attachments_dialog, "response",
                   G_CALLBACK(on_clear_attachments_response), self);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "prefs.delete-all", FALSE);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "prefs.clear-attachments", FALSE);
}
