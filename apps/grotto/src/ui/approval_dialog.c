#include "approval_dialog.h"
#include "nostr-kinds.h"
#include <string.h>

struct _GnostrApprovalDialog {
  AdwDialog parent_instance;
  GtkImage *header_icon;
  GtkLabel *header_title, *subtitle_label, *label_from, *label_event_kind;
  GtkLabel *label_identity, *label_reason, *label_expiry, *label_scope, *label_details,
      *label_timestamp;
  GtkLabel *content_preview, *label_position;
  GtkFrame *content_frame;
  GtkExpander *details_expander, *batch_expander;
  GtkBox *batch_box;
  GtkButton *btn_approve_selected;
  GtkBox *stepper;
  GtkCheckButton *chk_remember;
  GtkDropDown *ttl_dropdown, *trust_dropdown;
  GtkButton *btn_deny, *btn_approve, *btn_not_now, *btn_previous, *btn_next, *btn_change_trust;
  GnostrApprovalCallback callback;
  gpointer user_data;
  gchar *identity, *client_pubkey;
  gboolean finished, confirming, ready;
  guint position, count;
};
G_DEFINE_FINAL_TYPE(GnostrApprovalDialog, gnostr_approval_dialog, ADW_TYPE_DIALOG)
enum { DEFERRED, STEP, CHANGE_TRUST, APPROVE_SELECTED, N_SIGNALS };
static guint signals[N_SIGNALS];

const char *gnostr_approval_kind_name(guint32 kind) {
  switch (kind) {
  case NOSTR_KIND_PROFILE_METADATA:
    return "Profile metadata";
  case NOSTR_KIND_TEXT_NOTE:
    return "Text note";
  case NOSTR_KIND_FOLLOW_LIST:
    return "Contact list";
  case NOSTR_KIND_ENCRYPTED_DIRECT_MESSAGE:
    return "Encrypted message";
  case NOSTR_KIND_DELETION:
    return "Delete events";
  case NOSTR_KIND_REPOST:
  case NOSTR_KIND_GENERIC_REPOST:
    return "Repost";
  case NOSTR_KIND_REACTION:
    return "Reaction";
  case NOSTR_KIND_ARTICLE:
    return "Article";
  case NOSTR_KIND_COMMENT:
    return "Comment";
  case NOSTR_KIND_SEAL:
  case NOSTR_KIND_GIFT_WRAP:
    return "Encrypted message";
  case NOSTR_KIND_DIRECT_MESSAGE:
  case NOSTR_KIND_FILE_MESSAGE:
    return "Private message";
  case NOSTR_KIND_RELAY_LIST_METADATA:
    return "Relay list";
  case NOSTR_KIND_CLIENT_AUTHENTICATION:
    return "Relay authentication";
  case NOSTR_KIND_ZAP_REQUEST:
    return "Zap request";
  case NOSTR_KIND_ZAP:
    return "Zap receipt";
  case NOSTR_KIND_NOSTR_CONNECT:
    return "Remote signer message";
  default:
    return "Custom event";
  }
}
static const char *field(GVariant *info, const char *key) {
  const char *s = "";
  if (info)
    g_variant_lookup(info, key, "&s", &s);
  return s;
}
static gchar *bounded(const char *text, glong max) {
  g_autofree gchar *valid = g_utf8_make_valid(text ? text : "", -1);
  glong n = g_utf8_strlen(valid, -1);
  if (n <= max)
    return g_steal_pointer(&valid);
  g_autofree gchar *head = g_utf8_substring(valid, 0, max);
  return g_strconcat(head, "…", NULL);
}
static void finish_now(GnostrApprovalDialog *self, gboolean allow, gboolean remember, guint64 ttl) {
  if (self->finished || (allow && !self->ready) ||
      !gtk_widget_get_sensitive(GTK_WIDGET(allow ? self->btn_approve : self->btn_deny)))
    return;
  self->finished = TRUE;
  GnostrApprovalCallback cb = self->callback;
  self->callback = NULL;
  if (cb)
    cb(allow, remember, self->identity, ttl, self->user_data);
  adw_dialog_close(ADW_DIALOG(self));
}
typedef struct {
  GnostrApprovalDialog *dialog;
  gboolean allow;
} Forever;
static void forever_done(GObject *source, GAsyncResult *result, gpointer data) {
  Forever *f = data;
  const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
  f->dialog->confirming = FALSE;
  if (!strcmp(response, "remember"))
    finish_now(f->dialog, f->allow, TRUE, 0);
  g_object_unref(f->dialog);
  g_free(f);
}
static void do_finish(GnostrApprovalDialog *self, gboolean allow) {
  if (self->finished || self->confirming || (allow && !self->ready) ||
      !gtk_widget_get_sensitive(GTK_WIDGET(allow ? self->btn_approve : self->btn_deny)))
    return;
  gboolean remember = gtk_check_button_get_active(self->chk_remember) &&
                      gtk_widget_get_sensitive(GTK_WIDGET(self->chk_remember));
  static const guint64 durations[] = {600, 3600, 86400, 0};
  guint index = gtk_drop_down_get_selected(self->ttl_dropdown);
  if (remember && index >= G_N_ELEMENTS(durations))
    return;
  guint64 ttl = remember ? durations[index] : 0;
  if (remember && !ttl) {
    self->confirming = TRUE;
    g_autofree gchar *body =
        g_strdup_printf("%s forever: %s. This permission does not raise the client's trust level.",
                        allow ? "Allow" : "Deny", gtk_label_get_text(self->label_scope));
    AdwDialog *alert = adw_alert_dialog_new("Remember forever?", body);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", "Cancel", "remember",
                                   "Remember forever", NULL);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(alert), "cancel");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(alert), "cancel");
    Forever *f = g_new0(Forever, 1);
    f->dialog = g_object_ref(self);
    f->allow = allow;
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(alert), GTK_WIDGET(self), NULL, forever_done, f);
    return;
  }
  finish_now(self, allow, remember, ttl);
}
static void approve_clicked(GtkButton *button, gpointer data) {
  (void)button;
  do_finish(data, TRUE);
}
static void deny_clicked(GtkButton *button, gpointer data) {
  (void)button;
  do_finish(data, FALSE);
}
static void not_now_clicked(GtkButton *button, gpointer data) {
  (void)button;
  GnostrApprovalDialog *self = data;
  if (self->finished || self->confirming)
    return;
  self->finished = TRUE;
  self->callback = NULL;
  g_signal_emit(self, signals[DEFERRED], 0);
  adw_dialog_close(ADW_DIALOG(self));
}
static void selection_toggled(GtkCheckButton *button, gpointer data) {
  (void)button;
  GnostrApprovalDialog *self = data;
  gboolean selected = FALSE;
  for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->batch_box)); child;
       child = gtk_widget_get_next_sibling(child))
    selected |= gtk_check_button_get_active(GTK_CHECK_BUTTON(child));
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_approve_selected),
                           selected && self->ready && !self->finished);
}
static void selected_clicked(GtkButton *button, gpointer data) {
  GnostrApprovalDialog *self = data;
  if (!self->ready || self->finished || self->confirming ||
      !gtk_widget_get_sensitive(GTK_WIDGET(button)))
    return;
  g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
  for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->batch_box)); child;
       child = gtk_widget_get_next_sibling(child))
    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(child)))
      g_ptr_array_add(ids, g_strdup(g_object_get_data(G_OBJECT(child), "request-id")));
  if (!ids->len)
    return;
  g_ptr_array_add(ids, NULL);
  self->finished = TRUE;
  self->callback = NULL;
  g_signal_emit(self, signals[APPROVE_SELECTED], 0, ids->pdata);
  adw_dialog_close(ADW_DIALOG(self));
}
static void previous_clicked(GtkButton *button, gpointer data) {
  (void)button;
  g_signal_emit(data, signals[STEP], 0, -1);
}
static void next_clicked(GtkButton *button, gpointer data) {
  (void)button;
  g_signal_emit(data, signals[STEP], 0, 1);
}
static void remember_toggled(GtkCheckButton *button, gpointer data) {
  GnostrApprovalDialog *self = data;
  gtk_widget_set_sensitive(GTK_WIDGET(self->ttl_dropdown), gtk_check_button_get_active(button));
}
typedef struct {
  GnostrApprovalDialog *dialog;
  const char *tier;
} TierChange;
static void tier_confirmed(GObject *source, GAsyncResult *result, gpointer data) {
  TierChange *change = data;
  GnostrApprovalDialog *self = change->dialog;
  const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
  self->confirming = FALSE;
  if (!self->finished && !strcmp(response, "change") &&
      gtk_widget_get_sensitive(GTK_WIDGET(self->btn_change_trust))) {
    g_signal_emit(self, signals[CHANGE_TRUST], 0, change->tier);
  }
  g_object_unref(self);
  g_free(change);
}
static void change_trust_clicked(GtkButton *button, gpointer data) {
  GnostrApprovalDialog *self = data;
  if (self->finished || self->confirming || !gtk_widget_get_sensitive(GTK_WIDGET(button)))
    return;
  static const char *tiers[] = {"low", "medium", "full"};
  guint index = gtk_drop_down_get_selected(self->trust_dropdown);
  if (index >= G_N_ELEMENTS(tiers))
    return;
  TierChange *change = g_new0(TierChange, 1);
  change->dialog = g_object_ref(self);
  change->tier = tiers[index];
  self->confirming = TRUE;
  g_autofree gchar *heading = g_strdup_printf("Change client to %s trust?", tiers[index]);
  AdwDialog *alert = adw_alert_dialog_new(
      heading,
      "This changes future requests immediately. Full auto-signs permitted operations except "
      "required confirmations; Medium allows public publishing. Explicit permissions remain. This "
      "does not approve this request.");
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", "Cancel", "change",
                                 "Change trust level", NULL);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(alert), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(alert), "cancel");
  adw_alert_dialog_choose(ADW_ALERT_DIALOG(alert), GTK_WIDGET(self), NULL, tier_confirmed, change);
}
static void closed(AdwDialog *dialog) {
  GnostrApprovalDialog *self = GNOSTR_APPROVAL_DIALOG(dialog);
  GnostrApprovalCallback cb = self->callback;
  self->callback = NULL;
  self->finished = TRUE;
  if (cb)
    cb(FALSE, FALSE, self->identity, 0, self->user_data);
  if (ADW_DIALOG_CLASS(gnostr_approval_dialog_parent_class)->closed)
    ADW_DIALOG_CLASS(gnostr_approval_dialog_parent_class)->closed(dialog);
}
static void finalize(GObject *object) {
  GnostrApprovalDialog *self = GNOSTR_APPROVAL_DIALOG(object);
  g_free(self->identity);
  g_free(self->client_pubkey);
  G_OBJECT_CLASS(gnostr_approval_dialog_parent_class)->finalize(object);
}
static void gnostr_approval_dialog_class_init(GnostrApprovalDialogClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = finalize;
  ADW_DIALOG_CLASS(klass)->closed = closed;
  GtkWidgetClass *w = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(w, "/org/gnostr/signer/ui/approval-dialog.ui");
#define BIND(name) gtk_widget_class_bind_template_child(w, GnostrApprovalDialog, name)
  BIND(header_icon);
  BIND(header_title);
  BIND(subtitle_label);
  BIND(label_from);
  BIND(label_event_kind);
  BIND(label_identity);
  BIND(label_reason);
  BIND(label_expiry);
  BIND(label_scope);
  BIND(label_details);
  BIND(label_timestamp);
  BIND(batch_expander);
  BIND(batch_box);
  BIND(btn_approve_selected);
  BIND(content_preview);
  BIND(label_position);
  BIND(content_frame);
  BIND(details_expander);
  BIND(stepper);
  BIND(chk_remember);
  BIND(ttl_dropdown);
  BIND(trust_dropdown);
  BIND(btn_deny);
  BIND(btn_approve);
  BIND(btn_not_now);
  BIND(btn_previous);
  BIND(btn_next);
  BIND(btn_change_trust);
#undef BIND
  signals[APPROVE_SELECTED] =
      g_signal_new("approve-selected", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                   NULL, G_TYPE_NONE, 1, G_TYPE_STRV);
  signals[DEFERRED] = g_signal_new("deferred", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                   NULL, NULL, G_TYPE_NONE, 0);
  signals[STEP] = g_signal_new("step", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                               NULL, G_TYPE_NONE, 1, G_TYPE_INT);
  signals[CHANGE_TRUST] =
      g_signal_new("change-trust-level", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                   NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}
static gboolean shortcut(GtkWidget *widget, GVariant *args, gpointer data) {
  (void)args;
  do_finish(GNOSTR_APPROVAL_DIALOG(widget), GPOINTER_TO_INT(data));
  return TRUE;
}
static void gnostr_approval_dialog_init(GnostrApprovalDialog *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  self->ready = TRUE;
  const char *durations[] = {"10 minutes", "1 hour", "24 hours", "Forever", NULL};
  g_autoptr(GtkStringList) ttl = gtk_string_list_new(durations);
  gtk_drop_down_set_model(self->ttl_dropdown, G_LIST_MODEL(ttl));
  gtk_drop_down_set_selected(self->ttl_dropdown, 0);
  const char *tiers[] = {"Low — ask for every request", "Medium — ask for sensitive operations",
                         "Full — auto-sign permitted requests", NULL};
  g_autoptr(GtkStringList) trust = gtk_string_list_new(tiers);
  gtk_drop_down_set_model(self->trust_dropdown, G_LIST_MODEL(trust));
  gtk_drop_down_set_selected(self->trust_dropdown, 0);
#define CONNECT(widget, fn) g_signal_connect(self->widget, "clicked", G_CALLBACK(fn), self)
  CONNECT(btn_approve_selected, selected_clicked);
  CONNECT(btn_approve, approve_clicked);
  CONNECT(btn_deny, deny_clicked);
  CONNECT(btn_not_now, not_now_clicked);
  CONNECT(btn_previous, previous_clicked);
  CONNECT(btn_next, next_clicked);
  CONNECT(btn_change_trust, change_trust_clicked);
#undef CONNECT
  g_signal_connect(self->chk_remember, "toggled", G_CALLBACK(remember_toggled), self);
  GtkEventController *controller = gtk_shortcut_controller_new();
  gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(controller), GTK_SHORTCUT_SCOPE_LOCAL);
  const char *triggers[] = {"<Primary>d", "<Primary>a"};
  for (guint i = 0; i < G_N_ELEMENTS(triggers); i++)
    gtk_shortcut_controller_add_shortcut(
        GTK_SHORTCUT_CONTROLLER(controller),
        gtk_shortcut_new(gtk_shortcut_trigger_parse_string(triggers[i]),
                         gtk_callback_action_new(shortcut, GUINT_TO_POINTER(i), NULL)));
  gtk_widget_add_controller(GTK_WIDGET(self), controller);
  adw_dialog_set_focus(ADW_DIALOG(self), GTK_WIDGET(self->btn_deny));
  /* No default approve widget: Enter on a dropdown or Details is not consent. */
}
GnostrApprovalDialog *gnostr_approval_dialog_new(void) {
  return g_object_new(GNOSTR_TYPE_APPROVAL_DIALOG, NULL);
}
void gnostr_approval_dialog_set_callback(GnostrApprovalDialog *self,
                                         GnostrApprovalCallback callback, gpointer data) {
  self->callback = callback;
  self->user_data = data;
}
void gnostr_approval_dialog_set_identity(GnostrApprovalDialog *self, const char *identity) {
  g_free(self->identity);
  self->identity = g_strdup(identity);
  g_autofree gchar *label = g_strdup_printf(
      "Signing as: %s",
      identity && *identity ? identity : "No identity (identity-independent operation)");
  gtk_label_set_text(self->label_identity, label);
}
void gnostr_approval_dialog_set_app_name(GnostrApprovalDialog *self, const char *name) {
  gtk_label_set_text(self->label_from, name ? name : "Unverified app");
}
void gnostr_approval_dialog_set_event_type(GnostrApprovalDialog *self, int kind) {
  g_autofree gchar *label =
      g_strdup_printf("%s (kind %d)", gnostr_approval_kind_name((guint32)kind), kind);
  gtk_label_set_text(self->label_event_kind, label);
}
void gnostr_approval_dialog_set_timestamp(GnostrApprovalDialog *self, guint64 timestamp) {
  g_autofree gchar *text = g_strdup_printf("Request time: %" G_GUINT64_FORMAT, timestamp);
  gtk_label_set_text(self->label_timestamp, text);
}
void gnostr_approval_dialog_set_content(GnostrApprovalDialog *self, const char *content) {
  g_autofree gchar *preview = bounded(content, 280);
  gtk_label_set_text(self->content_preview, *preview ? preview : "No content");
}
/* Compatibility callers cannot change the identity pinned by the daemon.
 * Account metadata is never an authorization source or a first-key fallback. */
void gnostr_approval_dialog_set_accounts(GnostrApprovalDialog *self, AccountsStore *store,
                                         const char *selected) {
  (void)store;
  gnostr_approval_dialog_set_identity(self, selected);
}
void gnostr_approval_dialog_set_client_pubkey(GnostrApprovalDialog *self, const char *key) {
  g_free(self->client_pubkey);
  self->client_pubkey = g_strdup(key);
}
void gnostr_approval_dialog_set_request(GnostrApprovalDialog *self, const char *kind,
                                        const char *app, const char *claimed, gboolean verified,
                                        const char *preview) {
  gnostr_approval_dialog_set_app_name(self, app);
  gtk_label_set_text(self->label_event_kind, kind ? kind : "Unknown request");
  g_autofree gchar *details =
      g_strdup_printf("%s%s%s", verified ? "Verified caller" : "Unverified app",
                      claimed && *claimed ? "\nClaimed name (unverified): " : "",
                      claimed && *claimed ? claimed : "");
  gtk_label_set_text(self->label_details, details);
  gnostr_approval_dialog_set_content(self, preview);
  gtk_button_set_label(self->btn_approve, "Approve");
}
void gnostr_approval_dialog_set_batch(GnostrApprovalDialog *self, GVariant *rows) {
  while (gtk_widget_get_first_child(GTK_WIDGET(self->batch_box)))
    gtk_box_remove(self->batch_box, gtk_widget_get_first_child(GTK_WIDGET(self->batch_box)));
  gtk_widget_set_visible(GTK_WIDGET(self->batch_expander), rows && g_variant_n_children(rows) > 1);
  if (!rows)
    return;
  for (gsize i = 0; i < g_variant_n_children(rows); i++) {
    g_autoptr(GVariant) row = g_variant_get_child_value(rows, i);
    g_autoptr(GVariant) summary =
        g_variant_lookup_value(row, "event_summary", G_VARIANT_TYPE_VARDICT);
    guint32 kind;
    g_autofree gchar *operation =
        g_variant_lookup(row, "event_kind", "u", &kind)
            ? g_strdup_printf("%s · %s (kind %u)", field(row, "operation"),
                              gnostr_approval_kind_name(kind), kind)
            : g_strdup(field(row, "operation"));
    g_autofree gchar *label = g_strdup_printf(
        "%s\nPeer: %s", operation, *field(summary, "peer") ? field(summary, "peer") : "None");
    GtkWidget *check = gtk_check_button_new_with_label(label);
    g_object_set_data_full(G_OBJECT(check), "request-id", g_strdup(field(row, "request_id")),
                           g_free);
    g_signal_connect(check, "toggled", G_CALLBACK(selection_toggled), self);
    gtk_box_append(self->batch_box, check);
  }
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_approve_selected), FALSE);
}
void gnostr_approval_dialog_set_trust_level(GnostrApprovalDialog *self, const char *tier) {
  guint index = g_strcmp0(tier, "full") == 0 ? 2 : g_strcmp0(tier, "medium") == 0 ? 1 : 0;
  gtk_drop_down_set_selected(self->trust_dropdown, index);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_change_trust), tier != NULL && self->ready);
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->btn_change_trust),
                              tier
                                  ? "Changes the client's policy, not this request's Remember scope"
                                  : "Available after the client's first consent");
}
void gnostr_approval_dialog_set_position(GnostrApprovalDialog *self, guint position, guint count) {
  self->position = position;
  self->count = count;
  g_autofree gchar *label = g_strdup_printf("%u of %u", position, count);
  gtk_label_set_text(self->label_position, label);
  gtk_widget_set_visible(GTK_WIDGET(self->stepper), count > 1);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_previous), position > 1);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_next), position < count);
}
gboolean gnostr_approval_info_can_approve(GVariant *info) {
  const char *op = field(info, "operation");
  if (!strcmp(op, "sign_event")) {
    guint32 kind, count;
    if (!g_variant_lookup(info, "event_kind", "u", &kind) || kind > G_MAXUINT16 ||
        !*field(info, "identity"))
      return FALSE;
    g_autoptr(GVariant) summary =
        g_variant_lookup_value(info, "event_summary", G_VARIANT_TYPE_VARDICT);
    if (kind == NOSTR_KIND_DELETION)
      return summary && g_variant_lookup(summary, "target_count", "u", &count);
    if (kind == NOSTR_KIND_CLIENT_AUTHENTICATION)
      return *field(summary, "relay") != '\0';
    if (kind == NOSTR_KIND_REPOST || kind == NOSTR_KIND_GENERIC_REPOST ||
        kind == NOSTR_KIND_REACTION)
      return *field(summary, "target") != '\0';
  }
  return TRUE;
}
void gnostr_approval_dialog_set_info(GnostrApprovalDialog *self, GVariant *info, const char *name) {
  const char *op = field(info, "operation");
  gboolean verified = FALSE, rememberable = FALSE;
  g_variant_lookup(info, "verified", "b", &verified);
  g_variant_lookup(info, "rememberable", "b", &rememberable);
  gnostr_approval_dialog_set_request(self, op, name, field(info, "claimed_app_id"), verified,
                                     field(info, "preview"));
  gnostr_approval_dialog_set_identity(self, field(info, "identity"));
  g_autofree gchar *caller =
      g_strdup_printf("From: %s\n%s%s", name, verified ? "Verified caller: " : "Unverified app",
                      verified ? field(info, "principal") : "");
  gtk_label_set_text(self->label_from, caller);
  guint32 remaining = 0;
  g_variant_lookup(info, "remaining_seconds", "u", &remaining);
  g_autofree gchar *expiry = g_strdup_printf("Expires in at most %u seconds", remaining);
  gtk_label_set_text(self->label_expiry, expiry);
  self->ready = remaining > 0 && !strcmp(field(info, "state"), "PENDING");
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_approve), self->ready);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_deny), self->ready);
  gtk_widget_set_sensitive(GTK_WIDGET(self->chk_remember), rememberable);
  gtk_check_button_set_active(self->chk_remember, FALSE);
  const char *reason = field(info, "reason");
  g_autofree gchar *reason_text = bounded(reason, 160);
  g_autofree gchar *reason_label = g_strdup_printf("%s: %s",
                                                   !strcmp(field(info, "reason_source"), "caller")
                                                       ? "Reason supplied by the application"
                                                       : "Signer explanation",
                                                   reason_text);
  gtk_label_set_text(self->label_reason, reason_label);
  gtk_widget_set_visible(GTK_WIDGET(self->label_reason), *reason != '\0');
  guint32 kind = 0;
  gboolean has_kind = g_variant_lookup(info, "event_kind", "u", &kind);
  if (has_kind && kind <= G_MAXUINT16)
    gnostr_approval_dialog_set_event_type(self, (int)kind);
  else if (!strcmp(op, "sign_event")) {
    self->ready = FALSE;
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_approve), FALSE);
    gtk_label_set_text(self->label_event_kind, "Invalid or missing event kind");
  }
  g_autofree gchar *scope =
      has_kind ? g_strdup_printf("%s · %s · %s · kind %u", name, field(info, "identity"), op, kind)
               : g_strdup_printf("%s · %s · %s", name, field(info, "identity"), op);
  if (g_str_has_suffix(op, "encrypt") || g_str_has_suffix(op, "decrypt") ||
      !strcmp(op, "nip44_conversation_key")) {
    gchar *specific = scope;
    scope = g_strconcat(specific, " · all peers", NULL);
    g_free(specific);
  }
  gtk_label_set_text(self->label_scope, scope);
  g_autoptr(GVariant) summary =
      g_variant_lookup_value(info, "event_summary", G_VARIANT_TYPE_VARDICT);
  const char *peer = field(summary, "peer");
  g_autofree gchar *preview = NULL;
  const char *impact = "Approving authorizes this operation only; it does not publish anything.";
  if (!strcmp(op, "sign_event")) {
    guint32 tags = 0, targets = 0;
    if (summary) {
      g_variant_lookup(summary, "tag_count", "u", &tags);
      g_variant_lookup(summary, "target_count", "u", &targets);
    }
    switch (kind) {
    case NOSTR_KIND_ENCRYPTED_DIRECT_MESSAGE:
    case NOSTR_KIND_SEAL:
    case NOSTR_KIND_GIFT_WRAP:
    case NOSTR_KIND_DIRECT_MESSAGE:
    case NOSTR_KIND_FILE_MESSAGE:
    case NOSTR_KIND_NOSTR_CONNECT:
      preview = g_strdup("Encrypted message — private content is not shown.");
      break;
    case NOSTR_KIND_DELETION:
      preview = g_strdup_printf(
          "Delete %u target(s). Deletion is a permanent request; copies may remain elsewhere.",
          targets);
      break;
    case NOSTR_KIND_CLIENT_AUTHENTICATION:
      preview = g_strdup_printf(
          "Relay: %s\nAuthentication exposes this identity to this exact relay.",
          *field(summary, "relay") ? field(summary, "relay") : "Not supplied by daemon");
      break;
    case NOSTR_KIND_REPOST:
    case NOSTR_KIND_GENERIC_REPOST:
    case NOSTR_KIND_REACTION:
      preview = g_strdup_printf("Target: %s\n%s",
                                *field(summary, "target") ? field(summary, "target")
                                                          : "Not supplied by daemon",
                                field(summary, "preview"));
      break;
    case NOSTR_KIND_TEXT_NOTE:
    case NOSTR_KIND_ARTICLE:
    case NOSTR_KIND_COMMENT:
      preview = bounded(
          *field(summary, "content") ? field(summary, "content") : field(summary, "preview"), 280);
      break;
    default:
      preview = g_strdup_printf("Kind %u · %u structured tags\n%s", kind, tags,
                                field(summary, "preview"));
      break;
    }
  } else if (g_str_has_suffix(op, "encrypt") || g_str_has_suffix(op, "decrypt") ||
             !strcmp(op, "nip44_conversation_key")) {
    preview = g_strdup_printf("%s\nPeer fingerprint: %s\nPrivate message bodies are not shown.", op,
                              peer);
    if (!strcmp(op, "nip44_conversation_key"))
      impact =
          "Warning: this conversation key can open every NIP-44 message exchanged with this peer.";
  } else if (!strcmp(op, "connect")) {
    preview = g_strdup_printf("Client transport-key fingerprint: %s\nClaimed name (unverified): "
                              "%s\nRequested permissions (hints only): %s\nRelays: %s",
                              field(info, "principal"), field(info, "claimed_app_id"),
                              field(summary, "permissions"), field(summary, "relays"));
    impact = "Pairing is not a signing permission. Low trust asks for every request.";
  }
  if (preview)
    gnostr_approval_dialog_set_content(self, preview);
  if (!gnostr_approval_info_can_approve(info)) {
    self->ready = FALSE;
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_approve), FALSE);
    impact = "The daemon did not supply the required preview details. Update Grotto before "
             "approving; you can still deny this request.";
  }
  gtk_label_set_text(self->subtitle_label, impact);
  g_autofree gchar *details =
      g_strdup_printf("%s\nPrincipal: %s\nTransport: %s\nClaimed name (unverified): %s",
                      verified ? "Verified caller" : "Unverified app", field(info, "principal"),
                      field(info, "transport"), field(info, "claimed_app_id"));
  gtk_label_set_text(self->label_details, details);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_change_trust),
                           verified && *field(info, "principal"));
}
GnostrApprovalDialog *gnostr_show_approval_request_dialog(GtkWidget *parent, const char *identity,
                                                          const char *kind, const char *name,
                                                          const char *claimed, gboolean verified,
                                                          const char *preview, AccountsStore *store,
                                                          GnostrApprovalCallback callback,
                                                          gpointer data) {
  (void)store;
  GnostrApprovalDialog *dialog = gnostr_approval_dialog_new();
  gnostr_approval_dialog_set_identity(dialog, identity);
  gnostr_approval_dialog_set_request(dialog, kind, name, claimed, verified, preview);
  gnostr_approval_dialog_set_callback(dialog, callback, data);
  adw_dialog_present(ADW_DIALOG(dialog), parent);
  return dialog;
}
void gnostr_show_approval_dialog_full(GtkWidget *parent, const char *identity, const char *name,
                                      const char *content, int kind, guint64 timestamp,
                                      AccountsStore *store, GnostrApprovalCallback callback,
                                      gpointer data) {
  GnostrApprovalDialog *dialog = gnostr_show_approval_request_dialog(
      parent, identity, "sign_event", name, NULL, FALSE, content, store, callback, data);
  gnostr_approval_dialog_set_event_type(dialog, kind);
  gnostr_approval_dialog_set_timestamp(dialog, timestamp);
}
void gnostr_show_approval_dialog(GtkWidget *parent, const char *identity, const char *name,
                                 const char *content, AccountsStore *store,
                                 GnostrApprovalCallback callback, gpointer data) {
  gnostr_show_approval_dialog_full(parent, identity, name, content, NOSTR_KIND_TEXT_NOTE, 0, store,
                                   callback, data);
}
gboolean gnostr_show_approval_dialog_with_session(GtkWidget *parent, const char *client,
                                                  const char *identity, const char *name,
                                                  const char *content, int kind, guint64 timestamp,
                                                  AccountsStore *store,
                                                  GnostrApprovalCallback callback, gpointer data) {
  (void)client;
  /* Legacy entry point retained, but GUI sessions never authorize signing. */
  gnostr_show_approval_dialog_full(parent, identity, name, content, kind, timestamp, store,
                                   callback, data);
  return TRUE;
}
