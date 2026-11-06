/* SPDX-License-Identifier: MIT */
#include "grotto-panels.h"

struct _GrottoPanelsItem {
  GObject parent_instance;
  GrottoPanelsMetadata metadata;
  gboolean backup_banner_dismissed;
};
G_DEFINE_FINAL_TYPE(GrottoPanelsItem, grotto_panels_item, G_TYPE_OBJECT)

static void item_finalize(GObject *object) {
  GrottoPanelsMetadata *m = &GROTTO_PANELS_ITEM(object)->metadata;
  g_free((char *)m->id);
  g_free((char *)m->label);
  g_free((char *)m->public_key);
  g_free((char *)m->state);
  g_free((char *)m->reason);
  g_free((char *)m->access_policy);
  g_free((char *)m->principal); g_free((char *)m->verified_as); g_free((char *)m->transport);
  g_free((char *)m->operation); g_free((char *)m->outcome); g_free((char *)m->trust);
  g_free((char *)m->decision); g_free((char *)m->relay);
  g_strfreev((char **)m->relays); g_strfreev((char **)m->methods);
  g_strfreev((char **)m->effective_permissions);
  G_OBJECT_CLASS(grotto_panels_item_parent_class)->finalize(object);
}
static void grotto_panels_item_class_init(GrottoPanelsItemClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = item_finalize;
}
static void grotto_panels_item_init(GrottoPanelsItem *self) { (void)self; }
GrottoPanelsItem *grotto_panels_item_new(const GrottoPanelsMetadata *metadata, GError **error) {
  if (!grotto_panels_metadata_validate(metadata, error)) return NULL;
  GrottoPanelsItem *self = g_object_new(GROTTO_TYPE_PANELS_ITEM, NULL);
  self->metadata = *metadata;
#define COPY(field) self->metadata.field = g_strdup(metadata->field ? metadata->field : "")
  COPY(id); COPY(label); COPY(public_key); COPY(state); COPY(reason); COPY(access_policy);
  COPY(principal); COPY(verified_as); COPY(transport); COPY(operation); COPY(outcome); COPY(trust);
  COPY(decision); COPY(relay);
  self->metadata.relays = (const char *const *)g_strdupv((char **)metadata->relays);
  self->metadata.methods = (const char *const *)g_strdupv((char **)metadata->methods);
  self->metadata.effective_permissions = (const char *const *)g_strdupv((char **)metadata->effective_permissions);
#undef COPY
  return self;
}
GrottoPanelsItem *grotto_panels_item_new_with_backup_banner_state(const GrottoPanelsMetadata *metadata, gboolean dismissed, GError **error) {
  GrottoPanelsItem *self = grotto_panels_item_new(metadata, error);
  if (self) self->backup_banner_dismissed = dismissed;
  return self;
}
gboolean grotto_panels_item_get_backup_banner_dismissed(GrottoPanelsItem *self) {
  g_return_val_if_fail(GROTTO_IS_PANELS_ITEM(self), FALSE);
  return self->backup_banner_dismissed;
}
const GrottoPanelsMetadata *grotto_panels_item_get_metadata(GrottoPanelsItem *self) {
  g_return_val_if_fail(GROTTO_IS_PANELS_ITEM(self), NULL);
  return &self->metadata;
}

void grotto_panels_service_query_async(GrottoPanelsService *self, GrottoPanelsQuery query,
    const char *principal, guint64 before, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
  GrottoPanelsServiceInterface *iface = self ? GROTTO_PANELS_SERVICE_GET_IFACE(self) : NULL;
  g_autoptr(GError) error = NULL;
  if (!grotto_panels_public_text_validate(principal, &error)) {}
  else if ((query != GROTTO_PANELS_QUERY_PERMISSIONS && query != GROTTO_PANELS_QUERY_ACTIVITY) ||
      (query == GROTTO_PANELS_QUERY_PERMISSIONS && (!principal || !*principal || before)) ||
      (query == GROTTO_PANELS_QUERY_ACTIVITY && principal))
    g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid panel query");
  else if (!grotto_panels_service_is_available(self))
    g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED, "Signer unavailable");
  else if (iface->query_async && iface->query_finish) {
    iface->query_async(self, query, principal, before, cancel, callback, data); return;
  } else g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Query unavailable");
  GTask *task = g_task_new(self, cancel, callback, data);
  g_task_set_source_tag(task, grotto_panels_service_query_async);
  g_task_return_error(task, g_steal_pointer(&error)); g_object_unref(task);
}
GListModel *grotto_panels_service_query_finish(GrottoPanelsService *self, GAsyncResult *result, GError **error) {
  if (g_async_result_is_tagged(result, grotto_panels_service_query_async)) return g_task_propagate_pointer(G_TASK(result), error);
  GrottoPanelsServiceInterface *iface = GROTTO_PANELS_SERVICE_GET_IFACE(self);
  GListModel *model = iface->query_finish(self, result, error);
  if (model && g_list_model_get_item_type(model) != GROTTO_TYPE_PANELS_ITEM) {
    g_object_unref(model); g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid panel result"); return NULL;
  }
  return model;
}

GrottoPanelsAccess grotto_panel_command_get_access(GrottoPanelCommand command) {
  if (command == GROTTO_PANEL_PAUSE) return GROTTO_PANELS_ACCESS_SESSION;
  if (command == GROTTO_PANEL_SET_CLIENT_LABEL || command == GROTTO_PANEL_DISMISS_BACKUP_BANNER) return GROTTO_PANELS_ACCESS_MANAGER;
  return GROTTO_PANELS_ACCESS_APPROVER;
}

G_DEFINE_INTERFACE(GrottoPanelsService, grotto_panels_service, G_TYPE_OBJECT)
static void grotto_panels_service_default_init(GrottoPanelsServiceInterface *iface) {
  g_signal_new("capabilities-changed", G_TYPE_FROM_INTERFACE(iface), G_SIGNAL_RUN_LAST,
               0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  g_signal_new("availability-changed", G_TYPE_FROM_INTERFACE(iface), G_SIGNAL_RUN_LAST,
               0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  g_signal_new("profile-updated", G_TYPE_FROM_INTERFACE(iface), G_SIGNAL_RUN_LAST,
               0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}
typedef struct { char *key; GrottoProfileInfo *info; gsize bytes; } DisplayProfile;
typedef struct { GQueue entries; gsize bytes; } DisplayCache;
static void display_profile_free(gpointer data) {
  DisplayProfile *p = data; g_free(p->key); grotto_profile_info_free(p->info); g_free(p);
}
static void display_cache_free(gpointer data) {
  DisplayCache *c = data; g_queue_clear_full(&c->entries, display_profile_free); g_free(c);
}
static DisplayCache *display_cache(GrottoPanelsService *self) {
  DisplayCache *c = g_object_get_data(G_OBJECT(self), "grotto-display-profiles");
  if (!c) { c = g_new0(DisplayCache, 1); g_object_set_data_full(G_OBJECT(self), "grotto-display-profiles", c, display_cache_free); }
  return c;
}
static GList *display_find(DisplayCache *c, const char *key) {
  for (GList *l = c->entries.head; l; l = l->next)
    if (!g_strcmp0(((DisplayProfile *)l->data)->key, key)) return l;
  return NULL;
}
GrottoProfileInfo *grotto_panels_service_dup_profile(GrottoPanelsService *self, const char *key) {
  if (!self || !key) return NULL;
  g_return_val_if_fail(GROTTO_PANELS_IS_SERVICE(self), NULL);
  DisplayCache *c = display_cache(self); GList *l = display_find(c, key);
  if (!l) return NULL;
  DisplayProfile *p = l->data; g_queue_unlink(&c->entries, l); g_queue_push_head_link(&c->entries, l);
  return grotto_profile_info_copy(p->info);
}
static gboolean display_relays_equal(char **a, char **b) {
  if (!a || !a[0]) return !b || !b[0];
  return b && g_strv_equal((const char *const *)a, (const char *const *)b);
}
void grotto_panels_service_set_profile(GrottoPanelsService *self, const char *key, const GrottoProfileInfo *info) {
  g_return_if_fail(GROTTO_PANELS_IS_SERVICE(self));
  if (!key || !*key || !info || !grotto_panels_public_text_validate(key, NULL) || !grotto_profile_info_validate(info, NULL)) return;
  DisplayCache *c = display_cache(self); GList *l = display_find(c, key);
  g_autoptr(GrottoProfileInfo) hydrated = NULL;
  if (l) {
    DisplayProfile *p = l->data;
    if (p->info->created_at > info->created_at || (p->info->published && !info->published)) return;
    /* A cache read can finish after hydration of the same metadata revision.
     * Missing pixels must not erase an already loaded image for that URL. */
    if (p->info->created_at == info->created_at && p->info->published == info->published) {
      for (guint i = 0; i < 2; i++) {
        guint field = i ? GROTTO_PROFILE_BANNER : GROTTO_PROFILE_PICTURE;
        if (!info->images[i] && p->info->images[i] && !g_strcmp0(p->info->fields[field], info->fields[field])) {
          if (!hydrated) hydrated = grotto_profile_info_copy(info);
          g_set_object(&hydrated->images[i], p->info->images[i]);
        }
      }
      if (hydrated) info = hydrated;
    }
    gboolean equal = p->info->published == info->published && p->info->created_at == info->created_at &&
      p->info->bot == info->bot && p->info->tor_blocked == info->tor_blocked && p->info->cached == info->cached &&
      p->info->no_relays == info->no_relays && p->info->nip05_status == info->nip05_status &&
      p->info->bootstrap_used == info->bootstrap_used &&
      display_relays_equal(p->info->discovery_relays, info->discovery_relays) &&
      display_relays_equal(p->info->auth_required_relays, info->auth_required_relays) &&
      display_relays_equal(p->info->bootstrap_relays, info->bootstrap_relays) &&
      display_relays_equal(p->info->publish_relays, info->publish_relays);
    for (guint i = 0; i < GROTTO_PROFILE_N_FIELDS; i++) equal &= !g_strcmp0(p->info->fields[i], info->fields[i]);
    for (guint i = 0; i < 2; i++) equal &= p->info->images[i] == info->images[i];
    if (equal) return;
    c->bytes -= p->bytes; g_queue_delete_link(&c->entries, l); display_profile_free(p);
  }
  DisplayProfile *p = g_new0(DisplayProfile, 1); p->key = g_strdup(key); p->info = grotto_profile_info_copy(info);
  for (guint i = 0; i < 2; i++) if (p->info->images[i]) {
    gsize size = (gsize)gdk_texture_get_width(p->info->images[i]) * gdk_texture_get_height(p->info->images[i]) * 4;
    if (size > 32 * 1024 * 1024 - p->bytes) g_clear_object(&p->info->images[i]); else p->bytes += size;
  }
  c->bytes += p->bytes; g_queue_push_head(&c->entries, p);
  while (c->entries.length > 32 || c->bytes > 32 * 1024 * 1024) {
    DisplayProfile *old = g_queue_pop_tail(&c->entries); c->bytes -= old->bytes; display_profile_free(old);
  }
  g_signal_emit_by_name(self, "profile-updated", key);
}
GListModel *grotto_panels_service_get_model(GrottoPanelsService *self, GrottoPanelsModel model) {
  g_return_val_if_fail(GROTTO_PANELS_IS_SERVICE(self), NULL);
  g_return_val_if_fail(model >= 0 && model < GROTTO_PANELS_N_MODELS, NULL);
  GrottoPanelsServiceInterface *iface = GROTTO_PANELS_SERVICE_GET_IFACE(self);
  GListModel *result = iface->get_model ? iface->get_model(self, model) : NULL;
  /* Reject generic GObject/GVariant wrapper models, not just suspicious fields. */
  if (result && g_list_model_get_item_type(result) != GROTTO_TYPE_PANELS_ITEM) return NULL;
  return result;
}
gboolean grotto_panels_service_is_available(GrottoPanelsService *self) {
  if (!self) return FALSE;
  g_return_val_if_fail(GROTTO_PANELS_IS_SERVICE(self), FALSE);
  GrottoPanelsServiceInterface *iface = GROTTO_PANELS_SERVICE_GET_IFACE(self);
  return iface->is_available && iface->is_available(self);
}
gboolean grotto_panels_service_can(GrottoPanelsService *self, GrottoPanelCommand command) {
  if (command < 0 || command >= GROTTO_PANEL_N_COMMANDS || !grotto_panels_service_is_available(self)) return FALSE;
  GrottoPanelsServiceInterface *iface = GROTTO_PANELS_SERVICE_GET_IFACE(self);
  if (command == GROTTO_PANEL_DISMISS_BACKUP_BANNER)
    return iface->can_dismiss_backup_banner && iface->can_dismiss_backup_banner(self);
  return iface->can && iface->can(self, command);
}
void grotto_panels_service_invoke_async(GrottoPanelsService *self, GrottoPanelCommand command,
  const GrottoPanelCommandArgs *args, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) {
  g_return_if_fail(self == NULL || GROTTO_PANELS_IS_SERVICE(self));
  GrottoPanelsServiceInterface *iface = self ? GROTTO_PANELS_SERVICE_GET_IFACE(self) : NULL;
  g_autoptr(GError) error = NULL;
  const GrottoPanelCommandArgs empty = {0};
  if (!args) args = &empty;
  if (grotto_panel_command_args_validate(command, args, &error)) {
    if (!grotto_panels_service_is_available(self))
      g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED, "Signer unavailable");
    else if (!grotto_panels_service_can(self, command))
      g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Open in Grotto");
    else if (iface->invoke_async && iface->invoke_finish) {
      iface->invoke_async(self, command, args, cancellable, callback, user_data);
      return;
    } else g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unsupported panel command");
  }
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, grotto_panels_service_invoke_async);
  g_task_return_error(task, g_steal_pointer(&error));
  g_object_unref(task);
}
gboolean grotto_panels_service_invoke_finish(GrottoPanelsService *self, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(self == NULL || GROTTO_PANELS_IS_SERVICE(self), FALSE);
  if (g_async_result_is_tagged(result, grotto_panels_service_invoke_async))
    return g_task_propagate_boolean(G_TASK(result), error);
  g_return_val_if_fail(self != NULL, FALSE);
  GrottoPanelsServiceInterface *iface = GROTTO_PANELS_SERVICE_GET_IFACE(self);
  g_return_val_if_fail(iface->invoke_finish != NULL, FALSE);
  return iface->invoke_finish(self, result, error);
}
