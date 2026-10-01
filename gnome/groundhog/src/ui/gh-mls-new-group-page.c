#include "gh-mls-new-group-page.h"

#include "gh-mls-copy.h"
#include "gh-mls-invitee-picker.h"

#include <glib/gi18n.h>

#define MAX_RELAYS 16

/* ---- GhMlsRelayRow ------------------------------------------------------------------------ */

struct _GhMlsRelayRow {
  AdwActionRow parent_instance;
  GtkWidget *remove_button;
  gchar *url;
};

enum { RELAY_SIGNAL_REMOVE, RELAY_N_SIGNALS };
static guint relay_signals[RELAY_N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhMlsRelayRow, gh_mls_relay_row, ADW_TYPE_ACTION_ROW)

static void
relay_remove(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  g_signal_emit(widget, relay_signals[RELAY_SIGNAL_REMOVE], 0);
}

static void
gh_mls_relay_row_finalize(GObject *object)
{
  g_free(GH_MLS_RELAY_ROW(object)->url);
  G_OBJECT_CLASS(gh_mls_relay_row_parent_class)->finalize(object);
}

static void
gh_mls_relay_row_class_init(GhMlsRelayRowClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  G_OBJECT_CLASS(klass)->finalize = gh_mls_relay_row_finalize;
  relay_signals[RELAY_SIGNAL_REMOVE] = g_signal_new("remove", G_TYPE_FROM_CLASS(klass),
                                                    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                                    G_TYPE_NONE, 0);
  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-mls-relay-row.ui");
  gtk_widget_class_bind_template_child(widget_class, GhMlsRelayRow, remove_button);
  gtk_widget_class_install_action(widget_class, "relay.remove", NULL, relay_remove);
}

static void
gh_mls_relay_row_init(GhMlsRelayRow *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
}

GhMlsRelayRow *
gh_mls_relay_row_new(const gchar *url, gboolean removable)
{
  GhMlsRelayRow *self = g_object_new(GH_TYPE_MLS_RELAY_ROW, NULL);
  self->url = g_strdup(url);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self), url);
  gtk_widget_set_visible(self->remove_button, removable);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "relay.remove", removable);
  if (removable) {
    g_autofree gchar *label = g_strdup_printf(_("Remove %s"), url);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->remove_button),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  }
  return self;
}

const gchar *
gh_mls_relay_row_get_url(GhMlsRelayRow *self)
{
  g_return_val_if_fail(GH_IS_MLS_RELAY_ROW(self), NULL);
  return self->url;
}

/* ---- GhMlsNewGroupPage -------------------------------------------------------------------- */

struct _GhMlsNewGroupPage {
  AdwNavigationPage parent_instance;
  GtkStack *stack;
  AdwPreferencesGroup *identity_group;
  AdwActionRow *identity_row;
  GtkSpinner *identity_spinner;
  GtkImage *identity_icon;
  GtkWidget *retry_identity_button;
  AdwEntryRow *name_row;
  AdwEntryRow *about_row;
  GhMlsInviteePicker *picker;
  AdwPreferencesGroup *relays_group;
  AdwEntryRow *relay_entry;
  GtkLabel *relay_error;
  GtkLabel *error_label;
  GtkLabel *create_reason;
  GtkLabel *format_notice;
  GtkWidget *format_choice;
  GtkImage *status_icon;
  GtkSpinner *status_spinner;
  GtkLabel *status_title;
  GtkLabel *status_description;
  GtkWidget *open_button;
  GtkWidget *back_button;

  GhMlsUiContext context;  /* service, accounts, model, settings referenced */
  GPtrArray *relay_rows;   /* GhMlsRelayRow in relays_group */
  gchar *reason;           /* why Create can't run, or NULL */
  const gchar *notice;     /* the group's format said, or NULL (static) */
  GhMlsKeyPackageFormat format; /* the format shown: what Create asks for (review M2) */
  GCancellable *creating;  /* the running create */
  GhMlsGroup *group;       /* made by Create */
  guint invited;
};

enum { SIGNAL_OPEN_GROUP, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhMlsNewGroupPage, gh_mls_new_group_page, ADW_TYPE_NAVIGATION_PAGE)

static gchar *
entry_text(AdwEntryRow *row)
{
  g_autofree gchar *text = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(row))));
  return *text ? g_steal_pointer(&text) : NULL;
}

static void
announce(GhMlsNewGroupPage *self, const gchar *text, gboolean urgent)
{
  gtk_accessible_announce(GTK_ACCESSIBLE(self), text,
                          urgent ? GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_HIGH
                                 : GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
}

static GhMlsIdentityState
identity_state(GhMlsNewGroupPage *self)
{
  return self->context.service ? gh_mls_service_get_identity_state(self->context.service)
                               : GH_MLS_IDENTITY_NONE;
}

/* Create runs only when everything is ready; otherwise the first reason why
 * is under it (charter §7.1). */
static void
sync_create(GhMlsNewGroupPage *self)
{
  GhMlsIdentityCopy identity = gh_mls_identity_copy(identity_state(self));
  g_autofree gchar *name = entry_text(self->name_row);
  const gchar *reason = NULL;
  if (!self->context.service)
    reason = _("Encrypted groups aren’t running for this account.");
  else if (!identity.ready)
    reason = _("Approve this device in Nostr Signer first.");
  else if (!name)
    reason = _("Give the group a name.");
  else if (gh_mls_invitee_picker_get_n_selected(self->picker) == 0)
    reason = gh_mls_invitee_picker_get_n_listed(self->picker) == 0
      ? _("Only accepted contacts can be invited, and you have none yet.")
      : _("Choose at least one person.");
  else if (gh_mls_invitee_picker_get_checking(self->picker))
    reason = _("Checking whether everyone you chose can join…");
  else if (!gh_mls_invitee_picker_get_ready(self->picker))
    reason = _("Someone you chose can’t be invited yet. Remove them to continue.");
  else if (self->relay_rows->len == 0)
    reason = _("Add at least one group relay.");
  /* The group's format (nostrc-lf62), as the service will choose it: the
   * newer (adopted) one unless someone chosen has only the older one. */
  gboolean legacy_only = FALSE, adopted_only = FALSE;
  g_auto(GStrv) selected = gh_mls_invitee_picker_dup_selected(self->picker);
  for (guint i = 0; selected && selected[i]; i++) {
    GhMlsInviteeState state = gh_mls_invitee_picker_get_state(self->picker, selected[i]);
    legacy_only |= !gh_mls_invitee_can_join(state, TRUE) && gh_mls_invitee_can_join(state, FALSE);
    adopted_only |= gh_mls_invitee_can_join(state, TRUE) && !gh_mls_invitee_can_join(state, FALSE);
  }
  gboolean mixed = !reason && legacy_only && adopted_only;
  if (mixed)
    reason = _("Some people you chose use an older app version that joins only older-format "
               "groups, and others an app that joins only newer-format ones. One group can’t "
               "use both formats: keep one set, and make a separate group for the others.");
  self->notice = !reason && legacy_only
    ? _("Some people use an older app version; this group will use the older format. "
        "Remove them to make a newer-format group instead.") : NULL;
  self->format = legacy_only ? GH_MLS_KEY_PACKAGE_FORMAT_LEGACY
                             : GH_MLS_KEY_PACKAGE_FORMAT_ADOPTED;
  g_free(self->reason);
  self->reason = g_strdup(reason);
  gtk_label_set_text(self->create_reason, reason ? reason : "");
  gtk_widget_set_visible(GTK_WIDGET(self->create_reason), reason != NULL);
  gtk_label_set_text(self->format_notice, self->notice ? self->notice : "");
  gtk_widget_set_visible(GTK_WIDGET(self->format_notice), self->notice != NULL);
  gtk_widget_set_visible(self->format_choice, mixed);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "mls-new.keep-adopted", mixed);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "mls-new.keep-legacy", mixed);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "mls-new.create",
                                reason == NULL && !self->creating);
}

static void
sync_identity(GhMlsNewGroupPage *self)
{
  GhMlsIdentityCopy copy = gh_mls_identity_copy(identity_state(self));
  gboolean shown = copy.title != NULL;
  /* A copy: setting the title frees the row's old string (W22 review C1). */
  g_autofree gchar *before = gtk_widget_get_visible(GTK_WIDGET(self->identity_group))
    ? g_strdup(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(self->identity_row))) : NULL;
  gtk_widget_set_visible(GTK_WIDGET(self->identity_group), shown);
  if (shown) {
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->identity_row), copy.title);
    adw_action_row_set_subtitle(self->identity_row, copy.description);
    gtk_widget_set_visible(GTK_WIDGET(self->identity_spinner), copy.busy);
    gtk_spinner_set_spinning(self->identity_spinner, copy.busy);
    gtk_widget_set_visible(GTK_WIDGET(self->identity_icon), !copy.busy);
    gtk_widget_set_visible(self->retry_identity_button, copy.can_retry);
    if (g_strcmp0(before, copy.title) != 0)
      announce(self, copy.title, copy.can_retry);
  }
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "mls-new.retry-identity", copy.can_retry);
  sync_create(self);
}

static void
show_error(GhMlsNewGroupPage *self, const gchar *message)
{
  gtk_label_set_text(self->error_label, message ? message : "");
  gtk_widget_set_visible(GTK_WIDGET(self->error_label), message != NULL);
  if (message)
    announce(self, message, TRUE);
}

static void
set_status(GhMlsNewGroupPage *self, const gchar *icon, gboolean busy, const gchar *title,
           const gchar *description, gboolean can_open, gboolean can_retry)
{
  gtk_image_set_from_icon_name(self->status_icon, icon);
  gtk_widget_set_visible(GTK_WIDGET(self->status_icon), !busy);
  gtk_widget_set_visible(GTK_WIDGET(self->status_spinner), busy);
  gtk_spinner_set_spinning(self->status_spinner, busy);
  gtk_label_set_text(self->status_title, title);
  gtk_label_set_text(self->status_description, description);
  gtk_widget_set_visible(self->open_button, can_open);
  gtk_widget_set_visible(self->back_button, can_retry);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "mls-new.open", can_open);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "mls-new.back", can_retry);
  g_autofree gchar *spoken = g_strdup_printf(_("%s. %s"), title, description);
  announce(self, spoken, can_retry);
}

/* ---- relays ----------------------------------------------------------------------------- */

static void on_relay_remove(GhMlsRelayRow *row, gpointer data);

static gboolean
has_relay(GhMlsNewGroupPage *self, const gchar *url)
{
  for (guint i = 0; i < self->relay_rows->len; i++)
    if (g_str_equal(gh_mls_relay_row_get_url(g_ptr_array_index(self->relay_rows, i)), url))
      return TRUE;
  return FALSE;
}

static void
append_relay(GhMlsNewGroupPage *self, const gchar *url)
{
  GhMlsRelayRow *row = gh_mls_relay_row_new(url, TRUE);
  g_signal_connect(row, "remove", G_CALLBACK(on_relay_remove), self);
  adw_preferences_group_add(self->relays_group, GTK_WIDGET(row));
  g_ptr_array_add(self->relay_rows, row);
}

static void
on_relay_remove(GhMlsRelayRow *row, gpointer data)
{
  GhMlsNewGroupPage *self = data;
  if (!g_ptr_array_remove(self->relay_rows, row))
    return;
  adw_preferences_group_remove(self->relays_group, GTK_WIDGET(row));
  sync_create(self);
  gtk_widget_grab_focus(GTK_WIDGET(self->relay_entry));
}

gboolean
gh_mls_new_group_page_add_relay(GhMlsNewGroupPage *self, const gchar *text)
{
  g_return_val_if_fail(GH_IS_MLS_NEW_GROUP_PAGE(self), FALSE);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *url = gh_mls_parse_relay(text, &error);
  const gchar *problem = error ? error->message : NULL;
  if (url && has_relay(self, url))
    problem = _("This relay is already in the list.");
  else if (url && self->relay_rows->len >= MAX_RELAYS)
    problem = _("A group can use up to 16 relays.");
  gtk_label_set_text(self->relay_error, problem ? problem : "");
  gtk_widget_set_visible(GTK_WIDGET(self->relay_error), problem != NULL);
  if (problem) {
    announce(self, problem, TRUE);
    return FALSE;
  }
  append_relay(self, url);
  gtk_editable_set_text(GTK_EDITABLE(self->relay_entry), "");
  sync_create(self);
  return TRUE;
}

static void
on_relay_apply(GhMlsNewGroupPage *self)
{
  gh_mls_new_group_page_add_relay(self, gtk_editable_get_text(GTK_EDITABLE(self->relay_entry)));
}

GStrv
gh_mls_new_group_page_dup_relays(GhMlsNewGroupPage *self)
{
  g_return_val_if_fail(GH_IS_MLS_NEW_GROUP_PAGE(self), NULL);
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
  for (guint i = 0; i < self->relay_rows->len; i++)
    g_strv_builder_add(builder, gh_mls_relay_row_get_url(g_ptr_array_index(self->relay_rows, i)));
  return g_strv_builder_end(builder);
}

/* ---- create ------------------------------------------------------------------------------ */

static void
created(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GhMlsNewGroupPage) self = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMlsGroup) group = gh_mls_service_create_group_finish(GH_MLS_SERVICE(source),
                                                                   result, &error);
  gboolean cancelled = self->creating && g_cancellable_is_cancelled(self->creating);
  g_clear_object(&self->creating);
  if (cancelled || gtk_widget_in_destruction(GTK_WIDGET(self)))
    return;
  if (!group) {
    g_message("Groundhog could not create an encrypted group: %s", error->message);
    g_autofree gchar *words = gh_mls_error_copy(error);
    set_status(self, "dialog-warning-symbolic", FALSE, _("Group Not Created"), words, FALSE,
               TRUE);
    /* The KeyPackages changed since the check (review M2): check again, so
     * the page shows what Create would now make. */
    if (g_error_matches(error, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_FORMAT_CHANGED))
      gh_mls_invitee_picker_check_again(self->picker);
    sync_create(self);
    return;
  }
  g_set_object(&self->group, group);
  g_autofree gchar *description = g_strdup_printf(
    g_dngettext(NULL, "The invitation is on its way to %u person. They join once they accept.",
                "Invitations are on their way to %u people. They join once they accept.",
                self->invited), self->invited);
  set_status(self, "emblem-ok-symbolic", FALSE, _("Group Created"), description, TRUE, FALSE);
  gtk_widget_grab_focus(self->open_button);
  sync_create(self);
}

/* "Keep Newer-Format People" / "Keep Older-Format People": un-chooses
 * whoever cannot join a group of the kept format (nostrc-lf62). */
static void
action_keep(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)parameter;
  GhMlsNewGroupPage *self = GH_MLS_NEW_GROUP_PAGE(widget);
  gboolean adopted = g_str_equal(action, "mls-new.keep-adopted");
  g_auto(GStrv) selected = gh_mls_invitee_picker_dup_selected(self->picker);
  guint removed = 0;
  for (guint i = 0; selected && selected[i]; i++) {
    GhMlsInviteeState state = gh_mls_invitee_picker_get_state(self->picker, selected[i]);
    if (gh_mls_invitee_can_invite(state) && !gh_mls_invitee_can_join(state, adopted) &&
        gh_mls_invitee_picker_set_selected(self->picker, selected[i], FALSE))
      removed++;
  }
  g_autofree gchar *said = g_strdup_printf(
    g_dngettext(NULL, "Removed %u person from the group.", "Removed %u people from the group.",
                removed), removed);
  announce(self, said, FALSE);
  sync_create(self);
}

static void
action_create(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  GhMlsNewGroupPage *self = GH_MLS_NEW_GROUP_PAGE(widget);
  sync_create(self);
  if (self->reason || self->creating || !self->context.service)
    return;
  g_autofree gchar *name = entry_text(self->name_row);
  g_autofree gchar *about = entry_text(self->about_row);
  g_auto(GStrv) relays = gh_mls_new_group_page_dup_relays(self);
  g_auto(GStrv) invitees = gh_mls_invitee_picker_dup_selected(self->picker);
  self->invited = g_strv_length(invitees);
  show_error(self, NULL);
  self->creating = g_cancellable_new();
  gtk_stack_set_visible_child_name(self->stack, "status");
  const gchar *description =
    _("Sending the new group to its relays. The invitations follow once a relay accepts it.");
  set_status(self, "content-loading-symbolic", TRUE, _("Creating the Group…"), description,
             FALSE, FALSE);
  sync_create(self);
  gh_mls_service_create_group_in_format_async(self->context.service, name, about,
                                              (const gchar *const *)relays,
                                              (const gchar *const *)invitees, self->format,
                                              self->creating, created, g_object_ref(self));
}

static void
close_dialog(GhMlsNewGroupPage *self)
{
  GtkWidget *dialog = gtk_widget_get_ancestor(GTK_WIDGET(self), ADW_TYPE_DIALOG);
  if (dialog)
    adw_dialog_close(ADW_DIALOG(dialog));
}

static void
action_open(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  GhMlsNewGroupPage *self = GH_MLS_NEW_GROUP_PAGE(widget);
  if (!self->group)
    return;
  g_autoptr(GhMlsGroup) group = g_object_ref(self->group);
  g_object_ref(self);
  close_dialog(self);
  g_signal_emit(self, signals[SIGNAL_OPEN_GROUP], 0, group);
  g_object_unref(self);
}

static void
action_back(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  GhMlsNewGroupPage *self = GH_MLS_NEW_GROUP_PAGE(widget);
  gtk_stack_set_visible_child_name(self->stack, "form");
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "mls-new.back", FALSE);
  gtk_widget_grab_focus(GTK_WIDGET(self->name_row));
}

static void
action_retry_identity(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  GhMlsNewGroupPage *self = GH_MLS_NEW_GROUP_PAGE(widget);
  if (!self->context.service)
    return;
  g_autoptr(GError) error = NULL;
  if (!gh_mls_service_retry_identity(self->context.service, &error)) {
    g_autofree gchar *words = gh_mls_error_copy(error);
    show_error(self, words);
  }
  sync_identity(self);
}

/* Enter in the name or description: Create when it can run (it says why
 * otherwise). */
static void
on_name_activated(GhMlsNewGroupPage *self)
{
  gtk_widget_activate_action(GTK_WIDGET(self), "mls-new.create", NULL);
}

/* ---- construction ----------------------------------------------------------------------- */

GhMlsNewGroupPage *
gh_mls_new_group_page_new(const GhMlsUiContext *context)
{
  g_return_val_if_fail(context != NULL && GH_IS_MLS_SERVICE(context->service), NULL);
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(context->accounts), NULL);
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(context->model), NULL);
  GhMlsNewGroupPage *self = g_object_new(GH_TYPE_MLS_NEW_GROUP_PAGE, NULL);
  self->context = *context;
  self->context.default_relays = NULL;
  g_object_ref(context->service);
  g_object_ref(context->accounts);
  g_object_ref(context->model);
  if (context->settings)
    g_object_ref(context->settings);
  g_signal_connect_object(context->service, "notify::identity-state",
                          G_CALLBACK(sync_identity), self, G_CONNECT_SWAPPED);
  gh_mls_invitee_picker_setup(self->picker, &self->context, NULL);
  for (guint i = 0; context->default_relays && context->default_relays[i]; i++) {
    g_autofree gchar *url = gh_mls_parse_relay(context->default_relays[i], NULL);
    if (url && !has_relay(self, url) && self->relay_rows->len < MAX_RELAYS)
      append_relay(self, url);
  }
  sync_identity(self);
  return self;
}

GhMlsInviteePicker *
gh_mls_new_group_page_get_picker(GhMlsNewGroupPage *self)
{
  g_return_val_if_fail(GH_IS_MLS_NEW_GROUP_PAGE(self), NULL);
  return self->picker;
}

void
gh_mls_new_group_page_set_name(GhMlsNewGroupPage *self, const gchar *name)
{
  g_return_if_fail(GH_IS_MLS_NEW_GROUP_PAGE(self));
  gtk_editable_set_text(GTK_EDITABLE(self->name_row), name ? name : "");
}

const gchar *
gh_mls_new_group_page_get_create_reason(GhMlsNewGroupPage *self)
{
  g_return_val_if_fail(GH_IS_MLS_NEW_GROUP_PAGE(self), NULL);
  return self->reason;
}

const gchar *
gh_mls_new_group_page_get_format_notice(GhMlsNewGroupPage *self)
{
  g_return_val_if_fail(GH_IS_MLS_NEW_GROUP_PAGE(self), NULL);
  return self->notice;
}

GhMlsKeyPackageFormat
gh_mls_new_group_page_get_format(GhMlsNewGroupPage *self)
{
  g_return_val_if_fail(GH_IS_MLS_NEW_GROUP_PAGE(self), GH_MLS_KEY_PACKAGE_FORMAT_ADOPTED);
  return self->format;
}

gboolean
gh_mls_new_group_page_get_format_choice(GhMlsNewGroupPage *self)
{
  g_return_val_if_fail(GH_IS_MLS_NEW_GROUP_PAGE(self), FALSE);
  return gtk_widget_get_visible(self->format_choice);
}

const gchar *
gh_mls_new_group_page_get_identity_title(GhMlsNewGroupPage *self)
{
  g_return_val_if_fail(GH_IS_MLS_NEW_GROUP_PAGE(self), NULL);
  return gtk_widget_get_visible(GTK_WIDGET(self->identity_group))
    ? adw_preferences_row_get_title(ADW_PREFERENCES_ROW(self->identity_row)) : NULL;
}

const gchar *
gh_mls_new_group_page_get_status_title(GhMlsNewGroupPage *self)
{
  g_return_val_if_fail(GH_IS_MLS_NEW_GROUP_PAGE(self), NULL);
  return gtk_label_get_text(self->status_title);
}

GhMlsGroup *
gh_mls_new_group_page_get_group(GhMlsNewGroupPage *self)
{
  g_return_val_if_fail(GH_IS_MLS_NEW_GROUP_PAGE(self), NULL);
  return self->group;
}

static void
gh_mls_new_group_page_dispose(GObject *object)
{
  GhMlsNewGroupPage *self = GH_MLS_NEW_GROUP_PAGE(object);
  if (self->creating) {
    g_cancellable_cancel(self->creating);
    g_clear_object(&self->creating);
  }
  g_clear_object(&self->group);
  g_clear_object(&self->context.service);
  g_clear_object(&self->context.accounts);
  g_clear_object(&self->context.model);
  g_clear_object(&self->context.settings);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_MLS_NEW_GROUP_PAGE);
  G_OBJECT_CLASS(gh_mls_new_group_page_parent_class)->dispose(object);
}

static void
gh_mls_new_group_page_finalize(GObject *object)
{
  GhMlsNewGroupPage *self = GH_MLS_NEW_GROUP_PAGE(object);
  g_ptr_array_unref(self->relay_rows);
  g_free(self->reason);
  G_OBJECT_CLASS(gh_mls_new_group_page_parent_class)->finalize(object);
}

static void
gh_mls_new_group_page_class_init(GhMlsNewGroupPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  object_class->dispose = gh_mls_new_group_page_dispose;
  object_class->finalize = gh_mls_new_group_page_finalize;
  signals[SIGNAL_OPEN_GROUP] = g_signal_new("open-group", G_TYPE_FROM_CLASS(klass),
                                            G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                            G_TYPE_NONE, 1, GH_TYPE_MLS_GROUP);
  g_type_ensure(GH_TYPE_MLS_INVITEE_PICKER);
  gtk_widget_class_set_template_from_resource(
    widget_class, "/org/nostr/Groundhog/ui/gh-mls-new-group-page.ui");
#define BIND(name) gtk_widget_class_bind_template_child(widget_class, GhMlsNewGroupPage, name)
  BIND(stack);
  BIND(identity_group);
  BIND(identity_row);
  BIND(identity_spinner);
  BIND(identity_icon);
  BIND(retry_identity_button);
  BIND(name_row);
  BIND(about_row);
  BIND(picker);
  BIND(relays_group);
  BIND(relay_entry);
  BIND(relay_error);
  BIND(error_label);
  BIND(create_reason);
  BIND(format_notice);
  BIND(format_choice);
  BIND(status_icon);
  BIND(status_spinner);
  BIND(status_title);
  BIND(status_description);
  BIND(open_button);
  BIND(back_button);
#undef BIND
  gtk_widget_class_install_action(widget_class, "mls-new.create", NULL, action_create);
  gtk_widget_class_install_action(widget_class, "mls-new.keep-adopted", NULL, action_keep);
  gtk_widget_class_install_action(widget_class, "mls-new.keep-legacy", NULL, action_keep);
  gtk_widget_class_install_action(widget_class, "mls-new.open", NULL, action_open);
  gtk_widget_class_install_action(widget_class, "mls-new.back", NULL, action_back);
  gtk_widget_class_install_action(widget_class, "mls-new.retry-identity", NULL,
                                  action_retry_identity);
}

static void
gh_mls_new_group_page_init(GhMlsNewGroupPage *self)
{
  self->relay_rows = g_ptr_array_new();
  gtk_widget_init_template(GTK_WIDGET(self));
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "mls-new.open", FALSE);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "mls-new.back", FALSE);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "mls-new.create", FALSE);
  g_signal_connect_swapped(self->picker, "changed", G_CALLBACK(sync_create), self);
  g_signal_connect_swapped(self->name_row, "changed", G_CALLBACK(sync_create), self);
  g_signal_connect_swapped(self->name_row, "entry-activated", G_CALLBACK(on_name_activated),
                           self);
  g_signal_connect_swapped(self->about_row, "entry-activated", G_CALLBACK(on_name_activated),
                           self);
  g_signal_connect_swapped(self->relay_entry, "apply", G_CALLBACK(on_relay_apply), self);
  g_signal_connect_swapped(self->relay_entry, "entry-activated", G_CALLBACK(on_relay_apply),
                           self);
}
