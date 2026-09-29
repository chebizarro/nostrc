#include "gh-requests-view.h"

#include "gh-recipient.h"

#include <glib/gi18n.h>

struct _GhRequestsView {
  AdwBin parent_instance;
  GtkStack *stack;
  AdwAvatar *avatar;
  GtkLabel *title_label;
  GtkLabel *npub_label;
  GtkLabel *count_label;
  AdwPreferencesGroup *message_group;
  GtkLabel *subject_label;
  GtkLabel *message_label;
  GtkButton *block_button;
  GtkButton *delete_button;
  GtkButton *accept_button;
  AdwAlertDialog *delete_dialog;
  AdwAlertDialog *block_dialog;
  GhConversation *request;
  GhRequestsBackend backend;
  gboolean has_backend;
  gpointer backend_data;
  GDestroyNotify backend_destroy;
  AdwAlertDialog *confirmation;  /* delete_dialog or block_dialog while shown */
  GhConversation *confirm_request; /* the request it was shown for */
};

enum { PROP_0, PROP_REQUEST, N_PROPS };
static GParamSpec *props[N_PROPS];

enum { SIGNAL_ACCEPTED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhRequestsView, gh_requests_view, ADW_TYPE_BIN)

static void confirmation_dismiss(GhRequestsView *self);

/* ---- feedback ----------------------------------------------------------------- */

static void
tell(GhRequestsView *self, const gchar *text, GtkAccessibleAnnouncementPriority priority)
{
  gtk_accessible_announce(GTK_ACCESSIBLE(self), text, priority);
  GtkWidget *overlay = gtk_widget_get_ancestor(GTK_WIDGET(self), ADW_TYPE_TOAST_OVERLAY);
  if (overlay)
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(overlay), adw_toast_new(text));
}

/* ---- the shown request ----------------------------------------------------------- */

/* The sender's npub(s), one per line, in groups of four. */
static gchar *
grouped_npubs(GhConversation *request)
{
  g_autoptr(GString) text = g_string_new(NULL);
  for (const gchar *const *peer = gh_conversation_get_peers(request); peer && *peer; peer++) {
    g_autofree gchar *grouped = gh_recipient_npub_grouped(*peer);
    if (text->len)
      g_string_append_c(text, '\n');
    g_string_append(text, grouped ? grouped : *peer);
  }
  return g_string_free(g_steal_pointer(&text), FALSE);
}

static gchar *
bounded(const gchar *body)
{
  if (!body || !g_utf8_validate(body, -1, NULL))
    return g_strdup("");
  if (g_utf8_strlen(body, -1) <= GH_REQUESTS_VIEW_MAX_PREVIEW)
    return g_strdup(body);
  g_autofree gchar *cut = g_utf8_substring(body, 0, GH_REQUESTS_VIEW_MAX_PREVIEW);
  return g_strconcat(cut, "…", NULL);
}

static void
update_actions(GhRequestsView *self)
{
  gboolean shown = self->request != NULL;
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "requests.accept", shown);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "requests.delete", shown && self->has_backend);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "requests.block", shown && self->has_backend);
  /* Every unavailable control says why, in place (charter §7.1). */
  const gchar *why = _("Needs private message storage on this device");
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->delete_button),
                              self->has_backend ? _("Remove this request from this device") : why);
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->block_button),
                              self->has_backend ? _("Stop showing this conversation") : why);
}

static void
update(GhRequestsView *self)
{
  update_actions(self);
  if (!self->request) {
    gtk_stack_set_visible_child_name(self->stack, "none");
    return;
  }
  GhConversation *request = self->request;
  const gchar *title = gh_conversation_get_title(request);
  gtk_label_set_text(self->title_label, title);
  adw_avatar_set_text(self->avatar, title);
  g_autofree gchar *npubs = grouped_npubs(request);
  gtk_label_set_text(self->npub_label, npubs);

  guint n = g_list_model_get_n_items(G_LIST_MODEL(request));
  gboolean older = gh_conversation_get_has_older(request);
  g_autofree gchar *count = older
    ? g_strdup_printf(ngettext("More than %u message", "More than %u messages", n), n)
    : g_strdup_printf(ngettext("%u message", "%u messages", n), n);
  gtk_label_set_text(self->count_label, count);

  adw_preferences_group_set_title(self->message_group,
                                  older ? _("Earliest Loaded Message") : _("First Message"));
  g_autoptr(GhMessage) first = n ? g_list_model_get_item(G_LIST_MODEL(request), 0) : NULL;
  const gchar *subject = first ? gh_message_get_subject(first) : NULL;
  gtk_widget_set_visible(GTK_WIDGET(self->subject_label), subject && *subject);
  gtk_label_set_text(self->subject_label, subject ? subject : "");
  g_autofree gchar *body = bounded(first ? gh_message_get_content(first) : NULL);
  gtk_label_set_text(self->message_label, body);
  gtk_stack_set_visible_child_name(self->stack, "request");
}

static void
on_request_changed(GhRequestsView *self)
{
  /* Accepted elsewhere (a reply, the conversation list): no longer shown. */
  if (self->request && !gh_conversation_get_is_request(self->request))
    gh_requests_view_set_request(self, NULL);
  else
    update(self);
}

void
gh_requests_view_set_request(GhRequestsView *self, GhConversation *request)
{
  g_return_if_fail(GH_IS_REQUESTS_VIEW(self));
  g_return_if_fail(!request || GH_IS_CONVERSATION(request));
  if (request && !gh_conversation_get_is_request(request))
    request = NULL;
  if (request == self->request)
    return;
  if (self->request)
    g_signal_handlers_disconnect_by_data(self->request, self);
  g_set_object(&self->request, request);
  if (request) {
    g_signal_connect_object(request, "notify::is-request", G_CALLBACK(on_request_changed), self,
                            G_CONNECT_SWAPPED);
    g_signal_connect_object(request, "notify::title", G_CALLBACK(update), self,
                            G_CONNECT_SWAPPED);
    g_signal_connect_object(request, "items-changed", G_CALLBACK(update), self,
                            G_CONNECT_SWAPPED);
  }
  /* A confirmation for the previous request must not act on another. */
  confirmation_dismiss(self);
  update(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_REQUEST]);
}

GhConversation *
gh_requests_view_get_request(GhRequestsView *self)
{
  g_return_val_if_fail(GH_IS_REQUESTS_VIEW(self), NULL);
  return self->request;
}

void
gh_requests_view_set_backend(GhRequestsView *self, const GhRequestsBackend *backend,
                             gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail(GH_IS_REQUESTS_VIEW(self));
  g_return_if_fail(!backend || (backend->forget && backend->block));
  if (self->backend_destroy)
    self->backend_destroy(self->backend_data);
  self->has_backend = backend != NULL;
  self->backend = backend ? *backend : (GhRequestsBackend){ 0 };
  self->backend_data = backend ? data : NULL;
  self->backend_destroy = backend ? destroy : NULL;
  if (!backend && destroy)
    destroy(data);
  update_actions(self);
}

AdwAlertDialog *
gh_requests_view_get_confirmation(GhRequestsView *self)
{
  g_return_val_if_fail(GH_IS_REQUESTS_VIEW(self), NULL);
  return self->confirmation;
}

/* ---- actions ------------------------------------------------------------------ */

static void
on_accept(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  GhRequestsView *self = GH_REQUESTS_VIEW(widget);
  (void)action;
  (void)parameter;
  if (!self->request)
    return;
  g_autoptr(GhConversation) request = g_object_ref(self->request);
  /* Local only (charter P8); the directory may now look the sender up. */
  gh_conversation_accept(request);
  tell(self, _("Request accepted"), GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
  g_signal_emit(self, signals[SIGNAL_ACCEPTED], 0, request);
}

/* The confirmation (gh-requests-view.blp) is no longer shown. */
static void
confirmation_done(GhRequestsView *self)
{
  self->confirmation = NULL;
  g_clear_object(&self->confirm_request);
}

static void
on_confirmed(AdwAlertDialog *dialog, const gchar *response, GhRequestsView *self)
{
  if (dialog != self->confirmation)
    return;
  /* The dialog is done (it closes itself after answering). */
  g_autoptr(GhConversation) request = g_steal_pointer(&self->confirm_request);
  confirmation_done(self);
  const gboolean deleting = dialog == self->delete_dialog;
  if (g_strcmp0(response, deleting ? "delete-confirm" : "block-confirm") != 0 ||
      !self->has_backend || !request || !gh_conversation_get_is_request(request))
    return;
  g_autoptr(GError) error = NULL;
  gboolean ok = deleting ? self->backend.forget(self->backend_data, request, &error)
                         : self->backend.block(self->backend_data, request, &error);
  if (!ok) {
    g_message("Groundhog could not %s a message request: %s", deleting ? "delete" : "block",
              error ? error->message : "unknown error");
    tell(self, deleting ? _("Couldn't delete the request") : _("Couldn't block the conversation"),
         GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_HIGH);
    return;
  }
  if (self->request == request)
    gh_requests_view_set_request(self, NULL);
  tell(self, deleting ? _("Request deleted") : _("Conversation blocked"),
       GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
}

/* Closes a shown confirmation without acting on it. */
static void
confirmation_dismiss(GhRequestsView *self)
{
  AdwAlertDialog *shown = self->confirmation;
  confirmation_done(self);
  if (shown)
    adw_dialog_force_close(ADW_DIALOG(shown));
}

static void
confirm(GhRequestsView *self, AdwAlertDialog *dialog)
{
  if (!self->request || !self->has_backend)
    return;
  confirmation_dismiss(self);
  if (dialog == self->block_dialog) {
    g_autofree gchar *heading = g_strdup_printf(_("Block %s?"),
                                                gh_conversation_get_title(self->request));
    adw_alert_dialog_set_heading(dialog, heading);
  }
  self->confirmation = dialog;
  g_set_object(&self->confirm_request, self->request);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));
}

static void
on_delete(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  GhRequestsView *self = GH_REQUESTS_VIEW(widget);
  confirm(self, self->delete_dialog);
}

static void
on_block(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  GhRequestsView *self = GH_REQUESTS_VIEW(widget);
  confirm(self, self->block_dialog);
}

/* ---- GObject ------------------------------------------------------------------ */

GtkWidget *
gh_requests_view_new(void)
{
  return g_object_new(GH_TYPE_REQUESTS_VIEW, NULL);
}

static void
gh_requests_view_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  if (id == PROP_REQUEST)
    g_value_set_object(value, GH_REQUESTS_VIEW(object)->request);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
gh_requests_view_set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  if (id == PROP_REQUEST)
    gh_requests_view_set_request(GH_REQUESTS_VIEW(object), g_value_get_object(value));
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
gh_requests_view_dispose(GObject *object)
{
  GhRequestsView *self = GH_REQUESTS_VIEW(object);
  if (self->delete_dialog)
    g_signal_handlers_disconnect_by_data(self->delete_dialog, self);
  if (self->block_dialog)
    g_signal_handlers_disconnect_by_data(self->block_dialog, self);
  confirmation_dismiss(self);
  if (self->request)
    g_signal_handlers_disconnect_by_data(self->request, self);
  g_clear_object(&self->request);
  gh_requests_view_set_backend(self, NULL, NULL, NULL);
  /* stack is both the bin's child and a bound template child: detach it
   * here, or dispose_template would unparent it behind AdwBin's back. */
  adw_bin_set_child(ADW_BIN(self), NULL);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_REQUESTS_VIEW);
  G_OBJECT_CLASS(gh_requests_view_parent_class)->dispose(object);
}

static void
gh_requests_view_class_init(GhRequestsViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  object_class->get_property = gh_requests_view_get_property;
  object_class->set_property = gh_requests_view_set_property;
  object_class->dispose = gh_requests_view_dispose;
  props[PROP_REQUEST] = g_param_spec_object("request", NULL, NULL, GH_TYPE_CONVERSATION,
    G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, N_PROPS, props);
  signals[SIGNAL_ACCEPTED] =
    g_signal_new("accepted", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, GH_TYPE_CONVERSATION);

  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-requests-view.ui");
  gtk_widget_class_bind_template_child(widget_class, GhRequestsView, stack);
  gtk_widget_class_bind_template_child(widget_class, GhRequestsView, avatar);
  gtk_widget_class_bind_template_child(widget_class, GhRequestsView, title_label);
  gtk_widget_class_bind_template_child(widget_class, GhRequestsView, npub_label);
  gtk_widget_class_bind_template_child(widget_class, GhRequestsView, count_label);
  gtk_widget_class_bind_template_child(widget_class, GhRequestsView, message_group);
  gtk_widget_class_bind_template_child(widget_class, GhRequestsView, subject_label);
  gtk_widget_class_bind_template_child(widget_class, GhRequestsView, message_label);
  gtk_widget_class_bind_template_child(widget_class, GhRequestsView, block_button);
  gtk_widget_class_bind_template_child(widget_class, GhRequestsView, delete_button);
  gtk_widget_class_bind_template_child(widget_class, GhRequestsView, accept_button);
  gtk_widget_class_bind_template_child(widget_class, GhRequestsView, delete_dialog);
  gtk_widget_class_bind_template_child(widget_class, GhRequestsView, block_dialog);
  gtk_widget_class_install_action(widget_class, "requests.accept", NULL, on_accept);
  gtk_widget_class_install_action(widget_class, "requests.delete", NULL, on_delete);
  gtk_widget_class_install_action(widget_class, "requests.block", NULL, on_block);
}

static void
gh_requests_view_init(GhRequestsView *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  AdwAlertDialog *dialogs[] = { self->delete_dialog, self->block_dialog };
  for (guint i = 0; i < G_N_ELEMENTS(dialogs); i++)
    g_signal_connect(dialogs[i], "response", G_CALLBACK(on_confirmed), self);
  update(self);
}
