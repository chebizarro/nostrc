/* NIP-17 room model: canonical room ids, per-room order and dedup, subject,
 * unread bookkeeping (including a durable room's unloaded older history)
 * and the account-bound store. No relay or signer. */
#include "gh-conversation-private.h"
#include "gh-conversation-store.h"

#include "nostr-event.h"
#include "nostr-keys.h"
#include "nostr-tag.h"

#include <stdlib.h>
#include <string.h>

static gchar *hex[4];

static void
init_keys(void)
{
  static const gchar *const secrets[] = {
    NULL,
    "0000000000000000000000000000000000000000000000000000000000000001",
    "0000000000000000000000000000000000000000000000000000000000000002",
    "0000000000000000000000000000000000000000000000000000000000000003",
  };
  for (guint key = 1; key < 4; key++) {
    char *pub = nostr_key_get_public(secrets[key]);
    g_assert_nonnull(pub);
    hex[key] = g_strdup(pub);
    free(pub);
  }
}

typedef struct {
  guint author;
  guint p[4]; /* 0-terminated */
  gint64 created_at;
  const gchar *content;
  const gchar *subject;
  gboolean no_id;   /* omit the id: the model computes it */
  gboolean bad_id;
  gboolean signed_; /* a signed event is not a rumor */
  int kind;         /* 0: 14 */
  const gchar *raw_p; /* an extra p tag with this literal value */
  const gchar *expiration;
} Rumor;

static gchar *
rumor_json(const Rumor *r, gchar **id_out)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, r->kind ? r->kind : 14);
  nostr_event_set_pubkey(event, hex[r->author]);
  nostr_event_set_created_at(event, r->created_at);
  nostr_event_set_content(event, r->content ? r->content : "text");
  NostrTags *tags = nostr_tags_new(0);
  for (guint i = 0; r->p[i]; i++)
    nostr_tags_append(tags, nostr_tag_new("p", hex[r->p[i]], NULL));
  if (r->raw_p)
    nostr_tags_append(tags, nostr_tag_new("p", r->raw_p, NULL));
  if (r->subject)
    nostr_tags_append(tags, nostr_tag_new("subject", r->subject, NULL));
  if (r->expiration)
    nostr_tags_append(tags, nostr_tag_new("expiration", r->expiration, NULL));
  nostr_event_set_tags(event, tags);
  if (r->signed_) {
    static const gchar *const secrets[] = {
      NULL,
      "0000000000000000000000000000000000000000000000000000000000000001",
      "0000000000000000000000000000000000000000000000000000000000000002",
      "0000000000000000000000000000000000000000000000000000000000000003",
    };
    g_assert_cmpint(nostr_event_sign(event, secrets[r->author]), ==, 0);
  } else if (r->bad_id) {
    event->id = strdup("00000000000000000000000000000000000000000000000000000000000000ab");
  } else if (!r->no_id) {
    event->id = nostr_event_get_id(event);
  }
  if (id_out) {
    char *id = nostr_event_get_id(event);
    *id_out = g_strdup(id);
    free(id);
  }
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  gchar *copy = g_strdup(json);
  free(json);
  return copy;
}

static GhMessage *
message_for(guint account, const Rumor *r, gchar **id_out)
{
  g_autofree gchar *json = rumor_json(r, id_out);
  g_autoptr(GError) error = NULL;
  GhMessage *message = gh_message_new_from_rumor(hex[account], json, &error);
  g_assert_no_error(error);
  g_assert_nonnull(message);
  return message;
}

static GhConversationAddResult
add(GhConversationStore *store, guint account, const Rumor *r)
{
  g_autoptr(GhMessage) message = message_for(account, r, NULL);
  return gh_conversation_store_add_message(store, message, NULL);
}

static gint
compare_hex(gconstpointer a, gconstpointer b)
{
  return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

static gchar *
room_id(guint a, guint b, guint c)
{
  g_autoptr(GPtrArray) members = g_ptr_array_new();
  g_ptr_array_add(members, hex[a]);
  if (b)
    g_ptr_array_add(members, hex[b]);
  if (c)
    g_ptr_array_add(members, hex[c]);
  g_ptr_array_sort(members, compare_hex);
  g_ptr_array_add(members, NULL);
  return g_strjoinv(",", (gchar **)members->pdata);
}

typedef struct {
  guint calls;
  guint position, removed, added;
} ItemsChanged;

static void
on_items_changed(GListModel *model, guint position, guint removed, guint added,
                 gpointer data)
{
  ItemsChanged *seen = data;
  (void)model;
  seen->calls++;
  seen->position = position;
  seen->removed = removed;
  seen->added = added;
}

static void
test_message_fields(void)
{
  g_autofree gchar *id = NULL;
  Rumor r = { .author = 1, .p = { 2, 3, 2 }, .created_at = 42, .content = "hello",
              .subject = "Plans", .no_id = TRUE };
  g_autoptr(GhMessage) message = message_for(2, &r, &id);
  g_assert_cmpstr(gh_message_get_rumor_id(message), ==, id);
  g_assert_cmpstr(gh_message_get_account(message), ==, hex[2]);
  g_assert_cmpstr(gh_message_get_sender(message), ==, hex[1]);
  g_assert_cmpint(gh_message_get_created_at(message), ==, 42);
  g_assert_cmpstr(gh_message_get_content(message), ==, "hello");
  g_assert_cmpstr(gh_message_get_subject(message), ==, "Plans");
  g_assert_false(gh_message_is_self(message));
  /* Recipients unique in tag order; participants sorted incl. the author. */
  const gchar *const *recipients = gh_message_get_recipients(message);
  g_assert_cmpstr(recipients[0], ==, hex[2]);
  g_assert_cmpstr(recipients[1], ==, hex[3]);
  g_assert_null(recipients[2]);
  g_autofree gchar *room = room_id(1, 2, 3);
  g_assert_cmpstr(gh_message_get_room_id(message), ==, room);
  g_assert_cmpuint(g_strv_length((gchar **)gh_message_get_participants(message)), ==, 3);
  g_assert_null(gh_message_get_relays(message)[0]);
  g_assert_true(gh_message_add_relay(message, "wss://a.test.invalid"));
  g_assert_false(gh_message_add_relay(message, "wss://a.test.invalid"));
  g_assert_cmpstr(gh_message_get_relays(message)[0], ==, "wss://a.test.invalid");

  /* Not a chat message this account may hold. */
  struct { const gchar *name; Rumor r; guint account; } bad[] = {
    { "signed", { .author = 1, .p = { 2 }, .created_at = 1, .signed_ = TRUE }, 2 },
    { "kind 15", { .author = 1, .p = { 2 }, .created_at = 1, .kind = 15 }, 2 },
    { "bad id", { .author = 1, .p = { 2 }, .created_at = 1, .bad_id = TRUE }, 2 },
    { "no p", { .author = 1, .created_at = 1 }, 1 },
    { "upper p", { .author = 1, .p = { 2 }, .created_at = 1,
                   .raw_p = "79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798" }, 2 },
    { "no time", { .author = 1, .p = { 2 }, .created_at = 0 }, 2 },
    { "outsider", { .author = 1, .p = { 2 }, .created_at = 1 }, 3 },
  };
  for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
    g_test_message("invalid rumor: %s", bad[i].name);
    g_autofree gchar *json = rumor_json(&bad[i].r, NULL);
    g_autoptr(GError) error = NULL;
    g_assert_null(gh_message_new_from_rumor(hex[bad[i].account], json, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  }
  g_autoptr(GError) error = NULL;
  g_assert_null(gh_message_new_from_rumor("not-hex", "{}", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_room_canonicalization(void)
{
  g_autoptr(GhConversationStore) store = gh_conversation_store_new();
  gh_conversation_store_set_account(store, hex[1], NULL, NULL, NULL);
  /* A -> B, B -> A and A's own self-copy of a second message: one room. */
  Rumor a_to_b = { .author = 1, .p = { 2 }, .created_at = 10, .content = "a to b" };
  Rumor b_to_a = { .author = 2, .p = { 1 }, .created_at = 11, .content = "b to a" };
  Rumor self_copy = { .author = 1, .p = { 2, 1 }, .created_at = 12, .content = "cc me" };
  g_assert_cmpint(add(store, 1, &a_to_b), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(add(store, 1, &b_to_a), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(add(store, 1, &self_copy), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(store)), ==, 1);
  g_autofree gchar *pair = room_id(1, 2, 0);
  GhConversation *room = gh_conversation_store_lookup(store, pair);
  g_assert_nonnull(room);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(room)), ==, 3);
  g_assert_cmpstr(gh_conversation_get_account(room), ==, hex[1]);
  g_assert_cmpstr(gh_conversation_get_peers(room)[0], ==, hex[2]);
  g_assert_null(gh_conversation_get_peers(room)[1]);

  /* Adding C, from either side, is a different room. */
  Rumor a_to_bc = { .author = 1, .p = { 2, 3 }, .created_at = 13 };
  Rumor c_to_ab = { .author = 3, .p = { 1, 2 }, .created_at = 14 };
  g_assert_cmpint(add(store, 1, &a_to_bc), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(add(store, 1, &c_to_ab), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(store)), ==, 2);
  g_autofree gchar *trio = room_id(1, 2, 3);
  GhConversation *group = gh_conversation_store_lookup(store, trio);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(group)), ==, 2);
  g_assert_cmpuint(g_strv_length((gchar **)gh_conversation_get_peers(group)), ==, 2);

  /* A note to self is its own one-member room. */
  Rumor note = { .author = 1, .p = { 1 }, .created_at = 5 };
  g_assert_cmpint(add(store, 1, &note), ==, GH_CONVERSATION_ADD_NEW);
  g_autofree gchar *self_room = room_id(1, 0, 0);
  GhConversation *notes = gh_conversation_store_lookup(store, self_room);
  g_assert_nonnull(notes);
  g_assert_null(gh_conversation_get_peers(notes)[0]);
}

static void
test_order_and_dedup(void)
{
  g_autoptr(GhConversationStore) store = gh_conversation_store_new();
  gh_conversation_store_set_account(store, hex[2], NULL, NULL, NULL);
  gint64 times[] = { 30, 10, 20, 20, 5 };
  g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < G_N_ELEMENTS(times); i++) {
    gchar *id = NULL;
    g_autofree gchar *content = g_strdup_printf("m%u", i);
    Rumor r = { .author = 1, .p = { 2 }, .created_at = times[i], .content = content };
    g_autoptr(GhMessage) message = message_for(2, &r, &id);
    g_ptr_array_add(ids, id);
    g_assert_cmpint(gh_conversation_store_add_message(store, message, NULL), ==,
                    GH_CONVERSATION_ADD_NEW);
  }
  g_autofree gchar *pair = room_id(1, 2, 0);
  GhConversation *room = gh_conversation_store_lookup(store, pair);
  guint n = g_list_model_get_n_items(G_LIST_MODEL(room));
  g_assert_cmpuint(n, ==, 5);
  for (guint i = 1; i < n; i++) {
    g_autoptr(GhMessage) a = g_list_model_get_item(G_LIST_MODEL(room), i - 1);
    g_autoptr(GhMessage) b = g_list_model_get_item(G_LIST_MODEL(room), i);
    g_assert_cmpint(gh_message_compare(a, b), <, 0);
    g_assert_true(gh_message_get_created_at(a) < gh_message_get_created_at(b) ||
                  (gh_message_get_created_at(a) == gh_message_get_created_at(b) &&
                   strcmp(gh_message_get_rumor_id(a), gh_message_get_rumor_id(b)) < 0));
  }
  g_assert_cmpint(gh_conversation_get_last_activity(room), ==, 30);

  /* The same rumor again (another relay or a re-wrap) is stored once; the
   * relay that carried the copy is merged into the stored message. */
  ItemsChanged changed = { 0 };
  g_signal_connect(room, "items-changed", G_CALLBACK(on_items_changed), &changed);
  Rumor again = { .author = 1, .p = { 2 }, .created_at = 10, .content = "m1" };
  g_autoptr(GhMessage) copy = message_for(2, &again, NULL);
  gh_message_add_relay(copy, "wss://b.test.invalid");
  g_assert_cmpint(gh_conversation_store_add_message(store, copy, NULL), ==,
                  GH_CONVERSATION_ADD_DUPLICATE);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(room)), ==, 5);
  g_assert_cmpuint(changed.calls, ==, 0);
  GhMessage *stored = gh_conversation_store_lookup_message(store, g_ptr_array_index(ids, 1));
  g_assert_true(stored != copy);
  g_assert_cmpstr(gh_message_get_relays(stored)[0], ==, "wss://b.test.invalid");
  g_assert_true(gh_conversation_store_has_message(store, g_ptr_array_index(ids, 4)));
  g_signal_handlers_disconnect_by_data(room, &changed);
}

static void
test_subject(void)
{
  g_autoptr(GhConversationStore) store = gh_conversation_store_new();
  gh_conversation_store_set_account(store, hex[1], NULL, NULL, NULL);
  Rumor named = { .author = 2, .p = { 1, 3 }, .created_at = 20, .subject = "Newer" };
  Rumor older = { .author = 3, .p = { 1, 2 }, .created_at = 10, .subject = "Older" };
  Rumor plain = { .author = 1, .p = { 2, 3 }, .created_at = 30 };
  g_assert_cmpint(add(store, 1, &named), ==, GH_CONVERSATION_ADD_NEW);
  g_autofree gchar *trio = room_id(1, 2, 3);
  GhConversation *room = gh_conversation_store_lookup(store, trio);
  g_assert_cmpstr(gh_conversation_get_subject(room), ==, "Newer");
  /* A late-arriving older subject does not rename the room; a later message
   * without a subject keeps the name. */
  g_assert_cmpint(add(store, 1, &older), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(add(store, 1, &plain), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpstr(gh_conversation_get_subject(room), ==, "Newer");
  g_autofree gchar *property = NULL;
  g_object_get(room, "subject", &property, NULL);
  g_assert_cmpstr(property, ==, "Newer");
  /* The latest subject wins, and an empty one clears the name. */
  Rumor renamed = { .author = 3, .p = { 1, 2 }, .created_at = 40, .subject = "Renamed" };
  g_assert_cmpint(add(store, 1, &renamed), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpstr(gh_conversation_get_subject(room), ==, "Renamed");
  Rumor cleared = { .author = 2, .p = { 1, 3 }, .created_at = 50, .subject = "" };
  g_assert_cmpint(add(store, 1, &cleared), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_null(gh_conversation_get_subject(room));
}

static void
test_unread(void)
{
  g_autoptr(GhConversationStore) store = gh_conversation_store_new();
  gh_conversation_store_set_account(store, hex[1], NULL, NULL, NULL);
  Rumor in1 = { .author = 2, .p = { 1 }, .created_at = 10 };
  Rumor in2 = { .author = 2, .p = { 1 }, .created_at = 20 };
  g_assert_cmpint(add(store, 1, &in1), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(add(store, 1, &in2), ==, GH_CONVERSATION_ADD_NEW);
  g_autofree gchar *pair = room_id(1, 2, 0);
  GhConversation *room = gh_conversation_store_lookup(store, pair);
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, 2);
  gh_conversation_mark_read(room);
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, 0);
  /* Backfill older than the marker stays read; newer incoming is unread. */
  Rumor old = { .author = 2, .p = { 1 }, .created_at = 5 };
  Rumor in3 = { .author = 2, .p = { 1 }, .created_at = 30 };
  g_assert_cmpint(add(store, 1, &old), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(add(store, 1, &in3), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, 1);
  /* Replying implies reading what came before; own messages never count. */
  Rumor reply = { .author = 1, .p = { 2 }, .created_at = 35 };
  g_assert_cmpint(add(store, 1, &reply), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, 0);
  Rumor in4 = { .author = 2, .p = { 1 }, .created_at = 40 };
  g_assert_cmpint(add(store, 1, &in4), ==, GH_CONVERSATION_ADD_NEW);
  guint unread = 0;
  g_object_get(room, "unread-count", &unread, NULL);
  g_assert_cmpuint(unread, ==, 1);
}

static void
test_store_order_and_account(void)
{
  g_autoptr(GhConversationStore) store = gh_conversation_store_new();
  ItemsChanged changed = { 0 };
  g_signal_connect(store, "items-changed", G_CALLBACK(on_items_changed), &changed);
  /* Unbound: nothing is accepted. */
  Rumor early = { .author = 2, .p = { 1 }, .created_at = 10 };
  g_autoptr(GhMessage) unbound = message_for(1, &early, NULL);
  g_autoptr(GError) error = NULL;
  g_assert_cmpint(gh_conversation_store_add_message(store, unbound, &error), ==,
                  GH_CONVERSATION_ADD_REJECTED);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error(&error);

  gh_conversation_store_set_account(store, hex[1], NULL, NULL, NULL);
  g_assert_cmpint(add(store, 1, &early), ==, GH_CONVERSATION_ADD_NEW);
  Rumor group = { .author = 3, .p = { 1, 2 }, .created_at = 20 };
  g_assert_cmpint(add(store, 1, &group), ==, GH_CONVERSATION_ADD_NEW);
  g_autofree gchar *pair = room_id(1, 2, 0);
  g_autofree gchar *trio = room_id(1, 2, 3);
  g_autoptr(GhConversation) first = g_list_model_get_item(G_LIST_MODEL(store), 0);
  g_assert_cmpstr(gh_conversation_get_room_id(first), ==, trio);
  g_assert_cmpuint(changed.calls, ==, 2);
  g_assert_cmpuint(changed.position, ==, 0);
  g_assert_cmpuint(changed.added, ==, 1);

  /* New activity moves the pair to the top: one items-changed over the span. */
  Rumor later = { .author = 2, .p = { 1 }, .created_at = 30 };
  g_assert_cmpint(add(store, 1, &later), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpuint(changed.calls, ==, 3);
  g_assert_cmpuint(changed.position, ==, 0);
  g_assert_cmpuint(changed.removed, ==, 2);
  g_assert_cmpuint(changed.added, ==, 2);
  g_autoptr(GhConversation) top = g_list_model_get_item(G_LIST_MODEL(store), 0);
  g_assert_cmpstr(gh_conversation_get_room_id(top), ==, pair);
  /* Older backfill in the top room does not reorder anything. */
  Rumor backfill = { .author = 2, .p = { 1 }, .created_at = 1 };
  g_assert_cmpint(add(store, 1, &backfill), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpuint(changed.calls, ==, 3);

  /* Another account's message is refused, even when it names this one. */
  g_autoptr(GhMessage) foreign = message_for(2, &later, NULL);
  g_assert_cmpint(gh_conversation_store_add_message(store, foreign, &error), ==,
                  GH_CONVERSATION_ADD_REJECTED);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);

  /* Switching accounts drops every room before the next account's appear. */
  gh_conversation_store_set_account(store, hex[1], NULL, NULL, NULL);
  g_assert_cmpuint(changed.calls, ==, 3); /* same account: no-op */
  gh_conversation_store_set_account(store, hex[2], NULL, NULL, NULL);
  g_assert_cmpuint(changed.calls, ==, 4);
  g_assert_cmpuint(changed.removed, ==, 2);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(store)), ==, 0);
  g_assert_null(gh_conversation_store_lookup(store, pair));
  g_assert_cmpstr(gh_conversation_store_get_account(store), ==, hex[2]);
  g_assert_cmpint(add(store, 2, &later), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(store)), ==, 1);
  g_signal_handlers_disconnect_by_data(store, &changed);
}

typedef struct {
  guint count;
  GhConversation *conversation;
  GhMessage *message;
} AddedProbe;

static void
on_message_added(GhConversationStore *store, GhConversation *conversation,
                 GhMessage *message, gpointer data)
{
  AddedProbe *probe = data;
  probe->count++;
  probe->conversation = conversation;
  probe->message = message;
  /* The store is already consistent when the signal runs. */
  g_assert_true(gh_conversation_store_has_message(store, gh_message_get_rumor_id(message)));
  g_assert_true(gh_conversation_lookup_message(conversation,
                                               gh_message_get_rumor_id(message)) == message);
}

static void
test_message_added_signal(void)
{
  g_autoptr(GhConversationStore) store = gh_conversation_store_new();
  gh_conversation_store_set_account(store, hex[1], NULL, NULL, NULL);
  AddedProbe probe = { 0 };
  g_signal_connect(store, "message-added", G_CALLBACK(on_message_added), &probe);
  Rumor r = { .author = 2, .p = { 1 }, .created_at = 10 };
  g_autoptr(GhMessage) message = message_for(1, &r, NULL);
  g_assert_cmpint(gh_conversation_store_add_message(store, message, NULL), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(gh_conversation_store_add_message(store, message, NULL), ==,
                  GH_CONVERSATION_ADD_DUPLICATE);
  g_assert_cmpuint(probe.count, ==, 1);
  g_assert_true(probe.message == message);
}

/* A recording persistence delegate standing in for the encrypted store. */
typedef struct {
  GHashTable *wraps;
  GHashTable *rumors;
  GPtrArray *admits; /* "rumor-id wrap-id|-" per admit() call */
  gboolean fail;
  gboolean hide;     /* commit as seen only */
  guint destroyed;
  GHashTable *rejected;
  guint reads;
  guint accepts;
} FakeDelegate;

static gboolean
fake_has_wrap(gpointer data, const gchar *wrap_id)
{
  return g_hash_table_contains(((FakeDelegate *)data)->wraps, wrap_id);
}

static gboolean
fake_has_rumor(gpointer data, const gchar *rumor_id)
{
  return g_hash_table_contains(((FakeDelegate *)data)->rumors, rumor_id);
}

static gboolean
fake_admit(gpointer data, GhMessage *message, const gchar *wrap_id,
           GhConversationCommit *commit, GError **error)
{
  FakeDelegate *fake = data;
  g_assert_false(commit->hidden);
  g_assert_cmpint(commit->unread, ==, -1);
  commit->hidden = fake->hide;
  g_ptr_array_add(fake->admits, g_strdup_printf("%s %s", gh_message_get_rumor_id(message),
                                                wrap_id ? wrap_id : "-"));
  if (fake->fail) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "disk full");
    return FALSE;
  }
  if (wrap_id)
    g_hash_table_add(fake->wraps, g_strdup(wrap_id));
  g_hash_table_add(fake->rumors, g_strdup(gh_message_get_rumor_id(message)));
  return TRUE;
}

static void
fake_destroy(gpointer data)
{
  ((FakeDelegate *)data)->destroyed++;
}

static const GhConversationDelegate fake_delegate = {
  .has_wrap = fake_has_wrap, .has_rumor = fake_has_rumor, .admit = fake_admit,
};

static gboolean
fake_has_rejected(gpointer data, const gchar *wrap_id)
{
  return g_hash_table_contains(((FakeDelegate *)data)->rejected, wrap_id);
}

static gboolean
fake_add_rejected(gpointer data, const gchar *wrap_id, GError **error)
{
  (void)error;
  g_hash_table_add(((FakeDelegate *)data)->rejected, g_strdup(wrap_id));
  return TRUE;
}

static gboolean
fake_mark_read(gpointer data, GhConversation *conversation, GhMessage *last_read,
               GError **error)
{
  (void)error;
  g_assert_true(gh_conversation_lookup_message(conversation,
                                               gh_message_get_rumor_id(last_read)) == last_read);
  ((FakeDelegate *)data)->reads++;
  return TRUE;
}

static gboolean
fake_accept(gpointer data, GhConversation *conversation, GError **error)
{
  (void)error;
  g_assert_false(gh_conversation_get_is_request(conversation));
  ((FakeDelegate *)data)->accepts++;
  return TRUE;
}

/* The optional members of a durable delegate. */
static const GhConversationDelegate full_delegate = {
  .has_wrap = fake_has_wrap, .has_rumor = fake_has_rumor, .admit = fake_admit,
  .has_rejected = fake_has_rejected, .add_rejected = fake_add_rejected,
  .mark_read = fake_mark_read, .accept = fake_accept,
};

static void
fake_init(FakeDelegate *fake)
{
  fake->wraps = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  fake->rumors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  fake->admits = g_ptr_array_new_with_free_func(g_free);
  fake->rejected = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

static void
fake_clear(FakeDelegate *fake)
{
  g_hash_table_unref(fake->wraps);
  g_hash_table_unref(fake->rumors);
  g_ptr_array_unref(fake->admits);
  g_hash_table_unref(fake->rejected);
}

static void
test_persistence_delegate(void)
{
  FakeDelegate fake = { 0 };
  fake_init(&fake);
  GhConversationStore *store = gh_conversation_store_new();
  gh_conversation_store_set_account(store, hex[1], &fake_delegate, &fake, fake_destroy);
  AddedProbe probe = { 0 };
  g_signal_connect(store, "message-added", G_CALLBACK(on_message_added), &probe);
  const gchar *wrap_a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const gchar *wrap_b = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  const gchar *wrap_c = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

  /* A failed commit changes nothing and marks nothing seen. */
  g_autofree gchar *id = NULL;
  Rumor r = { .author = 2, .p = { 1 }, .created_at = 10 };
  g_autoptr(GhMessage) message = message_for(1, &r, &id);
  fake.fail = TRUE;
  g_autoptr(GError) error = NULL;
  g_assert_cmpint(gh_conversation_store_admit(store, message, wrap_a, &error), ==,
                  GH_CONVERSATION_ADD_FAILED);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
  g_clear_error(&error);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(store)), ==, 0);
  g_assert_false(gh_conversation_store_has_wrap(store, wrap_a));
  g_assert_cmpuint(probe.count, ==, 0);

  /* Exactly one commit per admission, before the model changes. */
  fake.fail = FALSE;
  g_assert_cmpint(gh_conversation_store_admit(store, message, wrap_a, NULL), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_cmpuint(fake.admits->len, ==, 2);
  g_autofree gchar *first = g_strdup_printf("%s %s", id, wrap_a);
  g_assert_cmpstr(g_ptr_array_index(fake.admits, 1), ==, first);
  g_assert_true(gh_conversation_store_has_wrap(store, wrap_a));
  g_assert_cmpuint(probe.count, ==, 1);
  /* A second wrap of the same rumor commits its wrap id as a duplicate. */
  g_assert_cmpint(gh_conversation_store_admit(store, message, wrap_b, NULL), ==,
                  GH_CONVERSATION_ADD_DUPLICATE);
  g_assert_cmpuint(fake.admits->len, ==, 3);
  g_assert_true(gh_conversation_store_has_wrap(store, wrap_b));

  /* A rumor the delegate already holds but the model does not (a restart
   * with the in-memory model) is committed idempotently and not shown. */
  g_autofree gchar *known_id = NULL;
  Rumor known = { .author = 3, .p = { 1 }, .created_at = 11 };
  g_autoptr(GhMessage) known_message = message_for(1, &known, &known_id);
  g_hash_table_add(fake.rumors, g_strdup(known_id));
  g_assert_cmpint(gh_conversation_store_admit(store, known_message, wrap_c, NULL), ==,
                  GH_CONVERSATION_ADD_DUPLICATE);
  g_assert_false(gh_conversation_store_has_message(store, known_id));
  g_assert_true(gh_conversation_store_has_wrap(store, wrap_c));

  /* A local echo has no wrap yet. */
  Rumor echo = { .author = 1, .p = { 2 }, .created_at = 12 };
  g_autofree gchar *echo_id = NULL;
  g_autoptr(GhMessage) echo_message = message_for(1, &echo, &echo_id);
  g_assert_cmpint(gh_conversation_store_add_message(store, echo_message, NULL), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_autofree gchar *echo_admit = g_strdup_printf("%s -", echo_id);
  g_assert_cmpstr(g_ptr_array_index(fake.admits, fake.admits->len - 1), ==, echo_admit);

  /* The same account may swap its delegate and keeps its rooms; another
   * account releases it and drops them. The store owns the delegate data. */
  FakeDelegate next = { 0 };
  fake_init(&next);
  gh_conversation_store_set_account(store, hex[1], &fake_delegate, &next, fake_destroy);
  g_assert_cmpuint(fake.destroyed, ==, 1);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(store)), ==, 1);
  g_assert_false(gh_conversation_store_has_wrap(store, wrap_a));
  gh_conversation_store_set_account(store, hex[2], NULL, NULL, NULL);
  g_assert_cmpuint(next.destroyed, ==, 1);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(store)), ==, 0);
  g_assert_false(gh_conversation_store_has_wrap(store, wrap_a));
  g_signal_handlers_disconnect_by_data(store, &probe);
  g_object_unref(store);
  g_assert_cmpuint(next.destroyed, ==, 1);
  fake_clear(&fake);
  fake_clear(&next);
}

/* HIDDEN commits, the rejected namespace, and the read/accept writes of a
 * durable delegate, each made once per change. */
static void
test_delegate_hooks(void)
{
  FakeDelegate fake = { 0 };
  fake_init(&fake);
  g_autoptr(GhConversationStore) store = gh_conversation_store_new();
  gh_conversation_store_set_account(store, hex[1], &full_delegate, &fake, NULL);
  AddedProbe probe = { 0 };
  g_signal_connect(store, "message-added", G_CALLBACK(on_message_added), &probe);
  const gchar *wrap_a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const gchar *wrap_b = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

  /* Committed as seen only (e.g. expired on arrival): never listed. */
  Rumor expired = { .author = 2, .p = { 1 }, .created_at = 10, .expiration = "11" };
  g_autoptr(GhMessage) hidden = message_for(1, &expired, NULL);
  fake.hide = TRUE;
  g_assert_cmpint(gh_conversation_store_admit(store, hidden, wrap_a, NULL), ==,
                  GH_CONVERSATION_ADD_HIDDEN);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(store)), ==, 0);
  g_assert_cmpuint(probe.count, ==, 0);
  fake.hide = FALSE;

  /* The rejected namespace is the delegate's. */
  g_assert_false(gh_conversation_store_has_rejected(store, wrap_b));
  g_assert_true(gh_conversation_store_record_rejected(store, wrap_b, NULL));
  g_assert_true(gh_conversation_store_has_rejected(store, wrap_b));
  g_assert_false(gh_conversation_store_has_wrap(store, wrap_b));

  /* Accepting a request and moving the read marker are written once. */
  Rumor in = { .author = 2, .p = { 1 }, .created_at = 20 };
  g_assert_cmpint(add(store, 1, &in), ==, GH_CONVERSATION_ADD_NEW);
  g_autofree gchar *pair = room_id(1, 2, 0);
  GhConversation *room = gh_conversation_store_lookup(store, pair);
  gh_conversation_accept(room);
  gh_conversation_accept(room);
  g_assert_cmpuint(fake.accepts, ==, 1);
  gh_conversation_mark_read(room);
  gh_conversation_mark_read(room);
  g_assert_cmpuint(fake.reads, ==, 1);
  Rumor reply = { .author = 1, .p = { 2 }, .created_at = 30 };
  g_assert_cmpint(add(store, 1, &reply), ==, GH_CONVERSATION_ADD_NEW);
  gh_conversation_mark_read(room); /* the reply already moved the marker */
  g_assert_cmpuint(fake.reads, ==, 1);
  Rumor more = { .author = 2, .p = { 1 }, .created_at = 40 };
  g_assert_cmpint(add(store, 1, &more), ==, GH_CONVERSATION_ADD_NEW);
  gh_conversation_mark_read(room);
  g_assert_cmpuint(fake.reads, ==, 2);
  /* A room the store no longer lists (account switch) writes nothing. */
  Rumor late = { .author = 2, .p = { 1 }, .created_at = 50 };
  g_assert_cmpint(add(store, 1, &late), ==, GH_CONVERSATION_ADD_NEW);
  g_object_ref(room);
  gh_conversation_store_set_account(store, hex[2], NULL, NULL, NULL);
  gh_conversation_mark_read(room);
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, 0);
  g_assert_cmpuint(fake.reads, ==, 2);
  g_object_unref(room);
  g_signal_handlers_disconnect_by_data(store, &probe);
  fake_clear(&fake);
}

/* W13b review B1: showing a durably stored room never reads unread messages
 * that are not listed. mark_read then reads only the listed ones, for this
 * session, and writes no marker (the unloaded ones stay unread after a
 * restart too); once the older page is listed, it reads everything and
 * writes the marker once. */
static void count_notify(GObject *object, GParamSpec *pspec, gpointer data);

static void
test_mark_read_keeps_unloaded_unread(void)
{
  FakeDelegate fake = { 0 };
  fake_init(&fake);
  g_autoptr(GhConversationStore) store = gh_conversation_store_new();
  gh_conversation_store_set_account(store, hex[1], &full_delegate, &fake, NULL);
  g_autoptr(GPtrArray) older = g_ptr_array_new_with_free_func(g_object_unref);
  g_autoptr(GPtrArray) newest = g_ptr_array_new_with_free_func(g_object_unref);
  for (guint i = 0; i < 4; i++) {
    Rumor r = { .author = 2, .p = { 1 }, .created_at = 100 + i };
    g_ptr_array_add(older, message_for(1, &r, NULL));
  }
  for (guint i = 0; i < 5; i++) {
    Rumor r = { .author = 2, .p = { 1 }, .created_at = 200 + i };
    g_ptr_array_add(newest, message_for(1, &r, NULL));
  }
  GhMessage *marker = g_ptr_array_index(older, 0);
  GhMessage *floor = g_ptr_array_index(newest, 0);
  /* Read up to the first older message: 3 unloaded and 5 listed unread. */
  GhConversationState state = {
    .accepted = TRUE,
    .has_marker = TRUE,
    .marker_created_at = gh_message_get_created_at(marker),
    .marker_id = gh_message_get_rumor_id(marker),
    .unread = 8,
    .has_older = TRUE,
    .floor_created_at = gh_message_get_created_at(floor),
    .floor_id = gh_message_get_rumor_id(floor),
  };
  g_autofree gchar *pair = room_id(1, 2, 0);
  GhConversation *room = gh_conversation_store_restore(store, pair, newest, &state);
  g_assert_nonnull(room);
  guint first = 99;
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, 8);
  g_assert_cmpuint(gh_conversation_get_listed_unread(room, &first), ==, 5);
  g_assert_cmpuint(first, ==, 0);

  /* Shown: the listed five are read, the unloaded three are not, and no
   * marker is written past them. */
  guint notified = 0;
  g_signal_connect(room, "notify::unread-count", G_CALLBACK(count_notify), &notified);
  gh_conversation_mark_read(room);
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, 3);
  g_assert_cmpuint(gh_conversation_get_listed_unread(room, &first), ==, 0);
  g_assert_cmpuint(first, ==, 5);
  g_assert_cmpuint(notified, ==, 1);
  gh_conversation_mark_read(room);
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, 3);
  g_assert_cmpuint(fake.reads, ==, 0);

  /* A new message while it is shown is unread until read, then read in
   * the same way. */
  Rumor fresh = { .author = 2, .p = { 1 }, .created_at = 300 };
  g_assert_cmpint(add(store, 1, &fresh), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, 4);
  g_assert_cmpuint(gh_conversation_get_listed_unread(room, &first), ==, 1);
  g_assert_cmpuint(first, ==, 5);
  gh_conversation_mark_read(room);
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, 3);
  g_assert_cmpuint(fake.reads, ==, 0);

  /* The older page is listed: its three unread are listed now (the store's
   * count still has all nine, nothing having been written), and reading the
   * room reads everything and writes the marker once. */
  state.unread = 9;
  state.has_older = FALSE;
  g_assert_true(gh_conversation_store_restore(store, pair, older, &state) == room);
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, 3);
  g_assert_cmpuint(gh_conversation_get_listed_unread(room, &first), ==, 3);
  g_assert_cmpuint(first, ==, 1);
  gh_conversation_mark_read(room);
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, 0);
  g_assert_cmpuint(gh_conversation_get_listed_unread(room, NULL), ==, 0);
  g_assert_cmpuint(fake.reads, ==, 1);
  g_signal_handlers_disconnect_by_data(room, &notified);
  fake_clear(&fake);
}

static void
count_notify(GObject *object, GParamSpec *pspec, gpointer data)
{
  (void)object;
  (void)pspec;
  (*(guint *)data)++;
}

static void
test_requests(void)
{
  g_autoptr(GhConversationStore) store = gh_conversation_store_new();
  gh_conversation_store_set_account(store, hex[1], NULL, NULL, NULL);
  /* Someone the account never wrote to starts a request. */
  Rumor incoming = { .author = 2, .p = { 1 }, .created_at = 10 };
  g_assert_cmpint(add(store, 1, &incoming), ==, GH_CONVERSATION_ADD_NEW);
  g_autofree gchar *pair = room_id(1, 2, 0);
  GhConversation *room = gh_conversation_store_lookup(store, pair);
  g_assert_true(gh_conversation_get_is_request(room));
  guint notified = 0;
  g_signal_connect(room, "notify::is-request", G_CALLBACK(count_notify), &notified);
  /* More incoming messages keep it a request. */
  Rumor again = { .author = 2, .p = { 1 }, .created_at = 11 };
  g_assert_cmpint(add(store, 1, &again), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_true(gh_conversation_get_is_request(room));
  g_assert_cmpuint(notified, ==, 0);
  gh_conversation_accept(room);
  g_assert_false(gh_conversation_get_is_request(room));
  g_assert_cmpuint(notified, ==, 1);
  gh_conversation_accept(room);
  g_assert_cmpuint(notified, ==, 1);

  /* Writing in a room (or its self-copy arriving, even as backfill) accepts
   * it; so does a room this account started. */
  Rumor group_in = { .author = 3, .p = { 1, 2 }, .created_at = 20 };
  g_assert_cmpint(add(store, 1, &group_in), ==, GH_CONVERSATION_ADD_NEW);
  g_autofree gchar *trio = room_id(1, 2, 3);
  GhConversation *group = gh_conversation_store_lookup(store, trio);
  gboolean is_request = FALSE;
  g_object_get(group, "is-request", &is_request, NULL);
  g_assert_true(is_request);
  guint group_notified = 0;
  g_signal_connect(group, "notify::is-request", G_CALLBACK(count_notify), &group_notified);
  Rumor own_old = { .author = 1, .p = { 2, 3 }, .created_at = 5 };
  g_assert_cmpint(add(store, 1, &own_old), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_false(gh_conversation_get_is_request(group));
  g_assert_cmpuint(group_notified, ==, 1);
  Rumor started = { .author = 1, .p = { 3 }, .created_at = 30 };
  g_assert_cmpint(add(store, 1, &started), ==, GH_CONVERSATION_ADD_NEW);
  g_autofree gchar *mine = room_id(1, 3, 0);
  g_assert_false(gh_conversation_get_is_request(gh_conversation_store_lookup(store, mine)));
  g_signal_handlers_disconnect_by_data(room, &notified);
  g_signal_handlers_disconnect_by_data(group, &group_notified);
}

static void
test_template_properties(void)
{
  g_autoptr(GhConversationStore) store = gh_conversation_store_new();
  gh_conversation_store_set_account(store, hex[1], NULL, NULL, NULL);
  g_autofree gchar *long_body = g_strnfill(100, 'x');
  g_autofree gchar *body = g_strconcat("first line\nsecond", NULL);
  Rumor r = { .author = 2, .p = { 1 }, .created_at = 10, .content = body,
              .expiration = "1900000000" };
  g_autoptr(GhMessage) message = message_for(1, &r, NULL);
  g_assert_cmpint(gh_conversation_store_add_message(store, message, NULL), ==,
                  GH_CONVERSATION_ADD_NEW);

  /* Message properties the rows bind to. */
  g_autofree gchar *m_body = NULL;
  gboolean outgoing = TRUE;
  gint64 created_at = 0, expires_at = 0;
  gint kind = 0;
  GhMessageStatus status = GH_MESSAGE_STATUS_SENT;
  g_object_get(message, "body", &m_body, "is-outgoing", &outgoing, "created-at", &created_at,
               "kind", &kind, "expires-at", &expires_at, "status", &status, NULL);
  g_assert_cmpstr(m_body, ==, body);
  g_assert_false(outgoing);
  g_assert_cmpint(created_at, ==, 10);
  g_assert_cmpint(kind, ==, 14);
  g_assert_cmpint(expires_at, ==, 1900000000);
  g_assert_cmpint(status, ==, GH_MESSAGE_STATUS_NONE);
  /* Status is a sender-side fact: ignored on incoming, notified on own. */
  gh_message_set_status(message, GH_MESSAGE_STATUS_SENT);
  g_assert_cmpint(gh_message_get_status(message), ==, GH_MESSAGE_STATUS_NONE);
  Rumor own = { .author = 1, .p = { 2 }, .created_at = 20, .content = long_body };
  g_autoptr(GhMessage) echo = message_for(1, &own, NULL);
  guint status_notified = 0;
  g_signal_connect(echo, "notify::status", G_CALLBACK(count_notify), &status_notified);
  gh_message_set_status(echo, GH_MESSAGE_STATUS_WAITING_FOR_SIGNER);
  gh_message_set_status(echo, GH_MESSAGE_STATUS_WAITING_FOR_SIGNER);
  g_assert_cmpuint(status_notified, ==, 1);
  g_assert_null(g_enum_get_value_by_nick(g_type_class_peek(GH_TYPE_MESSAGE_STATUS), "delivered"));
  g_assert_null(g_enum_get_value_by_nick(g_type_class_peek(GH_TYPE_MESSAGE_STATUS), "read"));

  /* Conversation properties the list rows bind to. */
  g_autofree gchar *pair = room_id(1, 2, 0);
  GhConversation *room = gh_conversation_store_lookup(store, pair);
  g_autofree gchar *title = NULL, *preview = NULL, *room_prop = NULL;
  GhConversationBackend backend = 0;
  g_object_get(room, "title", &title, "preview", &preview, "room-id", &room_prop,
               "backend", &backend, NULL);
  /* No subject: the peer's abbreviated npub, never a fetched profile name. */
  g_assert_true(g_str_has_prefix(title, "npub1"));
  g_assert_nonnull(strstr(title, "…"));
  g_assert_cmpstr(preview, ==, "first line");
  g_assert_cmpstr(room_prop, ==, pair);
  g_assert_cmpint(backend, ==, GH_CONVERSATION_BACKEND_NIP17);
  guint title_notified = 0, preview_notified = 0;
  g_signal_connect(room, "notify::title", G_CALLBACK(count_notify), &title_notified);
  g_signal_connect(room, "notify::preview", G_CALLBACK(count_notify), &preview_notified);
  g_assert_cmpint(gh_conversation_store_add_message(store, echo, NULL), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_cmpuint(preview_notified, ==, 1);
  const gchar *cut = gh_conversation_get_preview(room);
  g_assert_cmpuint(g_utf8_strlen(cut, -1), ==, 81); /* 80 characters and an ellipsis */
  /* Older backfill does not change the preview. */
  Rumor old = { .author = 2, .p = { 1 }, .created_at = 1, .content = "old" };
  g_assert_cmpint(add(store, 1, &old), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpuint(preview_notified, ==, 1);
  Rumor named = { .author = 2, .p = { 1 }, .created_at = 30, .subject = "Lunch" };
  g_assert_cmpint(add(store, 1, &named), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpstr(gh_conversation_get_title(room), ==, "Lunch");
  g_assert_cmpuint(title_notified, ==, 1);
  g_signal_handlers_disconnect_by_data(room, &title_notified);
  g_signal_handlers_disconnect_by_data(room, &preview_notified);
  g_signal_handlers_disconnect_by_data(echo, &status_notified);

  /* A note to self is titled with the account's own npub. */
  Rumor note = { .author = 1, .p = { 1 }, .created_at = 40 };
  g_assert_cmpint(add(store, 1, &note), ==, GH_CONVERSATION_ADD_NEW);
  g_autofree gchar *self_room = room_id(1, 0, 0);
  g_assert_true(g_str_has_prefix(
    gh_conversation_get_title(gh_conversation_store_lookup(store, self_room)), "npub1"));
}

/* Charter §7.9 (W13 review #4): a request's subject is text its sender
 * chose, so until the request is accepted the room is titled by the
 * sender's npub and the subject stays available as secondary text. The
 * title notifies when accepting or replying makes the subject the name. */
static void
test_request_title(void)
{
  g_autoptr(GhConversationStore) store = gh_conversation_store_new();
  gh_conversation_store_set_account(store, hex[1], NULL, NULL, NULL);
  Rumor offer = { .author = 2, .p = { 1 }, .created_at = 10, .content = "hi",
                  .subject = "You won a prize" };
  g_assert_cmpint(add(store, 1, &offer), ==, GH_CONVERSATION_ADD_NEW);
  g_autofree gchar *pair = room_id(1, 2, 0);
  GhConversation *room = gh_conversation_store_lookup(store, pair);
  g_assert_true(gh_conversation_get_is_request(room));
  g_assert_cmpstr(gh_conversation_get_subject(room), ==, "You won a prize");
  const gchar *title = gh_conversation_get_title(room);
  g_assert_true(g_str_has_prefix(title, "npub1"));
  g_assert_nonnull(strstr(title, "…"));
  g_autofree gchar *property = NULL;
  g_object_get(room, "title", &property, NULL);
  g_assert_cmpstr(property, ==, title);
  /* A later subject renames nothing while it is a request. */
  guint title_notified = 0, subject_notified = 0;
  g_signal_connect(room, "notify::title", G_CALLBACK(count_notify), &title_notified);
  g_signal_connect(room, "notify::subject", G_CALLBACK(count_notify), &subject_notified);
  Rumor again = { .author = 2, .p = { 1 }, .created_at = 20, .content = "hurry",
                  .subject = "Claim it now" };
  g_assert_cmpint(add(store, 1, &again), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpuint(subject_notified, ==, 1);
  g_assert_cmpuint(title_notified, ==, 0);
  g_assert_true(g_str_has_prefix(gh_conversation_get_title(room), "npub1"));
  /* Accepted: the subject names it, and the title says so once. */
  gh_conversation_accept(room);
  g_assert_cmpstr(gh_conversation_get_title(room), ==, "Claim it now");
  g_assert_cmpuint(title_notified, ==, 1);
  g_signal_handlers_disconnect_by_data(room, &title_notified);
  g_signal_handlers_disconnect_by_data(room, &subject_notified);

  /* Replying accepts too. */
  Rumor other = { .author = 3, .p = { 1 }, .created_at = 30, .content = "hey",
                  .subject = "Party" };
  g_assert_cmpint(add(store, 1, &other), ==, GH_CONVERSATION_ADD_NEW);
  g_autofree gchar *trio = room_id(1, 3, 0);
  GhConversation *party = gh_conversation_store_lookup(store, trio);
  g_assert_true(g_str_has_prefix(gh_conversation_get_title(party), "npub1"));
  title_notified = 0;
  g_signal_connect(party, "notify::title", G_CALLBACK(count_notify), &title_notified);
  Rumor reply = { .author = 1, .p = { 3 }, .created_at = 40, .content = "coming" };
  g_assert_cmpint(add(store, 1, &reply), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_false(gh_conversation_get_is_request(party));
  g_assert_cmpstr(gh_conversation_get_title(party), ==, "Party");
  g_assert_cmpuint(title_notified, ==, 1);
  g_signal_handlers_disconnect_by_data(party, &title_notified);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  init_keys();
  g_test_add_func("/groundhog/conversations/message-fields", test_message_fields);
  g_test_add_func("/groundhog/conversations/room-canonicalization", test_room_canonicalization);
  g_test_add_func("/groundhog/conversations/order-and-dedup", test_order_and_dedup);
  g_test_add_func("/groundhog/conversations/subject", test_subject);
  g_test_add_func("/groundhog/conversations/request-title", test_request_title);
  g_test_add_func("/groundhog/conversations/unread", test_unread);
  g_test_add_func("/groundhog/conversations/store-order-and-account",
                  test_store_order_and_account);
  g_test_add_func("/groundhog/conversations/message-added", test_message_added_signal);
  g_test_add_func("/groundhog/conversations/persistence-delegate", test_persistence_delegate);
  g_test_add_func("/groundhog/conversations/delegate-hooks", test_delegate_hooks);
  g_test_add_func("/groundhog/conversations/mark-read-keeps-unloaded-unread",
                  test_mark_read_keeps_unloaded_unread);
  g_test_add_func("/groundhog/conversations/requests", test_requests);
  g_test_add_func("/groundhog/conversations/template-properties", test_template_properties);
  int status = g_test_run();
  for (guint key = 1; key < 4; key++)
    g_free(hex[key]);
  return status;
}
