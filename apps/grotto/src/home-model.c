#include "home-model.h"
#include "i18n.h"

struct _GrottoHomeModel {
  GObject parent_instance;
  GDBusProxy *proxy;
  GCancellable *cancel;
  GVariant *status, *clients, *requests, *activity;
  gchar *error;
  guint generation, step;
  gboolean started, loading, dirty;
};
G_DEFINE_TYPE(GrottoHomeModel, grotto_home_model, G_TYPE_OBJECT)

typedef struct { GWeakRef model; guint generation, step; } Read;
static Read *read_new(GrottoHomeModel *self) {
  Read *r = g_new0(Read, 1); g_weak_ref_init(&r->model, self);
  r->generation = self->generation; r->step = self->step; return r;
}
static void read_free(Read *r) { g_weak_ref_clear(&r->model); g_free(r); }
static void changed(GrottoHomeModel *self) { g_signal_emit_by_name(self, "changed"); }
static void clear_snapshots(GrottoHomeModel *self) {
  g_clear_pointer(&self->status, g_variant_unref); g_clear_pointer(&self->clients, g_variant_unref);
  g_clear_pointer(&self->requests, g_variant_unref); g_clear_pointer(&self->activity, g_variant_unref);
}
static void read_next(GrottoHomeModel *self);
static void read_done(GObject *source, GAsyncResult *result, gpointer data) {
  Read *r = data; g_autoptr(GrottoHomeModel) self = g_weak_ref_get(&r->model);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
  if (self && self->started && self->generation == r->generation) {
    if (!reply) {
      clear_snapshots(self); g_free(self->error);
      self->error = g_strdup(_("Signer management is unavailable. Reopen Home to retry."));
      self->loading = FALSE; self->dirty = FALSE; changed(self);
    } else {
      GVariant **slots[] = {&self->status, &self->clients, &self->requests, &self->activity};
      g_clear_pointer(slots[r->step], g_variant_unref);
      *slots[r->step] = g_variant_get_child_value(reply, 0);
      self->step++;
      if (self->step < G_N_ELEMENTS(slots)) read_next(self);
      else {
        self->loading = FALSE; g_clear_pointer(&self->error, g_free);
        /* A signal during a read invalidates the mixed snapshot. Never enable
         * mutations with an owner/revision assembled across that transition. */
        if (self->dirty) grotto_home_model_refresh(self); else changed(self);
      }
    }
  }
  read_free(r);
}
static void read_next(GrottoHomeModel *self) {
  const gchar *methods[] = {"GetStatus", "ListClients", "ListRequests", "ListActivity"};
  GVariant *args = self->step == 3 ? g_variant_new("(tu)", (guint64)0, 100u) : NULL;
  g_dbus_proxy_call(self->proxy, methods[self->step], args, G_DBUS_CALL_FLAGS_NONE,
      10000, self->cancel, read_done, read_new(self));
}
void grotto_home_model_refresh(GrottoHomeModel *self) {
  g_return_if_fail(GROTTO_IS_HOME_MODEL(self));
  if (!self->started || !self->proxy) return;
  g_autofree gchar *owner = g_dbus_proxy_get_name_owner(self->proxy);
  if (!owner) return;
  if (self->loading) { self->dirty = TRUE; return; }
  self->loading = TRUE; self->dirty = FALSE; self->step = 0; read_next(self);
}
static void owner_changed(GObject *source, GParamSpec *pspec, gpointer data) {
  (void)source; (void)pspec; GrottoHomeModel *self = data;
  self->generation++; g_cancellable_cancel(self->cancel); g_clear_object(&self->cancel);
  self->cancel = g_cancellable_new(); self->loading = FALSE; self->dirty = FALSE;
  clear_snapshots(self); g_free(self->error); self->error = g_strdup(_("Signer not running"));
  changed(self); grotto_home_model_refresh(self);
}
static void signal_received(GDBusProxy *proxy, gchar *sender, gchar *name, GVariant *args, gpointer data) {
  (void)proxy; (void)sender; (void)args;
  if (!strcmp(name, "StatusChanged") || !strcmp(name, "ClientsChanged") ||
      !strcmp(name, "RequestsChanged") || !strcmp(name, "ActivityChanged"))
    grotto_home_model_refresh(data);
}
static void proxy_ready(GObject *source, GAsyncResult *result, gpointer data) {
  (void)source; Read *r = data; g_autoptr(GrottoHomeModel) self = g_weak_ref_get(&r->model);
  g_autoptr(GError) error = NULL;
  GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish(result, &error);
  if (self && self->started && self->generation == r->generation && proxy) {
    self->proxy = proxy;
    g_signal_connect(proxy, "g-signal", G_CALLBACK(signal_received), self);
    g_signal_connect(proxy, "notify::g-name-owner", G_CALLBACK(owner_changed), self);
    owner_changed(G_OBJECT(proxy), NULL, self);
  } else {
    g_clear_object(&proxy);
    if (self && self->started && self->generation == r->generation) {
      g_free(self->error); self->error = g_strdup(_("The signer session bus is unavailable.")); changed(self);
    }
  }
  read_free(r);
}
void grotto_home_model_start(GrottoHomeModel *self) {
  g_return_if_fail(GROTTO_IS_HOME_MODEL(self)); if (self->started) return;
  self->started = TRUE; self->generation++; self->cancel = g_cancellable_new();
  g_dbus_proxy_new_for_bus(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
      NULL, "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1",
      self->cancel, proxy_ready, read_new(self));
}
void grotto_home_model_stop(GrottoHomeModel *self) {
  self->started = FALSE; self->generation++; self->loading = FALSE; self->dirty = FALSE;
  if (self->cancel) g_cancellable_cancel(self->cancel);
  if (self->proxy) g_signal_handlers_disconnect_by_data(self->proxy, self);
  g_clear_object(&self->proxy); g_clear_object(&self->cancel); clear_snapshots(self);
}
static void dispose(GObject *object) {
  grotto_home_model_stop(GROTTO_HOME_MODEL(object)); G_OBJECT_CLASS(grotto_home_model_parent_class)->dispose(object);
}
static void finalize(GObject *object) {
  g_free(GROTTO_HOME_MODEL(object)->error); G_OBJECT_CLASS(grotto_home_model_parent_class)->finalize(object);
}
static void grotto_home_model_class_init(GrottoHomeModelClass *klass) {
  GObjectClass *oc = G_OBJECT_CLASS(klass); oc->dispose = dispose; oc->finalize = finalize;
  g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}
static void grotto_home_model_init(GrottoHomeModel *self) { self->error = g_strdup(_("Connecting to signer…")); }
GrottoHomeModel *grotto_home_model_new(void) { return g_object_new(GROTTO_TYPE_HOME_MODEL, NULL); }
GVariant *grotto_home_model_status(GrottoHomeModel *s) { return s->status; }
GVariant *grotto_home_model_clients(GrottoHomeModel *s) { return s->clients; }
GVariant *grotto_home_model_requests(GrottoHomeModel *s) { return s->requests; }
GVariant *grotto_home_model_activity(GrottoHomeModel *s) { return s->activity; }
GDBusProxy *grotto_home_model_proxy(GrottoHomeModel *s) { return s->proxy; }
gboolean grotto_home_model_writable(GrottoHomeModel *s) {
  guint version = 0;
  return s->status && !s->loading && !s->error && g_variant_lookup(s->status, "control_version", "u", &version) && version == 1;
}
const gchar *grotto_home_model_error(GrottoHomeModel *s) { return s->error; }
