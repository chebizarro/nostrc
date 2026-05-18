/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-group-service.c - NIP-29 group service/control plane
 */

#include "gn-nip29-group-service.h"

#include <json-glib/json-glib.h>
#include <nip29.h>
#include <nostr-event.h>
#include <nostr-kinds.h>
#include <nostr-tag.h>
#include <string.h>

#define SAVED_GROUPS_KEY "saved-groups.json"
#define ANONYMOUS_ACCOUNT_KEY "__anonymous__"

#define SNAPSHOT_LIMIT 64
#define MESSAGE_LIMIT 200

/* Fallbacks for older kind headers during in-flight rebases. */
#ifndef NOSTR_KIND_SIMPLE_GROUP_CHAT_MESSAGE
#define NOSTR_KIND_SIMPLE_GROUP_CHAT_MESSAGE 9
#endif
#ifndef NOSTR_KIND_SIMPLE_GROUP_THREADED_REPLY
#define NOSTR_KIND_SIMPLE_GROUP_THREADED_REPLY 10
#endif
#ifndef NOSTR_KIND_SIMPLE_GROUP_THREAD
#define NOSTR_KIND_SIMPLE_GROUP_THREAD 11
#endif
#ifndef NOSTR_KIND_SIMPLE_GROUP_REPLY
#define NOSTR_KIND_SIMPLE_GROUP_REPLY 12
#endif
#ifndef NOSTR_KIND_SIMPLE_GROUP_CREATE_GROUP
#define NOSTR_KIND_SIMPLE_GROUP_CREATE_GROUP 9007
#endif
#ifndef NOSTR_KIND_SIMPLE_GROUP_JOIN_REQUEST
#define NOSTR_KIND_SIMPLE_GROUP_JOIN_REQUEST 9021
#endif
#ifndef NOSTR_KIND_SIMPLE_GROUP_LEAVE_REQUEST
#define NOSTR_KIND_SIMPLE_GROUP_LEAVE_REQUEST 9022
#endif

typedef struct
{
  gchar  *relay_url;
  gchar  *group_id;
  gchar  *alias;
  gint64   last_opened;
} SavedGroup;

typedef struct
{
  gchar  *id;
  gchar  *relay_url;
  gchar  *event_json;
  gchar  *pubkey;
  gint64   created_at;
  gint     kind;
} GroupMessage;

typedef struct
{
  gchar        *key;
  gchar        *relay_url;
  gchar        *group_id;
  gchar        *alias;
  gint64        last_opened;

  nostr_group_t *group;

  GPtrArray    *messages;          /* GroupMessage* */
  GHashTable   *seen_message_ids;  /* id string set */

  guint64       snapshot_subscription_id;
  guint64       message_subscription_id;
  guint64       refresh_generation;
} GroupState;

typedef struct
{
  GnNip29GroupService *service;
  gchar               *group_key;
  guint64              generation;
  gboolean             snapshots;
  GCancellable        *cancellable;
} QueryData;

typedef struct
{
  GnNip29GroupService *service;
  gchar               *group_key;
  guint64              generation;
} SubscriptionData;

typedef enum
{
  ACTION_CREATE_GROUP,
  ACTION_JOIN_GROUP,
  ACTION_LEAVE_GROUP,
  ACTION_SEND_MESSAGE,
} ActionKind;

typedef struct
{
  ActionKind kind;
  gchar     *relay_url;
  gchar     *group_id;
  gchar     *group_key;
  gchar     *name;
  gchar     *about;
  gchar     *picture;
  gchar     *invite_code;
  gchar     *reason;
  gchar     *content;
  gboolean   is_private;
  gboolean   is_restricted;
  gboolean   is_hidden;
  gboolean   is_closed;
} ActionData;

struct _GnNip29GroupService
{
  GObject parent_instance;

  GnostrPluginContext *context;
  gchar               *current_pubkey;

  GHashTable          *saved_accounts; /* account key -> GPtrArray<SavedGroup*> */
  GHashTable          *groups;         /* relay'group -> GroupState* */
  GPtrArray           *cancellables;   /* GCancellable* */
  guint64              next_refresh_generation;

  gboolean             loaded_saved_groups;
  gboolean             identity_initialized;
  gboolean             shutting_down;
};

enum
{
  GROUPS_CHANGED,
  GROUP_UPDATED,
  ERROR_REPORTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE(GnNip29GroupService, gn_nip29_group_service, G_TYPE_OBJECT)

static void group_state_refresh(GroupState *state, GnNip29GroupService *self);
static gchar *make_group_key(const char *relay_url, const char *group_id);

static void
saved_group_free(SavedGroup *saved)
{
  if (saved == NULL)
    return;
  g_free(saved->relay_url);
  g_free(saved->group_id);
  g_free(saved->alias);
  g_free(saved);
}

static void
saved_group_array_free(GPtrArray *array)
{
  if (array != NULL)
    g_ptr_array_unref(array);
}

static void
group_message_free(GroupMessage *message)
{
  if (message == NULL)
    return;
  g_free(message->id);
  g_free(message->relay_url);
  g_free(message->event_json);
  g_free(message->pubkey);
  g_free(message);
}

static void
group_state_free(GroupState *state)
{
  if (state == NULL)
    return;
  g_free(state->key);
  g_free(state->relay_url);
  g_free(state->group_id);
  g_free(state->alias);
  if (state->group != NULL)
    nostr_free_group(state->group);
  g_clear_pointer(&state->messages, g_ptr_array_unref);
  g_clear_pointer(&state->seen_message_ids, g_hash_table_destroy);
  g_free(state);
}

static gboolean
group_state_reset_snapshots(GroupState *state,
                            GError    **error)
{
  g_return_val_if_fail(state != NULL, FALSE);

  g_autofree gchar *key = make_group_key(state->relay_url, state->group_id);
  nostr_group_t *group = nostr_new_group(key);
  if (group == NULL)
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "Failed to reset NIP-29 snapshot state for %s", key);
      return FALSE;
    }

  if (state->group != NULL)
    nostr_free_group(state->group);
  state->group = group;
  return TRUE;
}

static void
query_data_free(QueryData *data)
{
  if (data == NULL)
    return;
  g_clear_object(&data->service);
  g_clear_object(&data->cancellable);
  g_free(data->group_key);
  g_free(data);
}

static void
subscription_data_free(SubscriptionData *data)
{
  if (data == NULL)
    return;
  g_clear_object(&data->service);
  g_free(data->group_key);
  g_free(data);
}

static void
action_data_free(ActionData *data)
{
  if (data == NULL)
    return;
  g_free(data->relay_url);
  g_free(data->group_id);
  g_free(data->group_key);
  g_free(data->name);
  g_free(data->about);
  g_free(data->picture);
  g_free(data->invite_code);
  g_free(data->reason);
  g_free(data->content);
  g_free(data);
}

static gchar *
make_group_key(const char *relay_url,
               const char *group_id)
{
  return g_strdup_printf("%s'%s", relay_url, group_id);
}

static const char *
current_account_key(GnNip29GroupService *self)
{
  return (self->current_pubkey != NULL && self->current_pubkey[0] != '\0')
           ? self->current_pubkey
           : ANONYMOUS_ACCOUNT_KEY;
}

static gboolean
validate_group_address(const char *relay_url,
                       const char *group_id,
                       GError    **error)
{
  if (relay_url == NULL || relay_url[0] == '\0')
    {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                          "NIP-29 group relay URL is required");
      return FALSE;
    }
  if (group_id == NULL || group_id[0] == '\0')
    {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                          "NIP-29 group id is required");
      return FALSE;
    }

  g_autofree gchar *key = make_group_key(relay_url, group_id);
  nostr_group_address_t address = {0};
  gboolean ok = nostr_group_address_parse(key, &address) &&
                nostr_group_address_is_valid(&address);
  nostr_group_address_clear(&address);
  if (!ok)
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                  "Invalid NIP-29 group address: %s", key);
      return FALSE;
    }

  return TRUE;
}

static GroupState *
group_state_new(const char *relay_url,
                const char *group_id,
                const char *alias,
                gint64      last_opened)
{
  g_autofree gchar *key = make_group_key(relay_url, group_id);
  nostr_group_t *group = nostr_new_group(key);
  if (group == NULL)
    return NULL;

  GroupState *state = g_new0(GroupState, 1);
  state->key = g_strdup(key);
  state->relay_url = g_strdup(relay_url);
  state->group_id = g_strdup(group_id);
  state->alias = g_strdup(alias);
  state->last_opened = last_opened;
  state->group = group;
  state->messages = g_ptr_array_new_with_free_func((GDestroyNotify)group_message_free);
  state->seen_message_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  return state;
}

static GPtrArray *
ensure_saved_bucket(GnNip29GroupService *self,
                    const char          *account)
{
  GPtrArray *bucket = g_hash_table_lookup(self->saved_accounts, account);
  if (bucket == NULL)
    {
      bucket = g_ptr_array_new_with_free_func((GDestroyNotify)saved_group_free);
      g_hash_table_insert(self->saved_accounts, g_strdup(account), bucket);
    }
  return bucket;
}

static SavedGroup *
saved_bucket_find(GPtrArray  *bucket,
                  const char *relay_url,
                  const char *group_id)
{
  if (bucket == NULL)
    return NULL;

  for (guint i = 0; i < bucket->len; i++)
    {
      SavedGroup *saved = g_ptr_array_index(bucket, i);
      if (g_strcmp0(saved->relay_url, relay_url) == 0 &&
          g_strcmp0(saved->group_id, group_id) == 0)
        return saved;
    }
  return NULL;
}

static void
emit_error(GnNip29GroupService *self,
           const char          *message)
{
  if (message == NULL)
    return;
  g_warning("NIP-29 Groups service: %s", message);
  g_signal_emit(self, signals[ERROR_REPORTED], 0, message);
}

static void
cancel_pending_queries(GnNip29GroupService *self)
{
  if (self->cancellables == NULL)
    return;

  for (guint i = 0; i < self->cancellables->len; i++)
    {
      GCancellable *cancellable = g_ptr_array_index(self->cancellables, i);
      if (cancellable != NULL)
        g_cancellable_cancel(cancellable);
    }

  g_ptr_array_set_size(self->cancellables, 0);
}

static void
unsubscribe_group(GroupState *state,
                  GnNip29GroupService *self)
{
  if (state == NULL || self == NULL || self->context == NULL)
    return;

  if (state->snapshot_subscription_id > 0)
    {
      gnostr_plugin_context_unsubscribe_relays(self->context,
                                               state->snapshot_subscription_id);
      state->snapshot_subscription_id = 0;
    }

  if (state->message_subscription_id > 0)
    {
      gnostr_plugin_context_unsubscribe_relays(self->context,
                                               state->message_subscription_id);
      state->message_subscription_id = 0;
    }
}

static void
unsubscribe_all_groups(GnNip29GroupService *self)
{
  if (self->groups == NULL)
    return;

  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->groups);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    unsubscribe_group(value, self);
}

static void
clear_active_groups(GnNip29GroupService *self)
{
  unsubscribe_all_groups(self);
  if (self->groups != NULL)
    g_hash_table_remove_all(self->groups);
}

static gboolean
json_object_dup_string(JsonObject  *object,
                       const char  *member,
                       gchar      **out)
{
  if (!json_object_has_member(object, member))
    return FALSE;
  const char *value = json_object_get_string_member(object, member);
  if (value == NULL)
    return FALSE;
  *out = g_strdup(value);
  return TRUE;
}

static SavedGroup *
saved_group_from_json(JsonObject *object)
{
  g_autofree gchar *relay_url = NULL;
  g_autofree gchar *group_id = NULL;
  g_autofree gchar *alias = NULL;

  if (!json_object_dup_string(object, "relay_url", &relay_url) ||
      !json_object_dup_string(object, "group_id", &group_id))
    return NULL;

  json_object_dup_string(object, "alias", &alias);

  gint64 last_opened = 0;
  if (json_object_has_member(object, "last_opened"))
    last_opened = json_object_get_int_member(object, "last_opened");

  if (!validate_group_address(relay_url, group_id, NULL))
    return NULL;

  SavedGroup *saved = g_new0(SavedGroup, 1);
  saved->relay_url = g_steal_pointer(&relay_url);
  saved->group_id = g_steal_pointer(&group_id);
  saved->alias = g_steal_pointer(&alias);
  saved->last_opened = last_opened;
  return saved;
}

static void
load_saved_groups(GnNip29GroupService *self)
{
  if (self->loaded_saved_groups)
    return;
  self->loaded_saved_groups = TRUE;

  g_autoptr(GError) error = NULL;
  GBytes *bytes = gnostr_plugin_context_load_data(self->context, SAVED_GROUPS_KEY, &error);
  if (error != NULL)
    {
      g_debug("NIP-29 Groups service: no saved groups loaded: %s", error->message);
      return;
    }
  if (bytes == NULL)
    return;

  gsize size = 0;
  const char *json = g_bytes_get_data(bytes, &size);
  if (json == NULL || size == 0)
    {
      g_bytes_unref(bytes);
      return;
    }

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, json, size, &error))
    {
      emit_error(self, error ? error->message : "Failed to parse saved-groups.json");
      g_bytes_unref(bytes);
      return;
    }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root))
    {
      g_bytes_unref(bytes);
      return;
    }

  JsonObject *root_object = json_node_get_object(root);
  if (json_object_has_member(root_object, "version") &&
      json_object_get_int_member(root_object, "version") != 1)
    {
      emit_error(self, "Unsupported saved-groups.json version");
      g_bytes_unref(bytes);
      return;
    }

  if (!json_object_has_member(root_object, "accounts"))
    {
      g_bytes_unref(bytes);
      return;
    }

  JsonObject *accounts = json_object_get_object_member(root_object, "accounts");
  if (accounts == NULL)
    {
      g_bytes_unref(bytes);
      return;
    }

  GList *members = json_object_get_members(accounts);
  for (GList *l = members; l != NULL; l = l->next)
    {
      const char *account = l->data;
      JsonArray *groups = json_object_get_array_member(accounts, account);
      if (groups == NULL)
        continue;

      GPtrArray *bucket = ensure_saved_bucket(self, account);
      guint len = json_array_get_length(groups);
      for (guint i = 0; i < len; i++)
        {
          JsonObject *group_object = json_array_get_object_element(groups, i);
          if (group_object == NULL)
            continue;
          SavedGroup *saved = saved_group_from_json(group_object);
          if (saved != NULL &&
              saved_bucket_find(bucket, saved->relay_url, saved->group_id) == NULL)
            g_ptr_array_add(bucket, saved);
          else
            saved_group_free(saved);
        }
    }
  g_list_free(members);
  g_bytes_unref(bytes);
}

static gboolean
save_saved_groups(GnNip29GroupService *self,
                  GError             **error)
{
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);

  json_builder_set_member_name(builder, "version");
  json_builder_add_int_value(builder, 1);

  json_builder_set_member_name(builder, "accounts");
  json_builder_begin_object(builder);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->saved_accounts);
  while (g_hash_table_iter_next(&iter, &key, &value))
    {
      const char *account = key;
      GPtrArray *bucket = value;

      json_builder_set_member_name(builder, account);
      json_builder_begin_array(builder);

      for (guint i = 0; bucket != NULL && i < bucket->len; i++)
        {
          SavedGroup *saved = g_ptr_array_index(bucket, i);
          json_builder_begin_object(builder);

          json_builder_set_member_name(builder, "relay_url");
          json_builder_add_string_value(builder, saved->relay_url);
          json_builder_set_member_name(builder, "group_id");
          json_builder_add_string_value(builder, saved->group_id);
          if (saved->alias != NULL && saved->alias[0] != '\0')
            {
              json_builder_set_member_name(builder, "alias");
              json_builder_add_string_value(builder, saved->alias);
            }
          json_builder_set_member_name(builder, "last_opened");
          json_builder_add_int_value(builder, saved->last_opened);

          json_builder_end_object(builder);
        }

      json_builder_end_array(builder);
    }

  json_builder_end_object(builder);
  json_builder_end_object(builder);

  JsonNode *root = json_builder_get_root(builder);
  g_autoptr(JsonGenerator) generator = json_generator_new();
  json_generator_set_root(generator, root);

  gsize len = 0;
  char *json = json_generator_to_data(generator, &len);
  json_node_unref(root);
  if (json == NULL)
    {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "Failed to serialize saved-groups.json");
      return FALSE;
    }

  GBytes *bytes = g_bytes_new_take(json, len);
  gboolean ok = gnostr_plugin_context_store_data(self->context, SAVED_GROUPS_KEY,
                                                 bytes, error);
  g_bytes_unref(bytes);
  return ok;
}

static gchar *
build_filter_json(const int  *kinds,
                  gsize       n_kinds,
                  const char *tag_name,
                  const char *tag_value,
                  guint       limit)
{
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);

  json_builder_set_member_name(builder, "kinds");
  json_builder_begin_array(builder);
  for (gsize i = 0; i < n_kinds; i++)
    json_builder_add_int_value(builder, kinds[i]);
  json_builder_end_array(builder);

  json_builder_set_member_name(builder, tag_name);
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, tag_value);
  json_builder_end_array(builder);

  if (limit > 0)
    {
      json_builder_set_member_name(builder, "limit");
      json_builder_add_int_value(builder, limit);
    }

  json_builder_end_object(builder);

  JsonNode *root = json_builder_get_root(builder);
  g_autoptr(JsonGenerator) generator = json_generator_new();
  json_generator_set_root(generator, root);
  gchar *json = json_generator_to_data(generator, NULL);
  json_node_unref(root);
  return json;
}

static gboolean
event_has_tag_value(NostrEvent *event,
                    const char *tag_name,
                    const char *tag_value)
{
  NostrTags *tags = nostr_event_get_tags(event);
  if (tags == NULL)
    return FALSE;

  for (size_t i = 0; i < tags->count; i++)
    {
      NostrTag *tag = nostr_tags_get(tags, i);
      if (tag == NULL || nostr_tag_size(tag) < 2)
        continue;
      if (g_strcmp0(nostr_tag_get(tag, 0), tag_name) == 0 &&
          g_strcmp0(nostr_tag_get(tag, 1), tag_value) == 0)
        return TRUE;
    }

  return FALSE;
}

static NostrEvent *
parse_event_json(const char *event_json)
{
  if (event_json == NULL || event_json[0] == '\0')
    return NULL;

  NostrEvent *event = nostr_event_new();
  if (event == NULL)
    return NULL;

  if (!nostr_event_deserialize_compact(event, event_json, NULL))
    {
      nostr_event_free(event);
      return NULL;
    }

  return event;
}

static void
json_add_tag1(JsonBuilder *builder,
              const char  *name)
{
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, name);
  json_builder_end_array(builder);
}

static void
json_add_tag2(JsonBuilder *builder,
              const char  *name,
              const char  *value)
{
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, name);
  json_builder_add_string_value(builder, value ? value : "");
  json_builder_end_array(builder);
}

static void
append_previous_tag(JsonBuilder *builder,
                    GroupState  *state,
                    const char  *current_pubkey)
{
  (void)current_pubkey;
  if (state == NULL || state->messages == NULL || state->messages->len == 0)
    return;

  const guint max_refs = 3;
  g_autoptr(GPtrArray) refs = g_ptr_array_new_with_free_func(g_free);
  guint considered = 0;

  for (gint i = (gint)state->messages->len - 1;
       i >= 0 && considered < 50 && refs->len < max_refs;
       i--, considered++)
    {
      GroupMessage *message = g_ptr_array_index(state->messages, i);
      if (message == NULL || message->id == NULL || strlen(message->id) < 8)
        continue;
      g_ptr_array_add(refs, g_strndup(message->id, 8));
    }

  if (refs->len == 0)
    return;

  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "previous");
  for (guint i = 0; i < refs->len; i++)
    json_builder_add_string_value(builder, g_ptr_array_index(refs, i));
  json_builder_end_array(builder);
}

static gchar *
build_action_event_json(GnNip29GroupService *self,
                        ActionData          *data,
                        GroupState          *state,
                        GError             **error)
{
  g_return_val_if_fail(data != NULL, NULL);

  gint event_kind = 0;
  const char *content = "";

  switch (data->kind)
    {
    case ACTION_CREATE_GROUP:
      event_kind = NOSTR_KIND_SIMPLE_GROUP_CREATE_GROUP;
      break;
    case ACTION_JOIN_GROUP:
      event_kind = NOSTR_KIND_SIMPLE_GROUP_JOIN_REQUEST;
      content = data->reason ? data->reason : "";
      break;
    case ACTION_LEAVE_GROUP:
      event_kind = NOSTR_KIND_SIMPLE_GROUP_LEAVE_REQUEST;
      content = data->reason ? data->reason : "";
      break;
    case ACTION_SEND_MESSAGE:
      event_kind = NOSTR_KIND_SIMPLE_GROUP_CHAT_MESSAGE;
      content = data->content ? data->content : "";
      break;
    default:
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                          "Unknown NIP-29 action");
      return NULL;
    }

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);

  json_builder_set_member_name(builder, "kind");
  json_builder_add_int_value(builder, event_kind);
  json_builder_set_member_name(builder, "created_at");
  json_builder_add_int_value(builder, (gint64)(g_get_real_time() / G_USEC_PER_SEC));
  json_builder_set_member_name(builder, "content");
  json_builder_add_string_value(builder, content);

  json_builder_set_member_name(builder, "tags");
  json_builder_begin_array(builder);

  json_add_tag2(builder, "h", data->group_id);

  if (data->kind == ACTION_CREATE_GROUP)
    {
      if (data->name != NULL && data->name[0] != '\0')
        json_add_tag2(builder, "name", data->name);
      if (data->about != NULL && data->about[0] != '\0')
        json_add_tag2(builder, "about", data->about);
      if (data->picture != NULL && data->picture[0] != '\0')
        json_add_tag2(builder, "picture", data->picture);
      if (data->is_private)
        json_add_tag1(builder, "private");
      if (data->is_restricted)
        json_add_tag1(builder, "restricted");
      if (data->is_hidden)
        json_add_tag1(builder, "hidden");
      if (data->is_closed)
        json_add_tag1(builder, "closed");
    }
  else
    {
      append_previous_tag(builder, state, self->current_pubkey);
      if (data->kind == ACTION_JOIN_GROUP &&
          data->invite_code != NULL && data->invite_code[0] != '\0')
        json_add_tag2(builder, "code", data->invite_code);
    }

  json_builder_end_array(builder);
  json_builder_end_object(builder);

  JsonNode *root = json_builder_get_root(builder);
  g_autoptr(JsonGenerator) generator = json_generator_new();
  json_generator_set_root(generator, root);
  gchar *json = json_generator_to_data(generator, NULL);
  json_node_unref(root);

  if (json == NULL)
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Failed to serialize NIP-29 action event");
  return json;
}

static gint
compare_messages(gconstpointer a,
                 gconstpointer b)
{
  const GroupMessage *ma = *(GroupMessage * const *)a;
  const GroupMessage *mb = *(GroupMessage * const *)b;

  if (ma->created_at < mb->created_at)
    return -1;
  if (ma->created_at > mb->created_at)
    return 1;
  return g_strcmp0(ma->id, mb->id);
}

static gboolean
is_group_message_kind(gint kind)
{
  return kind == NOSTR_KIND_SIMPLE_GROUP_CHAT_MESSAGE ||
         kind == NOSTR_KIND_SIMPLE_GROUP_THREADED_REPLY ||
         kind == NOSTR_KIND_SIMPLE_GROUP_THREAD ||
         kind == NOSTR_KIND_SIMPLE_GROUP_REPLY;
}

static gboolean
merge_snapshot_event(GroupState *state,
                     NostrEvent *event)
{
  switch (nostr_event_get_kind(event))
    {
    case NOSTR_KIND_SIMPLE_GROUP_METADATA:
      return nostr_group_merge_in_metadata_event(state->group, event);
    case NOSTR_KIND_SIMPLE_GROUP_ADMINS:
      return nostr_group_merge_in_admins_event(state->group, event);
    case NOSTR_KIND_SIMPLE_GROUP_MEMBERS:
      return nostr_group_merge_in_members_event(state->group, event);
    case NOSTR_KIND_SIMPLE_GROUP_ROLES:
      return nostr_group_merge_in_roles_event(state->group, event);
    default:
      return FALSE;
    }
}

static void
process_snapshot_json(GnNip29GroupService *self,
                      GroupState          *state,
                      const char          *relay_url,
                      const char          *event_json)
{
  if (g_strcmp0(relay_url, state->relay_url) != 0)
    return;

  NostrEvent *event = parse_event_json(event_json);
  if (event == NULL)
    return;

  if (!event_has_tag_value(event, "d", state->group_id))
    {
      nostr_event_free(event);
      return;
    }

  if (merge_snapshot_event(state, event))
    g_signal_emit(self, signals[GROUP_UPDATED], 0, state->key);

  nostr_event_free(event);
}

static void
process_message_json(GnNip29GroupService *self,
                     GroupState          *state,
                     const char          *relay_url,
                     const char          *event_json)
{
  if (g_strcmp0(relay_url, state->relay_url) != 0)
    return;

  NostrEvent *event = parse_event_json(event_json);
  if (event == NULL)
    return;

  const gint kind = nostr_event_get_kind(event);
  g_autofree gchar *id = nostr_event_get_id(event);
  if (id == NULL || id[0] == '\0' ||
      !is_group_message_kind(kind) ||
      !event_has_tag_value(event, "h", state->group_id) ||
      g_hash_table_contains(state->seen_message_ids, id))
    {
      nostr_event_free(event);
      return;
    }

  GroupMessage *message = g_new0(GroupMessage, 1);
  message->id = g_strdup(id);
  message->relay_url = g_strdup(relay_url);
  message->event_json = g_strdup(event_json);
  message->pubkey = g_strdup(nostr_event_get_pubkey(event));
  message->created_at = nostr_event_get_created_at(event);
  message->kind = kind;

  g_hash_table_add(state->seen_message_ids, g_strdup(message->id));
  g_ptr_array_add(state->messages, message);
  g_ptr_array_sort(state->messages, compare_messages);

  g_signal_emit(self, signals[GROUP_UPDATED], 0, state->key);
  nostr_event_free(event);
}

static void
on_query_relays_done(GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  QueryData *data = user_data;
  GnNip29GroupService *self = data->service;

  if (self->cancellables != NULL && data->cancellable != NULL)
    g_ptr_array_remove(self->cancellables, data->cancellable);

  if (self->context == NULL)
    {
      query_data_free(data);
      return;
    }

  g_autoptr(GError) error = NULL;
  GPtrArray *events = gnostr_plugin_context_query_relays_finish(self->context,
                                                                result,
                                                                &error);
  if (error != NULL)
    {
      if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        emit_error(self, error->message);
      query_data_free(data);
      return;
    }

  GroupState *state = g_hash_table_lookup(self->groups, data->group_key);
  if (state == NULL || state->refresh_generation != data->generation)
    {
      if (events != NULL)
        g_ptr_array_unref(events);
      query_data_free(data);
      return;
    }

  for (guint i = 0; events != NULL && i < events->len; i++)
    {
      GnostrPluginRelayEvent *relay_event = g_ptr_array_index(events, i);
      if (relay_event == NULL)
        continue;
      if (data->snapshots)
        process_snapshot_json(self, state, relay_event->relay_url,
                              relay_event->event_json);
      else
        process_message_json(self, state, relay_event->relay_url,
                             relay_event->event_json);
    }

  if (events != NULL)
    g_ptr_array_unref(events);
  query_data_free(data);
}

static void
start_group_query(GnNip29GroupService *self,
                  GroupState          *state,
                  const int           *kinds,
                  gsize                n_kinds,
                  const char          *tag_name,
                  guint                limit,
                  gboolean             snapshots)
{
  if (self->context == NULL || self->shutting_down)
    return;

  g_autofree gchar *filter_json = build_filter_json(kinds, n_kinds, tag_name,
                                                    state->group_id, limit);
  const char *relay_urls[] = { state->relay_url, NULL };
  GnostrPluginRelayQuery query = {
    .relay_urls = relay_urls,
    .n_relay_urls = 1,
    .filter_json = filter_json,
  };

  QueryData *data = g_new0(QueryData, 1);
  data->service = g_object_ref(self);
  data->group_key = g_strdup(state->key);
  data->generation = state->refresh_generation;
  data->snapshots = snapshots;
  data->cancellable = g_cancellable_new();

  g_ptr_array_add(self->cancellables, g_object_ref(data->cancellable));
  gnostr_plugin_context_query_relays_async(self->context, &query,
                                           data->cancellable,
                                           on_query_relays_done,
                                           data);
}

static void
on_live_snapshot_event(GnostrPluginContext *context,
                       guint64              subscription_id,
                       const char          *relay_url,
                       const char          *event_json,
                       gpointer             user_data)
{
  SubscriptionData *data = user_data;
  GroupState *state = g_hash_table_lookup(data->service->groups, data->group_key);
  if (state == NULL ||
      state->snapshot_subscription_id != subscription_id ||
      state->refresh_generation != data->generation)
    return;

  process_snapshot_json(data->service, state, relay_url, event_json);
}

static void
on_live_message_event(GnostrPluginContext *context,
                      guint64              subscription_id,
                      const char          *relay_url,
                      const char          *event_json,
                      gpointer             user_data)
{
  SubscriptionData *data = user_data;
  GroupState *state = g_hash_table_lookup(data->service->groups, data->group_key);
  if (state == NULL ||
      state->message_subscription_id != subscription_id ||
      state->refresh_generation != data->generation)
    return;

  process_message_json(data->service, state, relay_url, event_json);
}

static void
start_group_subscription(GnNip29GroupService    *self,
                         GroupState             *state,
                         const int              *kinds,
                         gsize                   n_kinds,
                         const char             *tag_name,
                         gboolean                snapshots)
{
  g_autofree gchar *filter_json = build_filter_json(kinds, n_kinds, tag_name,
                                                    state->group_id, 0);
  const char *relay_urls[] = { state->relay_url, NULL };
  GnostrPluginRelayQuery query = {
    .relay_urls = relay_urls,
    .n_relay_urls = 1,
    .filter_json = filter_json,
  };

  SubscriptionData *data = g_new0(SubscriptionData, 1);
  data->service = g_object_ref(self);
  data->group_key = g_strdup(state->key);
  data->generation = state->refresh_generation;

  g_autoptr(GError) error = NULL;
  guint64 sub_id = gnostr_plugin_context_subscribe_relays(
    self->context,
    &query,
    snapshots ? on_live_snapshot_event : on_live_message_event,
    NULL,
    data,
    (GDestroyNotify)subscription_data_free,
    &error);

  if (sub_id == 0)
    {
      if (error != NULL)
        emit_error(self, error->message);
      subscription_data_free(data);
      return;
    }

  if (snapshots)
    state->snapshot_subscription_id = sub_id;
  else
    state->message_subscription_id = sub_id;
}

static void
group_state_refresh(GroupState *state,
                    GnNip29GroupService *self)
{
  if (state == NULL || self == NULL || self->context == NULL || self->shutting_down)
    return;

  unsubscribe_group(state, self);

  g_autoptr(GError) error = NULL;
  if (!group_state_reset_snapshots(state, &error))
    {
      emit_error(self, error ? error->message : "Failed to reset group snapshots");
      return;
    }

  state->refresh_generation = ++self->next_refresh_generation;
  g_signal_emit(self, signals[GROUP_UPDATED], 0, state->key);

  const int snapshot_kinds[] = {
    NOSTR_KIND_SIMPLE_GROUP_METADATA,
    NOSTR_KIND_SIMPLE_GROUP_ADMINS,
    NOSTR_KIND_SIMPLE_GROUP_MEMBERS,
    NOSTR_KIND_SIMPLE_GROUP_ROLES,
  };
  const int message_kinds[] = {
    NOSTR_KIND_SIMPLE_GROUP_CHAT_MESSAGE,
    NOSTR_KIND_SIMPLE_GROUP_THREADED_REPLY,
    NOSTR_KIND_SIMPLE_GROUP_THREAD,
    NOSTR_KIND_SIMPLE_GROUP_REPLY,
  };

  start_group_query(self, state, snapshot_kinds, G_N_ELEMENTS(snapshot_kinds),
                    "#d", SNAPSHOT_LIMIT, TRUE);
  start_group_query(self, state, message_kinds, G_N_ELEMENTS(message_kinds),
                    "#h", MESSAGE_LIMIT, FALSE);

  start_group_subscription(self, state, snapshot_kinds, G_N_ELEMENTS(snapshot_kinds),
                           "#d", TRUE);
  start_group_subscription(self, state, message_kinds, G_N_ELEMENTS(message_kinds),
                           "#h", FALSE);
}

static void
restore_active_groups_for_current_account(GnNip29GroupService *self)
{
  const char *account = current_account_key(self);
  GPtrArray *bucket = g_hash_table_lookup(self->saved_accounts, account);
  if (bucket == NULL)
    return;

  for (guint i = 0; i < bucket->len; i++)
    {
      SavedGroup *saved = g_ptr_array_index(bucket, i);
      GroupState *state = group_state_new(saved->relay_url,
                                          saved->group_id,
                                          saved->alias,
                                          saved->last_opened);
      if (state == NULL)
        continue;
      g_hash_table_replace(self->groups, g_strdup(state->key), state);
    }
}

static void
gn_nip29_group_service_dispose(GObject *object)
{
  GnNip29GroupService *self = GN_NIP29_GROUP_SERVICE(object);

  gn_nip29_group_service_shutdown(self);

  G_OBJECT_CLASS(gn_nip29_group_service_parent_class)->dispose(object);
}

static void
gn_nip29_group_service_finalize(GObject *object)
{
  GnNip29GroupService *self = GN_NIP29_GROUP_SERVICE(object);

  g_clear_pointer(&self->current_pubkey, g_free);
  g_clear_pointer(&self->saved_accounts, g_hash_table_destroy);
  g_clear_pointer(&self->groups, g_hash_table_destroy);
  g_clear_pointer(&self->cancellables, g_ptr_array_unref);

  G_OBJECT_CLASS(gn_nip29_group_service_parent_class)->finalize(object);
}

static void
gn_nip29_group_service_class_init(GnNip29GroupServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gn_nip29_group_service_dispose;
  object_class->finalize = gn_nip29_group_service_finalize;

  signals[GROUPS_CHANGED] =
    g_signal_new("groups-changed",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 0);

  signals[GROUP_UPDATED] =
    g_signal_new("group-updated",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[ERROR_REPORTED] =
    g_signal_new("error-reported",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gn_nip29_group_service_init(GnNip29GroupService *self)
{
  self->saved_accounts = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free,
                                               (GDestroyNotify)saved_group_array_free);
  self->groups = g_hash_table_new_full(g_str_hash, g_str_equal,
                                       g_free,
                                       (GDestroyNotify)group_state_free);
  self->cancellables = g_ptr_array_new_with_free_func(g_object_unref);
}

GnNip29GroupService *
gn_nip29_group_service_new(GnostrPluginContext *context)
{
  g_return_val_if_fail(context != NULL, NULL);

  GnNip29GroupService *self = g_object_new(GN_TYPE_NIP29_GROUP_SERVICE, NULL);
  self->context = context;
  load_saved_groups(self);
  return self;
}

void
gn_nip29_group_service_shutdown(GnNip29GroupService *self)
{
  g_return_if_fail(GN_IS_NIP29_GROUP_SERVICE(self));

  if (self->shutting_down)
    return;

  self->shutting_down = TRUE;
  cancel_pending_queries(self);
  clear_active_groups(self);
  self->context = NULL;
}

void
gn_nip29_group_service_set_current_pubkey(GnNip29GroupService *self,
                                          const char          *pubkey_hex)
{
  g_return_if_fail(GN_IS_NIP29_GROUP_SERVICE(self));

  const char *normalized = (pubkey_hex != NULL && pubkey_hex[0] != '\0')
                             ? pubkey_hex
                             : NULL;
  if (self->identity_initialized &&
      g_strcmp0(self->current_pubkey, normalized) == 0)
    return;

  cancel_pending_queries(self);
  clear_active_groups(self);
  g_free(self->current_pubkey);
  self->current_pubkey = g_strdup(normalized);
  self->identity_initialized = TRUE;

  restore_active_groups_for_current_account(self);
  gn_nip29_group_service_refresh_all(self);

  g_signal_emit(self, signals[GROUPS_CHANGED], 0);
}

const char *
gn_nip29_group_service_get_current_pubkey(GnNip29GroupService *self)
{
  g_return_val_if_fail(GN_IS_NIP29_GROUP_SERVICE(self), NULL);
  return self->current_pubkey;
}

guint
gn_nip29_group_service_get_group_count(GnNip29GroupService *self)
{
  g_return_val_if_fail(GN_IS_NIP29_GROUP_SERVICE(self), 0);
  return self->groups ? g_hash_table_size(self->groups) : 0;
}

gboolean
gn_nip29_group_service_track_group(GnNip29GroupService *self,
                                   const char          *relay_url,
                                   const char          *group_id,
                                   const char          *alias,
                                   GError             **error)
{
  g_return_val_if_fail(GN_IS_NIP29_GROUP_SERVICE(self), FALSE);

  if (!validate_group_address(relay_url, group_id, error))
    return FALSE;

  const char *account = current_account_key(self);
  GPtrArray *bucket = ensure_saved_bucket(self, account);
  SavedGroup *saved = saved_bucket_find(bucket, relay_url, group_id);

  gint64 now = (gint64)(g_get_real_time() / G_USEC_PER_SEC);
  gboolean inserted_saved = FALSE;
  g_autofree gchar *old_alias = NULL;
  gint64 old_last_opened = 0;

  if (saved == NULL)
    {
      saved = g_new0(SavedGroup, 1);
      saved->relay_url = g_strdup(relay_url);
      saved->group_id = g_strdup(group_id);
      saved->alias = g_strdup(alias);
      saved->last_opened = now;
      g_ptr_array_add(bucket, saved);
      inserted_saved = TRUE;
    }
  else
    {
      old_alias = g_strdup(saved->alias);
      old_last_opened = saved->last_opened;
      g_free(saved->alias);
      saved->alias = g_strdup(alias);
      saved->last_opened = now;
    }

  if (!save_saved_groups(self, error))
    {
      if (inserted_saved)
        g_ptr_array_remove(bucket, saved);
      else
        {
          g_free(saved->alias);
          saved->alias = g_steal_pointer(&old_alias);
          saved->last_opened = old_last_opened;
        }
      return FALSE;
    }

  g_autofree gchar *key = make_group_key(relay_url, group_id);
  GroupState *state = g_hash_table_lookup(self->groups, key);
  if (state == NULL)
    {
      state = group_state_new(relay_url, group_id, alias, saved->last_opened);
      if (state == NULL)
        {
          g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                      "Failed to create NIP-29 group state for %s", key);
          return FALSE;
        }
      g_hash_table_replace(self->groups, g_strdup(state->key), state);
    }
  else
    {
      g_free(state->alias);
      state->alias = g_strdup(alias);
      state->last_opened = saved->last_opened;
    }

  group_state_refresh(state, self);
  g_signal_emit(self, signals[GROUPS_CHANGED], 0);
  return TRUE;
}

static void
on_action_published(GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  (void)source;
  GTask *task = G_TASK(user_data);
  GnNip29GroupService *self = GN_NIP29_GROUP_SERVICE(g_task_get_source_object(task));
  ActionData *data = g_task_get_task_data(task);

  g_autoptr(GError) error = NULL;
  gboolean ok = gnostr_plugin_context_publish_event_finish(self->context,
                                                           result,
                                                           &error);
  if (!ok)
    {
      if (error != NULL)
        g_task_return_error(task, g_steal_pointer(&error));
      else
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to publish NIP-29 action");
      g_object_unref(task);
      return;
    }

  if (data->kind == ACTION_CREATE_GROUP)
    {
      const char *alias = (data->name != NULL && data->name[0] != '\0')
                            ? data->name
                            : NULL;
      if (!gn_nip29_group_service_track_group(self,
                                              data->relay_url,
                                              data->group_id,
                                              alias,
                                              &error))
        {
          if (error != NULL)
            g_task_return_error(task, g_steal_pointer(&error));
          else
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "Group was created but could not be saved locally");
          g_object_unref(task);
          return;
        }
    }
  else if (data->group_key != NULL)
    {
      GroupState *state = g_hash_table_lookup(self->groups, data->group_key);
      if (state != NULL)
        group_state_refresh(state, self);
    }

  g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}

static void
on_action_signed(GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  (void)source;
  GTask *task = G_TASK(user_data);
  GnNip29GroupService *self = GN_NIP29_GROUP_SERVICE(g_task_get_source_object(task));
  ActionData *data = g_task_get_task_data(task);

  g_autoptr(GError) error = NULL;
  g_autofree gchar *signed_json =
    gnostr_plugin_context_request_sign_event_finish(self->context, result, &error);
  if (signed_json == NULL)
    {
      if (error != NULL)
        g_task_return_error(task, g_steal_pointer(&error));
      else
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to sign NIP-29 action");
      g_object_unref(task);
      return;
    }

  const char *relay_urls[] = { data->relay_url, NULL };
  gnostr_plugin_context_publish_event_to_relays_async(
    self->context,
    signed_json,
    relay_urls,
    g_task_get_cancellable(task),
    on_action_published,
    task);
}

static void
start_signed_publish_action(GnNip29GroupService *self,
                            ActionData          *data,
                            GroupState          *state,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_task_data(task, data, (GDestroyNotify)action_data_free);

  if (self->context == NULL || self->shutting_down)
    {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CLOSED,
                              "NIP-29 group service is not available");
      g_object_unref(task);
      return;
    }

  g_autoptr(GError) error = NULL;
  g_autofree gchar *unsigned_json = build_action_event_json(self, data, state, &error);
  if (unsigned_json == NULL)
    {
      g_task_return_error(task, g_steal_pointer(&error));
      g_object_unref(task);
      return;
    }

  gnostr_plugin_context_request_sign_event(self->context,
                                           unsigned_json,
                                           cancellable,
                                           on_action_signed,
                                           task);
}

void
gn_nip29_group_service_create_group_async(GnNip29GroupService *self,
                                          const char          *relay_url,
                                          const char          *group_id,
                                          const char          *name,
                                          const char          *about,
                                          const char          *picture,
                                          gboolean             is_private,
                                          gboolean             is_restricted,
                                          gboolean             is_hidden,
                                          gboolean             is_closed,
                                          GCancellable        *cancellable,
                                          GAsyncReadyCallback  callback,
                                          gpointer             user_data)
{
  g_return_if_fail(GN_IS_NIP29_GROUP_SERVICE(self));

  ActionData *data = g_new0(ActionData, 1);
  data->kind = ACTION_CREATE_GROUP;
  data->relay_url = g_strdup(relay_url);
  data->group_id = g_strdup(group_id);
  data->name = g_strdup(name);
  data->about = g_strdup(about);
  data->picture = g_strdup(picture);
  data->is_private = is_private;
  data->is_restricted = is_restricted;
  data->is_hidden = is_hidden;
  data->is_closed = is_closed;

  g_autoptr(GError) error = NULL;
  if (!validate_group_address(relay_url, group_id, &error))
    {
      GTask *task = g_task_new(self, cancellable, callback, user_data);
      g_task_set_task_data(task, data, (GDestroyNotify)action_data_free);
      g_task_return_error(task, g_steal_pointer(&error));
      g_object_unref(task);
      return;
    }

  start_signed_publish_action(self, data, NULL, cancellable, callback, user_data);
}

gboolean
gn_nip29_group_service_create_group_finish(GnNip29GroupService *self,
                                           GAsyncResult        *result,
                                           GError             **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

static void
start_group_key_action(GnNip29GroupService *self,
                       const char          *group_key,
                       ActionKind           action_kind,
                       const char          *invite_code,
                       const char          *reason,
                       const char          *content,
                       GCancellable        *cancellable,
                       GAsyncReadyCallback  callback,
                       gpointer             user_data)
{
  GTask *error_task = NULL;
  GroupState *state = group_key ? g_hash_table_lookup(self->groups, group_key) : NULL;
  if (state == NULL)
    {
      error_task = g_task_new(self, cancellable, callback, user_data);
      g_task_return_new_error(error_task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              "NIP-29 group is not tracked");
      g_object_unref(error_task);
      return;
    }

  if (action_kind == ACTION_SEND_MESSAGE)
    {
      g_autofree gchar *trimmed = g_strdup(content ? content : "");
      if (g_strstrip(trimmed)[0] == '\0')
        {
          error_task = g_task_new(self, cancellable, callback, user_data);
          g_task_return_new_error(error_task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                  "Message text is required");
          g_object_unref(error_task);
          return;
        }
    }

  ActionData *data = g_new0(ActionData, 1);
  data->kind = action_kind;
  data->relay_url = g_strdup(state->relay_url);
  data->group_id = g_strdup(state->group_id);
  data->group_key = g_strdup(state->key);
  data->invite_code = g_strdup(invite_code);
  data->reason = g_strdup(reason);
  data->content = g_strdup(content);

  start_signed_publish_action(self, data, state, cancellable, callback, user_data);
}

void
gn_nip29_group_service_join_group_async(GnNip29GroupService *self,
                                        const char          *group_key,
                                        const char          *invite_code,
                                        const char          *reason,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data)
{
  g_return_if_fail(GN_IS_NIP29_GROUP_SERVICE(self));
  start_group_key_action(self, group_key, ACTION_JOIN_GROUP,
                         invite_code, reason, NULL,
                         cancellable, callback, user_data);
}

gboolean
gn_nip29_group_service_join_group_finish(GnNip29GroupService *self,
                                         GAsyncResult        *result,
                                         GError             **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void
gn_nip29_group_service_leave_group_async(GnNip29GroupService *self,
                                         const char          *group_key,
                                         const char          *reason,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data)
{
  g_return_if_fail(GN_IS_NIP29_GROUP_SERVICE(self));
  start_group_key_action(self, group_key, ACTION_LEAVE_GROUP,
                         NULL, reason, NULL,
                         cancellable, callback, user_data);
}

gboolean
gn_nip29_group_service_leave_group_finish(GnNip29GroupService *self,
                                          GAsyncResult        *result,
                                          GError             **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void
gn_nip29_group_service_send_message_async(GnNip29GroupService *self,
                                          const char          *group_key,
                                          const char          *content,
                                          GCancellable        *cancellable,
                                          GAsyncReadyCallback  callback,
                                          gpointer             user_data)
{
  g_return_if_fail(GN_IS_NIP29_GROUP_SERVICE(self));
  start_group_key_action(self, group_key, ACTION_SEND_MESSAGE,
                         NULL, NULL, content,
                         cancellable, callback, user_data);
}

gboolean
gn_nip29_group_service_send_message_finish(GnNip29GroupService *self,
                                           GAsyncResult        *result,
                                           GError             **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void
gn_nip29_group_service_refresh_all(GnNip29GroupService *self)
{
  g_return_if_fail(GN_IS_NIP29_GROUP_SERVICE(self));

  if (self->groups == NULL || self->context == NULL || self->shutting_down)
    return;

  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->groups);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    group_state_refresh(value, self);
}

/* ── UI-facing accessors ─────────────────────────────────────────── */

GList *
gn_nip29_group_service_list_group_keys(GnNip29GroupService *self)
{
  g_return_val_if_fail(GN_IS_NIP29_GROUP_SERVICE(self), NULL);
  if (self->groups == NULL)
    return NULL;
  return g_hash_table_get_keys(self->groups);
}

const char *
gn_nip29_group_service_get_group_relay_url(GnNip29GroupService *self,
                                           const char          *group_key)
{
  g_return_val_if_fail(GN_IS_NIP29_GROUP_SERVICE(self), NULL);
  GroupState *state = g_hash_table_lookup(self->groups, group_key);
  return state ? state->relay_url : NULL;
}

const char *
gn_nip29_group_service_get_group_group_id(GnNip29GroupService *self,
                                          const char          *group_key)
{
  g_return_val_if_fail(GN_IS_NIP29_GROUP_SERVICE(self), NULL);
  GroupState *state = g_hash_table_lookup(self->groups, group_key);
  return state ? state->group_id : NULL;
}

const char *
gn_nip29_group_service_get_group_alias(GnNip29GroupService *self,
                                       const char          *group_key)
{
  g_return_val_if_fail(GN_IS_NIP29_GROUP_SERVICE(self), NULL);
  GroupState *state = g_hash_table_lookup(self->groups, group_key);
  return state ? state->alias : NULL;
}

const nostr_group_t *
gn_nip29_group_service_get_group_data(GnNip29GroupService *self,
                                      const char          *group_key)
{
  g_return_val_if_fail(GN_IS_NIP29_GROUP_SERVICE(self), NULL);
  GroupState *state = g_hash_table_lookup(self->groups, group_key);
  return state ? state->group : NULL;
}

guint
gn_nip29_group_service_get_message_count_for_key(GnNip29GroupService *self,
                                                 const char          *group_key)
{
  g_return_val_if_fail(GN_IS_NIP29_GROUP_SERVICE(self), 0);
  GroupState *state = g_hash_table_lookup(self->groups, group_key);
  return (state && state->messages) ? state->messages->len : 0;
}

gboolean
gn_nip29_group_service_get_message_at(GnNip29GroupService *self,
                                      const char          *group_key,
                                      guint                index,
                                      GnNip29MessageRef   *out_ref)
{
  g_return_val_if_fail(GN_IS_NIP29_GROUP_SERVICE(self), FALSE);
  g_return_val_if_fail(out_ref != NULL, FALSE);

  GroupState *state = g_hash_table_lookup(self->groups, group_key);
  if (state == NULL || state->messages == NULL || index >= state->messages->len)
    return FALSE;

  GroupMessage *msg = g_ptr_array_index(state->messages, index);
  out_ref->id = msg->id;
  out_ref->event_json = msg->event_json;
  out_ref->created_at = msg->created_at;
  out_ref->kind = msg->kind;
  return TRUE;
}
