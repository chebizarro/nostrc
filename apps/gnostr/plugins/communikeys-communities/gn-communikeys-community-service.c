#include "gn-communikeys-community-service.h"
#include "model/gn-communikeys-community-item.h"

#include <nostr-event.h>
#include <nostr-tag.h>
#include <string.h>
#include <time.h>

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NostrEvent, nostr_event_free)

struct _GnCommunikeysCommunityService {
  GObject parent_instance;
  GnostrPluginContext *context; /* host-owned */
  GListStore *communities;
  guint64 definition_subscription;
  guint64 acl_subscription;
  guint64 content_subscription;
  gboolean shutting_down;
};

enum {
  DEFINITIONS_CHANGED,
  ACL_CHANGED,
  CONTENT_RECEIVED,
  ERROR_REPORTED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

G_DEFINE_TYPE(GnCommunikeysCommunityService,
              gn_communikeys_community_service, G_TYPE_OBJECT)

static void emit_error(GnCommunikeysCommunityService *self, const char *message) {
  g_warning("Communikeys: %s", message);
  g_signal_emit(self, signals[ERROR_REPORTED], 0, message);
}

static gint find_community(GnCommunikeysCommunityService *self,
                           const char *pubkey) {
  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->communities));
  for (guint i = 0; i < n; i++) {
    g_autoptr(GnCommunikeysCommunityItem) item =
      g_list_model_get_item(G_LIST_MODEL(self->communities), i);
    if (g_strcmp0(gn_communikeys_community_item_get_pubkey(item), pubkey) == 0)
      return (gint)i;
  }
  return -1;
}

static void ingest_definition(GnCommunikeysCommunityService *self,
                              const char *event_json) {
  if (self->shutting_down || event_json == NULL) return;
  g_autoptr(NostrEvent) event = nostr_event_new();
  if (event == NULL ||
      !nostr_event_deserialize_compact(event, event_json, NULL)) {
    emit_error(self, "Ignored an unreadable community definition");
    return;
  }

  nostr_communikeys_definition_t definition;
  if (!nostr_communikeys_definition_parse(event, &definition)) {
    emit_error(self, "Ignored a non-Communikeys definition event");
    return;
  }
  if (!definition.valid) {
    g_autofree gchar *message = g_strdup_printf(
      "Ignored invalid community definition: %s",
      nostr_communikeys_status_string(definition.validation_status));
    emit_error(self, message);
    nostr_communikeys_definition_clear(&definition);
    return;
  }

  gint index = find_community(self, definition.pubkey);
  if (index >= 0) {
    g_autoptr(GnCommunikeysCommunityItem) old =
      g_list_model_get_item(G_LIST_MODEL(self->communities), (guint)index);
    if (gn_communikeys_community_item_get_created_at(old) >
        nostr_event_get_created_at(event)) {
      nostr_communikeys_definition_clear(&definition);
      return;
    }
  }

  GnCommunikeysCommunityItem *item = gn_communikeys_community_item_new(
    definition.pubkey,
    definition.relays_len ? definition.relays[0] : NULL,
    definition.description,
    (guint)definition.sections_len,
    nostr_event_get_created_at(event));
  if (index >= 0) {
    g_list_store_remove(self->communities, (guint)index);
    g_list_store_insert(self->communities, (guint)index, item);
  } else {
    g_list_store_append(self->communities, item);
  }
  g_object_unref(item);
  nostr_communikeys_definition_clear(&definition);
  g_signal_emit(self, signals[DEFINITIONS_CHANGED], 0);
}

static void on_definition_event(const char *event_json, gpointer user_data) {
  ingest_definition(GN_COMMUNIKEYS_COMMUNITY_SERVICE(user_data), event_json);
}

static void on_acl_event(const char *event_json, gpointer user_data) {
  GnCommunikeysCommunityService *self =
    GN_COMMUNIKEYS_COMMUNITY_SERVICE(user_data);
  if (!self->shutting_down)
    g_signal_emit(self, signals[ACL_CHANGED], 0, event_json);
}

static void on_content_event(const char *event_json, gpointer user_data) {
  GnCommunikeysCommunityService *self =
    GN_COMMUNIKEYS_COMMUNITY_SERVICE(user_data);
  if (!self->shutting_down)
    g_signal_emit(self, signals[CONTENT_RECEIVED], 0, event_json);
}

void gn_communikeys_community_service_refresh(
    GnCommunikeysCommunityService *self) {
  g_return_if_fail(GN_IS_COMMUNIKEYS_COMMUNITY_SERVICE(self));
  if (self->shutting_down || self->context == NULL) return;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) events = gnostr_plugin_context_query_events(
    self->context, "{\"kinds\":[10222],\"limit\":200}", &error);
  if (events == NULL) {
    if (error) emit_error(self, error->message);
    return;
  }
  for (guint i = 0; i < events->len; i++)
    ingest_definition(self, g_ptr_array_index(events, i));
}

static void subscribe(GnCommunikeysCommunityService *self) {
  self->definition_subscription = gnostr_plugin_context_subscribe_events(
    self->context, "{\"kinds\":[10222]}",
    G_CALLBACK(on_definition_event), self, NULL);
  self->acl_subscription = gnostr_plugin_context_subscribe_events(
    self->context, "{\"kinds\":[30000]}",
    G_CALLBACK(on_acl_event), self, NULL);
  self->content_subscription = gnostr_plugin_context_subscribe_events(
    self->context, "{\"kinds\":[9,11,30222]}",
    G_CALLBACK(on_content_event), self, NULL);
}

void gn_communikeys_community_service_shutdown(
    GnCommunikeysCommunityService *self) {
  g_return_if_fail(GN_IS_COMMUNIKEYS_COMMUNITY_SERVICE(self));
  if (self->shutting_down) return;
  self->shutting_down = TRUE;
  if (self->context) {
    if (self->definition_subscription)
      gnostr_plugin_context_unsubscribe_events(
        self->context, self->definition_subscription);
    if (self->acl_subscription)
      gnostr_plugin_context_unsubscribe_events(
        self->context, self->acl_subscription);
    if (self->content_subscription)
      gnostr_plugin_context_unsubscribe_events(
        self->context, self->content_subscription);
  }
  self->definition_subscription = 0;
  self->acl_subscription = 0;
  self->content_subscription = 0;
}

static void gn_communikeys_community_service_dispose(GObject *object) {
  GnCommunikeysCommunityService *self =
    GN_COMMUNIKEYS_COMMUNITY_SERVICE(object);
  gn_communikeys_community_service_shutdown(self);
  g_clear_object(&self->communities);
  self->context = NULL;
  G_OBJECT_CLASS(gn_communikeys_community_service_parent_class)->dispose(object);
}

static void gn_communikeys_community_service_class_init(
    GnCommunikeysCommunityServiceClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gn_communikeys_community_service_dispose;
  signals[DEFINITIONS_CHANGED] = g_signal_new(
    "definitions-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[ACL_CHANGED] = g_signal_new(
    "acl-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[CONTENT_RECEIVED] = g_signal_new(
    "content-received", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[ERROR_REPORTED] = g_signal_new(
    "error-reported", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void gn_communikeys_community_service_init(
    GnCommunikeysCommunityService *self) {
  self->communities = g_list_store_new(GN_TYPE_COMMUNIKEYS_COMMUNITY_ITEM);
}

GnCommunikeysCommunityService *gn_communikeys_community_service_new(
    GnostrPluginContext *context) {
  g_return_val_if_fail(context != NULL, NULL);
  GnCommunikeysCommunityService *self = g_object_new(
    GN_TYPE_COMMUNIKEYS_COMMUNITY_SERVICE, NULL);
  self->context = context;
  subscribe(self);
  gn_communikeys_community_service_refresh(self);
  return self;
}

GListModel *gn_communikeys_community_service_get_model(
    GnCommunikeysCommunityService *self) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_COMMUNITY_SERVICE(self), NULL);
  return G_LIST_MODEL(self->communities);
}

static void on_publish_done(GObject *source, GAsyncResult *result,
                            gpointer user_data) {
  (void)source;
  GTask *task = G_TASK(user_data);
  GnCommunikeysCommunityService *self = g_task_get_source_object(task);
  g_autoptr(GError) error = NULL;
  if (!gnostr_plugin_context_publish_event_finish(
        self->context, result, &error))
    g_task_return_error(task, g_steal_pointer(&error));
  else
    g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}

static void on_signed(GObject *source, GAsyncResult *result,
                      gpointer user_data) {
  (void)source;
  GTask *task = G_TASK(user_data);
  GnCommunikeysCommunityService *self = g_task_get_source_object(task);
  if (self->shutting_down) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "Communikeys service was deactivated");
    g_object_unref(task);
    return;
  }
  g_autoptr(GError) error = NULL;
  g_autofree gchar *signed_json =
    gnostr_plugin_context_request_sign_event_finish(
      self->context, result, &error);
  if (signed_json == NULL) {
    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
    return;
  }
  gnostr_plugin_context_publish_event_async(
    self->context, signed_json, g_task_get_cancellable(task),
    on_publish_done, task);
}

static void sign_and_publish(GnCommunikeysCommunityService *self,
                             NostrEvent *event, GCancellable *cancellable,
                             GAsyncReadyCallback callback, gpointer user_data) {
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  if (self->shutting_down || self->context == NULL) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CLOSED,
                            "Communikeys service is shut down");
    g_object_unref(task);
    return;
  }
  char *unsigned_json = nostr_event_serialize_compact(event);
  if (unsigned_json == NULL) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Failed to serialize Communikeys event");
    g_object_unref(task);
    return;
  }
  gnostr_plugin_context_request_sign_event(
    self->context, unsigned_json, cancellable, on_signed, task);
  free(unsigned_json);
}

void gn_communikeys_community_service_publish_exclusive_async(
    GnCommunikeysCommunityService *self, const char *community_pubkey,
    int kind, const char *content, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  g_return_if_fail(GN_IS_COMMUNIKEYS_COMMUNITY_SERVICE(self));
  const char *author = self->context
    ? gnostr_plugin_context_get_user_pubkey(self->context) : NULL;
  g_autoptr(NostrEvent) event = nostr_event_new();
  if (event) {
    nostr_event_set_kind(event, kind);
    nostr_event_set_pubkey(event, author);
    nostr_event_set_created_at(event, (gint64)time(NULL));
    nostr_event_set_content(event, content ? content : "");
  }
  if (!event || !nostr_communikeys_exclusive_add_h(event, community_pubkey)) {
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Invalid kind-9/11 Communikeys publication");
    g_object_unref(task);
    return;
  }
  sign_and_publish(self, event, cancellable, callback, user_data);
}

gboolean gn_communikeys_community_service_publish_exclusive_finish(
    GnCommunikeysCommunityService *self, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void gn_communikeys_community_service_publish_target_async(
    GnCommunikeysCommunityService *self,
    const nostr_communikeys_targeted_publication_t *publication,
    GCancellable *cancellable, GAsyncReadyCallback callback,
    gpointer user_data) {
  g_return_if_fail(GN_IS_COMMUNIKEYS_COMMUNITY_SERVICE(self));
  g_autoptr(NostrEvent) event =
    nostr_communikeys_targeted_publication_to_event(
      publication, (gint64)time(NULL));
  if (!event) {
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Invalid kind-30222 targeting record");
    g_object_unref(task);
    return;
  }
  sign_and_publish(self, event, cancellable, callback, user_data);
}

gboolean gn_communikeys_community_service_publish_target_finish(
    GnCommunikeysCommunityService *self, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}
