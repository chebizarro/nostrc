/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-group-service.c - NIP-29 group service/control plane
 */

#include "gn-nip29-group-service.h"
#include "gn-nip29-events.h"
#include "gn-nip29-migration.h"

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
  /* nostrc-7n4t: the group's admins (kind:39001) as last seen, so their
   * kind:10009 can be read while the group relay is down. */
  GPtrArray *admins;               /* (element-type utf8) 64-hex, nullable */
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
  /* NIP-29 "?invite=<code>" from the reference the group was tracked with;
   * sent as the kind:9021 `code` tag. Memory only: never persisted. */
  gchar        *invite_code;

  nostr_group_t *group;

  GPtrArray    *messages;          /* GroupMessage* */
  GHashTable   *seen_message_ids;  /* id string set */

  guint64       snapshot_subscription_id;
  guint64       message_subscription_id;
  guint64       refresh_generation;

  /* nostrc-7n4t: migration / fork detection. */
  gboolean      relay_unreachable;       /* TCP probe of the relay failed */
  gboolean      relay_probing;
  gboolean      relocation_checking;
  gboolean      relocation_checked;
  gint64        relocation_checked_at;   /* monotonic µs */
  gchar        *relocated_relay;         /* another relay trusted lists name */
  guint         relocated_authors;
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
  gchar     *banner;
  gchar     *parent;
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
G_DEFINE_QUARK(gn-nip29-group-service-error-quark, gn_nip29_group_service_error)

static void group_state_refresh(GroupState *state, GnNip29GroupService *self);
static gchar *make_group_key(const char *relay_url, const char *group_id);
static void cache_group_admins(GnNip29GroupService *self, GroupState *state);
static void maybe_check_relocation(GnNip29GroupService *self, GroupState *state,
                                   gboolean must);
static void probe_group_relay(GnNip29GroupService *self, GroupState *state);

static void
saved_group_free(SavedGroup *saved)
{
  if (saved == NULL)
    return;
  g_free(saved->relay_url);
  g_free(saved->group_id);
  g_free(saved->alias);
  g_clear_pointer(&saved->admins, g_ptr_array_unref);
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
  g_free(state->invite_code);
  g_free(state->relocated_relay);
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
  g_free(data->banner);
  g_free(data->parent);
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

  JsonArray *admins = json_object_has_member(object, "admins")
                        ? json_object_get_array_member(object, "admins") : NULL;
  for (guint i = 0; admins != NULL && i < json_array_get_length(admins); i++)
    {
      JsonNode *node = json_array_get_element(admins, i);
      const char *pk = JSON_NODE_HOLDS_VALUE(node) ? json_node_get_string(node) : NULL;
      if (pk == NULL || strlen(pk) != 64)
        continue;
      if (saved->admins == NULL)
        saved->admins = g_ptr_array_new_with_free_func(g_free);
      g_ptr_array_add(saved->admins, g_ascii_strdown(pk, -1));
    }
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
          if (saved->admins != NULL && saved->admins->len > 0)
            {
              json_builder_set_member_name(builder, "admins");
              json_builder_begin_array(builder);
              for (guint a = 0; a < saved->admins->len; a++)
                json_builder_add_string_value(builder, g_ptr_array_index(saved->admins, a));
              json_builder_end_array(builder);
            }

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

  append_previous_tag(builder, state, self->current_pubkey);
  if (data->kind == ACTION_JOIN_GROUP &&
      data->invite_code != NULL && data->invite_code[0] != '\0')
    json_add_tag2(builder, "code", data->invite_code);

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
    case NOSTR_KIND_SIMPLE_GROUP_PINNED_EVENTS:
      return nostr_group_merge_in_pins_event(state->group, event);
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

  gboolean admins = nostr_event_get_kind(event) == NOSTR_KIND_SIMPLE_GROUP_ADMINS;
  if (merge_snapshot_event(state, event))
    {
      if (admins)
        {
          /* nostrc-7n4t: keep the admins for when the relay is down, and
           * look at their kind:10009 now and then (SHOULD). */
          cache_group_admins(self, state);
          maybe_check_relocation(self, state, FALSE);
        }
      g_signal_emit(self, signals[GROUP_UPDATED], 0, state->key);
    }

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
  GroupState *state = g_hash_table_lookup(self->groups, data->group_key);
  gboolean current = state != NULL && state->refresh_generation == data->generation;
  if (error != NULL)
    {
      if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          emit_error(self, error->message);
          /* nostrc-7n4t: is the group relay reachable at all? */
          if (current && data->snapshots)
            probe_group_relay(self, state);
        }
      query_data_free(data);
      return;
    }

  if (!current)
    {
      if (events != NULL)
        g_ptr_array_unref(events);
      query_data_free(data);
      return;
    }
  if (data->snapshots)
    {
      if (events == NULL || events->len == 0)
        {
          /* nostrc-7n4t: nothing about the group - down, or gone? */
          probe_group_relay(self, state);
        }
      else if (state->relay_unreachable)
        {
          state->relay_unreachable = FALSE;
          g_signal_emit(self, signals[GROUP_UPDATED], 0, state->key);
        }
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
    NOSTR_KIND_SIMPLE_GROUP_PINNED_EVENTS,
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

/* ---- nostrc-7n4t: migration / fork detection ----
 *
 * NIP-29: clients SHOULD periodically - and MUST when the group relay is
 * offline or unreachable - read the kind:10009 of the group's admins (and
 * of trusted friends); an entry for the group on another relay means it
 * may have moved or been forked. The admins are cached per saved group
 * (saved-groups.json "admins") so this works while the relay is down.
 * Trusted authors here: the cached and current admins, and the user (a
 * list the user changed on another device counts too). The lists are read
 * from the user's relays; gn_nip29_find_relocations() verifies them. */

#define RELOCATION_CHECK_INTERVAL_US (6 * G_TIME_SPAN_HOUR)
#define RELOCATION_LIST_LIMIT 200

typedef struct
{
  GnNip29GroupService *service;
  gchar               *group_key;
  gchar              **authors;
  GCancellable        *cancellable;
} RelocationCheck;

static void
relocation_check_free(RelocationCheck *check)
{
  if (check == NULL)
    return;
  if (check->cancellable != NULL && check->service != NULL &&
      check->service->cancellables != NULL)
    g_ptr_array_remove(check->service->cancellables, check->cancellable);
  g_clear_object(&check->cancellable);
  g_clear_object(&check->service);
  g_free(check->group_key);
  g_strfreev(check->authors);
  g_free(check);
}

static SavedGroup *
saved_group_for_state(GnNip29GroupService *self,
                      GroupState          *state)
{
  GPtrArray *bucket = g_hash_table_lookup(self->saved_accounts, current_account_key(self));
  return saved_bucket_find(bucket, state->relay_url, state->group_id);
}

static gboolean
strv_ptr_array_contains(GPtrArray *array, const char *value)
{
  for (guint i = 0; array != NULL && i < array->len; i++)
    if (g_ascii_strcasecmp(g_ptr_array_index(array, i), value) == 0)
      return TRUE;
  return FALSE;
}

/* Remember the group's current admins (kind:39001) for when its relay is
 * unreachable. Saved only when the set changed. */
static void
cache_group_admins(GnNip29GroupService *self,
                   GroupState          *state)
{
  SavedGroup *saved = saved_group_for_state(self, state);
  if (saved == NULL || state->group == NULL || !state->group->admins_loaded)
    return;

  GPtrArray *admins = g_ptr_array_new_with_free_func(g_free);
  for (size_t i = 0; i < state->group->admins_len; i++)
    {
      const char *pk = state->group->admins[i].pubkey;
      if (pk != NULL && strlen(pk) == 64 && !strv_ptr_array_contains(admins, pk))
        g_ptr_array_add(admins, g_ascii_strdown(pk, -1));
    }

  gboolean same = saved->admins != NULL && saved->admins->len == admins->len;
  for (guint i = 0; same && i < admins->len; i++)
    same = strv_ptr_array_contains(saved->admins, g_ptr_array_index(admins, i));
  if (same)
    {
      g_ptr_array_unref(admins);
      return;
    }
  g_clear_pointer(&saved->admins, g_ptr_array_unref);
  saved->admins = admins;
  g_autoptr(GError) error = NULL;
  if (!save_saved_groups(self, &error))
    g_debug("NIP-29: could not cache the admins of %s: %s", state->key,
            error ? error->message : "unknown error");
}

static gchar *
build_lists_filter_json(char **authors)
{
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "kinds");
  json_builder_begin_array(builder);
  json_builder_add_int_value(builder, NOSTR_KIND_SIMPLE_GROUP_LIST);
  json_builder_end_array(builder);
  json_builder_set_member_name(builder, "authors");
  json_builder_begin_array(builder);
  for (gsize i = 0; authors[i] != NULL; i++)
    json_builder_add_string_value(builder, authors[i]);
  json_builder_end_array(builder);
  json_builder_set_member_name(builder, "limit");
  json_builder_add_int_value(builder, RELOCATION_LIST_LIMIT);
  json_builder_end_object(builder);

  JsonNode *root = json_builder_get_root(builder);
  g_autoptr(JsonGenerator) generator = json_generator_new();
  json_generator_set_root(generator, root);
  gchar *json = json_generator_to_data(generator, NULL);
  json_node_unref(root);
  return json;
}

static void
relocation_check_done(GnNip29GroupService *self,
                      GroupState          *state,
                      GPtrArray           *relocations)
{
  state->relocation_checking = FALSE;
  state->relocation_checked = TRUE;
  state->relocation_checked_at = g_get_monotonic_time();
  g_clear_pointer(&state->relocated_relay, g_free);
  state->relocated_authors = 0;
  if (relocations != NULL && relocations->len > 0)
    {
      GnNip29Relocation *best = g_ptr_array_index(relocations, 0);
      state->relocated_relay = g_strdup(best->relay_url);
      state->relocated_authors = best->pubkeys->len;
      g_message("NIP-29: %s is listed on %s by %u trusted author%s (moved or forked?)",
                state->key, best->relay_url, best->pubkeys->len,
                best->pubkeys->len == 1 ? "" : "s");
    }
  g_signal_emit(self, signals[GROUP_UPDATED], 0, state->key);
}

static void
on_relocation_query_done(GObject      *source,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  (void)source;
  RelocationCheck *check = user_data;
  GnNip29GroupService *self = check->service;

  g_autoptr(GError) error = NULL;
  GPtrArray *events = self->context != NULL
    ? gnostr_plugin_context_query_relays_finish(self->context, result, &error)
    : NULL;
  GroupState *state = g_hash_table_lookup(self->groups, check->group_key);
  if (state == NULL || self->shutting_down ||
      g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      if (state != NULL)
        state->relocation_checking = FALSE;
      if (events != NULL)
        g_ptr_array_unref(events);
      relocation_check_free(check);
      return;
    }
  if (error != NULL)
    g_debug("NIP-29: reading kind:10009 lists for %s failed: %s", state->key, error->message);

  GPtrArray *jsons = g_ptr_array_new();
  for (guint i = 0; events != NULL && i < events->len; i++)
    {
      GnostrPluginRelayEvent *ev = g_ptr_array_index(events, i);
      if (ev != NULL && ev->event_json != NULL)
        g_ptr_array_add(jsons, ev->event_json);
    }
  g_autoptr(GPtrArray) relocations =
    gn_nip29_find_relocations(state->group_id, state->relay_url,
                              (const char * const *)check->authors, jsons);
  g_ptr_array_unref(jsons);
  if (events != NULL)
    g_ptr_array_unref(events);

  relocation_check_done(self, state, relocations);
  relocation_check_free(check);
}

/* @must: the group relay is unreachable (checked now, whatever the
 * interval); otherwise at most once per RELOCATION_CHECK_INTERVAL_US. */
static void
maybe_check_relocation(GnNip29GroupService *self,
                       GroupState          *state,
                       gboolean             must)
{
  if (self->context == NULL || self->shutting_down || state->relocation_checking)
    return;
  if (!must && state->relocation_checked &&
      g_get_monotonic_time() - state->relocation_checked_at < RELOCATION_CHECK_INTERVAL_US)
    return;

  GPtrArray *authors = g_ptr_array_new_with_free_func(g_free);
  SavedGroup *saved = saved_group_for_state(self, state);
  for (guint i = 0; saved != NULL && saved->admins != NULL && i < saved->admins->len; i++)
    if (!strv_ptr_array_contains(authors, g_ptr_array_index(saved->admins, i)))
      g_ptr_array_add(authors, g_strdup(g_ptr_array_index(saved->admins, i)));
  for (size_t i = 0; state->group != NULL && i < state->group->admins_len; i++)
    {
      const char *pk = state->group->admins[i].pubkey;
      if (pk != NULL && strlen(pk) == 64 && !strv_ptr_array_contains(authors, pk))
        g_ptr_array_add(authors, g_ascii_strdown(pk, -1));
    }
  if (self->current_pubkey != NULL && strlen(self->current_pubkey) == 64 &&
      !strv_ptr_array_contains(authors, self->current_pubkey))
    g_ptr_array_add(authors, g_ascii_strdown(self->current_pubkey, -1));

  gsize n_relays = 0;
  g_auto(GStrv) relays = gnostr_plugin_context_get_relay_urls(self->context, &n_relays);
  if (authors->len == 0 || relays == NULL || n_relays == 0)
    {
      /* Nobody to ask, or nowhere to ask: report "checked, nothing found". */
      g_ptr_array_unref(authors);
      relocation_check_done(self, state, NULL);
      return;
    }
  g_ptr_array_add(authors, NULL);

  RelocationCheck *check = g_new0(RelocationCheck, 1);
  check->service = g_object_ref(self);
  check->group_key = g_strdup(state->key);
  check->authors = (gchar **)g_ptr_array_free(authors, FALSE);

  g_autofree gchar *filter_json = build_lists_filter_json(check->authors);
  GnostrPluginRelayQuery query = {
    .relay_urls = (const char * const *)relays,
    .n_relay_urls = n_relays,
    .filter_json = filter_json,
  };
  state->relocation_checking = TRUE;
  check->cancellable = g_cancellable_new();
  g_ptr_array_add(self->cancellables, g_object_ref(check->cancellable));
  gnostr_plugin_context_query_relays_async(self->context, &query, check->cancellable,
                                           on_relocation_query_done, check);
}

/* The relay query API reports a relay that cannot be reached as "no
 * events" (no error), so an empty or failed snapshot query is followed by a
 * plain TCP connect to the relay's host: a failure there is "offline or
 * unreachable" (NIP-29 MUST look for the group elsewhere); a relay that is
 * up but says nothing about the group gets the periodic check (SHOULD). */
#define RELAY_PROBE_TIMEOUT_S 10

typedef struct
{
  GnNip29GroupService *service;
  gchar               *group_key;
  guint64              generation;
  GCancellable        *cancellable;
} RelayProbe;

static void
relay_probe_free(RelayProbe *probe)
{
  if (probe->cancellable != NULL && probe->service->cancellables != NULL)
    g_ptr_array_remove(probe->service->cancellables, probe->cancellable);
  g_clear_object(&probe->cancellable);
  g_clear_object(&probe->service);
  g_free(probe->group_key);
  g_free(probe);
}

static void
on_relay_probe_done(GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  RelayProbe *probe = user_data;
  GnNip29GroupService *self = probe->service;
  g_autoptr(GError) error = NULL;
  g_autoptr(GSocketConnection) conn =
    g_socket_client_connect_to_uri_finish(G_SOCKET_CLIENT(source), result, &error);
  if (conn != NULL)
    g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);

  GroupState *state = g_hash_table_lookup(self->groups, probe->group_key);
  if (state != NULL && !self->shutting_down &&
      !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
      state->refresh_generation == probe->generation)
    {
      state->relay_probing = FALSE;
      gboolean unreachable = conn == NULL;
      if (unreachable)
        g_message("NIP-29: relay of %s unreachable: %s", state->key,
                  error ? error->message : "no connection");
      if (unreachable != state->relay_unreachable)
        {
          state->relay_unreachable = unreachable;
          g_signal_emit(self, signals[GROUP_UPDATED], 0, state->key);
        }
      maybe_check_relocation(self, state, unreachable);
    }
  else if (state != NULL)
    state->relay_probing = FALSE;
  relay_probe_free(probe);
}

static void
probe_group_relay(GnNip29GroupService *self,
                  GroupState          *state)
{
  if (self->shutting_down || state->relay_probing)
    return;
  guint16 port = g_str_has_prefix(state->relay_url, "wss://") ? 443 : 80;
  g_autoptr(GSocketClient) client = g_socket_client_new();
  g_socket_client_set_timeout(client, RELAY_PROBE_TIMEOUT_S);

  RelayProbe *probe = g_new0(RelayProbe, 1);
  probe->service = g_object_ref(self);
  probe->group_key = g_strdup(state->key);
  probe->generation = state->refresh_generation;
  probe->cancellable = g_cancellable_new();
  g_ptr_array_add(self->cancellables, g_object_ref(probe->cancellable));
  state->relay_probing = TRUE;
  g_socket_client_connect_to_uri_async(client, state->relay_url, port, probe->cancellable,
                                       on_relay_probe_done, probe);
}

GnNip29RelocationState
gn_nip29_group_service_get_relocation(GnNip29GroupService *self,
                                      const char          *group_key,
                                      const char         **out_relay_url,
                                      guint               *out_n_authors)
{
  g_return_val_if_fail(GN_IS_NIP29_GROUP_SERVICE(self), GN_NIP29_RELOCATION_NONE);
  if (out_relay_url != NULL)
    *out_relay_url = NULL;
  if (out_n_authors != NULL)
    *out_n_authors = 0;
  GroupState *state = group_key ? g_hash_table_lookup(self->groups, group_key) : NULL;
  if (state == NULL)
    return GN_NIP29_RELOCATION_NONE;
  if (state->relocated_relay != NULL)
    {
      if (out_relay_url != NULL)
        *out_relay_url = state->relocated_relay;
      if (out_n_authors != NULL)
        *out_n_authors = state->relocated_authors;
      return GN_NIP29_RELOCATION_FOUND;
    }
  if (state->relay_unreachable)
    return state->relocation_checking ? GN_NIP29_RELOCATION_CHECKING
                                      : GN_NIP29_RELOCATION_UNREACHABLE;
  return GN_NIP29_RELOCATION_NONE;
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

gboolean
gn_nip29_group_service_track_group_reference(GnNip29GroupService *self,
                                             const char          *reference,
                                             const char          *alias,
                                             GError             **error)
{
  g_return_val_if_fail(GN_IS_NIP29_GROUP_SERVICE(self), FALSE);

  g_autofree gchar *trimmed = g_strstrip(g_strdup(reference ? reference : ""));
  nostr_group_address_t address = {0};
  char *invite_code = NULL;
  if (!nostr_group_reference_parse(trimmed, &address, &invite_code))
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                  "Not a NIP-29 group reference (expected naddr1… of a kind 39000 "
                  "with a relay hint, or relay'group-id): %s", trimmed);
      return FALSE;
    }

  gboolean ok = gn_nip29_group_service_track_group(self, address.relay, address.id,
                                                   alias, error);
  if (ok && invite_code != NULL)
    {
      g_autofree gchar *key = make_group_key(address.relay, address.id);
      GroupState *state = g_hash_table_lookup(self->groups, key);
      if (state != NULL)
        {
          g_free(state->invite_code);
          state->invite_code = g_strdup(invite_code);
        }
    }
  nostr_group_address_clear(&address);
  free(invite_code);
  return ok;
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

  if (data->group_key != NULL)
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

/* ---- nostrc-4gf4: create-group (kind:9007), then edit-metadata (9002) ----
 *
 * NIP-29's moderation table gives kind:9007 no tags besides `h`; name,
 * about, picture, banner, the access flags and a subgroup's parent are
 * group-metadata, set with kind:9002. A 9002 for a group the relay has not
 * created yet is refused, so it is only signed and sent once the relay has
 * answered OK to the 9007 (gnostr_plugin_context_publish_event_to_relay_ack). */

static GnNip29Metadata
action_metadata(const ActionData *data)
{
  return (GnNip29Metadata){
    .name = data->name, .about = data->about, .picture = data->picture,
    .banner = data->banner, .parent = data->parent,
    .is_private = data->is_private, .is_restricted = data->is_restricted,
    .is_hidden = data->is_hidden, .is_closed = data->is_closed,
  };
}

static void
create_group_fail(GTask *task, GError *error, const char *fallback)
{
  if (error != NULL)
    g_task_return_error(task, error);
  else
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", fallback);
  g_object_unref(task);
}

/* The metadata step failed after the group was created and tracked. */
static void
create_group_metadata_failed(GTask *task, GError *error)
{
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      create_group_fail(task, error, NULL);
      return;
    }
  g_task_return_new_error(task, GN_NIP29_GROUP_SERVICE_ERROR,
                          GN_NIP29_GROUP_SERVICE_ERROR_METADATA_NOT_APPLIED,
                          "The group was created, but its name and settings were not "
                          "applied: %s",
                          error != NULL ? error->message : "unknown error");
  g_clear_error(&error);
  g_object_unref(task);
}

static void
on_create_metadata_acked(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  GTask *task = G_TASK(user_data);
  GnNip29GroupService *self = GN_NIP29_GROUP_SERVICE(g_task_get_source_object(task));
  ActionData *data = g_task_get_task_data(task);
  GError *error = NULL;
  if (!gnostr_plugin_context_publish_event_to_relay_ack_finish(self->context, result, &error))
    {
      create_group_metadata_failed(task, error);
      return;
    }
  GroupState *state = g_hash_table_lookup(self->groups, data->group_key);
  if (state != NULL)
    group_state_refresh(state, self);
  g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}

static void
on_create_metadata_signed(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  GTask *task = G_TASK(user_data);
  GnNip29GroupService *self = GN_NIP29_GROUP_SERVICE(g_task_get_source_object(task));
  ActionData *data = g_task_get_task_data(task);
  GError *error = NULL;
  g_autofree gchar *signed_json =
    gnostr_plugin_context_request_sign_event_finish(self->context, result, &error);
  if (signed_json == NULL)
    {
      create_group_metadata_failed(task, error);
      return;
    }
  gnostr_plugin_context_publish_event_to_relay_ack_async(self->context, signed_json,
                                                         data->relay_url,
                                                         g_task_get_cancellable(task),
                                                         on_create_metadata_acked, task);
}

static void
on_create_group_acked(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  GTask *task = G_TASK(user_data);
  GnNip29GroupService *self = GN_NIP29_GROUP_SERVICE(g_task_get_source_object(task));
  ActionData *data = g_task_get_task_data(task);
  GError *error = NULL;
  if (!gnostr_plugin_context_publish_event_to_relay_ack_finish(self->context, result, &error))
    {
      create_group_fail(task, error, "The relay did not create the group");
      return;
    }

  /* The group exists now: keep it even if the metadata step fails. */
  const char *alias = (data->name != NULL && data->name[0] != '\0') ? data->name : NULL;
  if (!gn_nip29_group_service_track_group(self, data->relay_url, data->group_id, alias, &error))
    {
      create_group_fail(task, error, "Group was created but could not be saved locally");
      return;
    }
  data->group_key = make_group_key(data->relay_url, data->group_id);

  GnNip29Metadata md = action_metadata(data);
  if (gn_nip29_metadata_is_empty(&md))
    {
      g_task_return_boolean(task, TRUE);
      g_object_unref(task);
      return;
    }
  g_autofree gchar *unsigned_json =
    gn_nip29_build_edit_metadata_json(data->group_id, &md,
                                      (gint64)(g_get_real_time() / G_USEC_PER_SEC));
  gnostr_plugin_context_request_sign_event(self->context, unsigned_json,
                                           g_task_get_cancellable(task),
                                           on_create_metadata_signed, task);
}

static void
on_create_group_signed(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  GTask *task = G_TASK(user_data);
  GnNip29GroupService *self = GN_NIP29_GROUP_SERVICE(g_task_get_source_object(task));
  ActionData *data = g_task_get_task_data(task);
  GError *error = NULL;
  g_autofree gchar *signed_json =
    gnostr_plugin_context_request_sign_event_finish(self->context, result, &error);
  if (signed_json == NULL)
    {
      create_group_fail(task, error, "Failed to sign the create-group event");
      return;
    }
  gnostr_plugin_context_publish_event_to_relay_ack_async(self->context, signed_json,
                                                         data->relay_url,
                                                         g_task_get_cancellable(task),
                                                         on_create_group_acked, task);
}

void
gn_nip29_group_service_create_group_async(GnNip29GroupService *self,
                                          const char          *relay_url,
                                          const char          *group_id,
                                          const char          *name,
                                          const char          *about,
                                          const char          *picture,
                                          const char          *banner,
                                          const char          *parent_id,
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
  data->banner = g_strdup(banner);
  data->parent = g_strdup(parent_id);
  data->is_private = is_private;
  data->is_restricted = is_restricted;
  data->is_hidden = is_hidden;
  data->is_closed = is_closed;

  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, gn_nip29_group_service_create_group_async);
  g_task_set_task_data(task, data, (GDestroyNotify)action_data_free);

  g_autoptr(GError) error = NULL;
  if (!validate_group_address(relay_url, group_id, &error))
    {
      g_task_return_error(task, g_steal_pointer(&error));
      g_object_unref(task);
      return;
    }
  if (self->context == NULL || self->shutting_down)
    {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CLOSED,
                              "NIP-29 group service is not available");
      g_object_unref(task);
      return;
    }

  g_autofree gchar *unsigned_json =
    gn_nip29_build_create_group_json(group_id, (gint64)(g_get_real_time() / G_USEC_PER_SEC));
  gnostr_plugin_context_request_sign_event(self->context, unsigned_json, cancellable,
                                           on_create_group_signed, task);
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
  /* An explicit code wins; otherwise use the one from the ?invite= suffix. */
  data->invite_code = g_strdup((invite_code != NULL && invite_code[0] != '\0')
                                 ? invite_code
                                 : (action_kind == ACTION_JOIN_GROUP ? state->invite_code : NULL));
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
