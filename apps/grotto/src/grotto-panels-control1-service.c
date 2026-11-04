/* SPDX-License-Identifier: MIT */
#include "grotto-panels-control1-service.h"
#include <string.h>

#define CONTROL "org.nostr.Grotto.Control1"
#define CONTROL_PATH "/org/nostr/signer/grotto"
struct _GrottoPanelsControl1Service {
  GObject parent_instance;
  GDBusProxy *proxy;
  char *owner;
  guint64 generation, serial[GROTTO_PANELS_N_MODELS + 1];
  GListStore *models[GROTTO_PANELS_N_MODELS];
  GHashTable *tombstones;
  GCancellable *cancel;
  gboolean available, caps[GROTTO_PANEL_N_COMMANDS];
  gulong signal_id, owner_id;
};
static void service_iface_init(GrottoPanelsServiceInterface *iface);
G_DEFINE_TYPE_WITH_CODE(GrottoPanelsControl1Service, grotto_panels_control1_service, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GROTTO_TYPE_PANELS_SERVICE, service_iface_init))
/* This closed table is the ONLY RPC dispatch route for panel commands. */
static const char *const methods[] = {
  "SetClientLabel", "SetDefaultIdentityForLegacy", "SetClientTrust", "SetPermissions",
  "RevokeClient", "SetPaused", "SetPaused", "ClearActivity", "ConfigureBunker", "ResetBunkerEndpoint", NULL /* Local metadata, not a daemon RPC. */
};
static const char *const capability_names[] = {
  "set-client-label", "set-default-identity", "set-client-trust", "set-permissions",
  "revoke-client", "pause", "resume", "clear-activity", "configure-remote", "reset-remote", NULL
};
G_STATIC_ASSERT(G_N_ELEMENTS(methods) == GROTTO_PANEL_N_COMMANDS);
G_STATIC_ASSERT(G_N_ELEMENTS(capability_names) == GROTTO_PANEL_N_COMMANDS);
/* Internal slot 4 holds coarse availability/caller capabilities, not a model. */
#define STATUS_SLOT GROTTO_PANELS_N_MODELS

typedef struct {
  GrottoPanelsControl1Service *self;
  guint slot;
  guint64 generation, serial;
  gint64 started;
} Snapshot;
static void refresh(GrottoPanelsControl1Service *self, guint slot);
static const char *text(GVariant *row, const char *key) {
  const char *value = "";
  g_variant_lookup(row, key, "&s", &value);
  return value;
}
static void changed(GrottoPanelsControl1Service *self, const char *signal) {
  g_signal_emit_by_name(self, signal);
}
static void apply_rows(Snapshot *snapshot, GVariant *rows) {
  GrottoPanelsControl1Service *self = snapshot->self;
  GListStore *model = self->models[snapshot->slot];
  g_autoptr(GPtrArray) items = g_ptr_array_new_with_free_func(g_object_unref);
  g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
  for (gsize i = 0; i < g_variant_n_children(rows); i++) {
    g_autoptr(GVariant) row = g_variant_get_child_value(rows, i);
    const char *id = text(row, snapshot->slot == GROTTO_PANELS_REQUESTS ? "request_id" : "principal");
    guint64 sequence = 0;
    g_variant_lookup(row, "sequence", "t", &sequence);
    g_autofree char *sequence_id = g_strdup_printf("%" G_GUINT64_FORMAT, sequence);
    if (snapshot->slot == GROTTO_PANELS_ACTIVITY) id = sequence_id;
    if (!*id || g_hash_table_contains(seen, id)) continue;
    if (snapshot->slot == GROTTO_PANELS_REQUESTS && g_hash_table_contains(self->tombstones, id)) continue;
    GrottoPanelsMetadata m = {
      .id = id, .label = text(row, "label"), .public_key = text(row, "identity"),
      .state = text(row, "state"), .reason = text(row, "reason"),
      .access_policy = text(row, "access-policy"), .sequence = sequence,
      .principal = text(row, "principal"), .verified_as = text(row, "principal"),
      .transport = text(row, "transport"), .operation = text(row, "operation"),
      .outcome = text(row, "outcome"), .trust = text(row, "trust-tier"),
      .owner_generation = self->generation
    };
    g_variant_lookup(row, "first-consented-at", "t", &m.timestamp);
    g_variant_lookup(row, "last-used-at", "t", &m.last_used_at);
    g_variant_lookup(row, "timestamp", "t", &m.timestamp);
    guint32 kind = 0;
    m.has_event_kind = g_variant_lookup(row, "event_kind", "u", &kind);
    m.event_kind = kind;
    g_variant_lookup(row, "needs-review", "b", &m.needs_review);
    g_variant_lookup(row, "blocked", "b", &m.blocked);
    if (snapshot->slot == GROTTO_PANELS_REQUESTS) {
      guint32 remaining = 0;
      g_variant_lookup(row, "remaining_seconds", "u", &remaining);
      m.deadline_usec = snapshot->started + (gint64)remaining * G_USEC_PER_SEC;
      for (guint j = 0; j < g_list_model_get_n_items(G_LIST_MODEL(model)); j++) {
        g_autoptr(GrottoPanelsItem) old = g_list_model_get_item(G_LIST_MODEL(model), j);
        const GrottoPanelsMetadata *previous = grotto_panels_item_get_metadata(old);
        if (!strcmp(previous->id, id)) m.deadline_usec = MIN(m.deadline_usec, previous->deadline_usec);
      }
    }
    /* Reject explicitly credential-bearing rows; unknown public daemon fields
     * are otherwise projected away, never passed as a dictionary to a panel. */
    static const char *const forbidden_fields[] = {"nsec", "mnemonic", "token", "pairing_token", "secret", "passphrase", "private_key"};
    gboolean rejected = FALSE;
    for (guint key = 0; key < G_N_ELEMENTS(forbidden_fields); key++) {
      g_autoptr(GVariant) field = g_variant_lookup_value(row, forbidden_fields[key], NULL);
      rejected |= field != NULL;
    }
    if (rejected) continue;
    GrottoPanelsItem *item = grotto_panels_item_new(&m, NULL);
    if (!item) continue;
    g_ptr_array_add(items, item);
    g_hash_table_add(seen, (gpointer)grotto_panels_item_get_metadata(item)->id);
  }
  if (snapshot->slot == GROTTO_PANELS_REQUESTS) {
    for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(model)); i++) {
      g_autoptr(GrottoPanelsItem) old = g_list_model_get_item(G_LIST_MODEL(model), i);
      const char *id = grotto_panels_item_get_metadata(old)->id;
      if (!g_hash_table_contains(seen, id)) g_hash_table_add(self->tombstones, g_strdup(id));
    }
  }
  g_list_store_splice(model, 0, g_list_model_get_n_items(G_LIST_MODEL(model)), items->pdata, items->len);
}
static void snapshot_done(GObject *source, GAsyncResult *result, gpointer data) {
  Snapshot *snapshot = data;
  GrottoPanelsControl1Service *self = snapshot->self;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (snapshot->generation == self->generation && snapshot->serial == self->serial[snapshot->slot]) {
    if (snapshot->slot == STATUS_SLOT) {
      gboolean available = reply != NULL;
      gboolean caps[GROTTO_PANEL_N_COMMANDS] = {0};
      if (reply) {
        g_autoptr(GVariant) status = g_variant_get_child_value(reply, 0);
        g_auto(GStrv) relays = NULL, methods = NULL;
        g_variant_lookup(status, "bunker_relays", "^as", &relays);
        g_variant_lookup(status, "bunker_allowed_methods", "^as", &methods);
        GrottoPanelsMetadata metadata = {.id = "status", .state = text(status, "bunker_state"),
          .relays = (const char *const *)relays, .methods = (const char *const *)methods,
          .owner_generation = self->generation};
        g_variant_lookup(status, "paused", "b", &metadata.paused);
        g_variant_lookup(status, "bunker_enabled", "b", &metadata.remote_enabled);
        g_autoptr(GrottoPanelsItem) item = grotto_panels_item_new(&metadata, NULL);
        if (item) { gpointer items[] = {item};
          g_list_store_splice(self->models[GROTTO_PANELS_STATUS], 0,
            g_list_model_get_n_items(G_LIST_MODEL(self->models[GROTTO_PANELS_STATUS])), items, 1);
        }
        /* A separate caller-specific map: never use today's coarse capabilities. */
        g_autoptr(GVariant) allowed = g_variant_lookup_value(status, "management-capabilities", G_VARIANT_TYPE("a{sb}"));
        if (allowed) for (guint i = 0; i < GROTTO_PANEL_N_COMMANDS; i++)
          if (capability_names[i]) g_variant_lookup(allowed, capability_names[i], "b", &caps[i]);
      }
      if (!available) g_list_store_remove_all(self->models[GROTTO_PANELS_STATUS]);
      gboolean availability_changed = self->available != available;
      self->available = available;
      memcpy(self->caps, caps, sizeof caps);
      changed(self, "capabilities-changed");
      if (availability_changed) changed(self, "availability-changed");
    } else if (reply) {
      g_autoptr(GVariant) rows = g_variant_get_child_value(reply, 0);
      apply_rows(snapshot, rows);
    } else {
      /* Read authorization/availability failure is not stale successful data. */
      g_list_store_remove_all(self->models[snapshot->slot]);
    }
  }
  g_object_unref(self);
  g_free(snapshot);
}
static void refresh(GrottoPanelsControl1Service *self, guint slot) {
  self->serial[slot]++;
  if (!self->owner) return;
  static const char *const reads[] = {[GROTTO_PANELS_APPS] = "ListClients",
    [GROTTO_PANELS_REQUESTS] = "ListRequests", [GROTTO_PANELS_ACTIVITY] = "ListActivity", [STATUS_SLOT] = "GetStatus"};
  if (!reads[slot]) return; /* Identity metadata is W3/W6, never private inventory. */
  Snapshot *snapshot = g_new0(Snapshot, 1);
  *snapshot = (Snapshot){g_object_ref(self), slot, self->generation, self->serial[slot], g_get_monotonic_time()};
  GVariant *args = slot == GROTTO_PANELS_ACTIVITY ? g_variant_new("(tu)", (guint64)0, 100u) : NULL;
  g_dbus_connection_call(g_dbus_proxy_get_connection(self->proxy), self->owner, CONTROL_PATH, CONTROL,
    reads[slot], args, G_VARIANT_TYPE(slot == STATUS_SLOT ? "(a{sv})" : "(aa{sv})"),
    G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, self->cancel, snapshot_done, snapshot);
}
static void owner_changed(GObject *proxy, GParamSpec *pspec, gpointer data) {
  (void)pspec;
  GrottoPanelsControl1Service *self = data;
  g_autofree char *owner = g_dbus_proxy_get_name_owner(G_DBUS_PROXY(proxy));
  if (!g_strcmp0(owner, self->owner)) return;
  self->generation++;
  g_cancellable_cancel(self->cancel);
  g_clear_object(&self->cancel);
  self->cancel = g_cancellable_new();
  g_free(self->owner);
  self->owner = g_steal_pointer(&owner);
  self->available = FALSE;
  memset(self->caps, 0, sizeof self->caps);
  g_hash_table_remove_all(self->tombstones);
  for (guint i = 0; i < GROTTO_PANELS_N_MODELS; i++) g_list_store_remove_all(self->models[i]);
  changed(self, "capabilities-changed");
  changed(self, "availability-changed");
  for (guint i = GROTTO_PANELS_APPS; i <= STATUS_SLOT; i++) refresh(self, i);
}
static void daemon_signal(GDBusProxy *proxy, const char *sender, const char *name,
                           GVariant *parameters, gpointer data) {
  (void)proxy; (void)parameters;
  GrottoPanelsControl1Service *self = data;
  if (g_strcmp0(sender, self->owner)) return;
  if (!strcmp(name, "ClientsChanged")) refresh(self, GROTTO_PANELS_APPS);
  else if (!strcmp(name, "RequestsChanged")) refresh(self, GROTTO_PANELS_REQUESTS);
  else if (!strcmp(name, "ActivityChanged")) refresh(self, GROTTO_PANELS_ACTIVITY);
  else if (!strcmp(name, "StatusChanged")) refresh(self, STATUS_SLOT);
}
static GListModel *get_model(GrottoPanelsService *service, GrottoPanelsModel model) {
  return G_LIST_MODEL(GROTTO_PANELS_CONTROL1_SERVICE(service)->models[model]);
}
static gboolean is_available(GrottoPanelsService *service) {
  return GROTTO_PANELS_CONTROL1_SERVICE(service)->available;
}
static gboolean can(GrottoPanelsService *service, GrottoPanelCommand command) {
  return GROTTO_PANELS_CONTROL1_SERVICE(service)->caps[command];
}
typedef struct { guint64 generation; } Invocation;
static void invocation_done(GObject *source, GAsyncResult *result, gpointer data) {
  GTask *task = data;
  GrottoPanelsControl1Service *self = g_task_get_source_object(task);
  Invocation *invocation = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (invocation->generation != self->generation)
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Signer owner changed");
  else if (!reply) {
    /* One public error contract for real and fake services; never return remote
     * error text, which is outside the panel's public-metadata schema. */
    GIOErrorEnum code = G_IO_ERROR_FAILED;
    g_autofree char *name = g_dbus_error_get_remote_error(error);
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) code = G_IO_ERROR_CANCELLED;
    else if (name && (g_str_has_suffix(name, ".NotAuthorized") || g_str_has_suffix(name, ".AccessDenied") ||
                      g_str_has_suffix(name, ".SessionLocked"))) code = G_IO_ERROR_PERMISSION_DENIED;
    else if (name && g_str_has_suffix(name, ".Conflict")) code = G_IO_ERROR_BUSY;
    else if (name && (g_str_has_suffix(name, ".NotSupported") || g_str_has_suffix(name, ".UnknownMethod"))) code = G_IO_ERROR_NOT_SUPPORTED;
    else if (name && (g_str_has_suffix(name, ".InvalidInput") || g_str_has_suffix(name, ".InvalidArgs"))) code = G_IO_ERROR_INVALID_ARGUMENT;
    else if (name && (g_str_has_suffix(name, ".ServiceUnknown") || g_str_has_suffix(name, ".NameHasNoOwner"))) code = G_IO_ERROR_NOT_INITIALIZED;
    g_task_return_new_error(task, G_IO_ERROR, code, "Panel command failed");
  }
  else g_task_return_boolean(task, TRUE); /* Never pass arbitrary RPC payloads to a panel. */
  g_object_unref(task);
}
static void invoke_async(GrottoPanelsService *service, GrottoPanelCommand command, const GrottoPanelCommandArgs *args,
  GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
  GrottoPanelsControl1Service *self = GROTTO_PANELS_CONTROL1_SERVICE(service);
  GTask *task = g_task_new(self, cancel, callback, data);
  Invocation *invocation = g_new0(Invocation, 1);
  invocation->generation = self->generation;
  g_task_set_task_data(task, invocation, g_free);
  g_autoptr(GVariant) wire = NULL;
  if (command == GROTTO_PANEL_PAUSE || command == GROTTO_PANEL_RESUME)
    wire = g_variant_ref_sink(g_variant_new("(b)", command == GROTTO_PANEL_PAUSE));
  else if (command == GROTTO_PANEL_CONFIGURE_REMOTE) {
    GVariantBuilder config;
    g_variant_builder_init(&config, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&config, "{sv}", "enabled", g_variant_new_boolean(args->remote_enabled));
    g_variant_builder_add(&config, "{sv}", "relays", g_variant_new_strv(args->relays, args->relays ? -1 : 0));
    g_variant_builder_add(&config, "{sv}", "allowed-methods", g_variant_new_strv(args->methods, args->methods ? -1 : 0));
    wire = g_variant_ref_sink(g_variant_new("(a{sv})", &config));
  } else if (command == GROTTO_PANEL_SET_CLIENT_LABEL)
    wire = g_variant_ref_sink(g_variant_new("(ss)", args->principal, args->label));
  else if (command == GROTTO_PANEL_SET_CLIENT_TRUST)
    wire = g_variant_ref_sink(g_variant_new("(ss)", args->principal, args->trust));
  else if (command == GROTTO_PANEL_SET_DEFAULT_IDENTITY)
    wire = g_variant_ref_sink(g_variant_new("(s)", args->public_key));
  else if (command == GROTTO_PANEL_REVOKE_CLIENT)
    wire = g_variant_ref_sink(g_variant_new("(s)", args->principal));
  else if (command == GROTTO_PANEL_SET_PERMISSIONS) {
    GVariantBuilder rows;
    g_variant_builder_init(&rows, G_VARIANT_TYPE("aa{sv}"));
    for (gsize i = 0; i < args->n_permissions; i++) {
      const GrottoPanelPermission *p = &args->permissions[i];
      GVariantBuilder row;
      g_variant_builder_init(&row, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add(&row, "{sv}", "principal", g_variant_new_string(args->principal));
      g_variant_builder_add(&row, "{sv}", "identity", g_variant_new_string(args->public_key));
      g_variant_builder_add(&row, "{sv}", "operation", g_variant_new_string(p->operation));
      g_variant_builder_add(&row, "{sv}", "decision", g_variant_new_string(p->decision));
      g_variant_builder_add(&row, "{sv}", "event-kind", g_variant_new_int32(p->event_kind));
      g_variant_builder_add(&row, "{sv}", "expires-at", g_variant_new_uint64(p->expires_at));
      if (p->relay) g_variant_builder_add(&row, "{sv}", "relay", g_variant_new_string(p->relay));
      g_variant_builder_add_value(&rows, g_variant_builder_end(&row));
    }
    wire = g_variant_ref_sink(g_variant_new("(ssaa{sv}t)", args->principal, args->public_key, &rows, args->expected_revision));
  } else wire = g_variant_ref_sink(g_variant_new("()"));
  g_dbus_connection_call(g_dbus_proxy_get_connection(self->proxy), self->owner, CONTROL_PATH, CONTROL,
    methods[command], wire, NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, cancel, invocation_done, task);
}
void grotto_panels_control1_service_host_command_async(GrottoPanelsService *service,
  GrottoPanelCommand command, const GrottoPanelCommandArgs *args, GCancellable *cancel,
  GAsyncReadyCallback callback, gpointer data) {
  g_autoptr(GError) error = NULL;
  const GrottoPanelCommandArgs empty = {0}; if (!args) args = &empty;
  if (!grotto_panel_command_args_validate(command, args, &error) ||
      (args->expected_generation && args->expected_generation != GROTTO_PANELS_CONTROL1_SERVICE(service)->generation) ||
      command == GROTTO_PANEL_SET_IDENTITY_LABEL || !grotto_panels_service_is_available(service)) {
    GTask *task = g_task_new(service, cancel, callback, data);
    if (error) g_task_return_error(task, g_steal_pointer(&error));
    else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Command unavailable");
    g_object_unref(task); return;
  }
  invoke_async(service, command, args, cancel, callback, data);
}
static gboolean invoke_finish(GrottoPanelsService *self, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}
typedef struct { guint64 generation, revision; char *principal, *epoch; guint step; GrottoPanelsQuery query; GVariant *rows; } Query;
static void query_free(gpointer data) { Query *q = data; g_free(q->principal); g_free(q->epoch); g_clear_pointer(&q->rows, g_variant_unref); g_free(q); }
static void query_done(GObject *source, GAsyncResult *result, gpointer data) {
  GTask *task = data; GrottoPanelsControl1Service *self = g_task_get_source_object(task); Query *q = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (q->generation != self->generation) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Signer changed");
    g_object_unref(task); return;
  }
  if (g_task_return_error_if_cancelled(task)) { g_object_unref(task); return; }
  if (!reply) { g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Panel data unavailable"); g_object_unref(task); return; }
  g_autoptr(GVariant) value = g_variant_get_child_value(reply, 0);
  if (q->query == GROTTO_PANELS_QUERY_PERMISSIONS && q->step < 2) {
    const char *method; GVariant *args;
    if (!q->step) {
      g_variant_lookup(value, "permissions_revision", "t", &q->revision); q->epoch = g_strdup(text(value, "control_epoch"));
      method = "ListPermissions"; args = g_variant_new("(s)", q->principal);
    } else { q->rows = g_variant_ref(value); method = "GetStatus"; args = NULL; }
    q->step++;
    g_dbus_connection_call(g_dbus_proxy_get_connection(self->proxy), self->owner, CONTROL_PATH, CONTROL,
      method, args, G_VARIANT_TYPE(q->step == 1 ? "(aa{sv})" : "(a{sv})"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 10000, g_task_get_cancellable(task), query_done, task); return;
  }
  if (q->query == GROTTO_PANELS_QUERY_PERMISSIONS) {
    guint64 revision = 0; g_variant_lookup(value, "permissions_revision", "t", &revision);
    if (revision != q->revision || g_strcmp0(q->epoch, text(value, "control_epoch"))) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_BUSY, "Permissions changed"); g_object_unref(task); return;
    }
  } else q->rows = g_variant_ref(value);
  GListStore *model = g_list_store_new(GROTTO_TYPE_PANELS_ITEM);
  for (gsize i = 0; i < g_variant_n_children(q->rows); i++) {
    g_autoptr(GVariant) row = g_variant_get_child_value(q->rows, i);
    guint64 sequence = 0; g_variant_lookup(row, "sequence", "t", &sequence);
    g_autofree char *id = g_strdup_printf("%" G_GUINT64_FORMAT, q->query == GROTTO_PANELS_QUERY_ACTIVITY ? sequence : (guint64)i);
    GrottoPanelsMetadata m = {.id = id, .principal = text(row, "principal"), .public_key = text(row, "identity"),
      .label = text(row, "label"), .operation = text(row, "operation"), .outcome = text(row, "outcome"),
      .decision = text(row, "decision"), .state = text(row, "state"), .reason = text(row, "reason"),
      .relay = text(row, "relay"), .sequence = sequence, .revision = q->revision, .owner_generation = q->generation};
    g_variant_lookup(row, "timestamp", "t", &m.timestamp); g_variant_lookup(row, "expires-at", "t", &m.expires_at);
    if (q->query == GROTTO_PANELS_QUERY_ACTIVITY) { guint32 kind = 0; m.has_event_kind = g_variant_lookup(row, "event_kind", "u", &kind); m.event_kind = kind; }
    else m.has_event_kind = g_variant_lookup(row, "event-kind", "i", &m.event_kind);
    g_autoptr(GrottoPanelsItem) item = grotto_panels_item_new(&m, NULL);
    if (q->query == GROTTO_PANELS_QUERY_PERMISSIONS && (!item ||
        g_strcmp0(m.principal, q->principal) || !*m.public_key || !*m.operation ||
        (strcmp(m.decision, "allow") && strcmp(m.decision, "deny") && strcmp(m.decision, "review")))) {
      /* Never replace a permission set assembled from a lossy projection. */
      g_object_unref(model); g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid permission snapshot");
      g_object_unref(task); return;
    }
    if (item) g_list_store_append(model, item);
  }
  g_task_return_pointer(task, model, g_object_unref); g_object_unref(task);
}
static void query_async(GrottoPanelsService *service, GrottoPanelsQuery kind, const char *principal,
  guint64 before, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
  GrottoPanelsControl1Service *self = GROTTO_PANELS_CONTROL1_SERVICE(service);
  GTask *task = g_task_new(self, cancel, callback, data); Query *q = g_new0(Query, 1);
  q->generation = self->generation; q->principal = g_strdup(principal); q->query = kind;
  g_task_set_task_data(task, q, query_free);
  g_dbus_connection_call(g_dbus_proxy_get_connection(self->proxy), self->owner, CONTROL_PATH, CONTROL,
    kind == GROTTO_PANELS_QUERY_ACTIVITY ? "ListActivity" : "GetStatus",
    kind == GROTTO_PANELS_QUERY_ACTIVITY ? g_variant_new("(tu)", before, 100u) : NULL,
    G_VARIANT_TYPE(kind == GROTTO_PANELS_QUERY_ACTIVITY ? "(aa{sv})" : "(a{sv})"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 10000, cancel, query_done, task);
}
static GListModel *query_finish(GrottoPanelsService *service, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, service), NULL); return g_task_propagate_pointer(G_TASK(result), error);
}
static void service_iface_init(GrottoPanelsServiceInterface *iface) {
  iface->query_async = query_async; iface->query_finish = query_finish;
  iface->get_model = get_model;
  iface->is_available = is_available;
  iface->can = can;
  iface->invoke_async = invoke_async;
  iface->invoke_finish = invoke_finish;
}
static void dispose(GObject *object) {
  GrottoPanelsControl1Service *self = GROTTO_PANELS_CONTROL1_SERVICE(object);
  if (self->cancel) g_cancellable_cancel(self->cancel);
  if (self->proxy) {
    g_signal_handler_disconnect(self->proxy, self->signal_id);
    g_signal_handler_disconnect(self->proxy, self->owner_id);
  }
  g_clear_object(&self->proxy);
  g_clear_object(&self->cancel);
  for (guint i = 0; i < GROTTO_PANELS_N_MODELS; i++) g_clear_object(&self->models[i]);
  G_OBJECT_CLASS(grotto_panels_control1_service_parent_class)->dispose(object);
}
static void finalize(GObject *object) {
  GrottoPanelsControl1Service *self = GROTTO_PANELS_CONTROL1_SERVICE(object);
  g_free(self->owner);
  g_hash_table_unref(self->tombstones);
  G_OBJECT_CLASS(grotto_panels_control1_service_parent_class)->finalize(object);
}
static void grotto_panels_control1_service_class_init(GrottoPanelsControl1ServiceClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = dispose;
  G_OBJECT_CLASS(klass)->finalize = finalize;
}
static void grotto_panels_control1_service_init(GrottoPanelsControl1Service *self) {
  self->cancel = g_cancellable_new();
  self->tombstones = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < GROTTO_PANELS_N_MODELS; i++) self->models[i] = g_list_store_new(GROTTO_TYPE_PANELS_ITEM);
}
GrottoPanelsService *grotto_panels_control1_service_new(GDBusProxy *proxy) {
  g_return_val_if_fail(G_IS_DBUS_PROXY(proxy), NULL);
  g_return_val_if_fail(!strcmp(g_dbus_proxy_get_name(proxy), "org.nostr.Signer"), NULL);
  g_return_val_if_fail(!strcmp(g_dbus_proxy_get_interface_name(proxy), CONTROL), NULL);
  g_return_val_if_fail(!strcmp(g_dbus_proxy_get_object_path(proxy), CONTROL_PATH), NULL);
  GrottoPanelsControl1Service *self = g_object_new(GROTTO_TYPE_PANELS_CONTROL1_SERVICE, NULL);
  self->proxy = g_object_ref(proxy);
  self->signal_id = g_signal_connect(proxy, "g-signal", G_CALLBACK(daemon_signal), self);
  self->owner_id = g_signal_connect(proxy, "notify::g-name-owner", G_CALLBACK(owner_changed), self);
  owner_changed(G_OBJECT(proxy), NULL, self);
  return GROTTO_PANELS_SERVICE(self);
}
