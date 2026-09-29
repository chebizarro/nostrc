#include "gh-conversation-store.h"
#include "gh-conversation-private.h"

#include <string.h>

struct _GhConversationStore {
  GObject parent_instance;
  gchar *account;
  GPtrArray *conversations; /* GhConversation, in store order */
  GHashTable *rooms;        /* room id (owned by the conversation) -> conversation */
  GHashTable *messages;     /* rumor id (owned by the message) -> conversation */
  GhConversationDelegate delegate;
  gpointer delegate_data;
  GDestroyNotify delegate_destroy;
  gboolean has_delegate;
};

enum { SIGNAL_MESSAGE_ADDED, N_SIGNALS };
static guint signals[N_SIGNALS];

enum { PROP_0, PROP_ACCOUNT, N_PROPS };
static GParamSpec *props[N_PROPS];

static void gh_conversation_store_list_model_init(GListModelInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GhConversationStore, gh_conversation_store, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, gh_conversation_store_list_model_init))

static GType
list_get_item_type(GListModel *model)
{
  (void)model;
  return GH_TYPE_CONVERSATION;
}

static guint
list_get_n_items(GListModel *model)
{
  return GH_CONVERSATION_STORE(model)->conversations->len;
}

static gpointer
list_get_item(GListModel *model, guint position)
{
  GhConversationStore *self = GH_CONVERSATION_STORE(model);
  if (position >= self->conversations->len)
    return NULL;
  return g_object_ref(g_ptr_array_index(self->conversations, position));
}

static void
gh_conversation_store_list_model_init(GListModelInterface *iface)
{
  iface->get_item_type = list_get_item_type;
  iface->get_n_items = list_get_n_items;
  iface->get_item = list_get_item;
}

/* Newest activity first; the room id makes the order total. */
static gint
store_compare(GhConversation *a, GhConversation *b)
{
  gint64 la = gh_conversation_get_last_activity(a);
  gint64 lb = gh_conversation_get_last_activity(b);
  if (la != lb)
    return la > lb ? -1 : 1;
  return strcmp(gh_conversation_get_room_id(a), gh_conversation_get_room_id(b));
}

static guint
insertion_point(GhConversationStore *self, GhConversation *conversation)
{
  guint low = 0, high = self->conversations->len;
  while (low < high) {
    guint mid = low + (high - low) / 2;
    if (store_compare(g_ptr_array_index(self->conversations, mid), conversation) < 0)
      low = mid + 1;
    else
      high = mid;
  }
  return low;
}

GhConversationStore *
gh_conversation_store_new(void)
{
  return g_object_new(GH_TYPE_CONVERSATION_STORE, NULL);
}

static void
clear_delegate(GhConversationStore *self)
{
  gpointer data = self->delegate_data;
  GDestroyNotify destroy = self->delegate_destroy;
  memset(&self->delegate, 0, sizeof self->delegate);
  self->delegate_data = NULL;
  self->delegate_destroy = NULL;
  self->has_delegate = FALSE;
  if (destroy)
    destroy(data);
}

static void
detach_conversations(GPtrArray *conversations)
{
  for (guint i = 0; conversations && i < conversations->len; i++)
    gh_conversation_set_store(g_ptr_array_index(conversations, i), NULL);
}

void
gh_conversation_store_set_account(GhConversationStore *self, const gchar *account_pubkey,
                                  const GhConversationDelegate *delegate,
                                  gpointer delegate_data, GDestroyNotify destroy)
{
  g_return_if_fail(GH_IS_CONVERSATION_STORE(self));
  g_return_if_fail(!delegate || (delegate->has_wrap && delegate->has_rumor && delegate->admit));
  clear_delegate(self);
  if (delegate) {
    self->delegate = *delegate;
    self->delegate_data = delegate_data;
    self->delegate_destroy = destroy;
    self->has_delegate = TRUE;
  } else if (destroy) {
    destroy(delegate_data);
  }
  if (g_strcmp0(self->account, account_pubkey) == 0)
    return;
  guint removed = self->conversations->len;
  g_hash_table_remove_all(self->messages);
  g_hash_table_remove_all(self->rooms);
  g_free(self->account);
  self->account = g_strdup(account_pubkey);
  /* Keep the objects alive until the removal is announced. */
  g_autoptr(GPtrArray) old = g_steal_pointer(&self->conversations);
  detach_conversations(old);
  self->conversations = g_ptr_array_new_with_free_func(g_object_unref);
  if (removed)
    g_list_model_items_changed(G_LIST_MODEL(self), 0, removed, 0);
  /* After the removal: a listener sees the new account's (empty) model. */
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_ACCOUNT]);
}

const gchar *
gh_conversation_store_get_account(GhConversationStore *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(self), NULL);
  return self->account;
}

/* Lists a new room at its place in the store order. */
static void
list_new(GhConversationStore *self, GhConversation *conversation)
{
  g_hash_table_insert(self->rooms, (gpointer)gh_conversation_get_room_id(conversation),
                      conversation);
  gh_conversation_set_store(conversation, self);
  g_object_ref(conversation);
  guint position = insertion_point(self, conversation);
  g_ptr_array_insert(self->conversations, position, conversation);
  g_list_model_items_changed(G_LIST_MODEL(self), position, 0, 1);
}

/* Moves a listed room (which was at old_position) to its new place. */
static void
reposition(GhConversationStore *self, GhConversation *conversation, guint old_position)
{
  g_ptr_array_steal_index(self->conversations, old_position);
  guint position = insertion_point(self, conversation);
  g_ptr_array_insert(self->conversations, position, conversation);
  if (old_position != position) {
    guint first = MIN(old_position, position);
    guint span = MAX(old_position, position) - first + 1;
    g_list_model_items_changed(G_LIST_MODEL(self), first, span, span);
  }
}

static void
index_messages(GhConversationStore *self, GhConversation *conversation)
{
  guint n = g_list_model_get_n_items(G_LIST_MODEL(conversation));
  for (guint i = 0; i < n; i++) {
    g_autoptr(GhMessage) message = g_list_model_get_item(G_LIST_MODEL(conversation), i);
    /* The key is owned by the message, which the conversation keeps. */
    g_hash_table_insert(self->messages, (gpointer)gh_message_get_rumor_id(message),
                        conversation);
  }
}

/* A durable delegate's unread count is authoritative (it also counts the
 * room's unloaded history). */
static void
sync_unread(GhConversation *conversation, const GhConversationCommit *commit)
{
  if (conversation && commit->unread >= 0)
    gh_conversation_sync_unread(conversation, (guint)MIN(commit->unread, (gint64)G_MAXUINT));
}

GhConversationAddResult
gh_conversation_store_admit(GhConversationStore *self, GhMessage *message,
                            const gchar *wrap_id, GError **error)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(self), GH_CONVERSATION_ADD_REJECTED);
  g_return_val_if_fail(GH_IS_MESSAGE(message), GH_CONVERSATION_ADD_REJECTED);
  if (!self->account || g_strcmp0(gh_message_get_account(message), self->account) != 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "Message belongs to another account");
    return GH_CONVERSATION_ADD_REJECTED;
  }
  const gchar *rumor_id = gh_message_get_rumor_id(message);
  GhMessage *existing = gh_conversation_store_lookup_message(self, rumor_id);
  /* A delivered rumor the delegate committed but the model does not list (a
   * restart without restore, or unloaded history) is not shown again. A
   * local echo of a stored rumor is listed unless the delegate hides it. */
  gboolean known = existing ||
    (wrap_id && self->has_delegate && self->delegate.has_rumor(self->delegate_data, rumor_id));
  /* One commit per admission, before the model changes: a duplicate still
   * records the wrap id that carried it. */
  GhConversationCommit commit = { FALSE, -1 };
  if (self->has_delegate &&
      !self->delegate.admit(self->delegate_data, message, wrap_id, &commit, error))
    return GH_CONVERSATION_ADD_FAILED;
  GhConversation *conversation = g_hash_table_lookup(self->rooms,
                                                     gh_message_get_room_id(message));
  if (existing || known) {
    if (existing)
      for (const gchar *const *url = gh_message_get_relays(message); *url; url++)
        gh_message_add_relay(existing, *url);
    sync_unread(conversation, &commit);
    return GH_CONVERSATION_ADD_DUPLICATE;
  }
  if (commit.hidden)
    return GH_CONVERSATION_ADD_HIDDEN;
  if (conversation && gh_conversation_is_older_history(conversation, message)) {
    gh_conversation_add_older_history(conversation, message);
    sync_unread(conversation, &commit);
    return GH_CONVERSATION_ADD_NEW;
  }

  g_autoptr(GhConversation) created = NULL;
  guint old_position = G_MAXUINT;
  if (!conversation) {
    created = gh_conversation_new_for_message(message);
    conversation = created;
  } else {
    g_ptr_array_find(self->conversations, conversation, &old_position);
  }
  /* The room updates (and notifies) while the store still lists it where it
   * was; only then does it move to its new position. */
  if (!gh_conversation_insert(conversation, message)) {
    /* Unreachable: the room id and account match and the rumor id is new. */
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Conversation refused a new message");
    return GH_CONVERSATION_ADD_REJECTED;
  }
  sync_unread(conversation, &commit);
  g_hash_table_insert(self->messages, (gpointer)rumor_id, conversation);
  if (created)
    list_new(self, conversation);
  else
    reposition(self, conversation, old_position);
  g_signal_emit(self, signals[SIGNAL_MESSAGE_ADDED], 0, conversation, message);
  return GH_CONVERSATION_ADD_NEW;
}

GhConversationAddResult
gh_conversation_store_add_message(GhConversationStore *self, GhMessage *message,
                                  GError **error)
{
  return gh_conversation_store_admit(self, message, NULL, error);
}

gboolean
gh_conversation_store_has_wrap(GhConversationStore *self, const gchar *wrap_id)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(self), FALSE);
  return wrap_id && self->has_delegate &&
         self->delegate.has_wrap(self->delegate_data, wrap_id);
}

gboolean
gh_conversation_store_has_rejected(GhConversationStore *self, const gchar *wrap_id)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(self), FALSE);
  return wrap_id && self->has_delegate && self->delegate.has_rejected &&
         self->delegate.has_rejected(self->delegate_data, wrap_id);
}

gboolean
gh_conversation_store_record_rejected(GhConversationStore *self, const gchar *wrap_id,
                                      GError **error)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(self), FALSE);
  g_return_val_if_fail(wrap_id != NULL, FALSE);
  if (!self->has_delegate || !self->delegate.add_rejected)
    return TRUE;
  return self->delegate.add_rejected(self->delegate_data, wrap_id, error);
}

gboolean
gh_conversation_store_has_message(GhConversationStore *self, const gchar *rumor_id)
{
  return gh_conversation_store_lookup_message(self, rumor_id) != NULL;
}

GhMessage *
gh_conversation_store_lookup_message(GhConversationStore *self, const gchar *rumor_id)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(self), NULL);
  GhConversation *conversation = rumor_id ? g_hash_table_lookup(self->messages, rumor_id) : NULL;
  return conversation ? gh_conversation_lookup_message(conversation, rumor_id) : NULL;
}

GhConversation *
gh_conversation_store_lookup(GhConversationStore *self, const gchar *room_id)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(self), NULL);
  return room_id ? g_hash_table_lookup(self->rooms, room_id) : NULL;
}

/* ---- Conversation hooks and the durable restore seam (private) ------------ */

void
gh_conversation_store_persist_read(GhConversationStore *self, GhConversation *conversation,
                                   GhMessage *last_read)
{
  g_return_if_fail(GH_IS_CONVERSATION_STORE(self));
  if (!self->has_delegate || !self->delegate.mark_read)
    return;
  g_autoptr(GError) error = NULL;
  if (!self->delegate.mark_read(self->delegate_data, conversation, last_read, &error))
    g_warning("Groundhog could not save the read position of a conversation: %s",
              error ? error->message : "unknown error");
}

void
gh_conversation_store_persist_accept(GhConversationStore *self, GhConversation *conversation)
{
  g_return_if_fail(GH_IS_CONVERSATION_STORE(self));
  if (!self->has_delegate || !self->delegate.accept)
    return;
  g_autoptr(GError) error = NULL;
  if (!self->delegate.accept(self->delegate_data, conversation, &error))
    g_warning("Groundhog could not save an accepted message request: %s",
              error ? error->message : "unknown error");
}

GhConversation *
gh_conversation_store_restore(GhConversationStore *self, const gchar *room_id,
                              GPtrArray *messages, const GhConversationState *state)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(self), NULL);
  g_return_val_if_fail(room_id != NULL, NULL);
  g_return_val_if_fail(state != NULL, NULL);
  if (!self->account)
    return NULL;
  GhConversation *conversation = g_hash_table_lookup(self->rooms, room_id);
  if (conversation) {
    guint old_position = G_MAXUINT;
    g_ptr_array_find(self->conversations, conversation, &old_position);
    gh_conversation_restore(conversation, messages, state);
    index_messages(self, conversation);
    reposition(self, conversation, old_position);
    return conversation;
  }
  GhMessage *first = NULL;
  for (guint i = 0; messages && i < messages->len && !first; i++) {
    GhMessage *message = g_ptr_array_index(messages, i);
    if (g_strcmp0(gh_message_get_room_id(message), room_id) == 0 &&
        g_strcmp0(gh_message_get_account(message), self->account) == 0 &&
        !g_hash_table_contains(self->messages, gh_message_get_rumor_id(message)))
      first = message;
  }
  if (!first)
    return NULL;
  g_autoptr(GhConversation) created = gh_conversation_new_for_message(first);
  gh_conversation_restore(created, messages, state);
  index_messages(self, created);
  list_new(self, created);
  return created;
}

gboolean
gh_conversation_store_remove(GhConversationStore *self, const gchar *room_id)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(self), FALSE);
  GhConversation *conversation = room_id ? g_hash_table_lookup(self->rooms, room_id) : NULL;
  guint position = 0;
  if (!conversation || !g_ptr_array_find(self->conversations, conversation, &position))
    return FALSE;
  guint n = g_list_model_get_n_items(G_LIST_MODEL(conversation));
  for (guint i = 0; i < n; i++) {
    g_autoptr(GhMessage) message = g_list_model_get_item(G_LIST_MODEL(conversation), i);
    g_hash_table_remove(self->messages, gh_message_get_rumor_id(message));
  }
  g_hash_table_remove(self->rooms, room_id);
  gh_conversation_set_store(conversation, NULL);
  /* Keep it alive until the removal is announced. */
  g_autoptr(GhConversation) removed = g_ptr_array_steal_index(self->conversations, position);
  g_list_model_items_changed(G_LIST_MODEL(self), position, 1, 0);
  return TRUE;
}

gboolean
gh_conversation_store_remove_message(GhConversationStore *self, const gchar *rumor_id)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(self), FALSE);
  GhConversation *conversation = rumor_id ? g_hash_table_lookup(self->messages, rumor_id) : NULL;
  guint old_position = 0;
  if (!conversation || !g_ptr_array_find(self->conversations, conversation, &old_position))
    return FALSE;
  /* The key belongs to the message, which the room is about to release. */
  g_autofree gchar *id = g_strdup(rumor_id);
  g_hash_table_remove(self->messages, id);
  /* As on insert: the room notifies where it is listed, then moves. */
  gh_conversation_remove(conversation, id);
  reposition(self, conversation, old_position);
  return TRUE;
}

static void
gh_conversation_store_finalize(GObject *object)
{
  GhConversationStore *self = GH_CONVERSATION_STORE(object);
  clear_delegate(self);
  detach_conversations(self->conversations);
  g_hash_table_unref(self->messages);
  g_hash_table_unref(self->rooms);
  g_ptr_array_unref(self->conversations);
  g_free(self->account);
  G_OBJECT_CLASS(gh_conversation_store_parent_class)->finalize(object);
}

static void
gh_conversation_store_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  GhConversationStore *self = GH_CONVERSATION_STORE(object);
  switch (id) {
  case PROP_ACCOUNT:
    g_value_set_string(value, self->account);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_conversation_store_class_init(GhConversationStoreClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gh_conversation_store_finalize;
  object_class->get_property = gh_conversation_store_get_property;
  props[PROP_ACCOUNT] = g_param_spec_string("account", NULL, NULL, NULL,
                                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS |
                                            G_PARAM_EXPLICIT_NOTIFY);
  g_object_class_install_properties(object_class, N_PROPS, props);
  signals[SIGNAL_MESSAGE_ADDED] = g_signal_new("message-added",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, GH_TYPE_CONVERSATION, GH_TYPE_MESSAGE);
}

static void
gh_conversation_store_init(GhConversationStore *self)
{
  self->conversations = g_ptr_array_new_with_free_func(g_object_unref);
  self->rooms = g_hash_table_new(g_str_hash, g_str_equal);
  self->messages = g_hash_table_new(g_str_hash, g_str_equal);
}
