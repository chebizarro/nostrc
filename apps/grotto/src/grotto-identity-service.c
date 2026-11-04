/* SPDX-License-Identifier: MIT */
#include "grotto-identity-service.h"
#include "accounts_store.h"
#include "settings_manager.h"
#include <string.h>

/* This Grotto-only adapter joins Control1 snapshots and local public metadata.
 * It does not enumerate keyring items or put backend dictionaries in a model. */
struct _GrottoIdentityService {
  GObject parent_instance;
  GrottoHomeModel *control;
  GListStore *identities, *empty;
  GSettings *settings;
  guint accounts_changed;
  gulong control_changed, settings_changed;
};
static void iface_init(GrottoPanelsServiceInterface *iface);
G_DEFINE_TYPE_WITH_CODE(GrottoIdentityService, grotto_identity_service, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GROTTO_TYPE_PANELS_SERVICE, iface_init))
static gboolean available(GrottoPanelsService *service) {
  GrottoIdentityService *self = GROTTO_IDENTITY_SERVICE(service);
  return self->control && grotto_home_model_status(self->control) != NULL;
}
static gboolean can(GrottoPanelsService *service, GrottoPanelCommand command) {
  GrottoIdentityService *self = GROTTO_IDENTITY_SERVICE(service);
  return available(service) && grotto_home_model_writable(self->control) &&
    (command == GROTTO_PANEL_SET_IDENTITY_LABEL || command == GROTTO_PANEL_SET_DEFAULT_IDENTITY);
}
static void refresh(GrottoIdentityService *self) {
  AccountsStore *store = accounts_store_get_default();
  g_autoptr(GPtrArray) entries = accounts_store_list(store);
  g_autoptr(GPtrArray) items = g_ptr_array_new_with_free_func(g_object_unref);
  g_autofree char *default_id = g_settings_get_string(self->settings, "default-identity");
  GVariant *clients = grotto_home_model_clients(self->control);
  for (guint i = 0; i < entries->len; i++) {
    AccountEntry *entry = entries->pdata[i];
    guint count = 0;
    for (gsize j = 0; clients && j < g_variant_n_children(clients); j++) {
      g_autoptr(GVariant) row = g_variant_get_child_value(clients, j);
      const char *identity = NULL;
      if (g_variant_lookup(row, "identity", "&s", &identity) && !g_strcmp0(identity, entry->id)) count++;
    }
    GrottoPanelsMetadata m = {.id = entry->id, .public_key = entry->id, .label = entry->label,
      .state = entry->setup_state, .is_default = !g_strcmp0(default_id, entry->id),
      .watch_only = entry->watch_only, .can_sign = entry->has_secret,
      .backup_acknowledged = !g_strcmp0(entry->setup_state, "ready"), .app_count = count};
    /* Invalid stored labels cannot smuggle credential encodings into the DSO.
     * Keep the public identity navigable with a safe empty-label fallback. */
    if (!grotto_panels_public_text_validate(m.label, NULL)) m.label = "";
    GrottoPanelsItem *item = grotto_panels_item_new(&m, NULL);
    if (item) g_ptr_array_add(items, item);
  }
  g_list_store_splice(self->identities, 0, g_list_model_get_n_items(G_LIST_MODEL(self->identities)), items->pdata, items->len);
  g_signal_emit_by_name(self, "availability-changed");
  g_signal_emit_by_name(self, "capabilities-changed");
}
static void accounts_changed(AccountsStore *store, AccountsChangeType change, const char *id, gpointer data) {
  (void)store; (void)change; (void)id; refresh(data);
}
static void control_changed(GrottoHomeModel *control, gpointer data) { (void)control; refresh(data); }
static void settings_changed(GSettings *settings, const char *key, gpointer data) { (void)settings; (void)key; refresh(data); }
static GListModel *get_model(GrottoPanelsService *service, GrottoPanelsModel model) {
  GrottoIdentityService *self = GROTTO_IDENTITY_SERVICE(service);
  return G_LIST_MODEL(model == GROTTO_PANELS_IDENTITIES ? self->identities : self->empty);
}
static void default_done(GObject *source, GAsyncResult *result, gpointer data) {
  GTask *task = data; GrottoIdentityService *self = g_task_get_source_object(task);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  GDBusProxy *proxy = grotto_home_model_proxy(self->control);
  if (!proxy || g_strcmp0(g_task_get_task_data(task), g_dbus_proxy_get_name_owner(proxy)))
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Signer changed");
  else if (error) g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Identity wasn't updated");
  else { refresh(self); g_task_return_boolean(task, TRUE); }
  g_object_unref(task);
}
static void invoke(GrottoPanelsService *service, GrottoPanelCommand command, const GrottoPanelCommandArgs *args,
    GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
  GrottoIdentityService *self = GROTTO_IDENTITY_SERVICE(service);
  GTask *task = g_task_new(self, cancel, callback, data);
  if (g_task_return_error_if_cancelled(task)) { g_object_unref(task); return; }
  AccountEntry *entry = accounts_store_find(accounts_store_get_default(), args->public_key);
  if (!can(service, command) || !entry || g_strcmp0(entry->id, args->public_key) ||
      (command == GROTTO_PANEL_SET_DEFAULT_IDENTITY && (!entry->has_secret || entry->watch_only))) {
    accounts_store_entry_free(entry);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Identity action unavailable");
    g_object_unref(task); return;
  }
  accounts_store_entry_free(entry);
  if (command == GROTTO_PANEL_SET_IDENTITY_LABEL) {
    if (accounts_store_set_label(accounts_store_get_default(), args->public_key, args->label, NULL)) g_task_return_boolean(task, TRUE);
    else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Label wasn't saved");
    g_object_unref(task); return;
  }
  GDBusProxy *proxy = grotto_home_model_proxy(self->control);
  const char *owner = g_dbus_proxy_get_name_owner(proxy);
  g_task_set_task_data(task, g_strdup(owner), g_free);
  g_dbus_connection_call(g_dbus_proxy_get_connection(proxy), owner,
    "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "SetDefaultIdentityForLegacy",
    g_variant_new("(s)", args->public_key), G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NO_AUTO_START,
    10000, cancel, default_done, task);
}
static gboolean finish(GrottoPanelsService *service, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, service), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}
static void iface_init(GrottoPanelsServiceInterface *iface) {
  iface->get_model = get_model; iface->is_available = available; iface->can = can;
  iface->invoke_async = invoke; iface->invoke_finish = finish;
}
static void dispose(GObject *object) {
  GrottoIdentityService *self = GROTTO_IDENTITY_SERVICE(object);
  if (self->accounts_changed) accounts_store_disconnect_changed(accounts_store_get_default(), self->accounts_changed);
  self->accounts_changed = 0;
  if (self->control) g_signal_handler_disconnect(self->control, self->control_changed);
  if (self->settings) g_signal_handler_disconnect(self->settings, self->settings_changed);
  g_clear_object(&self->control); g_clear_object(&self->settings);
  g_clear_object(&self->identities); g_clear_object(&self->empty);
  G_OBJECT_CLASS(grotto_identity_service_parent_class)->dispose(object);
}
static void grotto_identity_service_class_init(GrottoIdentityServiceClass *klass) { G_OBJECT_CLASS(klass)->dispose = dispose; }
static void grotto_identity_service_init(GrottoIdentityService *self) {
  self->identities = g_list_store_new(GROTTO_TYPE_PANELS_ITEM); self->empty = g_list_store_new(GROTTO_TYPE_PANELS_ITEM);
}
GrottoPanelsService *grotto_identity_service_new(GrottoHomeModel *control) {
  GrottoIdentityService *self = g_object_new(GROTTO_TYPE_IDENTITY_SERVICE, NULL);
  self->control = g_object_ref(control);
  self->settings = g_object_ref(settings_manager_get_gsettings(settings_manager_get_default()));
  self->control_changed = g_signal_connect(control, "changed", G_CALLBACK(control_changed), self);
  self->settings_changed = g_signal_connect(self->settings, "changed::default-identity", G_CALLBACK(settings_changed), self);
  self->accounts_changed = accounts_store_connect_changed(accounts_store_get_default(), accounts_changed, self);
  refresh(self);
  return GROTTO_PANELS_SERVICE(self);
}
