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

#include "../gh-test-gdk-frame.h"

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
  gboolean check;   /* a relay list's pre-publish check: {kinds:[10002]} */
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
  open->check = nostr_filter_kinds_len(filter) == 1 &&
                nostr_filter_kinds_get(filter, 0) == 10002;
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
    .offer_relay_list = TRUE,   /* as with GH_FEATURE_ENCRYPTED_GROUPS (nostrc-0bdg) */
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

/* A GhWindowNewMessageFunc that counts how often New Message opened. */
static void
count_new_message(GhWindow *window, gpointer data)
{
  (void)window;
  (*(guint *)data)++;
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

/* Build the full target list: the chosen URLs + non-duplicate relay
 * suggestions (adopt with empty discovery adds them as targets). */
static gchar **
all_targets(const gchar *const *chosen)
{
  g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; chosen[i]; i++)
    g_ptr_array_add(urls, g_strdup(chosen[i]));
  g_autoptr(GPtrArray) s = gh_inbox_setup_load_suggestions(NULL);
  for (guint i = 0; s && i < s->len; i++) {
    GhInboxSuggestion *sg = g_ptr_array_index(s, i);
    gboolean found = FALSE;
    for (guint j = 0; !found && chosen[j]; j++)
      found = g_str_equal(chosen[j], sg->url);
    if (!found)
      g_ptr_array_add(urls, g_strdup(sg->url));
  }
  g_ptr_array_add(urls, NULL);
  return (gchar **)g_ptr_array_steal(urls, NULL);
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

static gboolean
signer_probed(gpointer data)
{
  return gh_account_controller_get_signer_availability(data) != GH_SIGNER_AVAILABILITY_UNKNOWN;
}

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

  /* The signer test: one signature and a NIP-44 round trip, no relay. The
   * status comes from an asynchronous bus probe (NameHasOwner) that nothing
   * above waited for (nostrc-qp24.85, nostrc-qp24.92): wait for its answer,
   * then the row must say it. */
  gh_test_spin_until(signer_probed, f->accounts);
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
  /* In Tor mode the relays don't see the IP address (W16 review #5). */
  g_settings_set_string(f->settings, "network-mode", "tor");
  g_assert_nonnull(strstr(gtk_label_get_text(check_note), "through Tor"));
  g_assert_nonnull(strstr(gtk_label_get_text(check_note), "neither your IP address"));
  g_settings_reset(f->settings, "network-mode");
  g_assert_null(strstr(gtk_label_get_text(check_note), "through Tor"));
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
  /* No finished discovery, so no knowing whether a relay list exists: none
   * is offered (nostrc-0bdg). */
  g_assert_false(gtk_widget_get_visible(child(f, "relay_list_group")));
  const gchar *targets = adw_action_row_get_subtitle(child(f, "confirm_targets"));
  g_assert_nonnull(strstr(targets, suggested + strlen("wss://")));
  g_assert_nonnull(strstr(targets, "inbox.example.org"));

  /* PT-9: welcome, account, signer (with its test), inbox and confirm
   * contacted no relay. */
  g_assert_cmpuint(connections(f), ==, 0);

  act(f, "onboarding.publish");
  g_assert_cmpstr(gh_onboarding_view_get_page(f->view), ==, "publish");
  const gchar *chosen[] = { suggested, custom, NULL };
  /* Chosen relays + non-duplicate suggestions (adopt with empty discovery). */
  g_auto(GStrv) pub_targets = all_targets(chosen);
  guint n_pubs = g_strv_length(pub_targets);
  OpenCount pub_count = { f, n_pubs };
  gh_test_spin_until(pubs_live, &pub_count);
  g_assert_cmpuint(f->rec.pubs->len, ==, n_pubs);
  answer_publish(f, chosen, TRUE); /* the typed example.org relay serves reads */
  GhInboxSetup *setup = gh_onboarding_view_get_setup(f->view);
  g_assert_cmpint(gh_inbox_setup_get_state(setup), ==, GH_INBOX_SETUP_DONE);
  GListModel *results = gh_onboarding_view_get_results(f->view);
  g_assert_cmpuint(g_list_model_get_n_items(results), ==, n_pubs);
  for (guint i = 0; i < n_pubs; i++)
    g_assert_cmpstr(gh_onboarding_item_get_icon_name(item_at(results, i)), ==,
                    "emblem-ok-symbolic");
  g_assert_true(gtk_widget_get_visible(child(f, "publish_continue")));
  g_assert_false(gtk_widget_get_visible(child(f, "publish_retry")));
  /* The user agreed: the relays that kept the list now find it.
   * Discovery saves only the user's chosen relays, not the suggestion
   * relays (which were publish targets for discoverability only). */
  g_auto(GStrv) discovery = g_settings_get_strv(f->settings, "discovery-relays");
  g_assert_cmpuint(g_strv_length(discovery), ==, 2);

  /* Charter §7.8 step 6 (nostrc-qp24.80): [Start a Conversation] finishes
   * and opens New Message, and is offered only while the window can. */
  guint new_message = 0;
  gh_window_set_new_message_handler(f->window, count_new_message, &new_message, NULL);
  act(f, "onboarding.publish-continue");
  g_assert_cmpstr(gh_onboarding_view_get_page(f->view), ==, "done");
  GtkWidget *start = child(f, "start_conversation_button");
  g_assert_true(gtk_widget_get_visible(start));
  g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(start)), ==, "_Start a Conversation");
  g_assert_cmpstr(gtk_actionable_get_action_name(GTK_ACTIONABLE(start)), ==,
                  "onboarding.start-conversation");
  g_assert_true(gtk_widget_has_css_class(start, "suggested-action"));
  g_assert_cmpstr(gtk_actionable_get_action_name(GTK_ACTIONABLE(child(f, "done_button"))), ==,
                  "onboarding.finish");
  gh_window_set_new_message_enabled(f->window, FALSE);
  g_assert_false(gtk_widget_get_visible(start));
  gh_window_set_new_message_enabled(f->window, TRUE);
  g_assert_true(gtk_widget_get_visible(start));
  act(f, "onboarding.start-conversation");
  g_assert_cmpstr(root_page(f), ==, "main");
  g_assert_cmpuint(new_message, ==, 1);
  gh_window_set_new_message_handler(f->window, NULL, NULL, NULL);
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

static ScopeOpen *
discovery_scope(Fixture *f, const gchar *url)
{
  for (guint i = 0; i < f->rec.scopes->len; i++) {
    ScopeOpen *open = g_ptr_array_index(f->rec.scopes, i);
    if (!open->probe && !open->check && !open->closed && g_str_equal(open->url, url))
      return open;
  }
  return NULL;
}

static ScopeOpen *
check_scope(Fixture *f, const gchar *url)
{
  for (guint i = 0; i < f->rec.scopes->len; i++) {
    ScopeOpen *open = g_ptr_array_index(f->rec.scopes, i);
    if (open->check && !open->closed && g_str_equal(open->url, url))
      return open;
  }
  return NULL;
}

typedef struct {
  Fixture *f;
  const gchar *const *urls;
} ChecksWait;

static gboolean
checks_open(gpointer data)
{
  ChecksWait *wait = data;
  for (guint i = 0; wait->urls[i]; i++)
    if (!check_scope(wait->f, wait->urls[i]))
      return FALSE;
  return TRUE;
}

/* Every target of the relay list's pre-publish check answers "none". */
static void
answer_checks(Fixture *f, const gchar *const *urls)
{
  ChecksWait wait = { f, urls };
  gh_test_spin_until(checks_open, &wait);
  for (guint i = 0; urls[i]; i++)
    gh_relay_scope_eose(check_scope(f, urls[i])->scope, urls[i]);
}

typedef struct {
  Fixture *f;
  const gchar *url;
} DiscoveryWait;

static gboolean
discovery_asked(gpointer data)
{
  DiscoveryWait *wait = data;
  return discovery_scope(wait->f, wait->url) != NULL;
}

static gboolean
discovery_complete(gpointer data)
{
  return gh_account_relays_get_state(data) == GH_ACCOUNT_RELAYS_COMPLETE;
}

/* nostrc-0bdg review H2: an account whose discovery finished without any
 * kind 10002 is offered one on the confirm page (consent, PD-13); Publish
 * then signs and publishes it too, to the same relays, and the result page
 * says that people can now invite it to encrypted groups. */
static void
test_relay_list_offered(Fixture *f, gconstpointer data)
{
  (void)data;
  gh_test_spin_until(is_active, f->accounts);
  const gchar *find = "wss://find.example.org";
  const gchar *const discovery[] = { find, NULL };
  g_settings_set_strv(f->settings, "discovery-relays", discovery);
  DiscoveryWait asked = { f, find };
  gh_test_spin_until(discovery_asked, &asked);
  /* The discovery relay answers: no 10002, no 10050. */
  gh_relay_scope_eose(discovery_scope(f, find)->scope, find);
  gh_test_spin_until(discovery_complete, f->relays);
  g_assert_false(gh_account_relays_has_relay_list(f->relays));

  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->window), GH_STATUS_ACTION_SETUP_INBOX,
                                           NULL));
  g_assert_cmpstr(gh_onboarding_view_get_page(f->view), ==, "inbox");
  activate_row(f, "relay_list", 0);
  act(f, "onboarding.inbox-continue");
  g_assert_cmpstr(gh_onboarding_view_get_page(f->view), ==, "confirm");
  g_assert_true(gtk_widget_get_visible(child(f, "relay_list_group")));
  g_assert_true(adw_switch_row_get_active(child(f, "relay_list_switch")));

  act(f, "onboarding.publish");
  /* Before the relay list is signed, each target is asked whether it holds
   * one of the account's (re-review R1); both say no. */
  const gchar *chosen_url =
    gh_inbox_setup_get_relay(gh_onboarding_view_get_setup(f->view), 0)->url;
  const gchar *const both[] = { chosen_url, find, NULL };
  answer_checks(f, both);
  /* ...and again once the signer answered, right before publishing (final
   * review F1). */
  answer_checks(f, both);
  /* The message list and the relay list, each to the chosen relay and the
   * discovery relay. */
  OpenCount four = { f, 4 };
  gh_test_spin_until(pubs_live, &four);
  g_assert_cmpuint(f->rec.pubs->len, ==, 4);
  GhInboxSetup *setup = gh_onboarding_view_get_setup(f->view);
  guint relay_lists = 0;
  for (guint i = 0; i < f->rec.pubs->len; i++) {
    PubOpen *open = g_ptr_array_index(f->rec.pubs, i);
    const gchar *id = gh_relay_publish_get_event_id(open->publish);
    relay_lists += g_strcmp0(id, gh_inbox_setup_get_event_id(setup)) != 0;
    gh_relay_publish_ok(open->publish, open->url, id, TRUE, "");
  }
  g_assert_cmpuint(relay_lists, ==, 2);
  for (guint i = 0; i < gh_inbox_setup_get_n_relays(setup); i++) {
    const GhInboxSetupRelay *relay = gh_inbox_setup_get_relay(setup, i);
    if (relay->probed)
      gh_relay_scope_eose(live_probe(f, relay->url)->scope, relay->url);
  }
  gh_test_spin_until(setup_finished, f);
  g_assert_cmpint(gh_inbox_setup_get_state(setup), ==, GH_INBOX_SETUP_DONE);
  g_assert_cmpint(gh_inbox_setup_get_relay_list_state(setup), ==,
                  GH_INBOX_SETUP_RELAY_LIST_DONE);
  g_assert_nonnull(strstr(gtk_label_get_text(child(f, "publish_description")),
                          "People can now invite you to encrypted groups."));
}

static gboolean
has_list(gpointer data)
{
  return gh_account_relays_has_relay_list(data);
}

/* Final review F3: an account whose relay list names no relay it publishes
 * to is offered to add these relays to it -- an edit of the user's own
 * list, so opt-in: the switch starts off, and Publish with it off sends no
 * relay list and asks nothing about one. */
static void
test_relay_list_edit_opt_in(Fixture *f, gconstpointer data)
{
  (void)data;
  gh_test_spin_until(is_active, f->accounts);
  const gchar *find = "wss://find.example.org";
  const gchar *const discovery[] = { find, NULL };
  g_settings_set_strv(f->settings, "discovery-relays", discovery);
  DiscoveryWait asked = { f, find };
  gh_test_spin_until(discovery_asked, &asked);
  NostrEvent *list = nostr_event_new();
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("r", "wss://read-only.example.org", "read", NULL));
  nostr_event_set_kind(list, 10002);
  nostr_event_set_created_at(list, g_get_real_time() / G_USEC_PER_SEC - 3600);
  nostr_event_set_content(list, "");
  nostr_event_set_tags(list, tags);
  g_assert_cmpint(nostr_event_sign(list, gh_test_secret[1]), ==, 0);
  char *json = nostr_event_serialize_compact(list);
  nostr_event_free(list);
  ScopeOpen *scope = discovery_scope(f, find);
  gh_relay_scope_event(scope->scope, find, json);
  free(json);
  gh_relay_scope_eose(scope->scope, find);
  gh_test_spin_until(has_list, f->relays);
  gh_test_spin_until(discovery_complete, f->relays);

  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->window), GH_STATUS_ACTION_SETUP_INBOX,
                                           NULL));
  activate_row(f, "relay_list", 0);
  act(f, "onboarding.inbox-continue");
  g_assert_cmpstr(gh_onboarding_view_get_page(f->view), ==, "confirm");
  AdwSwitchRow *edit = child(f, "relay_list_switch");
  g_assert_true(gtk_widget_get_visible(child(f, "relay_list_group")));
  g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(edit)), ==,
                  "Add These Relays to Your Relay List");
  g_assert_false(adw_switch_row_get_active(edit));
  guint scopes = f->rec.scopes->len;
  act(f, "onboarding.publish");
  /* discovery-relays is non-empty → suggestions are not added as targets. */
  OpenCount two = { f, 2 };
  gh_test_spin_until(pubs_live, &two);
  GhInboxSetup *setup = gh_onboarding_view_get_setup(f->view);
  g_assert_cmpint(gh_inbox_setup_get_relay_list_state(setup), ==,
                  GH_INBOX_SETUP_RELAY_LIST_NONE);
  for (guint i = scopes; i < f->rec.scopes->len; i++)
    g_assert_false(((ScopeOpen *)g_ptr_array_index(f->rec.scopes, i))->check);
}

static gboolean
later_offered(gpointer data)
{
  Fixture *f = data;
  return gtk_widget_get_visible(child(f, "relay_list_later_button"));
}

static gboolean
later_done(gpointer data)
{
  Fixture *f = data;
  const gchar *text = adw_action_row_get_subtitle(child(f, "relay_list_later_row"));
  return text && strstr(text, "People can now invite you to encrypted groups.");
}

/* nostrc-0bdg re-review R2: a real first run (discovery-relays empty, the
 * default) is offered the relay list on the result page, once Groundhog
 * looked on the relays it just adopted to find settings and found none;
 * accepting publishes it (after the check of every relay) and says the
 * account can now be invited. */
static void
test_first_run_offers_relay_list(Fixture *f, gconstpointer data)
{
  (void)data;
  g_auto(GStrv) none = g_settings_get_strv(f->settings, "discovery-relays");
  g_assert_null(none[0]);
  choose_account(f);
  act(f, "onboarding.signer-continue");
  activate_row(f, "relay_list", 0);
  act(f, "onboarding.inbox-continue");
  g_assert_false(gtk_widget_get_visible(child(f, "relay_list_group")));   /* not known yet */
  g_assert_true(adw_switch_row_get_active(child(f, "discovery_switch")));
  act(f, "onboarding.publish");
  GhInboxSetup *setup = gh_onboarding_view_get_setup(f->view);
  const gchar *chosen = gh_inbox_setup_get_relay(setup, 0)->url;
  const gchar *const chosen_list[] = { chosen, NULL };
  g_auto(GStrv) inbox_targets = all_targets(chosen_list);
  guint n_inbox_pubs = g_strv_length(inbox_targets);
  OpenCount inbox_count = { f, n_inbox_pubs };
  gh_test_spin_until(pubs_live, &inbox_count);
  answer_publish(f, chosen_list, FALSE);
  g_assert_cmpint(gh_inbox_setup_get_state(setup), ==, GH_INBOX_SETUP_DONE);
  g_assert_true(gh_inbox_setup_get_adopted_discovery(setup));
  g_assert_false(later_offered(f));

  /* Groundhog now looks on the adopted relay: no relay list there. */
  DiscoveryWait asked = { f, chosen };
  gh_test_spin_until(discovery_asked, &asked);
  gh_relay_scope_eose(discovery_scope(f, chosen)->scope, chosen);
  gh_test_spin_until(later_offered, f);
  g_assert_cmpstr(gh_onboarding_view_get_page(f->view), ==, "publish");

  act(f, "onboarding.publish-relay-list");
  answer_checks(f, (const gchar *const *)inbox_targets);
  answer_checks(f, (const gchar *const *)inbox_targets);   /* after the signer (final review F1) */
  OpenCount rl_pubs = { f, n_inbox_pubs };
  gh_test_spin_until(pubs_live, &rl_pubs);
  for (guint i = 0; i < f->rec.pubs->len; i++) {
    PubOpen *open = g_ptr_array_index(f->rec.pubs, i);
    if (!open->closed)
      gh_relay_publish_ok(open->publish, open->url, gh_relay_publish_get_event_id(open->publish),
                          TRUE, "");
  }
  gh_test_spin_until(later_done, f);
  g_assert_false(later_offered(f));
}

/* nostrc-a4po: while the signer request is pending (SIGNING), a Cancel button
 * is offered; pressing it cancels the setup (FAILED), and the result says
 * nothing was published, with Try Again. */
static gboolean
signing(gpointer data)
{
  Fixture *f = data;
  GhInboxSetup *setup = gh_onboarding_view_get_setup(f->view);
  return setup && gh_inbox_setup_get_state(setup) == GH_INBOX_SETUP_SIGNING;
}

static void
test_cancel_while_signing(Fixture *f, gconstpointer data)
{
  (void)data;
  choose_account(f);
  act(f, "onboarding.signer-continue");
  activate_row(f, "relay_list", 0);
  act(f, "onboarding.inbox-continue");

  /* Hold the signer call so we stay in SIGNING. */
  f->mock.hold = TRUE;
  act(f, "onboarding.publish");
  gh_test_spin_until(signing, f);

  /* The cancel button must be visible during SIGNING (nostrc-a4po). */
  g_assert_true(gtk_widget_get_visible(child(f, "publish_cancel")));
  g_assert_false(gtk_widget_get_visible(child(f, "publish_continue")));
  g_assert_false(gtk_widget_get_visible(child(f, "publish_retry")));

  /* Pressing Cancel finishes with FAILED and the right result. */
  act(f, "onboarding.cancel-publish");
  gh_test_spin_until(setup_finished, f);
  GhInboxSetup *setup = gh_onboarding_view_get_setup(f->view);
  g_assert_cmpint(gh_inbox_setup_get_state(setup), ==, GH_INBOX_SETUP_FAILED);
  g_assert_cmpstr(gtk_label_get_text(child(f, "publish_title")), ==,
                  "Publishing Stopped");
  g_assert_false(gtk_widget_get_visible(child(f, "publish_cancel")));
  g_assert_true(gtk_widget_get_visible(child(f, "publish_retry")));
  g_assert_true(gtk_widget_get_visible(child(f, "publish_later")));
  g_assert_false(gtk_widget_get_visible(child(f, "publish_continue")));

  /* No relay was contacted: the cancel prevented it. */
  g_assert_cmpuint(f->rec.pubs->len, ==, 0);

  /* Release the held signer call so the fixture cleanup doesn't wait. */
  f->mock.hold = FALSE;
  gh_test_signer_release_all(&f->mock);
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
  g_auto(GStrv) shoot_targets = all_targets(chosen);
  OpenCount shoot_pubs = { f, g_strv_length(shoot_targets) };
  gh_test_spin_until(pubs_live, &shoot_pubs);
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
  gh_test_tolerate_gdk_frame_warning();
#define ADD(path, func, current) \
  g_test_add("/groundhog/onboarding/" path, Fixture, current, fixture_setup, func, \
             fixture_teardown)
  ADD("first-run-publishes", test_first_run_publishes, NULL);
  ADD("read-only", test_read_only, NULL);
  ADD("set-up-later", test_set_up_later, NULL);
  ADD("cancel-while-signing", test_cancel_while_signing, NULL);
  ADD("signer-denied", test_signer_denied, NULL);
  ADD("minimum-size", test_minimum_size, NULL);
  ADD("returning-user", test_returning_user, npub[1]);
  ADD("relay-list-offered", test_relay_list_offered, npub[1]);
  ADD("first-run-offers-relay-list", test_first_run_offers_relay_list, NULL);
  ADD("relay-list-edit-opt-in", test_relay_list_edit_opt_in, npub[1]);
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
