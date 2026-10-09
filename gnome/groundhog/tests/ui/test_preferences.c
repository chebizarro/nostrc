/* GhPreferencesDialog (privacy charter §7.11, §8.2 G17) on a memory GSettings
 * backend: no relay, no signer, no network. Covered: every preference key of
 * org.nostr.Groundhog has exactly one row, inside the dialog, and each row is
 * bound both ways (switches, choice rows including a value no choice has, the
 * Tor address and both URL lists) when its feature exists; in this build
 * (src/app/gh-features.h) every row whose feature is missing is unbound,
 * insensitive and says why, and flips with its GH_FEATURE_* (W13b review
 * B2); the copy says only what the build does (disappearing messages,
 * retention, web content, autostart); the Tor choice and Tor address row are
 * absent until G09's feature is set, and present with it; the URL
 * rules reject non-ws(s)/https schemes, credentials, plain ws:// off loopback,
 * query and fragment, duplicates and a 17th entry; "Delete All Messages on This
 * Device" appears only with an account and a forget function, asks for
 * confirmation in an AdwAlertDialog and runs the forget function for the
 * account only after "Delete and Sign Out", keeping the dialog open until it
 * finished, and reports deleted, key kept and failed apart (a fake stands in
 * for GhAccountStore's forget); the 360x294 layout
 * and the accessible labels of the list rows. Needs a display: it self-skips
 * (77) without one. Waits iterate the main context against a deadline; they
 * never sleep. With GROUNDHOG_TEST_SCREENSHOTS=<dir> the screenshots case
 * renders every page, narrow, light and dark, to <dir>/groundhog-g17-*.png.
 */
#include "gh-features.h"
#include "gh-preferences-dialog.h"
#include "gh-qr-code.h"
#include "gh-window.h"

#include <string.h>

#include "nostrc-test-gdk-frame.h"

void groundhog_register_resource(void);

#define SCHEMA_ID "org.nostr.Groundhog"
#define NOT_AVAILABLE "Not available in this version yet"
/* The npub of the x-only key 79be667e...16f81798; the dialog only shows it. */
#define NPUB "npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqpkge6d"

/* ---- waits ------------------------------------------------------------------- */

static gboolean
deadline_hit(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

static void
spin_until_at(gboolean (*pred)(gpointer), gpointer data, int line)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(5, deadline_hit, &expired);
  while (!pred(data) && !expired)
    g_main_context_iteration(NULL, TRUE);
  if (expired)
    g_error("condition waited for at line %d did not hold within 5s", line);
  g_source_remove(timer);
}
#define spin_until(pred, data) spin_until_at((pred), (data), __LINE__)

static void
drain_idle(void)
{
  for (int i = 0; i < 200 && g_main_context_iteration(NULL, FALSE); i++)
    ;
}

/* ---- fixture ----------------------------------------------------------------- */

/* The schema keys that are not preferences (no row). */
static const char *const not_preferences[] = {
  "current-npub", "current-backend", "backend-migration-version", "signer-method",
  "window-width", "window-height", "window-maximized",
};

/* Charter §7.11, one row each. */
static const char *const preference_keys[] = {
  "notifications-enabled", "notification-privacy", "sound-enabled", "load-remote-images",
  "link-previews", "load-profile-pictures", "filter-unknown-senders", "show-message-previews",
  "network-mode", "tor-socks-address", "discovery-relays",
  "run-in-background", "launch-on-login", "diagnostics-enabled", "retention-days",
  "default-disappearing-seconds", "enter-sends",
  "blossom-servers", "only-join-verified-mls-groups", "mls-legacy-key-packages",
  "default-dm-protocol",
};

static const char *const switch_keys[] = {
  "notifications-enabled", "sound-enabled", "load-remote-images", "link-previews",
  "load-profile-pictures", "filter-unknown-senders", "show-message-previews", "enter-sends",
  "run-in-background", "launch-on-login", "diagnostics-enabled",
  "only-join-verified-mls-groups", "mls-legacy-key-packages",
};

typedef struct {
  GSettings *settings;
  GhPreferencesDialog *dialog;
  GtkWindow *window; /* only when a case presents the dialog */
} Fixture;

static void
reset_all(GSettings *settings)
{
  g_autoptr(GSettingsSchema) schema = NULL;
  g_object_get(settings, "settings-schema", &schema, NULL);
  g_auto(GStrv) keys = g_settings_schema_list_keys(schema);
  for (guint i = 0; keys[i]; i++)
    g_settings_reset(settings, keys[i]);
  drain_idle();
}

/* data: GUINT_TO_POINTER(GhPreferencesFeatures). */
static void
fixture_setup(Fixture *f, gconstpointer data)
{
  f->settings = g_settings_new(SCHEMA_ID);
  reset_all(f->settings);
  f->dialog = g_object_ref_sink(gh_preferences_dialog_new(f->settings, GPOINTER_TO_UINT(data)));
}

static void
fixture_teardown(Fixture *f, gconstpointer data)
{
  (void)data;
  if (f->window) {
    adw_dialog_force_close(ADW_DIALOG(f->dialog));
    gtk_window_destroy(f->window);
    drain_idle();
  }
  g_clear_object(&f->dialog);
  reset_all(f->settings);
  g_clear_object(&f->settings);
}

static gpointer
child(Fixture *f, const char *name)
{
  GObject *object = gtk_widget_get_template_child(GTK_WIDGET(f->dialog),
                                                  GH_TYPE_PREFERENCES_DIALOG, name);
  g_assert_nonnull(object);
  return object;
}

static gboolean
is_mapped(gpointer widget)
{
  return gtk_widget_get_mapped(GTK_WIDGET(widget));
}

typedef struct {
  GtkWidget *window;
  int width, height;
} SizeWait;

static gboolean
has_size(gpointer data)
{
  SizeWait *wait = data;
  return gtk_widget_get_width(wait->window) == wait->width &&
         gtk_widget_get_height(wait->window) == wait->height;
}

/* Presents window with a width x height content area. A non-composited
 * display (Xvfb in CI) draws a solid client-side border inside the default
 * size, which GNOME's compositor draws outside it: grow the default size by
 * that border so the content is the size the charter means (§7.12). */
static void
present_window(GtkWindow *window, int width, int height)
{
  gtk_window_set_default_size(window, width, height);
  gtk_window_present(window);
  spin_until(is_mapped, window);
  drain_idle();
  int border_x = width - gtk_widget_get_width(GTK_WIDGET(window));
  int border_y = height - gtk_widget_get_height(GTK_WIDGET(window));
  if (border_x > 0 || border_y > 0) {
    SizeWait wait = { GTK_WIDGET(window), width, height };
    gtk_window_set_default_size(window, width + MAX(border_x, 0), height + MAX(border_y, 0));
    spin_until(has_size, &wait);
  }
}

static void
present(Fixture *f, int width, int height)
{
  f->window = GTK_WINDOW(adw_window_new());
  present_window(f->window, width, height);
  adw_dialog_present(ADW_DIALOG(f->dialog), GTK_WIDGET(f->window));
  spin_until(is_mapped, f->dialog);
  drain_idle();
}

/* ---- every key has one row --------------------------------------------------- */

static gboolean
in_list(const char *const *list, gsize n, const char *key)
{
  for (gsize i = 0; i < n; i++)
    if (g_str_equal(list[i], key))
      return TRUE;
  return FALSE;
}

static void
test_every_key_has_a_row(Fixture *f, gconstpointer data)
{
  (void)data;
  g_autoptr(GSettingsSchema) schema = NULL;
  g_object_get(f->settings, "settings-schema", &schema, NULL);
  g_auto(GStrv) keys = g_settings_schema_list_keys(schema);
  g_autoptr(GHashTable) widgets = g_hash_table_new(NULL, NULL);
  guint n_preferences = 0;
  /* An AdwDialog's content is inside it only while presented. */
  present(f, 800, 700);
  for (guint i = 0; keys[i]; i++) {
    GtkWidget *widget = gh_preferences_dialog_get_key_widget(f->dialog, keys[i]);
    if (in_list(not_preferences, G_N_ELEMENTS(not_preferences), keys[i])) {
      g_assert_null(widget);
      continue;
    }
    /* A key the schema gained without a row here fails: G01's allowlist and
     * this dialog change together (charter §8.4). */
    if (!in_list(preference_keys, G_N_ELEMENTS(preference_keys), keys[i]))
      g_error("schema key %s is neither a §7.11 preference nor known state", keys[i]);
    g_assert_nonnull(widget);
    g_assert_true(gtk_widget_is_ancestor(widget, GTK_WIDGET(f->dialog)));
    g_assert_false(g_hash_table_contains(widgets, widget));
    g_hash_table_add(widgets, widget);
    n_preferences++;
  }
  g_assert_cmpuint(n_preferences, ==, G_N_ELEMENTS(preference_keys));
  g_assert_null(gh_preferences_dialog_get_key_widget(f->dialog, "no-such-key"));
}

/* ---- switches ------------------------------------------------------------------ */

static void
test_switch_rows_bind_both_ways(Fixture *f, gconstpointer data)
{
  (void)data;
  for (guint i = 0; i < G_N_ELEMENTS(switch_keys); i++) {
    const char *key = switch_keys[i];
    GtkWidget *widget = gh_preferences_dialog_get_key_widget(f->dialog, key);
    g_assert_true(ADW_IS_SWITCH_ROW(widget));
    AdwSwitchRow *row = ADW_SWITCH_ROW(widget);
    gboolean initial = g_settings_get_boolean(f->settings, key);
    g_assert_cmpint(adw_switch_row_get_active(row), ==, initial);
    for (int round = 0; round < 2; round++) {
      gboolean value = round == 0 ? !initial : initial;
      g_settings_set_boolean(f->settings, key, value);
      drain_idle();
      g_assert_cmpint(adw_switch_row_get_active(row), ==, value);
      adw_switch_row_set_active(row, !value);
      drain_idle();
      g_assert_cmpint(g_settings_get_boolean(f->settings, key), ==, !value);
    }
    g_settings_reset(f->settings, key);
  }
  /* Notification content and sound follow the Notifications switch. */
  g_settings_set_boolean(f->settings, "notifications-enabled", FALSE);
  drain_idle();
  g_assert_false(gtk_widget_get_sensitive(
    gh_preferences_dialog_get_key_widget(f->dialog, "notification-privacy")));
  g_assert_false(gtk_widget_get_sensitive(
    gh_preferences_dialog_get_key_widget(f->dialog, "sound-enabled")));
  g_settings_set_boolean(f->settings, "notifications-enabled", TRUE);
  drain_idle();
  g_assert_true(gtk_widget_get_sensitive(
    gh_preferences_dialog_get_key_widget(f->dialog, "notification-privacy")));
  g_assert_true(gtk_widget_get_sensitive(
    gh_preferences_dialog_get_key_widget(f->dialog, "sound-enabled")));
}

/* ---- choices ------------------------------------------------------------------- */

typedef struct {
  const char *key;
  const char *items[5];  /* the row's labels, in order */
  const char *values[5]; /* GVariant text of each item's value */
  const char *custom;    /* a valid value that no item has */
  const char *custom_label;
} Choice;

static const Choice choices[] = {
  { "notification-privacy", { "Hidden", "Sender Name", "Sender and Message" },
    { "'hidden'", "'sender'", "'preview'" }, "'everything'", "Unsupported (“everything”)" },
  { "default-disappearing-seconds", { "Off", "1 Day", "1 Week", "4 Weeks" },
    { "0", "86400", "604800", "2419200" }, "3600", "1 Hour" },
  { "retention-days", { "Forever", "1 Year", "30 Days" }, { "0", "365", "30" }, "90",
    "90 Days" },
  /* Without G09 there is no Tor item; a stored "tor" is shown, not offered. */
  { "network-mode", { "System Settings", "No Proxy" }, { "'system'", "'none'" }, "'tor'",
    "Tor (not available in this version)" },
};

static GVariant *
parse_value(GSettings *settings, const char *key, const char *text)
{
  g_autoptr(GSettingsSchema) schema = NULL;
  g_object_get(settings, "settings-schema", &schema, NULL);
  g_autoptr(GSettingsSchemaKey) schema_key = g_settings_schema_get_key(schema, key);
  g_autoptr(GError) error = NULL;
  GVariant *value = g_variant_parse(g_settings_schema_key_get_value_type(schema_key), text,
                                    NULL, NULL, &error);
  g_assert_no_error(error);
  return g_variant_ref_sink(value);
}

static void
assert_key(GSettings *settings, const char *key, const char *text)
{
  g_autoptr(GVariant) want = parse_value(settings, key, text);
  g_autoptr(GVariant) have = g_settings_get_value(settings, key);
  if (!g_variant_equal(want, have)) {
    g_autofree char *printed = g_variant_print(have, FALSE);
    g_error("%s is %s, expected %s", key, printed, text);
  }
}

static void
assert_items(GtkStringList *model, const Choice *choice, guint n, const char *extra)
{
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(model)), ==, n + (extra ? 1 : 0));
  for (guint i = 0; i < n; i++)
    g_assert_cmpstr(gtk_string_list_get_string(model, i), ==, choice->items[i]);
  if (extra)
    g_assert_cmpstr(gtk_string_list_get_string(model, n), ==, extra);
}

static void
test_choice_rows_bind_both_ways(Fixture *f, gconstpointer data)
{
  (void)data;
  for (guint c = 0; c < G_N_ELEMENTS(choices); c++) {
    const Choice *choice = &choices[c];
    GtkWidget *widget = gh_preferences_dialog_get_key_widget(f->dialog, choice->key);
    g_assert_true(ADW_IS_COMBO_ROW(widget));
    AdwComboRow *row = ADW_COMBO_ROW(widget);
    GtkStringList *model = GTK_STRING_LIST(adw_combo_row_get_model(row));
    guint n = 0;
    while (n < G_N_ELEMENTS(choice->items) && choice->items[n])
      n++;
    assert_items(model, choice, n, NULL);
    /* The default is the first item for every choice row. */
    g_assert_cmpuint(adw_combo_row_get_selected(row), ==, 0);
    assert_key(f->settings, choice->key, choice->values[0]);

    /* key -> row */
    for (guint i = n; i-- > 0;) {
      g_autoptr(GVariant) value = parse_value(f->settings, choice->key, choice->values[i]);
      g_assert_true(g_settings_set_value(f->settings, choice->key, value));
      drain_idle();
      g_assert_cmpuint(adw_combo_row_get_selected(row), ==, i);
    }
    /* row -> key */
    for (guint i = 0; i < n; i++) {
      adw_combo_row_set_selected(row, (i + 1) % n);
      drain_idle();
      assert_key(f->settings, choice->key, choice->values[(i + 1) % n]);
    }
    /* A value no item has is shown as it is and never rewritten... */
    g_autoptr(GVariant) custom = parse_value(f->settings, choice->key, choice->custom);
    g_assert_true(g_settings_set_value(f->settings, choice->key, custom));
    drain_idle();
    assert_items(model, choice, n, choice->custom_label);
    g_assert_cmpuint(adw_combo_row_get_selected(row), ==, n);
    assert_key(f->settings, choice->key, choice->custom);
    /* ...until the user picks an item, which removes it. */
    adw_combo_row_set_selected(row, n - 1);
    drain_idle();
    assert_key(f->settings, choice->key, choice->values[n - 1]);
    assert_items(model, choice, n, NULL);
    g_assert_cmpuint(adw_combo_row_get_selected(row), ==, n - 1);
    g_settings_reset(f->settings, choice->key);
    drain_idle();
    g_assert_cmpuint(adw_combo_row_get_selected(row), ==, 0);
  }
}

/* ---- older Marmot invitations (nostrc-lf62) ----------------------------------------- */

/* "Let people using older Marmot apps invite me" is a choice only when the
 * adopted producer is built: then it's shown and bound; without it the older
 * format is the only one and the switch isn't shown at all (review N4). */
static GtkWidget *
find_row_title(GtkWidget *widget, const char *title)
{
  if (ADW_IS_ACTION_ROW(widget) &&
      g_strcmp0(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(widget)), title) == 0)
    return widget;
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c;
       c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = find_row_title(c, title);
    if (found)
      return found;
  }
  return NULL;
}

static void
test_older_marmot_switch(Fixture *f, gconstpointer data)
{
  gboolean producer = (GPOINTER_TO_UINT(data) & GH_PREFERENCES_FEATURE_ADOPTED_KEY_PACKAGES) != 0;
  GtkWidget *row = gh_preferences_dialog_get_key_widget(f->dialog, "mls-legacy-key-packages");
  g_assert_true(row == child(f, "older_marmot_invites_row"));
  g_assert_cmpint(gtk_widget_get_visible(row), ==, producer);
  g_assert_cmpint(gh_preferences_dialog_get_key_available(f->dialog, "mls-legacy-key-packages"),
                  ==, producer);
  if (!producer) {
    g_assert_false(gtk_widget_get_sensitive(row));
    return;
  }
  g_assert_true(gtk_widget_get_sensitive(row));
  present(f, 800, 700);
  GtkWidget *compatibility = find_row_title(GTK_WIDGET(f->dialog), "Marmot compatibility");
  g_assert_nonnull(compatibility);
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(compatibility)),
                          "Some intermediate Marmot app versions cannot join either format."));
  g_assert_true(adw_switch_row_get_active(ADW_SWITCH_ROW(row))); /* on by default */
  g_settings_set_boolean(f->settings, "mls-legacy-key-packages", FALSE);
  drain_idle();
  g_assert_false(adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
}

/* ---- network mode and Tor -------------------------------------------------------- */

static void
test_tor_hidden_without_g09(Fixture *f, gconstpointer data)
{
  (void)data;
  GtkWidget *tor_row = gh_preferences_dialog_get_key_widget(f->dialog, "tor-socks-address");
  AdwComboRow *mode = ADW_COMBO_ROW(gh_preferences_dialog_get_key_widget(f->dialog,
                                                                          "network-mode"));
  GListModel *items = adw_combo_row_get_model(mode);
  g_assert_cmpuint(g_list_model_get_n_items(items), ==, 2);
  for (guint i = 0; i < 2; i++)
    g_assert_false(strstr(gtk_string_list_get_string(GTK_STRING_LIST(items), i), "Tor"));
  g_assert_false(gtk_widget_get_visible(tor_row));
  g_assert_true(gtk_widget_get_visible(child(f, "proxy_note")));
  g_assert_false(gtk_widget_get_visible(child(f, "tor_note")));

  g_assert_false(gtk_widget_get_visible(child(f, "tor_unavailable_note")));

  /* A stored "tor" (e.g. from dconf) shows the Tor row nowhere, and the
   * page says that nothing connects (nostrc-6v0i, gh-relay-guard.h). */
  g_settings_set_string(f->settings, "network-mode", "tor");
  drain_idle();
  g_assert_false(gtk_widget_get_visible(tor_row));
  g_assert_false(gtk_widget_get_visible(child(f, "tor_note")));
  g_assert_cmpuint(adw_combo_row_get_selected(mode), ==, 2);
  GtkWidget *unavailable = child(f, "tor_unavailable_note");
  g_assert_true(gtk_widget_get_visible(unavailable));
  g_assert_false(gtk_widget_get_visible(child(f, "proxy_note")));
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(unavailable)), ==,
                  "Tor isn't available in this build, so Groundhog won't connect until you "
                  "choose another network setting.");

  /* Choosing a mode this build has puts the direct-connection note back. */
  adw_combo_row_set_selected(mode, 1);
  drain_idle();
  assert_key(f->settings, "network-mode", "'none'");
  g_assert_false(gtk_widget_get_visible(unavailable));
  g_assert_true(gtk_widget_get_visible(child(f, "proxy_note")));
}

static void
test_tor_with_g09(Fixture *f, gconstpointer data)
{
  (void)data;
  GtkWidget *tor_widget = gh_preferences_dialog_get_key_widget(f->dialog, "tor-socks-address");
  AdwEntryRow *tor_row = ADW_ENTRY_ROW(tor_widget);
  AdwComboRow *mode = ADW_COMBO_ROW(gh_preferences_dialog_get_key_widget(f->dialog,
                                                                          "network-mode"));
  GtkStringList *items = GTK_STRING_LIST(adw_combo_row_get_model(mode));
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(items)), ==, 3);
  g_assert_cmpstr(gtk_string_list_get_string(items, 2), ==, "Tor");
  g_assert_false(gtk_widget_get_visible(tor_widget));
  /* Outside Tor mode the page says who sees the IP address, honestly for
   * the mode: relays are reached directly even with system proxy settings. */
  GtkLabel *proxy_note = GTK_LABEL(child(f, "proxy_note"));
  GtkWidget *tor_note = child(f, "tor_note");
  GtkWidget *status = child(f, "tor_status_row");
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(proxy_note)));
  g_assert_false(gtk_widget_get_visible(tor_note));
  g_assert_false(gtk_widget_get_visible(child(f, "tor_unavailable_note")));
  g_assert_nonnull(strstr(gtk_label_get_text(proxy_note), "relays directly"));
  g_assert_nonnull(strstr(gtk_label_get_text(proxy_note), "system's proxy settings"));
  adw_combo_row_set_selected(mode, 1);
  drain_idle();
  g_assert_null(strstr(gtk_label_get_text(proxy_note), "proxy settings"));
  g_assert_nonnull(strstr(gtk_label_get_text(proxy_note), "IP address"));
  g_assert_false(gtk_widget_get_visible(status));

  adw_combo_row_set_selected(mode, 2);
  drain_idle();
  assert_key(f->settings, "network-mode", "'tor'");
  g_assert_true(gtk_widget_get_visible(tor_widget));
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(tor_row)), ==, "127.0.0.1:9050");
  /* Tor mode: what Tor hides and what it doesn't (charter §2.2 surface 5). */
  g_assert_false(gtk_widget_get_visible(GTK_WIDGET(proxy_note)));
  g_assert_true(gtk_widget_get_visible(tor_note));
  const char *copy = gtk_label_get_text(GTK_LABEL(tor_note));
  g_assert_nonnull(strstr(copy, "hides your IP address"));
  /* Only from what Groundhog itself connects to (W16 review #4): links open
   * in the browser, outside Tor. */
  g_assert_null(strstr(copy, "websites"));
  g_assert_nonnull(strstr(copy, "browser, which doesn't use Groundhog's Tor connection"));
  g_assert_nonnull(strstr(copy, "doesn't hide your account"));
  g_assert_nonnull(strstr(copy, "sign in to your own inbox"));
  g_assert_nonnull(strstr(copy, "doesn't connect at all"));
  /* The status row appears once the app knows, and says it plainly. */
  g_assert_false(gtk_widget_get_visible(status));
  gh_preferences_dialog_set_tor_status(f->dialog, GH_PREFERENCES_TOR_STATUS_UNREACHABLE);
  g_assert_true(gtk_widget_get_visible(status));
  g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(status)), ==,
                  "Can't reach Tor at 127.0.0.1:9050 — Groundhog won't connect without it");
  gh_preferences_dialog_set_tor_status(f->dialog, GH_PREFERENCES_TOR_STATUS_REACHABLE);
  g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(status)), ==,
                  "Tor is reachable at 127.0.0.1:9050");

  /* key -> row */
  g_settings_set_string(f->settings, "tor-socks-address", "127.0.0.1:9150");
  drain_idle();
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(tor_row)), ==, "127.0.0.1:9150");
  /* row -> key, only on apply and only when valid */
  GtkWidget *error = child(f, "tor_address_error");
  static const char *const bad[] = { "socks5://127.0.0.1:9050", "127.0.0.1", "", "host:0" };
  for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
    gtk_editable_set_text(GTK_EDITABLE(tor_row), bad[i]);
    g_signal_emit_by_name(tor_row, "apply");
    drain_idle();
    g_assert_cmpstr(g_settings_get_string(f->settings, "tor-socks-address"), ==,
                    "127.0.0.1:9150");
    g_assert_true(gtk_widget_get_visible(error));
    g_assert_true(gtk_widget_has_css_class(tor_widget, "error"));
  }
  gtk_editable_set_text(GTK_EDITABLE(tor_row), " [::1]:9050 ");
  g_assert_false(gtk_widget_get_visible(error)); /* editing clears the error */
  g_signal_emit_by_name(tor_row, "apply");
  drain_idle();
  g_autofree char *stored = g_settings_get_string(f->settings, "tor-socks-address");
  g_assert_cmpstr(stored, ==, "[::1]:9050");
  g_assert_false(gtk_widget_has_css_class(tor_widget, "error"));

  g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(status)), ==,
                  "Tor is reachable at [::1]:9050");

  adw_combo_row_set_selected(mode, 0);
  drain_idle();
  g_assert_false(gtk_widget_get_visible(tor_widget));
  g_assert_false(gtk_widget_get_visible(status));
  g_assert_false(gtk_widget_get_visible(tor_note));
}

/* ---- URL lists ------------------------------------------------------------------- */

static guint
n_rows(GtkListBox *list)
{
  guint n = 0;
  while (gtk_list_box_get_row_at_index(list, (int)n))
    n++;
  return n;
}

static const char *
row_title(GtkListBox *list, guint index)
{
  GtkListBoxRow *row = gtk_list_box_get_row_at_index(list, (int)index);
  g_assert_true(ADW_IS_ACTION_ROW(row));
  return adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row));
}

/* The first button under widget with this label (any label if NULL); with
 * mapped_only, only one on screen: libadwaita 1.5's AdwAlertDialog keeps a
 * second, hidden set of response buttons for its other layout. */
static GtkWidget *
find_button_full(GtkWidget *widget, const char *label, gboolean mapped_only)
{
  if (GTK_IS_BUTTON(widget) && (!mapped_only || gtk_widget_get_mapped(widget)) &&
      (!label || g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), label) == 0))
    return widget;
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = find_button_full(c, label, mapped_only);
    if (found)
      return found;
  }
  return NULL;
}

static GtkWidget *
find_button(GtkWidget *widget, const char *label)
{
  return find_button_full(widget, label, FALSE);
}

/* The first button under widget with this tooltip (G22: an attachment
 * server's row also has Move Up and Move Down before Remove). */
static GtkWidget *
find_tooltip_button(GtkWidget *widget, const char *tooltip)
{
  if (GTK_IS_BUTTON(widget) && g_strcmp0(gtk_widget_get_tooltip_text(widget), tooltip) == 0)
    return widget;
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = find_tooltip_button(c, tooltip);
    if (found)
      return found;
  }
  return NULL;
}

static void
assert_strv(GSettings *settings, const char *key, const char *const *want)
{
  g_auto(GStrv) have = g_settings_get_strv(settings, key);
  if (!g_strv_equal((const char *const *)have, want)) {
    g_autofree char *joined = g_strjoinv(" ", have);
    g_error("%s is [%s]", key, joined);
  }
}

typedef struct {
  const char *key;
  const char *entry;
  const char *error;
  const char *list;
  const char *good_in;     /* accepted as typed... */
  const char *good_out;    /* ...and stored like this */
  const char *second;      /* another accepted address */
  const char *bad[12];
} ListCase;

static const ListCase list_cases[] = {
  { "discovery-relays", "discovery_entry", "discovery_error", "discovery_list",
    " WSS://Relay.Example.COM/ ", "wss://relay.example.com", "ws://127.0.0.1:7777",
    { "https://relay.example.com", "wss://alice:secret@relay.example.com",
      "wss://alice@relay.example.com", "ws://relay.example.com", "relay.example.com", "wss://",
      "wss://relay.example.com/?token=1", "wss://relay.example.com/#x", "",
      "wss://relay.example.com", /* a duplicate of good_out */
      NULL } },
  { "blossom-servers", "blossom_entry", "blossom_error", "blossom_list",
    "https://Blossom.Example.com/", "https://blossom.example.com",
    "https://files.example.org:8443/media",
    { "wss://blossom.example.com", "http://blossom.example.com", "http://127.0.0.1:3000",
      "https://user:pw@blossom.example.com", "blossom.example.com", "https://",
      "https://blossom.example.com?x=1", "", "https://BLOSSOM.example.com/", NULL } },
};

static void
test_url_lists_bind_both_ways(Fixture *f, gconstpointer data)
{
  (void)data;
  for (guint c = 0; c < G_N_ELEMENTS(list_cases); c++) {
    const ListCase *lc = &list_cases[c];
    GtkListBox *list = GTK_LIST_BOX(gh_preferences_dialog_get_key_widget(f->dialog, lc->key));
    g_assert_true(list == child(f, lc->list));
    AdwEntryRow *entry = child(f, lc->entry);
    GtkWidget *error = child(f, lc->error);
    /* Empty by default (PD-13, D6): nothing to contact. */
    g_assert_cmpuint(n_rows(list), ==, 0);
    g_assert_false(gtk_widget_get_visible(error));

    /* row -> key: a valid entry is normalized and appended, then cleared. */
    gtk_editable_set_text(GTK_EDITABLE(entry), lc->good_in);
    g_signal_emit_by_name(entry, "apply");
    drain_idle();
    assert_strv(f->settings, lc->key, (const char *const[]){ lc->good_out, NULL });
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(entry)), ==, "");
    g_assert_false(gtk_widget_get_visible(error));
    g_assert_cmpuint(n_rows(list), ==, 1);
    g_assert_cmpstr(row_title(list, 0), ==, lc->good_out);

    /* Invalid entries change nothing and say why, in place. */
    for (guint i = 0; lc->bad[i]; i++) {
      gtk_editable_set_text(GTK_EDITABLE(entry), lc->bad[i]);
      g_signal_emit_by_name(entry, "apply");
      drain_idle();
      assert_strv(f->settings, lc->key, (const char *const[]){ lc->good_out, NULL });
      if (!gtk_widget_get_visible(error))
        g_error("%s accepted \"%s\"", lc->key, lc->bad[i]);
      g_assert_nonnull(gtk_label_get_text(GTK_LABEL(error)));
      g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(error)), !=, "");
      g_assert_true(gtk_widget_has_css_class(GTK_WIDGET(entry), "error"));
      g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(entry)), ==, lc->bad[i]);
    }
    gtk_editable_set_text(GTK_EDITABLE(entry), lc->second);
    g_assert_false(gtk_widget_get_visible(error));
    g_signal_emit_by_name(entry, "apply");
    drain_idle();
    assert_strv(f->settings, lc->key, (const char *const[]){ lc->good_out, lc->second, NULL });
    g_assert_cmpuint(n_rows(list), ==, 2);

    /* key -> rows */
    const char *const replaced[] = { lc->second, lc->good_out, NULL };
    g_settings_set_strv(f->settings, lc->key, replaced);
    drain_idle();
    g_assert_cmpuint(n_rows(list), ==, 2);
    g_assert_cmpstr(row_title(list, 0), ==, lc->second);
    g_assert_cmpstr(row_title(list, 1), ==, lc->good_out);

    /* Each row removes its own address; its button says which. */
    GtkWidget *remove = find_tooltip_button(GTK_WIDGET(gtk_list_box_get_row_at_index(list, 0)),
                                            "Remove");
    g_assert_nonnull(remove);
    g_autofree char *label = g_strdup_printf("Remove %s", lc->second);
    gtk_test_accessible_assert_property(GTK_ACCESSIBLE(remove), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                        label);
    g_signal_emit_by_name(remove, "clicked");
    drain_idle();
    assert_strv(f->settings, lc->key, (const char *const[]){ lc->good_out, NULL });
    g_assert_cmpuint(n_rows(list), ==, 1);

    /* At most 16. */
    g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
    for (guint i = 0; i < 16; i++) {
      g_autofree char *url = g_strdup_printf("%s/%u", lc->good_out, i);
      g_strv_builder_add(builder, url);
    }
    g_auto(GStrv) full = g_strv_builder_end(builder);
    g_settings_set_strv(f->settings, lc->key, (const char *const *)full);
    drain_idle();
    g_assert_cmpuint(n_rows(list), ==, 16);
    gtk_editable_set_text(GTK_EDITABLE(entry), lc->second);
    g_signal_emit_by_name(entry, "apply");
    drain_idle();
    g_assert_true(gtk_widget_get_visible(error));
    g_auto(GStrv) after = g_settings_get_strv(f->settings, lc->key);
    g_assert_cmpuint(g_strv_length(after), ==, 16);
    g_settings_reset(f->settings, lc->key);
    drain_idle();
    g_assert_cmpuint(n_rows(list), ==, 0);
  }
}

static void
test_url_rules(void)
{
  static const struct {
    gboolean relay;
    const char *in;
    gboolean onion;
    const char *out; /* NULL: rejected */
  } cases[] = {
    { TRUE, "wss://relay.example.com", FALSE, "wss://relay.example.com" },
    { TRUE, "wss://relay.example.com:4443/nostr/", FALSE, "wss://relay.example.com:4443/nostr" },
    { TRUE, "ws://localhost:7777", FALSE, "ws://localhost:7777" },
    { TRUE, "ws://127.0.0.2", FALSE, "ws://127.0.0.2" },
    { TRUE, "ws://[::1]:7777", FALSE, "ws://[::1]:7777" },
    { TRUE, "ws://relay.example.com", FALSE, NULL },
    { TRUE, "ws://abcdef.onion", FALSE, NULL },
    { TRUE, "ws://abcdef.onion", TRUE, "ws://abcdef.onion" },
    { TRUE, "http://relay.example.com", FALSE, NULL },
    { TRUE, "https://relay.example.com", FALSE, NULL },
    { TRUE, "wss://:pw@relay.example.com", FALSE, NULL },
    { TRUE, "wss://user@relay.example.com", FALSE, NULL },
    { TRUE, "wss:relay.example.com", FALSE, NULL },
    { TRUE, "javascript:alert(1)", FALSE, NULL },
    { TRUE, NULL, FALSE, NULL },
    { FALSE, "https://cdn.example.com/", FALSE, "https://cdn.example.com" },
    { FALSE, "http://abcdef.onion", TRUE, "http://abcdef.onion" },
    { FALSE, "http://abcdef.onion", FALSE, NULL },
    { FALSE, "http://localhost:3000", FALSE, NULL },
    { FALSE, "wss://cdn.example.com", FALSE, NULL },
    { FALSE, "https://u:p@cdn.example.com", FALSE, NULL },
    { FALSE, "file:///etc/passwd", FALSE, NULL },
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_autoptr(GError) error = NULL;
    g_autofree char *out =
      cases[i].relay ? gh_preferences_normalize_relay_url(cases[i].in, cases[i].onion, &error)
                     : gh_preferences_normalize_server_url(cases[i].in, cases[i].onion, &error);
    if (g_strcmp0(out, cases[i].out) != 0)
      g_error("\"%s\" (onion %d) gave \"%s\", expected \"%s\"",
              cases[i].in ? cases[i].in : "(null)", cases[i].onion, out ? out : "(rejected)",
              cases[i].out ? cases[i].out : "(rejected)");
    if (!cases[i].out)
      g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    else
      g_assert_no_error(error);
  }

  static const struct {
    const char *in;
    gboolean ok;
  } socks[] = {
    { "127.0.0.1:9050", TRUE }, { "localhost:9150", TRUE }, { "[::1]:9050", TRUE },
    { "tor-gateway.lan:9050", TRUE }, { "127.0.0.1", FALSE }, { "socks5://127.0.0.1:9050", FALSE },
    { "127.0.0.1:0", FALSE }, { "127.0.0.1:70000", FALSE }, { "", FALSE }, { NULL, FALSE },
  };
  for (guint i = 0; i < G_N_ELEMENTS(socks); i++) {
    g_autoptr(GError) error = NULL;
    if (gh_preferences_validate_socks_address(socks[i].in, &error) != socks[i].ok)
      g_error("SOCKS address \"%s\" was %s", socks[i].in ? socks[i].in : "(null)",
              socks[i].ok ? "rejected" : "accepted");
    if (!socks[i].ok)
      g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  }
}

/* ---- account and Delete All Messages ------------------------------------------------ */

static struct {
  guint calls;
  char *npub;
  GObject *target;
  GTask *pending;
} fake;

static void
fake_forget_async(GObject *target, const char *npub, GCancellable *cancellable,
                  GAsyncReadyCallback callback, gpointer user_data)
{
  g_assert_true(target == fake.target);
  g_assert_null(fake.pending);
  fake.calls++;
  g_free(fake.npub);
  fake.npub = g_strdup(npub);
  fake.pending = g_task_new(target, cancellable, callback, user_data);
}

static GhPreferencesForgetResult
fake_forget_finish(GObject *target, GAsyncResult *result, GError **error)
{
  g_assert_true(g_task_is_valid(result, target));
  gssize outcome = g_task_propagate_int(G_TASK(result), error);
  return outcome < 0 ? GH_PREFERENCES_FORGET_FAILED : (GhPreferencesForgetResult)outcome;
}

static void
fake_reset(void)
{
  g_clear_object(&fake.pending);
  g_clear_pointer(&fake.npub, g_free);
  g_clear_object(&fake.target);
  fake.calls = 0;
}

static void
fake_complete(GhPreferencesForgetResult outcome)
{
  GTask *task = g_steal_pointer(&fake.pending);
  g_assert_nonnull(task);
  if (outcome == GH_PREFERENCES_FORGET_FAILED)
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "The disk is read-only");
  else
    g_task_return_int(task, outcome);
  g_object_unref(task);
}

static void
test_account_rows(Fixture *f, gconstpointer data)
{
  (void)data;
  GtkWidget *none = child(f, "no_account_row");
  AdwActionRow *account = child(f, "account_row");
  GtkWidget *group = child(f, "delete_group");
  g_assert_true(gtk_widget_get_visible(none));
  g_assert_false(gtk_widget_get_visible(GTK_WIDGET(account)));
  g_assert_false(gtk_widget_get_visible(group));

  gh_preferences_dialog_set_account(f->dialog, NPUB, "Alice <b>");
  g_assert_false(gtk_widget_get_visible(none));
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(account)));
  /* Names are shown as text, never as markup. */
  g_assert_false(adw_preferences_row_get_use_markup(ADW_PREFERENCES_ROW(account)));
  g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(account)), ==, "Alice <b>");
  g_assert_cmpstr(adw_action_row_get_subtitle(account), ==, NPUB);
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->dialog), "prefs.copy-npub", NULL));
  GdkContentProvider *content = gdk_clipboard_get_content(
    gtk_widget_get_clipboard(GTK_WIDGET(f->dialog)));
  g_assert_nonnull(content);
  GValue copied = G_VALUE_INIT;
  g_value_init(&copied, G_TYPE_STRING);
  g_autoptr(GError) copy_error = NULL;
  g_assert_true(gdk_content_provider_get_value(content, &copied, &copy_error));
  g_assert_cmpstr(g_value_get_string(&copied), ==, NPUB);
  g_value_unset(&copied);
  AdwAvatar *avatar = child(f, "account_avatar");
  const guint8 pixel[] = { 0, 0, 0 };
  g_autoptr(GBytes) bytes = g_bytes_new_static(pixel, sizeof pixel);
  g_autoptr(GdkTexture) picture = gdk_memory_texture_new(1, 1, GDK_MEMORY_R8G8B8,
                                                         bytes, 3);
  gh_preferences_dialog_set_account_picture(f->dialog, GDK_PAINTABLE(picture));
  g_assert_true(adw_avatar_get_custom_image(avatar) == GDK_PAINTABLE(picture));
  /* No forget function (a build without the encrypted store): nothing offered. */
  g_assert_false(gtk_widget_get_visible(group));

  gh_preferences_dialog_set_account(f->dialog, NPUB, NULL);
  g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(account)), ==,
                  "npub10xlxv…pkge6d");

  fake.target = g_object_new(G_TYPE_OBJECT, NULL);
  gh_preferences_dialog_set_forget_func(f->dialog, fake_forget_async, fake_forget_finish,
                                        fake.target);
  g_assert_true(gtk_widget_get_visible(group));
  gh_preferences_dialog_set_account(f->dialog, NULL, NULL);
  g_assert_null(adw_avatar_get_custom_image(avatar));
  g_assert_false(gtk_widget_get_visible(group));
  g_assert_true(gtk_widget_get_visible(none));
  /* Nothing to delete: the action does nothing. */
  gtk_widget_activate_action(GTK_WIDGET(f->dialog), "prefs.delete-all", NULL);
  drain_idle();
  g_assert_null(gtk_widget_get_root(GTK_WIDGET(child(f, "delete_all_dialog"))));
  g_assert_cmpuint(fake.calls, ==, 0);
  fake_reset();
}

typedef struct {
  GtkWidget *widget;
  GtkWindow *window;
} RootWait;

static gboolean
alert_shown(gpointer data)
{
  RootWait *wait = data;
  return gtk_widget_get_root(wait->widget) == GTK_ROOT(wait->window) &&
         gtk_widget_get_mapped(wait->widget);
}

static gboolean
alert_gone(gpointer data)
{
  return gtk_widget_get_root(((RootWait *)data)->widget) == NULL;
}

typedef struct {
  GtkWidget *alert;
  const char *label;
  GtkWidget *button;
} ButtonWait;

static gboolean
response_button_shown(gpointer data)
{
  ButtonWait *wait = data;
  wait->button = find_button_full(wait->alert, wait->label, TRUE);
  return wait->button != NULL;
}

static gboolean
dialog_can_close(gpointer data)
{
  return adw_dialog_get_can_close(ADW_DIALOG(data));
}

static void
confirm(Fixture *f, const char *response_label)
{
  RootWait wait = { child(f, "delete_all_dialog"), f->window };
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->dialog), "prefs.delete-all", NULL));
  spin_until(alert_shown, &wait);
  /* Its response buttons are laid out (and mapped) after it is. */
  ButtonWait shown = { wait.widget, response_label, NULL };
  spin_until(response_button_shown, &shown);
  g_test_message("responding %s", response_label);
  g_signal_emit_by_name(shown.button, "clicked");
  spin_until(alert_gone, &wait);
  drain_idle();
}

static void
setup_inbox_activated(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action;
  (void)parameter;
  (*(guint *)data)++;
}

/* Network › Encrypted Groups (nostrc-f8a5, nostrc-0bdg review H2): honest
 * about whether people can invite the account; [Set Up] when it has no relay
 * list, which opens the relay step (win.setup-inbox) and closes the dialog. */
static void
test_key_package_row(Fixture *f, gconstpointer data)
{
  gboolean groups = (GPOINTER_TO_UINT(data) & GH_PREFERENCES_FEATURE_ENCRYPTED_GROUPS) != 0;
  GtkWidget *group = child(f, "key_package_group");
  GtkWidget *setup = child(f, "key_package_setup");
  AdwActionRow *row = child(f, "key_package_row");
  g_assert_false(gtk_widget_get_visible(group));   /* not known yet: no row */
  gh_preferences_dialog_set_key_package_state(f->dialog, GH_PREFERENCES_KEY_PACKAGE_NO_RELAYS);
  g_assert_cmpint(gh_preferences_dialog_get_key_package_state(f->dialog), ==,
                  GH_PREFERENCES_KEY_PACKAGE_NO_RELAYS);
  g_assert_cmpint(gtk_widget_get_visible(group), ==, groups);
  if (!groups) {
    g_assert_false(gtk_widget_get_visible(setup));
    return;
  }
  g_assert_true(gtk_widget_get_visible(setup));
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(row),
                          "People can't invite you to encrypted groups yet"));
  gh_preferences_dialog_set_key_package_state(f->dialog, GH_PREFERENCES_KEY_PACKAGE_PUBLISHED);
  g_assert_false(gtk_widget_get_visible(setup));
  g_assert_cmpstr(adw_action_row_get_subtitle(row), ==,
                  "People can invite you to encrypted groups.");
  /* Held for pending invitations (re-review A5): said, nothing to do. */
  gh_preferences_dialog_set_key_package_state(f->dialog, GH_PREFERENCES_KEY_PACKAGE_HELD);
  g_assert_false(gtk_widget_get_visible(setup));
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(row), "waits until you accept or decline"));
  /* A relay list without a relay the account publishes to (R3): honest, and
   * [Add Relays] (the relay step adds them to that list). */
  gh_preferences_dialog_set_key_package_state(f->dialog,
                                              GH_PREFERENCES_KEY_PACKAGE_NO_WRITE_RELAYS);
  g_assert_true(gtk_widget_get_visible(setup));
  g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(setup)), ==, "_Add Relays");
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(row),
                          "your relay list names no relay you publish to"));
  gh_preferences_dialog_set_key_package_state(f->dialog, GH_PREFERENCES_KEY_PACKAGE_NO_RELAYS);
  g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(setup)), ==, "_Set Up");
  gh_preferences_dialog_set_key_package_state(f->dialog, GH_PREFERENCES_KEY_PACKAGE_UNKNOWN);
  g_assert_false(gtk_widget_get_visible(group));

  /* Identity-proof states (nostrc-q74l): the section shows with the right
   * subtitle and [Try Again] for declined and failed. */
  gh_preferences_dialog_set_key_package_state(f->dialog,
                                              GH_PREFERENCES_KEY_PACKAGE_IDENTITY_WAITING);
  g_assert_true(gtk_widget_get_visible(group));
  g_assert_false(gtk_widget_get_visible(setup));  /* waiting: nothing to press */
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(row),
                          "Waiting for Grotto to prove your account"));

  gh_preferences_dialog_set_key_package_state(f->dialog,
                                              GH_PREFERENCES_KEY_PACKAGE_IDENTITY_DECLINED);
  g_assert_true(gtk_widget_get_visible(group));
  g_assert_true(gtk_widget_get_visible(setup));
  g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(setup)), ==, "_Try Again");
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(row),
                          "You declined the account proof"));

  gh_preferences_dialog_set_key_package_state(f->dialog,
                                              GH_PREFERENCES_KEY_PACKAGE_IDENTITY_FAILED);
  g_assert_true(gtk_widget_get_visible(group));
  g_assert_true(gtk_widget_get_visible(setup));
  g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(setup)), ==, "_Try Again");
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(row),
                          "account proof couldn't be completed"));

  /* [Set Up] runs the window's relay step and closes the dialog. */
  gh_preferences_dialog_set_key_package_state(f->dialog, GH_PREFERENCES_KEY_PACKAGE_NO_RELAYS);
  present(f, 800, 700);
  guint opened = 0;
  g_autoptr(GSimpleActionGroup) win = g_simple_action_group_new();
  g_autoptr(GSimpleAction) action = g_simple_action_new("setup-inbox", NULL);
  g_signal_connect(action, "activate", G_CALLBACK(setup_inbox_activated), &opened);
  g_action_map_add_action(G_ACTION_MAP(win), G_ACTION(action));
  gtk_widget_insert_action_group(GTK_WIDGET(f->window), "win", G_ACTION_GROUP(win));
  g_signal_emit_by_name(setup, "clicked");
  drain_idle();
  g_assert_cmpuint(opened, ==, 1);
  g_assert_false(gtk_widget_get_mapped(GTK_WIDGET(f->dialog)));
}

/* Network › Where People Reach You (nostrc-mi1z): the published relay lists
 * and the "Change Relays…" button. */
static void
change_relays_handler(GhPreferencesDialog *dialog, gpointer data)
{
  (void)dialog;
  (*(guint *)data)++;
}

static void
test_published_relays(Fixture *f, gconstpointer data)
{
  (void)data;
  GtkWidget *group = child(f, "published_relays_group");
  GtkListBox *inbox = child(f, "inbox_relays_list");
  GtkListBox *write = child(f, "write_relays_list");
  GtkWidget *change = child(f, "published_relays_change");

  /* Before setting relays: the section is hidden. */
  g_assert_false(gtk_widget_get_visible(group));

  /* Setting both relay lists shows the section and populates each list. */
  const gchar *const inbox_urls[] = { "wss://inbox.example.com", "wss://inbox2.example.com", NULL };
  const gchar *const write_urls[] = { "wss://relay.example.com", NULL };
  gh_preferences_dialog_set_published_relays(f->dialog, inbox_urls, write_urls);
  g_assert_true(gtk_widget_get_visible(group));
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(inbox)));
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(write)));
  /* Group headings sit outside the boxed lists; only real URLs and an
   * add-entry row are inside them. */
  AdwPreferencesGroup *inbox_group = child(f, "inbox_relays_group");
  AdwPreferencesGroup *write_group = child(f, "write_relays_group");
  g_assert_cmpstr(adw_preferences_group_get_title(inbox_group), ==, "Message Relays (Inbox)");
  g_assert_cmpstr(adw_preferences_group_get_title(write_group), ==, "Publish Relays");
  g_assert_cmpuint(n_rows(inbox), ==, 3);
  g_assert_cmpuint(n_rows(write), ==, 2);
  g_assert_cmpstr(row_title(inbox, 0), ==, "wss://inbox.example.com");
  g_assert_cmpstr(row_title(inbox, 1), ==, "wss://inbox2.example.com");
  g_assert_cmpstr(row_title(write, 0), ==, "wss://relay.example.com");

  /* Only inbox relays, no write relays: section shown, write list hidden. */
  gh_preferences_dialog_set_published_relays(f->dialog, inbox_urls, NULL);
  g_assert_true(gtk_widget_get_visible(group));
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(inbox)));
  g_assert_false(gtk_widget_get_visible(GTK_WIDGET(write_group)));

  /* Clearing both hides the section. */
  gh_preferences_dialog_set_published_relays(f->dialog, NULL, NULL);
  g_assert_false(gtk_widget_get_visible(group));

  /* "Change Relays…" emits the change-relays signal. */
  guint changed = 0;
  g_signal_connect(f->dialog, "change-relays", G_CALLBACK(change_relays_handler), &changed);
  gh_preferences_dialog_set_published_relays(f->dialog, inbox_urls, write_urls);
  present(f, 800, 700);
  g_signal_emit_by_name(change, "clicked");
  drain_idle();
  g_assert_cmpuint(changed, ==, 1);
}

static GtkWidget *icon_button(GtkWidget *widget, const gchar *icon);

static void
test_relay_layout(Fixture *f, gconstpointer data)
{
  (void)data;
  const gchar *urls[] = { "wss://relay.example.com", NULL };
  gh_preferences_dialog_set_published_relays(f->dialog, urls, urls);
  present(f, 800, 700);
  adw_preferences_dialog_set_visible_page_name(ADW_PREFERENCES_DIALOG(f->dialog), "network");
  drain_idle();
  gtk_test_widget_wait_for_draw(GTK_WIDGET(f->window));
  const gchar *lists[] = { "inbox_relays_list", "write_relays_list" };
  const gchar *groups[] = { "inbox_relays_group", "write_relays_group" };
  for (guint i = 0; i < G_N_ELEMENTS(lists); i++) {
    GtkWidget *group = child(f, groups[i]);
    GtkListBox *list = child(f, lists[i]);
    GtkWidget *url = GTK_WIDGET(gtk_list_box_get_row_at_index(list, 0));
    GtkWidget *entry = GTK_WIDGET(gtk_list_box_get_row_at_index(list, 1));
    g_assert_true(ADW_IS_PREFERENCES_GROUP(group));
    g_assert_true(ADW_IS_ACTION_ROW(url));
    g_assert_true(ADW_IS_ENTRY_ROW(entry));
    g_assert_cmpuint(n_rows(list), ==, 2); /* no disabled pseudo-heading */
    g_assert_true(gtk_widget_get_sensitive(url));
    /* A fifth page can leave the switched-to Network page awaiting a frame;
     * measure the row after its actual allocation, not the first dialog draw. */
    for (guint frame = 0; frame < 12 && gtk_widget_get_width(url) <= 200; frame++)
      gtk_test_widget_wait_for_draw(GTK_WIDGET(f->window));
    g_assert_cmpint(gtk_widget_get_width(url), >, 200);
    g_assert_cmpint(gtk_widget_get_width(url), ==, gtk_widget_get_width(entry));
    GtkWidget *remove = icon_button(url, "edit-delete-symbolic");
    GtkWidget *add = icon_button(entry, "list-add-symbolic");
    g_assert_nonnull(remove);
    g_assert_nonnull(add);
    g_assert_cmpint(gtk_widget_get_valign(remove), ==, GTK_ALIGN_CENTER);
    g_assert_cmpint(gtk_widget_get_valign(add), ==, GTK_ALIGN_CENTER);
  }
}

typedef struct { guint adds, removes; gint kind; gchar *url; } RelayEdits;
static void relay_added(GhPreferencesDialog *dialog, gint kind, const gchar *url, RelayEdits *edits)
{
  (void)dialog;
  edits->adds++; edits->kind = kind;
  g_free(edits->url); edits->url = g_strdup(url);
}
static void relay_removed(GhPreferencesDialog *dialog, gint kind, const gchar *url, RelayEdits *edits)
{
  (void)dialog;
  edits->removes++; edits->kind = kind;
  g_free(edits->url); edits->url = g_strdup(url);
}
static GtkWidget *icon_button(GtkWidget *widget, const gchar *icon)
{
  if (GTK_IS_BUTTON(widget) && g_strcmp0(gtk_button_get_icon_name(GTK_BUTTON(widget)), icon) == 0) return widget;
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = icon_button(c, icon);
    if (found) return found;
  }
  return NULL;
}
static void test_relay_controls(Fixture *f, gconstpointer data)
{
  (void)data;
  const gchar *urls[] = { "wss://relay.example.com", NULL };
  gh_preferences_dialog_set_published_relays(f->dialog, urls, urls);
  present(f, 800, 700);
  adw_preferences_dialog_set_visible_page_name(ADW_PREFERENCES_DIALOG(f->dialog), "network");
  RelayEdits edits = { 0 };
  g_signal_connect(f->dialog, "add-relay", G_CALLBACK(relay_added), &edits);
  g_signal_connect(f->dialog, "remove-relay", G_CALLBACK(relay_removed), &edits);
  const gchar *lists[] = { "inbox_relays_list", "write_relays_list" };
  for (guint i = 0; i < 2; i++) {
    GtkListBox *list = child(f, lists[i]);
    GtkWidget *entry = GTK_WIDGET(gtk_list_box_get_row_at_index(list, 1));
    g_assert_true(ADW_IS_ENTRY_ROW(entry));
    GtkWidget *add = icon_button(entry, "list-add-symbolic");
    g_assert_nonnull(add);
    g_assert_true(gtk_widget_get_focusable(add));
    const gchar *label = i == 0 ? "Add Message Relay" : "Add Publish Relay";
    gtk_test_accessible_assert_property(add, GTK_ACCESSIBLE_PROPERTY_LABEL, label);
    gtk_widget_grab_focus(entry);
    gtk_widget_child_focus(entry, GTK_DIR_TAB_FORWARD);
    gtk_editable_set_text(GTK_EDITABLE(entry), "http://127.0.0.1/not-a-relay");
    g_signal_emit_by_name(add, "clicked");
    g_assert_cmpuint(edits.adds, ==, i);
    g_assert_true(gtk_widget_has_css_class(entry, "error"));
    g_assert_nonnull(gtk_widget_get_tooltip_text(entry));
    gtk_editable_set_text(GTK_EDITABLE(entry), urls[0]);
    g_signal_emit_by_name(entry, "entry-activated");
    g_assert_cmpuint(edits.adds, ==, i);
    g_assert_true(gtk_widget_has_css_class(entry, "error"));
    gtk_editable_set_text(GTK_EDITABLE(entry), "  wss://new.example.com/  ");
    g_signal_emit_by_name(add, "clicked");
    g_assert_cmpuint(edits.adds, ==, i + 1);
    g_assert_cmpint(edits.kind, ==, i == 0 ? 10050 : 10002);
    g_assert_cmpstr(edits.url, ==, "wss://new.example.com");
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(entry)), ==, "");
    /* No optimistic replacement: the service owns the signed full-list update. */
    g_assert_cmpuint(n_rows(list), ==, 2);
    GtkWidget *remove = icon_button(GTK_WIDGET(gtk_list_box_get_row_at_index(list, 0)), "edit-delete-symbolic");
    gtk_test_accessible_assert_property(remove, GTK_ACCESSIBLE_PROPERTY_LABEL, "Remove wss://relay.example.com");
    g_signal_emit_by_name(remove, "clicked");
    g_assert_cmpuint(edits.removes, ==, i + 1);
    g_assert_cmpint(edits.kind, ==, i == 0 ? 10050 : 10002);
    g_assert_cmpstr(edits.url, ==, urls[0]);
    g_assert_cmpuint(n_rows(list), ==, 2);
  }
  const gchar *empty[] = { NULL };
  gh_preferences_dialog_set_published_relays(f->dialog, empty, empty);
  g_assert_true(gtk_widget_get_visible(child(f, "published_relays_group")));
  g_assert_cmpuint(n_rows(child(f, "inbox_relays_list")), ==, 1);
  g_signal_handlers_disconnect_by_data(f->dialog, &edits);
  g_free(edits.url);
}

static void
test_delete_all_runs_forget(Fixture *f, gconstpointer data)
{
  (void)data;
  AdwAlertDialog *alert = child(f, "delete_all_dialog");
  GtkWidget *button = find_button(child(f, "delete_group"), "Delete…");
  g_assert_nonnull(button);
  present(f, 800, 700);
  fake.target = g_object_new(G_TYPE_OBJECT, NULL);
  gh_preferences_dialog_set_account(f->dialog, NPUB, "Alice");
  gh_preferences_dialog_set_forget_func(f->dialog, fake_forget_async, fake_forget_finish,
                                        fake.target);
  g_assert_true(gtk_widget_get_sensitive(button));

  /* The confirmation is destructive, defaults to Cancel and closes as Cancel. */
  g_assert_cmpstr(adw_alert_dialog_get_heading(alert), ==,
                  "Delete All Messages on This Device?");
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(alert), "Copies stay on relays"));
  g_assert_cmpint(adw_alert_dialog_get_response_appearance(alert, "delete"), ==,
                  ADW_RESPONSE_DESTRUCTIVE);
  g_assert_cmpstr(adw_alert_dialog_get_default_response(alert), ==, "cancel");
  g_assert_cmpstr(adw_alert_dialog_get_close_response(alert), ==, "cancel");

  /* Cancel deletes nothing. */
  confirm(f, "_Cancel");
  g_assert_cmpuint(fake.calls, ==, 0);

  /* Confirming runs the account store path for this account, once. */
  confirm(f, "_Delete and Sign Out");
  g_assert_cmpuint(fake.calls, ==, 1);
  g_assert_cmpstr(fake.npub, ==, NPUB);
  /* It runs to its end: the dialog stays open and offers nothing more. */
  g_assert_false(adw_dialog_get_can_close(ADW_DIALOG(f->dialog)));
  g_assert_false(gtk_widget_get_sensitive(button));
  gtk_widget_activate_action(GTK_WIDGET(f->dialog), "prefs.delete-all", NULL);
  drain_idle();
  g_assert_null(gtk_widget_get_root(GTK_WIDGET(alert)));
  adw_dialog_close(ADW_DIALOG(f->dialog));
  drain_idle();
  g_assert_true(gtk_widget_get_mapped(GTK_WIDGET(f->dialog)));
  g_assert_cmpuint(fake.calls, ==, 1);

  /* The account store signs the account out; the application then clears it. */
  gh_preferences_dialog_set_account(f->dialog, NULL, NULL);
  fake_complete(GH_PREFERENCES_FORGET_DELETED);
  spin_until(dialog_can_close, f->dialog);
  g_assert_false(gtk_widget_get_visible(child(f, "delete_group")));
  g_assert_cmpstr(gh_preferences_dialog_get_last_toast(f->dialog), ==,
                  "All messages of the account were deleted from this device");

  /* The files are gone but the key item stayed: said as it is (W13b review
   * 4a), not as a failure to delete the messages. */
  gh_preferences_dialog_set_account(f->dialog, NPUB, "Alice");
  confirm(f, "_Delete and Sign Out");
  g_assert_cmpuint(fake.calls, ==, 2);
  gh_preferences_dialog_set_account(f->dialog, NULL, NULL);
  fake_complete(GH_PREFERENCES_FORGET_KEY_KEPT);
  spin_until(dialog_can_close, f->dialog);
  g_assert_cmpstr(gh_preferences_dialog_get_last_toast(f->dialog), ==,
                  "Messages deleted; the storage key couldn't be removed from the keyring");

  /* A failure is reported and the action is offered again. */
  gh_preferences_dialog_set_account(f->dialog, NPUB, "Alice");
  confirm(f, "_Delete and Sign Out");
  g_assert_cmpuint(fake.calls, ==, 3);
  fake_complete(GH_PREFERENCES_FORGET_FAILED);
  spin_until(dialog_can_close, f->dialog);
  g_assert_cmpstr(gh_preferences_dialog_get_last_toast(f->dialog), ==,
                  "Couldn't delete all messages: The disk is read-only");
  g_assert_true(gtk_widget_get_sensitive(button));
  g_assert_true(gtk_widget_get_mapped(GTK_WIDGET(f->dialog)));
  fake_reset();
}

/* The deletion outlives the dialog's widgets (e.g. the window is destroyed at
 * quit): it still finishes, and the account updates that follow are ignored. */
static void
test_delete_all_outlives_dialog(Fixture *f, gconstpointer data)
{
  (void)data;
  present(f, 800, 700);
  fake.target = g_object_new(G_TYPE_OBJECT, NULL);
  gh_preferences_dialog_set_account(f->dialog, NPUB, "Alice");
  gh_preferences_dialog_set_forget_func(f->dialog, fake_forget_async, fake_forget_finish,
                                        fake.target);
  confirm(f, "_Delete and Sign Out");
  g_assert_cmpuint(fake.calls, ==, 1);
  gtk_window_destroy(f->window);
  f->window = NULL;
  g_object_run_dispose(G_OBJECT(f->dialog));
  gh_preferences_dialog_set_account(f->dialog, NULL, NULL);
  g_assert_null(gh_preferences_dialog_get_key_widget(f->dialog, "enter-sends"));
  fake_complete(GH_PREFERENCES_FORGET_FAILED);
  drain_idle();
  /* The finished deletion released the dialog: only the fixture holds it. */
  g_assert_cmpuint(G_OBJECT(f->dialog)->ref_count, ==, 1);
  fake_reset();
}

/* ---- features this build lacks (W13b review B2) ---------------------------------------- */

/* A row whose feature is missing: unbound, showing what the build does,
 * insensitive and saying why; its content and sound rows (notifications),
 * the Tor address and the attachment server editor are not shown at all. */
static void
assert_gated(Fixture *f, const char *key)
{
  GtkWidget *widget = gh_preferences_dialog_get_key_widget(f->dialog, key);
  g_assert_nonnull(widget);
  g_test_message("gated: %s", key);
  g_assert_false(gh_preferences_dialog_get_key_available(f->dialog, key));
  if (g_str_equal(key, "notification-privacy") || g_str_equal(key, "sound-enabled") ||
      g_str_equal(key, "tor-socks-address")) {
    g_assert_false(gtk_widget_get_visible(widget));
    return;
  }
  if (g_str_equal(key, "blossom-servers")) {
    g_assert_false(gtk_widget_get_sensitive(widget));
    g_assert_false(gtk_widget_get_visible(child(f, "blossom_entry")));
    return;
  }
  g_assert_false(gtk_widget_get_sensitive(widget));
  /* The older-format switch is not shown at all without the adopted
   * producer: that format is then the only one (review N4). */
  if (g_str_equal(key, "mls-legacy-key-packages")) {
    guint features = 0;
    g_object_get(f->dialog, "features", &features, NULL);
    g_assert_cmpint(gtk_widget_get_visible(widget), ==,
                    (features & GH_PREFERENCES_FEATURE_ADOPTED_KEY_PACKAGES) != 0);
  }
  const char *subtitle = adw_action_row_get_subtitle(ADW_ACTION_ROW(widget));
  gboolean always_on = g_str_equal(key, "filter-unknown-senders");
  if (always_on)
    g_assert_nonnull(strstr(subtitle, "Always on in this version"));
  else
    g_assert_cmpstr(subtitle, ==, NOT_AVAILABLE);
  if (ADW_IS_SWITCH_ROW(widget)) {
    AdwSwitchRow *row = ADW_SWITCH_ROW(widget);
    /* What the build does: off, except that requests are always apart. */
    g_assert_cmpint(adw_switch_row_get_active(row), ==, always_on);
    /* Unbound both ways. */
    g_settings_set_boolean(f->settings, key, !g_settings_get_boolean(f->settings, key));
    drain_idle();
    g_assert_cmpint(adw_switch_row_get_active(row), ==, always_on);
    g_settings_reset(f->settings, key);
    adw_switch_row_set_active(row, !always_on);
    drain_idle();
    g_autoptr(GVariant) user = g_settings_get_user_value(f->settings, key);
    g_assert_null(user);
    adw_switch_row_set_active(row, always_on);
  } else {
    AdwComboRow *row = ADW_COMBO_ROW(widget);
    /* The first choice: no notifications, no timer, keep everything. */
    g_assert_cmpuint(adw_combo_row_get_selected(row), ==, 0);
    adw_combo_row_set_selected(row, 1);
    drain_idle();
    g_autoptr(GVariant) user = g_settings_get_user_value(f->settings, key);
    g_assert_null(user);
    adw_combo_row_set_selected(row, 0);
  }
}

/* Every row of this build (src/app/gh-features.h) is live exactly when its
 * feature is: flipping a GH_FEATURE_* there flips its rows here. */
static void
test_gated_rows_this_build(Fixture *f, gconstpointer data)
{
  (void)data;
  static const struct {
    const char *key;
    gboolean available;
  } expect[] = {
    { "notifications-enabled", GH_FEATURE_NOTIFICATIONS },
    { "notification-privacy", GH_FEATURE_NOTIFICATIONS },
    { "sound-enabled", GH_FEATURE_NOTIFICATIONS },
    { "load-remote-images", GH_FEATURE_REMOTE_IMAGES },
    { "link-previews", GH_FEATURE_LINK_PREVIEWS },
    { "load-profile-pictures", GH_FEATURE_PROFILE_PICTURES },
    { "filter-unknown-senders", GH_FEATURE_REQUEST_FILTER },
    { "show-message-previews", TRUE },
    { "network-mode", TRUE },
    { "tor-socks-address", GH_FEATURE_TOR },
    { "discovery-relays", TRUE },
    { "run-in-background", TRUE },
    { "launch-on-login", TRUE },
    { "diagnostics-enabled", TRUE },
    { "retention-days", GH_FEATURE_EXPIRY },
    { "default-disappearing-seconds", GH_FEATURE_COMPOSER && GH_FEATURE_EXPIRY },
    { "enter-sends", GH_FEATURE_COMPOSER },
    { "blossom-servers", GH_FEATURE_ATTACHMENTS },
    { "only-join-verified-mls-groups", GH_FEATURE_ENCRYPTED_GROUPS },
    { "mls-legacy-key-packages", GH_FEATURE_ENCRYPTED_GROUPS && GH_FEATURE_ADOPTED_KEY_PACKAGES },
    { "default-dm-protocol", TRUE },
  };
  G_STATIC_ASSERT(G_N_ELEMENTS(expect) == G_N_ELEMENTS(preference_keys));
  present(f, 800, 700);
  for (guint i = 0; i < G_N_ELEMENTS(expect); i++) {
    g_assert_true(in_list(preference_keys, G_N_ELEMENTS(preference_keys), expect[i].key));
    if (!expect[i].available) {
      assert_gated(f, expect[i].key);
      continue;
    }
    g_assert_true(gh_preferences_dialog_get_key_available(f->dialog, expect[i].key));
    GtkWidget *widget = gh_preferences_dialog_get_key_widget(f->dialog, expect[i].key);
    if (ADW_IS_ACTION_ROW(widget))
      g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(widget)), !=, NOT_AVAILABLE);
  }
  /* W13b B2's claims, for this build: no notification is promised, and no
   * relay deletion unless something can send. */
  const char *disappearing = adw_preferences_group_get_description(child(f, "disappearing_group"));
  if (!GH_FEATURE_COMPOSER)
    g_assert_true(!disappearing || !strstr(disappearing, "relays"));
  if (!GH_FEATURE_NOTIFICATIONS) {
    g_assert_false(gtk_widget_get_visible(child(f, "notifications_note")));
    g_assert_false(adw_switch_row_get_active(child(f, "notifications_row")));
  }
  g_assert_false(gh_preferences_dialog_get_key_available(f->dialog, "no-such-key"));
}

/* With no feature at all, every gated row is gated at once. */
static void
test_gated_rows_none(Fixture *f, gconstpointer data)
{
  (void)data;
  static const char *const gated[] = {
    "notifications-enabled", "notification-privacy", "sound-enabled", "load-remote-images",
    "link-previews", "load-profile-pictures", "filter-unknown-senders", "tor-socks-address",
    "retention-days", "default-disappearing-seconds", "enter-sends", "blossom-servers",
    "only-join-verified-mls-groups", "mls-legacy-key-packages",
  };
  present(f, 800, 700);
  for (guint i = 0; i < G_N_ELEMENTS(gated); i++)
    assert_gated(f, gated[i]);
  g_assert_false(gtk_widget_get_visible(child(f, "web_note")));
  /* Nothing expires here, so nothing is said about it. */
  const char *disappearing = adw_preferences_group_get_description(child(f, "disappearing_group"));
  g_assert_cmpstr(disappearing ? disappearing : "", ==, "");
}

static GhPreferencesDialog *
dialog_with(GSettings *settings, GhPreferencesFeatures features)
{
  return g_object_ref_sink(gh_preferences_dialog_new(settings, features));
}

static gpointer
template_child(GhPreferencesDialog *dialog, const char *name)
{
  return gtk_widget_get_template_child(GTK_WIDGET(dialog), GH_TYPE_PREFERENCES_DIALOG, name);
}

/* The copy says only what the given build does (charter P4). */
static void
test_copy_follows_features(void)
{
  g_autoptr(GSettings) settings = g_settings_new(SCHEMA_ID);
  reset_all(settings);

  /* G07 without anything that sends: expiry deletes here, nothing asks relays. */
  g_autoptr(GhPreferencesDialog) expiry = dialog_with(settings, GH_PREFERENCES_FEATURE_EXPIRY);
  g_assert_cmpstr(adw_preferences_group_get_description(template_child(expiry,
                                                                       "disappearing_group")),
                  ==, "Messages that carry a timer are deleted from this device when they expire.");
  g_assert_true(gh_preferences_dialog_get_key_available(expiry, "retention-days"));
  g_assert_false(gh_preferences_dialog_get_key_available(expiry, "default-disappearing-seconds"));
  const char *keep = adw_action_row_get_subtitle(template_child(expiry, "retention_row"));
  g_assert_nonnull(strstr(keep, "deleted from this device only"));

  /* With sending too: sent messages ask relays (NIP-40), up to a day late. */
  g_autoptr(GhPreferencesDialog) sending =
    dialog_with(settings, GH_PREFERENCES_FEATURE_EXPIRY | GH_PREFERENCES_FEATURE_COMPOSER);
  const char *both = adw_preferences_group_get_description(template_child(sending,
                                                                          "disappearing_group"));
  g_assert_nonnull(strstr(both, "ask relays to delete them within about a day"));
  g_assert_nonnull(strstr(both, "may not honour this"));
  g_assert_true(gh_preferences_dialog_get_key_available(sending, "default-disappearing-seconds"));
  /* W14 review B1, nostrc-qp24.83: the default reaches new conversations
   * the account starts only (not a stranger's request); existing ones keep
   * theirs. */
  const char *timer = adw_action_row_get_subtitle(template_child(sending, "disappearing_row"));
  g_assert_nonnull(strstr(timer, "Only for new conversations you start"));
  g_assert_nonnull(strstr(timer, "Ones others start begin without it"));
  g_assert_nonnull(strstr(timer, "existing conversations keep their own timer"));

  /* Web content: the Tor clause only with Tor. */
  g_autoptr(GhPreferencesDialog) images =
    dialog_with(settings, GH_PREFERENCES_FEATURE_REMOTE_IMAGES);
  GtkLabel *note = template_child(images, "web_note");
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(note)));
  g_assert_null(strstr(gtk_label_get_text(note), "Tor"));
  g_autoptr(GhPreferencesDialog) all = dialog_with(settings, GH_PREFERENCES_FEATURES_ALL);
  g_assert_nonnull(strstr(gtk_label_get_text(template_child(all, "web_note")),
                          "unless you use Tor"));
  g_assert_true(gtk_widget_get_visible(template_child(all, "notifications_note")));

  /* Background delivery and login startup are separate choices. */
  const char *background = adw_action_row_get_subtitle(template_child(all,
                                                                      "run_in_background_row"));
  g_assert_null(strstr(background, "log in"));
  const char *launch = adw_action_row_get_subtitle(template_child(all,
                                                                  "launch_on_login_row"));
  g_assert_nonnull(strstr(launch, "inbox relays"));
  reset_all(settings);
}

/* ---- layout -------------------------------------------------------------------------- */

static void
test_minimum_size_layout(Fixture *f, gconstpointer data)
{
  (void)data;
  static const char *const pages[] = { "privacy", "messages", "network", "account" };
  gh_preferences_dialog_set_account(f->dialog, NPUB, "Alice");
  present(f, 360, 294);
  for (guint i = 0; i < G_N_ELEMENTS(pages); i++) {
    adw_preferences_dialog_set_visible_page_name(ADW_PREFERENCES_DIALOG(f->dialog), pages[i]);
    drain_idle();
    gtk_test_widget_wait_for_draw(GTK_WIDGET(f->window));
    /* The dialog is a bottom sheet here; its page must fit the window, which
     * keeps its minimum size (a larger content minimum would also warn, and
     * warnings are fatal). */
    GtkWidget *page = GTK_WIDGET(
      adw_preferences_dialog_get_visible_page(ADW_PREFERENCES_DIALOG(f->dialog)));
    g_assert_cmpstr(adw_preferences_page_get_name(ADW_PREFERENCES_PAGE(page)), ==, pages[i]);
    spin_until(is_mapped, page);
    int min_width = 0, min_height = 0;
    gtk_widget_measure(page, GTK_ORIENTATION_HORIZONTAL, -1, &min_width, NULL, NULL, NULL);
    gtk_widget_measure(page, GTK_ORIENTATION_VERTICAL, -1, &min_height, NULL, NULL, NULL);
    g_test_message("%s: minimum %dx%d, allocated %dx%d", pages[i], min_width, min_height,
                   gtk_widget_get_width(page), gtk_widget_get_height(page));
    g_assert_cmpint(min_width, <=, 360);
    g_assert_cmpint(min_height, <=, 294);
    g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(f->window)), ==, 360);
    g_assert_cmpint(gtk_widget_get_height(GTK_WIDGET(f->window)), ==, 294);
  }
}

/* ---- screenshots (opt-in evidence) ------------------------------------------------- */

static void
save_png(GtkWidget *window, const char *dir, const char *name)
{
  g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new(window);
  GtkSnapshot *snapshot = gtk_snapshot_new();
  gdk_paintable_snapshot(paintable, snapshot, gdk_paintable_get_intrinsic_width(paintable),
                         gdk_paintable_get_intrinsic_height(paintable));
  g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node(snapshot);
  g_assert_nonnull(node);
  graphene_rect_t bounds;
  gsk_render_node_get_bounds(node, &bounds);
  GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(window));
  g_autoptr(GdkTexture) texture = gsk_renderer_render_texture(renderer, node, &bounds);
  g_autofree char *path = g_strdup_printf("%s/groundhog-g17-%s.png", dir, name);
  g_assert_true(gdk_texture_save_to_png(texture, path));
  g_test_message("saved %s", path);
}

static void
take_shot(GSettings *settings, const char *dir, const char *page, gboolean confirm_delete,
          int width, int height, const char *name)
{
  GhWindow *window = gh_window_new(NULL);
  GhPreferencesDialog *dialog = gh_preferences_dialog_new(settings, gh_features_for_preferences());
  GObject *target = g_object_new(G_TYPE_OBJECT, NULL);
  gh_preferences_dialog_set_account(dialog, NPUB, "Alice");
  const gchar *published[] = { "wss://relay.example.com", NULL };
  gh_preferences_dialog_set_published_relays(dialog, published, published);
  gh_preferences_dialog_set_forget_func(dialog, fake_forget_async, fake_forget_finish, target);
  present_window(GTK_WINDOW(window), width, height);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
  adw_preferences_dialog_set_visible_page_name(ADW_PREFERENCES_DIALOG(dialog), page);
  spin_until(is_mapped, dialog);
  if (confirm_delete) {
    RootWait wait = { GTK_WIDGET(gtk_widget_get_template_child(
                        GTK_WIDGET(dialog), GH_TYPE_PREFERENCES_DIALOG, "delete_all_dialog")),
                      GTK_WINDOW(window) };
    g_assert_true(gtk_widget_activate_action(GTK_WIDGET(dialog), "prefs.delete-all", NULL));
    spin_until(alert_shown, &wait);
  }
  if (g_getenv("GROUNDHOG_PREFS_SHOT")) {
    GtkListBox *list = GTK_LIST_BOX(gtk_widget_get_template_child(GTK_WIDGET(dialog), GH_TYPE_PREFERENCES_DIALOG, "inbox_relays_list"));
    gtk_widget_grab_focus(GTK_WIDGET(gtk_list_box_get_row_at_index(list, 1)));
  }
  drain_idle();
  gtk_test_widget_wait_for_draw(GTK_WIDGET(window));
  save_png(GTK_WIDGET(window), dir, name);
  gtk_window_destroy(GTK_WINDOW(window));
  drain_idle();
  g_object_unref(target);
}

static void
test_screenshots(Fixture *f, gconstpointer data)
{
  (void)data;
  const char *dir = g_getenv("GROUNDHOG_TEST_SCREENSHOTS");
  if (!dir || !*dir) {
    g_test_skip("GROUNDHOG_TEST_SCREENSHOTS is not set");
    return;
  }
  if (g_getenv("GROUNDHOG_PREFS_SHOT")) {
    take_shot(f->settings, dir, "network", FALSE, 900, 700, "prefs");
    return;
  }
  /* As in test_conversation_list.c: switching the color scheme may warn about
   * libadwaita's own CSS on some GTK builds; criticals stay fatal. */
  GLogLevelFlags fatal = g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
  AdwStyleManager *style = adw_style_manager_get_default();
  static const struct {
    AdwColorScheme scheme;
    const char *name;
  } schemes[] = {
    { ADW_COLOR_SCHEME_FORCE_LIGHT, "light" },
    { ADW_COLOR_SCHEME_FORCE_DARK, "dark" },
  };
  static const char *const pages[] = { "privacy", "messages", "network", "account" };
  const char *const relays[] = { "wss://relay.example.com", NULL };
  g_settings_set_strv(f->settings, "discovery-relays", relays);
  for (guint s = 0; s < G_N_ELEMENTS(schemes); s++) {
    adw_style_manager_set_color_scheme(style, schemes[s].scheme);
    for (guint p = 0; p < G_N_ELEMENTS(pages); p++) {
      g_autofree char *narrow = g_strdup_printf("%s-%s-narrow", pages[p], schemes[s].name);
      take_shot(f->settings, dir, pages[p], FALSE, 360, 720, narrow);
    }
    g_autofree char *confirm_name = g_strdup_printf("delete-confirm-%s-narrow", schemes[s].name);
    take_shot(f->settings, dir, "account", TRUE, 360, 720, confirm_name);
    g_autofree char *wide = g_strdup_printf("privacy-%s-wide", schemes[s].name);
    take_shot(f->settings, dir, "privacy", FALSE, 900, 700, wide);
  }
  adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_DEFAULT);
  g_log_set_always_fatal(fatal);
  fake_reset();
}

static void
test_npub_qr_texture(void)
{
  g_autofree gchar *uri = g_strconcat("nostr:", NPUB, NULL);
  g_autoptr(GdkTexture) texture = gh_qr_code_texture_new(uri);
  g_assert_nonnull(texture);
  g_assert_cmpint(gdk_texture_get_width(texture), ==, gdk_texture_get_height(texture));
  g_assert_cmpint(gdk_texture_get_width(texture), >, 100);
}

int
main(int argc, char **argv)
{
  if (!gtk_init_check()) {
    g_printerr("groundhog-preferences test skipped: no graphical display\n");
    return 77;
  }
  adw_init();
  groundhog_register_resource();
  g_autoptr(GtkCssProvider) css = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(css, "/org/nostr/Groundhog/style.css");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  /* As in test_shell_layout.c: 1sp is one pixel, no animations, and GNOME's
   * close-only window controls. */
  g_object_set(gtk_settings_get_default(), "gtk-xft-dpi", 96 * 1024, "gtk-enable-animations",
               FALSE, "gtk-decoration-layout", "appmenu:close", NULL);

  g_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  const GhPreferencesFeatures build = gh_features_for_preferences();
  const GhPreferencesFeatures all = GH_PREFERENCES_FEATURES_ALL;
  const GhPreferencesFeatures no_tor = all & ~GH_PREFERENCES_FEATURE_TOR;
#define ADD(path, func, features)                                                             \
  g_test_add("/groundhog/preferences/" path, Fixture, GUINT_TO_POINTER(features),             \
             fixture_setup, func, fixture_teardown)
  ADD("every-key-has-a-row", test_every_key_has_a_row, build);
  ADD("every-key-has-a-row-every-feature", test_every_key_has_a_row, all);
  ADD("gated-rows-this-build", test_gated_rows_this_build, build);
  ADD("gated-rows-no-feature", test_gated_rows_none, 0);
  /* The bindings themselves, with every feature a row can need. */
  ADD("switch-rows-bind-both-ways", test_switch_rows_bind_both_ways, all);
  ADD("choice-rows-bind-both-ways", test_choice_rows_bind_both_ways, no_tor);
  ADD("tor-hidden-without-g09", test_tor_hidden_without_g09, no_tor);
  ADD("tor-with-g09", test_tor_with_g09, all);
  ADD("url-lists-bind-both-ways", test_url_lists_bind_both_ways, no_tor);
  ADD("account-rows", test_account_rows, build);
  ADD("key-package-row", test_key_package_row, all);
  ADD("key-package-row-no-feature", test_key_package_row,
      all & ~GH_PREFERENCES_FEATURE_ENCRYPTED_GROUPS);
  ADD("older-marmot-switch", test_older_marmot_switch, all);
  ADD("older-marmot-switch-no-producer", test_older_marmot_switch,
      all & ~GH_PREFERENCES_FEATURE_ADOPTED_KEY_PACKAGES);
  ADD("published-relays", test_published_relays, all);
  ADD("relay-layout", test_relay_layout, all);
  ADD("relay-controls", test_relay_controls, all);
  ADD("delete-all-runs-forget", test_delete_all_runs_forget, build);
  ADD("delete-all-outlives-dialog", test_delete_all_outlives_dialog, build);
  ADD("minimum-size-layout", test_minimum_size_layout, build);
  ADD("minimum-size-layout-every-feature", test_minimum_size_layout, all);
  ADD("screenshots", test_screenshots, build);
#undef ADD
  g_test_add_func("/groundhog/preferences/copy-follows-features", test_copy_follows_features);
  g_test_add_func("/groundhog/preferences/url-rules", test_url_rules);
  g_test_add_func("/groundhog/preferences/npub-qr-texture", test_npub_qr_texture);
  return g_test_run();
}
