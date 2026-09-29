#include "gh-conversation-private.h"

#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>
#include <stdlib.h>
#include <string.h>

#define PREVIEW_CHARS 80

struct _GhConversation {
  GObject parent_instance;
  gchar *account;
  gchar *room_id;
  GStrv participants;
  GStrv peers;
  GPtrArray *messages;       /* GhMessage, in gh_message_compare order */
  GHashTable *by_id;         /* rumor id (owned by the message) -> message */
  GhMessage *subject_source; /* latest loaded message with a subject tag */
  gchar *stored_subject;     /* the durable room name; used while no loaded
                              * message carries a subject */
  /* The read marker: the last read message's place in the message order. It
   * may lie in the unloaded older history. An own message moves it, so every
   * message after it is someone else's and unread. */
  gboolean has_marker;       /* FALSE: nothing read yet */
  gint64 marker_created_at;
  gchar *marker_id;
  guint unread;
  guint unread_older;        /* unread messages in the unloaded older history */
  /* Listed messages read on screen while older unread ones were still
   * unloaded (W13b review B1): [seen_first, seen_last] in the message order.
   * The marker cannot say "these are read, older ones are not", so it stays
   * put (and nothing is persisted) until the older unread are listed; the
   * range only keeps this session's count honest. */
  gboolean has_seen;
  gint64 seen_first_created_at;
  gchar *seen_first_id;
  gint64 seen_last_created_at;
  gchar *seen_last_id;
  /* Durable paging: stored messages before the floor are not loaded. */
  gboolean has_older;
  gint64 floor_created_at;
  gchar *floor_id;
  gchar *fallback_title;     /* abbreviated npubs of the peers */
  gchar *preview;
  gboolean has_own_message;
  gboolean accepted;
  GhConversationStore *store; /* persists read state and acceptance; not a ref */
};

enum {
  PROP_0,
  PROP_ROOM_ID,
  PROP_BACKEND,
  PROP_TITLE,
  PROP_SUBJECT,
  PROP_PREVIEW,
  PROP_LAST_ACTIVITY,
  PROP_UNREAD_COUNT,
  PROP_IS_REQUEST,
  N_PROPS
};
static GParamSpec *props[N_PROPS];

static void gh_conversation_list_model_init(GListModelInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GhConversation, gh_conversation, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, gh_conversation_list_model_init))

GType
gh_conversation_backend_get_type(void)
{
  static gsize type = 0;
  if (g_once_init_enter(&type)) {
    static const GEnumValue values[] = {
      { GH_CONVERSATION_BACKEND_NIP17, "GH_CONVERSATION_BACKEND_NIP17", "nip17" },
      { GH_CONVERSATION_BACKEND_NIP29, "GH_CONVERSATION_BACKEND_NIP29", "nip29" },
      { GH_CONVERSATION_BACKEND_MLS, "GH_CONVERSATION_BACKEND_MLS", "mls" },
      { 0, NULL, NULL }
    };
    g_once_init_leave(&type, g_enum_register_static(
      g_intern_static_string("GhConversationBackend"), values));
  }
  return type;
}

static GType
list_get_item_type(GListModel *model)
{
  (void)model;
  return GH_TYPE_MESSAGE;
}

static guint
list_get_n_items(GListModel *model)
{
  return GH_CONVERSATION(model)->messages->len;
}

static gpointer
list_get_item(GListModel *model, guint position)
{
  GhConversation *self = GH_CONVERSATION(model);
  if (position >= self->messages->len)
    return NULL;
  return g_object_ref(g_ptr_array_index(self->messages, position));
}

static void
gh_conversation_list_model_init(GListModelInterface *iface)
{
  iface->get_item_type = list_get_item_type;
  iface->get_n_items = list_get_n_items;
  iface->get_item = list_get_item;
}

/* "npub1abcde…wxyz" for a lowercase hex pubkey. */
static gchar *
abbreviated_npub(const gchar *pubkey_hex)
{
  guint8 bytes[32];
  gchar *npub = NULL;
  if (!nostr_hex2bin(bytes, pubkey_hex, sizeof bytes) ||
      nostr_nip19_encode_npub(bytes, &npub) != 0 || !npub)
    return g_strdup(pubkey_hex);
  gsize length = strlen(npub);
  gchar *out = length > 16 ? g_strdup_printf("%.10s…%s", npub, npub + length - 4)
                           : g_strdup(npub);
  free(npub);
  return out;
}

static gchar *
fallback_title(GhConversation *self)
{
  if (!self->peers[0])
    return abbreviated_npub(self->account);
  g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; self->peers[i]; i++)
    g_ptr_array_add(names, abbreviated_npub(self->peers[i]));
  g_ptr_array_add(names, NULL);
  return g_strjoinv(", ", (gchar **)names->pdata);
}

static gchar *
preview_of(GhMessage *message)
{
  const gchar *body = gh_message_get_content(message);
  const gchar *newline = strchr(body, '\n');
  g_autofree gchar *line = newline ? g_strndup(body, newline - body) : g_strdup(body);
  if (!g_utf8_validate(line, -1, NULL))
    return g_strdup("");
  if (g_utf8_strlen(line, -1) <= PREVIEW_CHARS)
    return g_steal_pointer(&line);
  g_autofree gchar *cut = g_utf8_substring(line, 0, PREVIEW_CHARS);
  return g_strconcat(cut, "…", NULL);
}

GhConversation *
gh_conversation_new_for_message(GhMessage *message)
{
  g_return_val_if_fail(GH_IS_MESSAGE(message), NULL);
  GhConversation *self = g_object_new(GH_TYPE_CONVERSATION, NULL);
  self->account = g_strdup(gh_message_get_account(message));
  self->room_id = g_strdup(gh_message_get_room_id(message));
  self->participants = g_strdupv((GStrv)gh_message_get_participants(message));
  g_autoptr(GStrvBuilder) peers = g_strv_builder_new();
  for (guint i = 0; self->participants[i]; i++)
    if (!g_str_equal(self->participants[i], self->account))
      g_strv_builder_add(peers, self->participants[i]);
  self->peers = g_strv_builder_end(peers);
  self->fallback_title = fallback_title(self);
  return self;
}

/* <0, 0 or >0 as place a sorts before, at or after place b of the message
 * order. */
static gint
compare_places(gint64 a_created_at, const gchar *a_id, gint64 b_created_at, const gchar *b_id)
{
  if (a_created_at != b_created_at)
    return a_created_at < b_created_at ? -1 : 1;
  return strcmp(a_id, b_id);
}

/* <0, 0 or >0 as message sorts before, at or after the (created_at, id)
 * place of the message order. */
static gint
compare_place(GhMessage *message, gint64 created_at, const gchar *id)
{
  return compare_places(gh_message_get_created_at(message), gh_message_get_rumor_id(message),
                        created_at, id);
}

static gboolean
after_marker(GhConversation *self, GhMessage *message)
{
  return !self->has_marker ||
         compare_place(message, self->marker_created_at, self->marker_id) > 0;
}

static gboolean
in_seen(GhConversation *self, GhMessage *message)
{
  return self->has_seen &&
         compare_place(message, self->seen_first_created_at, self->seen_first_id) >= 0 &&
         compare_place(message, self->seen_last_created_at, self->seen_last_id) <= 0;
}

static void
clear_seen(GhConversation *self)
{
  self->has_seen = FALSE;
  g_clear_pointer(&self->seen_first_id, g_free);
  g_clear_pointer(&self->seen_last_id, g_free);
}

static void
set_marker(GhConversation *self, gint64 created_at, const gchar *id)
{
  gchar *copy = g_strdup(id);
  g_free(self->marker_id);
  self->marker_id = copy;
  self->marker_created_at = created_at;
  self->has_marker = TRUE;
  /* Everything the seen range held is now read by the marker itself. */
  if (self->has_seen &&
      compare_places(created_at, copy, self->seen_last_created_at, self->seen_last_id) >= 0)
    clear_seen(self);
}

static void
set_marker_to(GhConversation *self, GhMessage *message)
{
  set_marker(self, gh_message_get_created_at(message), gh_message_get_rumor_id(message));
}

/* First index whose message sorts after message. */
static guint
insertion_point(GhConversation *self, GhMessage *message)
{
  guint low = 0, high = self->messages->len;
  while (low < high) {
    guint mid = low + (high - low) / 2;
    if (gh_message_compare(g_ptr_array_index(self->messages, mid), message) <= 0)
      low = mid + 1;
    else
      high = mid;
  }
  return low;
}

/* Loaded messages after the marker from someone else: all of them (what the
 * durable count covers), or with skip_seen only those not read on screen
 * (what the unread count shows). *out_first (nullable) is the position of
 * the first one counted, the number of loaded messages when none is. */
static guint
count_loaded_unread(GhConversation *self, gboolean skip_seen, guint *out_first)
{
  guint unread = 0, first = self->messages->len;
  for (guint i = self->messages->len; i-- > 0;) {
    GhMessage *message = g_ptr_array_index(self->messages, i);
    if (!after_marker(self, message))
      break;
    if (!gh_message_is_self(message) && !(skip_seen && in_seen(self, message))) {
      unread++;
      first = i;
    }
  }
  if (out_first)
    *out_first = first;
  return unread;
}

static void
update_unread(GhConversation *self)
{
  guint unread = self->unread_older + count_loaded_unread(self, TRUE, NULL);
  if (unread == self->unread)
    return;
  self->unread = unread;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_UNREAD_COUNT]);
}

static gboolean
belongs(GhConversation *self, GhMessage *message)
{
  return g_strcmp0(gh_message_get_room_id(message), self->room_id) == 0 &&
         g_strcmp0(gh_message_get_account(message), self->account) == 0 &&
         !g_hash_table_contains(self->by_id, gh_message_get_rumor_id(message));
}

/* Places message in order and updates what derives from the message list
 * (subject source, own-message flag, preview). Returns its position. */
static guint
place_message(GhConversation *self, GhMessage *message)
{
  guint position = insertion_point(self, message);
  g_ptr_array_insert(self->messages, position, g_object_ref(message));
  g_hash_table_insert(self->by_id, (gpointer)gh_message_get_rumor_id(message), message);
  if (gh_message_get_subject(message) &&
      (!self->subject_source || gh_message_compare(message, self->subject_source) > 0))
    self->subject_source = message;
  if (gh_message_is_self(message))
    self->has_own_message = TRUE;
  if (position == self->messages->len - 1) {
    g_free(self->preview);
    self->preview = preview_of(message);
  }
  return position;
}

gboolean
gh_conversation_insert(GhConversation *self, GhMessage *message)
{
  g_return_val_if_fail(GH_IS_CONVERSATION(self), FALSE);
  g_return_val_if_fail(GH_IS_MESSAGE(message), FALSE);
  if (!belongs(self, message))
    return FALSE;
  gint64 last_activity = gh_conversation_get_last_activity(self);
  g_autofree gchar *old_subject = g_strdup(gh_conversation_get_subject(self));
  g_autofree gchar *old_title = g_strdup(gh_conversation_get_title(self));
  gboolean was_request = gh_conversation_get_is_request(self);
  guint position = place_message(self, message);
  /* Replying implies having read what came before. */
  if (gh_message_is_self(message) && after_marker(self, message))
    set_marker_to(self, message);
  gboolean subject_changed = g_strcmp0(old_subject, gh_conversation_get_subject(self)) != 0;
  gboolean newest = position == self->messages->len - 1;

  g_object_freeze_notify(G_OBJECT(self));
  g_list_model_items_changed(G_LIST_MODEL(self), position, 0, 1);
  if (subject_changed)
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_SUBJECT]);
  if (g_strcmp0(old_title, gh_conversation_get_title(self)) != 0)
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_TITLE]);
  if (newest)
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_PREVIEW]);
  if (gh_conversation_get_last_activity(self) != last_activity)
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_LAST_ACTIVITY]);
  if (was_request != gh_conversation_get_is_request(self))
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_IS_REQUEST]);
  update_unread(self);
  g_object_thaw_notify(G_OBJECT(self));
  return TRUE;
}

void
gh_conversation_set_store(GhConversation *self, GhConversationStore *store)
{
  g_return_if_fail(GH_IS_CONVERSATION(self));
  self->store = store;
}

gboolean
gh_conversation_remove(GhConversation *self, const gchar *rumor_id)
{
  g_return_val_if_fail(GH_IS_CONVERSATION(self), FALSE);
  GhMessage *found = rumor_id ? g_hash_table_lookup(self->by_id, rumor_id) : NULL;
  guint position = 0;
  if (!found || !g_ptr_array_find(self->messages, found, &position))
    return FALSE;
  gint64 last_activity = gh_conversation_get_last_activity(self);
  g_autofree gchar *old_subject = g_strdup(gh_conversation_get_subject(self));
  gboolean was_request = gh_conversation_get_is_request(self);
  /* The hash key is the message's own id: unhash before the list drops it. */
  g_autoptr(GhMessage) message = g_object_ref(found);
  g_hash_table_remove(self->by_id, gh_message_get_rumor_id(message));
  g_ptr_array_remove_index(self->messages, position);
  /* An own message accepted the room when it was added; has_own_message
   * stays set so the room does not turn back into a request. */
  if (self->subject_source == message) {
    self->subject_source = NULL;
    for (guint i = self->messages->len; i-- > 0 && !self->subject_source;) {
      GhMessage *candidate = g_ptr_array_index(self->messages, i);
      if (gh_message_get_subject(candidate))
        self->subject_source = candidate;
    }
  }
  gboolean newest = position == self->messages->len;
  if (newest) {
    g_free(self->preview);
    self->preview = self->messages->len
      ? preview_of(g_ptr_array_index(self->messages, self->messages->len - 1)) : NULL;
  }

  g_object_freeze_notify(G_OBJECT(self));
  g_list_model_items_changed(G_LIST_MODEL(self), position, 1, 0);
  if (g_strcmp0(old_subject, gh_conversation_get_subject(self)) != 0) {
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_SUBJECT]);
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_TITLE]);
  }
  if (newest)
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_PREVIEW]);
  if (gh_conversation_get_last_activity(self) != last_activity)
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_LAST_ACTIVITY]);
  if (was_request != gh_conversation_get_is_request(self))
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_IS_REQUEST]);
  update_unread(self);
  g_object_thaw_notify(G_OBJECT(self));
  return TRUE;
}

void
gh_conversation_restore(GhConversation *self, GPtrArray *messages,
                        const GhConversationState *state)
{
  g_return_if_fail(GH_IS_CONVERSATION(self));
  g_return_if_fail(state != NULL);
  g_return_if_fail(!state->has_marker || state->marker_id);
  g_return_if_fail(!state->has_older || state->floor_id);
  gint64 last_activity = gh_conversation_get_last_activity(self);
  g_autofree gchar *old_subject = g_strdup(gh_conversation_get_subject(self));
  g_autofree gchar *old_title = g_strdup(gh_conversation_get_title(self));
  g_autofree gchar *old_preview = g_strdup(self->preview);
  gboolean was_request = gh_conversation_get_is_request(self);

  g_object_freeze_notify(G_OBJECT(self));
  for (guint i = 0; messages && i < messages->len; i++) {
    GhMessage *message = g_ptr_array_index(messages, i);
    if (!GH_IS_MESSAGE(message) || !belongs(self, message))
      continue;
    guint position = place_message(self, message);
    g_list_model_items_changed(G_LIST_MODEL(self), position, 0, 1);
  }
  /* A request accepted in memory stays accepted even if persisting it failed. */
  self->accepted = self->accepted || state->accepted;
  if (state->has_marker) {
    set_marker(self, state->marker_created_at, state->marker_id);
  } else {
    self->has_marker = FALSE;
    g_clear_pointer(&self->marker_id, g_free);
  }
  g_free(self->stored_subject);
  self->stored_subject = g_strdup(state->subject);
  g_free(self->floor_id);
  self->floor_id = state->has_older ? g_strdup(state->floor_id) : NULL;
  self->floor_created_at = state->has_older ? state->floor_created_at : 0;
  self->has_older = state->has_older;
  guint loaded = count_loaded_unread(self, FALSE, NULL);
  self->unread_older = state->unread > loaded ? state->unread - loaded : 0;

  if (g_strcmp0(old_subject, gh_conversation_get_subject(self)) != 0)
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_SUBJECT]);
  if (g_strcmp0(old_title, gh_conversation_get_title(self)) != 0)
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_TITLE]);
  if (g_strcmp0(old_preview, self->preview) != 0)
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_PREVIEW]);
  if (gh_conversation_get_last_activity(self) != last_activity)
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_LAST_ACTIVITY]);
  if (was_request != gh_conversation_get_is_request(self))
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_IS_REQUEST]);
  update_unread(self);
  g_object_thaw_notify(G_OBJECT(self));
}

gboolean
gh_conversation_is_older_history(GhConversation *self, GhMessage *message)
{
  g_return_val_if_fail(GH_IS_CONVERSATION(self), FALSE);
  g_return_val_if_fail(GH_IS_MESSAGE(message), FALSE);
  return self->has_older &&
         compare_place(message, self->floor_created_at, self->floor_id) < 0;
}

void
gh_conversation_add_older_history(GhConversation *self, GhMessage *message)
{
  g_return_if_fail(GH_IS_CONVERSATION(self));
  g_return_if_fail(GH_IS_MESSAGE(message));
  gboolean was_request = gh_conversation_get_is_request(self);
  if (gh_message_is_self(message)) {
    self->has_own_message = TRUE;
    if (after_marker(self, message)) {
      /* How many unloaded messages follow it is the store's to say (sync). */
      set_marker_to(self, message);
      self->unread_older = 0;
    }
  } else if (after_marker(self, message)) {
    self->unread_older++;
  }
  g_object_freeze_notify(G_OBJECT(self));
  if (was_request != gh_conversation_get_is_request(self))
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_IS_REQUEST]);
  update_unread(self);
  g_object_thaw_notify(G_OBJECT(self));
}

void
gh_conversation_sync_unread(GhConversation *self, guint unread)
{
  g_return_if_fail(GH_IS_CONVERSATION(self));
  guint loaded = count_loaded_unread(self, FALSE, NULL);
  self->unread_older = unread > loaded ? unread - loaded : 0;
  update_unread(self);
}

gboolean
gh_conversation_get_floor(GhConversation *self, gint64 *created_at, const gchar **id)
{
  g_return_val_if_fail(GH_IS_CONVERSATION(self), FALSE);
  if (created_at)
    *created_at = self->has_older ? self->floor_created_at : 0;
  if (id)
    *id = self->has_older ? self->floor_id : NULL;
  return self->has_older;
}

const gchar *
gh_conversation_get_account(GhConversation *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION(self), NULL);
  return self->account;
}

const gchar *
gh_conversation_get_room_id(GhConversation *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION(self), NULL);
  return self->room_id;
}

const gchar *const *
gh_conversation_get_participants(GhConversation *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION(self), NULL);
  return (const gchar *const *)self->participants;
}

const gchar *const *
gh_conversation_get_peers(GhConversation *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION(self), NULL);
  return (const gchar *const *)self->peers;
}

const gchar *
gh_conversation_get_subject(GhConversation *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION(self), NULL);
  /* Loaded messages are the newest ones, so a loaded subject is never older
   * than the stored name of the unloaded history. */
  const gchar *subject = self->subject_source ?
    gh_message_get_subject(self->subject_source) : self->stored_subject;
  return subject && *subject ? subject : NULL;
}

gint64
gh_conversation_get_last_activity(GhConversation *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION(self), 0);
  if (self->messages->len == 0)
    return 0;
  return gh_message_get_created_at(
    g_ptr_array_index(self->messages, self->messages->len - 1));
}

guint
gh_conversation_get_unread_count(GhConversation *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION(self), 0);
  return self->unread;
}

void
gh_conversation_mark_read(GhConversation *self)
{
  g_return_if_fail(GH_IS_CONVERSATION(self));
  if (self->messages->len == 0)
    return;
  GhMessage *first = g_ptr_array_index(self->messages, 0);
  GhMessage *last = g_ptr_array_index(self->messages, self->messages->len - 1);
  if (self->unread_older > 0) {
    /* Unread messages are still in the unloaded older history: only what is
     * listed is read. The marker stays before the unloaded ones, so they stay
     * unread (here and after a restart) until they are listed too. */
    gchar *first_id = g_strdup(gh_message_get_rumor_id(first));
    gchar *last_id = g_strdup(gh_message_get_rumor_id(last));
    clear_seen(self);
    self->has_seen = TRUE;
    self->seen_first_created_at = gh_message_get_created_at(first);
    self->seen_first_id = first_id;
    self->seen_last_created_at = gh_message_get_created_at(last);
    self->seen_last_id = last_id;
    update_unread(self);
    return;
  }
  gboolean moved = after_marker(self, last);
  set_marker_to(self, last);
  update_unread(self);
  if (moved && self->store)
    gh_conversation_store_persist_read(self->store, self, last);
}

guint
gh_conversation_get_listed_unread(GhConversation *self, guint *out_first)
{
  g_return_val_if_fail(GH_IS_CONVERSATION(self), 0);
  return count_loaded_unread(self, TRUE, out_first);
}

gboolean
gh_conversation_get_has_older(GhConversation *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION(self), FALSE);
  return self->has_older;
}

GhConversationBackend
gh_conversation_get_backend(GhConversation *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION(self), GH_CONVERSATION_BACKEND_NIP17);
  return GH_CONVERSATION_BACKEND_NIP17;
}

const gchar *
gh_conversation_get_title(GhConversation *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION(self), NULL);
  /* A request's subject is whatever its sender chose: never its name. */
  const gchar *subject = gh_conversation_get_is_request(self)
                           ? NULL : gh_conversation_get_subject(self);
  return subject ? subject : self->fallback_title;
}

const gchar *
gh_conversation_get_preview(GhConversation *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION(self), NULL);
  return self->preview;
}

gboolean
gh_conversation_get_is_request(GhConversation *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION(self), FALSE);
  return !self->accepted && !self->has_own_message;
}

void
gh_conversation_accept(GhConversation *self)
{
  g_return_if_fail(GH_IS_CONVERSATION(self));
  gboolean was_request = gh_conversation_get_is_request(self);
  self->accepted = TRUE;
  if (!was_request)
    return;
  g_object_freeze_notify(G_OBJECT(self));
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_IS_REQUEST]);
  if (gh_conversation_get_subject(self))
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_TITLE]);
  g_object_thaw_notify(G_OBJECT(self));
  if (self->store)
    gh_conversation_store_persist_accept(self->store, self);
}

GhMessage *
gh_conversation_lookup_message(GhConversation *self, const gchar *rumor_id)
{
  g_return_val_if_fail(GH_IS_CONVERSATION(self), NULL);
  return rumor_id ? g_hash_table_lookup(self->by_id, rumor_id) : NULL;
}

static void
gh_conversation_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  GhConversation *self = GH_CONVERSATION(object);
  switch (id) {
  case PROP_ROOM_ID:
    g_value_set_string(value, self->room_id);
    break;
  case PROP_BACKEND:
    g_value_set_enum(value, gh_conversation_get_backend(self));
    break;
  case PROP_TITLE:
    g_value_set_string(value, gh_conversation_get_title(self));
    break;
  case PROP_PREVIEW:
    g_value_set_string(value, self->preview);
    break;
  case PROP_IS_REQUEST:
    g_value_set_boolean(value, gh_conversation_get_is_request(self));
    break;
  case PROP_SUBJECT:
    g_value_set_string(value, gh_conversation_get_subject(self));
    break;
  case PROP_LAST_ACTIVITY:
    g_value_set_int64(value, gh_conversation_get_last_activity(self));
    break;
  case PROP_UNREAD_COUNT:
    g_value_set_uint(value, self->unread);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_conversation_finalize(GObject *object)
{
  GhConversation *self = GH_CONVERSATION(object);
  g_hash_table_unref(self->by_id);
  g_ptr_array_unref(self->messages);
  g_free(self->account);
  g_free(self->room_id);
  g_strfreev(self->participants);
  g_strfreev(self->peers);
  g_free(self->stored_subject);
  g_free(self->marker_id);
  clear_seen(self);
  g_free(self->floor_id);
  g_free(self->fallback_title);
  g_free(self->preview);
  G_OBJECT_CLASS(gh_conversation_parent_class)->finalize(object);
}

static void
gh_conversation_class_init(GhConversationClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = gh_conversation_get_property;
  object_class->finalize = gh_conversation_finalize;
  props[PROP_ROOM_ID] = g_param_spec_string("room-id", NULL, NULL, NULL,
    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  props[PROP_BACKEND] = g_param_spec_enum("backend", NULL, NULL,
    GH_TYPE_CONVERSATION_BACKEND, GH_CONVERSATION_BACKEND_NIP17,
    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  props[PROP_TITLE] = g_param_spec_string("title", NULL, NULL, NULL,
    G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  props[PROP_PREVIEW] = g_param_spec_string("preview", NULL, NULL, NULL,
    G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  props[PROP_IS_REQUEST] = g_param_spec_boolean("is-request", NULL, NULL, TRUE,
    G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  props[PROP_SUBJECT] = g_param_spec_string("subject", NULL, NULL, NULL,
    G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  props[PROP_LAST_ACTIVITY] = g_param_spec_int64("last-activity", NULL, NULL,
    0, G_MAXINT64, 0, G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  props[PROP_UNREAD_COUNT] = g_param_spec_uint("unread-count", NULL, NULL,
    0, G_MAXUINT, 0, G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, N_PROPS, props);
}

static void
gh_conversation_init(GhConversation *self)
{
  self->messages = g_ptr_array_new_with_free_func(g_object_unref);
  self->by_id = g_hash_table_new(g_str_hash, g_str_equal);
}
