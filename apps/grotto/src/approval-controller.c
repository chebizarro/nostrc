#include "approval-controller.h"
#include <string.h>

#define SIGNER "org.nostr.Signer"
#define SIGNER_PATH "/org/nostr/signer"
#define CONTROL "org.nostr.Grotto.Control1"
#define CONTROL_PATH "/org/nostr/signer/grotto"
#define SUMMARY_ID "grotto-pending"

struct _GrottoApprovalRequest {
  GObject parent_instance;
  gchar *id;
  GVariant *info;
  GrottoApprovalState state;
  GCancellable *cancel;
  gint64 deadline;
  guint expiry;
  gboolean held, deferred;
};
G_DEFINE_FINAL_TYPE(GrottoApprovalRequest, grotto_approval_request, G_TYPE_OBJECT)
static void grotto_approval_request_finalize(GObject *object) {
  GrottoApprovalRequest *r = GROTTO_APPROVAL_REQUEST(object);
  if (r->expiry)
    g_source_remove(r->expiry);
  g_clear_pointer(&r->info, g_variant_unref);
  g_clear_object(&r->cancel);
  g_free(r->id);
  G_OBJECT_CLASS(grotto_approval_request_parent_class)->finalize(object);
}
static void grotto_approval_request_class_init(GrottoApprovalRequestClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = grotto_approval_request_finalize;
}
static void grotto_approval_request_init(GrottoApprovalRequest *r) {
  r->state = GROTTO_APPROVAL_FETCHING;
  r->cancel = g_cancellable_new();
}
const char *grotto_approval_request_get_id(GrottoApprovalRequest *r) {
  return r->id;
}
GVariant *grotto_approval_request_get_info(GrottoApprovalRequest *r) {
  return r->info;
}
GrottoApprovalState grotto_approval_request_get_state(GrottoApprovalRequest *r) {
  return r->state;
}

struct _GrottoApprovalController {
  GObject parent_instance;
  GWeakRef app, review_window;
  GDBusConnection *bus;
  GListStore *pending;
  GHashTable *requests, *tombstones, *labels, *tiers;
  GCancellable *cancel;
  gchar *owner, *review_id, *wanted_review;
  guint64 generation, revision, clients_serial;
  guint watch, lock_watch, subscriptions[3], holds, summary_timer, notification_watch[1];
  guint64 lock_revision;
  gint64 summary_sent;
  gboolean stopped, compatible, checking_owner, listing, list_again, locked, notifications,
      bootstrap_held;
  GnostrApprovalDialog *dialog;
};
G_DEFINE_FINAL_TYPE(GrottoApprovalController, grotto_approval_controller, G_TYPE_OBJECT)
enum { MESSAGE, AVAILABILITY_CHANGED, NOTIFICATIONS_CHANGED, RECONCILED, N_SIGNALS };
static guint signals[N_SIGNALS];
static const char *text_field(GVariant *row, const char *key) {
  const char *s = "";
  if (row)
    g_variant_lookup(row, key, "&s", &s);
  return s;
}
static void message(GrottoApprovalController *s, const char *text) {
  g_signal_emit(s, signals[MESSAGE], 0, text);
}
static void hold(GrottoApprovalController *s) {
  g_autoptr(GApplication) app = g_weak_ref_get(&s->app);
  if (app) {
    g_application_hold(app);
    s->holds++;
  }
}
static void release(GrottoApprovalController *s) {
  g_autoptr(GApplication) app = g_weak_ref_get(&s->app);
  if (s->holds) {
    s->holds--;
    if (app)
      g_application_release(app);
  }
}
static void withdraw(GrottoApprovalController *s, const char *id) {
  g_autoptr(GApplication) app = g_weak_ref_get(&s->app);
  if (app && g_application_get_is_registered(app))
    g_application_withdraw_notification(app, id);
}
static gchar *notification_id(const char *id) {
  return g_strconcat("grotto-request-", id, NULL);
}
static const char *client_name(GrottoApprovalController *s, GVariant *info) {
  const char *principal = text_field(info, "principal");
  const char *label = g_hash_table_lookup(s->labels, principal);
  if (label && *label)
    return label;
  gboolean verified = FALSE;
  if (info)
    g_variant_lookup(info, "verified", "b", &verified);
  if (!verified)
    return "Unverified app";
  const char *app_id = text_field(info, "app_id");
  return *app_id ? app_id : "Unverified app";
}
static gchar *operation_label(GVariant *info) {
  const char *op = text_field(info, "operation");
  guint32 kind;
  if (!strcmp(op, "sign_event") && g_variant_lookup(info, "event_kind", "u", &kind))
    return g_strdup_printf("Sign event · %s (kind %u)", gnostr_approval_kind_name(kind), kind);
  if (!strcmp(op, "connect"))
    return g_strdup("Connect application");
  if (!strcmp(op, "get_public_key"))
    return g_strdup("Share public key");
  if (!strcmp(op, "nip44_conversation_key"))
    return g_strdup("Share conversation key");
  if (g_str_has_suffix(op, "encrypt"))
    return g_strdup("Encrypt message");
  if (g_str_has_suffix(op, "decrypt"))
    return g_strdup("Decrypt message");
  return g_strdup("Review signer request");
}
static void update_notifications(GrottoApprovalController *s);
static gboolean summary_ready(gpointer data) {
  GrottoApprovalController *s = data;
  s->summary_timer = 0;
  update_notifications(s);
  return G_SOURCE_REMOVE;
}
static void update_notifications(GrottoApprovalController *s) {
  if (s->stopped)
    return;
  g_autoptr(GApplication) app = g_weak_ref_get(&s->app);
  if (!app || !g_application_get_is_registered(app))
    return;
  guint count = 0;
  GrottoApprovalRequest *single = NULL;
  for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(s->pending)); i++) {
    g_autoptr(GrottoApprovalRequest) r = g_list_model_get_item(G_LIST_MODEL(s->pending), i);
    if (r->state == GROTTO_APPROVAL_READY && !r->deferred) {
      count++;
      single = r;
    }
    g_autofree gchar *id = notification_id(r->id);
    withdraw(s, id);
  }
  if (!s->notifications || !count) {
    withdraw(s, SUMMARY_ID);
    return;
  }
  if (count == 1) {
    withdraw(s, SUMMARY_ID);
    g_autofree gchar *id = notification_id(single->id);
    g_autoptr(GNotification) n =
        g_notification_new(s->locked ? "Grotto" : client_name(s, single->info));
    g_autofree gchar *body = operation_label(single->info);
    g_notification_set_body(n, s->locked ? "Grotto needs your approval" : body);
    g_notification_set_priority(n, G_NOTIFICATION_PRIORITY_NORMAL);
    g_notification_set_category(n, "device");
    g_notification_set_default_action_and_target(n, "app.review-request", "s", single->id);
    g_notification_add_button_with_target(n, "Review", "app.review-request", "s", single->id);
    g_notification_add_button_with_target(n, "Deny once", "app.deny-request", "s", single->id);
    g_application_send_notification(app, id, n);
  } else {
    gint64 left = s->summary_sent + 2 * G_USEC_PER_SEC - g_get_monotonic_time();
    if (left > 0) {
      if (!s->summary_timer)
        s->summary_timer = g_timeout_add((left + 999) / 1000, summary_ready, s);
      return;
    }
    g_autoptr(GNotification) n = g_notification_new("Grotto");
    g_autofree gchar *body = g_strdup_printf("%u requests waiting for review", count);
    g_notification_set_body(n, s->locked ? "Grotto needs your approval" : body);
    g_notification_set_priority(n, G_NOTIFICATION_PRIORITY_NORMAL);
    g_notification_set_default_action_and_target(n, "app.review-request", "s", "");
    g_notification_add_button_with_target(n, "Review", "app.review-request", "s", "");
    g_application_send_notification(app, SUMMARY_ID, n);
    s->summary_sent = g_get_monotonic_time();
  }
}
static gboolean destroy_review_window(gpointer data) {
  gtk_window_destroy(GTK_WINDOW(data));
  return G_SOURCE_REMOVE;
}
static void retire_review_window(GrottoApprovalController *s) {
  g_autoptr(GtkWindow) win = g_weak_ref_get(&s->review_window);
  /* AdwDialog 1.5 emits ::closed before its host unparents the dialog. */
  if (win)
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, destroy_review_window, g_object_ref(win),
                    g_object_unref);
  g_weak_ref_set(&s->review_window, NULL);
}
static void close_review(GrottoApprovalController *s) {
  if (s->dialog) {
    gnostr_approval_dialog_set_callback(s->dialog, NULL, NULL);
    g_signal_handlers_disconnect_by_data(s->dialog, s);
    adw_dialog_force_close(ADW_DIALOG(s->dialog));
    g_clear_object(&s->dialog);
  }
  g_clear_pointer(&s->review_id, g_free);
  retire_review_window(s);
}
static void resolve(GrottoApprovalController *s, const char *id, GrottoApprovalState state) {
  g_hash_table_add(s->tombstones, g_strdup(id));
  GrottoApprovalRequest *r = g_hash_table_lookup(s->requests, id);
  if (!r)
    return;
  r->state = state;
  g_cancellable_cancel(r->cancel);
  if (r->expiry) {
    g_source_remove(r->expiry);
    r->expiry = 0;
  }
  if (!g_strcmp0(s->review_id, id))
    close_review(s);
  g_autofree gchar *nid = notification_id(id);
  withdraw(s, nid);
  if (r->held) {
    r->held = FALSE;
    release(s);
  }
  guint index;
  if (g_list_store_find(s->pending, r, &index))
    g_list_store_remove(s->pending, index);
  g_hash_table_remove(s->requests, id);
  update_notifications(s);
}
typedef struct {
  GrottoApprovalController *self;
  GrottoApprovalRequest *request;
  guint64 generation, revision, clients_serial;
  gint64 started;
  gboolean held;
} Fetch;
static Fetch *fetch_new(GrottoApprovalController *s, GrottoApprovalRequest *r) {
  Fetch *f = g_new0(Fetch, 1);
  f->self = g_object_ref(s);
  f->request = r ? g_object_ref(r) : NULL;
  f->generation = s->generation;
  f->revision = s->revision;
  f->started = g_get_monotonic_time();
  return f;
}
static gboolean fetch_current(Fetch *f) {
  GrottoApprovalController *s = f->self;
  return !s->stopped && s->owner && f->generation == s->generation &&
         (!f->request || g_hash_table_lookup(s->requests, f->request->id) == f->request);
}
static void fetch_free(Fetch *f) {
  if (f->held)
    release(f->self);
  g_clear_object(&f->request);
  g_object_unref(f->self);
  g_free(f);
}
static gboolean expired(gpointer data) {
  Fetch *f = data;
  if (fetch_current(f)) {
    f->request->expiry = 0;
    resolve(f->self, f->request->id, GROTTO_APPROVAL_RESOLVED);
  }
  return G_SOURCE_REMOVE;
}
static gboolean same_scope(GVariant *a, GVariant *b) {
  const char *keys[] = {"identity",      "principal", "transport", "operation",    "event_kind",
                        "event_summary", "flow_id",   "reason",    "reason_source"};
  for (guint i = 0; i < G_N_ELEMENTS(keys); i++) {
    g_autoptr(GVariant) left = g_variant_lookup_value(a, keys[i], NULL);
    g_autoptr(GVariant) right = g_variant_lookup_value(b, keys[i], NULL);
    if ((!left != !right) || (left && !g_variant_equal(left, right)))
      return FALSE;
  }
  return TRUE;
}
static void apply_info(GrottoApprovalController *s, GrottoApprovalRequest *r, GVariant *info,
                       gint64 started) {
  if (r->info && !same_scope(r->info, info)) {
    resolve(s, r->id, GROTTO_APPROVAL_UNAVAILABLE);
    message(s, "The request's pinned scope changed. Ask the client to send a new request.");
    return;
  }
  const char *state = text_field(info, "state");
  if (*state && strcmp(state, "PENDING")) {
    resolve(s, r->id, GROTTO_APPROVAL_RESOLVED);
    return;
  }
  guint32 remaining;
  guint64 expires;
  gint64 deadline = 0;
  if (g_variant_lookup(info, "remaining_seconds", "u", &remaining))
    deadline = started + (gint64)remaining * G_USEC_PER_SEC;
  else if (g_variant_lookup(info, "expires_at", "t", &expires) &&
           expires <= G_MAXINT64 / G_USEC_PER_SEC)
    deadline = g_get_monotonic_time() +
               MAX((gint64)0, (gint64)expires * G_USEC_PER_SEC - g_get_real_time());
  /* Missing authoritative age is read-only: never restart a catch-up TTL. */
  if (r->deadline)
    deadline = deadline ? MIN(deadline, r->deadline) : r->deadline;
  if (deadline && deadline <= g_get_monotonic_time()) {
    resolve(s, r->id, GROTTO_APPROVAL_RESOLVED);
    return;
  }
  g_debug("Approval controller metadata: request %s, state %s, compatible %d, deadline known %d",
          r->id, state, s->compatible, deadline != 0);
  r->deadline = deadline;
  g_clear_pointer(&r->info, g_variant_unref);
  r->info = g_variant_ref(info);
  if (r->state != GROTTO_APPROVAL_SUBMITTING)
    r->state = deadline && s->compatible ? GROTTO_APPROVAL_READY : GROTTO_APPROVAL_UNAVAILABLE;
  if (r->expiry)
    g_source_remove(r->expiry);
  r->expiry = deadline ? g_timeout_add_full(
                             G_PRIORITY_DEFAULT,
                             MAX((gint64)1, MIN((gint64)G_MAXUINT,
                                                (deadline - g_get_monotonic_time() + 999) / 1000)),
                             expired, fetch_new(s, r), (GDestroyNotify)fetch_free)
                       : 0;
  guint index;
  if (g_list_store_find(s->pending, r, &index))
    g_list_model_items_changed(G_LIST_MODEL(s->pending), index, 1, 1);
  update_notifications(s);
  if (s->wanted_review && *s->wanted_review && !strcmp(s->wanted_review, r->id) &&
      r->state == GROTTO_APPROVAL_READY) {
    g_autofree gchar *id = g_steal_pointer(&s->wanted_review);
    grotto_approval_controller_review(s, *id ? id : r->id);
  }
}
static GrottoApprovalRequest *insert(GrottoApprovalController *s, const char *id) {
  if (!*id || g_hash_table_contains(s->tombstones, id))
    return NULL;
  GrottoApprovalRequest *r = g_hash_table_lookup(s->requests, id);
  if (r)
    return r;
  r = g_object_new(GROTTO_TYPE_APPROVAL_REQUEST, NULL);
  r->id = g_strdup(id);
  r->held = TRUE;
  hold(s);
  g_hash_table_insert(s->requests, g_strdup(id), r);
  g_list_store_append(s->pending, r);
  return r;
}
static void info_done(GObject *source, GAsyncResult *result, gpointer data) {
  Fetch *f = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (fetch_current(f)) {
    if (reply) {
      g_autoptr(GVariant) row = g_variant_get_child_value(reply, 0);
      apply_info(f->self, f->request, row, f->started);
    } else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_autofree gchar *remote = g_dbus_error_get_remote_error(error);
      if (g_str_has_suffix(remote ? remote : "", ".NotFound"))
        resolve(f->self, f->request->id, GROTTO_APPROVAL_RESOLVED);
      else {
        f->request->state = GROTTO_APPROVAL_UNAVAILABLE;
        message(f->self, "Request details unavailable. Reopen Grotto to retry.");
      }
    }
  }
  fetch_free(f);
}
static void fetch_info(GrottoApprovalController *s, GrottoApprovalRequest *r) {
  g_dbus_connection_call(s->bus, s->owner, SIGNER_PATH, SIGNER, "GetApprovalInfo",
                         g_variant_new("(s)", r->id), G_VARIANT_TYPE("(a{sv})"),
                         G_DBUS_CALL_FLAGS_NONE, 5000, r->cancel, info_done, fetch_new(s, r));
}
static void list_done(GObject *source, GAsyncResult *result, gpointer data) {
  Fetch *f = data;
  GrottoApprovalController *s = f->self;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (fetch_current(f)) {
    s->listing = FALSE;
    if (reply && f->revision == s->revision) {
      g_autoptr(GVariant) rows = g_variant_get_child_value(reply, 0);
      g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
      for (gsize i = 0; i < g_variant_n_children(rows); i++) {
        g_autoptr(GVariant) row = g_variant_get_child_value(rows, i);
        const char *id = text_field(row, "request_id");
        g_hash_table_add(seen, g_strdup(id));
        GrottoApprovalRequest *r = insert(s, id);
        if (r)
          apply_info(s, r, row, f->started);
      }
      /* Only remove requests known before this snapshot was sent. Arrival
       * bumps revision, so an older snapshot never removes a new arrival. */
      for (gint i = (gint)g_list_model_get_n_items(G_LIST_MODEL(s->pending)) - 1; i >= 0; i--) {
        g_autoptr(GrottoApprovalRequest) r = g_list_model_get_item(G_LIST_MODEL(s->pending), i);
        if (!g_hash_table_contains(seen, r->id))
          resolve(s, r->id, GROTTO_APPROVAL_RESOLVED);
      }
    } else if (reply)
      s->list_again = TRUE;
    else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      if (s->wanted_review) {
        gboolean specific = *s->wanted_review != '\0';
        g_clear_pointer(&s->wanted_review, g_free);
        message(s, specific ? "That request is no longer waiting."
                            : "Pending requests unavailable. Reopen Grotto to retry.");
      } else
        message(s, "Pending requests unavailable. Reopen Grotto to retry.");
      g_signal_emit(s, signals[RECONCILED], 0, FALSE);
    }
    if (s->list_again) {
      s->list_again = FALSE;
      grotto_approval_controller_reconcile(s);
    } else if (s->wanted_review && *s->wanted_review &&
               !g_hash_table_contains(s->requests, s->wanted_review)) {
      g_clear_pointer(&s->wanted_review, g_free);
      message(s, "That request is no longer waiting.");
    }
    if (reply && f->revision == s->revision) {
      if (s->wanted_review && !*s->wanted_review) {
        g_clear_pointer(&s->wanted_review, g_free);
        if (!g_list_model_get_n_items(G_LIST_MODEL(s->pending)))
          message(s, "No requests are waiting.");
      }
      g_signal_emit(s, signals[RECONCILED], 0, TRUE);
    }
  }
  fetch_free(f);
}
void grotto_approval_controller_reconcile(GrottoApprovalController *s) {
  if (s->stopped)
    return;
  if (!s->owner || !s->compatible) {
    if (s->bootstrap_held || s->checking_owner)
      return;
    if (s->wanted_review) {
      gboolean specific = *s->wanted_review != '\0';
      g_clear_pointer(&s->wanted_review, g_free);
      message(s, specific ? "That request is no longer waiting."
                          : "Signer unavailable. Requests could not be listed.");
    }
    g_signal_emit(s, signals[RECONCILED], 0, FALSE);
    return;
  }
  if (s->listing) {
    s->list_again = TRUE;
    return;
  }
  s->listing = TRUE;
  Fetch *f = fetch_new(s, NULL);
  f->held = TRUE;
  hold(s);
  g_dbus_connection_call(s->bus, s->owner, CONTROL_PATH, CONTROL, "ListRequests", NULL,
                         G_VARIANT_TYPE("(aa{sv})"), G_DBUS_CALL_FLAGS_NONE, 5000, s->cancel,
                         list_done, f);
}
static void status_done(GObject *source, GAsyncResult *result, gpointer data) {
  Fetch *f = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (fetch_current(f)) {
    f->self->checking_owner = FALSE;
    guint32 version = 0;
    if (reply) {
      g_autoptr(GVariant) row = g_variant_get_child_value(reply, 0);
      g_variant_lookup(row, "control_version", "u", &version);
    }
    g_debug("Approval controller: Control1 version %u", version);
    f->self->compatible = version == 1;
    if (version == 1)
      grotto_approval_controller_reconcile(f->self);
    else {
      message(f->self, "Update Grotto to review requests. This daemon is read-only.");
      grotto_approval_controller_reconcile(f->self);
    }
    g_signal_emit(f->self, signals[AVAILABILITY_CHANGED], 0);
  }
  fetch_free(f);
}
static void clients_done(GObject *source, GAsyncResult *result, gpointer data) {
  Fetch *f = data;
  g_autoptr(GVariant) reply =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);
  if (fetch_current(f) && reply && f->clients_serial == f->self->clients_serial) {
    g_hash_table_remove_all(f->self->labels);
    g_hash_table_remove_all(f->self->tiers);
    g_autoptr(GVariant) rows = g_variant_get_child_value(reply, 0);
    for (gsize i = 0; i < g_variant_n_children(rows); i++) {
      g_autoptr(GVariant) row = g_variant_get_child_value(rows, i);
      g_hash_table_insert(f->self->labels, g_strdup(text_field(row, "principal")),
                          g_strdup(text_field(row, "label")));
      g_hash_table_insert(f->self->tiers, g_strdup(text_field(row, "principal")),
                          g_strdup(text_field(row, "trust-tier")));
    }
    GrottoApprovalController *s = f->self;
    GrottoApprovalRequest *review =
        s->review_id ? g_hash_table_lookup(s->requests, s->review_id) : NULL;
    if (s->dialog && review)
      gnostr_approval_dialog_set_trust_level(
          s->dialog, g_hash_table_lookup(s->tiers, text_field(review->info, "principal")));
    guint count = g_list_model_get_n_items(G_LIST_MODEL(s->pending));
    if (count)
      g_list_model_items_changed(G_LIST_MODEL(s->pending), 0, count, count);
    update_notifications(f->self);
  }
  fetch_free(f);
}
static void fetch_clients(GrottoApprovalController *s) {
  Fetch *f = fetch_new(s, NULL);
  f->clients_serial = ++s->clients_serial;
  g_dbus_connection_call(s->bus, s->owner, CONTROL_PATH, CONTROL, "ListClients", NULL,
                         G_VARIANT_TYPE("(aa{sv})"), G_DBUS_CALL_FLAGS_NONE, 5000, s->cancel,
                         clients_done, f);
}
static void reset_owner(GrottoApprovalController *s) {
  s->generation++;
  s->compatible = FALSE;
  s->checking_owner = FALSE;
  s->listing = s->list_again = FALSE;
  g_cancellable_cancel(s->cancel);
  g_clear_object(&s->cancel);
  s->cancel = g_cancellable_new();
  close_review(s);
  while (g_list_model_get_n_items(G_LIST_MODEL(s->pending))) {
    g_autoptr(GrottoApprovalRequest) r = g_list_model_get_item(G_LIST_MODEL(s->pending), 0);
    resolve(s, r->id, GROTTO_APPROVAL_UNAVAILABLE);
  }
  g_hash_table_remove_all(s->tombstones);
  g_hash_table_remove_all(s->labels);
  g_hash_table_remove_all(s->tiers);
  g_clear_pointer(&s->owner, g_free);
  s->revision = 0;
  withdraw(s, SUMMARY_ID);
}
static void release_bootstrap(GrottoApprovalController *s) {
  if (s->bootstrap_held) {
    s->bootstrap_held = FALSE;
    release(s);
  }
}
static void appeared(GDBusConnection *bus, const char *name, const char *owner, gpointer data) {
  (void)bus;
  (void)name;
  GrottoApprovalController *s = data;
  reset_owner(s);
  s->owner = g_strdup(owner);
  s->checking_owner = TRUE;
  Fetch *f = fetch_new(s, NULL);
  f->held = TRUE;
  hold(s);
  g_dbus_connection_call(s->bus, s->owner, CONTROL_PATH, CONTROL, "GetStatus", NULL,
                         G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 5000, s->cancel,
                         status_done, f);
  fetch_clients(s);
  release_bootstrap(s);
}
static void vanished(GDBusConnection *bus, const char *name, gpointer data) {
  (void)bus;
  (void)name;
  GrottoApprovalController *s = data;
  reset_owner(s);
  release_bootstrap(s);
  if (s->wanted_review) {
    gboolean specific = *s->wanted_review != '\0';
    g_clear_pointer(&s->wanted_review, g_free);
    message(s, specific ? "That request is no longer waiting." : "No requests are waiting.");
  }
  g_signal_emit(s, signals[RECONCILED], 0, FALSE);
  g_signal_emit(s, signals[AVAILABILITY_CHANGED], 0);
}
static void signal_received(GDBusConnection *bus, const char *sender, const char *path,
                            const char *interface, const char *name, GVariant *args,
                            gpointer data) {
  (void)bus;
  (void)path;
  (void)interface;
  GrottoApprovalController *s = data;
  g_debug("Approval controller signal %s from %s (owner %s)", name, sender,
          s->owner ? s->owner : "none");
  if (s->stopped || g_strcmp0(sender, s->owner))
    return;
  if (!strcmp(name, "ApprovalCompleted")) {
    const char *id;
    gboolean ok;
    g_variant_get(args, "(&sb)", &id, &ok);
    (void)ok;
    s->revision++;
    resolve(s, id, GROTTO_APPROVAL_RESOLVED);
  } else if (!strcmp(name, "ApprovalRequested")) {
    const char *p, *identity, *kind, *preview, *id;
    g_variant_get(args, "(&s&s&s&s&s)", &p, &identity, &kind, &preview, &id);
    (void)p;
    (void)identity;
    (void)kind;
    (void)preview;
    s->revision++;
    GrottoApprovalRequest *r = insert(s, id);
    if (r && r->state == GROTTO_APPROVAL_FETCHING)
      fetch_info(s, r);
  } else if (!strcmp(name, "RequestsChanged")) {
    /* The revision is an owner-scoped invalidation, not a timestamp. */
    s->revision++;
    grotto_approval_controller_reconcile(s);
  } else if (!strcmp(name, "ClientsChanged"))
    fetch_clients(s);
}
/* The GNotification backend has no portable display-hint setter. Until a
 * screen-lock service positively reports unlocked, use generic text. This
 * also protects desktops without lock observation; no content enters a body. */
static void lock_changed(GDBusConnection *bus, const char *sender, const char *path,
                         const char *interface, const char *name, GVariant *args, gpointer data) {
  (void)bus;
  (void)sender;
  (void)path;
  (void)interface;
  (void)name;
  GrottoApprovalController *s = data;
  s->lock_revision++;
  g_variant_get(args, "(b)", &s->locked);
  g_signal_emit_by_name(s, "screen-lock-changed", s->locked);
  update_notifications(s);
}
static void lock_done(GObject *source, GAsyncResult *result, gpointer data) {
  Fetch *f = data;
  GrottoApprovalController *s = f->self;
  g_autoptr(GVariant) r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);
  if (r && !s->stopped && f->revision == s->lock_revision) {
    g_variant_get(r, "(b)", &s->locked);
    g_signal_emit_by_name(s, "screen-lock-changed", s->locked);
    update_notifications(s);
  }
  fetch_free(f);
}
static void lock_appeared(GDBusConnection *bus, const char *name, const char *owner,
                          gpointer data) {
  (void)name;
  GrottoApprovalController *s = data;
  s->locked = TRUE;
  s->lock_revision++;
  update_notifications(s);
  Fetch *f = fetch_new(s, NULL);
  f->revision = s->lock_revision;
  g_dbus_connection_call(bus, owner, "/org/gnome/ScreenSaver", "org.gnome.ScreenSaver", "GetActive",
                         NULL, G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, NULL,
                         lock_done, f);
}
static void lock_vanished(GDBusConnection *bus, const char *name, gpointer data) {
  (void)bus;
  (void)name;
  GrottoApprovalController *s = data;
  s->lock_revision++;
  s->locked = TRUE;
  update_notifications(s);
}
static void notification_appeared(GDBusConnection *bus, const char *name, const char *owner,
                                  gpointer data) {
  (void)bus;
  (void)name;
  (void)owner;
  GrottoApprovalController *s = data;
  s->notifications = TRUE;
  update_notifications(s);
  g_signal_emit(s, signals[NOTIFICATIONS_CHANGED], 0);
}
static void notification_vanished(GDBusConnection *bus, const char *name, gpointer data) {
  (void)bus;
  (void)name;
  GrottoApprovalController *s = data;
  s->notifications = FALSE;
  update_notifications(s);
  g_signal_emit(s, signals[NOTIFICATIONS_CHANGED], 0);
}

/* A decision uses a fresh authoritative snapshot on the pinned owner. The
 * daemon remains the atomic decision arbiter; a completion can cancel any
 * stage without resurrecting the request or retrying cryptography. */
typedef struct {
  Fetch *fetch;
  gboolean allow, remember;
  guint64 ttl;
} Decision;
static void decision_free(Decision *d) {
  fetch_free(d->fetch);
  g_free(d);
}
static void decision_done(GObject *source, GAsyncResult *result, gpointer data) {
  GTask *task = data;
  Decision *d = g_task_get_task_data(task);
  Fetch *f = d->fetch;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  gboolean ok = FALSE;
  if (reply)
    g_variant_get(reply, "(b)", &ok);
  if (reply) {
    if (fetch_current(f))
      resolve(f->self, f->request->id, GROTTO_APPROVAL_RESOLVED);
    if (ok)
      g_task_return_boolean(task, TRUE);
    else
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                              "The signer could not complete the decision.");
  } else {
    if (fetch_current(f)) {
      f->request->state = GROTTO_APPROVAL_UNAVAILABLE;
      grotto_approval_controller_reconcile(f->self);
    }
    g_task_return_error(task, g_steal_pointer(&error));
  }
  g_object_unref(task);
}
static void decision_checked(GObject *source, GAsyncResult *result, gpointer data) {
  GTask *task = data;
  Decision *d = g_task_get_task_data(task);
  Fetch *f = d->fetch;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (g_task_return_error_if_cancelled(task)) {
    if (fetch_current(f)) {
      f->request->state = GROTTO_APPROVAL_READY;
      grotto_approval_controller_reconcile(f->self);
    }
    g_object_unref(task);
    return;
  }
  if (!fetch_current(f)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "That request is no longer waiting.");
    g_object_unref(task);
    return;
  }
  if (!reply) {
    f->request->state = GROTTO_APPROVAL_UNAVAILABLE;
    grotto_approval_controller_reconcile(f->self);
    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
    return;
  }
  g_autoptr(GVariant) row = g_variant_get_child_value(reply, 0);
  if (!same_scope(row, f->request->info)) {
    resolve(f->self, f->request->id, GROTTO_APPROVAL_UNAVAILABLE);
    g_task_return_new_error(
        task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "The request's pinned scope changed. Ask the client to send a new request.");
    g_object_unref(task);
    return;
  }
  apply_info(f->self, f->request, row, f->started);
  gboolean rememberable = FALSE;
  g_variant_lookup(row, "rememberable", "b", &rememberable);
  if (!fetch_current(f) || !f->request->deadline || !f->self->compatible ||
      (d->remember && !rememberable) || (d->allow && !gnostr_approval_info_can_approve(row))) {
    if (fetch_current(f)) {
      f->request->state = GROTTO_APPROVAL_READY;
      apply_info(f->self, f->request, row, f->started);
    }
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "Request cannot accept this decision.");
    g_object_unref(task);
    return;
  }
  /* Keep cleanup anchored to the original deadline even while a decision
   * or its recovery snapshot is uncertain. The daemon arbitrates execution. */
  g_dbus_connection_call(f->self->bus, f->self->owner, SIGNER_PATH, SIGNER, "ApproveRequest",
                         g_variant_new("(sbbt)", f->request->id, d->allow, d->remember, d->ttl),
                         G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 30000, f->self->cancel,
                         decision_done, task);
}
void grotto_approval_controller_decide_async(GrottoApprovalController *s, const char *id,
                                             gboolean allow, gboolean remember, guint64 ttl,
                                             GCancellable *cancel, GAsyncReadyCallback callback,
                                             gpointer data) {
  GTask *task = g_task_new(s, cancel, callback, data);
  GrottoApprovalRequest *r = g_hash_table_lookup(s->requests, id);
  if (s->stopped || !s->compatible || !r || r->state != GROTTO_APPROVAL_READY ||
      r->deadline <= g_get_monotonic_time()) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "That request is no longer waiting.");
    g_object_unref(task);
    return;
  }
  r->state = GROTTO_APPROVAL_SUBMITTING;
  Decision *d = g_new0(Decision, 1);
  d->fetch = fetch_new(s, r);
  d->allow = allow;
  d->remember = remember;
  d->ttl = ttl;
  g_task_set_task_data(task, d, (GDestroyNotify)decision_free);
  g_dbus_connection_call(s->bus, s->owner, SIGNER_PATH, SIGNER, "GetApprovalInfo",
                         g_variant_new("(s)", id), G_VARIANT_TYPE("(a{sv})"),
                         G_DBUS_CALL_FLAGS_NONE, 5000, r->cancel, decision_checked, task);
}
gboolean grotto_approval_controller_decide_finish(GrottoApprovalController *s, GAsyncResult *result,
                                                  GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, s), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

static gboolean same_group(GVariant *a, GVariant *b) {
  const char *keys[] = {"principal", "identity", "transport", "flow_id"};
  if (!*text_field(a, "principal"))
    return FALSE;
  for (guint i = 0; i < G_N_ELEMENTS(keys); i++)
    if (strcmp(text_field(a, keys[i]), text_field(b, keys[i])))
      return FALSE;
  return TRUE;
}
static gchar *next_group_id(GrottoApprovalController *s, GVariant *info) {
  for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(s->pending)); i++) {
    g_autoptr(GrottoApprovalRequest) r = g_list_model_get_item(G_LIST_MODEL(s->pending), i);
    if (r->state == GROTTO_APPROVAL_READY && !r->deferred && same_group(info, r->info))
      return g_strdup(r->id);
  }
  return NULL;
}
static void review_decided(GObject *source, GAsyncResult *result, gpointer data) {
  GVariant *info = data;
  GrottoApprovalController *s = GROTTO_APPROVAL_CONTROLLER(source);
  g_autoptr(GError) error = NULL;
  if (!grotto_approval_controller_decide_finish(s, result, &error)) {
    if (!s->stopped)
      message(
          s,
          "The signer could not complete this decision. Review the pending inbox before retrying.");
  } else {
    g_autofree gchar *id = next_group_id(s, info);
    if (id)
      grotto_approval_controller_review(s, id);
  }
  g_variant_unref(info);
}
static void review_decision(gboolean allow, gboolean remember, const char *identity, guint64 ttl,
                            gpointer data) {
  (void)identity;
  GrottoApprovalController *s = data;
  GrottoApprovalRequest *r = s->review_id ? g_hash_table_lookup(s->requests, s->review_id) : NULL;
  if (!r)
    return;
  g_autofree gchar *id = g_strdup(r->id);
  GVariant *info = g_variant_ref(r->info);
  /* Never destroy the dialog from inside its decision callback. */
  grotto_approval_controller_decide_async(s, id, allow, remember, ttl, NULL, review_decided, info);
}
static void review_closed(AdwDialog *dialog, gpointer data) {
  GrottoApprovalController *s = data;
  if (s->dialog != GNOSTR_APPROVAL_DIALOG(dialog))
    return;
  g_clear_object(&s->dialog);
  g_clear_pointer(&s->review_id, g_free);
  retire_review_window(s);
}
static void batch_decided(GObject *source, GAsyncResult *result, gpointer data) {
  (void)data;
  GrottoApprovalController *s = GROTTO_APPROVAL_CONTROLLER(source);
  g_autoptr(GError) error = NULL;
  if (!grotto_approval_controller_decide_finish(s, result, &error) && !s->stopped)
    message(s, "A selected request could not be completed. Unselected requests remain pending.");
}
static void review_selected(GnostrApprovalDialog *dialog, gchar **ids, gpointer data) {
  (void)dialog;
  GrottoApprovalController *s = data;
  GrottoApprovalRequest *anchor =
      s->review_id ? g_hash_table_lookup(s->requests, s->review_id) : NULL;
  if (!anchor || !*text_field(anchor->info, "flow_id"))
    return;
  g_autoptr(GVariant) info = g_variant_ref(anchor->info);
  for (guint i = 0; ids[i]; i++) {
    GrottoApprovalRequest *r = g_hash_table_lookup(s->requests, ids[i]);
    if (r && r->state == GROTTO_APPROVAL_READY && same_group(info, r->info))
      grotto_approval_controller_decide_async(s, r->id, TRUE, FALSE, 0, NULL, batch_decided, NULL);
  }
}
static void review_deferred(GnostrApprovalDialog *dialog, gpointer data) {
  (void)dialog;
  GrottoApprovalController *s = data;
  GrottoApprovalRequest *r = s->review_id ? g_hash_table_lookup(s->requests, s->review_id) : NULL;
  if (r) {
    r->deferred = TRUE;
    g_autofree gchar *id = notification_id(r->id);
    withdraw(s, id);
    update_notifications(s);
  }
}
static void refresh_navigation(GListModel *model, guint position, guint removed, guint added,
                               gpointer data) {
  (void)position;
  (void)removed;
  (void)added;
  GrottoApprovalController *s = data;
  GrottoApprovalRequest *current =
      s->review_id ? g_hash_table_lookup(s->requests, s->review_id) : NULL;
  if (!s->dialog || !current)
    return;
  guint count = 0, current_position = 1;
  for (guint i = 0; i < g_list_model_get_n_items(model); i++) {
    g_autoptr(GrottoApprovalRequest) r = g_list_model_get_item(model, i);
    if (r->state == GROTTO_APPROVAL_READY && same_group(current->info, r->info)) {
      count++;
      if (r == current)
        current_position = count;
    }
  }
  /* Navigation is live; the displayed batch/checked-ID set stays frozen. */
  gnostr_approval_dialog_set_position(s->dialog, current_position, MAX(1u, count));
}
static void review_step(GnostrApprovalDialog *dialog, int delta, gpointer data) {
  (void)dialog;
  GrottoApprovalController *s = data;
  GrottoApprovalRequest *current = g_hash_table_lookup(s->requests, s->review_id);
  if (!current)
    return;
  g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
  guint position = 0;
  for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(s->pending)); i++) {
    g_autoptr(GrottoApprovalRequest) r = g_list_model_get_item(G_LIST_MODEL(s->pending), i);
    if (r->state == GROTTO_APPROVAL_READY && same_group(current->info, r->info)) {
      if (r == current)
        position = ids->len;
      g_ptr_array_add(ids, g_strdup(r->id));
    }
  }
  gint next = (gint)position + delta;
  if (next >= 0 && next < (gint)ids->len)
    grotto_approval_controller_review(s, ids->pdata[next]);
}
static void tier_done(GObject *source, GAsyncResult *result, gpointer data) {
  Fetch *f = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (fetch_current(f)) {
    gboolean pairing = f->request && !strcmp(text_field(f->request->info, "operation"), "connect");
    message(f->self, reply ? (pairing ? "Trust choice saved. A new client still needs pairing approval. Remembered permissions are unchanged."
                                      : "Trust level changed. Remembered permissions are unchanged.")
                           : "Trust level could not be changed. No request was approved.");
  }
  fetch_free(f);
}
static void change_tier(GnostrApprovalDialog *dialog, const char *tier, gpointer data) {
  (void)dialog;
  GrottoApprovalController *s = data;
  GrottoApprovalRequest *r = s->review_id ? g_hash_table_lookup(s->requests, s->review_id) : NULL;
  if (!r || !s->compatible || r->state != GROTTO_APPROVAL_READY)
    return;
  Fetch *f = fetch_new(s, r);
  f->held = TRUE;
  hold(s);
  g_dbus_connection_call(s->bus, s->owner, CONTROL_PATH, CONTROL, "SetClientTrust",
                         g_variant_new("(ss)", text_field(r->info, "principal"), tier),
                         G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NONE, 5000, s->cancel, tier_done,
                         f);
}
void grotto_approval_controller_review(GrottoApprovalController *s, const char *id) {
  if (s->stopped)
    return;
  if ((!s->owner && !s->bootstrap_held) || (s->owner && !s->compatible && !s->checking_owner)) {
    message(s, id && *id ? "That request is no longer waiting."
                         : "Signer unavailable. Requests could not be listed.");
    return;
  }
  if (!id || !*id) {
    g_free(s->wanted_review);
    s->wanted_review = g_strdup("");
    grotto_approval_controller_reconcile(s);
    return;
  }
  GrottoApprovalRequest *r = g_hash_table_lookup(s->requests, id);
  if (!r) {
    g_free(s->wanted_review);
    s->wanted_review = g_strdup(id);
    grotto_approval_controller_reconcile(s);
    return;
  }
  if (r->state == GROTTO_APPROVAL_FETCHING) {
    g_free(s->wanted_review);
    s->wanted_review = g_strdup(id);
    return;
  }
  if (r->state != GROTTO_APPROVAL_READY) {
    message(s, "This request is not available for review.");
    return;
  }
  g_autoptr(GApplication) app = g_weak_ref_get(&s->app);
  if (!app || !GTK_IS_APPLICATION(app))
    return;
  close_review(s);
  GtkWidget *win = adw_window_new();
  gtk_window_set_application(GTK_WINDOW(win), GTK_APPLICATION(app));
  gtk_window_set_title(GTK_WINDOW(win), "Grotto · Review request");
  gtk_window_set_default_size(GTK_WINDOW(win), 480, 640);
  g_weak_ref_set(&s->review_window, win);
  gtk_window_present(GTK_WINDOW(win));
  s->review_id = g_strdup(id);
  s->dialog = gnostr_approval_dialog_new();
  g_object_ref_sink(s->dialog);
  gnostr_approval_dialog_set_info(s->dialog, r->info, client_name(s, r->info));
  gnostr_approval_dialog_set_trust_level(
      s->dialog, g_hash_table_lookup(s->tiers, text_field(r->info, "principal")));
  guint count = 0, position = 1;
  GVariantBuilder batch;
  g_variant_builder_init(&batch, G_VARIANT_TYPE("aa{sv}"));
  for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(s->pending)); i++) {
    g_autoptr(GrottoApprovalRequest) other = g_list_model_get_item(G_LIST_MODEL(s->pending), i);
    if (other->state == GROTTO_APPROVAL_READY && same_group(r->info, other->info)) {
      count++;
      if (other == r)
        position = count;
      if (*text_field(r->info, "flow_id"))
        g_variant_builder_add_value(&batch, other->info);
    }
  }
  g_autoptr(GVariant) batch_rows = g_variant_ref_sink(g_variant_builder_end(&batch));
  gnostr_approval_dialog_set_batch(s->dialog, batch_rows);
  gnostr_approval_dialog_set_position(s->dialog, position, MAX(1u, count));
  gnostr_approval_dialog_set_callback(s->dialog, review_decision, s);
  g_signal_connect(s->dialog, "closed", G_CALLBACK(review_closed), s);
  g_signal_connect(s->dialog, "approve-selected", G_CALLBACK(review_selected), s);
  g_signal_connect(s->dialog, "deferred", G_CALLBACK(review_deferred), s);
  g_signal_connect(s->dialog, "step", G_CALLBACK(review_step), s);
  g_signal_connect(s->dialog, "change-trust-level", G_CALLBACK(change_tier), s);
  adw_dialog_present(ADW_DIALOG(s->dialog), win);
  grotto_approval_controller_reconcile(s);
}

/* The pending group can be hosted by G10's Home without owning request state. */
static void row_activated(AdwActionRow *row, gpointer data) {
  grotto_approval_controller_review(data, g_object_get_data(G_OBJECT(row), "request-id"));
}
static void group_notifications_changed(GrottoApprovalController *s, gpointer data) {
  adw_preferences_group_set_description(
      ADW_PREFERENCES_GROUP(data),
      s->notifications ? NULL : "Notifications unavailable. Requests remain here for review.");
}
static void group_update(GListModel *model, guint position, guint removed, guint added,
                         gpointer data) {
  (void)position;
  (void)removed;
  (void)added;
  AdwPreferencesGroup *group = data;
  GrottoApprovalController *s = g_object_get_data(G_OBJECT(group), "controller");
  GPtrArray *rows = g_object_get_data(G_OBJECT(group), "rows");
  for (guint i = 0; i < rows->len; i++)
    adw_preferences_group_remove(group, rows->pdata[i]);
  g_ptr_array_set_size(rows, 0);
  for (guint i = 0; i < g_list_model_get_n_items(model); i++) {
    g_autoptr(GrottoApprovalRequest) r = g_list_model_get_item(model, i);
    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), client_name(s, r->info));
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    g_autofree gchar *operation = operation_label(r->info);
    adw_action_row_set_subtitle(row, r->state == GROTTO_APPROVAL_READY
                                         ? operation
                                         : "Request details unavailable or loading");
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    g_object_set_data_full(G_OBJECT(row), "request-id", g_strdup(r->id), g_free);
    g_signal_connect_object(row, "activated", G_CALLBACK(row_activated), s, 0);
    adw_preferences_group_add(group, GTK_WIDGET(row));
    g_ptr_array_add(rows, row);
  }
  gtk_widget_set_visible(GTK_WIDGET(group), rows->len > 0);
  group_notifications_changed(s, group);
}
GtkWidget *grotto_approval_pending_group_new(GrottoApprovalController *s) {
  GtkWidget *group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), "Pending requests");
  g_object_set_data_full(G_OBJECT(group), "controller", g_object_ref(s), g_object_unref);
  g_object_set_data_full(G_OBJECT(group), "rows", g_ptr_array_new(),
                         (GDestroyNotify)g_ptr_array_unref);
  g_signal_connect_object(s->pending, "items-changed", G_CALLBACK(group_update), group, 0);
  g_signal_connect_object(s, "notifications-changed", G_CALLBACK(group_notifications_changed),
                          group, 0);
  group_update(G_LIST_MODEL(s->pending), 0, 0, 0, group);
  return group;
}
GListModel *grotto_approval_controller_get_pending(GrottoApprovalController *s) {
  return G_LIST_MODEL(s->pending);
}
guint grotto_approval_controller_get_holds(GrottoApprovalController *s) {
  return s->holds;
}
gboolean grotto_approval_controller_notifications_available(GrottoApprovalController *s) {
  return s->notifications;
}
void grotto_approval_controller_stop(GrottoApprovalController *s) {
  if (s->stopped)
    return;
  s->stopped = TRUE;
  if (s->watch) {
    g_bus_unwatch_name(s->watch);
    s->watch = 0;
  }
  if (s->lock_watch) {
    g_bus_unwatch_name(s->lock_watch);
    s->lock_watch = 0;
  }
  for (guint i = 0; i < G_N_ELEMENTS(s->notification_watch); i++)
    if (s->notification_watch[i]) {
      g_bus_unwatch_name(s->notification_watch[i]);
      s->notification_watch[i] = 0;
    }
  for (guint i = 0; i < G_N_ELEMENTS(s->subscriptions); i++)
    if (s->subscriptions[i]) {
      g_dbus_connection_signal_unsubscribe(s->bus, s->subscriptions[i]);
      s->subscriptions[i] = 0;
    }
  if (s->summary_timer) {
    g_source_remove(s->summary_timer);
    s->summary_timer = 0;
  }
  reset_owner(s);
  g_cancellable_cancel(s->cancel);
  release_bootstrap(s);
  while (s->holds)
    release(s);
}
static void grotto_approval_controller_dispose(GObject *object) {
  GrottoApprovalController *s = GROTTO_APPROVAL_CONTROLLER(object);
  grotto_approval_controller_stop(s);
  g_clear_object(&s->pending);
  g_clear_object(&s->bus);
  g_clear_object(&s->cancel);
  G_OBJECT_CLASS(grotto_approval_controller_parent_class)->dispose(object);
}
static void grotto_approval_controller_finalize(GObject *object) {
  GrottoApprovalController *s = GROTTO_APPROVAL_CONTROLLER(object);
  g_hash_table_unref(s->requests);
  g_hash_table_unref(s->tombstones);
  g_hash_table_unref(s->labels);
  g_hash_table_unref(s->tiers);
  g_free(s->wanted_review);
  g_weak_ref_clear(&s->app);
  g_weak_ref_clear(&s->review_window);
  G_OBJECT_CLASS(grotto_approval_controller_parent_class)->finalize(object);
}
static void grotto_approval_controller_class_init(GrottoApprovalControllerClass *klass) {
  /* Only positively observed lock state is exported, not fail-closed notification fallback. */
  g_signal_new("screen-lock-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
               NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
  GObjectClass *o = G_OBJECT_CLASS(klass);
  o->dispose = grotto_approval_controller_dispose;
  o->finalize = grotto_approval_controller_finalize;
  signals[RECONCILED] = g_signal_new("reconciled", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                                     NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
  signals[MESSAGE] = g_signal_new("message", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                  NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[AVAILABILITY_CHANGED] =
      g_signal_new("availability-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
                   NULL, NULL, G_TYPE_NONE, 0);
  signals[NOTIFICATIONS_CHANGED] =
      g_signal_new("notifications-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
                   NULL, NULL, G_TYPE_NONE, 0);
}
static void grotto_approval_controller_init(GrottoApprovalController *s) {
  g_weak_ref_init(&s->app, NULL);
  g_weak_ref_init(&s->review_window, NULL);
  s->locked = TRUE;
  s->pending = g_list_store_new(GROTTO_TYPE_APPROVAL_REQUEST);
  g_signal_connect_object(s->pending, "items-changed", G_CALLBACK(refresh_navigation), s, 0);
  s->requests = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  s->tombstones = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  s->labels = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  s->tiers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  s->cancel = g_cancellable_new();
}
GrottoApprovalController *grotto_approval_controller_new(GApplication *app, GDBusConnection *bus) {
  GrottoApprovalController *s = g_object_new(GROTTO_TYPE_APPROVAL_CONTROLLER, NULL);
  g_weak_ref_set(&s->app, app);
  s->bus = g_object_ref(bus);
  s->bootstrap_held = TRUE;
  hold(s);
  s->subscriptions[0] =
      g_dbus_connection_signal_subscribe(bus, SIGNER, SIGNER, NULL, SIGNER_PATH, NULL,
                                         G_DBUS_SIGNAL_FLAGS_NONE, signal_received, s, NULL);
  s->subscriptions[1] =
      g_dbus_connection_signal_subscribe(bus, SIGNER, CONTROL, NULL, CONTROL_PATH, NULL,
                                         G_DBUS_SIGNAL_FLAGS_NONE, signal_received, s, NULL);
  s->subscriptions[2] = g_dbus_connection_signal_subscribe(
      bus, "org.gnome.ScreenSaver", "org.gnome.ScreenSaver", "ActiveChanged",
      "/org/gnome/ScreenSaver", NULL, G_DBUS_SIGNAL_FLAGS_NONE, lock_changed, s, NULL);
  s->lock_watch =
      g_bus_watch_name_on_connection(bus, "org.gnome.ScreenSaver", G_BUS_NAME_WATCHER_FLAGS_NONE,
                                     lock_appeared, lock_vanished, s, NULL);
  s->notification_watch[0] = g_bus_watch_name_on_connection(
      bus, "org.freedesktop.Notifications", G_BUS_NAME_WATCHER_FLAGS_NONE, notification_appeared,
      notification_vanished, s, NULL);
  s->watch = g_bus_watch_name_on_connection(bus, SIGNER, G_BUS_NAME_WATCHER_FLAGS_NONE, appeared,
                                            vanished, s, NULL);
  return s;
}
