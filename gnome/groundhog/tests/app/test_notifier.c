/* Private notifications (privacy charter §5.1, §5.2, §8.2 G16; tests NO-1…NO-8
 * of §9.2, and the NO-11 locked-store notice): GhNotifier over a real
 * conversation model, sending through GLib's GTK notification backend to the
 * fake org.gtk.Notifications server (H4) on the shared private bus
 * (tests/common/nostrc-test-bus.h), on a fake GhClock (H6). NO-7 uses a real
 * SQLCipher store for the mute. Nothing sleeps: time moves only with the fake
 * clock, and every D-Bus wait is a round trip with a failure deadline.
 *
 * --gui: the window glue (a notification opens its conversation; a stale one
 * shows the list and a toast), with no private bus; exits 77 without a
 * display. */
#include "gh-conversation-list.h"
#include "gh-conversation-private.h"
#include "gh-notifier.h"
#include "gh-store-conversations.h"
#include "fake-gtk-notifications.h"
#include "nostrc-test-bus.h"

#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr-utils.h"
#include "nostr/nip19/nip19.h"
#include "../ui/gh-test-active.h"

#include <adwaita.h>
#include <stdlib.h>
#include <string.h>

#define T0 ((gint64)1790000000)
#define APP_SCHEMA "org.nostr.Groundhog"
#define CANARY "GROUNDHOG-CANARY-notifier"

void groundhog_register_resource(void);

static gchar *ACCOUNT_A, *ACCOUNT_B, *PEER[5];
static NostrcTestBus *bus;
static FakeGtkNotifications *fake;
static guint app_serial;
static guint wrap_serial;

static gchar *
hex_of(const gchar *seed)
{
  return g_compute_checksum_for_string(G_CHECKSUM_SHA256, seed, -1);
}

static gchar *
npub_of(const gchar *hex, gboolean short_form)
{
  guint8 bytes[32];
  char *npub = NULL;
  g_assert_true(nostr_hex2bin(bytes, hex, sizeof bytes));
  g_assert_cmpint(nostr_nip19_encode_npub(bytes, &npub), ==, 0);
  gsize length = strlen(npub);
  gchar *out = short_form ? g_strdup_printf("%.10s…%s", npub, npub + length - 4) : g_strdup(npub);
  free(npub);
  return out;
}

/* ---- rumors ------------------------------------------------------------------------- */

typedef struct {
  const gchar *author;
  const gchar *to[4];
  gint64 created_at;
  const gchar *content;
  const gchar *subject;
  gint64 expiration;
} Rumor;

static GhMessage *
message_new(const gchar *account, const Rumor *r)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 14);
  nostr_event_set_pubkey(event, r->author);
  nostr_event_set_created_at(event, r->created_at);
  nostr_event_set_content(event, r->content ? r->content : "text");
  NostrTags *tags = nostr_tags_new(0);
  for (guint i = 0; r->to[i]; i++)
    nostr_tags_append(tags, nostr_tag_new("p", r->to[i], NULL));
  if (r->subject)
    nostr_tags_append(tags, nostr_tag_new("subject", r->subject, NULL));
  if (r->expiration) {
    g_autofree gchar *value = g_strdup_printf("%" G_GINT64_FORMAT, r->expiration);
    nostr_tags_append(tags, nostr_tag_new("expiration", value, NULL));
  }
  nostr_event_set_tags(event, tags);
  event->id = nostr_event_get_id(event);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  g_autoptr(GError) error = NULL;
  GhMessage *message = gh_message_new_from_rumor(account, json, &error);
  free(json);
  g_assert_no_error(error);
  return message;
}

/* ---- the fixture ---------------------------------------------------------------------- */

typedef struct {
  GApplication *app;
  GSettings *settings;
  GhClock *clock;
  GhConversationStore *model;
  GhNotifier *notifier;
  GHashTable *mutes;                 /* room id -> muted_until; -1 = blocked */
  GhStoreConversations *conversations; /* NO-7: the real store's delegate */
  guint activations;
  GPtrArray *opened;                 /* GhConversation */
  GPtrArray *stale;                  /* npub, or "" when unknown */
  GArray *sounds;                    /* gint64 monotonic */
  gint64 last_seen;                  /* the last-seen marker offered (N1); 0 none */
  guint last_seen_asked;
} Fixture;

static gint64
last_seen(gpointer data, const gchar *account)
{
  Fixture *f = data;
  if (g_strcmp0(account, ACCOUNT_A) != 0)
    return 0; /* another account's marker is its own */
  f->last_seen_asked++;
  return f->last_seen;
}

static gboolean
room_state(gpointer data, const gchar *room_id, GhNotifierRoomState *state)
{
  Fixture *f = data;
  if (f->conversations) {
    GhStoreNotifyState stored = { 0 };
    if (!gh_store_conversations_get_notify_state(f->conversations, room_id, &stored, NULL))
      return FALSE;
    state->muted_until = stored.muted_until;
    state->blocked = stored.blocked;
    return TRUE;
  }
  gint64 *value = g_hash_table_lookup(f->mutes, room_id);
  state->blocked = value && *value == -1;
  state->muted_until = value && *value > 0 ? *value : 0;
  return TRUE;
}

static void
set_mute(Fixture *f, const gchar *room_id, gint64 value)
{
  gint64 *copy = g_new(gint64, 1);
  *copy = value;
  g_hash_table_replace(f->mutes, g_strdup(room_id), copy);
}

static void
on_activate(GApplication *app, Fixture *f)
{
  (void)app;
  f->activations++;
}

static void
on_open(GhNotifier *notifier, GhConversation *conversation, Fixture *f)
{
  (void)notifier;
  g_ptr_array_add(f->opened, g_object_ref(conversation));
}

static void
on_stale(GhNotifier *notifier, const gchar *npub, Fixture *f)
{
  (void)notifier;
  g_ptr_array_add(f->stale, g_strdup(npub ? npub : ""));
}

static void
on_sound(GhNotifier *notifier, Fixture *f)
{
  (void)notifier;
  gint64 now = gh_clock_get_monotonic_time(f->clock);
  g_array_append_val(f->sounds, now);
}

static GhNotifier *
notifier_new(Fixture *f)
{
  GhNotifierConfig config = {
    .settings = f->settings,
    .conversations = f->model,
    .clock = f->clock,
    .room_state = room_state,
    .room_state_data = f,
    .last_seen = last_seen,
    .last_seen_data = f,
  };
  GhNotifier *notifier = gh_notifier_new(f->app, &config);
  g_signal_connect(notifier, "open-conversation", G_CALLBACK(on_open), f);
  g_signal_connect(notifier, "stale-activation", G_CALLBACK(on_stale), f);
  g_signal_connect(notifier, "play-sound", G_CALLBACK(on_sound), f);
  return notifier;
}

static void
fixture_up(Fixture *f, const gchar *level, gboolean bind)
{
  memset(f, 0, sizeof *f);
  f->clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
  fake_gtk_notifications_set_clock(fake, f->clock);
  fake_gtk_notifications_clear(fake);
  f->settings = g_settings_new(APP_SCHEMA);
  g_settings_set_boolean(f->settings, "notifications-enabled", TRUE);
  g_settings_set_string(f->settings, "notification-privacy", level);
  g_settings_set_boolean(f->settings, "sound-enabled", FALSE);
  g_autofree gchar *id = g_strdup_printf("org.nostr.GroundhogTest.Notifier%u", ++app_serial);
  f->app = g_application_new(id, G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(f->app, "activate", G_CALLBACK(on_activate), f);
  g_autoptr(GError) error = NULL;
  g_assert_true(g_application_register(f->app, NULL, &error));
  g_assert_no_error(error);
  f->mutes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  f->opened = g_ptr_array_new_with_free_func(g_object_unref);
  f->stale = g_ptr_array_new_with_free_func(g_free);
  f->sounds = g_array_new(FALSE, FALSE, sizeof(gint64));
  f->model = gh_conversation_store_new();
  f->notifier = notifier_new(f);
  /* The session starts when the model binds the account (at T0). */
  if (bind)
    gh_conversation_store_set_account(f->model, ACCOUNT_A, NULL, NULL, NULL);
}

static GDBusConnection *
client(Fixture *f)
{
  return g_application_get_dbus_connection(f->app);
}

static void
sync_calls(Fixture *f)
{
  fake_gtk_notifications_sync(fake, client(f));
}

/* Moves the clock delta_us forward one due timeout at a time, recording
 * after each, so every call is stamped with the time it was sent. */
static void
run_for(Fixture *f, gint64 delta_us)
{
  gint64 target = gh_clock_get_monotonic_time(f->clock) + delta_us;
  gint64 due;
  while ((due = gh_clock_fake_get_next_deadline(f->clock)) >= 0 && due <= target) {
    gh_clock_fake_advance(f->clock, MAX(due - gh_clock_get_monotonic_time(f->clock), 0));
    sync_calls(f);
  }
  gh_clock_fake_advance(f->clock, target - gh_clock_get_monotonic_time(f->clock));
  sync_calls(f);
}

/* Runs what is due on the next main-loop turn (a zero-delay GhClock
 * timeout is due 1 µs later on the fake clock). */
static void
settle(Fixture *f)
{
  run_for(f, 1);
}

static void
advance_ms(Fixture *f, gint64 ms)
{
  run_for(f, ms * 1000);
}

/* N5 and the icon rule, for every notification this fixture sent. (GLib's
 * GTK backend carries no category: NO-8 checks it through the freedesktop
 * backend.) */
static void
assert_common(void)
{
  GPtrArray *calls = fake_gtk_notifications_get_calls(fake);
  for (guint i = 0; i < calls->len; i++) {
    FakeNotificationCall *call = g_ptr_array_index(calls, i);
    if (!call->added)
      continue;
    const gchar *priority = NULL;
    g_assert_true(g_variant_lookup(call->notification, "priority", "&s", &priority));
    g_assert_cmpstr(priority, ==, "normal");
    g_assert_false(g_variant_lookup(call->notification, "icon", "*", NULL));
    g_assert_false(g_variant_lookup(call->notification, "buttons", "*", NULL));
  }
}

static void
fixture_down(Fixture *f)
{
  assert_common();
  /* Dispose withdraws what it shows. */
  g_object_run_dispose(G_OBJECT(f->notifier));
  sync_calls(f);
  GPtrArray *calls = fake_gtk_notifications_get_calls(fake);
  for (guint i = 0; i < calls->len; i++) {
    FakeNotificationCall *call = g_ptr_array_index(calls, i);
    g_assert_null(fake_gtk_notifications_lookup(fake, call->id));
  }
  g_assert_null(g_action_map_lookup_action(G_ACTION_MAP(f->app), GH_NOTIFIER_ACTION));
  g_clear_object(&f->notifier);
  if (f->conversations)
    gh_store_conversations_close(f->conversations);
  g_clear_object(&f->model);
  g_clear_object(&f->conversations);
  g_clear_object(&f->app);
  g_settings_reset(f->settings, "notifications-enabled");
  g_settings_reset(f->settings, "notification-privacy");
  g_settings_reset(f->settings, "sound-enabled");
  g_clear_object(&f->settings);
  g_hash_table_unref(f->mutes);
  g_ptr_array_unref(f->opened);
  g_ptr_array_unref(f->stale);
  g_array_unref(f->sounds);
  fake_gtk_notifications_set_clock(fake, NULL);
  fake_gtk_notifications_clear(fake);
  g_clear_pointer(&f->clock, gh_clock_unref);
}

static GhConversation *
deliver_as(Fixture *f, const gchar *account, const Rumor *r)
{
  g_autoptr(GhMessage) message = message_new(account, r);
  g_autofree gchar *seed = g_strdup_printf("wrap/%u", ++wrap_serial);
  g_autofree gchar *wrap = hex_of(seed);
  g_autoptr(GError) error = NULL;
  g_assert_cmpint(gh_conversation_store_admit(f->model, message, wrap, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_no_error(error);
  return gh_conversation_store_lookup(f->model, gh_message_get_room_id(message));
}

static GhConversation *
deliver(Fixture *f, const Rumor *r)
{
  return deliver_as(f, gh_conversation_store_get_account(f->model), r);
}

/* A message from peer (to the account and others), now. */
static GhConversation *
receive(Fixture *f, const gchar *peer, const gchar *other, const gchar *content)
{
  const gchar *account = gh_conversation_store_get_account(f->model);
  Rumor r = { .author = peer, .to = { account, other }, .content = content,
              .created_at = gh_clock_get_unix(f->clock) };
  return deliver(f, &r);
}

/* An accepted conversation: the account wrote in it before this session. */
static GhConversation *
open_room(Fixture *f, const gchar *peer, const gchar *other)
{
  const gchar *account = gh_conversation_store_get_account(f->model);
  Rumor r = { .author = account, .to = { peer, other }, .content = "earlier",
              .created_at = T0 - 1000 };
  GhConversation *conversation = deliver(f, &r);
  g_assert_false(gh_conversation_get_is_request(conversation));
  return conversation;
}

static const gchar *
field(GVariant *notification, const gchar *key)
{
  const gchar *value = NULL;
  g_assert_nonnull(notification);
  if (!g_variant_lookup(notification, key, "&s", &value))
    return NULL;
  return value;
}

static GVariant *
shown(const gchar *id)
{
  return fake_gtk_notifications_lookup(fake, id);
}

static GVariant *
target_of(const gchar *id)
{
  GVariant *notification = shown(id);
  g_assert_nonnull(notification);
  g_assert_cmpstr(field(notification, "default-action"), ==, "app." GH_NOTIFIER_ACTION);
  GVariant *target = g_variant_lookup_value(notification, "default-action-target",
                                            G_VARIANT_TYPE("(tx)"));
  g_assert_nonnull(target);
  return target;
}

static void
assert_absent(const gchar *haystack, const gchar *needle)
{
  if (strstr(haystack, needle))
    g_error("notification payloads contain \"%s\":\n%s", needle, haystack);
}

/* No name, npub, key, subject or text of these in any payload. */
static void
assert_payloads_hide(const gchar *const *secrets)
{
  g_autofree gchar *dump = fake_gtk_notifications_dump(fake);
  for (guint i = 0; secrets[i]; i++)
    assert_absent(dump, secrets[i]);
}

static void
assert_identity_hidden(const gchar *peer)
{
  g_autofree gchar *full = npub_of(peer, FALSE);
  g_autofree gchar *head = g_strndup(full, 15);
  g_autofree gchar *account_npub = npub_of(ACCOUNT_A, FALSE);
  g_autofree gchar *account_head = g_strndup(account_npub, 15);
  const gchar *const secrets[] = { peer, head, ACCOUNT_A, account_head, CANARY, NULL };
  assert_payloads_hide(secrets);
}

/* ---- NO-1 levels ------------------------------------------------------------------------ */

static void
test_no1_hidden(void)
{
  Fixture f;
  fixture_up(&f, "hidden", TRUE);
  open_room(&f, PEER[0], NULL);
  for (guint i = 0; i < 3; i++) {
    Rumor r = { .author = PEER[0], .to = { ACCOUNT_A }, .created_at = T0 + i,
                .content = CANARY " text", .subject = CANARY " subject" };
    deliver(&f, &r);
  }
  settle(&f);
  /* One app-wide id, sent once for the three. */
  g_assert_cmpuint(fake_gtk_notifications_count(fake, TRUE, NULL), ==, 1);
  GVariant *n = shown(GH_NOTIFIER_ID_MESSAGES);
  g_assert_cmpstr(field(n, "title"), ==, "New message");
  g_assert_cmpstr(field(n, "body"), ==, "3 new messages");
  g_autoptr(GVariant) target = target_of(GH_NOTIFIER_ID_MESSAGES);
  assert_identity_hidden(PEER[0]);

  /* A second conversation: the count grows, and the target goes (it would
   * open only one of them). */
  open_room(&f, PEER[1], NULL);
  advance_ms(&f, GH_NOTIFIER_UPDATE_INTERVAL_MS);
  receive(&f, PEER[1], NULL, CANARY " more");
  settle(&f);
  n = shown(GH_NOTIFIER_ID_MESSAGES);
  g_assert_cmpstr(field(n, "body"), ==, "4 new messages");
  g_assert_null(field(n, "default-action"));
  g_assert_cmpuint(fake_gtk_notifications_count(fake, TRUE, NULL), ==, 2);
  assert_identity_hidden(PEER[1]);
  fixture_down(&f);
}

static void
test_no1_sender(void)
{
  Fixture f;
  fixture_up(&f, "sender", TRUE);
  GhConversation *p = open_room(&f, PEER[0], NULL);
  GhConversation *q = open_room(&f, PEER[1], NULL);
  receive(&f, PEER[0], NULL, CANARY " one");
  receive(&f, PEER[1], NULL, CANARY " two");
  settle(&f);
  GVariant *n = shown(GH_NOTIFIER_ID_CONVERSATION_PREFIX "1");
  g_assert_cmpstr(field(n, "title"), ==, gh_conversation_get_title(p));
  g_assert_cmpstr(field(n, "body"), ==, "New message");
  n = shown(GH_NOTIFIER_ID_CONVERSATION_PREFIX "2");
  g_assert_cmpstr(field(n, "title"), ==, gh_conversation_get_title(q));
  advance_ms(&f, GH_NOTIFIER_UPDATE_INTERVAL_MS);
  receive(&f, PEER[0], NULL, CANARY " three");
  settle(&f);
  /* The same id, updated. */
  n = shown(GH_NOTIFIER_ID_CONVERSATION_PREFIX "1");
  g_assert_cmpstr(field(n, "body"), ==, "2 new messages");
  g_assert_null(shown(GH_NOTIFIER_ID_MESSAGES));
  const gchar *const secrets[] = { CANARY, PEER[0], PEER[1], ACCOUNT_A, NULL };
  assert_payloads_hide(secrets);
  fixture_down(&f);
}

static void
test_no1_preview(void)
{
  Fixture f;
  fixture_up(&f, "preview", TRUE);
  GhConversation *p = open_room(&f, PEER[0], NULL);
  receive(&f, PEER[0], NULL, "line one\n\n   line two\r\nthree\t4");
  settle(&f);
  GVariant *n = shown(GH_NOTIFIER_ID_CONVERSATION_PREFIX "1");
  g_assert_cmpstr(field(n, "title"), ==, gh_conversation_get_title(p));
  g_assert_cmpstr(field(n, "body"), ==, "line one line two three 4");

  /* At most 120 graphemes: 200 family emoji (5 code points each) keep 119
   * and an ellipsis. */
  g_autoptr(GString) family = g_string_new(NULL);
  g_autoptr(GString) expected = g_string_new(NULL);
  for (guint i = 0; i < 200; i++) {
    g_string_append(family, "👩‍👩‍👧");
    if (i < GH_NOTIFIER_PREVIEW_MAX - 1)
      g_string_append(expected, "👩‍👩‍👧");
  }
  g_string_append(expected, "…");
  advance_ms(&f, GH_NOTIFIER_UPDATE_INTERVAL_MS);
  receive(&f, PEER[0], NULL, family->str);
  settle(&f);
  g_assert_cmpstr(field(shown(GH_NOTIFIER_ID_CONVERSATION_PREFIX "1"), "body"), ==,
                  expected->str);
  /* 120 plain characters stay whole. */
  g_autofree gchar *exact = g_strnfill(GH_NOTIFIER_PREVIEW_MAX, 'x');
  advance_ms(&f, GH_NOTIFIER_UPDATE_INTERVAL_MS);
  receive(&f, PEER[0], NULL, exact);
  settle(&f);
  g_assert_cmpstr(field(shown(GH_NOTIFIER_ID_CONVERSATION_PREFIX "1"), "body"), ==, exact);

  /* A group names its author. */
  open_room(&f, PEER[1], PEER[2]);
  receive(&f, PEER[1], PEER[2], "hello group");
  settle(&f);
  g_autofree gchar *author = npub_of(PEER[1], TRUE);
  g_autofree gchar *body = g_strdup_printf("%s: hello group", author);
  g_assert_cmpstr(field(shown(GH_NOTIFIER_ID_CONVERSATION_PREFIX "2"), "body"), ==, body);
  fixture_down(&f);
}

/* ---- NO-2 forced hidden ------------------------------------------------------------------- */

static void
test_no2_forced_hidden(void)
{
  Fixture f;
  fixture_up(&f, "preview", TRUE);
  /* A request (nobody here wrote in it) is hidden even at preview. */
  GhConversation *request = receive(&f, PEER[3], NULL, CANARY " request");
  g_assert_true(gh_conversation_get_is_request(request));
  settle(&f);
  GVariant *n = shown(GH_NOTIFIER_ID_MESSAGES);
  g_assert_cmpstr(field(n, "title"), ==, "New message");
  g_assert_cmpstr(field(n, "body"), ==, "1 new message");
  g_assert_cmpuint(fake_gtk_notifications_count(fake, TRUE, NULL), ==, 1);
  assert_identity_hidden(PEER[3]);
  const gchar *const subject[] = { gh_conversation_get_title(request), NULL };
  assert_payloads_hide(subject);

  /* Blocked: nothing at all. */
  fake_gtk_notifications_clear(fake);
  GhConversation *blocked = open_room(&f, PEER[0], NULL);
  set_mute(&f, gh_conversation_get_room_id(blocked), -1);
  advance_ms(&f, GH_NOTIFIER_UPDATE_INTERVAL_MS);
  receive(&f, PEER[0], NULL, CANARY " blocked");
  settle(&f);
  advance_ms(&f, 3 * GH_NOTIFIER_UPDATE_INTERVAL_MS);
  g_assert_cmpuint(fake_gtk_notifications_count(fake, TRUE, NULL), ==, 0);

  /* A wrap that failed to decrypt is recorded as rejected and never reaches
   * the model: nothing. */
  g_autofree gchar *rejected = hex_of("no2/rejected");
  g_assert_true(gh_conversation_store_record_rejected(f.model, rejected, NULL));
  advance_ms(&f, GH_NOTIFIER_UPDATE_INTERVAL_MS);
  g_assert_cmpuint(fake_gtk_notifications_count(fake, TRUE, NULL), ==, 0);
  fixture_down(&f);
}

/* ---- NO-3 suppression ------------------------------------------------------------------------ */

static void
assert_nothing_sent(Fixture *f)
{
  advance_ms(f, 3 * GH_NOTIFIER_UPDATE_INTERVAL_MS);
  g_assert_cmpuint(fake_gtk_notifications_count(fake, TRUE, NULL), ==, 0);
}

static void
test_no3_suppression(void)
{
  Fixture f;
  fixture_up(&f, "preview", TRUE);
  GhConversation *p = open_room(&f, PEER[0], NULL);
  const gchar *room = gh_conversation_get_room_id(p);

  /* Visible in the active window. */
  gh_notifier_set_visible_conversation(f.notifier, p);
  receive(&f, PEER[0], NULL, "while visible");
  assert_nothing_sent(&f);
  gh_notifier_set_visible_conversation(f.notifier, NULL);

  /* Muted, until the mute ends. */
  gint64 until = gh_clock_get_unix(f.clock) + 60;
  set_mute(&f, room, until);
  receive(&f, PEER[0], NULL, "while muted");
  assert_nothing_sent(&f);
  set_mute(&f, room, GH_STORE_CONVERSATIONS_MUTED_ALWAYS);
  receive(&f, PEER[0], NULL, "muted always");
  assert_nothing_sent(&f);
  set_mute(&f, room, until);
  advance_ms(&f, 60 * 1000);
  gh_conversation_mark_read(p);

  /* A self-copy, and an own message sent elsewhere. */
  Rumor own = { .author = ACCOUNT_A, .to = { PEER[0] }, .content = "mine",
                .created_at = gh_clock_get_unix(f.clock) };
  deliver(&f, &own);
  assert_nothing_sent(&f);

  /* Backfill from before the session. */
  Rumor old = { .author = PEER[0], .to = { ACCOUNT_A }, .content = "old", .created_at = T0 - 1 };
  deliver(&f, &old);
  assert_nothing_sent(&f);

  /* Expired. */
  gint64 now = gh_clock_get_unix(f.clock);
  Rumor gone = { .author = PEER[0], .to = { ACCOUNT_A }, .content = "gone",
                 .created_at = now, .expiration = now };
  deliver(&f, &gone);
  assert_nothing_sent(&f);
  gh_conversation_mark_read(p);

  /* Read before it would go out (the window marks it read in the same
   * turn). */
  receive(&f, PEER[0], NULL, "read at once");
  gh_conversation_mark_read(p);
  assert_nothing_sent(&f);

  /* Notifications off. */
  g_settings_set_boolean(f.settings, "notifications-enabled", FALSE);
  receive(&f, PEER[0], NULL, "while off");
  assert_nothing_sent(&f);
  g_settings_set_boolean(f.settings, "notifications-enabled", TRUE);
  gh_conversation_mark_read(p);

  /* And the control: the same conversation, nothing in the way. */
  receive(&f, PEER[0], NULL, "notified");
  settle(&f);
  g_assert_cmpstr(field(shown(GH_NOTIFIER_ID_CONVERSATION_PREFIX "1"), "body"), ==, "notified");
  g_assert_cmpuint(fake_gtk_notifications_count(fake, TRUE, NULL), ==, 1);
  fixture_down(&f);
}

/* ---- nostrc-qp24.75: read by arrival ------------------------------------------------------ */

/* A message from peer at created_at whose rumor id sorts before
 * reference_id (a burst within one second). */
static GhMessage *
same_second_before(const gchar *peer, gint64 created_at, const gchar *reference_id)
{
  for (guint i = 0; i < 1000; i++) {
    g_autofree gchar *content = g_strdup_printf("burst %u", i);
    Rumor r = { .author = peer, .to = { ACCOUNT_A }, .content = content,
                .created_at = created_at };
    GhMessage *message = message_new(ACCOUNT_A, &r);
    if (strcmp(gh_message_get_rumor_id(message), reference_id) < 0)
      return message;
    g_object_unref(message);
  }
  g_assert_not_reached();
}

/* The G24 NO-1 case: two messages within one second, the first read at
 * once. The second has a lower rumor id, so it sorts before the read one;
 * it arrived after the read, so it is unread and notified (it used to count
 * as read and be dropped). */
static void
test_read_by_arrival(void)
{
  Fixture f;
  fixture_up(&f, "hidden", TRUE);
  GhConversation *p = open_room(&f, PEER[0], NULL);
  receive(&f, PEER[0], NULL, "first");
  gh_conversation_mark_read(p);
  assert_nothing_sent(&f);
  guint n = g_list_model_get_n_items(G_LIST_MODEL(p));
  g_autoptr(GhMessage) read = g_list_model_get_item(G_LIST_MODEL(p), n - 1);
  g_autoptr(GhMessage) burst = same_second_before(PEER[0], gh_message_get_created_at(read),
                                                  gh_message_get_rumor_id(read));
  g_autofree gchar *wrap = hex_of("arrival/burst");
  g_assert_cmpint(gh_conversation_store_admit(f.model, burst, wrap, NULL), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_cmpuint(gh_conversation_get_unread_count(p), ==, 1);
  settle(&f);
  g_assert_cmpuint(fake_gtk_notifications_count(fake, TRUE, NULL), ==, 1);
  g_assert_cmpstr(field(shown(GH_NOTIFIER_ID_MESSAGES), "body"), ==, "1 new message");
  /* Read now: withdrawn. */
  gh_conversation_mark_read(p);
  settle(&f);
  g_assert_null(shown(GH_NOTIFIER_ID_MESSAGES));
  fixture_down(&f);
}

/* ---- nostrc-qp24.84: what came while not receiving ------------------------------------------ */

/* Charter §5.2 N1 (amended): with autostart at login, what arrived overnight
 * is notified when it was written after the account's last-seen marker (the
 * inbox checkpoint, less GH_NOTIFIER_LAST_SEEN_GRACE), coalesced like any
 * burst (one hidden "N new messages"), requests hidden too; what was written
 * before it is old history and never notified; without a marker (a first
 * session) only what is written after binding is. */
static void
test_backfill(void)
{
  Fixture f;
  fixture_up(&f, "sender", FALSE);
  const gint64 night = T0 - 8 * 3600; /* Groundhog stopped receiving here */
  f.last_seen = night;
  gh_conversation_store_set_account(f.model, ACCOUNT_A, NULL, NULL, NULL);
  g_assert_cmpuint(f.last_seen_asked, >=, 1);
  /* Accepted conversations: the account wrote in them before the night (an
   * own message from another device reads whatever sorts before it). */
  Rumor mine_p = { .author = ACCOUNT_A, .to = { PEER[0] }, .content = "earlier",
                   .created_at = night - 3 * GH_NOTIFIER_LAST_SEEN_GRACE };
  Rumor mine_q = { .author = ACCOUNT_A, .to = { PEER[1] }, .content = "earlier",
                   .created_at = night - 3 * GH_NOTIFIER_LAST_SEEN_GRACE };
  GhConversation *p = deliver(&f, &mine_p);
  GhConversation *q = deliver(&f, &mine_q);

  /* Old history (before the marker less the grace): nothing. */
  Rumor old = { .author = PEER[0], .to = { ACCOUNT_A }, .content = "old",
                .created_at = night - GH_NOTIFIER_LAST_SEEN_GRACE - 1 };
  deliver(&f, &old);
  assert_nothing_sent(&f);
  gh_conversation_mark_read(p);

  /* Overnight (after the marker, and within the grace before it), in two
   * conversations and a request: one notification per id, each once. */
  const gint64 times[] = { night - GH_NOTIFIER_LAST_SEEN_GRACE + 60, night + 3600, T0 - 60 };
  for (guint i = 0; i < G_N_ELEMENTS(times); i++) {
    Rumor r = { .author = PEER[0], .to = { ACCOUNT_A }, .content = CANARY " overnight",
                .created_at = times[i] };
    deliver(&f, &r);
  }
  Rumor to_q = { .author = PEER[1], .to = { ACCOUNT_A }, .content = CANARY " q",
                 .created_at = night + 60 };
  deliver(&f, &to_q);
  Rumor stranger = { .author = PEER[3], .to = { ACCOUNT_A }, .content = CANARY " request",
                     .created_at = night + 120 };
  GhConversation *request = deliver(&f, &stranger);
  g_assert_true(gh_conversation_get_is_request(request));
  settle(&f);
  g_assert_cmpstr(field(shown(GH_NOTIFIER_ID_CONVERSATION_PREFIX "1"), "title"), ==,
                  gh_conversation_get_title(p));
  g_assert_cmpstr(field(shown(GH_NOTIFIER_ID_CONVERSATION_PREFIX "1"), "body"), ==,
                  "3 new messages");
  g_assert_cmpstr(field(shown(GH_NOTIFIER_ID_CONVERSATION_PREFIX "2"), "title"), ==,
                  gh_conversation_get_title(q));
  /* The request stays in the hidden, count-only notification. */
  g_assert_cmpstr(field(shown(GH_NOTIFIER_ID_MESSAGES), "title"), ==, "New message");
  g_assert_cmpstr(field(shown(GH_NOTIFIER_ID_MESSAGES), "body"), ==, "1 new message");
  g_assert_cmpuint(fake_gtk_notifications_count(fake, TRUE, NULL), ==, 3);
  assert_identity_hidden(PEER[3]);
  const gchar *const secrets[] = { CANARY, NULL };
  assert_payloads_hide(secrets);
  fixture_down(&f);

  /* No marker (the store's first session): what was written before binding
   * is never notified, so a first backfill is not replayed. */
  fixture_up(&f, "hidden", FALSE);
  f.last_seen = 0;
  gh_conversation_store_set_account(f.model, ACCOUNT_A, NULL, NULL, NULL);
  open_room(&f, PEER[0], NULL);
  Rumor before = { .author = PEER[0], .to = { ACCOUNT_A }, .content = "history",
                   .created_at = T0 - 1 };
  deliver(&f, &before);
  assert_nothing_sent(&f);
  receive(&f, PEER[0], NULL, "new");
  settle(&f);
  g_assert_cmpuint(fake_gtk_notifications_count(fake, TRUE, NULL), ==, 1);
  fixture_down(&f);
}

/* ---- NO-4 coalescing ------------------------------------------------------------------------- */

static void
assert_spaced(GArray *times, gint64 interval_us)
{
  for (guint i = 1; i < times->len; i++)
    g_assert_cmpint(g_array_index(times, gint64, i) - g_array_index(times, gint64, i - 1), >=,
                    interval_us);
}

static void
test_no4_burst(void)
{
  Fixture f;
  fixture_up(&f, "sender", TRUE);
  g_settings_set_boolean(f.settings, "sound-enabled", TRUE);
  GhConversation *rooms[5];
  for (guint i = 0; i < 5; i++)
    rooms[i] = open_room(&f, PEER[i], NULL);
  /* 100 messages across 5 conversations in 10 s. */
  for (guint i = 0; i < 100; i++) {
    g_autofree gchar *text = g_strdup_printf("burst %u", i);
    receive(&f, PEER[i % 5], NULL, text);
    settle(&f);
    advance_ms(&f, 100);
  }
  advance_ms(&f, 2 * GH_NOTIFIER_UPDATE_INTERVAL_MS);
  GPtrArray *calls = fake_gtk_notifications_get_calls(fake);
  for (guint r = 0; r < 5; r++) {
    g_autofree gchar *id = g_strdup_printf(GH_NOTIFIER_ID_CONVERSATION_PREFIX "%u", r + 1);
    g_autoptr(GArray) times = g_array_new(FALSE, FALSE, sizeof(gint64));
    for (guint i = 0; i < calls->len; i++) {
      FakeNotificationCall *call = g_ptr_array_index(calls, i);
      if (call->added && g_str_equal(call->id, id))
        g_array_append_val(times, call->at);
    }
    /* At most one update per 2 s per id: 10 s of traffic, 5 or 6 sends. */
    g_assert_cmpuint(times->len, >=, 2);
    g_assert_cmpuint(times->len, <=, 7);
    assert_spaced(times, GH_NOTIFIER_UPDATE_INTERVAL_MS * 1000);
    /* The last update counts all twenty. */
    g_assert_cmpstr(field(shown(id), "title"), ==, gh_conversation_get_title(rooms[r]));
    g_assert_cmpstr(field(shown(id), "body"), ==, "20 new messages");
  }
  /* Sound at most once per 10 s. */
  g_assert_cmpuint(f.sounds->len, >=, 1);
  g_assert_cmpuint(f.sounds->len, <=, 2);
  assert_spaced(f.sounds, GH_NOTIFIER_SOUND_INTERVAL_MS * 1000);
  fixture_down(&f);
}

/* A backlog admitted in one turn (100 messages) is one notification. */
static void
test_no4_one_turn(void)
{
  Fixture f;
  fixture_up(&f, "sender", TRUE);
  g_settings_set_boolean(f.settings, "sound-enabled", TRUE);
  open_room(&f, PEER[0], NULL);
  for (guint i = 0; i < 100; i++) {
    Rumor r = { .author = PEER[0], .to = { ACCOUNT_A }, .created_at = T0 + i, .content = "x" };
    deliver(&f, &r);
  }
  settle(&f);
  advance_ms(&f, 3 * GH_NOTIFIER_UPDATE_INTERVAL_MS);
  g_assert_cmpuint(fake_gtk_notifications_count(fake, TRUE, NULL), ==, 1);
  g_assert_cmpstr(field(shown(GH_NOTIFIER_ID_CONVERSATION_PREFIX "1"), "body"), ==,
                  "100 new messages");
  g_assert_cmpuint(f.sounds->len, ==, 1);
  /* Sound stays off unless chosen. */
  g_settings_set_boolean(f.settings, "sound-enabled", FALSE);
  advance_ms(&f, GH_NOTIFIER_SOUND_INTERVAL_MS);
  receive(&f, PEER[0], NULL, "quiet");
  settle(&f);
  g_assert_cmpuint(fake_gtk_notifications_count(fake, TRUE, NULL), ==, 2);
  g_assert_cmpuint(f.sounds->len, ==, 1);
  fixture_down(&f);
}

/* ---- NO-5 withdrawal ------------------------------------------------------------------------- */

static void
assert_withdrawn(const gchar *id)
{
  g_assert_null(shown(id));
  g_assert_cmpuint(fake_gtk_notifications_count(fake, FALSE, id), >=, 1);
}

static void
test_no5_withdrawal(void)
{
  Fixture f;
  fixture_up(&f, "sender", TRUE);
  const gchar *conv1 = GH_NOTIFIER_ID_CONVERSATION_PREFIX "1";
  const gchar *conv2 = GH_NOTIFIER_ID_CONVERSATION_PREFIX "2";
  GhConversation *p = open_room(&f, PEER[0], NULL);
  GhConversation *q = open_room(&f, PEER[1], NULL);

  /* Opened in the active window. */
  receive(&f, PEER[0], NULL, "a");
  settle(&f);
  g_assert_nonnull(shown(conv1));
  gh_notifier_set_visible_conversation(f.notifier, p);
  sync_calls(&f);
  assert_withdrawn(conv1);
  gh_notifier_set_visible_conversation(f.notifier, NULL);

  /* Read (marked read anywhere). */
  receive(&f, PEER[1], NULL, "b");
  settle(&f);
  g_assert_nonnull(shown(conv2));
  gh_conversation_mark_read(q);
  sync_calls(&f);
  assert_withdrawn(conv2);

  /* Purge: a message it counts disappears (NIP-40) at its expiry. */
  advance_ms(&f, GH_NOTIFIER_UPDATE_INTERVAL_MS);
  gint64 now = gh_clock_get_unix(f.clock);
  Rumor fleeting = { .author = PEER[0], .to = { ACCOUNT_A }, .content = "fleeting",
                     .created_at = now, .expiration = now + 60 };
  deliver(&f, &fleeting);
  settle(&f);
  g_assert_nonnull(shown(conv1));
  fake_gtk_notifications_clear(fake);
  advance_ms(&f, 59 * 1000);
  g_assert_cmpuint(fake_gtk_notifications_count(fake, FALSE, conv1), ==, 0);
  advance_ms(&f, 1000);
  assert_withdrawn(conv1);
  gh_conversation_mark_read(p);

  /* Muted or blocked while shown (the caller's hook). */
  advance_ms(&f, GH_NOTIFIER_UPDATE_INTERVAL_MS);
  receive(&f, PEER[0], NULL, "c");
  settle(&f);
  g_assert_nonnull(shown(conv1));
  gh_notifier_withdraw_conversation(f.notifier, gh_conversation_get_room_id(p));
  sync_calls(&f);
  assert_withdrawn(conv1);
  gh_conversation_mark_read(p);

  /* The content level changes, or notifications are turned off. */
  advance_ms(&f, GH_NOTIFIER_UPDATE_INTERVAL_MS);
  receive(&f, PEER[0], NULL, "d");
  settle(&f);
  g_assert_nonnull(shown(conv1));
  g_settings_set_string(f.settings, "notification-privacy", "hidden");
  sync_calls(&f);
  assert_withdrawn(conv1);
  advance_ms(&f, GH_NOTIFIER_UPDATE_INTERVAL_MS);
  receive(&f, PEER[0], NULL, "e");
  settle(&f);
  g_assert_nonnull(shown(GH_NOTIFIER_ID_MESSAGES));
  g_settings_set_boolean(f.settings, "notifications-enabled", FALSE);
  sync_calls(&f);
  assert_withdrawn(GH_NOTIFIER_ID_MESSAGES);
  g_settings_set_boolean(f.settings, "notifications-enabled", TRUE);
  g_settings_set_string(f.settings, "notification-privacy", "sender");
  gh_conversation_mark_read(p);

  /* Forget: the conversation is unlisted. */
  advance_ms(&f, GH_NOTIFIER_UPDATE_INTERVAL_MS);
  receive(&f, PEER[0], NULL, "f");
  settle(&f);
  g_assert_nonnull(shown(conv1));
  g_assert_true(gh_conversation_store_remove(f.model, gh_conversation_get_room_id(p)));
  sync_calls(&f);
  assert_withdrawn(conv1);

  /* Account switch: every id. */
  advance_ms(&f, GH_NOTIFIER_UPDATE_INTERVAL_MS);
  receive(&f, PEER[1], NULL, "g");
  receive(&f, PEER[3], NULL, "request");
  settle(&f);
  g_assert_nonnull(shown(conv2));
  g_assert_nonnull(shown(GH_NOTIFIER_ID_MESSAGES));
  gh_conversation_store_set_account(f.model, ACCOUNT_B, NULL, NULL, NULL);
  sync_calls(&f);
  assert_withdrawn(conv2);
  assert_withdrawn(GH_NOTIFIER_ID_MESSAGES);
  fixture_down(&f);
}

/* ---- NO-6 stale activation ------------------------------------------------------------------- */

static void
activate(Fixture *f, GVariant *target)
{
  g_action_group_activate_action(G_ACTION_GROUP(f->app), GH_NOTIFIER_ACTION, target);
}

static void
test_no6_stale(void)
{
  Fixture f;
  fixture_up(&f, "sender", TRUE);
  GhConversation *a_room = open_room(&f, PEER[0], NULL);
  receive(&f, PEER[0], NULL, "for a");
  settle(&f);
  g_autoptr(GVariant) target_a = target_of(GH_NOTIFIER_ID_CONVERSATION_PREFIX "1");

  /* Current: the application comes up and shows the conversation. */
  activate(&f, target_a);
  g_assert_cmpuint(f.activations, ==, 1);
  g_assert_cmpuint(f.opened->len, ==, 1);
  g_assert_true(g_ptr_array_index(f.opened, 0) == a_room);
  g_assert_cmpuint(f.stale->len, ==, 0);

  /* Account B, with its own conversation 1. */
  gh_conversation_store_set_account(f.model, ACCOUNT_B, NULL, NULL, NULL);
  GhConversation *b_room = open_room(&f, PEER[0], NULL);
  advance_ms(&f, GH_NOTIFIER_UPDATE_INTERVAL_MS);
  receive(&f, PEER[0], NULL, "for b");
  settle(&f);
  g_autoptr(GVariant) target_b = target_of(GH_NOTIFIER_ID_CONVERSATION_PREFIX "1");
  guint64 generation_a = 0, generation_b = 0;
  gint64 number_a = 0, number_b = 0;
  g_variant_get(target_a, "(tx)", &generation_a, &number_a);
  g_variant_get(target_b, "(tx)", &generation_b, &number_b);
  g_assert_cmpint(number_a, ==, number_b);
  g_assert_cmpuint(generation_a, !=, generation_b);

  /* A's notification never opens B's (or A's) thread. */
  activate(&f, target_a);
  g_assert_cmpuint(f.activations, ==, 2);
  g_assert_cmpuint(f.opened->len, ==, 1);
  g_assert_cmpuint(f.stale->len, ==, 1);
  g_autofree gchar *npub_a = npub_of(ACCOUNT_A, FALSE);
  g_assert_cmpstr(g_ptr_array_index(f.stale, 0), ==, npub_a);
  activate(&f, target_b);
  g_assert_cmpuint(f.opened->len, ==, 2);
  g_assert_true(g_ptr_array_index(f.opened, 1) == b_room);

  /* A conversation number this generation never gave out opens nothing. */
  activate(&f, g_variant_new("(tx)", generation_b, (gint64)99));
  g_assert_cmpuint(f.opened->len, ==, 2);
  g_assert_cmpuint(f.stale->len, ==, 1);

  /* A new process: every earlier target is stale, its account unknown. */
  g_object_run_dispose(G_OBJECT(f.notifier));
  g_object_unref(f.notifier);
  f.notifier = notifier_new(&f);
  activate(&f, target_b);
  g_assert_cmpuint(f.opened->len, ==, 2);
  g_assert_cmpuint(f.stale->len, ==, 2);
  g_assert_cmpstr(g_ptr_array_index(f.stale, 1), ==, "");
  fixture_down(&f);
}

/* ---- NO-7 mute storage ------------------------------------------------------------------------ */

static void
test_no7_mute_storage(void)
{
  Fixture f;
  fixture_up(&f, "preview", FALSE);
  g_autoptr(GError) error = NULL;
  guint8 raw[GH_STORE_KEY_SIZE];
  for (guint i = 0; i < sizeof raw; i++)
    raw[i] = g_random_int_range(0, 256);
  g_autoptr(GBytes) key = g_bytes_new(raw, sizeof raw);
  g_autofree gchar *store_id = g_uuid_string_random();
  GhStoreConfig config = { .account_pubkey = ACCOUNT_A, .clock = f.clock };
  GhStore *store = gh_store_open_with_key(&config, key, store_id, GH_STORE_OPEN_CREATE, &error);
  g_assert_no_error(error);
  f.conversations = gh_store_conversations_new(store);
  g_assert_true(gh_store_conversations_attach(f.conversations, f.model, 0, &error));
  g_assert_no_error(error);

  GhConversation *p = open_room(&f, PEER[0], NULL);
  const gchar *room = gh_conversation_get_room_id(p);
  g_assert_true(gh_store_conversations_set_muted_until(f.conversations, room,
                                                       GH_STORE_CONVERSATIONS_MUTED_ALWAYS,
                                                       &error));
  g_assert_no_error(error);
  receive(&f, PEER[0], NULL, "muted");
  assert_nothing_sent(&f);

  /* The mute is in the database; GSettings holds nothing of the
   * conversation. */
  GhStoreNotifyState state = { 0 };
  g_assert_true(gh_store_conversations_get_notify_state(f.conversations, room, &state, &error));
  g_assert_cmpint(state.muted_until, ==, GH_STORE_CONVERSATIONS_MUTED_ALWAYS);
  g_autoptr(GSettingsSchema) schema = NULL;
  g_object_get(f.settings, "settings-schema", &schema, NULL);
  g_auto(GStrv) keys = g_settings_schema_list_keys(schema);
  g_autoptr(GString) dump = g_string_new(NULL);
  for (guint i = 0; keys[i]; i++) {
    g_autoptr(GVariant) value = g_settings_get_value(f.settings, keys[i]);
    g_autofree gchar *printed = g_variant_print(value, FALSE);
    g_string_append_printf(dump, "%s=%s\n", keys[i], printed);
  }
  g_autofree gchar *peer_npub = npub_of(PEER[0], FALSE);
  g_autofree gchar *peer_head = g_strndup(peer_npub, 15);
  assert_absent(dump->str, room);
  assert_absent(dump->str, PEER[0]);
  assert_absent(dump->str, peer_head);
  g_assert_null(strstr(dump->str, "mute"));

  /* Unmuted in the store: notified again. */
  g_assert_true(gh_store_conversations_set_muted_until(f.conversations, room, 0, &error));
  gh_conversation_mark_read(p);
  receive(&f, PEER[0], NULL, "unmuted");
  settle(&f);
  g_assert_cmpstr(field(shown(GH_NOTIFIER_ID_CONVERSATION_PREFIX "1"), "body"), ==, "unmuted");

  /* Muted while shown: the next update reads the store and withdraws it. */
  g_assert_true(gh_store_conversations_set_muted_until(f.conversations, room,
    gh_clock_get_unix(f.clock) + 3600, &error));
  advance_ms(&f, GH_NOTIFIER_UPDATE_INTERVAL_MS);
  receive(&f, PEER[0], NULL, "while muted");
  advance_ms(&f, GH_NOTIFIER_UPDATE_INTERVAL_MS);
  assert_withdrawn(GH_NOTIFIER_ID_CONVERSATION_PREFIX "1");

  gh_store_conversations_close(f.conversations);
  gh_conversation_store_set_account(f.model, NULL, NULL, NULL, NULL);
  fixture_down(&f);
  gh_store_close(store);
}

/* ---- NO-8 category and priority ------------------------------------------------------------- */

/* The category, as GLib's freedesktop backend hands it to a notification
 * server: category im.received, urgency normal (1), never critical. GLib
 * picks the backend once per process, so this runs in a child with its own
 * bus and fake. */
static void
no8_freedesktop(void)
{
  Fixture f;
  fixture_up(&f, "preview", TRUE);
  open_room(&f, PEER[0], NULL);
  receive(&f, PEER[0], NULL, "fdo");
  receive(&f, PEER[3], NULL, "fdo request");
  settle(&f);
  GPtrArray *notifies = fake_gtk_notifications_get_fdo_notifies(fake);
  g_assert_cmpuint(notifies->len, ==, 2);
  for (guint i = 0; i < notifies->len; i++) {
    g_autoptr(GVariant) hints = g_variant_get_child_value(g_ptr_array_index(notifies, i), 6);
    const gchar *category = NULL;
    guchar urgency = 0xff;
    g_assert_true(g_variant_lookup(hints, "category", "&s", &category));
    g_assert_cmpstr(category, ==, GH_NOTIFIER_CATEGORY);
    g_assert_true(g_variant_lookup(hints, "urgency", "y", &urgency));
    g_assert_cmpuint(urgency, ==, 1);
  }
  g_assert_cmpuint(fake_gtk_notifications_count(fake, TRUE, NULL), ==, 0);
  fixture_down(&f);
}

static void
test_no8_category_priority(void)
{
  if (g_test_subprocess()) {
    no8_freedesktop();
    return;
  }
  static const gchar *const levels[] = { "hidden", "sender", "preview", "unknown-level" };
  for (guint l = 0; l < G_N_ELEMENTS(levels); l++) {
    Fixture f;
    fixture_up(&f, levels[l], TRUE);
    open_room(&f, PEER[0], NULL);
    receive(&f, PEER[0], NULL, CANARY);
    receive(&f, PEER[3], NULL, CANARY " request");
    settle(&f);
    g_assert_cmpuint(fake_gtk_notifications_count(fake, TRUE, NULL), >=, 1);
    /* An unknown level counts as hidden. */
    if (l == 3 || l == 0) {
      g_assert_cmpuint(fake_gtk_notifications_count(fake, TRUE, NULL), ==, 1);
      assert_identity_hidden(PEER[0]);
    }
    g_autofree gchar *dump = fake_gtk_notifications_dump(fake);
    assert_absent(dump, "urgent");
    assert_absent(dump, "'high'");
    fixture_down(&f); /* every payload: priority normal, no icon or buttons */
  }
  g_auto(GStrv) envp = g_environ_setenv(g_get_environ(), "GNOTIFICATION_BACKEND",
                                        "freedesktop", TRUE);
  g_test_trap_subprocess_with_envp(NULL, (const gchar *const *)envp, 0,
                                   G_TEST_SUBPROCESS_INHERIT_STDERR);
  g_test_trap_assert_passed();
}

/* ---- NO-11 the locked-store notice --------------------------------------------------------- */

static void
test_no11_locked_notice(void)
{
  Fixture f;
  fixture_up(&f, "preview", TRUE);
  gh_notifier_set_store_locked(f.notifier, TRUE);
  gh_notifier_set_store_locked(f.notifier, TRUE);
  sync_calls(&f);
  g_assert_cmpuint(fake_gtk_notifications_count(fake, TRUE, NULL), ==, 1);
  GVariant *n = shown(GH_NOTIFIER_ID_STORE_LOCKED);
  g_assert_cmpstr(field(n, "title"), ==, "Unlock to receive messages");
  /* Activating it only opens the application, whose window offers Unlock. */
  g_assert_null(field(n, "default-action"));
  assert_identity_hidden(PEER[0]);
  g_settings_set_boolean(f.settings, "notifications-enabled", FALSE);
  sync_calls(&f);
  assert_withdrawn(GH_NOTIFIER_ID_STORE_LOCKED);
  g_settings_set_boolean(f.settings, "notifications-enabled", TRUE);
  sync_calls(&f);
  g_assert_nonnull(shown(GH_NOTIFIER_ID_STORE_LOCKED));
  gh_notifier_set_store_locked(f.notifier, FALSE);
  sync_calls(&f);
  assert_withdrawn(GH_NOTIFIER_ID_STORE_LOCKED);
  g_assert_cmpuint(fake_gtk_notifications_count(fake, TRUE, NULL), ==, 2);
  fixture_down(&f);
}

/* NO-11 with the default settings (W14 review non-blocking #1): the notice
 * names nothing, so it shows while notifications-enabled is merely at its
 * default (off until onboarding asks, PD-9); message notifications stay
 * off. Only the user's own "off" keeps the notice back (charter §5.3 B4). */
static void
test_no11_default_settings(void)
{
  Fixture f;
  fixture_up(&f, "hidden", TRUE);
  g_settings_reset(f.settings, "notifications-enabled");
  g_autoptr(GVariant) user = g_settings_get_user_value(f.settings, "notifications-enabled");
  g_assert_null(user);
  g_assert_false(g_settings_get_boolean(f.settings, "notifications-enabled"));
  sync_calls(&f);
  guint before = fake_gtk_notifications_count(fake, TRUE, NULL);

  gh_notifier_set_store_locked(f.notifier, TRUE);
  sync_calls(&f);
  GVariant *n = shown(GH_NOTIFIER_ID_STORE_LOCKED);
  g_assert_nonnull(n);
  g_assert_cmpstr(field(n, "title"), ==, "Unlock to receive messages");
  g_assert_null(field(n, "default-action"));
  assert_identity_hidden(PEER[0]);
  g_assert_cmpuint(fake_gtk_notifications_count(fake, TRUE, NULL), ==, before + 1);

  /* The user turns notifications off: withdrawn, and none while it stays off. */
  g_settings_set_boolean(f.settings, "notifications-enabled", FALSE);
  sync_calls(&f);
  assert_withdrawn(GH_NOTIFIER_ID_STORE_LOCKED);
  gh_notifier_set_store_locked(f.notifier, FALSE);
  gh_notifier_set_store_locked(f.notifier, TRUE);
  sync_calls(&f);
  g_assert_cmpuint(fake_gtk_notifications_count(fake, TRUE, NULL), ==, before + 1);

  /* Back to the default: shown again. */
  g_settings_reset(f.settings, "notifications-enabled");
  sync_calls(&f);
  g_assert_nonnull(shown(GH_NOTIFIER_ID_STORE_LOCKED));
  g_assert_cmpuint(fake_gtk_notifications_count(fake, TRUE, NULL), ==, before + 2);
  gh_notifier_set_store_locked(f.notifier, FALSE);
  sync_calls(&f);
  assert_withdrawn(GH_NOTIFIER_ID_STORE_LOCKED);
  fixture_down(&f);
}

/* ---- --gui: the window glue ------------------------------------------------------------------ */

static gboolean
has_toast(GtkWidget *widget, const gchar *title, const gchar *button)
{
  gboolean title_seen = FALSE, button_seen = button == NULL;
  g_autoptr(GPtrArray) stack = g_ptr_array_new();
  g_ptr_array_add(stack, widget);
  while (stack->len) {
    GtkWidget *w = g_ptr_array_steal_index(stack, stack->len - 1);
    if (GTK_IS_LABEL(w) && g_strcmp0(gtk_label_get_label(GTK_LABEL(w)), title) == 0)
      title_seen = TRUE;
    if (button && GTK_IS_BUTTON(w) &&
        g_strcmp0(gtk_button_get_label(GTK_BUTTON(w)), button) == 0)
      button_seen = TRUE;
    for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
      g_ptr_array_add(stack, c);
  }
  return title_seen && button_seen;
}

static void
spin(void)
{
  while (g_main_context_iteration(NULL, FALSE))
    ;
}

static void
test_gui_open_and_stale(void)
{
  Fixture f = { 0 };
  f.clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
  f.settings = g_settings_new(APP_SCHEMA);
  f.app = g_application_new("org.nostr.GroundhogTest.NotifierGui", G_APPLICATION_DEFAULT_FLAGS);
  f.mutes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  f.opened = g_ptr_array_new_with_free_func(g_object_unref);
  f.stale = g_ptr_array_new_with_free_func(g_free);
  f.sounds = g_array_new(FALSE, FALSE, sizeof(gint64));
  f.model = gh_conversation_store_new();
  /* Notifications stay off (the default): nothing is sent from here. */
  f.notifier = notifier_new(&f);
  gh_conversation_store_set_account(f.model, ACCOUNT_A, NULL, NULL, NULL);
  GhConversation *accepted = open_room(&f, PEER[0], NULL);
  GhConversation *request = receive(&f, PEER[3], NULL, "request");
  GhWindow *window = gh_window_new(NULL);
  gh_conversation_list_attach(window, f.model, NULL);
  gh_notifier_attach_window(f.notifier, window);
  gtk_window_present(GTK_WINDOW(window));
  spin();
  GhSidebarPage *sidebar = gh_window_get_sidebar(window);

  /* Open: the conversation is selected and shown, from whichever list. */
  GhTestActiveSpan span;
  gh_test_active_span_begin(&span, GTK_WINDOW(window));
  g_signal_emit_by_name(f.notifier, "open-conversation", request);
  g_assert_true(gh_sidebar_page_get_selected(sidebar) == request);
  g_assert_true(gh_sidebar_page_get_show_requests(sidebar));
  g_assert_true(gh_window_get_content_visible(window));
  g_signal_emit_by_name(f.notifier, "open-conversation", accepted);
  g_assert_true(gh_sidebar_page_get_selected(sidebar) == accepted);
  g_assert_false(gh_sidebar_page_get_show_requests(sidebar));
  /* The notifier follows the shown conversation only while the window is
   * active (a display without a window manager may never activate it). */
  spin();
  gboolean active = FALSE;
  GhConversation *visible = gh_notifier_get_visible_conversation(f.notifier);
  if (gh_test_active_span_end(&span, &active))
    g_assert_true(visible == (active ? accepted : NULL));
  else /* activation changed meanwhile (nostrc-9g6e) */
    g_assert_true(visible == accepted || visible == NULL);

  /* Stale: the list, no thread, and the toast with Switch for a known
   * account. */
  g_autofree gchar *npub_b = npub_of(ACCOUNT_B, FALSE);
  g_signal_emit_by_name(f.notifier, "stale-activation", npub_b);
  spin();
  g_assert_null(gh_sidebar_page_get_selected(sidebar));
  g_assert_false(gh_sidebar_page_get_show_requests(sidebar));
  g_assert_null(gh_notifier_get_visible_conversation(f.notifier));
  g_assert_true(has_toast(GTK_WIDGET(gh_window_get_toasts(window)),
                          "That notification was for another account", "Switch"));

  /* The window gone, nothing is visible and the glue lets go. */
  gtk_window_destroy(GTK_WINDOW(window));
  spin();
  g_assert_null(gh_notifier_get_visible_conversation(f.notifier));
  g_signal_emit_by_name(f.notifier, "stale-activation", NULL);

  g_object_run_dispose(G_OBJECT(f.notifier));
  g_clear_object(&f.notifier);
  g_clear_object(&f.model);
  g_clear_object(&f.app);
  g_clear_object(&f.settings);
  g_hash_table_unref(f.mutes);
  g_ptr_array_unref(f.opened);
  g_ptr_array_unref(f.stale);
  g_array_unref(f.sounds);
  g_clear_pointer(&f.clock, gh_clock_unref);
}

/* Two runs (two CTest entries), as in test_background.c: the default on the
 * shared private bus without GTK, and --gui with a display and no private
 * bus. */
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
  /* GLib's GTK backend: what a GNOME desktop receives. */
  g_setenv("GNOTIFICATION_BACKEND", "gtk", FALSE); /* NO-8's child chooses its own */
  gboolean gui_available = FALSE;
  if (gui_mode) {
#ifdef __APPLE__
    /* CLI test runners may not own a macOS WindowServer session. */
    gboolean display = g_strcmp0(g_getenv("GROUNDHOG_RUN_GUI_SMOKE"), "1") == 0;
#else
    gboolean display = (g_getenv("DISPLAY") && *g_getenv("DISPLAY")) ||
                       (g_getenv("WAYLAND_DISPLAY") && *g_getenv("WAYLAND_DISPLAY"));
#endif
    /* Before g_test_init(), as the other GUI tests: a host theme's parser
     * warnings are not this test's. */
    gui_available = display && gtk_init_check();
    if (gui_available) {
      adw_init();
      groundhog_register_resource();
      g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
    }
  }
  /* The store (NO-7) writes under private XDG directories. The GUI run
   * writes nothing and keeps the system's: isolated XDG_DATA_DIRS would hide
   * the MIME database that icon loading needs. */
  if (gui_mode)
    g_test_init(&argc, &argv, NULL);
  else
    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
  ACCOUNT_A = hex_of("account-a");
  ACCOUNT_B = hex_of("account-b");
  for (guint i = 0; i < G_N_ELEMENTS(PEER); i++) {
    g_autofree gchar *seed = g_strdup_printf("peer-%u", i);
    PEER[i] = hex_of(seed);
  }
  int status;
  if (gui_mode) {
    if (!gui_available) {
      g_printerr("Groundhog notifier GUI tests skipped: no graphical display\n");
      status = 77;
      goto out;
    }
    g_test_add_func("/groundhog/notifier-gui/open-and-stale", test_gui_open_and_stale);
    status = g_test_run();
    goto out;
  }
  if (!nostrc_test_bus_available()) {
    g_printerr("Groundhog notifier tests skipped: no dbus-daemon\n");
    status = 77;
    goto out;
  }
  bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  nostrc_test_bus_up(bus);
  fake = fake_gtk_notifications_new(nostrc_test_bus_connect(bus));
  nostrc_test_bus_add_func("/groundhog/notifier/no1-hidden", test_no1_hidden);
  nostrc_test_bus_add_func("/groundhog/notifier/no1-sender", test_no1_sender);
  nostrc_test_bus_add_func("/groundhog/notifier/no1-preview", test_no1_preview);
  nostrc_test_bus_add_func("/groundhog/notifier/no2-forced-hidden", test_no2_forced_hidden);
  nostrc_test_bus_add_func("/groundhog/notifier/no3-suppression", test_no3_suppression);
  nostrc_test_bus_add_func("/groundhog/notifier/no4-burst", test_no4_burst);
  nostrc_test_bus_add_func("/groundhog/notifier/no4-one-turn", test_no4_one_turn);
  nostrc_test_bus_add_func("/groundhog/notifier/no5-withdrawal", test_no5_withdrawal);
  nostrc_test_bus_add_func("/groundhog/notifier/no6-stale", test_no6_stale);
  nostrc_test_bus_add_func("/groundhog/notifier/no7-mute-storage", test_no7_mute_storage);
  nostrc_test_bus_add_func("/groundhog/notifier/no8-category-priority",
                           test_no8_category_priority);
  nostrc_test_bus_add_func("/groundhog/notifier/no11-locked-notice", test_no11_locked_notice);
  nostrc_test_bus_add_func("/groundhog/notifier/no11-default-settings",
                           test_no11_default_settings);
  nostrc_test_bus_add_func("/groundhog/notifier/read-by-arrival", test_read_by_arrival);
  nostrc_test_bus_add_func("/groundhog/notifier/backfill", test_backfill);
  status = g_test_run();
  fake_gtk_notifications_free(fake);
  nostrc_test_bus_down(bus);
out:
  g_free(ACCOUNT_A);
  g_free(ACCOUNT_B);
  for (guint i = 0; i < G_N_ELEMENTS(PEER); i++)
    g_free(PEER[i]);
  return status;
}
