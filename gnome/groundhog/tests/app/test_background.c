/* Background delivery (privacy charter §5.3, §8.2 G15; tests NO-9…NO-12 of
 * §9.2): GhBackground over a real account controller on the shared private
 * bus (tests/common/nostrc-test-bus.h) with the mock signer, the FakeSecret
 * key backend (H5), recording relay transports (H1), the real NIP-17 inbox
 * and SQLCipher store, a fake org.freedesktop.portal.Background, private XDG
 * directories, and the real groundhog executable for the process-level
 * checks. The in-process service runs build the stack in the order and
 * teardown of gh-app-services.c (with injected transports); the process
 * tests cover the executable's own wiring. Nothing sleeps: every wait is on
 * an observable condition with a failure deadline.
 *
 * NO-11's "one hidden notification" belongs to G16 (private notifications);
 * here a locked store gives no inbox REQ, no prompt and a portal status that
 * says so. */
#include "gh-account-store.h"
#include "gh-background.h"
#include "gh-test-signer.h"
#include "gh-window.h"
#include "fake-secret.h"

#include "nostr-tag.h"
#include "nostr/nip59/nip59.h"

#include <adwaita.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <sys/stat.h>

#define DISCOVERY "wss://discovery.test.invalid"
#define INBOX_A "wss://inbox-a.test.invalid"
#define CANARY "GROUNDHOG-CANARY-background"
#define RUN "run-in-background"

void groundhog_register_resource(void);

static gchar *npub[GH_TEST_KEYS];
static gchar *hex[GH_TEST_KEYS];
static GhTestBus bus;
static GhTestSigner signer;
static gboolean gui_available;     /* --gui: this process has a display */
static gboolean display_available; /* the groundhog executable would have one */
static guint app_serial;

/* ---- recording relay transport (H1) -------------------------------------------- */

typedef struct {
  GhRelayScope *scope;
  gchar *url;
  gboolean closed;
} Req;

typedef struct {
  GPtrArray *reqs;      /* Req: every DM inbox REQ ever opened */
  GPtrArray *discovery; /* GhRelayScope: every relay-list discovery REQ */
} Recorder;

static gchar discovery_handle;

static void
req_free(gpointer data)
{
  Req *req = data;
  gh_relay_scope_unref(req->scope);
  g_free(req->url);
  g_free(req);
}

static gpointer
recorder_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters,
              gpointer data, GError **error)
{
  Recorder *rec = data;
  (void)error;
  const NostrFilter *filter = &filters->filters[0];
  if (nostr_filter_kinds_len(filter) == 2) {
    g_ptr_array_add(rec->discovery, gh_relay_scope_ref(scope));
    return &discovery_handle;
  }
  g_assert_cmpint(nostr_filter_kinds_get(filter, 0), ==, 1059);
  Req *req = g_new0(Req, 1);
  req->scope = gh_relay_scope_ref(scope);
  req->url = g_strdup(url);
  g_ptr_array_add(rec->reqs, req);
  return req;
}

static void
recorder_close(gpointer handle, gpointer data)
{
  (void)data;
  if (handle != &discovery_handle)
    ((Req *)handle)->closed = TRUE;
}

static const GhRelayTransport recorder_transport = { recorder_open, recorder_close };

static gboolean
recorder_send_auth(gpointer handle, const gchar *json, gpointer data, GError **error)
{
  (void)handle; (void)json; (void)data; (void)error;
  return TRUE;
}

static void
recorder_resubscribe(gpointer handle, gpointer data)
{
  (void)handle; (void)data;
}

static const GhRelayAuthTransport recorder_auth = { recorder_send_auth, recorder_resubscribe };

static Req *
open_req(Recorder *rec, const gchar *url)
{
  for (guint i = 0; i < rec->reqs->len; i++) {
    Req *req = g_ptr_array_index(rec->reqs, i);
    if (!req->closed && g_str_equal(req->url, url))
      return req;
  }
  return NULL;
}

static guint
open_reqs(Recorder *rec)
{
  guint n = 0;
  for (guint i = 0; i < rec->reqs->len; i++)
    n += !((Req *)g_ptr_array_index(rec->reqs, i))->closed;
  return n;
}

/* ---- fake org.freedesktop.portal.Background --------------------------------------- */

typedef struct {
  GDBusNodeInfo *node;
  guint registration;
  guint version;
  gboolean deny;          /* Response 1 (the user said no) */
  gboolean unknown;       /* answer RequestBackground as an unknown method */
  GPtrArray *requests;    /* GVariant a{sv}: every RequestBackground's options */
  GPtrArray *statuses;    /* gchar*: every SetStatus message */
} FakePortal;

static FakePortal portal;

static void
portal_call(GDBusConnection *connection, const gchar *sender, const gchar *path,
            const gchar *interface, const gchar *method, GVariant *parameters,
            GDBusMethodInvocation *invocation, gpointer user_data)
{
  (void)path; (void)interface; (void)user_data;
  if (g_str_equal(method, "SetStatus")) {
    g_autoptr(GVariant) options = g_variant_get_child_value(parameters, 0);
    const gchar *message = NULL;
    g_assert_true(g_variant_lookup(options, "message", "&s", &message));
    g_assert_cmpuint(g_utf8_strlen(message, -1), <=, 96);
    g_ptr_array_add(portal.statuses, g_strdup(message));
    g_dbus_method_invocation_return_value(invocation, NULL);
    return;
  }
  if (portal.unknown) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.freedesktop.DBus.Error.UnknownMethod", "no background portal here");
    return;
  }
  const gchar *window = NULL;
  g_autoptr(GVariant) options = NULL;
  g_variant_get(parameters, "(&s@a{sv})", &window, &options);
  g_assert_cmpstr(window, ==, "");
  g_ptr_array_add(portal.requests, g_variant_ref(options));
  const gchar *token = NULL;
  g_assert_true(g_variant_lookup(options, "handle_token", "&s", &token));
  g_autofree gchar *who = g_strdup(sender + 1);
  g_strdelimit(who, ".", '_');
  g_autofree gchar *handle = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s",
                                             who, token);
  g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", handle));
  gboolean autostart = FALSE;
  g_variant_lookup(options, "autostart", "b", &autostart);
  GVariantBuilder results;
  g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&results, "{sv}", "background", g_variant_new_boolean(!portal.deny));
  g_variant_builder_add(&results, "{sv}", "autostart",
                        g_variant_new_boolean(!portal.deny && autostart));
  g_dbus_connection_emit_signal(connection, sender, handle, "org.freedesktop.portal.Request",
                                "Response", g_variant_new("(ua{sv})", portal.deny ? 1u : 0u,
                                                          &results), NULL);
}

static GVariant *
portal_get_property(GDBusConnection *connection, const gchar *sender, const gchar *path,
                    const gchar *interface, const gchar *property, GError **error,
                    gpointer user_data)
{
  (void)connection; (void)sender; (void)path; (void)interface; (void)error; (void)user_data;
  g_assert_cmpstr(property, ==, "version");
  return g_variant_new_uint32(portal.version);
}

static const GDBusInterfaceVTable portal_vtable = { portal_call, portal_get_property, NULL,
                                                    { 0 } };

static void
portal_up(void)
{
  g_autoptr(GError) error = NULL;
  portal.node = g_dbus_node_info_new_for_xml(
    "<node><interface name='org.freedesktop.portal.Background'>"
    "<method name='RequestBackground'><arg type='s' direction='in'/>"
    "<arg type='a{sv}' direction='in'/><arg type='o' direction='out'/></method>"
    "<method name='SetStatus'><arg type='a{sv}' direction='in'/></method>"
    "<property name='version' type='u' access='read'/>"
    "</interface></node>", &error);
  g_assert_no_error(error);
  portal.registration = g_dbus_connection_register_object(bus.owner,
    "/org/freedesktop/portal/desktop", portal.node->interfaces[0], &portal_vtable, NULL, NULL,
    &error);
  g_assert_no_error(error);
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync(bus.owner, "org.freedesktop.DBus",
    "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
    g_variant_new("(su)", "org.freedesktop.portal.Desktop", 4u), G_VARIANT_TYPE("(u)"),
    G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
  g_assert_no_error(error);
  portal.requests = g_ptr_array_new_with_free_func((GDestroyNotify)g_variant_unref);
  portal.statuses = g_ptr_array_new_with_free_func(g_free);
}

static void
portal_reset(guint version)
{
  portal.version = version;
  portal.deny = FALSE;
  portal.unknown = FALSE;
  g_ptr_array_set_size(portal.requests, 0);
  g_ptr_array_set_size(portal.statuses, 0);
}

static void
portal_down(void)
{
  g_dbus_connection_unregister_object(bus.owner, portal.registration);
  g_dbus_node_info_unref(portal.node);
  g_ptr_array_unref(portal.requests);
  g_ptr_array_unref(portal.statuses);
}

static gboolean
portal_has_status(gpointer data)
{
  for (guint i = 0; i < portal.statuses->len; i++)
    if (g_str_equal(g_ptr_array_index(portal.statuses, i), data))
      return TRUE;
  return FALSE;
}

static GVariant *
portal_request(guint i)
{
  g_assert_cmpuint(i, <, portal.requests->len);
  return g_ptr_array_index(portal.requests, i);
}

static gboolean
request_autostart(guint i)
{
  gboolean autostart = FALSE;
  g_assert_true(g_variant_lookup(portal_request(i), "autostart", "b", &autostart));
  return autostart;
}

/* ---- fixture ------------------------------------------------------------------------- */

typedef struct _Fixture Fixture;
typedef void (*Script)(Fixture *f);

struct _Fixture {
  Recorder rec;
  GSettings *settings;
  GSettings *gnostr;
  FakeSecret *secret;
  gchar *root, *data_dir, *state_dir, *config_dir;
  gint64 list_time;
  GhBackgroundMethod method;
  gboolean with_stack;
  /* The stack: what one Groundhog process owns (gh-app-services.c). */
  GhAccountController *accounts;
  GhAccountRelays *relays;
  GhConversationStore *model;
  GhDmInbox *inbox;
  GhStoreKey *store_key;
  GhAccountStore *store;
  GhBackground *background;
  /* The application run. */
  GApplication *app;
  Script script;
  guint activations;
  gboolean quit_activated;
  gboolean shut_down;
  GPtrArray *events;
  gchar *db_path;
};

static GPtrArray *
fake_list(gpointer data, GError **error)
{
  (void)data;
  (void)error;
  GPtrArray *ids = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  for (guint key = 1; key <= 2; key++) {
    GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
    info->npub = g_strdup(npub[key]);
    info->label = g_strdup_printf("Key %u", key);
    g_ptr_array_add(ids, info);
  }
  return ids;
}

static gboolean
listed(gpointer data)
{
  return gh_account_controller_get_state(data) != GH_ACCOUNT_STATE_DISCOVERING;
}

static void
on_store_closed(GhAccountStore *store, const gchar *account, Fixture *f)
{
  (void)store;
  g_ptr_array_add(f->events, g_strdup_printf("closed %s", account));
}

static void
fixture_up(Fixture *f, GhBackgroundMethod method)
{
  g_autoptr(GError) error = NULL;
  f->method = method;
  f->rec.reqs = g_ptr_array_new_with_free_func(req_free);
  f->rec.discovery = g_ptr_array_new_with_free_func((GDestroyNotify)gh_relay_scope_unref);
  f->events = g_ptr_array_new_with_free_func(g_free);
  f->settings = g_settings_new("org.nostr.Groundhog");
  f->gnostr = g_settings_new("org.gnostr.Client");
  g_settings_reset(f->settings, RUN); /* the default, unconfirmed */
  g_settings_set_string(f->gnostr, "current-npub", npub[1]);
  const gchar *sources[] = { DISCOVERY, NULL };
  g_settings_set_strv(f->settings, "discovery-relays", sources);
  g_settings_set_string(f->settings, "signer-method", "auto");
  g_settings_set_string(f->settings, "current-npub", npub[2]);
  f->secret = fake_secret_new();
  f->root = g_dir_make_tmp("groundhog-background-XXXXXX", &error);
  g_assert_no_error(error);
  f->data_dir = g_build_filename(f->root, "data", NULL);
  f->state_dir = g_build_filename(f->root, "state", NULL);
  f->config_dir = g_build_filename(f->root, "config", NULL);
  g_assert_cmpint(g_mkdir(f->data_dir, 0700), ==, 0);
  f->list_time = 1700000000;
}

static void
fixture_down(Fixture *f)
{
  g_clear_object(&f->app);
  g_ptr_array_unref(f->rec.reqs);
  g_ptr_array_unref(f->rec.discovery);
  g_ptr_array_unref(f->events);
  g_settings_reset(f->gnostr, "current-npub");
  g_settings_reset(f->settings, RUN);
  g_settings_reset(f->settings, "current-npub");
  g_settings_reset(f->settings, "discovery-relays");
  g_object_unref(f->gnostr);
  g_object_unref(f->settings);
  g_object_unref(f->secret);
  gh_test_remove_tree(f->root);
  g_free(f->root);
  g_free(f->data_dir);
  g_free(f->state_dir);
  g_free(f->config_dir);
  g_free(f->db_path);
}

static GhBackground *
background_new(Fixture *f, GApplication *app, GDBusConnection *connection)
{
  GhBackgroundConfig config = {
    .settings = f->settings,
    .connection = connection,
    .method = f->method,
    .config_dir = f->config_dir,
    .state_dir = f->state_dir,
    .account_store = f->store ? G_OBJECT(f->store) : NULL,
  };
  return gh_background_new(app, &config);
}

/* gh-app-services.c's table, top to bottom, with injected transports. */
static void
stack_up(Fixture *f)
{
  f->accounts = gh_account_controller_new_full(f->settings, bus.client, fake_list, NULL);
  gh_test_spin_until(listed, f->accounts);
  f->relays = gh_account_relays_new(f->accounts, f->settings, &recorder_transport, &f->rec);
  f->model = gh_conversation_store_new();
  f->inbox = gh_dm_inbox_new_with_storage(f->accounts, f->relays, f->model,
                                          &recorder_transport, &recorder_auth, &f->rec);
  f->store_key = gh_store_key_new(GH_STORE_KEY_BACKEND(f->secret));
  GhAccountStoreConfig config = {
    .accounts = f->accounts,
    .store_key = f->store_key,
    .conversations = f->model,
    .inbox = f->inbox,
    .settings = f->settings,
    .data_dir = f->data_dir,
    .legacy_state_dir = f->state_dir,
  };
  f->store = gh_account_store_new(&config);
  g_signal_connect(f->store, "store-closed", G_CALLBACK(on_store_closed), f);
}

/* ... and bottom to top: the background service first. */
static void
stack_down(Fixture *f)
{
  if (f->background)
    gh_test_release(g_steal_pointer(&f->background));
  if (!f->store)
    return;
  gh_test_release(g_steal_pointer(&f->store));
  gh_test_release(g_steal_pointer(&f->store_key));
  gh_test_release(g_steal_pointer(&f->inbox));
  g_clear_object(&f->model);
  gh_test_release(g_steal_pointer(&f->relays));
  gh_test_release(g_steal_pointer(&f->accounts));
  GhTestSenders check = { &bus, &signer };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
}

static gboolean
state_settled(gpointer data)
{
  return gh_account_store_get_state(data) != GH_ACCOUNT_STORE_OPENING;
}

static GhAccountStoreState
settle(Fixture *f)
{
  gh_test_spin_until(state_settled, f->store);
  gh_test_run_until_idle();
  return gh_account_store_get_state(f->store);
}

static void
publish_inbox_list(Fixture *f, guint key)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 10050);
  nostr_event_set_created_at(event, ++f->list_time);
  nostr_event_set_content(event, "");
  nostr_event_set_tags(event, nostr_tags_new(1, nostr_tag_new("relay", INBOX_A, NULL)));
  g_assert_cmpint(nostr_event_sign(event, gh_test_secret[key]), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  g_assert_cmpuint(f->rec.discovery->len, >, 0);
  gh_relay_scope_event(g_ptr_array_index(f->rec.discovery, f->rec.discovery->len - 1),
                       DISCOVERY, json);
  free(json);
  gh_test_run_until_idle();
}

/* A NIP-17 message from key `from` to key `to`, gift-wrapped (NIP-59). */
static gchar *
craft_wrap(guint from, guint to, gint64 created_at, const gchar *content)
{
  NostrEvent *rumor = nostr_event_new();
  nostr_event_set_kind(rumor, 14);
  nostr_event_set_pubkey(rumor, hex[from]);
  nostr_event_set_created_at(rumor, created_at);
  nostr_event_set_content(rumor, content);
  nostr_event_set_tags(rumor, nostr_tags_new(1, nostr_tag_new("p", hex[to], NULL)));
  rumor->id = nostr_event_get_id(rumor);
  char *rumor_json = nostr_event_serialize_compact(rumor);
  nostr_event_free(rumor);
  guint8 sk[32], pk[32];
  g_assert_true(nostr_hex2bin(sk, gh_test_secret[from], sizeof sk));
  g_assert_true(nostr_hex2bin(pk, hex[to], sizeof pk));
  char *ciphertext = NULL;
  g_assert_cmpint(nostr_nip44_encrypt_v2(sk, pk, (const guint8 *)rumor_json,
                                          strlen(rumor_json), &ciphertext), ==, 0);
  free(rumor_json);
  NostrEvent *seal = nostr_event_new();
  nostr_event_set_kind(seal, 13);
  nostr_event_set_pubkey(seal, hex[from]);
  nostr_event_set_content(seal, ciphertext);
  nostr_event_set_created_at(seal, created_at);
  nostr_event_set_tags(seal, nostr_tags_new(0));
  free(ciphertext);
  g_assert_cmpint(nostr_event_sign(seal, gh_test_secret[from]), ==, 0);
  guint8 ephemeral[32];
  g_assert_true(nostr_hex2bin(ephemeral, gh_test_secret[4], sizeof ephemeral));
  NostrEvent *wrap = nostr_nip59_wrap_with_key(seal, hex[to], ephemeral);
  nostr_event_free(seal);
  g_assert_nonnull(wrap);
  char *json = nostr_event_serialize_compact(wrap);
  nostr_event_free(wrap);
  gchar *out = g_strdup(json);
  free(json);
  return out;
}

static gboolean
model_has_items(gpointer data)
{
  return g_list_model_get_n_items(G_LIST_MODEL(data)) > 0;
}

static gchar *
store_db_path(Fixture *f, guint key)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *dir = gh_store_account_dir_path(f->data_dir, hex[key], &error);
  g_assert_no_error(error);
  return g_build_filename(dir, "store.db", NULL);
}

static goffset
file_size(const gchar *path)
{
  GStatBuf st;
  return g_stat(path, &st) == 0 ? (goffset)st.st_size : -1;
}

/* GDBus's timeout for a method call still waiting for its reply (the name
 * gdbusconnection.c gives it, GLib 2.58 on): the call is in flight, not an
 * idle timer. GDBus destroys it (from its worker thread) when the reply
 * lands. */
#define DBUS_REPLY_TIMEOUT "[gio] send_message_with_reply_unlocked"

/* NO-12: no timer on the main context fires within the next minute. GLib
 * source ids grow from 1 per context, so every live source is visited; a
 * timeout's ready time is its next expiry. Returns the first short one
 * (skipping own[]), or 0; *in_flight counts short D-Bus reply timeouts,
 * which are not returned. */
static guint
first_short_timer(const guint *own, guint n_own, gint64 *due_ms, guint *in_flight)
{
  GMainContext *context = g_main_context_default();
  GSource *probe = g_idle_source_new();
  guint last = g_source_attach(probe, context);
  g_source_destroy(probe);
  g_source_unref(probe);
  gint64 now = g_get_monotonic_time();
  *in_flight = 0;
  for (guint id = 1; id < last; id++) {
    gboolean mine = FALSE;
    for (guint i = 0; i < n_own; i++)
      mine = mine || own[i] == id;
    GSource *source = mine ? NULL : g_main_context_find_source_by_id(context, id);
    if (!source || g_source_is_destroyed(source))
      continue;
    gint64 ready = g_source_get_ready_time(source);
    if (ready < 0 || ready - now >= 59 * G_USEC_PER_SEC)
      continue;
    if (g_strcmp0(g_source_get_name(source), DBUS_REPLY_TIMEOUT) == 0) {
      (*in_flight)++;
      continue;
    }
    *due_ms = (ready - now) / 1000;
    return id;
  }
  return 0;
}

/* Checked after every main-loop turn until it holds: a short timer that is
 * not a call in flight fails at once, named; calls in flight are waited out
 * (their replies may arm timers, so their callbacks run and it is checked
 * again). No time window decides anything: under load a reply can take
 * longer than any window, and a window long enough for that would hide a
 * one-shot timer firing inside it (nostrc-yzlp: the old two-second window
 * also removed its own deadline after it had fired, when the last reply
 * landed in the turn the deadline expired). The deadline only bounds a
 * reply that never comes. */
static void
assert_no_short_timers(void)
{
  gboolean expired = FALSE;
  guint own[] = { g_timeout_add_seconds(10, gh_test_deadline_hit, &expired),
                  g_timeout_add(10, gh_test_tick, NULL) };
  for (;;) {
    gint64 due = 0;
    guint in_flight = 0;
    guint id = first_short_timer(own, G_N_ELEMENTS(own), &due, &in_flight);
    if (id) {
      GSource *source = g_main_context_find_source_by_id(NULL, id);
      g_error("source %u (%s) fires in %" G_GINT64_FORMAT " ms while idle", id,
              source && g_source_get_name(source) ? g_source_get_name(source) : "unnamed", due);
    }
    if (in_flight == 0) {
      /* Replies' callbacks, and whatever they start, before the verdict. */
      if (!g_main_context_iteration(NULL, FALSE))
        break;
      continue;
    }
    if (expired)
      g_error("%u D-Bus call(s) still waiting for a reply after 10 s", in_flight);
    g_main_context_iteration(NULL, TRUE);
  }
  g_source_remove(own[1]);
  if (!expired) /* a fired deadline removed itself (G_SOURCE_REMOVE) */
    g_source_remove(own[0]);
}

/* ---- in-process application runs ----------------------------------------------------- */

static void
on_quit(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  Fixture *f = data;
  (void)action;
  (void)parameter;
  f->quit_activated = TRUE;
  g_application_quit(f->app);
}

static void
on_quit_seen(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action;
  (void)parameter;
  ((Fixture *)data)->quit_activated = TRUE;
}

static gboolean
run_script(gpointer data)
{
  Fixture *f = data;
  f->script(f);
  return G_SOURCE_REMOVE;
}

static void
on_startup(GApplication *app, Fixture *f)
{
  if (GTK_IS_APPLICATION(app)) {
    gh_window_setup_application(GTK_APPLICATION(app)); /* app.quit, as in main.c */
    g_signal_connect(g_action_map_lookup_action(G_ACTION_MAP(app), "quit"), "activate",
                     G_CALLBACK(on_quit_seen), f);
  } else {
    g_autoptr(GSimpleAction) quit = g_simple_action_new("quit", NULL);
    g_signal_connect(quit, "activate", G_CALLBACK(on_quit), f);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(quit));
  }
  if (f->with_stack)
    stack_up(f);
  f->background = background_new(f, app, NULL);
  g_idle_add(run_script, f);
}

static void
on_activate(GApplication *app, Fixture *f)
{
  f->activations++;
  if (GTK_IS_APPLICATION(app)) {
    GhWindow *window = gh_window_new(GTK_APPLICATION(app));
    gtk_window_present(GTK_WINDOW(window));
  }
}

static void
on_shutdown(GApplication *app, Fixture *f)
{
  (void)app;
  stack_down(f);
  f->shut_down = TRUE;
}

static int
run_app(Fixture *f, GApplication *app, gboolean service)
{
  f->app = app;
  g_signal_connect(app, "startup", G_CALLBACK(on_startup), f);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), f);
  g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), f);
  gchar *argv[] = { "test-groundhog-background", service ? "--gapplication-service" : NULL,
                    NULL };
  return g_application_run(app, service ? 2 : 1, argv);
}

static GApplication *
service_app(void)
{
  g_autofree gchar *id = g_strdup_printf("org.nostr.GroundhogTest.Service%u", ++app_serial);
  return g_application_new(id, G_APPLICATION_DEFAULT_FLAGS);
}

/* NO-9 (service mode) and NO-12 on one run. */
static void
script_service(Fixture *f)
{
  /* --gapplication-service: nothing activated, so nothing opened a window;
   * the background hold keeps the process (no 10 s service exit timer). */
  g_assert_cmpuint(f->activations, ==, 0);
  g_assert_true(gh_background_get_holding(f->background));
  g_assert_cmpint(settle(f), ==, GH_ACCOUNT_STORE_OPEN);
  /* The inbox scope opens windowless. */
  publish_inbox_list(f, 2);
  Req *req = open_req(&f->rec, INBOX_A);
  g_assert_nonnull(req);
  g_autofree gchar *wrap = craft_wrap(1, 2, g_get_real_time() / G_USEC_PER_SEC - 60,
                                      CANARY " background");
  gh_relay_scope_event(req->scope, INBOX_A, wrap);
  gh_test_spin_until(model_has_items, f->model);
  gh_relay_scope_eose(req->scope, INBOX_A);
  gh_test_run_until_idle();
  /* NO-12: after EOSE with no traffic, nothing wakes within a minute. */
  assert_no_short_timers();
  /* The status says so, through the portal (a desktop's background list). */
  gh_test_spin_until(portal_has_status, (gpointer)GH_BACKGROUND_STATUS_RECEIVING);
  g_assert_cmpstr(gh_background_get_status(f->background), ==, GH_BACKGROUND_STATUS_RECEIVING);
  /* The default is not a confirmed choice: no autostart request. */
  g_assert_cmpuint(portal.requests->len, ==, 0);
  /* The message went to the store's WAL; quitting must checkpoint it. */
  f->db_path = store_db_path(f, 2);
  g_autofree gchar *wal = g_strconcat(f->db_path, "-wal", NULL);
  g_assert_cmpint(file_size(wal), >, 0);
  g_action_group_activate_action(G_ACTION_GROUP(f->app), "quit", NULL);
}

static void
test_no9_service_mode(void)
{
  Fixture f = { 0 };
  fixture_up(&f, GH_BACKGROUND_METHOD_PORTAL);
  portal_reset(2);
  f.with_stack = TRUE;
  f.script = script_service;
  int status = run_app(&f, service_app(), TRUE);
  g_assert_cmpint(status, ==, 0);
  /* app.quit stopped everything: scopes closed, the store closed after the
   * inbox let go of it, the WAL checkpointed away. */
  g_assert_true(f.quit_activated);
  g_assert_true(f.shut_down);
  g_assert_cmpuint(open_reqs(&f.rec), ==, 0);
  g_assert_cmpuint(f.events->len, ==, 1);
  g_autofree gchar *closed = g_strdup_printf("closed %s", hex[2]);
  g_assert_cmpstr(g_ptr_array_index(f.events, 0), ==, closed);
  g_autofree gchar *wal = g_strconcat(f.db_path, "-wal", NULL);
  g_assert_cmpint(file_size(wal), <=, 0);
  g_assert_null(gh_background_get_for_application(f.app));
  fixture_down(&f);
}

/* NO-11: the keyring is locked when the autostarted service comes up. */
static void
script_locked(Fixture *f)
{
  g_assert_cmpuint(f->activations, ==, 0);
  g_assert_cmpint(settle(f), ==, GH_ACCOUNT_STORE_LOCKED);
  publish_inbox_list(f, 2);
  /* No inbox REQ (the wraps stay on the relays) and no keyring prompt. */
  g_assert_cmpuint(f->rec.reqs->len, ==, 0);
  g_assert_cmpuint(fake_secret_prompts(f->secret), ==, 0);
  /* Still held, and honest about it. */
  g_assert_true(gh_background_get_holding(f->background));
  g_assert_cmpstr(gh_background_get_status(f->background), ==, GH_BACKGROUND_STATUS_LOCKED);
  gh_test_spin_until(portal_has_status, (gpointer)GH_BACKGROUND_STATUS_LOCKED);
  g_assert_false(portal_has_status((gpointer)GH_BACKGROUND_STATUS_RECEIVING));
  assert_no_short_timers();
  g_action_group_activate_action(G_ACTION_GROUP(f->app), "quit", NULL);
}

static void
test_no11_locked_start(void)
{
  Fixture f = { 0 };
  fixture_up(&f, GH_BACKGROUND_METHOD_PORTAL);
  portal_reset(2);
  fake_secret_set_locked(f.secret, TRUE);
  f.with_stack = TRUE;
  f.script = script_locked;
  g_assert_cmpint(run_app(&f, service_app(), TRUE), ==, 0);
  g_assert_true(f.shut_down);
  g_assert_cmpuint(fake_secret_prompts(f.secret), ==, 0);
  g_assert_cmpuint(gh_test_count_files(f.data_dir), ==, 0);
  fixture_down(&f);
}

/* NO-12 on its own: a quiet account after EOSE, host method, no portal. */
static void
script_idle(Fixture *f)
{
  g_assert_cmpint(settle(f), ==, GH_ACCOUNT_STORE_OPEN);
  publish_inbox_list(f, 2);
  Req *req = open_req(&f->rec, INBOX_A);
  g_assert_nonnull(req);
  gh_relay_scope_eose(req->scope, INBOX_A);
  gh_test_run_until_idle();
  assert_no_short_timers();
  g_action_group_activate_action(G_ACTION_GROUP(f->app), "quit", NULL);
}

static void
test_no12_idle_timers(void)
{
  Fixture f = { 0 };
  fixture_up(&f, GH_BACKGROUND_METHOD_FILE);
  f.with_stack = TRUE;
  f.script = script_idle;
  g_assert_cmpint(run_app(&f, service_app(), TRUE), ==, 0);
  g_assert_true(f.quit_activated);
  fixture_down(&f);
}

/* ---- window close (GUI) ---------------------------------------------------------------- */

static gboolean
app_has_no_window(gpointer data)
{
  return gtk_application_get_windows(GTK_APPLICATION(data)) == NULL;
}

static GtkWindow *
only_window(Fixture *f)
{
  GList *windows = gtk_application_get_windows(GTK_APPLICATION(f->app));
  g_assert_nonnull(windows);
  g_assert_null(windows->next);
  return windows->data;
}

static gboolean
count_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  (void)widget;
  (void)clock;
  return ++*(guint *)data < 3 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static gboolean
ticked_thrice(gpointer data)
{
  return *(guint *)data >= 3;
}

/* libadwaita opens a presented dialog's sheet on its second frame after
 * mapping (AdwDialog's map tick); a click before that finds nothing open to
 * close. Nobody can click a dialog they have not seen: wait those frames. */
static void
wait_until_open(AdwDialog *dialog)
{
  guint ticks = 0;
  gtk_widget_add_tick_callback(GTK_WIDGET(dialog), count_tick, &ticks, NULL);
  gh_test_spin_until(ticked_thrice, &ticks);
}

/* The one-time explanation: shows on the first close, holds the close back,
 * and acts on the answer once it has closed. */
static AdwDialog *
close_into_explanation(Fixture *f)
{
  GtkWindow *window = only_window(f);
  gtk_window_close(window);
  g_assert_true(gtk_application_get_windows(GTK_APPLICATION(f->app)) != NULL);
  AdwDialog *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_true(ADW_IS_ALERT_DIALOG(dialog));
  wait_until_open(dialog);
  g_assert_cmpstr(adw_alert_dialog_get_close_response(ADW_ALERT_DIALOG(dialog)), ==,
                  "background");
  g_assert_false(gh_background_get_explained(f->background));
  return dialog;
}

static GtkWidget *
find_button(GtkWidget *widget, const gchar *label)
{
  if (GTK_IS_BUTTON(widget) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), label) == 0)
    return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child;
       child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_button(child, label);
    if (found)
      return found;
  }
  return NULL;
}

/* Clicks the dialog's own button, in libadwaita's own order (the dialog
 * closes, then responds). */
static void
answer(AdwDialog *dialog, const gchar *label)
{
  GtkWidget *button = find_button(GTK_WIDGET(dialog), label);
  g_assert_nonnull(button);
  g_signal_emit_by_name(button, "clicked");
}

static void
script_close_keeps_running(Fixture *f)
{
  g_assert_cmpuint(f->activations, ==, 1);
  g_assert_true(gh_background_get_holding(f->background));
  answer(close_into_explanation(f), "_Keep Running");
  gh_test_spin_until(app_has_no_window, f->app);
  /* The window is gone, the process is not (the hold), and the choice to
   * explain is remembered on disk. */
  g_assert_true(gh_background_get_holding(f->background));
  g_assert_true(gh_background_get_explained(f->background));
  g_autofree gchar *marker = g_build_filename(f->state_dir, GH_BACKGROUND_EXPLAINED_FILE, NULL);
  g_assert_true(g_file_test(marker, G_FILE_TEST_IS_REGULAR));
  /* Opened again from the shell: a new window; its close is not asked. */
  g_application_activate(f->app);
  g_assert_cmpuint(f->activations, ==, 2);
  GtkWindow *window = only_window(f);
  gtk_window_close(window);
  gh_test_spin_until(app_has_no_window, f->app);
  g_assert_true(gh_background_get_holding(f->background));
  /* Turning background delivery off with no window ends the process: no
   * quit is activated, the run simply returns once this script does. */
  g_settings_set_boolean(f->settings, RUN, FALSE);
  g_assert_false(gh_background_get_holding(f->background));
}

static GApplication *
gui_app(void)
{
  g_autofree gchar *id = g_strdup_printf("org.nostr.GroundhogTest.Gui%u", ++app_serial);
  /* Non-unique: this binary's GUI mode has no private bus to own a name on. */
  return G_APPLICATION(adw_application_new(id, G_APPLICATION_NON_UNIQUE));
}

static void
test_no9_close_keeps_running(void)
{
  if (!gui_available) {
    g_test_skip("no graphical display");
    return;
  }
  Fixture f = { 0 };
  fixture_up(&f, GH_BACKGROUND_METHOD_FILE);
  f.script = script_close_keeps_running;
  g_assert_cmpint(run_app(&f, gui_app(), FALSE), ==, 0);
  g_assert_false(f.quit_activated);
  g_assert_true(f.shut_down);
  /* The explicit "off" is a confirmed choice, but there is nothing to
   * remove; nothing was ever written for the default. */
  g_autofree gchar *autostart = g_build_filename(f.config_dir, "autostart", NULL);
  g_assert_false(g_file_test(autostart, G_FILE_TEST_EXISTS));
  fixture_down(&f);
}

static void
script_explanation_quit(Fixture *f)
{
  answer(close_into_explanation(f), "_Quit Groundhog");
  /* Quitting from the explanation ends the run (below). */
}

static void
test_no9_explanation_quit(void)
{
  if (!gui_available) {
    g_test_skip("no graphical display");
    return;
  }
  Fixture f = { 0 };
  fixture_up(&f, GH_BACKGROUND_METHOD_FILE);
  f.script = script_explanation_quit;
  g_assert_cmpint(run_app(&f, gui_app(), FALSE), ==, 0);
  g_assert_true(f.shut_down);
  g_autofree gchar *marker = g_build_filename(f.state_dir, GH_BACKGROUND_EXPLAINED_FILE, NULL);
  g_assert_true(g_file_test(marker, G_FILE_TEST_IS_REGULAR));
  fixture_down(&f);
}

static void
script_close_off(Fixture *f)
{
  g_assert_false(gh_background_get_holding(f->background));
  gtk_window_close(only_window(f));
  /* No explanation and nothing holds: the run ends with the window. */
  g_assert_null(gtk_application_get_windows(GTK_APPLICATION(f->app)));
}

static void
test_no9_close_background_off(void)
{
  if (!gui_available) {
    g_test_skip("no graphical display");
    return;
  }
  Fixture f = { 0 };
  fixture_up(&f, GH_BACKGROUND_METHOD_FILE);
  g_settings_set_boolean(f.settings, RUN, FALSE);
  f.script = script_close_off;
  g_assert_cmpint(run_app(&f, gui_app(), FALSE), ==, 0);
  g_assert_true(f.shut_down);
  g_assert_false(f.quit_activated);
  g_autofree gchar *marker = g_build_filename(f.state_dir, GH_BACKGROUND_EXPLAINED_FILE, NULL);
  g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));
  fixture_down(&f);
}

/* ---- NO-10: autostart ------------------------------------------------------------------ */

typedef struct {
  GAsyncResult *result;
} Wait;

static void
on_set(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  ((Wait *)data)->result = g_object_ref(result);
}

static gboolean
waited(gpointer data)
{
  return ((Wait *)data)->result != NULL;
}

static gboolean
set_enabled(GhBackground *background, gboolean enabled, GError **error)
{
  Wait wait = { 0 };
  gh_background_set_enabled_async(background, enabled, NULL, on_set, &wait);
  gh_test_spin_until(waited, &wait);
  gboolean ok = gh_background_set_enabled_finish(background, wait.result, error);
  g_object_unref(wait.result);
  return ok;
}

static gchar *
autostart_path(Fixture *f)
{
  return g_build_filename(f->config_dir, "autostart", GH_BACKGROUND_AUTOSTART_FILE, NULL);
}

static gchar *
read_file(const gchar *path)
{
  gchar *contents = NULL;
  return g_file_get_contents(path, &contents, NULL, NULL) ? contents : NULL;
}

static gchar *
template_contents(void)
{
  gchar *contents = read_file(GROUNDHOG_TEST_AUTOSTART);
  g_assert_nonnull(contents);
  return contents;
}

static void
write_file(const gchar *path, const gchar *contents)
{
  g_autofree gchar *dir = g_path_get_dirname(path);
  g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
  g_assert_true(g_file_set_contents(path, contents, -1, NULL));
}

static void
drop(GhBackground *background)
{
  g_object_run_dispose(G_OBJECT(background));
  g_object_unref(background);
}

static GApplication *
unit_app(void)
{
  return g_application_new("org.nostr.GroundhogTest.Unit", G_APPLICATION_NON_UNIQUE);
}

static void
test_no10_host_file(void)
{
  Fixture f = { 0 };
  fixture_up(&f, GH_BACKGROUND_METHOD_FILE);
  g_autoptr(GApplication) app = unit_app();
  g_autofree gchar *path = autostart_path(&f);
  g_autofree gchar *expected = template_contents();
  g_assert_nonnull(strstr(expected, "\nExec="));
  g_assert_nonnull(strstr(expected, "/groundhog --gapplication-service\n"));
  g_autoptr(GError) error = NULL;

  /* The default (D11: on) holds, but writes nothing until confirmed. */
  GhBackground *background = background_new(&f, app, NULL);
  g_assert_cmpint(gh_background_get_method(background), ==, GH_BACKGROUND_METHOD_FILE);
  g_assert_true(gh_background_get_enabled(background));
  g_assert_true(gh_background_get_holding(background));
  g_autofree gchar *autostart_dir = g_build_filename(f.config_dir, "autostart", NULL);
  g_assert_false(g_file_test(autostart_dir, G_FILE_TEST_EXISTS));

  /* Onboarding confirms: the entry is written, private, as templated. */
  g_assert_true(set_enabled(background, TRUE, &error));
  g_assert_no_error(error);
  g_autofree gchar *written = read_file(path);
  g_assert_cmpstr(written, ==, expected);
  GStatBuf st;
  g_assert_cmpint(g_stat(path, &st), ==, 0);
  g_assert_cmpint(st.st_mode & 0777, ==, 0600);
  g_assert_true(gh_background_get_explained(background));
  g_autoptr(GVariant) user = g_settings_get_user_value(f.settings, RUN);
  g_assert_nonnull(user);

  /* NO-10: Preferences switch it off (a plain settings write): removed, and
   * the hold is released. */
  g_settings_set_boolean(f.settings, RUN, FALSE);
  gh_test_run_until_idle();
  g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
  g_assert_false(gh_background_get_holding(background));
  g_assert_null(gh_background_get_status(background));

  /* Reset to the default: held again, still nothing at login. */
  g_settings_set_boolean(f.settings, RUN, TRUE);
  gh_test_run_until_idle();
  g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS));
  g_settings_reset(f.settings, RUN);
  gh_test_run_until_idle();
  g_assert_true(gh_background_get_holding(background));
  g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));

  /* An entry of the user's own under this name is never touched. */
  const gchar *foreign = "[Desktop Entry]\nType=Application\nName=Mine\nExec=groundhog\n";
  write_file(path, foreign);
  g_assert_true(set_enabled(background, FALSE, &error));
  g_autofree gchar *kept = read_file(path);
  g_assert_cmpstr(kept, ==, foreign);
  g_assert_true(set_enabled(background, TRUE, &error));
  g_autofree gchar *kept_again = read_file(path);
  g_assert_cmpstr(kept_again, ==, foreign);
  drop(background);

  /* At start, a confirmed "on" repairs a stale entry of ours ... */
  g_assert_cmpint(g_unlink(path), ==, 0);
  write_file(path, "[Desktop Entry]\nType=Application\nName=Groundhog\n"
                   "Exec=/old/prefix/groundhog --gapplication-service\n"
                   "X-Groundhog-Autostart=true\n");
  background = background_new(&f, app, NULL);
  gh_test_run_until_idle();
  g_autofree gchar *repaired = read_file(path);
  g_assert_cmpstr(repaired, ==, expected);
  drop(background);

  /* ... but leaves one the user switched off in the desktop's settings. */
  g_autofree gchar *hidden = g_strconcat(expected, "Hidden=true\n", NULL);
  write_file(path, hidden);
  background = background_new(&f, app, NULL);
  gh_test_run_until_idle();
  g_autofree gchar *still_hidden = read_file(path);
  g_assert_cmpstr(still_hidden, ==, hidden);
  drop(background);

  /* A confirmed "off" at start removes a leftover entry of ours. */
  write_file(path, expected);
  g_settings_set_boolean(f.settings, RUN, FALSE);
  background = background_new(&f, app, NULL);
  gh_test_run_until_idle();
  g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
  g_assert_false(gh_background_get_holding(background));
  drop(background);
  fixture_down(&f);
}

static gboolean
requests_at_least(gpointer data)
{
  return portal.requests->len >= GPOINTER_TO_UINT(data);
}

static void
test_no10_portal(void)
{
  Fixture f = { 0 };
  fixture_up(&f, GH_BACKGROUND_METHOD_PORTAL);
  portal_reset(2);
  g_autoptr(GApplication) app = unit_app();
  g_autoptr(GError) error = NULL;

  /* Unconfirmed default: status only, no background request. */
  GhBackground *background = background_new(&f, app, bus.client);
  g_assert_cmpint(gh_background_get_method(background), ==, GH_BACKGROUND_METHOD_PORTAL);
  gh_test_spin_until(portal_has_status, (gpointer)GH_BACKGROUND_STATUS_RECEIVING);
  g_assert_cmpuint(portal.requests->len, ==, 0);

  /* Confirmed on: RequestBackground with autostart and the service command. */
  g_assert_true(set_enabled(background, TRUE, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(portal.requests->len, ==, 1);
  GVariant *options = portal_request(0);
  g_assert_true(request_autostart(0));
  g_autofree const gchar **commandline = NULL;
  g_assert_true(g_variant_lookup(options, "commandline", "^a&s", &commandline));
  g_assert_cmpuint(g_strv_length((gchar **)commandline), ==, 2);
  g_assert_cmpstr(commandline[0], ==, "groundhog");
  g_assert_cmpstr(commandline[1], ==, "--gapplication-service");
  gboolean activatable = TRUE;
  g_assert_true(g_variant_lookup(options, "dbus-activatable", "b", &activatable));
  g_assert_false(activatable);
  const gchar *reason = NULL;
  g_assert_true(g_variant_lookup(options, "reason", "&s", &reason));
  g_assert_cmpuint(strlen(reason), >, 0);
  /* The portal owns the entry: Groundhog writes none of its own. */
  g_autofree gchar *autostart_dir = g_build_filename(f.config_dir, "autostart", NULL);
  g_assert_false(g_file_test(autostart_dir, G_FILE_TEST_EXISTS));

  /* NO-10: off → the portal is told autostart=false. */
  g_assert_true(set_enabled(background, FALSE, &error));
  g_assert_cmpuint(portal.requests->len, ==, 2);
  g_assert_false(request_autostart(1));
  g_assert_false(gh_background_get_holding(background));

  /* From Preferences (a settings write), and two changes in a row: one
   * request at a time, the last choice wins. */
  g_settings_set_boolean(f.settings, RUN, TRUE);
  g_settings_set_boolean(f.settings, RUN, FALSE);
  gh_test_spin_until(requests_at_least, GUINT_TO_POINTER(4));
  gh_test_run_until_idle();
  g_assert_cmpuint(portal.requests->len, ==, 4);
  g_assert_true(request_autostart(2));
  g_assert_false(request_autostart(3));

  /* The portal refuses background activity: the key goes off, honestly,
   * with no further request. */
  portal.deny = TRUE;
  g_assert_false(set_enabled(background, TRUE, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error(&error);
  gh_test_run_until_idle();
  g_assert_cmpuint(portal.requests->len, ==, 5);
  g_assert_false(g_settings_get_boolean(f.settings, RUN));
  g_assert_false(gh_background_get_holding(background));
  drop(background);

  /* No Background portal behind the name: not supported, nothing written. */
  portal_reset(2);
  portal.unknown = TRUE;
  background = background_new(&f, app, bus.client);
  g_assert_false(set_enabled(background, TRUE, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error(&error);
  g_assert_false(g_file_test(autostart_dir, G_FILE_TEST_EXISTS));
  drop(background);

  /* Background version 1 has no SetStatus. */
  portal_reset(1);
  g_settings_reset(f.settings, RUN);
  background = background_new(&f, app, bus.client);
  g_assert_true(set_enabled(background, TRUE, &error));
  g_assert_no_error(error);
  gh_test_run_until_idle();
  g_assert_cmpuint(portal.statuses->len, ==, 0);
  drop(background);
  fixture_down(&f);
}

/* A host install never goes through the portal, even when one answers. */
static void
test_auto_host_uses_file(void)
{
  if (g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS)) {
    g_test_skip("running inside a Flatpak sandbox");
    return;
  }
  Fixture f = { 0 };
  fixture_up(&f, GH_BACKGROUND_METHOD_AUTO);
  portal_reset(2);
  g_autoptr(GApplication) app = unit_app();
  g_autoptr(GError) error = NULL;
  GhBackground *background = background_new(&f, app, bus.client);
  g_assert_cmpint(gh_background_get_method(background), ==, GH_BACKGROUND_METHOD_FILE);
  g_assert_true(set_enabled(background, TRUE, &error));
  g_assert_no_error(error);
  g_autofree gchar *path = autostart_path(&f);
  g_assert_true(g_file_test(path, G_FILE_TEST_IS_REGULAR));
  gh_test_run_until_idle();
  g_assert_cmpuint(portal.requests->len, ==, 0);
  g_assert_cmpuint(portal.statuses->len, ==, 0);
  g_assert_true(gh_background_get_for_application(app) == background);
  drop(background);
  g_assert_null(gh_background_get_for_application(app));
  fixture_down(&f);
}

/* ---- the real executable ------------------------------------------------------------------ */

typedef struct {
  GSubprocess *process;
  gboolean exited;
} Child;

static void
on_child_exit(GObject *source, GAsyncResult *result, gpointer data)
{
  Child *child = data;
  g_subprocess_wait_finish(G_SUBPROCESS(source), result, NULL);
  child->exited = TRUE;
}

/* Waits for exit with its own deadline: a service with nothing holding it
 * exits after GLib's 10 s service wait, beyond gh_test_spin_until's bound. */
static void
wait_exit(Child *child, guint seconds)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(seconds, gh_test_deadline_hit, &expired);
  guint tick = g_timeout_add(10, gh_test_tick, NULL);
  while (!child->exited && !expired)
    g_main_context_iteration(NULL, TRUE);
  g_source_remove(tick);
  if (expired) {
    g_subprocess_force_exit(child->process);
    g_error("groundhog did not exit within %u s", seconds);
  }
  g_source_remove(timer);
}

static gboolean
name_owned(gpointer data)
{
  Child *child = data;
  g_assert_false(child->exited);
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync(bus.client, "org.freedesktop.DBus",
    "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
    g_variant_new("(s)", "org.nostr.Groundhog"), G_VARIANT_TYPE("(b)"),
    G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
  gboolean owned = FALSE;
  if (reply)
    g_variant_get(reply, "(b)", &owned);
  return owned;
}

typedef enum { SETTINGS_MEMORY, SETTINGS_KEYFILE_ON, SETTINGS_KEYFILE_OFF } SettingsMode;

static Child *
spawn_service(Fixture *f, SettingsMode mode)
{
  g_autoptr(GError) error = NULL;
  /* Output to a log file, never to ctest's pipes. */
  g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
    G_SUBPROCESS_FLAGS_STDERR_MERGE);
  g_autofree gchar *log = g_build_filename(f->root, "groundhog.log", NULL);
  g_subprocess_launcher_set_stdout_file_path(launcher, log);
  g_subprocess_launcher_setenv(launcher, "GTK_A11Y", "none", TRUE);
  g_subprocess_launcher_setenv(launcher, "DBUS_SESSION_BUS_ADDRESS",
                               nostrc_test_bus_get_address(bus.bus), TRUE);
  g_subprocess_launcher_setenv(launcher, "GSETTINGS_SCHEMA_DIR", GROUNDHOG_TEST_SCHEMA_DIR, TRUE);
  g_subprocess_launcher_setenv(launcher, "XDG_CONFIG_HOME", f->config_dir, TRUE);
  g_subprocess_launcher_setenv(launcher, "XDG_DATA_HOME", f->data_dir, TRUE);
  g_subprocess_launcher_setenv(launcher, "XDG_STATE_HOME", f->state_dir, TRUE);
  g_autofree gchar *cache = g_build_filename(f->root, "cache", NULL);
  g_subprocess_launcher_setenv(launcher, "XDG_CACHE_HOME", cache, TRUE);
  g_subprocess_launcher_unsetenv(launcher, "G_DEBUG");
  if (mode == SETTINGS_MEMORY) {
    g_subprocess_launcher_setenv(launcher, "GSETTINGS_BACKEND", "memory", TRUE);
  } else {
    g_subprocess_launcher_setenv(launcher, "GSETTINGS_BACKEND", "keyfile", TRUE);
    g_autofree gchar *keyfile = g_build_filename(f->config_dir, "glib-2.0", "settings",
                                                 "keyfile", NULL);
    write_file(keyfile, mode == SETTINGS_KEYFILE_ON
                          ? "[org/nostr/Groundhog]\nrun-in-background=true\n"
                          : "[org/nostr/Groundhog]\nrun-in-background=false\n");
  }
  Child *child = g_new0(Child, 1);
  child->process = g_subprocess_launcher_spawn(launcher, &error, GROUNDHOG_TEST_EXECUTABLE,
                                               "--gapplication-service", NULL);
  g_assert_no_error(error);
  g_subprocess_wait_async(child->process, NULL, on_child_exit, child);
  return child;
}

static void
child_free(Child *child, Fixture *f)
{
  if (!g_subprocess_get_if_exited(child->process) ||
      g_subprocess_get_exit_status(child->process) != 0) {
    g_autofree gchar *log = g_build_filename(f->root, "groundhog.log", NULL);
    g_autofree gchar *text = read_file(log);
    g_printerr("groundhog log:\n%s\n", text ? text : "(none)");
  }
  g_object_unref(child->process);
  g_free(child);
}

/* Exit status 0: g_application_run() returned (a signal death has none). */
static void
assert_clean_exit(Child *child, Fixture *f)
{
  if (g_subprocess_get_if_exited(child->process) &&
      g_subprocess_get_exit_status(child->process) == 0)
    return;
  child_free(child, f);
  g_error("groundhog did not shut down normally");
}

static gboolean
file_exists(gpointer path)
{
  return g_file_test(path, G_FILE_TEST_EXISTS);
}

static gboolean
file_missing(gpointer path)
{
  return !g_file_test(path, G_FILE_TEST_EXISTS);
}

/* NO-9 on the shipped binary: the autostart command starts windowless and
 * stays; app.quit and SIGTERM both run the normal shutdown. */
static void
test_no9_process_service(void)
{
  if (!display_available) {
    g_test_skip("no graphical display for the groundhog executable");
    return;
  }
  Fixture f = { 0 };
  fixture_up(&f, GH_BACKGROUND_METHOD_FILE);
  Child *child = spawn_service(&f, SETTINGS_MEMORY);
  gh_test_spin_until(name_owned, child);
  g_autoptr(GError) error = NULL;
  /* GtkApplication exports each window under .../window/<id>: none. */
  g_autoptr(GVariant) xml = g_dbus_connection_call_sync(bus.client, "org.nostr.Groundhog",
    "/org/nostr/Groundhog", "org.freedesktop.DBus.Introspectable", "Introspect", NULL,
    G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
  g_assert_no_error(error);
  const gchar *text = NULL;
  g_variant_get(xml, "(&s)", &text);
  g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(text, &error);
  g_assert_no_error(error);
  for (guint i = 0; node->nodes && node->nodes[i]; i++)
    g_assert_cmpstr(node->nodes[i]->path, !=, "window");
  /* The default is unconfirmed: nothing at login. */
  g_autofree gchar *path = autostart_path(&f);
  g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
  g_autoptr(GVariant) done = g_dbus_connection_call_sync(bus.client, "org.nostr.Groundhog",
    "/org/nostr/Groundhog", "org.gtk.Actions", "Activate",
    g_variant_new("(sava{sv})", "quit", NULL, NULL), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
    &error);
  g_assert_no_error(error);
  wait_exit(child, 20);
  assert_clean_exit(child, &f);
  child_free(child, &f);

  child = spawn_service(&f, SETTINGS_MEMORY);
  gh_test_spin_until(name_owned, child);
  g_subprocess_send_signal(child->process, SIGTERM);
  wait_exit(child, 20);
  assert_clean_exit(child, &f);
  child_free(child, &f);
  fixture_down(&f);
}

/* NO-10 on the shipped binary: a confirmed "on" installs the host entry;
 * a confirmed "off" removes it, and nothing holds the service. */
static void
test_no10_process_autostart(void)
{
  if (!display_available) {
    g_test_skip("no graphical display for the groundhog executable");
    return;
  }
  Fixture f = { 0 };
  fixture_up(&f, GH_BACKGROUND_METHOD_FILE);
  g_autofree gchar *path = autostart_path(&f);
  g_autoptr(GError) error = NULL;
  Child *child = spawn_service(&f, SETTINGS_KEYFILE_ON);
  gh_test_spin_until(name_owned, child);
  gh_test_spin_until(file_exists, path);
  g_autofree gchar *entry = read_file(path);
  g_autofree gchar *expected = template_contents();
  g_assert_cmpstr(entry, ==, expected);
  g_autoptr(GVariant) done = g_dbus_connection_call_sync(bus.client, "org.nostr.Groundhog",
    "/org/nostr/Groundhog", "org.gtk.Actions", "Activate",
    g_variant_new("(sava{sv})", "quit", NULL, NULL), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
    &error);
  g_assert_no_error(error);
  wait_exit(child, 20);
  assert_clean_exit(child, &f);
  child_free(child, &f);

  /* Off: removed, and the service leaves by itself (no hold; GLib's 10 s
   * service wait), without anyone asking it to quit. */
  child = spawn_service(&f, SETTINGS_KEYFILE_OFF);
  gh_test_spin_until(file_missing, path);
  wait_exit(child, 30);
  assert_clean_exit(child, &f);
  child_free(child, &f);
  fixture_down(&f);
}

/* Two runs of this binary (two CTest entries):
 *  - default: everything on the shared private session bus (the stack, the
 *    fake portal, the real executable). This process never initializes GTK,
 *    whose portal and style singletons would keep that bus's connection.
 *  - --gui: the window-close tests, with no private bus; exits 77 without a
 *    display. */
int
main(int argc, char **argv)
{
  gboolean gui_mode = argc > 1 && g_str_equal(argv[1], "--gui");
  if (gui_mode) {
    argv[1] = argv[0];
    argv++;
    argc--;
  }
  /* Private XDG homes before anything asks GLib for them. */
  g_autofree gchar *xdg_root = g_dir_make_tmp("groundhog-background-xdg-XXXXXX", NULL);
  g_assert_nonnull(xdg_root);
  static const gchar *const vars[] = { "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME",
                                       "XDG_CONFIG_HOME" };
  for (guint i = 0; i < G_N_ELEMENTS(vars); i++) {
    g_autofree gchar *dir = g_build_filename(xdg_root, vars[i], NULL);
    g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
    g_setenv(vars[i], dir, TRUE);
  }
  /* No accessibility bus to find on a test bus. */
  g_setenv("GTK_A11Y", "none", TRUE);
  g_test_init(&argc, &argv, NULL);
#ifdef __APPLE__
  /* CLI test runners may not own a macOS WindowServer session. */
  display_available = g_strcmp0(g_getenv("GROUNDHOG_RUN_GUI_SMOKE"), "1") == 0;
#else
  display_available = (g_getenv("DISPLAY") && *g_getenv("DISPLAY")) ||
                      (g_getenv("WAYLAND_DISPLAY") && *g_getenv("WAYLAND_DISPLAY"));
#endif
  for (guint key = 1; key < GH_TEST_KEYS; key++) {
    npub[key] = gh_test_npub(key);
    hex[key] = gh_test_pub(key);
  }
  int status;
  if (gui_mode) {
    gui_available = display_available && gtk_init_check();
    if (!gui_available) {
      g_printerr("Groundhog background GUI tests skipped: no graphical display\n");
      status = 77;
      goto out;
    }
    groundhog_register_resource();
    /* Dialogs open and close without waiting on animations. */
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
    g_test_add_func("/groundhog/background-gui/no9-close-keeps-running",
                    test_no9_close_keeps_running);
    g_test_add_func("/groundhog/background-gui/no9-explanation-quit", test_no9_explanation_quit);
    g_test_add_func("/groundhog/background-gui/no9-close-background-off",
                    test_no9_close_background_off);
    status = g_test_run();
    goto out;
  }
  gh_test_bus_up(&bus);
  gh_test_signer_up(&bus, &signer);
  portal_up();
  /* The bus outlives every case: each re-arms the macOS EBADF tolerance
   * (W13 review, non-blocking #1; W13b review, non-blocking #6). */
  nostrc_test_bus_add_func("/groundhog/background/no9-service-mode", test_no9_service_mode);
  nostrc_test_bus_add_func("/groundhog/background/no9-process-service",
                           test_no9_process_service);
  nostrc_test_bus_add_func("/groundhog/background/no10-host-file", test_no10_host_file);
  nostrc_test_bus_add_func("/groundhog/background/no10-portal", test_no10_portal);
  nostrc_test_bus_add_func("/groundhog/background/no10-process-autostart",
                           test_no10_process_autostart);
  nostrc_test_bus_add_func("/groundhog/background/auto-host-uses-file",
                           test_auto_host_uses_file);
  nostrc_test_bus_add_func("/groundhog/background/no11-locked-start", test_no11_locked_start);
  nostrc_test_bus_add_func("/groundhog/background/no12-idle-timers", test_no12_idle_timers);
  status = g_test_run();
  portal_down();
  gh_test_signer_down(&bus, &signer);
  gh_test_bus_down(&bus);
out:
  for (guint key = 1; key < GH_TEST_KEYS; key++) {
    g_free(npub[key]);
    g_free(hex[key]);
  }
  gh_test_remove_tree(xdg_root);
  return status;
}
