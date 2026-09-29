#include "gh-notifier.h"

#include <glib/gi18n.h>
#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>
#include <pango/pango.h>
#include <stdlib.h>
#include <string.h>

#define NOTIFIER_DATA "groundhog-notifier"
#define KEY_ENABLED "notifications-enabled"
#define KEY_PRIVACY "notification-privacy"
#define KEY_SOUND "sound-enabled"
/* Earlier account generations whose account the stale toast may still name. */
#define MAX_STALE 16
#define NEVER G_MININT64

typedef enum { LEVEL_HIDDEN, LEVEL_SENDER, LEVEL_PREVIEW } Level;

/* The new incoming messages of one conversation that a notification counts. */
typedef struct {
  GhNotifier *self;
  gchar *room_id;
  GhConversation *conversation;
  GPtrArray *messages;  /* GhMessage, in message order */
  gboolean fresh;       /* a message arrived since this room was last sent */
  guint expiry;         /* GhClock timeout: the earliest expires_at among them */
  gint64 expiry_at;
} Room;

/* One notification id. */
typedef struct {
  GhNotifier *self;
  gchar *id;
  gint64 last_sent;     /* monotonic µs, NEVER */
  guint timer;          /* GhClock timeout of the next send */
  gboolean shown;
} Slot;

struct _GhNotifier {
  GObject parent_instance;
  GApplication *app;        /* weak */
  GSettings *settings;
  GhConversationStore *model;
  GhClock *clock;
  GhNotifierRoomStateFunc room_state;
  gpointer room_state_data;
  GSimpleAction *action;

  gchar *account;           /* the model's account, last seen */
  guint64 generation;       /* the targets' t: random base, +1 per account switch */
  GHashTable *stale;        /* guint64 generation -> account hex */
  GQueue stale_order;       /* guint64 *, oldest first */
  gint64 session_start;     /* unix seconds: the model bound the account */
  guint next_number;
  GHashTable *numbers;      /* room id -> number */
  GHashTable *number_rooms; /* number -> room id */
  GHashTable *rooms;        /* room id -> Room */
  GHashTable *slots;        /* id -> Slot */
  GhConversation *visible;
  gint64 last_sound;        /* monotonic µs, NEVER */
  gboolean store_locked;
  gboolean notice_shown;
  GhWindow *window;         /* weak */
  GhSidebarPage *sidebar;   /* weak: the window's, which it clears before "destroy" */
  AdwNavigationSplitView *split; /* weak */
};

enum { SIGNAL_OPEN_CONVERSATION, SIGNAL_STALE_ACTIVATION, SIGNAL_PLAY_SOUND, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhNotifier, gh_notifier, G_TYPE_OBJECT)

static void refresh(GhNotifier *self, gboolean ask_store);

/* ---- helpers ------------------------------------------------------------------- */

static gint64
now_unix(GhNotifier *self)
{
  return gh_clock_get_unix(self->clock);
}

static gboolean
enabled(GhNotifier *self)
{
  return g_settings_get_boolean(self->settings, KEY_ENABLED);
}

/* Anything but a known level counts as hidden. */
static Level
level_of(GhNotifier *self)
{
  g_autofree gchar *value = g_settings_get_string(self->settings, KEY_PRIVACY);
  if (g_strcmp0(value, "preview") == 0)
    return LEVEL_PREVIEW;
  if (g_strcmp0(value, "sender") == 0)
    return LEVEL_SENDER;
  return LEVEL_HIDDEN;
}

/* "npub1…" of a lowercase hex pubkey, or "npub1abcde…wxyz" when short. */
static gchar *
npub_of(const gchar *hex, gboolean short_form)
{
  guint8 bytes[32];
  char *npub = NULL;
  if (!hex || strlen(hex) != 64 || !nostr_hex2bin(bytes, hex, sizeof bytes) ||
      nostr_nip19_encode_npub(bytes, &npub) != 0 || !npub)
    return NULL;
  gsize length = strlen(npub);
  gchar *out = short_form && length > 16 ? g_strdup_printf("%.10s…%s", npub, npub + length - 4)
                                         : g_strdup(npub);
  free(npub);
  return out;
}

static guint
number_of(GhNotifier *self, const gchar *room_id)
{
  gpointer number = g_hash_table_lookup(self->numbers, room_id);
  if (number)
    return GPOINTER_TO_UINT(number);
  guint assigned = self->next_number++;
  g_hash_table_insert(self->numbers, g_strdup(room_id), GUINT_TO_POINTER(assigned));
  g_hash_table_insert(self->number_rooms, GUINT_TO_POINTER(assigned), g_strdup(room_id));
  return assigned;
}

/* Requests always share the app-wide, count-only notification. */
static gchar *
slot_id_for(GhNotifier *self, Room *room, Level level)
{
  if (level == LEVEL_HIDDEN || gh_conversation_get_is_request(room->conversation))
    return g_strdup(GH_NOTIFIER_ID_MESSAGES);
  return g_strdup_printf(GH_NOTIFIER_ID_CONVERSATION_PREFIX "%u",
                         number_of(self, room->room_id));
}

static GhMessage *
room_newest(Room *room)
{
  return room->messages->len ? g_ptr_array_index(room->messages, room->messages->len - 1)
                             : NULL;
}

/* ---- rooms ----------------------------------------------------------------------- */

static void
room_free(gpointer data)
{
  Room *room = data;
  g_signal_handlers_disconnect_by_data(room->conversation, room->self);
  if (room->expiry)
    gh_clock_source_remove(room->self->clock, room->expiry);
  g_ptr_array_unref(room->messages);
  g_object_unref(room->conversation);
  g_free(room->room_id);
  g_free(room);
}

static Slot *slot_get(GhNotifier *self, const gchar *id);
static void schedule(GhNotifier *self, Slot *slot);

/* Read, accepted (its messages move to their own id), or a message purged
 * from it. */
static void
on_room_changed(GhNotifier *self)
{
  refresh(self, FALSE);
  GHashTableIter iter;
  Room *room;
  g_hash_table_iter_init(&iter, self->rooms);
  while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&room)) {
    if (!room->fresh)
      continue;
    g_autofree gchar *id = slot_id_for(self, room, level_of(self));
    schedule(self, slot_get(self, id));
  }
}

static Room *
room_get(GhNotifier *self, GhConversation *conversation)
{
  const gchar *room_id = gh_conversation_get_room_id(conversation);
  Room *room = g_hash_table_lookup(self->rooms, room_id);
  if (room && room->conversation == conversation)
    return room;
  room = g_new0(Room, 1);
  room->self = self;
  room->room_id = g_strdup(room_id);
  room->conversation = g_object_ref(conversation);
  room->messages = g_ptr_array_new_with_free_func(g_object_unref);
  g_signal_connect_swapped(conversation, "notify::unread-count", G_CALLBACK(on_room_changed),
                           self);
  g_signal_connect_swapped(conversation, "notify::is-request", G_CALLBACK(on_room_changed),
                           self);
  g_signal_connect_swapped(conversation, "items-changed", G_CALLBACK(on_room_changed), self);
  g_hash_table_replace(self->rooms, room->room_id, room);
  return room;
}

static gboolean room_expired(gpointer data);

static void
room_arm_expiry(GhNotifier *self, Room *room)
{
  gint64 earliest = 0;
  for (guint i = 0; i < room->messages->len; i++) {
    gint64 expires = gh_message_get_expires_at(g_ptr_array_index(room->messages, i));
    if (expires > 0 && (earliest == 0 || expires < earliest))
      earliest = expires;
  }
  if (earliest == room->expiry_at && room->expiry)
    return;
  if (room->expiry)
    gh_clock_source_remove(self->clock, room->expiry);
  room->expiry = 0;
  room->expiry_at = earliest;
  if (!earliest)
    return;
  gint64 wait = MAX(earliest - now_unix(self), 0);
  room->expiry = gh_clock_timeout_add(self->clock, (guint64)wait * 1000, room_expired, room, NULL);
}

/* A counted message disappears: what named it is withdrawn (charter §3.7). */
static gboolean
room_expired(gpointer data)
{
  Room *room = data;
  GhNotifier *self = room->self;
  room->expiry = 0;
  g_autofree gchar *room_id = g_strdup(room->room_id);
  refresh(self, FALSE);
  /* Early (a clamped system-clock interval): wait for the rest. */
  Room *left = g_hash_table_lookup(self->rooms, room_id);
  if (left)
    room_arm_expiry(self, left);
  return G_SOURCE_REMOVE;
}

/* Mute and block, from the encrypted store; unreadable counts as muted. */
static gboolean
room_allowed(GhNotifier *self, Room *room)
{
  if (!self->room_state)
    return TRUE;
  GhNotifierRoomState state = { 0 };
  if (!self->room_state(self->room_state_data, room->room_id, &state))
    return FALSE;
  return !state.blocked && state.muted_until <= now_unix(self);
}

/* Drops what the notification may no longer count; FALSE when nothing is
 * left. Read messages go first: they are the oldest. */
static gboolean
room_prune(GhNotifier *self, Room *room, gboolean ask_store)
{
  if (gh_conversation_store_lookup(self->model, room->room_id) != room->conversation ||
      room->conversation == self->visible)
    return FALSE; /* forgotten, another account's, or on screen */
  gint64 now = now_unix(self);
  for (guint i = 0; i < room->messages->len; i++) {
    GhMessage *message = g_ptr_array_index(room->messages, i);
    gint64 expires = gh_message_get_expires_at(message);
    if ((expires > 0 && expires <= now) ||
        !gh_conversation_lookup_message(room->conversation, gh_message_get_rumor_id(message)))
      return FALSE; /* expired or purged: never keep what named it */
  }
  guint unread = gh_conversation_get_unread_count(room->conversation);
  if (room->messages->len > unread)
    g_ptr_array_remove_range(room->messages, 0, room->messages->len - unread);
  if (room->messages->len == 0)
    return FALSE;
  return !ask_store || room_allowed(self, room);
}

/* ---- slots ------------------------------------------------------------------------ */

static void
slot_free(gpointer data)
{
  Slot *slot = data;
  if (slot->timer)
    gh_clock_source_remove(slot->self->clock, slot->timer);
  g_free(slot->id);
  g_free(slot);
}

static Slot *
slot_get(GhNotifier *self, const gchar *id)
{
  Slot *slot = g_hash_table_lookup(self->slots, id);
  if (slot)
    return slot;
  slot = g_new0(Slot, 1);
  slot->self = self;
  slot->id = g_strdup(id);
  slot->last_sent = NEVER;
  g_hash_table_insert(self->slots, slot->id, slot);
  return slot;
}

/* The rooms an id counts now. */
static GPtrArray *
slot_rooms(GhNotifier *self, const gchar *id, Level level)
{
  GPtrArray *rooms = g_ptr_array_new();
  GHashTableIter iter;
  Room *room;
  g_hash_table_iter_init(&iter, self->rooms);
  while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&room)) {
    g_autofree gchar *room_slot = slot_id_for(self, room, level);
    if (g_str_equal(room_slot, id))
      g_ptr_array_add(rooms, room);
  }
  return rooms;
}

static void
withdraw_slot(GhNotifier *self, Slot *slot)
{
  if (!slot->shown)
    return;
  slot->shown = FALSE;
  if (self->app)
    g_application_withdraw_notification(self->app, slot->id);
}

/* A shown notification that counts nothing any more goes. One that counts
 * less is not sent again: that would alert again. */
static void
withdraw_empty(GhNotifier *self)
{
  Level level = level_of(self);
  GHashTableIter iter;
  Slot *slot;
  g_hash_table_iter_init(&iter, self->slots);
  while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&slot)) {
    if (!slot->shown)
      continue;
    g_autoptr(GPtrArray) rooms = slot_rooms(self, slot->id, level);
    if (rooms->len == 0)
      withdraw_slot(self, slot);
  }
}

static void
refresh(GhNotifier *self, gboolean ask_store)
{
  GHashTableIter iter;
  Room *room;
  g_hash_table_iter_init(&iter, self->rooms);
  while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&room))
    if (!room_prune(self, room, ask_store))
      g_hash_table_iter_remove(&iter);
  withdraw_empty(self);
}

/* Everything shown goes; pending messages are dropped. */
static void
clear_shown(GhNotifier *self)
{
  GHashTableIter iter;
  Slot *slot;
  g_hash_table_iter_init(&iter, self->slots);
  while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&slot))
    withdraw_slot(self, slot);
  g_hash_table_remove_all(self->slots);
  g_hash_table_remove_all(self->rooms);
}

/* ---- content ---------------------------------------------------------------------- */

static gboolean
is_line_break(gunichar c)
{
  return c == '\n' || c == '\r' || c == 0x0b || c == 0x0c || c == 0x85 || c == 0x2028 ||
         c == 0x2029;
}

/* One line: every run of line breaks (and the spaces around it) becomes one
 * space; other control characters are dropped. */
static gchar *
collapse_lines(const gchar *text)
{
  g_autofree gchar *valid = g_utf8_make_valid(text ? text : "", -1);
  GString *out = g_string_new(NULL);
  gboolean gap = FALSE;
  for (const gchar *p = valid; *p; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char(p);
    if (is_line_break(c)) {
      gap = TRUE;
      continue;
    }
    if (c == '\t')
      c = ' ';
    else if (g_unichar_iscntrl(c))
      continue;
    if (gap) {
      if (g_unichar_isspace(c))
        continue;
      while (out->len && out->str[out->len - 1] == ' ')
        g_string_truncate(out, out->len - 1);
      if (out->len)
        g_string_append_c(out, ' ');
      gap = FALSE;
    }
    g_string_append_unichar(out, c);
  }
  return g_strstrip(g_string_free(out, FALSE));
}

/* At most max graphemes (extended grapheme clusters): a longer text keeps
 * max - 1 of them and ends with "…". */
static gchar *
truncate_graphemes(const gchar *text, guint max)
{
  glong n_chars = g_utf8_strlen(text, -1);
  if (n_chars <= (glong)max)
    return g_strdup(text);
  g_autofree PangoLogAttr *attrs = g_new0(PangoLogAttr, n_chars + 1);
  pango_get_log_attrs(text, -1, -1, pango_language_from_string("und"), attrs, n_chars + 1);
  guint graphemes = 0;
  glong keep = n_chars;
  for (glong i = 1; i <= n_chars; i++) {
    if (!attrs[i].is_cursor_position)
      continue;
    if (++graphemes == max - 1)
      keep = i;
  }
  if (graphemes <= max)
    return g_strdup(text);
  const gchar *end = g_utf8_offset_to_pointer(text, keep);
  g_autofree gchar *head = g_strndup(text, end - text);
  return g_strconcat(head, "…", NULL);
}

/* preview: the newest message's text, prefixed with its author in a group. */
static gchar *
preview_of(Room *room)
{
  GhMessage *message = room_newest(room);
  g_autofree gchar *text = gh_message_get_kind(message) == 15
                             ? g_strdup(_("File"))
                             : collapse_lines(gh_message_get_content(message));
  if (!*text) {
    g_free(text);
    text = g_strdup(_("New message"));
  }
  const gchar *const *peers = gh_conversation_get_peers(room->conversation);
  g_autofree gchar *author = peers && peers[0] && peers[1]
                               ? npub_of(gh_message_get_sender(message), TRUE) : NULL;
  g_autofree gchar *body = author ? g_strdup_printf(_("%s: %s"), author, text)
                                  : g_steal_pointer(&text);
  return truncate_graphemes(body, GH_NOTIFIER_PREVIEW_MAX);
}

static gchar *
count_text(guint total)
{
  return g_strdup_printf(ngettext("%u new message", "%u new messages", total), total);
}

static GNotification *
build(GhNotifier *self, Level level, const gchar *id, GPtrArray *rooms, guint total)
{
  GNotification *notification;
  Room *newest = NULL;
  for (guint i = 0; i < rooms->len; i++) {
    Room *room = g_ptr_array_index(rooms, i);
    if (!newest || gh_message_compare(room_newest(room), room_newest(newest)) > 0)
      newest = room;
  }
  if (g_str_equal(id, GH_NOTIFIER_ID_MESSAGES)) {
    /* Count only: no name, npub, subject or text. */
    notification = g_notification_new(_("New message"));
    g_autofree gchar *body = count_text(total);
    g_notification_set_body(notification, body);
  } else {
    notification = g_notification_new(gh_conversation_get_title(newest->conversation));
    g_autofree gchar *body = level == LEVEL_PREVIEW ? preview_of(newest)
                             : total == 1           ? g_strdup(_("New message"))
                                                    : count_text(total);
    g_notification_set_body(notification, body);
  }
  g_notification_set_category(notification, GH_NOTIFIER_CATEGORY);
  g_notification_set_priority(notification, G_NOTIFICATION_PRIORITY_NORMAL);
  if (rooms->len == 1)
    g_notification_set_default_action_and_target_value(notification, "app." GH_NOTIFIER_ACTION,
      g_variant_new("(tx)", self->generation, (gint64)number_of(self, newest->room_id)));
  return notification;
}

/* ---- sending -------------------------------------------------------------------------- */

static void
maybe_sound(GhNotifier *self)
{
  if (!g_settings_get_boolean(self->settings, KEY_SOUND))
    return;
  gint64 now = gh_clock_get_monotonic_time(self->clock);
  if (self->last_sound != NEVER &&
      now - self->last_sound < (gint64)GH_NOTIFIER_SOUND_INTERVAL_MS * 1000)
    return;
  self->last_sound = now;
  g_signal_emit(self, signals[SIGNAL_PLAY_SOUND], 0);
}

static void
flush(GhNotifier *self, Slot *slot)
{
  refresh(self, TRUE);
  if (!self->app || !enabled(self))
    return;
  Level level = level_of(self);
  g_autoptr(GPtrArray) rooms = slot_rooms(self, slot->id, level);
  guint total = 0;
  gboolean fresh = FALSE;
  for (guint i = 0; i < rooms->len; i++) {
    Room *room = g_ptr_array_index(rooms, i);
    total += room->messages->len;
    fresh = fresh || room->fresh;
  }
  if (!fresh || total == 0)
    return;
  g_autoptr(GNotification) notification = build(self, level, slot->id, rooms, total);
  g_application_send_notification(self->app, slot->id, notification);
  for (guint i = 0; i < rooms->len; i++)
    ((Room *)g_ptr_array_index(rooms, i))->fresh = FALSE;
  slot->shown = TRUE;
  slot->last_sent = gh_clock_get_monotonic_time(self->clock);
  maybe_sound(self);
}

static gboolean
slot_due(gpointer data)
{
  Slot *slot = data;
  slot->timer = 0;
  flush(slot->self, slot);
  return G_SOURCE_REMOVE;
}

/* On the next main-loop turn (a burst, and a read in the same turn, fold
 * in), and never sooner than GH_NOTIFIER_UPDATE_INTERVAL_MS after the last
 * send of the id. */
static void
schedule(GhNotifier *self, Slot *slot)
{
  if (slot->timer)
    return;
  gint64 wait_us = 0;
  if (slot->last_sent != NEVER)
    wait_us = slot->last_sent + (gint64)GH_NOTIFIER_UPDATE_INTERVAL_MS * 1000 -
              gh_clock_get_monotonic_time(self->clock);
  guint64 wait_ms = wait_us > 0 ? (guint64)(wait_us + 999) / 1000 : 0;
  slot->timer = gh_clock_timeout_add(self->clock, wait_ms, slot_due, slot, NULL);
}

/* ---- the locked-store notice (charter §3.4, NO-11) ---------------------------------- */

static void
update_notice(GhNotifier *self)
{
  gboolean want = self->app && self->store_locked && !self->window && enabled(self);
  if (want == self->notice_shown || !self->app)
    return;
  self->notice_shown = want;
  if (!want) {
    g_application_withdraw_notification(self->app, GH_NOTIFIER_ID_STORE_LOCKED);
    return;
  }
  /* Hidden level: no account, no count. Activating it opens the window,
   * whose locked state offers Unlock (the way back). */
  g_autoptr(GNotification) notice = g_notification_new(_("Unlock to receive messages"));
  g_notification_set_body(notice, _("Your keyring is locked, so Groundhog can't receive "
                                    "messages. Open Groundhog to unlock it."));
  g_notification_set_priority(notice, G_NOTIFICATION_PRIORITY_NORMAL);
  g_application_send_notification(self->app, GH_NOTIFIER_ID_STORE_LOCKED, notice);
}

/* ---- the model and settings --------------------------------------------------------- */

static void
remember_stale(GhNotifier *self, guint64 generation, const gchar *account)
{
  guint64 *key = g_new(guint64, 1);
  *key = generation;
  g_hash_table_replace(self->stale, key, g_strdup(account));
  g_queue_push_tail(&self->stale_order, key);
  while (g_queue_get_length(&self->stale_order) > MAX_STALE)
    g_hash_table_remove(self->stale, g_queue_pop_head(&self->stale_order));
}

/* The model switched accounts: withdraw every id, start a new generation. */
static void
sync_account(GhNotifier *self)
{
  const gchar *account = gh_conversation_store_get_account(self->model);
  if (g_strcmp0(account, self->account) == 0)
    return;
  clear_shown(self);
  g_hash_table_remove_all(self->numbers);
  g_hash_table_remove_all(self->number_rooms);
  self->next_number = 1;
  if (self->account)
    remember_stale(self, self->generation, self->account);
  g_free(self->account);
  self->account = g_strdup(account);
  self->generation++;
  self->session_start = now_unix(self);
  g_clear_object(&self->visible);
}

static void
on_message_added(GhNotifier *self, GhConversation *conversation, GhMessage *message)
{
  sync_account(self);
  if (!self->account || g_strcmp0(gh_message_get_account(message), self->account) != 0 ||
      !enabled(self) || gh_message_is_self(message) || conversation == self->visible)
    return;
  /* Backfill of what came while this session was not receiving. */
  if (gh_message_get_created_at(message) < self->session_start)
    return;
  gint64 expires = gh_message_get_expires_at(message);
  if (expires > 0 && expires <= now_unix(self))
    return;
  Room *room = room_get(self, conversation);
  guint position = room->messages->len;
  while (position > 0 &&
         gh_message_compare(g_ptr_array_index(room->messages, position - 1), message) > 0)
    position--;
  g_ptr_array_insert(room->messages, position, g_object_ref(message));
  room->fresh = TRUE;
  room_arm_expiry(self, room);
  g_autofree gchar *id = slot_id_for(self, room, level_of(self));
  schedule(self, slot_get(self, id));
}

static void
on_account(GhNotifier *self)
{
  sync_account(self);
}

static void
on_model_changed(GhNotifier *self)
{
  if (g_hash_table_size(self->rooms))
    refresh(self, FALSE);
}

static void
on_enabled_changed(GhNotifier *self)
{
  if (!enabled(self))
    clear_shown(self);
  update_notice(self);
}

/* Later messages use the new level; what the old one showed goes now. */
static void
on_level_changed(GhNotifier *self)
{
  clear_shown(self);
}

/* ---- activation ----------------------------------------------------------------------- */

static void
on_open_conversation(GSimpleAction *action, GVariant *target, gpointer data)
{
  GhNotifier *self = data;
  guint64 generation = 0;
  gint64 number = 0;
  (void)action;
  g_variant_get(target, "(tx)", &generation, &number);
  sync_account(self);
  gboolean current = self->account && generation == self->generation;
  g_autoptr(GhConversation) conversation = NULL;
  g_autofree gchar *npub = NULL;
  if (current && number > 0 && number <= G_MAXUINT) {
    const gchar *room_id = g_hash_table_lookup(self->number_rooms,
                                               GUINT_TO_POINTER((guint)number));
    GhConversation *listed = room_id ? gh_conversation_store_lookup(self->model, room_id) : NULL;
    conversation = listed ? g_object_ref(listed) : NULL;
  } else if (!current) {
    npub = npub_of(g_hash_table_lookup(self->stale, &generation), FALSE);
  }
  g_object_ref(self);
  /* The window comes up (or to the front) first. */
  if (self->app)
    g_application_activate(self->app);
  if (!current)
    g_signal_emit(self, signals[SIGNAL_STALE_ACTIVATION], 0, npub);
  else if (conversation)
    g_signal_emit(self, signals[SIGNAL_OPEN_CONVERSATION], 0, conversation);
  g_object_unref(self);
}

static void
real_open_conversation(GhNotifier *self, GhConversation *conversation)
{
  if (!self->window || !self->sidebar)
    return;
  gh_window_open_item(self->window, conversation);
  gtk_window_present(GTK_WINDOW(self->window));
}

/* N7: the main list and a toast, never the other account's thread. */
static void
real_stale_activation(GhNotifier *self, const gchar *npub)
{
  GhWindow *window = self->window;
  if (!window || !self->sidebar || !self->split)
    return;
  gh_sidebar_page_set_show_requests(self->sidebar, FALSE);
  gh_sidebar_page_unselect(self->sidebar);
  adw_navigation_split_view_set_show_content(self->split, FALSE);
  AdwToast *toast = adw_toast_new(_("That notification was for another account"));
  if (npub) {
    adw_toast_set_button_label(toast, _("Switch"));
    adw_toast_set_action_name(toast, "account.select");
    adw_toast_set_action_target_value(toast, g_variant_new_string(npub));
  }
  adw_toast_overlay_add_toast(gh_window_get_toasts(window), toast);
  gtk_window_present(GTK_WINDOW(window));
}

/* The desktop's alert sound. */
static void
real_play_sound(GhNotifier *self)
{
  GdkDisplay *display = GTK_IS_APPLICATION(self->app) ? gdk_display_get_default() : NULL;
  if (display)
    gdk_display_beep(display);
}

/* ---- the window ------------------------------------------------------------------------ */

static void
update_visible(GhNotifier *self)
{
  GhWindow *window = self->window;
  GhConversation *visible = NULL;
  if (window && self->sidebar && self->split && gtk_widget_get_visible(GTK_WIDGET(window)) &&
      gtk_window_is_active(GTK_WINDOW(window)) && gh_window_get_content_visible(window)) {
    gpointer selected = gh_sidebar_page_get_selected(self->sidebar);
    if (GH_IS_CONVERSATION(selected))
      visible = selected;
  }
  gh_notifier_set_visible_conversation(self, visible);
}

static void
detach_window(GhNotifier *self)
{
  if (self->window)
    g_signal_handlers_disconnect_by_data(self->window, self);
  if (self->sidebar)
    g_signal_handlers_disconnect_by_data(self->sidebar, self);
  if (self->split)
    g_signal_handlers_disconnect_by_data(self->split, self);
  g_clear_weak_pointer(&self->window);
  g_clear_weak_pointer(&self->sidebar);
  g_clear_weak_pointer(&self->split);
}

static void
on_window_destroy(GhNotifier *self)
{
  detach_window(self);
  gh_notifier_set_visible_conversation(self, NULL);
  update_notice(self);
}

/* ---- GObject ------------------------------------------------------------------------- */

static void
gh_notifier_dispose(GObject *object)
{
  GhNotifier *self = GH_NOTIFIER(object);
  detach_window(self);
  if (self->model)
    g_signal_handlers_disconnect_by_data(self->model, self);
  if (self->settings)
    g_signal_handlers_disconnect_by_data(self->settings, self);
  clear_shown(self);
  self->store_locked = FALSE;
  update_notice(self);
  if (self->app) {
    g_action_map_remove_action(G_ACTION_MAP(self->app), GH_NOTIFIER_ACTION);
    if (g_object_get_data(G_OBJECT(self->app), NOTIFIER_DATA) == self)
      g_object_set_data(G_OBJECT(self->app), NOTIFIER_DATA, NULL);
    g_clear_weak_pointer(&self->app);
  }
  g_clear_object(&self->visible);
  G_OBJECT_CLASS(gh_notifier_parent_class)->dispose(object);
}

static void
gh_notifier_finalize(GObject *object)
{
  GhNotifier *self = GH_NOTIFIER(object);
  g_hash_table_unref(self->rooms);
  g_hash_table_unref(self->slots);
  g_hash_table_unref(self->numbers);
  g_hash_table_unref(self->number_rooms);
  g_hash_table_unref(self->stale);
  g_queue_clear(&self->stale_order);
  g_clear_object(&self->action);
  g_clear_object(&self->settings);
  g_clear_object(&self->model);
  g_clear_pointer(&self->clock, gh_clock_unref);
  g_free(self->account);
  G_OBJECT_CLASS(gh_notifier_parent_class)->finalize(object);
}

static void
gh_notifier_class_init(GhNotifierClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gh_notifier_dispose;
  object_class->finalize = gh_notifier_finalize;
  signals[SIGNAL_OPEN_CONVERSATION] = g_signal_new_class_handler("open-conversation",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_CALLBACK(real_open_conversation),
    NULL, NULL, NULL, G_TYPE_NONE, 1, GH_TYPE_CONVERSATION);
  signals[SIGNAL_STALE_ACTIVATION] = g_signal_new_class_handler("stale-activation",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_CALLBACK(real_stale_activation),
    NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_PLAY_SOUND] = g_signal_new_class_handler("play-sound",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_CALLBACK(real_play_sound),
    NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
gh_notifier_init(GhNotifier *self)
{
  self->rooms = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, room_free);
  self->slots = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, slot_free);
  self->numbers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->number_rooms = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
  self->stale = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
  g_queue_init(&self->stale_order);
  self->next_number = 1;
  self->last_sound = NEVER;
}

/* ---- public ------------------------------------------------------------------------------ */

GhNotifier *
gh_notifier_new(GApplication *app, const GhNotifierConfig *config)
{
  g_return_val_if_fail(G_IS_APPLICATION(app), NULL);
  g_return_val_if_fail(config != NULL, NULL);
  g_return_val_if_fail(G_IS_SETTINGS(config->settings), NULL);
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(config->conversations), NULL);
  GhNotifier *self = g_object_new(GH_TYPE_NOTIFIER, NULL);
  g_set_weak_pointer(&self->app, app);
  self->settings = g_object_ref(config->settings);
  self->model = g_object_ref(config->conversations);
  self->clock = config->clock ? gh_clock_ref(config->clock) : gh_clock_new_system();
  self->room_state = config->room_state;
  self->room_state_data = config->room_state_data;
  /* Unique to this notifier, so no earlier process's target is current. */
  self->generation = ((guint64)g_random_int() << 32 | g_random_int()) & G_MAXINT64;
  self->account = g_strdup(gh_conversation_store_get_account(self->model));
  self->session_start = now_unix(self);

  self->action = g_simple_action_new(GH_NOTIFIER_ACTION, G_VARIANT_TYPE("(tx)"));
  g_signal_connect(self->action, "activate", G_CALLBACK(on_open_conversation), self);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(self->action));
  g_object_set_data(G_OBJECT(app), NOTIFIER_DATA, self);

  g_signal_connect_swapped(self->model, "message-added", G_CALLBACK(on_message_added), self);
  g_signal_connect_swapped(self->model, "notify::account", G_CALLBACK(on_account), self);
  g_signal_connect_swapped(self->model, "items-changed", G_CALLBACK(on_model_changed), self);
  g_signal_connect_swapped(self->settings, "changed::" KEY_ENABLED,
                           G_CALLBACK(on_enabled_changed), self);
  g_signal_connect_swapped(self->settings, "changed::" KEY_PRIVACY,
                           G_CALLBACK(on_level_changed), self);
  /* GSettings reports a change only for keys read since connecting. */
  (void)enabled(self);
  (void)level_of(self);
  return self;
}

GhNotifier *
gh_notifier_get_for_application(GApplication *app)
{
  g_return_val_if_fail(G_IS_APPLICATION(app), NULL);
  return g_object_get_data(G_OBJECT(app), NOTIFIER_DATA);
}

void
gh_notifier_set_visible_conversation(GhNotifier *self, GhConversation *conversation)
{
  g_return_if_fail(GH_IS_NOTIFIER(self));
  g_return_if_fail(!conversation || GH_IS_CONVERSATION(conversation));
  if (self->visible == conversation)
    return;
  g_set_object(&self->visible, conversation);
  if (conversation)
    refresh(self, FALSE);
}

GhConversation *
gh_notifier_get_visible_conversation(GhNotifier *self)
{
  g_return_val_if_fail(GH_IS_NOTIFIER(self), NULL);
  return self->visible;
}

void
gh_notifier_withdraw_conversation(GhNotifier *self, const gchar *room_id)
{
  g_return_if_fail(GH_IS_NOTIFIER(self));
  g_return_if_fail(room_id != NULL);
  if (g_hash_table_remove(self->rooms, room_id))
    withdraw_empty(self);
}

void
gh_notifier_set_store_locked(GhNotifier *self, gboolean locked)
{
  g_return_if_fail(GH_IS_NOTIFIER(self));
  self->store_locked = !!locked;
  update_notice(self);
}

void
gh_notifier_attach_window(GhNotifier *self, GhWindow *window)
{
  g_return_if_fail(GH_IS_NOTIFIER(self));
  g_return_if_fail(GH_IS_WINDOW(window));
  detach_window(self);
  g_set_weak_pointer(&self->window, window);
  g_set_weak_pointer(&self->sidebar, gh_window_get_sidebar(window));
  g_set_weak_pointer(&self->split, gh_window_get_split(window));
  g_signal_connect_swapped(window, "notify::is-active", G_CALLBACK(update_visible), self);
  g_signal_connect_swapped(window, "notify::visible", G_CALLBACK(update_visible), self);
  g_signal_connect_swapped(window, "destroy", G_CALLBACK(on_window_destroy), self);
  g_signal_connect_swapped(self->sidebar, "notify::selected", G_CALLBACK(update_visible), self);
  g_signal_connect_swapped(self->split, "notify::show-content", G_CALLBACK(update_visible), self);
  g_signal_connect_swapped(self->split, "notify::collapsed", G_CALLBACK(update_visible), self);
  update_visible(self);
  /* The window shows a locked store itself. */
  update_notice(self);
}
