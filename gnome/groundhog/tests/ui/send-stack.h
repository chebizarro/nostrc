/* One Groundhog account as the executable wires it, for the G13 GUI tests
 * (test_composer.c, test_e2e_dm.c): the account controller on the private
 * test bus (the mock signer answers for every test key), its relay lists,
 * the conversation model, the NIP-17 inbox, the store key over a FakeSecret
 * (H5), the durable outbox and the encrypted SQLCipher store in a private
 * directory, and a GhWindow with the account UI, the conversation list and
 * the send UI attached exactly as gh_app_services_attach_window() does. Each
 * stack has its own in-memory GSettings backend, so two accounts can run in
 * one process. The relay transports are the test's: recording ones, or NULL
 * for real sockets. A stack can be torn down and brought up again over the
 * same directory and FakeSecret: a restart. Header-only; include it once per
 * test executable, after gh-test-signer.h. */
#ifndef GH_TEST_SEND_STACK_H
#define GH_TEST_SEND_STACK_H

#include "fake-secret.h"
#include "gh-account-ui.h"
#include "gh-app-outbox.h"
#include "gh-composer.h"
#include "gh-conversation-list.h"
#include "gh-conversation-view.h"
#include "gh-send-ui.h"

#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>
#include <glib/gstdio.h>

#include "nostr-tag.h"
#include "nostr/nip59/nip59.h"

void groundhog_register_resource(void);

static gchar *stack_hex[GH_TEST_KEYS];
static gchar *stack_npub[GH_TEST_KEYS];

/* GTK first, on the session bus it was given (its accessibility bus and
 * portals live there), then the private test bus for the mock signer, which
 * is not the session bus: GhSigner opens its private senders to the address
 * DBUS_SESSION_BUS_ADDRESS names at each call, so that is pointed at it. FALSE
 * without a display. */
static G_GNUC_UNUSED gboolean
stack_gtk_and_bus_up(GhTestBus *bus)
{
#ifdef __APPLE__
  /* GTK 4.22's macOS accessibility backend has no announce: announcing to an
   * active window calls a NULL function. Linux (AT-SPI) is unaffected. */
  g_setenv("GTK_A11Y", "none", FALSE);
#endif
  if (!gtk_init_check())
    return FALSE;
  adw_init();
  bus->bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NOT_SESSION);
  nostrc_test_bus_up(bus->bus);
  bus->client = nostrc_test_bus_connect(bus->bus);
  bus->owner = nostrc_test_bus_connect(bus->bus);
  g_setenv("DBUS_SESSION_BUS_ADDRESS", nostrc_test_bus_get_address(bus->bus), TRUE);
  return TRUE;
}

static G_GNUC_UNUSED void
stack_keys_init(void)
{
  for (guint key = 1; key < GH_TEST_KEYS; key++) {
    stack_hex[key] = gh_test_pub(key);
    stack_npub[key] = gh_test_npub(key);
  }
}

static G_GNUC_UNUSED void
stack_keys_clear(void)
{
  for (guint key = 1; key < GH_TEST_KEYS; key++) {
    g_clear_pointer(&stack_hex[key], g_free);
    g_clear_pointer(&stack_npub[key], g_free);
  }
}

typedef struct {
  /* Kept across restarts. */
  guint key;                   /* the account's test key */
  gchar *root;
  gchar *data_dir;
  gchar *state_dir;
  FakeSecret *secret;
  GSettingsBackend *backend;
  GSettings *settings;
  GDBusConnection *bus;        /* the signer's bus; NULL: no bus */
  const GhRelayTransport *scope_transport;          /* NULL: gnostr relays */
  const GhRelayAuthTransport *auth_transport;
  gpointer scope_data;
  const GhRelayPublishTransport *publish_transport; /* NULL: gnostr relays */
  gpointer publish_data;
  GhInboxResolver *resolver;   /* recipient 10050s; NULL: a GhInboxLookup */
  GhClock *clock;              /* the store's (and outbox's); NULL: the system clock */
  /* One run. */
  GhAccountController *accounts;
  GhAccountRelays *relays;
  GhConversationStore *model;
  GhDmInbox *inbox;
  GhStoreKey *store_key;
  GhAppOutbox *sender;
  GhAccountStore *store;
  GhWindow *window;
} SendStack;

static GPtrArray *
stack_list(gpointer data, GError **error)
{
  (void)data;
  (void)error;
  GPtrArray *ids = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  for (guint key = 1; key <= 3; key++) {
    GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
    info->npub = g_strdup(stack_npub[key]);
    info->label = g_strdup_printf("Key %u", key);
    g_ptr_array_add(ids, info);
  }
  return ids;
}

/* The account (key), its settings on their own backend and a private
 * directory; discovery the relay-list sources. */
static G_GNUC_UNUSED void
send_stack_init(SendStack *s, guint key, GDBusConnection *bus, const gchar *const *discovery)
{
  g_autoptr(GError) error = NULL;
  s->key = key;
  s->bus = bus;
  s->root = g_dir_make_tmp("groundhog-send-XXXXXX", &error);
  g_assert_no_error(error);
  s->data_dir = g_build_filename(s->root, "data", NULL);
  s->state_dir = g_build_filename(s->root, "state", NULL);
  g_assert_cmpint(g_mkdir(s->data_dir, 0700), ==, 0);
  s->secret = fake_secret_new();
  s->backend = g_memory_settings_backend_new();
  s->settings = g_settings_new_with_backend("org.nostr.Groundhog", s->backend);
  g_settings_set_strv(s->settings, "discovery-relays", discovery);
  g_settings_set_string(s->settings, "signer-method", "auto");
  g_settings_set_string(s->settings, "current-npub", stack_npub[key]);
}

static gboolean
stack_listed(gpointer data)
{
  return gh_account_controller_get_state(data) != GH_ACCOUNT_STATE_DISCOVERING;
}

static gboolean
stack_store_settled(gpointer data)
{
  return gh_account_store_get_state(data) != GH_ACCOUNT_STORE_OPENING;
}

/* The account's services, as gh-app-services.c builds them, until its store
 * has settled (opened, or an explicit state such as LOCKED). */
static G_GNUC_UNUSED void
send_stack_up(SendStack *s)
{
  s->accounts = gh_account_controller_new_full(s->settings, s->bus, stack_list, NULL);
  gh_test_spin_until(stack_listed, s->accounts);
  s->relays = gh_account_relays_new(s->accounts, s->settings, s->scope_transport, s->scope_data);
  s->model = gh_conversation_store_new();
  s->inbox = gh_dm_inbox_new_with_storage(s->accounts, s->relays, s->model, s->scope_transport,
                                          s->auth_transport, s->scope_data);
  s->store_key = gh_store_key_new(GH_STORE_KEY_BACKEND(s->secret));
  GhAppOutboxConfig outbox = {
    .accounts = s->accounts,
    .account_relays = s->relays,
    .settings = s->settings,
    .inboxes = s->resolver,
    .transport = s->publish_transport,
    .transport_data = s->publish_data,
  };
  s->sender = gh_app_outbox_new(&outbox);
  GhAccountStoreConfig config = {
    .accounts = s->accounts,
    .store_key = s->store_key,
    .conversations = s->model,
    .inbox = s->inbox,
    .settings = s->settings,
    .data_dir = s->data_dir,
    .legacy_state_dir = s->state_dir,
    .clock = s->clock,
    .create_outbox = gh_app_outbox_create,
    .outbox_data = s->sender,
  };
  s->store = gh_account_store_new(&config);
  gh_test_spin_until(stack_store_settled, s->store);
  gh_test_run_until_idle();
}

/* The window, attached as the application does, at width x height. */
static G_GNUC_UNUSED void
send_stack_window(SendStack *s, gint width, gint height)
{
  s->window = gh_window_new(NULL);
  gh_account_ui_attach(s->window, s->accounts, s->settings);
  gh_conversation_list_attach(s->window, s->model, s->settings);
  GhSendUiConfig send = {
    .accounts = s->accounts,
    .conversations = s->model,
    .account_store = s->store,
    .inbox = s->inbox,
    .settings = s->settings,
  };
  gh_send_ui_attach(s->window, &send);
  gtk_window_set_default_size(GTK_WINDOW(s->window), width, height);
  gtk_window_present(GTK_WINDOW(s->window));
}

static G_GNUC_UNUSED GhComposer *
send_stack_composer(SendStack *s)
{
  return gh_content_page_get_composer(gh_window_get_content(s->window));
}

static G_GNUC_UNUSED GhConversationView *
send_stack_view(SendStack *s)
{
  return GH_CONVERSATION_VIEW(gh_content_page_get_view(gh_window_get_content(s->window)));
}

static gboolean
stack_is_null(gpointer data)
{
  return *(gpointer *)data == NULL;
}

/* Closes the window, then the services in gh-app-services.c's teardown
 * order (the account store first). */
static G_GNUC_UNUSED void
send_stack_down(SendStack *s)
{
  if (s->window) {
    gpointer weak = s->window;
    g_object_add_weak_pointer(G_OBJECT(s->window), &weak);
    gtk_window_destroy(GTK_WINDOW(g_steal_pointer(&s->window)));
    gh_test_spin_until(stack_is_null, &weak);
  }
  gh_test_release(g_steal_pointer(&s->store));
  g_clear_pointer(&s->sender, gh_app_outbox_free);
  gh_test_release(g_steal_pointer(&s->store_key));
  gh_test_release(g_steal_pointer(&s->inbox));
  g_clear_object(&s->model);
  gh_test_release(g_steal_pointer(&s->relays));
  gh_test_release(g_steal_pointer(&s->accounts));
  gh_test_run_until_idle();
}

static G_GNUC_UNUSED void
send_stack_clear(SendStack *s)
{
  if (s->accounts)
    send_stack_down(s);
  g_clear_object(&s->settings);
  g_clear_object(&s->backend);
  g_clear_object(&s->secret);
  g_clear_pointer(&s->clock, gh_clock_unref);
  gh_test_remove_tree(s->root);
  g_free(s->root);
  g_free(s->data_dir);
  g_free(s->state_dir);
}

static G_GNUC_UNUSED GhStoreConversations *
send_stack_drafts(SendStack *s)
{
  GhStoreConversations *drafts = gh_account_store_get_conversations(s->store);
  g_assert_nonnull(drafts);
  return drafts;
}

/* The stored draft of a room (NULL: none). */
static G_GNUC_UNUSED gchar *
send_stack_draft(SendStack *s, const gchar *room)
{
  gchar *draft = NULL;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_conversations_get_draft(send_stack_drafts(s), room, &draft, &error));
  g_assert_no_error(error);
  return draft;
}

/* ---- conversations ------------------------------------------------------------------ */

/* The canonical NIP-17 room of the given keys (0-terminated). */
static G_GNUC_UNUSED gchar *
stack_room(const guint *keys)
{
  g_autoptr(GPtrArray) members = g_ptr_array_new();
  for (const guint *k = keys; *k; k++) {
    gboolean seen = FALSE;
    for (guint i = 0; i < members->len; i++)
      seen = seen || g_str_equal(g_ptr_array_index(members, i), stack_hex[*k]);
    if (!seen)
      g_ptr_array_add(members, stack_hex[*k]);
  }
  g_ptr_array_sort_values(members, (GCompareFunc)strcmp);
  g_ptr_array_add(members, NULL);
  return g_strjoinv(",", (gchar **)members->pdata);
}

/* A kind-14 message from key `from`, p-tagged to `recipients`
 * (0-terminated), gift-wrapped (NIP-59) for key `wrap_to`. */
static G_GNUC_UNUSED gchar *
stack_craft_wrap(guint from, guint wrap_to, const guint *recipients, gint64 created_at,
                 const gchar *content)
{
  NostrEvent *rumor = nostr_event_new();
  nostr_event_set_kind(rumor, 14);
  nostr_event_set_pubkey(rumor, stack_hex[from]);
  nostr_event_set_created_at(rumor, created_at);
  nostr_event_set_content(rumor, content);
  NostrTags *tags = nostr_tags_new(0);
  for (const guint *r = recipients; *r; r++)
    nostr_tags_append(tags, nostr_tag_new("p", stack_hex[*r], NULL));
  nostr_event_set_tags(rumor, tags);
  rumor->id = nostr_event_get_id(rumor);
  char *rumor_json = nostr_event_serialize_compact(rumor);
  nostr_event_free(rumor);

  guint8 sk[32], pk[32];
  g_assert_true(nostr_hex2bin(sk, gh_test_secret[from], sizeof sk));
  g_assert_true(nostr_hex2bin(pk, stack_hex[wrap_to], sizeof pk));
  char *ciphertext = NULL;
  g_assert_cmpint(nostr_nip44_encrypt_v2(sk, pk, (const guint8 *)rumor_json,
                                          strlen(rumor_json), &ciphertext), ==, 0);
  free(rumor_json);
  NostrEvent *seal = nostr_event_new();
  nostr_event_set_kind(seal, 13);
  nostr_event_set_pubkey(seal, stack_hex[from]);
  nostr_event_set_content(seal, ciphertext);
  nostr_event_set_created_at(seal, created_at);
  nostr_event_set_tags(seal, nostr_tags_new(0));
  free(ciphertext);
  g_assert_cmpint(nostr_event_sign(seal, gh_test_secret[from]), ==, 0);

  guint8 ephemeral[32];
  g_assert_true(nostr_hex2bin(ephemeral, gh_test_secret[4], sizeof ephemeral));
  NostrEvent *wrap = nostr_nip59_wrap_with_key(seal, stack_hex[wrap_to], ephemeral);
  nostr_event_free(seal);
  g_assert_nonnull(wrap);
  char *json = nostr_event_serialize_compact(wrap);
  nostr_event_free(wrap);
  gchar *out = g_strdup(json);
  free(json);
  return out;
}

/* A signed kind-10050 of key naming urls (NULL-terminated). */
static G_GNUC_UNUSED gchar *
stack_inbox_list(guint key, gint64 created_at, const gchar *const *urls)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 10050);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, "");
  NostrTags *tags = nostr_tags_new(0);
  for (guint i = 0; urls[i]; i++)
    nostr_tags_append(tags, nostr_tag_new("relay", urls[i], NULL));
  nostr_event_set_tags(event, tags);
  g_assert_cmpint(nostr_event_sign(event, gh_test_secret[key]), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  gchar *out = g_strdup(json);
  free(json);
  return out;
}

/* Selects conversation in the sidebar list as a click does, which shows it
 * in the content page. */
static G_GNUC_UNUSED void
send_stack_select(SendStack *s, GhConversation *conversation)
{
  GhSidebarPage *sidebar = gh_window_get_sidebar(s->window);
  GtkSelectionModel *list = gtk_list_view_get_model(gh_sidebar_page_get_list(sidebar));
  guint n = g_list_model_get_n_items(G_LIST_MODEL(list));
  guint position = GTK_INVALID_LIST_POSITION;
  for (guint i = 0; i < n && position == GTK_INVALID_LIST_POSITION; i++) {
    g_autoptr(GObject) item = g_list_model_get_item(G_LIST_MODEL(list), i);
    if (item == G_OBJECT(conversation))
      position = i;
  }
  g_assert_cmpuint(position, !=, GTK_INVALID_LIST_POSITION);
  gtk_selection_model_select_item(list, position, TRUE);
  g_assert_true(gh_sidebar_page_get_selected(sidebar) == (gpointer)conversation);
  g_assert_true(gh_conversation_view_get_conversation(send_stack_view(s)) == conversation);
  gh_test_run_until_idle();
}

/* The newest message of a conversation (borrowed). */
static G_GNUC_UNUSED GhMessage *
stack_newest(GhConversation *conversation)
{
  guint n = g_list_model_get_n_items(G_LIST_MODEL(conversation));
  g_assert_cmpuint(n, >, 0);
  GhMessage *message = g_list_model_get_item(G_LIST_MODEL(conversation), n - 1);
  g_object_unref(message);
  return message;
}

/* The message of a conversation with this text (borrowed): messages of one
 * second sort by id, so the newest need not be the last one sent. */
static G_GNUC_UNUSED GhMessage *
stack_find(GhConversation *conversation, const gchar *content)
{
  GListModel *model = G_LIST_MODEL(conversation);
  for (guint i = 0; i < g_list_model_get_n_items(model); i++) {
    g_autoptr(GhMessage) message = g_list_model_get_item(model, i);
    if (g_strcmp0(gh_message_get_content(message), content) == 0)
      return message; /* the conversation holds it */
  }
  g_error("no message \"%s\" in the conversation", content);
  return NULL;
}

/* "Typing": text inserted at the cursor, as the text view does. */
static G_GNUC_UNUSED void
stack_type(GhComposer *composer, const gchar *text)
{
  gtk_text_buffer_insert_at_cursor(gtk_text_view_get_buffer(gh_composer_get_text_view(composer)),
                                   text, -1);
}

/* A key press on the composer's text view as its key controller sees it;
 * TRUE when the composer handled it (FALSE: the text view gets it). */
static G_GNUC_UNUSED gboolean
stack_press(GhComposer *composer, guint keyval, GdkModifierType mods)
{
  GtkWidget *text_view = GTK_WIDGET(gh_composer_get_text_view(composer));
  g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(text_view);
  for (guint i = 0; i < g_list_model_get_n_items(controllers); i++) {
    g_autoptr(GtkEventController) controller = g_list_model_get_item(controllers, i);
    if (g_strcmp0(gtk_event_controller_get_name(controller), "groundhog-composer-keys") != 0)
      continue;
    gboolean handled = FALSE;
    g_signal_emit_by_name(controller, "key-pressed", keyval, 0, mods, &handled);
    return handled;
  }
  g_error("the composer's key controller is missing");
  return FALSE;
}

/* ---- screenshots (opt-in evidence, GROUNDHOG_TEST_SCREENSHOTS=<dir>) --------------- */

static G_GNUC_UNUSED void
stack_save_png(GtkWidget *window, const char *path)
{
  GdkPaintable *paintable = gtk_widget_paintable_new(window);
  int width = gtk_widget_get_width(window), height = gtk_widget_get_height(window);
  GtkSnapshot *snapshot = gtk_snapshot_new();
  gdk_paintable_snapshot(paintable, snapshot, width, height);
  g_object_unref(paintable);
  g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node(snapshot);
  g_assert_nonnull(node);
  GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(window));
  g_autoptr(GdkTexture) texture =
    gsk_renderer_render_texture(renderer, node, &GRAPHENE_RECT_INIT(0, 0, width, height));
  g_assert_true(gdk_texture_save_to_png(texture, path));
  g_test_message("saved %s (%dx%d)", path, width, height);
}

#endif
