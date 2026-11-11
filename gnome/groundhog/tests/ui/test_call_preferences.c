/* Preferences › Calls (nostrc-mqrgk.5) on a memory GSettings backend, with a
 * private test bus: no relay, no signer, no network. Covered: without the
 * CALLS feature the page is hidden (widget and view-stack page) and every
 * call row is gated; with it the page shows, the STUN list follows its key
 * both ways in order and refuses anything but a credential-free stun://
 * address (Move Up/Down and Remove per row), the TURN rows follow their keys
 * and refuse the same for turn:// and turns://, the relay switch is
 * insensitive until a credential is saved and says so (usable to turn a
 * stored on off), the Tor note replaces the local-network note in Tor mode,
 * and a credential apply refuses without a server, then reaches a Secret
 * Service (tests/app/test_secret_service.py on the private bus): saved, the
 * row cleared, the status found again and the relay switch unlocked. Needs a
 * display and dbus-daemon: it self-skips (77) without either. Waits iterate
 * the main context against a deadline; they never sleep.
 *
 * Two processes: the parent owns the private bus (as the session bus) and
 * the Secret Service under its lifeline, and runs this executable again as
 * the child, which does GTK and the cases. GTK and libadwaita keep the
 * session connection they were given for the process's life, so a child is
 * what lets the bus teardown verify that nothing leaked the connection. */
#include "gh-features.h"
#include "gh-preferences-dialog.h"
#include "nostrc-test-bus.h"
#include "nostrc-test-gdk-frame.h"
#if GROUNDHOG_HAVE_CALLS
#include <libsecret/secret.h>
#endif

#include <string.h>

void groundhog_register_resource(void);

#define SCHEMA_ID "org.nostr.Groundhog"
#define NOT_AVAILABLE "Not available in this version yet"

static const char *const call_keys[] = {
  "allow-calls-from-unaccepted", "call-stun-servers", "call-turn-url", "call-turn-username",
  "call-relay-only",
};

static NostrcTestBus *bus;

/* ---- waits ------------------------------------------------------------------- */

static gboolean
deadline_hit(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

static gboolean
tick(gpointer data)
{
  (void)data;
  return G_SOURCE_CONTINUE;
}

static void
spin_until_at(gboolean (*pred)(gpointer), gpointer data, int line)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(20, deadline_hit, &expired);
  guint ticker = g_timeout_add(10, tick, NULL);
  while (!pred(data) && !expired)
    g_main_context_iteration(NULL, TRUE);
  g_source_remove(ticker);
  if (expired)
    g_error("condition waited for at line %d did not hold within 20s", line);
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

typedef struct {
  GSettings *settings;
  GhPreferencesDialog *dialog;
  GtkWindow *window;
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
  drain_idle();
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

static void
present(Fixture *f)
{
  f->window = GTK_WINDOW(adw_window_new());
  gtk_window_set_default_size(f->window, 800, 700);
  gtk_window_present(f->window);
  spin_until(is_mapped, f->window);
  adw_dialog_present(ADW_DIALOG(f->dialog), GTK_WIDGET(f->window));
  spin_until(is_mapped, f->dialog);
  drain_idle();
}

static guint
n_rows(GtkListBox *list)
{
  guint n = 0;
  while (gtk_list_box_get_row_at_index(list, n))
    n++;
  return n;
}

static const char *
row_title(GtkListBox *list, guint index)
{
  GtkListBoxRow *row = gtk_list_box_get_row_at_index(list, index);
  g_assert_nonnull(row);
  return adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row));
}

static GtkWidget *
find_tooltip_button(GtkWidget *root, const char *tooltip)
{
  if (GTK_IS_BUTTON(root) && !g_strcmp0(gtk_widget_get_tooltip_text(root), tooltip))
    return root;
  for (GtkWidget *c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = find_tooltip_button(c, tooltip);
    if (found)
      return found;
  }
  return NULL;
}

static void
assert_strv(Fixture *f, const char *key, const char *const *expected)
{
  g_auto(GStrv) actual = g_settings_get_strv(f->settings, key);
  g_assert_true(g_strv_equal((const char *const *)actual, expected));
}

static void
apply_entry(AdwEntryRow *entry, const char *text)
{
  gtk_editable_set_text(GTK_EDITABLE(entry), text);
  g_signal_emit_by_name(entry, "apply");
  drain_idle();
}

static const char *
credential_status(Fixture *f)
{
  return adw_action_row_get_subtitle(ADW_ACTION_ROW(child(f, "turn_credential_row")));
}

static gboolean
credential_settled(gpointer data)
{
  return !g_str_has_prefix(credential_status(data), "Checking");
}

static AdwViewStackPage *
stack_page(Fixture *f)
{
  GtkWidget *page = child(f, "calls_page");
  GtkWidget *stack = gtk_widget_get_ancestor(page, ADW_TYPE_VIEW_STACK);
  g_assert_nonnull(stack);
  AdwViewStackPage *entry = adw_view_stack_get_page(ADW_VIEW_STACK(stack), page);
  g_assert_nonnull(entry);
  return entry;
}

/* ---- without the feature ------------------------------------------------------- */

static void
test_hidden_without_feature(Fixture *f, gconstpointer data)
{
  (void)data;
  present(f);
  g_assert_false(gtk_widget_get_visible(child(f, "calls_page")));
  g_assert_false(adw_view_stack_page_get_visible(stack_page(f)));
  for (guint i = 0; i < G_N_ELEMENTS(call_keys); i++) {
    g_assert_false(gh_preferences_dialog_get_key_available(f->dialog, call_keys[i]));
    GtkWidget *widget = gh_preferences_dialog_get_key_widget(f->dialog, call_keys[i]);
    g_assert_nonnull(widget);
    g_assert_false(gtk_widget_get_sensitive(widget));
  }
  g_assert_false(gtk_widget_get_sensitive(child(f, "turn_password_row")));
  g_assert_false(gtk_widget_get_visible(child(f, "stun_entry")));
  g_assert_cmpstr(credential_status(f), ==, NOT_AVAILABLE);
  g_assert_cmpstr(adw_action_row_get_subtitle(child(f, "relay_only_row")), ==, NOT_AVAILABLE);
  /* Unbound: the stored values are shown once and nothing follows. */
  AdwSwitchRow *relay = child(f, "relay_only_row");
  g_assert_false(adw_switch_row_get_active(relay));
  g_settings_set_boolean(f->settings, "call-relay-only", TRUE);
  drain_idle();
  g_assert_false(adw_switch_row_get_active(relay));
  g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(relay)), ==, NOT_AVAILABLE);
  /* A credential apply says why nothing happens. */
  AdwEntryRow *password = child(f, "turn_password_row");
  apply_entry(password, "secret");
  g_assert_true(gtk_widget_get_visible(child(f, "turn_error")));
  g_assert_cmpstr(gtk_label_get_text(child(f, "turn_error")), ==, NOT_AVAILABLE);
}

/* ---- with the feature ---------------------------------------------------------- */

static void
test_shown_with_feature(Fixture *f, gconstpointer data)
{
  (void)data;
  present(f);
  g_assert_true(gtk_widget_get_visible(child(f, "calls_page")));
  g_assert_true(adw_view_stack_page_get_visible(stack_page(f)));
  for (guint i = 0; i < G_N_ELEMENTS(call_keys); i++)
    g_assert_true(gh_preferences_dialog_get_key_available(f->dialog, call_keys[i]));
  /* The schema's defaults, in the order the backend reads them (the first
   * entry is the active one), each movable and removable. */
  GtkListBox *list = GTK_LIST_BOX(gh_preferences_dialog_get_key_widget(f->dialog, "call-stun-servers"));
  g_assert_true(list == child(f, "stun_list"));
  g_assert_true(gtk_widget_get_visible(child(f, "stun_entry")));
  g_assert_cmpuint(n_rows(list), ==, 2);
  g_assert_cmpstr(row_title(list, 0), ==, "stun://stun.cloudflare.com:3478");
  g_assert_cmpstr(row_title(list, 1), ==, "stun://stun.l.google.com:19302");
  GtkWidget *first = GTK_WIDGET(gtk_list_box_get_row_at_index(list, 0));
  g_assert_false(gtk_widget_get_sensitive(find_tooltip_button(first, "Move Up")));
  g_assert_true(gtk_widget_get_sensitive(find_tooltip_button(first, "Move Down")));
  g_assert_nonnull(find_tooltip_button(first, "Remove"));
  /* The switch follows its key both ways. */
  AdwSwitchRow *ring = child(f, "calls_from_unaccepted_row");
  g_assert_true(GTK_WIDGET(ring) ==
                gh_preferences_dialog_get_key_widget(f->dialog, "allow-calls-from-unaccepted"));
  g_assert_false(adw_switch_row_get_active(ring));
  adw_switch_row_set_active(ring, TRUE);
  drain_idle();
  g_assert_true(g_settings_get_boolean(f->settings, "allow-calls-from-unaccepted"));
  g_settings_set_boolean(f->settings, "allow-calls-from-unaccepted", FALSE);
  drain_idle();
  g_assert_false(adw_switch_row_get_active(ring));
}

static void
test_stun_rules(Fixture *f, gconstpointer data)
{
  (void)data;
#if !GROUNDHOG_HAVE_CALLS
  (void)f;
  g_test_skip("the ICE URL rules (gh-call-settings.c) are not in this build");
  return;
#else
  present(f);
  GtkListBox *list = child(f, "stun_list");
  AdwEntryRow *entry = child(f, "stun_entry");
  GtkWidget *error = child(f, "stun_error");
  const char *const defaults[] = { "stun://stun.cloudflare.com:3478",
                                   "stun://stun.l.google.com:19302", NULL };
  static const char *const bad[] = {
    "", "stun.example.net:3478", "turn://stun.example.net:3478", "https://stun.example.net",
    "stun://", "stun://user@stun.example.net:3478", "stun://stun.example.net:3478/path",
    "stun://stun.example.net:3478#x", "stun://stun.example.net:3478?transport=tls",
    "stun://stun.cloudflare.com:3478", /* already listed */
  };
  for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
    apply_entry(entry, bad[i]);
    if (!gtk_widget_get_visible(error))
      g_error("call-stun-servers accepted \"%s\"", bad[i]);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(error)), !=, "");
    g_assert_true(gtk_widget_has_css_class(GTK_WIDGET(entry), "error"));
    assert_strv(f, "call-stun-servers", defaults);
    g_assert_cmpuint(n_rows(list), ==, 2);
  }
  /* A valid entry is trimmed and appended, then the entry is cleared. */
  apply_entry(entry, "  stun://stun.example.net:3478 ");
  g_assert_false(gtk_widget_get_visible(error));
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(entry)), ==, "");
  assert_strv(f, "call-stun-servers",
              (const char *const[]){ defaults[0], defaults[1], "stun://stun.example.net:3478", NULL });
  g_assert_cmpuint(n_rows(list), ==, 3);
  /* Move Up makes it the active one in two steps; Remove drops it. */
  GtkWidget *up = find_tooltip_button(GTK_WIDGET(gtk_list_box_get_row_at_index(list, 2)), "Move Up");
  g_signal_emit_by_name(up, "clicked");
  drain_idle();
  up = find_tooltip_button(GTK_WIDGET(gtk_list_box_get_row_at_index(list, 1)), "Move Up");
  g_signal_emit_by_name(up, "clicked");
  drain_idle();
  assert_strv(f, "call-stun-servers",
              (const char *const[]){ "stun://stun.example.net:3478", defaults[0], defaults[1], NULL });
  g_assert_cmpstr(row_title(list, 0), ==, "stun://stun.example.net:3478");
  GtkWidget *remove = find_tooltip_button(GTK_WIDGET(gtk_list_box_get_row_at_index(list, 0)), "Remove");
  g_signal_emit_by_name(remove, "clicked");
  drain_idle();
  assert_strv(f, "call-stun-servers", defaults);
  /* key -> rows: an empty list shows none (and uses none). */
  g_settings_set_strv(f->settings, "call-stun-servers", (const char *const[]){ NULL });
  drain_idle();
  g_assert_cmpuint(n_rows(list), ==, 0);
  g_settings_set_strv(f->settings, "call-stun-servers", (const char *const[]){ defaults[1], NULL });
  drain_idle();
  g_assert_cmpuint(n_rows(list), ==, 1);
  g_assert_cmpstr(row_title(list, 0), ==, defaults[1]);
#endif
}

static void
test_turn_rules(Fixture *f, gconstpointer data)
{
  (void)data;
#if !GROUNDHOG_HAVE_CALLS
  (void)f;
  g_test_skip("the ICE URL rules (gh-call-settings.c) are not in this build");
  return;
#else
  present(f);
  AdwEntryRow *url = child(f, "turn_url_row");
  AdwEntryRow *username = child(f, "turn_username_row");
  GtkWidget *error = child(f, "turn_error");
  g_assert_true(GTK_WIDGET(url) == gh_preferences_dialog_get_key_widget(f->dialog, "call-turn-url"));
  g_assert_true(GTK_WIDGET(username) ==
                gh_preferences_dialog_get_key_widget(f->dialog, "call-turn-username"));
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(url)), ==, "");
  apply_entry(url, " turn://turn.example.net:3478?transport=udp ");
  g_assert_false(gtk_widget_get_visible(error));
  g_autofree char *stored = g_settings_get_string(f->settings, "call-turn-url");
  g_assert_cmpstr(stored, ==, "turn://turn.example.net:3478?transport=udp");
  static const char *const bad[] = {
    "turn.example.net:3478", "stun://turn.example.net:3478", "turn://user:pw@turn.example.net:3478",
    "turn://turn.example.net:3478?transport=tls", "turn://turn.example.net:3478/x",
    "turns://turn.example.net:5349#frag", "turns://", "https://turn.example.net",
  };
  for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
    apply_entry(url, bad[i]);
    if (!gtk_widget_get_visible(error))
      g_error("call-turn-url accepted \"%s\"", bad[i]);
    g_assert_true(gtk_widget_has_css_class(GTK_WIDGET(url), "error"));
    g_autofree char *unchanged = g_settings_get_string(f->settings, "call-turn-url");
    g_assert_cmpstr(unchanged, ==, "turn://turn.example.net:3478?transport=udp");
  }
  /* Typing clears the error; key -> row; empty clears the server. */
  gtk_editable_set_text(GTK_EDITABLE(url), "x");
  g_assert_false(gtk_widget_get_visible(error));
  g_settings_set_string(f->settings, "call-turn-url", "turns://turn.example.net:5349?transport=tcp");
  drain_idle();
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(url)), ==,
                  "turns://turn.example.net:5349?transport=tcp");
  apply_entry(url, "");
  g_autofree char *cleared = g_settings_get_string(f->settings, "call-turn-url");
  g_assert_cmpstr(cleared, ==, "");
  /* The username is any text, trimmed. */
  apply_entry(username, "  alice ");
  g_assert_false(gtk_widget_get_visible(error));
  g_autofree char *user = g_settings_get_string(f->settings, "call-turn-username");
  g_assert_cmpstr(user, ==, "alice");
  g_settings_set_string(f->settings, "call-turn-username", "bob");
  drain_idle();
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(username)), ==, "bob");
#endif
}

static void
test_relay_only_needs_credential(Fixture *f, gconstpointer data)
{
  (void)data;
  present(f);
  AdwSwitchRow *relay = child(f, "relay_only_row");
  g_assert_true(GTK_WIDGET(relay) == gh_preferences_dialog_get_key_widget(f->dialog, "call-relay-only"));
  g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(relay)));
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(relay)), "Needs a TURN server"));
  g_assert_nonnull(strstr(credential_status(f), "server address and username first"));
  /* A server and username alone: the store is asked and has none (or no
   * store answers); either way nothing is saved and the switch stays off. */
  g_settings_set_string(f->settings, "call-turn-url", "turn://turn.example.net:3478");
  g_settings_set_string(f->settings, "call-turn-username", "alice");
  drain_idle();
  spin_until(credential_settled, f);
  g_test_message("credential status: %s", credential_status(f));
  g_assert_null(strstr(credential_status(f), "Saved for"));
  g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(relay)));
  /* A stored on (e.g. from dconf) stays reachable to turn off, and says
   * that calls can't connect. */
  g_settings_set_boolean(f->settings, "call-relay-only", TRUE);
  drain_idle();
  g_assert_true(adw_switch_row_get_active(relay));
  g_assert_true(gtk_widget_get_sensitive(GTK_WIDGET(relay)));
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(relay)), "can't connect"));
  adw_switch_row_set_active(relay, FALSE);
  drain_idle();
  g_assert_false(g_settings_get_boolean(f->settings, "call-relay-only"));
  g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(relay)));
  /* Clearing the server: nothing to look up. */
  g_settings_set_string(f->settings, "call-turn-url", "");
  drain_idle();
  g_assert_nonnull(strstr(credential_status(f), "server address and username first"));
}

static void
test_network_notes(Fixture *f, gconstpointer data)
{
  (void)data;
  present(f);
  GtkLabel *tor = child(f, "calls_tor_note");
  GtkLabel *network = child(f, "calls_network_note");
  g_assert_false(gtk_widget_get_visible(GTK_WIDGET(tor)));
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(network)));
  g_assert_nonnull(strstr(gtk_label_get_text(network), "Allow Local Network is off"));
  g_assert_nonnull(strstr(gtk_label_get_text(network), "never shares"));
  g_assert_nonnull(strstr(gtk_label_get_text(network), "IP address"));
  g_settings_set_boolean(f->settings, "allow-local-network", TRUE);
  drain_idle();
  g_assert_nonnull(strstr(gtk_label_get_text(network), "Allow Local Network is on"));
  g_settings_set_string(f->settings, "network-mode", "tor");
  drain_idle();
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(tor)));
  g_assert_nonnull(strstr(gtk_label_get_text(tor), "Tor mode"));
  g_assert_nonnull(strstr(gtk_label_get_text(tor), "IP address"));
  g_assert_false(gtk_widget_get_visible(GTK_WIDGET(network)));
  g_settings_set_string(f->settings, "network-mode", "none");
  drain_idle();
  g_assert_false(gtk_widget_get_visible(GTK_WIDGET(tor)));
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(network)));
}

/* ---- the secret store ---------------------------------------------------------- */

typedef struct {
  const gchar *name;
  gboolean appeared;
} NameWatch;

static void
on_name_appeared(GDBusConnection *connection, const gchar *name, const gchar *name_owner,
                 gpointer user_data)
{
  (void)connection;
  (void)name;
  (void)name_owner;
  ((NameWatch *)user_data)->appeared = TRUE;
}

static gboolean
name_seen(gpointer data)
{
  return ((NameWatch *)data)->appeared;
}

/* tests/app/test_secret_service.py, as the two-instance acceptance test runs
 * it: a plaintext JSON store in the bus directory, under the bus lifeline. */
static void
secret_service_up(void)
{
  const gchar *script = g_getenv("SECRET_SERVICE_SCRIPT");
  const gchar *python = g_getenv("SECRET_SERVICE_PYTHON");
  g_assert_nonnull(script);
  g_assert_nonnull(python);
  g_autofree gchar *store = g_build_filename(nostrc_test_bus_get_dir(bus), "secrets.json", NULL);
  const gchar *argv[] = { python, script, store, NULL };
  nostrc_test_bus_spawn_supervised(bus, "secret-service.log", NULL, argv);
  NameWatch watch = { "org.freedesktop.secrets", FALSE };
  guint watcher = g_bus_watch_name(G_BUS_TYPE_SESSION, watch.name, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                   on_name_appeared, NULL, &watch, NULL);
  spin_until(name_seen, &watch);
  g_bus_unwatch_name(watcher);
}

static gboolean
toast_or_error(gpointer data)
{
  Fixture *f = data;
  return gh_preferences_dialog_get_last_toast(f->dialog) != NULL ||
         gtk_widget_get_visible(child(f, "turn_error"));
}

static gboolean
credential_saved(gpointer data)
{
  return strstr(credential_status(data), "Saved for") != NULL;
}

static void
test_credential_saved(Fixture *f, gconstpointer data)
{
  (void)data;
  present(f);
  AdwEntryRow *password = child(f, "turn_password_row");
  GtkWidget *error = child(f, "turn_error");
  /* Nothing to save to yet: the apply says so, in place. */
  apply_entry(password, "s3cret");
  g_assert_true(gtk_widget_get_visible(error));
  g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(error)), "server address and username"));
  g_assert_true(gtk_widget_has_css_class(GTK_WIDGET(password), "error"));
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(password)), ==, "s3cret");
  g_assert_null(gh_preferences_dialog_get_last_toast(f->dialog));
  g_settings_set_string(f->settings, "call-turn-url", "turns://turn.example.net:5349?transport=tcp");
  g_settings_set_string(f->settings, "call-turn-username", "alice");
  drain_idle();
  spin_until(credential_settled, f);
#if !GROUNDHOG_HAVE_CALLS
  g_assert_cmpstr(credential_status(f), ==, NOT_AVAILABLE);
  g_test_skip("the secret store client (gh-call-settings.c) is not in this build");
  return;
#else
  g_test_message("before saving: %s", credential_status(f));
  g_assert_nonnull(strstr(credential_status(f), "None saved for alice at"));
  apply_entry(password, "");
  g_assert_true(gtk_widget_get_visible(error));
  g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(error)), "Enter the credential"));
  /* The real client against the test Secret Service: saved, cleared from the
   * row, found again, and the relay switch unlocked. */
  apply_entry(password, "s3cret");
  g_assert_false(gtk_widget_get_visible(error));
  spin_until(toast_or_error, f);
  if (gtk_widget_get_visible(error))
    g_error("the credential was not saved: %s", gtk_label_get_text(GTK_LABEL(error)));
  g_assert_cmpstr(gh_preferences_dialog_get_last_toast(f->dialog), ==, "Credential saved");
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(password)), ==, "");
  spin_until(credential_saved, f);
  g_assert_cmpstr(credential_status(f), ==,
                  "Saved for alice at turns://turn.example.net:5349?transport=tcp");
  AdwSwitchRow *relay = child(f, "relay_only_row");
  g_assert_true(gtk_widget_get_sensitive(GTK_WIDGET(relay)));
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(relay)), "never learns"));
  adw_switch_row_set_active(relay, TRUE);
  drain_idle();
  g_assert_true(g_settings_get_boolean(f->settings, "call-relay-only"));
  /* Another username has its own credential: none yet, and the switch,
   * still on, says calls can't connect. */
  g_settings_set_string(f->settings, "call-turn-username", "bob");
  drain_idle();
  spin_until(credential_settled, f);
  g_assert_nonnull(strstr(credential_status(f), "None saved for bob at"));
  g_assert_true(gtk_widget_get_sensitive(GTK_WIDGET(relay)));
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(relay)), "can't connect"));
  g_settings_set_string(f->settings, "call-turn-username", "alice");
  drain_idle();
  spin_until(credential_saved, f);
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(relay)), "never learns"));
  /* Nothing of the credential is in the settings. */
  g_autoptr(GSettingsSchema) schema = NULL;
  g_object_get(f->settings, "settings-schema", &schema, NULL);
  g_auto(GStrv) keys = g_settings_schema_list_keys(schema);
  for (guint i = 0; keys[i]; i++) {
    g_autoptr(GVariant) value = g_settings_get_value(f->settings, keys[i]);
    g_autofree gchar *text = g_variant_print(value, FALSE);
    g_assert_null(strstr(text, "s3cret"));
  }
#endif
}

#define CHILD_VARIABLE "GH_CALL_PREFERENCES_CHILD"

/* The cases, on the session bus the parent exported. */
static int
child_main(int argc, char **argv)
{
#ifdef __APPLE__
  /* As send-stack.h: GTK's macOS accessibility backend cannot announce. */
  g_setenv("GTK_A11Y", "none", FALSE);
#endif
  if (!gtk_init_check()) {
    g_printerr("groundhog-call-preferences test skipped: no graphical display\n");
    return 77;
  }
  adw_init();
  groundhog_register_resource();
  g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
  g_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  const GhPreferencesFeatures all = GH_PREFERENCES_FEATURES_ALL;
  const GhPreferencesFeatures no_calls = all & ~GH_PREFERENCES_FEATURE_CALLS;
#define ADD(path, func, features)                                                             \
  g_test_add("/groundhog/preferences/calls/" path, Fixture, GUINT_TO_POINTER(features),       \
             fixture_setup, func, fixture_teardown)
  ADD("hidden-without-feature", test_hidden_without_feature, no_calls);
  ADD("shown-with-feature", test_shown_with_feature, all);
  ADD("stun-rules", test_stun_rules, all);
  ADD("turn-rules", test_turn_rules, all);
  ADD("relay-only-needs-credential", test_relay_only_needs_credential, all);
  ADD("network-notes", test_network_notes, all);
  ADD("credential-saved", test_credential_saved, all);
#undef ADD
  int result = g_test_run();
#if GROUNDHOG_HAVE_CALLS
  /* libsecret's service singleton holds the session connection. */
  secret_service_disconnect();
#endif
  return result;
}

int
main(int argc, char **argv)
{
  if (g_getenv(CHILD_VARIABLE))
    return child_main(argc, argv);
  if (!nostrc_test_bus_available()) {
    g_printerr("groundhog-call-preferences test skipped: no dbus-daemon\n");
    return 77;
  }
  bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  nostrc_test_bus_up(bus); /* exports DBUS_SESSION_BUS_ADDRESS for the child */
  secret_service_up();
  g_autoptr(GError) error = NULL;
  g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_setenv(launcher, CHILD_VARIABLE, "1", TRUE);
  g_autoptr(GSubprocess) child =
    g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv, &error);
  g_assert_no_error(error);
  g_subprocess_wait(child, NULL, &error);
  g_assert_no_error(error);
  int result = g_subprocess_get_if_exited(child) ? g_subprocess_get_exit_status(child) : 1;
  if (!g_subprocess_get_if_exited(child))
    g_printerr("groundhog-call-preferences: the child died with signal %d\n",
               g_subprocess_get_term_sig(child));
  nostrc_test_bus_down(bus);
  return result;
}
