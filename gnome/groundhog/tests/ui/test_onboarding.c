/* The onboarding flow (privacy charter §7.8, §8.2 G14) through the real
 * window: GhOnboardingView attached to a GhWindow with the account pages
 * (gh-account-ui.c) and the inbox status wiring, on a real account
 * controller (the mock org.nostr.Signer on a private bus), relay-list
 * discovery and NIP-17 inbox, all on recording relay transports (H1).
 *
 *  UX-6  choosing an identity writes only org.nostr.Groundhog current-npub
 *        (Gnostr's key and every other Groundhog key are unchanged), and
 *        each skip path (read-only, "Set Up Later", a declined signature)
 *        reaches the main view with the right banner, whose [Set Up] opens
 *        the inbox step again;
 *  PT-9  with no relay confirmed, the welcome, account, signer, inbox and
 *        confirm pages open zero connections; Publish then contacts exactly
 *        the relays the confirm page listed;
 *  UX-1  every page fits the 360x294 minimum.
 * With GROUNDHOG_TEST_SCREENSHOTS=<dir> the screenshots case renders every
 * step, wide and at 360x294, light and dark, to
 * <dir>/groundhog-g14-<n>-<step>-<size>-<scheme>.png. Needs a display; skips
 * (77) without one. Waits iterate the main context with a failure deadline;
 * nothing sleeps. */
#include "gh-account-ui.h"
#include "gh-inbox-status.h"
#include "gh-onboarding-view.h"
#include "../app/gh-test-signer.h"

#include <glib/gstdio.h>

void groundhog_register_resource(void);

static gchar *npub[GH_TEST_KEYS];
static gchar *hex[GH_TEST_KEYS];
/* One private session bus for the whole binary, up before GTK and
 * libadwaita start: they reach for the session bus (the settings portal, the
 * accessibility bus) during init and keep that connection, and
 * nostrc-test-bus requires that no other session connection exists when it
 * comes up. GhSigner reaches the mock signer on the session bus. */
static GhTestBus bus;
static gchar *a11y_services; /* a service dir with only org.a11y.Bus */

/* ---- recording transports ------------------------------------------------ */

typedef struct {
  GhRelayScope *scope;
  gchar *url;
  gboolean probe;
  gboolean closed;
} ScopeOpen;

typedef struct {
  GhRelayPublish *publish;
  gchar *url;
  gboolean closed;
} PubOpen;

typedef struct {
  GPtrArray *scopes;
  GPtrArray *pubs;
} Recorder;

static void
scope_open_free(gpointer data)
{
  ScopeOpen *open = data;
  gh_relay_scope_unref(open->scope);
  g_free(open->url);
  g_free(open);
}

static gpointer
scope_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters, gpointer data,
           GError **error)
{
  Recorder *rec = data;
  (void)error;
  const NostrFilter *filter = &filters->filters[0];
  ScopeOpen *open = g_new0(ScopeOpen, 1);
  open->scope = gh_relay_scope_ref(scope);
  open->url = g_strdup(url);
  open->probe = nostr_filter_kinds_len(filter) == 1 && nostr_filter_kinds_get(filter, 0) == 1059 &&
                nostr_filter_authors_len(filter) == 0;
  g_ptr_array_add(rec->scopes, open);
  return open;
}

static void
scope_close(gpointer handle, gpointer data)
{
  (void)data;
  ((ScopeOpen *)handle)->closed = TRUE;
}

static void
pub_open_free(gpointer data)
{
  PubOpen *open = data;
  gh_relay_publish_unref(open->publish);
  g_free(open->url);
  g_free(open);
}

static gpointer
pub_open(GhRelayPublish *publish, const gchar *url, const gchar *event_json, gpointer data,
         GError **error)
{
  Recorder *rec = data;
  (void)event_json;
  (void)error;
  PubOpen *open = g_new0(PubOpen, 1);
  open->publish = gh_relay_publish_ref(publish);
  open->url = g_strdup(url);
  g_ptr_array_add(rec->pubs, open);
  return open;
}

static void
pub_close(gpointer handle, gpointer data)
{
  (void)data;
  ((PubOpen *)handle)->closed = TRUE;
}

static const GhRelayTransport scope_transport = { scope_open, scope_close };
static const GhRelayPublishTransport pub_transport = { pub_open, pub_close };

/* ---- fixture ---------------------------------------------------------------- */

static GPtrArray *
fake_list(gpointer data, GError **error)
{
  (void)data;
  (void)error;
  GPtrArray *ids = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  static const gchar *const labels[] = { NULL, "Robin", NULL };
  for (guint key = 1; key <= 2; key++) {
    GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
    info->npub = g_strdup(npub[key]);
    info->label = g_strdup(labels[key]);
    g_ptr_array_add(ids, info);
  }
  return ids;
}

typedef struct {
  GhTestSigner mock;
  Recorder rec;
  GSettings *settings;
  GSettings *gnostr;
  gchar *state_dir;
  GhAccountController *accounts;
  GhAccountRelays *relays;
  GhConversationStore *store;
  GhDmInbox *inbox;
  GhWindow *window;
  GhOnboardingView *view;
} Fixture;

static gboolean
listed(gpointer data)
{
  return gh_account_controller_get_identities(data) != NULL;
}

static void
fixture_services_up(Fixture *f, const gchar *current)
{
  gh_test_signer_up(&bus, &f->mock);
  f->rec.scopes = g_ptr_array_new_with_free_func(scope_open_free);
  f->rec.pubs = g_ptr_array_new_with_free_func(pub_open_free);
  f->settings = g_settings_new("org.nostr.Groundhog");
  g_autoptr(GSettingsSchema) schema = NULL;
  g_object_get(f->settings, "settings-schema", &schema, NULL);
  g_auto(GStrv) keys = g_settings_schema_list_keys(schema);
  for (guint i = 0; keys[i]; i++)
    g_settings_reset(f->settings, keys[i]);
  g_settings_set_string(f->settings, "current-npub", current);
  /* Gnostr's own choice, which Groundhog must never move (UX-6). */
  f->gnostr = g_settings_new("org.gnostr.Client");
  g_settings_set_string(f->gnostr, "current-npub", npub[2]);
  g_autoptr(GError) error = NULL;
  f->state_dir = g_dir_make_tmp("groundhog-onboarding-XXXXXX", &error);
  g_assert_no_error(error);
  f->accounts = gh_account_controller_new_full(f->settings, bus.client, fake_list, NULL);
  gh_test_spin_until(listed, f->accounts);
  f->relays = gh_account_relays_new(f->accounts, f->settings, &scope_transport, &f->rec);
  f->store = gh_conversation_store_new();
  f->inbox = gh_dm_inbox_new(f->accounts, f->relays, f->store, f->state_dir, &scope_transport,
                             NULL, &f->rec);
}

static GhInboxSetupConfig
config_of(Fixture *f)
{
  return (GhInboxSetupConfig){
    .accounts = f->accounts,
    .account_relays = f->relays,
    .settings = f->settings,
    .probe_transport = &scope_transport,
    .probe_transport_data = &f->rec,
    .publish_transport = &pub_transport,
    .publish_transport_data = &f->rec,
  };
}

/* The executable's wiring (gh-app-services.c) on a fresh window. */
static void
window_up(Fixture *f)
{
  f->window = gh_window_new(NULL);
  gh_account_ui_attach(f->window, f->accounts, f->settings);
  gh_inbox_status_attach(gh_window_get_status(f->window), f->inbox, f->relays);
  GhInboxSetupConfig config = config_of(f);
  f->view = gh_onboarding_attach(f->window, &config);
}

static void
window_down(Fixture *f)
{
  if (f->window)
    gtk_window_destroy(GTK_WINDOW(g_steal_pointer(&f->window)));
  f->view = NULL;
}

static void
fixture_setup(Fixture *f, gconstpointer data)
{
  fixture_services_up(f, data ? data : "");
  window_up(f);
}

static void
remove_tree(const gchar *dir)
{
  g_autoptr(GDir) handle = g_dir_open(dir, 0, NULL);
  const gchar *name;
  while (handle && (name = g_dir_read_name(handle))) {
    g_autofree gchar *path = g_build_filename(dir, name, NULL);
    if (g_file_test(path, G_FILE_TEST_IS_DIR))
      remove_tree(path);
    else
      g_assert_cmpint(g_remove(path), ==, 0);
  }
  g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void
fixture_teardown(Fixture *f, gconstpointer data)
{
  (void)data;
  window_down(f);
  gh_test_release(g_steal_pointer(&f->inbox));
  g_clear_object(&f->store);
  gh_test_release(g_steal_pointer(&f->relays));
  gh_test_release(g_steal_pointer(&f->accounts));
  GhTestSenders check = { &bus, &f->mock };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
  for (guint i = 0; i < f->rec.scopes->len; i++)
    g_assert_true(((ScopeOpen *)g_ptr_array_index(f->rec.scopes, i))->closed);
  for (guint i = 0; i < f->rec.pubs->len; i++)
    g_assert_true(((PubOpen *)g_ptr_array_index(f->rec.pubs, i))->closed);
  g_ptr_array_unref(f->rec.scopes);
  g_ptr_array_unref(f->rec.pubs);
  g_settings_reset(f->gnostr, "current-npub");
  g_clear_object(&f->gnostr);
  g_clear_object(&f->settings);
  remove_tree(f->state_dir);
  g_free(f->state_dir);
  gh_test_signer_down(&bus, &f->mock);
}

/* ---- helpers ---------------------------------------------------------------- */

static gpointer
child(Fixture *f, const gchar *name)
{
  GObject *object = gtk_widget_get_template_child(GTK_WIDGET(f->view), GH_TYPE_ONBOARDING_VIEW,
                                                  name);
  g_assert_nonnull(object);
  return object;
}

static void
act(Fixture *f, const gchar *action)
{
  if (!gtk_widget_activate_action(GTK_WIDGET(f->view), action, NULL))
    g_error("%s is not available", action);
}

/* A button follows its action's enabled state. */
static gboolean
sensitive(Fixture *f, const gchar *button)
{
  return gtk_widget_get_sensitive(child(f, button));
}

static const gchar *
root_page(Fixture *f)
{
  return gtk_stack_get_visible_child_name(gh_window_get_root_stack(f->window));
}

/* As a click or Enter on the row does. */
static void
activate_row(Fixture *f, const gchar *list, guint position)
{
  GtkListBoxRow *row = gtk_list_box_get_row_at_index(child(f, list), (int)position);
  g_assert_nonnull(row);
  g_assert_true(gtk_list_box_row_get_activatable(row));
  g_signal_emit_by_name(row, "activate");
}

static guint
connections(Fixture *f)
{
  return f->rec.scopes->len + f->rec.pubs->len;
}

static GhOnboardingItem *
item_at(GListModel *model, guint position)
{
  g_autoptr(GhOnboardingItem) item = g_list_model_get_item(model, position);
  g_assert_nonnull(item);
  return item; /* borrowed: the model keeps it */
}

static guint
find_position(GListModel *model, const gchar *key)
{
  for (guint i = 0; i < g_list_model_get_n_items(model); i++)
    if (g_str_equal(gh_onboarding_item_get_key(item_at(model, i)), key))
      return i;
  g_error("%s is not listed", key);
}

static gboolean
is_active(gpointer data)
{
  return gh_account_controller_get_state(data) == GH_ACCOUNT_STATE_ACTIVE;
}

static gboolean
is_unselected(gpointer data)
{
  return gh_account_controller_get_state(data) == GH_ACCOUNT_STATE_UNSELECTED;
}

static gboolean
signer_answered(gpointer data)
{
  Fixture *f = data;
  return gtk_widget_get_visible(child(f, "signer_result")) &&
         !gtk_widget_get_visible(child(f, "signer_spinner"));
}

static gboolean
setup_finished(gpointer data)
{
  Fixture *f = data;
  GhInboxSetup *setup = gh_onboarding_view_get_setup(f->view);
  GhInboxSetupState state = setup ? gh_inbox_setup_get_state(setup) : GH_INBOX_SETUP_IDLE;
  return state == GH_INBOX_SETUP_DONE || state == GH_INBOX_SETUP_FAILED;
}

typedef struct {
  Fixture *f;
  guint count;
} OpenCount;

static guint
live_pubs(Fixture *f)
{
  guint n = 0;
  for (guint i = 0; i < f->rec.pubs->len; i++)
    n += !((PubOpen *)g_ptr_array_index(f->rec.pubs, i))->closed;
  return n;
}

static gboolean
pubs_live(gpointer data)
{
  OpenCount *want = data;
  return live_pubs(want->f) == want->count;
}

static ScopeOpen *
live_probe(Fixture *f, const gchar *url)
{
  for (guint i = f->rec.scopes->len; i > 0; i--) {
    ScopeOpen *open = g_ptr_array_index(f->rec.scopes, i - 1);
    if (open->probe && !open->closed && g_str_equal(open->url, url))
      return open;
  }
  g_error("no live check REQ on %s", url);
}

static const gchar *
banner_title(Fixture *f)
{
  AdwBanner *banner = gh_sidebar_page_get_banner(gh_window_get_sidebar(f->window));
  return adw_banner_get_revealed(banner) ? adw_banner_get_title(banner) : NULL;
}

/* Welcome -> account -> key 1 -> signer, as a user would. */
static void
choose_account(Fixture *f)
{
  g_assert_cmpstr(gh_onboarding_view_get_page(f->view), ==, "welcome");
  act(f, "onboarding.start");
  g_assert_cmpstr(gh_onboarding_view_get_page(f->view), ==, "account");
  GListModel *identities = gh_onboarding_view_get_identities(f->view);
  g_assert_cmpuint(g_list_model_get_n_items(identities), ==, 2);
  g_assert_false(sensitive(f, "account_continue"));
  activate_row(f, "identity_list", find_position(identities, npub[1]));
  gh_test_spin_until(is_active, f->accounts);
  GhOnboardingItem *chosen = item_at(identities, find_position(identities, npub[1]));
  g_assert_cmpstr(gh_onboarding_item_get_icon_name(chosen), ==, "object-select-symbolic");
  g_assert_cmpstr(gh_onboarding_item_get_title(chosen), ==, "Robin");
  act(f, "onboarding.account-continue");
  g_assert_cmpstr(gh_onboarding_view_get_page(f->view), ==, "signer");
}

/* The published list's answers: every live publication accepted, the
 * first message relay private and the others private or open. */
static void
answer_publish(Fixture *f, const gchar *const *chosen, gboolean others_open)
{
  GhInboxSetup *setup = gh_onboarding_view_get_setup(f->view);
  const gchar *id = gh_inbox_setup_get_event_id(setup);
  for (guint i = 0; i < f->rec.pubs->len; i++) {
    PubOpen *open = g_ptr_array_index(f->rec.pubs, i);
    if (!open->closed)
      gh_relay_publish_ok(open->publish, open->url, id, TRUE, "");
  }
  for (guint i = 0; chosen[i]; i++) {
    ScopeOpen *probe = live_probe(f, chosen[i]);
    if (i == 0 || !others_open)
      gh_relay_scope_notice(probe->scope, chosen[i], GH_RELAY_NOTICE_CLOSED, NULL, FALSE,
                            "auth-required: you must auth");
    else
      gh_relay_scope_eose(probe->scope, chosen[i]);
  }
  gh_test_spin_until(setup_finished, f);
}

/* ---- UX-6, PT-9: the whole flow ------------------------------------------ */

static void
test_first_run_publishes(Fixture *f, gconstpointer data)
{
  (void)data;
  /* First run: no account chosen, so the window opens on onboarding. */
  g_assert_cmpstr(root_page(f), ==, "onboarding");
  g_autoptr(GSettingsSchema) schema = NULL;
  g_object_get(f->settings, "settings-schema", &schema, NULL);
  g_auto(GStrv) keys = g_settings_schema_list_keys(schema);
  g_autoptr(GPtrArray) before = g_ptr_array_new_with_free_func((GDestroyNotify)g_variant_unref);
  for (guint i = 0; keys[i]; i++)
    g_ptr_array_add(before, g_settings_get_value(f->settings, keys[i]));

  choose_account(f);
  /* UX-6: only Groundhog's current-npub changed; Gnostr's did not. */
  for (guint i = 0; keys[i]; i++) {
    g_autoptr(GVariant) now = g_settings_get_value(f->settings, keys[i]);
    if (g_str_equal(keys[i], "current-npub"))
      g_assert_cmpstr(g_variant_get_string(now, NULL), ==, npub[1]);
    else
      g_assert_true(g_variant_equal(now, g_ptr_array_index(before, i)));
  }
  g_autofree gchar *gnostr = g_settings_get_string(f->gnostr, "current-npub");
  g_assert_cmpstr(gnostr, ==, npub[2]);

  /* The signer test: one signature and a NIP-44 round trip, no relay. */
  AdwActionRow *status = child(f, "signer_status");
  g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(status)), ==,
                  "Nostr Signer is running");
  act(f, "onboarding.test-signer");
  gh_test_spin_until(signer_answered, f);
  g_assert_true(g_str_has_prefix(gtk_label_get_text(child(f, "signer_result")), "It works"));
  g_assert_cmpuint(f->mock.calls, ==, 3);
  act(f, "onboarding.signer-continue");

  /* The inbox step: the reviewed suggestions, none ticked. */
  g_assert_cmpstr(gh_onboarding_view_get_page(f->view), ==, "inbox");
  GListModel *relays = gh_onboarding_view_get_relays(f->view);
  g_autoptr(GPtrArray) shipped = gh_inbox_setup_load_suggestions(NULL);
  g_assert_nonnull(shipped);
  g_assert_cmpuint(g_list_model_get_n_items(relays), ==, shipped->len);
  for (guint i = 0; i < shipped->len; i++)
    g_assert_false(gh_onboarding_item_get_checked(item_at(relays, i)));
  g_assert_false(sensitive(f, "inbox_continue"));
  g_assert_false(gtk_widget_get_visible(child(f, "keep_button")));
  /* P9 (W14 review non-blocking #2): what Check Privacy reveals is said in
   * visible text beside it, not only in a tooltip, and the footer does not
   * claim that nothing is contacted before the confirm page. */
  GtkLabel *check_note = child(f, "check_note");
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(check_note)));
  g_assert_true(gtk_widget_get_parent(GTK_WIDGET(check_note)) ==
                gtk_widget_get_parent(child(f, "check_button")));
  g_assert_nonnull(strstr(gtk_label_get_text(check_note), "connects to the ticked relays"));
  g_assert_nonnull(strstr(gtk_label_get_text(check_note), "IP address"));
  const gchar *footer = gtk_label_get_text(child(f, "inbox_footer"));
  g_assert_null(strstr(footer, "until you confirm"));
  g_assert_nonnull(strstr(footer, "until you check or publish"));

  /* A typed address is checked in place. */
  AdwEntryRow *entry = child(f, "custom_entry");
  gtk_editable_set_text(GTK_EDITABLE(entry), "ws://inbox.example.org");
  g_signal_emit_by_name(entry, "apply");
  g_assert_true(gtk_widget_get_visible(child(f, "custom_error")));
  g_assert_true(gtk_widget_has_css_class(GTK_WIDGET(entry), "error"));
  gtk_editable_set_text(GTK_EDITABLE(entry), "Inbox.Example.org/");
  g_assert_false(gtk_widget_get_visible(child(f, "custom_error")));
  g_signal_emit_by_name(entry, "apply");
  const gchar *custom = "wss://inbox.example.org";
  GhOnboardingItem *added = item_at(relays, find_position(relays, custom));
  g_assert_true(gh_onboarding_item_get_checked(added));
  const gchar *suggested = ((GhInboxSuggestion *)g_ptr_array_index(shipped, 0))->url;
  activate_row(f, "relay_list", find_position(relays, suggested));
  g_assert_true(gh_onboarding_item_get_checked(item_at(relays, 0)));
  g_assert_true(sensitive(f, "inbox_continue"));
  act(f, "onboarding.inbox-continue");

  /* Confirm lists exactly what publishing will contact. */
  g_assert_cmpstr(gh_onboarding_view_get_page(f->view), ==, "confirm");
  g_assert_true(gtk_widget_get_visible(child(f, "discovery_group")));
  const gchar *targets = adw_action_row_get_subtitle(child(f, "confirm_targets"));
  g_assert_nonnull(strstr(targets, suggested + strlen("wss://")));
  g_assert_nonnull(strstr(targets, "inbox.example.org"));

  /* PT-9: welcome, account, signer (with its test), inbox and confirm
   * contacted no relay. */
  g_assert_cmpuint(connections(f), ==, 0);

  act(f, "onboarding.publish");
  g_assert_cmpstr(gh_onboarding_view_get_page(f->view), ==, "publish");
  OpenCount two = { f, 2 };
  gh_test_spin_until(pubs_live, &two);
  /* Exactly the chosen relays (nothing else is configured yet). */
  g_assert_cmpuint(f->rec.pubs->len, ==, 2);
  const gchar *chosen[] = { suggested, custom, NULL };
  answer_publish(f, chosen, TRUE); /* the typed example.org relay serves reads */
  GhInboxSetup *setup = gh_onboarding_view_get_setup(f->view);
  g_assert_cmpint(gh_inbox_setup_get_state(setup), ==, GH_INBOX_SETUP_DONE);
  GListModel *results = gh_onboarding_view_get_results(f->view);
  g_assert_cmpuint(g_list_model_get_n_items(results), ==, 2);
  for (guint i = 0; i < 2; i++)
    g_assert_cmpstr(gh_onboarding_item_get_icon_name(item_at(results, i)), ==,
                    "emblem-ok-symbolic");
  g_assert_true(gtk_widget_get_visible(child(f, "publish_continue")));
  g_assert_false(gtk_widget_get_visible(child(f, "publish_retry")));
  /* The user agreed: the relays that kept the list now find it. */
  g_auto(GStrv) discovery = g_settings_get_strv(f->settings, "discovery-relays");
  g_assert_cmpuint(g_strv_length(discovery), ==, 2);

  act(f, "onboarding.publish-continue");
  g_assert_cmpstr(gh_onboarding_view_get_page(f->view), ==, "done");
  act(f, "onboarding.finish");
  g_assert_cmpstr(root_page(f), ==, "main");
}

/* ---- UX-6: skip paths ----------------------------------------------------- */

static void
test_read_only(Fixture *f, gconstpointer data)
{
  (void)data;
  act(f, "onboarding.start");
  act(f, "onboarding.read-only");
  g_assert_cmpstr(root_page(f), ==, "main");
  gh_test_spin_until(is_unselected, f->accounts);
  g_autofree gchar *current = g_settings_get_string(f->settings, "current-npub");
  g_assert_cmpstr(current, ==, "");
  /* The read-only main view: the "Choose an Account" page, no banner. */
  GtkStack *stack = gh_sidebar_page_get_stack(gh_window_get_sidebar(f->window));
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "account-unselected");
  g_assert_null(banner_title(f));
  g_assert_cmpuint(connections(f), ==, 0);
}

static gboolean
banner_is_no_relays(gpointer data)
{
  return gh_status_get_banner(gh_window_get_status(((Fixture *)data)->window)) ==
         GH_STATUS_BANNER_NO_RELAYS;
}

static void
assert_setup_banner(Fixture *f)
{
  gh_test_spin_until(banner_is_no_relays, f);
  AdwBanner *banner = gh_sidebar_page_get_banner(gh_window_get_sidebar(f->window));
  g_assert_cmpstr(banner_title(f), ==,
                  "No relay is set up yet, so Groundhog can't receive messages");
  g_assert_cmpstr(adw_banner_get_button_label(banner), ==, "Set Up");
  g_assert_cmpstr(gtk_actionable_get_action_name(GTK_ACTIONABLE(banner)), ==,
                  GH_STATUS_ACTION_SETUP_INBOX);
}

/* "Set Up Later" reaches the main view with the banner, whose [Set Up]
 * returns to the inbox step (and nothing was contacted). */
static void
test_set_up_later(Fixture *f, gconstpointer data)
{
  (void)data;
  choose_account(f);
  act(f, "onboarding.signer-continue");
  act(f, "onboarding.later");
  g_assert_cmpstr(root_page(f), ==, "main");
  assert_setup_banner(f);
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->window), GH_STATUS_ACTION_SETUP_INBOX,
                                           NULL));
  g_assert_cmpstr(root_page(f), ==, "onboarding");
  g_assert_cmpstr(gh_onboarding_view_get_page(f->view), ==, "inbox");
  act(f, "onboarding.later");
  g_assert_cmpstr(root_page(f), ==, "main");
  g_assert_cmpuint(connections(f), ==, 0);
}

/* A returning user (an account chosen before) starts in the main view;
 * the banner's [Set Up] opens only the inbox step. */
static void
test_returning_user(Fixture *f, gconstpointer data)
{
  (void)data;
  g_assert_cmpstr(root_page(f), ==, "main");
  gh_test_spin_until(is_active, f->accounts);
  assert_setup_banner(f);
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->window), GH_STATUS_ACTION_SETUP_INBOX,
                                           NULL));
  g_assert_cmpstr(gh_onboarding_view_get_page(f->view), ==, "inbox");
  AdwNavigationView *navigation = child(f, "navigation");
  g_assert_null(adw_navigation_view_get_previous_page(
    navigation, adw_navigation_view_get_visible_page(navigation)));
  g_assert_cmpuint(connections(f), ==, 0);
}

/* A declined signature: the signer test says so, and Publish sends
 * nothing; "Set Up Later" then leaves the banner. */
static void
test_signer_denied(Fixture *f, gconstpointer data)
{
  (void)data;
  choose_account(f);
  f->mock.deny = TRUE;
  act(f, "onboarding.test-signer");
  gh_test_spin_until(signer_answered, f);
  g_assert_true(g_str_has_prefix(gtk_label_get_text(child(f, "signer_result")),
                                 "You declined the request"));
  act(f, "onboarding.signer-continue");
  activate_row(f, "relay_list", 0);
  act(f, "onboarding.inbox-continue");
  act(f, "onboarding.publish");
  gh_test_spin_until(setup_finished, f);
  g_assert_cmpstr(gtk_label_get_text(child(f, "publish_title")), ==, "Nothing Was Published");
  g_assert_true(gtk_widget_get_visible(child(f, "publish_retry")));
  g_assert_true(gtk_widget_get_visible(child(f, "publish_later")));
  g_assert_false(gtk_widget_get_visible(child(f, "publish_continue")));
  g_assert_cmpuint(connections(f), ==, 0);
  /* Try Again returns to the confirm page with a fresh plan. */
  act(f, "onboarding.retry");
  g_assert_cmpstr(gh_onboarding_view_get_page(f->view), ==, "confirm");
  act(f, "onboarding.publish");
  gh_test_spin_until(setup_finished, f);
  act(f, "onboarding.later");
  g_assert_cmpstr(root_page(f), ==, "main");
  assert_setup_banner(f);
  g_assert_cmpuint(connections(f), ==, 0);
}

/* ---- UX-1: every page at 360x294 ------------------------------------------ */

static gboolean
is_mapped(gpointer data)
{
  return gtk_widget_get_mapped(data) && gtk_widget_get_width(data) > 0;
}

static void
drain_idle(void)
{
  for (int i = 0; i < 100 && g_main_context_iteration(NULL, FALSE); i++)
    ;
}

static void
test_minimum_size(Fixture *f, gconstpointer data)
{
  (void)data;
  gtk_window_set_default_size(GTK_WINDOW(f->window), 360, 294);
  gtk_window_present(GTK_WINDOW(f->window));
  gh_test_spin_until(is_mapped, f->window);
  AdwNavigationView *navigation = child(f, "navigation");
  static const gchar *const pages[] = {
    "welcome", "account", "signer", "inbox", "confirm", "publish", "done",
  };
  for (guint i = 0; i < G_N_ELEMENTS(pages); i++) {
    const gchar *tags[] = { pages[i] };
    adw_navigation_view_replace_with_tags(navigation, tags, 1);
    drain_idle();
    int min_width = 0, min_height = 0;
    gtk_widget_measure(GTK_WIDGET(f->window), GTK_ORIENTATION_HORIZONTAL, -1, &min_width, NULL,
                       NULL, NULL);
    gtk_widget_measure(GTK_WIDGET(f->window), GTK_ORIENTATION_VERTICAL, 360, &min_height, NULL,
                       NULL, NULL);
    if (min_width > 360 || min_height > 294)
      g_error("onboarding page %s needs %dx%d, more than 360x294", pages[i], min_width,
              min_height);
    g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(f->view)), <=, 360);
  }
}

/* ---- screenshots (opt-in evidence) ----------------------------------------- */

static void
save_png(GtkWidget *window, const gchar *dir, const gchar *name)
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
  g_autofree gchar *path = g_strdup_printf("%s/groundhog-g14-%s.png", dir, name);
  g_assert_true(gdk_texture_save_to_png(texture, path));
  g_test_message("saved %s (window %dx%d)", path, gtk_widget_get_width(window),
                 gtk_widget_get_height(window));
}

static void
shoot(Fixture *f, const gchar *dir, guint step, const gchar *page, const gchar *variant)
{
  g_assert_cmpstr(gh_onboarding_view_get_page(f->view), ==, page);
  drain_idle();
  gtk_test_widget_wait_for_draw(GTK_WIDGET(f->window));
  g_autofree gchar *name = g_strdup_printf("%u-%s-%s", step, page, variant);
  save_png(GTK_WIDGET(f->window), dir, name);
}

/* One fresh window per size and scheme (a mapped window keeps its size),
 * driven through every step as a user would. */
static void
shoot_flow(Fixture *f, const gchar *dir, int width, int height, const gchar *variant)
{
  g_assert_true(gh_account_controller_select(f->accounts, "", NULL));
  gh_test_spin_until(is_unselected, f->accounts);
  g_settings_reset(f->settings, "discovery-relays");
  window_up(f);
  gtk_window_set_default_size(GTK_WINDOW(f->window), width, height);
  gtk_window_present(GTK_WINDOW(f->window));
  gh_test_spin_until(is_mapped, f->window);

  shoot(f, dir, 1, "welcome", variant);
  act(f, "onboarding.start");
  shoot(f, dir, 2, "account", variant);
  activate_row(f, "identity_list", 0);
  gh_test_spin_until(is_active, f->accounts);
  act(f, "onboarding.account-continue");
  act(f, "onboarding.test-signer");
  gh_test_spin_until(signer_answered, f);
  shoot(f, dir, 3, "signer", variant);
  act(f, "onboarding.signer-continue");
  /* Two suggestions ticked and checked (the user pressed Check Privacy). */
  GListModel *relays = gh_onboarding_view_get_relays(f->view);
  activate_row(f, "relay_list", 0);
  activate_row(f, "relay_list", 1);
  act(f, "onboarding.check-relays");
  const gchar *chosen[] = { gh_onboarding_item_get_key(item_at(relays, 0)),
                            gh_onboarding_item_get_key(item_at(relays, 1)), NULL };
  ScopeOpen *first = live_probe(f, chosen[0]);
  gh_relay_scope_notice(first->scope, chosen[0], GH_RELAY_NOTICE_CLOSED, NULL, FALSE,
                        "auth-required: you must auth");
  gh_relay_scope_notice(first->scope, chosen[1], GH_RELAY_NOTICE_CLOSED, NULL, FALSE,
                        "auth-required: members only");
  shoot(f, dir, 4, "inbox", variant);
  act(f, "onboarding.inbox-continue");
  shoot(f, dir, 5, "confirm", variant);
  act(f, "onboarding.publish");
  OpenCount two = { f, 2 };
  gh_test_spin_until(pubs_live, &two);
  /* The reviewed suggestions refuse unauthenticated reads (as checked on
   * the review date), so the evidence shows them that way. */
  answer_publish(f, chosen, FALSE);
  shoot(f, dir, 6, "publish", variant);
  act(f, "onboarding.publish-continue");
  shoot(f, dir, 7, "done", variant);
  act(f, "onboarding.finish");
  window_down(f);
}

static void
test_screenshots(Fixture *f, gconstpointer data)
{
  (void)data;
  const gchar *dir = g_getenv("GROUNDHOG_TEST_SCREENSHOTS");
  if (!dir || !*dir) {
    g_test_skip("GROUNDHOG_TEST_SCREENSHOTS is not set");
    return;
  }
  window_down(f);
  /* Switching the color scheme re-parses the theme; some GTK builds warn
   * about libadwaita's own CSS then, so only criticals stay fatal here. */
  GLogLevelFlags fatal = g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
  AdwStyleManager *style = adw_style_manager_get_default();
  static const struct {
    AdwColorScheme scheme;
    const gchar *name;
  } schemes[] = {
    { ADW_COLOR_SCHEME_FORCE_LIGHT, "light" },
    { ADW_COLOR_SCHEME_FORCE_DARK, "dark" },
  };
  for (guint i = 0; i < G_N_ELEMENTS(schemes); i++) {
    adw_style_manager_set_color_scheme(style, schemes[i].scheme);
    g_autofree gchar *wide = g_strdup_printf("wide-%s", schemes[i].name);
    g_autofree gchar *narrow = g_strdup_printf("360x294-%s", schemes[i].name);
    shoot_flow(f, dir, 900, 640, wide);
    shoot_flow(f, dir, 360, 294, narrow);
  }
  adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_DEFAULT);
  g_log_set_always_fatal(fatal);
}

int
main(int argc, char **argv)
{
  /* The executable's "online" check must not depend on the host network. */
  g_setenv("GIO_USE_NETWORK_MONITOR", "base", TRUE);
  if (!nostrc_test_bus_available()) {
    g_printerr("groundhog-onboarding test skipped: dbus-daemon is not installed\n");
    return 77;
  }
  /* GTK announces through AT-SPI (its in-process test backend cannot
   * announce on GTK 4.14), so the private bus can start the accessibility
   * bus, and nothing else. */
  g_autofree gchar *a11y_service = NULL;
  for (const gchar *const *dir = g_get_system_data_dirs(); *dir && !a11y_service; dir++) {
    gchar *path = g_build_filename(*dir, "dbus-1", "services", "org.a11y.Bus.service", NULL);
    if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
      a11y_service = path;
    else
      g_free(path);
  }
  if (!a11y_service) {
    g_printerr("groundhog-onboarding test skipped: the AT-SPI bus service (at-spi2-core) is "
               "not installed\n");
    return 77;
  }
  g_autoptr(GError) error = NULL;
  g_autofree gchar *service_text = NULL;
  a11y_services = g_dir_make_tmp("groundhog-onboarding-a11y-XXXXXX", &error);
  g_assert_no_error(error);
  g_autofree gchar *copy = g_build_filename(a11y_services, "org.a11y.Bus.service", NULL);
  g_assert_true(g_file_get_contents(a11y_service, &service_text, NULL, NULL));
  g_assert_true(g_file_set_contents(copy, service_text, -1, NULL));
  bus.bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  nostrc_test_bus_add_service_dir(bus.bus, a11y_services);
  nostrc_test_bus_up(bus.bus);
  bus.client = nostrc_test_bus_connect(bus.bus);
  bus.owner = nostrc_test_bus_connect(bus.bus);
  if (!gtk_init_check()) {
    g_printerr("groundhog-onboarding test skipped: no graphical display\n");
    return 77;
  }
  adw_init();
  groundhog_register_resource();
  /* The app icon is installed into hicolor; from the build tree it is only in
   * the resource. */
  gtk_icon_theme_add_resource_path(gtk_icon_theme_get_for_display(gdk_display_get_default()),
                                   "/org/nostr/Groundhog/icons");
  g_autoptr(GtkCssProvider) css = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(css, "/org/nostr/Groundhog/style.css");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  /* As in the shell tests: 1sp = 1px, no animations, GNOME's close-only
   * window controls (the desktop the 360x294 minimum is for). */
  g_object_set(gtk_settings_get_default(), "gtk-xft-dpi", 96 * 1024,
               "gtk-enable-animations", FALSE, "gtk-decoration-layout", "appmenu:close", NULL);
  for (guint key = 1; key < GH_TEST_KEYS; key++) {
    npub[key] = gh_test_npub(key);
    hex[key] = gh_test_pub(key);
  }
  g_test_init(&argc, &argv, NULL);
#define ADD(path, func, current) \
  g_test_add("/groundhog/onboarding/" path, Fixture, current, fixture_setup, func, \
             fixture_teardown)
  ADD("first-run-publishes", test_first_run_publishes, NULL);
  ADD("read-only", test_read_only, NULL);
  ADD("set-up-later", test_set_up_later, NULL);
  ADD("signer-denied", test_signer_denied, NULL);
  ADD("minimum-size", test_minimum_size, NULL);
  ADD("returning-user", test_returning_user, npub[1]);
  ADD("screenshots", test_screenshots, NULL);
#undef ADD
  int status = g_test_run();
  for (guint key = 1; key < GH_TEST_KEYS; key++) {
    g_free(npub[key]);
    g_free(hex[key]);
  }
  /* GTK and libadwaita keep their session connection (the settings portal
   * lookup) for the life of the process, so the bus is not brought down:
   * nostrc-test-bus's lifeline supervisor stops it when this process exits,
   * however it exits. */
  g_autofree gchar *copied = g_build_filename(a11y_services, "org.a11y.Bus.service", NULL);
  g_remove(copied);
  g_rmdir(a11y_services);
  g_free(a11y_services);
  return status;
}
