/* Conversation Info (privacy charter §7.4, §2.2, §3.7, §3.8, §5.2; §8.2 G19;
 * ST-9 via the UI, NO-7).
 *
 * Default mode: GhConversationActions with the real GhNotifier sending
 * through GLib's GTK backend to the fake org.gtk.Notifications (H4) on the
 * shared private bus: mute, block and forget withdraw what shows for the
 * conversation, nothing more is notified for it, and each survives a restart
 * of a real SQLCipher store; forget keeps its tombstone (ST-9); verification
 * marks persist. Time moves only with the fake GhClock (H6).
 *
 * --gui: the dialog itself on a real store and conversation model (no bus,
 * no notifier): Forget on This Device through its confirmation (ST-9),
 * mute kept in the database across a restart and absent from GSettings
 * (NO-7), block through its confirmation, Verify Key (the safety code and
 * the local mark, across a restart), the disappearing timer, profile text
 * that is never markup, a closed store, and the content page's info button.
 * It exits 77 without a display. With GROUNDHOG_TEST_SCREENSHOTS=<dir> it
 * also renders the dialog to <dir>/groundhog-g19-*.png. */
#include "gh-conversation-actions.h"
#include "gh-conversation-info-dialog.h"
#include "gh-conversation-private.h"
#include "gh-conversation-list.h"
#include "gh-conversation-view.h"
#include "gh-timeline-row.h"
#include "gh-expiry.h"

#include "nostrc-test-gdk-frame.h"
#include "gh-notifier.h"
#include "gh-privacy-summary.h"
#include "gh-store-contacts.h"
#include "gh-store-conversations.h"
#include "fake-gtk-notifications.h"
#include "gh-test-dialog.h"
#include "nostrc-test-bus.h"

#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr-utils.h"
#include "nostr/nip19/nip19.h"

#include <adwaita.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

#define T0 ((gint64)1790000000)
#define APP_SCHEMA "org.nostr.Groundhog"

void groundhog_register_resource(void);

static gchar *ACCOUNT, *PEER[3];
static NostrcTestBus *bus;
static FakeGtkNotifications *fake;
static guint app_serial;

static gchar *
hex_of(const gchar *seed)
{
  return g_compute_checksum_for_string(G_CHECKSUM_SHA256, seed, -1);
}

static gchar *
npub_of(const gchar *hex)
{
  guint8 bytes[32];
  char *npub = NULL;
  g_assert_true(nostr_hex2bin(bytes, hex, sizeof bytes));
  g_assert_cmpint(nostr_nip19_encode_npub(bytes, &npub), ==, 0);
  gchar *out = g_strdup(npub);
  free(npub);
  return out;
}

/* ---- rumors ------------------------------------------------------------------------- */

static GhMessage *
message_new(const gchar *author, const gchar *to, gint64 created_at, const gchar *content)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 14);
  nostr_event_set_pubkey(event, author);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, content);
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("p", to, NULL));
  nostr_event_set_tags(event, tags);
  event->id = nostr_event_get_id(event);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  g_autoptr(GError) error = NULL;
  GhMessage *message = gh_message_new_from_rumor(ACCOUNT, json, &error);
  free(json);
  g_assert_no_error(error);
  return message;
}

static gchar *
random_hex(void)
{
  guint8 bytes[32];
  for (guint i = 0; i < sizeof bytes; i++)
    bytes[i] = (guint8)g_random_int_range(0, 256);
  GString *hex = g_string_new(NULL);
  for (guint i = 0; i < sizeof bytes; i++)
    g_string_append_printf(hex, "%02x", bytes[i]);
  return g_string_free(hex, FALSE);
}

/* ---- an account's store across restarts --------------------------------------------- */

typedef struct {
  gchar *dir;
  GBytes *key;
  gchar *store_id;
  GhClock *clock;
  GhStore *store;
  GhConversationStore *model;
  GhStoreConversations *conversations;
  GhExpiry *expiry;
  gboolean ephemeral; /* "Continue Without Saving Messages": in memory only */
} Account;

/* Opens the account's store (creating it the first time) as the
 * application does on start: a fresh model with the delegate attached
 * (restoring the stored rooms) and the expiry engine. */
static void
account_open(Account *a)
{
  g_autoptr(GError) error = NULL;
  GhStoreConfig config = { .data_dir = a->dir, .account_pubkey = ACCOUNT, .clock = a->clock };
  a->store = a->ephemeral ? gh_store_open_ephemeral(ACCOUNT, a->clock, &error)
                          : gh_store_open_with_key(&config, a->key, a->store_id,
                                                   GH_STORE_OPEN_CREATE, &error);
  g_assert_no_error(error);
  a->model = gh_conversation_store_new();
  a->conversations = gh_store_conversations_new(a->store);
  g_assert_true(gh_store_conversations_attach(a->conversations, a->model, 0, &error));
  g_assert_no_error(error);
  GhExpiryConfig expiry = { .store = a->store, .conversations = a->conversations };
  a->expiry = gh_expiry_new(&expiry);
}

static void
account_close(Account *a)
{
  if (!a->store)
    return;
  g_object_run_dispose(G_OBJECT(a->expiry));
  g_clear_object(&a->expiry);
  gh_store_conversations_close(a->conversations);
  g_clear_object(&a->conversations);
  gh_conversation_store_set_account(a->model, NULL, NULL, NULL, NULL);
  g_clear_object(&a->model);
  g_clear_pointer(&a->store, gh_store_close);
}

static void
account_restart(Account *a)
{
  account_close(a);
  account_open(a);
}

static void
account_init(Account *a)
{
  memset(a, 0, sizeof *a);
  g_autoptr(GError) error = NULL;
  a->dir = g_dir_make_tmp("groundhog-g19-XXXXXX", &error);
  g_assert_no_error(error);
  guint8 raw[GH_STORE_KEY_SIZE];
  for (guint i = 0; i < sizeof raw; i++)
    raw[i] = (guint8)g_random_int_range(0, 256);
  a->key = g_bytes_new(raw, sizeof raw);
  a->store_id = g_uuid_string_random();
  a->clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
  account_open(a);
}

static void
remove_tree(const gchar *path)
{
  GDir *dir = g_dir_open(path, 0, NULL);
  if (dir) {
    const gchar *name;
    while ((name = g_dir_read_name(dir))) {
      g_autofree gchar *child = g_build_filename(path, name, NULL);
      if (g_file_test(child, G_FILE_TEST_IS_DIR) && !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
        remove_tree(child);
      else
        g_unlink(child);
    }
    g_dir_close(dir);
  }
  g_rmdir(path);
}

static void
account_clear(Account *a)
{
  account_close(a);
  remove_tree(a->dir);
  g_free(a->dir);
  g_bytes_unref(a->key);
  g_free(a->store_id);
  gh_clock_unref(a->clock);
}

static gint64
now(Account *a)
{
  return gh_clock_get_unix(a->clock);
}

/* A message from peer to the account, delivered in a fresh wrap. */
static GhConversationAddResult
receive_full(Account *a, const gchar *peer, gint64 created_at, const gchar *text,
             GhMessage **out_message)
{
  g_autoptr(GhMessage) message = message_new(peer, ACCOUNT, created_at, text);
  g_autofree gchar *wrap = random_hex();
  g_autoptr(GError) error = NULL;
  GhConversationAddResult result = gh_conversation_store_admit(a->model, message, wrap, &error);
  g_assert_no_error(error);
  if (out_message)
    *out_message = g_object_ref(message);
  return result;
}

static GhConversation *
receive(Account *a, const gchar *peer, gint64 created_at, const gchar *text)
{
  g_autoptr(GhMessage) message = NULL;
  g_assert_cmpint(receive_full(a, peer, created_at, text, &message), ==, GH_CONVERSATION_ADD_NEW);
  GhConversation *conversation =
    gh_conversation_store_lookup(a->model, gh_message_get_room_id(message));
  g_assert_nonnull(conversation);
  return conversation;
}

static gchar *
room_of(const gchar *peer)
{
  g_autoptr(GhMessage) message = message_new(peer, ACCOUNT, T0, "room");
  return g_strdup(gh_message_get_room_id(message));
}

static GhStoreNotifyState
notify_state(Account *a, const gchar *room)
{
  GhStoreNotifyState state = { 0 };
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_conversations_get_notify_state(a->conversations, room, &state, &error));
  g_assert_no_error(error);
  return state;
}

static gint64
verified(Account *a, const gchar *pubkey)
{
  gint64 at = -1;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_contacts_get_verified(a->store, pubkey, &at, &error));
  g_assert_no_error(error);
  return at;
}

/* NO-7: nothing of a conversation is in GSettings. */
static void
assert_settings_clean(const gchar *room, const gchar *peer)
{
  g_autoptr(GSettings) settings = g_settings_new(APP_SCHEMA);
  g_autoptr(GSettingsSchema) schema = NULL;
  g_object_get(settings, "settings-schema", &schema, NULL);
  g_auto(GStrv) keys = g_settings_schema_list_keys(schema);
  g_autoptr(GString) dump = g_string_new(NULL);
  for (guint i = 0; keys[i]; i++) {
    g_autoptr(GVariant) value = g_settings_get_value(settings, keys[i]);
    g_autofree gchar *printed = g_variant_print(value, FALSE);
    g_string_append_printf(dump, "%s=%s\n", keys[i], printed);
  }
  g_autofree gchar *npub = npub_of(peer);
  g_assert_null(strstr(dump->str, room));
  g_assert_null(strstr(dump->str, peer));
  g_assert_null(strstr(dump->str, npub + 5));
  g_assert_null(strstr(dump->str, "mute"));
}

/* ==== Default mode: actions, notifier and store =========================================== */

typedef struct {
  Account account;
  GApplication *app;
  GSettings *settings;
  GhNotifier *notifier;
} Fixture;

static gboolean
room_state(gpointer data, const gchar *room_id, GhNotifierRoomState *state)
{
  Fixture *f = data;
  GhStoreNotifyState stored = { 0 };
  if (!f->account.conversations ||
      !gh_store_conversations_get_notify_state(f->account.conversations, room_id, &stored, NULL))
    return FALSE;
  state->muted_until = stored.muted_until;
  state->blocked = stored.blocked;
  return TRUE;
}

static void
fixture_up(Fixture *f)
{
  memset(f, 0, sizeof *f);
  account_init(&f->account);
  fake_gtk_notifications_set_clock(fake, f->account.clock);
  fake_gtk_notifications_clear(fake);
  f->settings = g_settings_new(APP_SCHEMA);
  g_settings_set_boolean(f->settings, "notifications-enabled", TRUE);
  /* The default level: one app-wide notification, no names (NO-1). */
  g_settings_set_string(f->settings, "notification-privacy", "hidden");
  g_settings_set_boolean(f->settings, "sound-enabled", FALSE);
  g_autofree gchar *id = g_strdup_printf("org.nostr.GroundhogTest.Info%u", ++app_serial);
  f->app = g_application_new(id, G_APPLICATION_DEFAULT_FLAGS);
  g_autoptr(GError) error = NULL;
  g_assert_true(g_application_register(f->app, NULL, &error));
  g_assert_no_error(error);
  GhNotifierConfig config = {
    .settings = f->settings,
    .conversations = f->account.model,
    .clock = f->account.clock,
    .room_state = room_state,
    .room_state_data = f,
  };
  f->notifier = gh_notifier_new(f->app, &config);
}

static void
fixture_down(Fixture *f)
{
  g_object_run_dispose(G_OBJECT(f->notifier));
  g_clear_object(&f->notifier);
  account_clear(&f->account);
  g_clear_object(&f->app);
  g_clear_object(&f->settings);
}

static void
sync_calls(Fixture *f)
{
  fake_gtk_notifications_sync(fake, g_application_get_dbus_connection(f->app));
}

/* Runs what is due within delta_us of the fake clock, then records. */
static void
run_for(Fixture *f, gint64 delta_us)
{
  GhClock *clock = f->account.clock;
  gint64 target = gh_clock_get_monotonic_time(clock) + delta_us;
  gint64 due;
  while ((due = gh_clock_fake_get_next_deadline(clock)) >= 0 && due <= target) {
    gh_clock_fake_advance(clock, MAX(due - gh_clock_get_monotonic_time(clock), 0));
    sync_calls(f);
  }
  gh_clock_fake_advance(clock, target - gh_clock_get_monotonic_time(clock));
  sync_calls(f);
}

static void
settle(Fixture *f)
{
  run_for(f, 1);
}

/* The hidden level's one notification. */
static const gchar *
shown_id(void)
{
  return GH_NOTIFIER_ID_MESSAGES;
}

static void
receive_shown(Fixture *f, const gchar *peer, const gchar *text)
{
  receive(&f->account, peer, now(&f->account), text);
  settle(f);
  g_assert_nonnull(fake_gtk_notifications_lookup(fake, shown_id()));
}

static void
test_mute_withdraws_and_persists(void)
{
  Fixture f;
  fixture_up(&f);
  Account *a = &f.account;
  g_autofree gchar *room = room_of(PEER[0]);
  receive_shown(&f, PEER[0], "hello");

  /* Muting withdraws what shows; later messages stay quiet. */
  gint64 until = now(a) + 8 * 3600;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_conversation_actions_mute(a->conversations, f.notifier, room, until, &error));
  g_assert_no_error(error);
  sync_calls(&f);
  g_assert_null(fake_gtk_notifications_lookup(fake, shown_id()));
  g_assert_cmpuint(fake_gtk_notifications_count(fake, FALSE, shown_id()), ==, 1);
  fake_gtk_notifications_clear(fake);
  receive(a, PEER[0], now(a), "while muted");
  run_for(&f, 5 * G_USEC_PER_SEC);
  g_assert_cmpuint(fake_gtk_notifications_count(fake, TRUE, NULL), ==, 0);

  /* NO-7: in the database, across a restart; nothing in GSettings. */
  g_assert_cmpint(notify_state(a, room).muted_until, ==, until);
  account_restart(a);
  g_assert_cmpint(notify_state(a, room).muted_until, ==, until);
  assert_settings_clean(room, PEER[0]);

  /* Unmuting withdraws nothing; "always" is kept as such. */
  g_assert_true(gh_conversation_actions_mute(a->conversations, NULL, room, 0, &error));
  g_assert_cmpint(notify_state(a, room).muted_until, ==, 0);
  g_assert_true(gh_conversation_actions_mute(a->conversations, NULL, room,
                                             GH_STORE_CONVERSATIONS_MUTED_ALWAYS, &error));
  account_restart(a);
  g_assert_cmpint(notify_state(a, room).muted_until, ==, GH_STORE_CONVERSATIONS_MUTED_ALWAYS);
  /* A room that is not stored cannot be muted. */
  g_autofree gchar *unknown = room_of(PEER[2]);
  g_assert_false(gh_conversation_actions_mute(a->conversations, NULL, unknown, until, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND);
  fixture_down(&f);
}

static void
test_block(void)
{
  Fixture f;
  fixture_up(&f);
  Account *a = &f.account;
  g_autofree gchar *room = room_of(PEER[0]);
  receive_shown(&f, PEER[0], "hello");
  receive(a, PEER[1], now(a) - 60, "another conversation");

  g_autoptr(GError) error = NULL;
  g_assert_true(gh_conversation_actions_block(a->conversations, f.notifier, room, TRUE, &error));
  g_assert_no_error(error);
  sync_calls(&f);
  /* Withdrawn, out of the list, blocked in the database. */
  g_assert_null(fake_gtk_notifications_lookup(fake, shown_id()));
  g_assert_null(gh_conversation_store_lookup(a->model, room));
  g_assert_true(notify_state(a, room).blocked);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(a->model)), ==, 1);

  /* New messages in it are recorded as seen only: not stored, never
   * notified. */
  fake_gtk_notifications_clear(fake);
  g_autoptr(GhMessage) hidden = NULL;
  g_assert_cmpint(receive_full(a, PEER[0], now(a), "while blocked", &hidden), ==,
                  GH_CONVERSATION_ADD_HIDDEN);
  run_for(&f, 5 * G_USEC_PER_SEC);
  g_assert_cmpuint(fake_gtk_notifications_count(fake, TRUE, NULL), ==, 0);
  g_assert_null(gh_conversation_store_lookup(a->model, room));

  /* A restart does not list it again. */
  account_restart(a);
  g_assert_null(gh_conversation_store_lookup(a->model, room));
  g_assert_true(notify_state(a, room).blocked);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(a->model)), ==, 1);

  /* Unblocking lists it again with its history from before the block
   * (what arrived meanwhile was never stored), as a message request:
   * nobody here wrote in it, and unblocking is not accepting. */
  g_assert_true(gh_conversation_actions_block(a->conversations, NULL, room, FALSE, &error));
  g_assert_no_error(error);
  GhConversation *back = gh_conversation_store_lookup(a->model, room);
  g_assert_nonnull(back);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(back)), ==, 1);
  g_assert_null(gh_conversation_lookup_message(back, gh_message_get_rumor_id(hidden)));
  g_assert_true(gh_conversation_get_is_request(back));
  g_assert_false(notify_state(a, room).blocked);
  account_restart(a);
  g_assert_nonnull(gh_conversation_store_lookup(a->model, room));

  g_autofree gchar *unknown = room_of(PEER[2]);
  g_assert_false(gh_conversation_actions_block(a->conversations, NULL, unknown, TRUE, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND);
  fixture_down(&f);
}

/* ST-9 through the action Conversation Info runs. */
static void
test_forget(void)
{
  Fixture f;
  fixture_up(&f);
  Account *a = &f.account;
  g_autofree gchar *room = room_of(PEER[0]);
  g_autoptr(GhMessage) old = NULL;
  g_assert_cmpint(receive_full(a, PEER[0], now(a) - 3600, "old", &old), ==,
                  GH_CONVERSATION_ADD_NEW);
  receive_shown(&f, PEER[0], "hello");

  g_autoptr(GError) error = NULL;
  g_assert_true(gh_conversation_actions_forget(a->conversations, f.notifier, room, &error));
  g_assert_no_error(error);
  sync_calls(&f);
  g_assert_null(fake_gtk_notifications_lookup(fake, shown_id()));
  g_assert_null(gh_conversation_store_lookup(a->model, room));

  /* Backfill of what came before cannot bring it back: the same rumor in
   * another wrap, nor an older one never seen. */
  run_for(&f, 60 * G_USEC_PER_SEC);
  g_autoptr(GhMessage) again = g_object_ref(old);
  g_autofree gchar *wrap = random_hex();
  g_assert_cmpint(gh_conversation_store_admit(a->model, again, wrap, &error), ==,
                  GH_CONVERSATION_ADD_DUPLICATE);
  g_assert_cmpint(receive_full(a, PEER[0], T0 - 7200, "older, never seen", NULL), ==,
                  GH_CONVERSATION_ADD_HIDDEN);
  g_assert_null(gh_conversation_store_lookup(a->model, room));
  account_restart(a);
  g_assert_null(gh_conversation_store_lookup(a->model, room));

  /* A new message starts it again with only that message. */
  GhConversation *fresh = receive(a, PEER[0], now(a), "new start");
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(fresh)), ==, 1);
  account_restart(a);
  fresh = gh_conversation_store_lookup(a->model, room);
  g_assert_nonnull(fresh);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(fresh)), ==, 1);
  fixture_down(&f);
}

static void
test_verified_marks(void)
{
  Account a;
  account_init(&a);
  g_autoptr(GError) error = NULL;
  /* Schema v3: contacts.verified_at. */
  g_assert_cmpint(gh_store_contacts_set_verified(a.store, PEER[0], T0, &error), ==, TRUE);
  g_assert_no_error(error);
  g_assert_cmpint(verified(&a, PEER[0]), ==, T0);
  g_assert_cmpint(verified(&a, PEER[1]), ==, 0);
  account_restart(&a);
  g_assert_cmpint(verified(&a, PEER[0]), ==, T0);
  /* Clearing a mark never creates a row; clearing twice is fine. */
  g_assert_true(gh_store_contacts_set_verified(a.store, PEER[0], 0, &error));
  g_assert_true(gh_store_contacts_set_verified(a.store, PEER[1], 0, &error));
  g_assert_no_error(error);
  account_restart(&a);
  g_assert_cmpint(verified(&a, PEER[0]), ==, 0);
  sqlite3_stmt *stmt = NULL;
  g_assert_cmpint(sqlite3_prepare_v2(gh_store_get_db(a.store),
                                     "SELECT count(*) FROM contacts", -1, &stmt, NULL), ==,
                  SQLITE_OK);
  g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
  g_assert_cmpint(sqlite3_column_int(stmt, 0), ==, 1);
  sqlite3_finalize(stmt);
  /* Malformed keys and times are refused. */
  gint64 at = 0;
  g_assert_false(gh_store_contacts_get_verified(a.store, "ABC", &at, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  g_clear_error(&error);
  g_autofree gchar *upper = g_ascii_strup(PEER[0], -1);
  g_assert_false(gh_store_contacts_set_verified(a.store, upper, T0, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  g_clear_error(&error);
  g_assert_false(gh_store_contacts_set_verified(a.store, PEER[0], -5, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  account_clear(&a);
}

/* ==== --gui: the dialog ==================================================================== */

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

/* As test_preferences.c: a width x height content area even where the
 * display draws the client-side border inside the default size (Xvfb). */
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

static GtkWidget *
find_button(GtkWidget *widget, const char *label, gboolean mapped_only)
{
  if (GTK_IS_BUTTON(widget) && (!mapped_only || gtk_widget_get_mapped(widget)) &&
      g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), label) == 0)
    return widget;
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = find_button(c, label, mapped_only);
    if (found)
      return found;
  }
  return NULL;
}

/* The first descendant row of type (by name) whose title is title (NULL:
 * any). */
static AdwActionRow *
find_row(GtkWidget *widget, const char *type_name, const char *title)
{
  if (ADW_IS_ACTION_ROW(widget) && (!type_name || g_str_equal(G_OBJECT_TYPE_NAME(widget), type_name)) &&
      (!title ||
       g_strcmp0(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(widget)), title) == 0))
    return ADW_ACTION_ROW(widget);
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    AdwActionRow *found = find_row(c, type_name, title);
    if (found)
      return found;
  }
  return NULL;
}

/* The first descendant label showing text (markup already applied). */
static GtkLabel *
find_label(GtkWidget *widget, const char *text)
{
  if (GTK_IS_LABEL(widget) && g_strcmp0(gtk_label_get_text(GTK_LABEL(widget)), text) == 0)
    return GTK_LABEL(widget);
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GtkLabel *found = find_label(c, text);
    if (found)
      return found;
  }
  return NULL;
}

typedef struct {
  Account account;
  GhWindow *window;
  GhConversationInfoDialog *dialog; /* a reference */
  gboolean closed;
} Gui;

static void
profile(const gchar *pubkey, GhConversationInfoProfile *out, gpointer data)
{
  (void)data;
  if (g_strcmp0(pubkey, PEER[0]) == 0) {
    out->name = "Alice";
    out->nip05 = "alice@example.com";
  } else if (g_strcmp0(pubkey, PEER[1]) == 0) {
    /* Someone's kind-0 name is text, never markup. */
    out->name = "<b>Eve</b> & <i>co";
  }
}

static GhConversationInfoServices
services_of(Account *a)
{
  return (GhConversationInfoServices){
    .model = a->model,
    .conversations = a->conversations,
    .store = a->store,
    .expiry = a->expiry,
    .profile = profile,
  };
}

static gboolean
services_func(GhConversationInfoServices *services, gpointer data)
{
  *services = services_of(data);
  return TRUE;
}

static gpointer
child(Gui *g, const char *name)
{
  GObject *object = gtk_widget_get_template_child(GTK_WIDGET(g->dialog),
                                                  GH_TYPE_CONVERSATION_INFO_DIALOG, name);
  g_assert_nonnull(object);
  return object;
}

static void
on_closed(AdwDialog *dialog, Gui *g)
{
  (void)dialog;
  g->closed = TRUE;
}

static gboolean
is_closed(gpointer data)
{
  return ((Gui *)data)->closed;
}

static void
gui_window(Gui *g, int width, int height, GhConversation *show)
{
  g->window = gh_window_new(NULL);
  gh_conversation_list_attach(g->window, g->account.model, NULL);
  present_window(GTK_WINDOW(g->window), width, height);
  if (show)
    g_assert_true(gh_window_open_item(g->window, show));
  drain_idle();
}

static void
gui_open(Gui *g, GhConversation *conversation)
{
  GhConversationInfoServices services = services_of(&g->account);
  g->closed = FALSE;
  g->dialog = g_object_ref_sink(gh_conversation_info_dialog_new(conversation, &services));
  g_signal_connect(g->dialog, "closed", G_CALLBACK(on_closed), g);
  adw_dialog_present(ADW_DIALOG(g->dialog), GTK_WIDGET(g->window));
  spin_until(gh_test_dialog_shown, g->dialog);
  drain_idle();
}

static void
gui_close(Gui *g)
{
  if (g->dialog) {
    if (!g->closed)
      adw_dialog_force_close(ADW_DIALOG(g->dialog));
    g_signal_handlers_disconnect_by_data(g->dialog, g);
    g_clear_object(&g->dialog);
  }
  if (g->window)
    gtk_window_destroy(GTK_WINDOW(g->window));
  g->window = NULL;
  drain_idle();
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
  GtkWidget *root;
  const char *label;
  GtkWidget *button;
} ButtonWait;

static gboolean
button_shown(gpointer data)
{
  ButtonWait *wait = data;
  wait->button = find_button(wait->root, wait->label, TRUE);
  return wait->button != NULL;
}

/* Runs action, whose confirmation is alert_name, and answers it with the
 * response button labelled label: the path a user takes. */
static void
confirm(Gui *g, const char *action, const char *alert_name, const char *label)
{
  RootWait wait = { child(g, alert_name), GTK_WINDOW(g->window) };
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(g->dialog), action, NULL));
  spin_until(alert_shown, &wait);
  ButtonWait shown = { wait.widget, label, NULL };
  spin_until(button_shown, &shown);
  g_signal_emit_by_name(shown.button, "clicked");
  spin_until(alert_gone, &wait);
  drain_idle();
}

/* ST-9 via the UI: Forget on This Device through its confirmation. */
static void
test_gui_forget(void)
{
  Gui g = { 0 };
  account_init(&g.account);
  Account *a = &g.account;
  g_autofree gchar *room = room_of(PEER[0]);
  g_autoptr(GhMessage) old = NULL;
  g_assert_cmpint(receive_full(a, PEER[0], T0 - 3600, "old", &old), ==, GH_CONVERSATION_ADD_NEW);
  GhConversation *conversation = receive(a, PEER[0], T0 - 60, "hello");
  gui_window(&g, 900, 760, conversation);
  gui_open(&g, conversation);

  /* The copy is honest: the others and relays keep their copies. */
  AdwAlertDialog *alert = child(&g, "forget_dialog");
  g_assert_cmpstr(adw_alert_dialog_get_heading(alert), ==, "Forget This Conversation?");
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(alert), "Alice still has their copy"));
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(alert), "may remain on relays"));
  g_assert_cmpint(adw_alert_dialog_get_response_appearance(alert, "forget-confirm"), ==,
                  ADW_RESPONSE_DESTRUCTIVE);
  g_assert_cmpstr(adw_alert_dialog_get_default_response(alert), ==, "forget-cancel");
  g_assert_cmpstr(adw_alert_dialog_get_close_response(alert), ==, "forget-cancel");

  /* Cancel changes nothing. */
  confirm(&g, "info.forget", "forget_dialog", "_Cancel");
  g_assert_false(g.closed);
  g_assert_true(gh_conversation_store_lookup(a->model, room) == conversation);

  confirm(&g, "info.forget", "forget_dialog", "_Forget");
  spin_until(is_closed, &g);
  g_assert_null(gh_conversation_store_lookup(a->model, room));
  g_assert_false(gh_content_page_get_conversation_shown(gh_window_get_content(g.window)));
  gui_close(&g);

  /* Relay backfill cannot bring it back. */
  gh_clock_fake_advance(a->clock, 60 * G_USEC_PER_SEC);
  g_autofree gchar *wrap = random_hex();
  g_autoptr(GError) error = NULL;
  g_assert_cmpint(gh_conversation_store_admit(a->model, old, wrap, &error), ==,
                  GH_CONVERSATION_ADD_DUPLICATE);
  g_assert_cmpint(receive_full(a, PEER[0], T0 - 7200, "never seen, older", NULL), ==,
                  GH_CONVERSATION_ADD_HIDDEN);
  account_restart(a);
  g_assert_null(gh_conversation_store_lookup(a->model, room));
  /* A new message starts it again with only that message. */
  GhConversation *fresh = receive(a, PEER[0], now(a), "new start");
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(fresh)), ==, 1);
  account_restart(a);
  fresh = gh_conversation_store_lookup(a->model, room);
  g_assert_nonnull(fresh);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(fresh)), ==, 1);
  account_clear(a);
}

/* NO-7 via the UI: the mute is in the database and survives a restart. */
static void
test_gui_mute(void)
{
  Gui g = { 0 };
  account_init(&g.account);
  Account *a = &g.account;
  g_autofree gchar *room = room_of(PEER[0]);
  GhConversation *conversation = receive(a, PEER[0], T0 - 60, "hello");
  gui_window(&g, 900, 760, conversation);
  gui_open(&g, conversation);
  AdwActionRow *row = child(&g, "mute_row");
  g_assert_cmpstr(adw_action_row_get_subtitle(row), ==, "Notifications are on");
  g_assert_cmpstr(gtk_menu_button_get_label(child(&g, "mute_button")), ==, "Mute");

  /* The menu offers the charter's durations (§5.2 N8) and Unmute. */
  GMenuModel *menu = gtk_menu_button_get_menu_model(child(&g, "mute_button"));
  GMenuModel *durations = g_menu_model_get_item_link(menu, 0, G_MENU_LINK_SECTION);
  g_assert_cmpint(g_menu_model_get_n_items(durations), ==, 4);
  static const char *const targets[] = { "3600", "28800", "604800", "always" };
  for (int i = 0; i < 4; i++) {
    g_autoptr(GVariant) target =
      g_menu_model_get_item_attribute_value(durations, i, G_MENU_ATTRIBUTE_TARGET, NULL);
    g_assert_cmpstr(g_variant_get_string(target, NULL), ==, targets[i]);
  }
  g_object_unref(durations);

  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(g.dialog), "info.mute", "s", "28800"));
  g_assert_cmpint(notify_state(a, room).muted_until, ==, T0 + 28800);
  g_assert_true(g_str_has_prefix(adw_action_row_get_subtitle(row), "Muted until "));
  g_assert_cmpstr(gtk_menu_button_get_label(child(&g, "mute_button")), ==, "Change");
  gui_close(&g);

  account_restart(a);
  g_assert_cmpint(notify_state(a, room).muted_until, ==, T0 + 28800);
  assert_settings_clean(room, PEER[0]);

  /* Reopened after the restart, it shows the stored mute. */
  conversation = gh_conversation_store_lookup(a->model, room);
  gui_window(&g, 900, 760, conversation);
  gui_open(&g, conversation);
  row = child(&g, "mute_row");
  g_assert_true(g_str_has_prefix(adw_action_row_get_subtitle(row), "Muted until "));
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(g.dialog), "info.mute", "s", "always"));
  g_assert_cmpint(notify_state(a, room).muted_until, ==, GH_STORE_CONVERSATIONS_MUTED_ALWAYS);
  g_assert_cmpstr(adw_action_row_get_subtitle(row), ==,
                  "Muted until you turn notifications back on");
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(g.dialog), "info.unmute", NULL));
  g_assert_cmpint(notify_state(a, room).muted_until, ==, 0);
  g_assert_cmpstr(adw_action_row_get_subtitle(row), ==, "Notifications are on");
  gui_close(&g);
  account_clear(a);
}

typedef struct {
  GtkWidget *root;
} UndoWait;

static void
test_gui_block(void)
{
  Gui g = { 0 };
  account_init(&g.account);
  Account *a = &g.account;
  g_autofree gchar *room = room_of(PEER[0]);
  GhConversation *conversation = receive(a, PEER[0], T0 - 60, "hello");
  receive(a, PEER[2], T0 - 120, "someone else");
  gui_window(&g, 900, 760, conversation);
  gui_open(&g, conversation);
  AdwAlertDialog *alert = child(&g, "block_dialog");
  g_assert_cmpstr(adw_alert_dialog_get_heading(alert), ==, "Block Alice?");
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(alert), "Nobody is told"));
  /* It says how the block is lifted, and that is what lifts it (G18). */
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(alert),
                          "To unblock it, choose Undo, or later start a new message to Alice."));
  g_assert_cmpstr(adw_alert_dialog_get_close_response(alert), ==, "block-cancel");

  confirm(&g, "info.block", "block_dialog", "_Block");
  spin_until(is_closed, &g);
  g_assert_null(gh_conversation_store_lookup(a->model, room));
  g_assert_true(notify_state(a, room).blocked);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(a->model)), ==, 1);

  /* The window offers Undo, which lists it again. */
  ButtonWait undo = { GTK_WIDGET(g.window), "_Undo", NULL };
  spin_until(button_shown, &undo);
  g_signal_emit_by_name(undo.button, "clicked");
  drain_idle();
  g_assert_nonnull(gh_conversation_store_lookup(a->model, room));
  g_assert_false(notify_state(a, room).blocked);

  /* Blocked again, it stays out of the list across a restart. */
  conversation = gh_conversation_store_lookup(a->model, room);
  g_assert_true(gh_window_open_item(g.window, conversation));
  g_clear_object(&g.dialog);
  gui_open(&g, conversation);
  confirm(&g, "info.block", "block_dialog", "_Block");
  spin_until(is_closed, &g);
  gui_close(&g);
  account_restart(a);
  g_assert_null(gh_conversation_store_lookup(a->model, room));
  g_assert_true(notify_state(a, room).blocked);
  account_clear(a);
}

static void
test_gui_verify(void)
{
  Gui g = { 0 };
  account_init(&g.account);
  Account *a = &g.account;
  g_autofree gchar *room = room_of(PEER[0]);
  GhConversation *conversation = receive(a, PEER[0], T0 - 60, "hello");
  gui_window(&g, 900, 760, conversation);
  gui_open(&g, conversation);

  AdwActionRow *person = find_row(GTK_WIDGET(child(&g, "people_group")),
                                  "GhConversationInfoPerson", NULL);
  g_assert_nonnull(person);
  g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(person)), ==, "Alice");
  g_autofree gchar *peer_npub = npub_of(PEER[0]);
  const gchar *subtitle = adw_action_row_get_subtitle(person);
  g_assert_nonnull(strstr(subtitle, "npub1"));
  /* PD-2: the NIP-05 address is only what they claim. */
  g_assert_nonnull(strstr(subtitle, "Says they are alice@example.com (not checked)"));
  g_assert_null(strstr(subtitle, "verified"));

  /* The row opens Verify Key: their full key, the code, your key. */
  g_signal_emit_by_name(person, "activated");
  drain_idle();
  AdwNavigationView *navigation = child(&g, "navigation");
  g_assert_cmpstr(adw_navigation_page_get_tag(adw_navigation_view_get_visible_page(navigation)),
                  ==, "verify");
  g_autofree gchar *their_key = gh_privacy_format_key(peer_npub);
  g_assert_cmpstr(adw_preferences_row_get_title(child(&g, "their_key_row")), ==, their_key);
  g_assert_cmpstr(adw_preferences_group_get_title(child(&g, "their_group")), ==, "Alice’s Key");
  /* Two codes, each from one key (W15 review B2): theirs from the key held
   * for them, yours from the account's; never one code over both. */
  g_autofree gchar *their_code = gh_privacy_fingerprint(PEER[0]);
  g_autofree gchar *your_code = gh_privacy_fingerprint(ACCOUNT);
  g_assert_cmpstr(gtk_label_get_text(child(&g, "their_code_title")), ==, "Alice’s code");
  g_assert_cmpstr(gtk_label_get_text(child(&g, "their_code_label")), ==, their_code);
  g_assert_cmpstr(gtk_label_get_text(child(&g, "your_code_label")), ==, your_code);
  g_assert_cmpstr(their_code, !=, your_code);
  /* The copy says what matching proves, and that both are compared. */
  const gchar *how = adw_preferences_group_get_description(child(&g, "code_group"));
  g_assert_nonnull(strstr(how, "Compare both codes"));
  g_assert_nonnull(strstr(how, "the other way round"));
  g_assert_null(strstr(how, "real keys"));
  g_autofree gchar *own_npub = npub_of(ACCOUNT);
  g_autofree gchar *own_key = gh_privacy_format_key(own_npub);
  g_assert_cmpstr(adw_preferences_row_get_title(child(&g, "your_key_row")), ==, own_key);
  g_assert_true(gtk_widget_get_visible(child(&g, "verify_button")));
  g_assert_false(gtk_widget_get_visible(child(&g, "verified_row")));
  g_assert_cmpint(verified(a, PEER[0]), ==, 0);

  /* Marking records the time locally and says only that. */
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(g.dialog), "info.mark-verified", NULL));
  g_assert_cmpint(verified(a, PEER[0]), ==, T0);
  g_assert_true(gtk_widget_get_visible(child(&g, "verified_row")));
  g_assert_false(gtk_widget_get_visible(child(&g, "verify_button")));
  g_assert_cmpstr(adw_preferences_row_get_title(child(&g, "verified_row")), ==,
                  "You marked this key verified");
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(person), "You marked this key verified"));
  gui_close(&g);

  account_restart(a);
  g_assert_cmpint(verified(a, PEER[0]), ==, T0);
  conversation = gh_conversation_store_lookup(a->model, room);
  gui_window(&g, 900, 760, conversation);
  gui_open(&g, conversation);
  person = find_row(GTK_WIDGET(child(&g, "people_group")), "GhConversationInfoPerson", NULL);
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(person), "You marked this key verified"));
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(g.dialog), "info.verify", "s", PEER[0]));
  g_assert_true(gtk_widget_get_visible(child(&g, "verified_row")));
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(g.dialog), "info.clear-verified", NULL));
  g_assert_cmpint(verified(a, PEER[0]), ==, 0);
  g_assert_null(strstr(adw_action_row_get_subtitle(person), "verified"));
  gui_close(&g);
  account_clear(a);
}

/* The shown conversation's timer-change row (nostrc-qp24.83): its text in
 * the timeline, or NULL. */
static const gchar *
timeline_event(Gui *g)
{
  GtkWidget *view = gh_content_page_get_view(gh_window_get_content(g->window));
  GListModel *timeline = gh_conversation_view_get_timeline(GH_CONVERSATION_VIEW(view));
  for (guint i = 0; timeline && i < g_list_model_get_n_items(timeline); i++) {
    g_autoptr(GhTimelineItem) item = g_list_model_get_item(timeline, i);
    if (gh_timeline_item_get_event_text(item))
      return gh_timeline_item_get_event_text(item); /* the model keeps it */
  }
  return NULL;
}

typedef struct {
  GtkWidget *root;
  const gchar *text;
} EventWait;

/* A mapped timeline row showing text, whose accessible text starts with it. */
static gboolean
event_row_shown(GtkWidget *widget, const gchar *text)
{
  if (GH_IS_TIMELINE_ROW(widget) && gtk_widget_get_mapped(widget)) {
    GhTimelineItem *item = gh_timeline_row_get_item(GH_TIMELINE_ROW(widget));
    if (item && g_strcmp0(gh_timeline_item_get_event_text(item), text) == 0 &&
        g_str_has_prefix(gh_timeline_row_get_summary(GH_TIMELINE_ROW(widget)), text))
      return TRUE;
  }
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c))
    if (event_row_shown(c, text))
      return TRUE;
  return FALSE;
}

static gboolean
event_shown(gpointer data)
{
  EventWait *wait = data;
  return event_row_shown(wait->root, wait->text);
}

/* Charter §3.7 UI (nostrc-qp24.83): changing the timer in Conversation Info
 * adds the local timeline row "You set messages to disappear after 1 week"
 * (and "You turned off disappearing messages"); it stays after a restart,
 * and it is this device's record only: nothing is queued or sent, and the
 * conversation's messages are unchanged. */
static void
test_gui_timer(void)
{
  Gui g = { 0 };
  account_init(&g.account);
  Account *a = &g.account;
  g_autofree gchar *room = room_of(PEER[0]);
  GhConversation *conversation = receive(a, PEER[0], T0 - 60, "hello");
  gh_conversation_accept(conversation); /* shown in the conversation view */
  gui_window(&g, 900, 760, conversation);
  GtkWidget *view = gh_content_page_get_view(gh_window_get_content(g.window));
  g_assert_true(gh_conversation_view_get_conversation(GH_CONVERSATION_VIEW(view)) ==
                conversation);
  g_assert_null(timeline_event(&g));
  gui_open(&g, conversation);
  AdwComboRow *timer = child(&g, "timer_row");
  g_assert_true(gtk_widget_get_sensitive(GTK_WIDGET(timer)));
  g_assert_cmpuint(adw_combo_row_get_selected(timer), ==, 0);
  adw_combo_row_set_selected(timer, 2);
  gint64 seconds = -1;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_expiry_get_timer(a->expiry, room, &seconds, &error));
  g_assert_cmpint(seconds, ==, GH_EXPIRY_TIMER_WEEK);
  const gchar *week = "You set messages to disappear after 1 week";
  g_assert_cmpstr(timeline_event(&g), ==, week);
  EventWait shown = { GTK_WIDGET(g.window), week };
  adw_dialog_force_close(ADW_DIALOG(g.dialog));
  spin_until(event_shown, &shown);
  /* Local only (P8): nothing queued, nothing new in the conversation. */
  g_autoptr(GArray) queued = gh_store_outbox_list_unfinished(a->store, GH_STORE_BACKEND_NIP17,
                                                             &error);
  g_assert_no_error(error);
  g_assert_cmpuint(queued->len, ==, 0);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(conversation)), ==, 1);
  gui_close(&g);
  account_restart(a);
  g_assert_true(gh_expiry_get_timer(a->expiry, room, &seconds, &error));
  g_assert_cmpint(seconds, ==, GH_EXPIRY_TIMER_WEEK);
  conversation = gh_conversation_store_lookup(a->model, room);
  gui_window(&g, 900, 760, conversation);
  g_assert_cmpstr(timeline_event(&g), ==, week);
  gui_open(&g, conversation);
  g_assert_cmpuint(adw_combo_row_get_selected(child(&g, "timer_row")), ==, 2);
  adw_combo_row_set_selected(child(&g, "timer_row"), 0);
  g_assert_true(gh_expiry_get_timer(a->expiry, room, &seconds, &error));
  g_assert_cmpint(seconds, ==, GH_EXPIRY_TIMER_OFF);
  g_assert_cmpstr(timeline_event(&g), ==, "You turned off disappearing messages");
  gui_close(&g);
  account_clear(a);
}

/* The privacy group shows gh_privacy_summary_new(); a profile name is
 * shown as text wherever it appears (P4, A9: no markup injection). */
static void
test_gui_privacy_and_markup(void)
{
  Gui g = { 0 };
  account_init(&g.account);
  Account *a = &g.account;
  const gchar *name = "<b>Eve</b> & <i>co";
  GhConversation *conversation = receive(a, PEER[1], T0 - 60, "hello");
  gui_window(&g, 900, 760, conversation);
  gui_open(&g, conversation);
  GtkLabel *title = child(&g, "title_label");
  g_assert_cmpstr(gtk_label_get_text(title), ==, name);
  g_assert_false(gtk_label_get_use_markup(title));
  /* A stranger's first message is a message request (charter §7.9). */
  g_assert_true(gh_conversation_get_is_request(conversation));
  g_assert_cmpstr(gtk_label_get_text(child(&g, "subtitle_label")), ==,
                  "Message request · end-to-end encrypted");
  AdwActionRow *person = find_row(GTK_WIDGET(child(&g, "people_group")),
                                  "GhConversationInfoPerson", NULL);
  g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(person)), ==, name);
  g_assert_false(adw_preferences_row_get_use_markup(ADW_PREFERENCES_ROW(person)));

  GhPrivacyContext context = { .backend = GH_PRIVACY_BACKEND_NIP17, .peer_name = name,
                               .n_people = 1 };
  g_autoptr(GhPrivacySummary) summary = gh_privacy_summary_new(&context);
  AdwActionRow *privacy = child(&g, "privacy_row");
  g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(privacy)), ==,
                  summary->heading);
  g_assert_cmpstr(adw_action_row_get_subtitle(privacy), ==, summary->encrypted);
  g_assert_false(adw_preferences_row_get_use_markup(ADW_PREFERENCES_ROW(privacy)));
  for (guint i = 0; summary->visible[i]; i++) {
    AdwActionRow *row = find_row(GTK_WIDGET(child(&g, "visible_row")), "AdwActionRow",
                                 summary->visible[i]);
    g_assert_nonnull(row);
    g_assert_false(adw_preferences_row_get_use_markup(ADW_PREFERENCES_ROW(row)));
  }
  for (guint i = 0; summary->unprotected[i]; i++)
    g_assert_nonnull(find_row(GTK_WIDGET(child(&g, "unprotected_row")), "AdwActionRow",
                              summary->unprotected[i]));
  g_assert_cmpstr(adw_action_row_get_subtitle(child(&g, "storage_row")), ==, summary->storage);

  /* A group title is markup: the name is escaped there, so what shows is
   * the name itself. (libadwaita 1.5 returns the shown text as the title,
   * later releases the markup; the label is what the user sees.) */
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(g.dialog), "info.verify", "s", PEER[1]));
  GtkLabel *heading = find_label(GTK_WIDGET(child(&g, "their_group")), "<b>Eve</b> & <i>co’s Key");
  g_assert_nonnull(heading);
  g_assert_true(gtk_label_get_use_markup(heading));
  g_assert_cmpstr(adw_alert_dialog_get_heading(child(&g, "block_dialog")), ==,
                  "Block <b>Eve</b> & <i>co?");
  g_assert_false(adw_alert_dialog_get_heading_use_markup(child(&g, "block_dialog")));
  gui_close(&g);
  account_clear(a);
}

/* No open store: every row that would change it says why it can't. */
static void
test_gui_without_store(void)
{
  Gui g = { 0 };
  account_init(&g.account);
  Account *a = &g.account;
  GhConversation *conversation = receive(a, PEER[0], T0 - 60, "hello");
  gui_window(&g, 900, 760, conversation);
  GhConversationInfoServices services = { .model = a->model, .profile = profile };
  g.dialog = g_object_ref_sink(gh_conversation_info_dialog_new(conversation, &services));
  g_signal_connect(g.dialog, "closed", G_CALLBACK(on_closed), &g);
  adw_dialog_present(ADW_DIALOG(g.dialog), GTK_WIDGET(g.window));
  spin_until(gh_test_dialog_shown, g.dialog);
  static const char *const rows[] = { "mute_row", "timer_row", "block_row", "forget_row" };
  for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
    AdwActionRow *row = child(&g, rows[i]);
    g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(row)));
    g_assert_cmpstr(adw_action_row_get_subtitle(row), ==,
                    "Unavailable while Groundhog isn’t saving messages on this device");
  }
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(g.dialog), "info.verify", "s", PEER[0]));
  g_assert_false(gtk_widget_get_sensitive(child(&g, "verify_button")));
  /* Nothing is kept on this device, and the privacy summary says so. */
  const gchar *storage = adw_action_row_get_subtitle(child(&g, "storage_row"));
  g_assert_null(strstr(storage, "encrypted on this device"));
  g_assert_nonnull(strstr(storage, "isn't saving messages"));
  gui_close(&g);
  account_clear(a);
}

/* Continue Without Saving Messages: the privacy summary doesn't claim a copy
 * on disk, and the dialog still works on the in-memory store. */
static void
test_gui_ephemeral(void)
{
  Gui g = { 0 };
  account_init(&g.account);
  Account *a = &g.account;
  account_close(a);
  a->ephemeral = TRUE;
  account_open(a);
  g_assert_true(gh_store_is_ephemeral(a->store));
  GhConversation *conversation = receive(a, PEER[0], T0 - 60, "hello");
  gui_window(&g, 900, 760, conversation);
  gui_open(&g, conversation);
  const gchar *storage = adw_action_row_get_subtitle(child(&g, "storage_row"));
  g_assert_null(strstr(storage, "encrypted on this device"));
  g_assert_nonnull(strstr(storage, "in memory only"));
  g_assert_true(gtk_widget_get_sensitive(child(&g, "mute_row")));
  gui_close(&g);
  account_clear(a);
}

/* The dialog lets go of the store before it closes (account switch). */
static void
test_gui_closes_with_account(void)
{
  Gui g = { 0 };
  account_init(&g.account);
  Account *a = &g.account;
  GhConversation *conversation = receive(a, PEER[0], T0 - 60, "hello");
  gui_window(&g, 900, 760, conversation);
  gui_open(&g, conversation);
  account_close(a);
  spin_until(is_closed, &g);
  /* Acting afterwards reaches no store. */
  gtk_widget_activate_action(GTK_WIDGET(g.dialog), "info.verify", "s", PEER[0]);
  gtk_widget_activate_action(GTK_WIDGET(g.dialog), "info.mark-verified", NULL);
  gui_close(&g);
  account_open(a);
  account_clear(a);
}

/* The content page's info button and win.conversation-info (charter §7.4). */
static void
test_gui_header_button(void)
{
  Gui g = { 0 };
  account_init(&g.account);
  Account *a = &g.account;
  GhConversation *conversation = receive(a, PEER[0], T0 - 60, "hello");
  gh_conversation_accept(conversation);
  gui_window(&g, 900, 760, NULL);
  GhContentPage *content = gh_window_get_content(g.window);
  GtkWidget *button = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(content),
                                                               GH_TYPE_CONTENT_PAGE, "info_button"));
  g_assert_false(gtk_widget_get_visible(button));
  gh_conversation_info_attach(g.window, services_func, a, NULL);
  GAction *action = g_action_map_lookup_action(G_ACTION_MAP(g.window), "conversation-info");
  g_assert_nonnull(action);
  g_assert_false(g_action_get_enabled(action));

  g_assert_true(gh_window_open_item(g.window, conversation));
  drain_idle();
  g_assert_true(gtk_widget_get_visible(button));
  g_assert_true(g_action_get_enabled(action));
  g_assert_cmpstr(gtk_widget_get_tooltip_text(button), ==, "Conversation Info");
  /* The header subtitle comes from the privacy summary (§2.2 surface 1). */
  g_assert_cmpstr(gh_content_page_get_subtitle(content), ==,
                  "Private · end-to-end encrypted");

  g_action_activate(action, NULL);
  drain_idle();
  AdwDialog *shown = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(g.window));
  g_assert_true(GH_IS_CONVERSATION_INFO_DIALOG(shown));
  g_assert_true(gh_conversation_info_dialog_get_conversation(GH_CONVERSATION_INFO_DIALOG(shown)) ==
                conversation);
  spin_until(gh_test_dialog_shown, shown);
  adw_dialog_force_close(shown);
  drain_idle();

  /* A relay group (G20a): this dialog's Block, Forget, Mute and copy are
   * NIP-17's, so it is not offered there (its own dialog is G20b's), also
   * when the group follows a DM in the same view. */
  g_autofree gchar *group_id = gh_message_nip29_room_id("wss://groups.example.org", "pizza");
  GhConversation *group = gh_conversation_store_ensure_group(a->model, group_id, "Pizza");
  g_assert_nonnull(group);
  g_assert_true(gh_window_open_item(g.window, group));
  drain_idle();
  g_assert_false(g_action_get_enabled(action));
  g_action_activate(action, NULL);
  drain_idle();
  g_assert_null(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(g.window)));
  g_assert_true(gh_window_open_item(g.window, conversation));
  drain_idle();
  g_assert_true(g_action_get_enabled(action));
  gui_close(&g);
  account_clear(a);
}

/* ---- screenshots (opt-in evidence) ----------------------------------------------------- */

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
  g_autofree char *path = g_strdup_printf("%s/groundhog-g19-%s.png", dir, name);
  g_assert_true(gdk_texture_save_to_png(texture, path));
  g_test_message("saved %s", path);
}

typedef enum { SHOT_MAIN, SHOT_PRIVACY, SHOT_VERIFY, SHOT_BLOCK, SHOT_FORGET } Shot;

/* Scrolls the page holding widget so it is at the top. */
static void
scroll_to(GtkWidget *widget)
{
  GtkWidget *scrolled = gtk_widget_get_ancestor(widget, GTK_TYPE_SCROLLED_WINDOW);
  if (!scrolled)
    return;
  GtkWidget *content = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scrolled));
  graphene_point_t point;
  if (!gtk_widget_compute_point(widget, content, &GRAPHENE_POINT_INIT(0, 0), &point))
    return;
  GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled));
  gtk_adjustment_set_value(adjustment, MAX(point.y - 12, 0));
}

static void
take_shot(Gui *g, GhConversation *conversation, Shot shot, int width, int height,
          const char *dir, const char *name)
{
  gui_window(g, width, height, conversation);
  gui_open(g, conversation);
  switch (shot) {
  case SHOT_MAIN:
    break;
  case SHOT_PRIVACY:
    adw_expander_row_set_expanded(child(g, "visible_row"), TRUE);
    drain_idle();
    gtk_test_widget_wait_for_draw(GTK_WIDGET(g->window));
    scroll_to(gtk_widget_get_ancestor(GTK_WIDGET(child(g, "privacy_row")),
                                      ADW_TYPE_PREFERENCES_GROUP));
    break;
  case SHOT_VERIFY:
    gtk_widget_activate_action(GTK_WIDGET(g->dialog), "info.verify", "s", PEER[0]);
    break;
  case SHOT_BLOCK:
  case SHOT_FORGET: {
    RootWait wait = { child(g, shot == SHOT_BLOCK ? "block_dialog" : "forget_dialog"),
                      GTK_WINDOW(g->window) };
    gtk_widget_activate_action(GTK_WIDGET(g->dialog), shot == SHOT_BLOCK ? "info.block"
                                                                          : "info.forget", NULL);
    spin_until(alert_shown, &wait);
    break;
  }
  }
  drain_idle();
  gtk_test_widget_wait_for_draw(GTK_WIDGET(g->window));
  drain_idle();
  save_png(GTK_WIDGET(g->window), dir, name);
  gui_close(g);
}

static void
test_gui_screenshots(void)
{
  const char *dir = g_getenv("GROUNDHOG_TEST_SCREENSHOTS");
  if (!dir || !*dir) {
    g_test_skip("GROUNDHOG_TEST_SCREENSHOTS is not set");
    return;
  }
  /* As the other screenshot cases: switching the color scheme may warn
   * about libadwaita's own CSS on some GTK builds; criticals stay fatal. */
  GLogLevelFlags fatal = g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
  Gui g = { 0 };
  account_init(&g.account);
  Account *a = &g.account;
  receive(a, PEER[0], T0 - 3600, "Hi! Is this the right key for you?");
  GhConversation *conversation =
    receive(a, PEER[0], T0 - 60, "Let's compare our safety codes on the call.");
  gh_conversation_accept(conversation);
  g_autofree gchar *room = g_strdup(gh_conversation_get_room_id(conversation));
  g_assert_true(gh_store_conversations_set_muted_until(a->conversations, room, T0 + 8 * 3600,
                                                       NULL));
  g_assert_true(gh_expiry_set_timer(a->expiry, room, GH_EXPIRY_TIMER_WEEK, NULL));
  AdwStyleManager *style = adw_style_manager_get_default();
  static const struct {
    AdwColorScheme scheme;
    const char *name;
  } schemes[] = {
    { ADW_COLOR_SCHEME_FORCE_LIGHT, "light" },
    { ADW_COLOR_SCHEME_FORCE_DARK, "dark" },
  };
  for (guint s = 0; s < G_N_ELEMENTS(schemes); s++) {
    adw_style_manager_set_color_scheme(style, schemes[s].scheme);
    g_autofree char *wide = g_strdup_printf("info-%s-wide", schemes[s].name);
    g_autofree char *narrow = g_strdup_printf("info-%s-narrow", schemes[s].name);
    g_autofree char *privacy = g_strdup_printf("privacy-%s-narrow", schemes[s].name);
    g_autofree char *verify = g_strdup_printf("verify-%s-narrow", schemes[s].name);
    g_autofree char *verify_wide = g_strdup_printf("verify-%s-wide", schemes[s].name);
    g_autofree char *block = g_strdup_printf("block-confirm-%s", schemes[s].name);
    g_autofree char *forget = g_strdup_printf("forget-confirm-%s", schemes[s].name);
    take_shot(&g, conversation, SHOT_MAIN, 960, 760, dir, wide);
    take_shot(&g, conversation, SHOT_MAIN, 360, 720, dir, narrow);
    take_shot(&g, conversation, SHOT_PRIVACY, 360, 720, dir, privacy);
    take_shot(&g, conversation, SHOT_VERIFY, 360, 720, dir, verify);
    take_shot(&g, conversation, SHOT_VERIFY, 960, 760, dir, verify_wide);
    take_shot(&g, conversation, SHOT_BLOCK, 960, 760, dir, block);
    take_shot(&g, conversation, SHOT_FORGET, 360, 720, dir, forget);
  }
  adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_DEFAULT);
  account_clear(a);
  g_log_set_always_fatal(fatal);
}

/* ---- main ------------------------------------------------------------------------------- */

int
main(int argc, char **argv)
{
  gboolean gui_mode = argc > 1 && g_str_equal(argv[1], "--gui");
  if (gui_mode) {
    argv[1] = argv[0];
    argv++;
    argc--;
  }
  g_setenv("GTK_A11Y", "none", TRUE);
  g_setenv("GNOTIFICATION_BACKEND", "gtk", FALSE);
  ACCOUNT = hex_of("account-a");
  for (guint i = 0; i < G_N_ELEMENTS(PEER); i++) {
    g_autofree gchar *seed = g_strdup_printf("peer-%u", i);
    PEER[i] = hex_of(seed);
  }
  int status;
  if (gui_mode) {
    /* Before g_test_init(), as the other GUI tests: a host theme's parser
     * warnings are not this test's. */
    if (!gtk_init_check()) {
      g_printerr("groundhog-conversation-info GUI test skipped: no graphical display\n");
      status = 77;
      goto out;
    }
    adw_init();
    groundhog_register_resource();
    g_autoptr(GtkCssProvider) css = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(css, "/org/nostr/Groundhog/style.css");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    /* 1sp is one pixel, no animations, GNOME's close-only controls. */
    g_object_set(gtk_settings_get_default(), "gtk-xft-dpi", 96 * 1024, "gtk-enable-animations",
                 FALSE, "gtk-decoration-layout", "appmenu:close", NULL);
    g_test_init(&argc, &argv, NULL);
    nostrc_test_tolerate_gdk_frame_warning();
    g_test_add_func("/groundhog/conversation-info-gui/st9-forget", test_gui_forget);
    g_test_add_func("/groundhog/conversation-info-gui/mute-in-database", test_gui_mute);
    g_test_add_func("/groundhog/conversation-info-gui/block", test_gui_block);
    g_test_add_func("/groundhog/conversation-info-gui/verify", test_gui_verify);
    g_test_add_func("/groundhog/conversation-info-gui/timer", test_gui_timer);
    g_test_add_func("/groundhog/conversation-info-gui/privacy-and-markup",
                    test_gui_privacy_and_markup);
    g_test_add_func("/groundhog/conversation-info-gui/without-store", test_gui_without_store);
    g_test_add_func("/groundhog/conversation-info-gui/ephemeral", test_gui_ephemeral);
    g_test_add_func("/groundhog/conversation-info-gui/closes-with-account",
                    test_gui_closes_with_account);
    g_test_add_func("/groundhog/conversation-info-gui/header-button", test_gui_header_button);
    g_test_add_func("/groundhog/conversation-info-gui/screenshots", test_gui_screenshots);
    status = g_test_run();
    goto out;
  }
  g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
  g_test_add_func("/groundhog/conversation-info/verified-marks", test_verified_marks);
  if (!nostrc_test_bus_available()) {
    g_printerr("Groundhog conversation-info notifier tests skipped: no dbus-daemon\n");
    status = g_test_run();
    goto out;
  }
  bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  nostrc_test_bus_up(bus);
  fake = fake_gtk_notifications_new(nostrc_test_bus_connect(bus));
  nostrc_test_bus_add_func("/groundhog/conversation-info/mute", test_mute_withdraws_and_persists);
  nostrc_test_bus_add_func("/groundhog/conversation-info/block", test_block);
  nostrc_test_bus_add_func("/groundhog/conversation-info/st9-forget", test_forget);
  status = g_test_run();
  fake_gtk_notifications_free(fake);
  nostrc_test_bus_down(bus);
out:
  g_free(ACCOUNT);
  for (guint i = 0; i < G_N_ELEMENTS(PEER); i++)
    g_free(PEER[i]);
  return status;
}
