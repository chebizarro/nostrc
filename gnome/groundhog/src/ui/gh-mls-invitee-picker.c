#include "gh-mls-invitee-picker.h"

#include "gh-mls-copy.h"
#include "gh-recipient.h"

#include <glib/gi18n.h>

/* ---- GhMlsInviteeRow (data/ui/gh-mls-invitee-row.blp) ----------------------------------- */

#define GH_TYPE_MLS_INVITEE_ROW (gh_mls_invitee_row_get_type())
G_DECLARE_FINAL_TYPE(GhMlsInviteeRow, gh_mls_invitee_row, GH, MLS_INVITEE_ROW, AdwActionRow)

struct _GhMlsInviteeRow {
  AdwActionRow parent_instance;
  GtkCheckButton *check;
  AdwAvatar *avatar;
  GtkSpinner *spinner;
  GtkImage *state_icon;
  GtkWidget *retry_button;

  gpointer picker;          /* GhMlsInviteePicker: its group holds the row */
  gchar *pubkey;
  gchar *npub_short;
  gboolean named;          /* the title is a cached name, the npub the subtitle */
  GhMlsInviteeState state;
  GCancellable *checking;  /* the running check */
};

enum { ROW_SIGNAL_RETRY, ROW_N_SIGNALS };
static guint row_signals[ROW_N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhMlsInviteeRow, gh_mls_invitee_row, ADW_TYPE_ACTION_ROW)

static void
row_retry(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  g_signal_emit(widget, row_signals[ROW_SIGNAL_RETRY], 0);
}

static void
gh_mls_invitee_row_dispose(GObject *object)
{
  GhMlsInviteeRow *self = GH_MLS_INVITEE_ROW(object);
  if (self->checking) {
    g_cancellable_cancel(self->checking);
    g_clear_object(&self->checking);
  }
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_MLS_INVITEE_ROW);
  G_OBJECT_CLASS(gh_mls_invitee_row_parent_class)->dispose(object);
}

static void
gh_mls_invitee_row_finalize(GObject *object)
{
  GhMlsInviteeRow *self = GH_MLS_INVITEE_ROW(object);
  g_free(self->pubkey);
  g_free(self->npub_short);
  G_OBJECT_CLASS(gh_mls_invitee_row_parent_class)->finalize(object);
}

static void
gh_mls_invitee_row_class_init(GhMlsInviteeRowClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  G_OBJECT_CLASS(klass)->dispose = gh_mls_invitee_row_dispose;
  G_OBJECT_CLASS(klass)->finalize = gh_mls_invitee_row_finalize;
  row_signals[ROW_SIGNAL_RETRY] = g_signal_new("retry", G_TYPE_FROM_CLASS(klass),
                                               G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                               G_TYPE_NONE, 0);
  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-mls-invitee-row.ui");
  gtk_widget_class_bind_template_child(widget_class, GhMlsInviteeRow, check);
  gtk_widget_class_bind_template_child(widget_class, GhMlsInviteeRow, avatar);
  gtk_widget_class_bind_template_child(widget_class, GhMlsInviteeRow, spinner);
  gtk_widget_class_bind_template_child(widget_class, GhMlsInviteeRow, state_icon);
  gtk_widget_class_bind_template_child(widget_class, GhMlsInviteeRow, retry_button);
  gtk_widget_class_install_action(widget_class, "invitee.retry", NULL, row_retry);
}

static void
gh_mls_invitee_row_init(GhMlsInviteeRow *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
}

static gboolean
row_selected(GhMlsInviteeRow *row)
{
  return gtk_check_button_get_active(row->check);
}

/* The row's words and widgets for its state (or the npub while not chosen). */
static void
row_sync(GhMlsInviteeRow *row)
{
  gboolean chosen = row_selected(row);
  GhMlsInviteeState state = row->state;
  gboolean checking = chosen && state == GH_MLS_INVITEE_CHECKING;
  const gchar *words = chosen ? gh_mls_invitee_copy(state) : NULL;
  g_autofree gchar *subtitle = NULL;
  if (words && row->named)
    subtitle = g_strdup_printf(_("%s · %s"), row->npub_short, words);
  else if (words)
    subtitle = g_strdup(words);
  else
    subtitle = g_strdup(row->named ? row->npub_short : "");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
  gtk_widget_set_visible(GTK_WIDGET(row->spinner), checking);
  gtk_spinner_set_spinning(row->spinner, checking);
  gboolean shown = chosen && !checking;
  gtk_widget_set_visible(GTK_WIDGET(row->state_icon), shown);
  if (shown)
    gtk_image_set_from_icon_name(row->state_icon,
                                 gh_mls_invitee_can_invite(state) ? "emblem-ok-symbolic"
                                                                  : "dialog-warning-symbolic");
  gboolean retry = chosen && (state == GH_MLS_INVITEE_UNREACHABLE ||
                              state == GH_MLS_INVITEE_FAILED);
  gtk_widget_set_visible(row->retry_button, retry);
}

/* ---- GhMlsInviteePicker ------------------------------------------------------------------- */

struct _GhMlsInviteePicker {
  AdwPreferencesGroup parent_instance;
  AdwActionRow *empty_row;

  GhMlsUiContext context;   /* objects referenced */
  GPtrArray *rows;          /* GhMlsInviteeRow, listed order */
  gulong accounts_handler;
  guint64 generation;
};

enum { SIGNAL_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhMlsInviteePicker, gh_mls_invitee_picker, ADW_TYPE_PREFERENCES_GROUP)

static void
changed(GhMlsInviteePicker *self)
{
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
}

static void
cancel_check(GhMlsInviteeRow *row)
{
  if (row->checking) {
    g_cancellable_cancel(row->checking);
    g_clear_object(&row->checking);
  }
}

typedef struct {
  GhMlsInviteePicker *picker; /* weak */
  GhMlsInviteeRow *row;       /* weak through the picker's rows */
  GCancellable *cancellable;
} Check;

static void
check_free(Check *check)
{
  if (check->picker)
    g_object_remove_weak_pointer(G_OBJECT(check->picker), (gpointer *)&check->picker);
  g_object_unref(check->cancellable);
  g_free(check);
}

static void
check_done(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  Check *check = data;
  g_autoptr(GError) error = NULL;
  GhMlsInviteeState state = gh_mls_invitee_check_finish(result, &error);
  GhMlsInviteePicker *self = check->picker;
  if (!error && self && check->row->checking == check->cancellable) {
    GhMlsInviteeRow *row = check->row;
    g_clear_object(&row->checking);
    row->state = state;
    row_sync(row);
    /* The subtitle changed out of sight of a screen reader: say it. */
    g_autofree gchar *spoken = g_strdup_printf(
      _("%s: %s"), adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row)),
      gh_mls_invitee_copy(state));
    gtk_accessible_announce(GTK_ACCESSIBLE(row), spoken,
                            GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
    changed(self);
  }
  check_free(check);
}

static void
start_check(GhMlsInviteePicker *self, GhMlsInviteeRow *row)
{
  cancel_check(row);
  row->state = GH_MLS_INVITEE_CHECKING;
  row->checking = g_cancellable_new();
  Check *check = g_new0(Check, 1);
  check->picker = self;
  g_object_add_weak_pointer(G_OBJECT(self), (gpointer *)&check->picker);
  check->row = row;
  check->cancellable = g_object_ref(row->checking);
  gh_mls_invitee_check_async(self->context.accounts, self->context.settings, row->pubkey,
                             self->context.lookup_deadline, row->checking, check_done, check);
  row_sync(row);
}

/* At most GH_MLS_SERVICE_MAX_INVITEES chosen: the others wait, saying why. */
static void
sync_limit(GhMlsInviteePicker *self)
{
  gboolean full = gh_mls_invitee_picker_get_n_selected(self) >= GH_MLS_SERVICE_MAX_INVITEES;
  for (guint i = 0; i < self->rows->len; i++) {
    GhMlsInviteeRow *row = g_ptr_array_index(self->rows, i);
    gboolean open = !full || row_selected(row);
    gtk_widget_set_sensitive(GTK_WIDGET(row->check), open);
    gtk_widget_set_tooltip_text(GTK_WIDGET(row->check),
                                open ? NULL : _("Up to 32 people can be invited at a time"));
  }
}

static void
on_toggled(GtkCheckButton *check, GParamSpec *pspec, gpointer data)
{
  (void)check;
  (void)pspec;
  GhMlsInviteeRow *row = data;
  GhMlsInviteePicker *self = row->picker;
  if (row_selected(row)) {
    start_check(self, row);
  } else {
    cancel_check(row);
    row->state = GH_MLS_INVITEE_CHECKING;
    row_sync(row);
  }
  sync_limit(self);
  changed(self);
}

static void
on_retry(GhMlsInviteeRow *row, gpointer data)
{
  (void)data;
  GhMlsInviteePicker *self = row->picker;
  if (row_selected(row)) {
    start_check(self, row);
    changed(self);
  }
}

static void
clear_rows(GhMlsInviteePicker *self)
{
  for (guint i = 0; i < self->rows->len; i++) {
    GhMlsInviteeRow *row = g_ptr_array_index(self->rows, i);
    cancel_check(row);
    adw_preferences_group_remove(ADW_PREFERENCES_GROUP(self), GTK_WIDGET(row));
  }
  g_ptr_array_set_size(self->rows, 0);
}

static void
clear_context(GhMlsInviteePicker *self)
{
  if (self->accounts_handler && self->context.accounts)
    g_signal_handler_disconnect(self->context.accounts, self->accounts_handler);
  self->accounts_handler = 0;
  g_clear_object(&self->context.service);
  g_clear_object(&self->context.accounts);
  g_clear_object(&self->context.model);
  g_clear_object(&self->context.settings);
  memset(&self->context, 0, sizeof self->context);
}

/* An account generation change ends every check (the lookups end
 * CANCELLED); what was chosen stays chosen and is checked again. */
static void
on_accounts_changed(GhMlsInviteePicker *self)
{
  guint64 generation = gh_account_controller_get_generation(self->context.accounts);
  if (generation == self->generation)
    return;
  self->generation = generation;
  /* A lookup belongs to its generation (a switch or a network reconnect
   * ends it): the chosen are checked again under the new one; with no
   * active account that check ends at once and the next generation retries. */
  for (guint i = 0; i < self->rows->len; i++) {
    GhMlsInviteeRow *row = g_ptr_array_index(self->rows, i);
    if (row_selected(row))
      start_check(self, row);
  }
  changed(self);
}

void
gh_mls_invitee_picker_check_again(GhMlsInviteePicker *self)
{
  g_return_if_fail(GH_IS_MLS_INVITEE_PICKER(self));
  for (guint i = 0; i < self->rows->len; i++) {
    GhMlsInviteeRow *row = g_ptr_array_index(self->rows, i);
    if (row_selected(row))
      start_check(self, row);
  }
  changed(self);
}

void
gh_mls_invitee_picker_setup(GhMlsInviteePicker *self, const GhMlsUiContext *context,
                            const gchar *const *members)
{
  g_return_if_fail(GH_IS_MLS_INVITEE_PICKER(self));
  g_return_if_fail(context != NULL && GH_IS_ACCOUNT_CONTROLLER(context->accounts));
  g_return_if_fail(GH_IS_CONVERSATION_STORE(context->model));
  clear_rows(self);
  clear_context(self);
  self->context = *context;
  self->context.default_relays = NULL;   /* not the picker's */
  if (context->service)
    g_object_ref(context->service);
  g_object_ref(context->accounts);
  g_object_ref(context->model);
  if (context->settings)
    g_object_ref(context->settings);
  self->generation = gh_account_controller_get_generation(context->accounts);
  self->accounts_handler = g_signal_connect_swapped(context->accounts, "changed",
                                                    G_CALLBACK(on_accounts_changed), self);
  g_auto(GStrv) contacts = gh_mls_contacts_dup(context->model);
  for (guint i = 0; contacts && contacts[i]; i++) {
    if (members && g_strv_contains(members, contacts[i]))
      continue;
    GhMlsInviteeRow *row = g_object_new(GH_TYPE_MLS_INVITEE_ROW, NULL);
    row->picker = self;
    row->pubkey = g_strdup(contacts[i]);
    row->npub_short = gh_recipient_npub_short(contacts[i]);
    const gchar *name = context->display_name
      ? context->display_name(contacts[i], context->names_data) : NULL;
    row->named = name && *name;
    const gchar *title = row->named ? name : row->npub_short;
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_avatar_set_text(row->avatar, title);
    gtk_accessible_update_property(GTK_ACCESSIBLE(row->check), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   title, -1);
    row_sync(row);
    g_signal_connect(row->check, "notify::active", G_CALLBACK(on_toggled), row);
    g_signal_connect(row, "retry", G_CALLBACK(on_retry), NULL);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(self), GTK_WIDGET(row));
    g_ptr_array_add(self->rows, row);
  }
  gtk_widget_set_visible(GTK_WIDGET(self->empty_row), self->rows->len == 0);
  sync_limit(self);
  changed(self);
}

static GhMlsInviteeRow *
find_row(GhMlsInviteePicker *self, const gchar *pubkey)
{
  for (guint i = 0; pubkey && i < self->rows->len; i++) {
    GhMlsInviteeRow *row = g_ptr_array_index(self->rows, i);
    if (g_ascii_strcasecmp(row->pubkey, pubkey) == 0)
      return row;
  }
  return NULL;
}

GStrv
gh_mls_invitee_picker_dup_selected(GhMlsInviteePicker *self)
{
  g_return_val_if_fail(GH_IS_MLS_INVITEE_PICKER(self), NULL);
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
  for (guint i = 0; i < self->rows->len; i++) {
    GhMlsInviteeRow *row = g_ptr_array_index(self->rows, i);
    if (row_selected(row))
      g_strv_builder_add(builder, row->pubkey);
  }
  return g_strv_builder_end(builder);
}

guint
gh_mls_invitee_picker_get_n_selected(GhMlsInviteePicker *self)
{
  g_return_val_if_fail(GH_IS_MLS_INVITEE_PICKER(self), 0);
  guint n = 0;
  for (guint i = 0; i < self->rows->len; i++)
    n += row_selected(g_ptr_array_index(self->rows, i)) ? 1 : 0;
  return n;
}

guint
gh_mls_invitee_picker_get_n_listed(GhMlsInviteePicker *self)
{
  g_return_val_if_fail(GH_IS_MLS_INVITEE_PICKER(self), 0);
  return self->rows->len;
}

gboolean
gh_mls_invitee_picker_get_ready(GhMlsInviteePicker *self)
{
  g_return_val_if_fail(GH_IS_MLS_INVITEE_PICKER(self), FALSE);
  guint chosen = 0;
  for (guint i = 0; i < self->rows->len; i++) {
    GhMlsInviteeRow *row = g_ptr_array_index(self->rows, i);
    if (!row_selected(row))
      continue;
    if (!gh_mls_invitee_can_invite(row->state))
      return FALSE;
    chosen++;
  }
  return chosen > 0;
}

gboolean
gh_mls_invitee_picker_get_checking(GhMlsInviteePicker *self)
{
  g_return_val_if_fail(GH_IS_MLS_INVITEE_PICKER(self), FALSE);
  for (guint i = 0; i < self->rows->len; i++) {
    GhMlsInviteeRow *row = g_ptr_array_index(self->rows, i);
    if (row_selected(row) && row->state == GH_MLS_INVITEE_CHECKING)
      return TRUE;
  }
  return FALSE;
}

GhMlsInviteeState
gh_mls_invitee_picker_get_state(GhMlsInviteePicker *self, const gchar *pubkey)
{
  g_return_val_if_fail(GH_IS_MLS_INVITEE_PICKER(self), GH_MLS_INVITEE_CHECKING);
  GhMlsInviteeRow *row = find_row(self, pubkey);
  return row && row_selected(row) ? row->state : GH_MLS_INVITEE_CHECKING;
}

gboolean
gh_mls_invitee_picker_set_selected(GhMlsInviteePicker *self, const gchar *pubkey,
                                   gboolean selected)
{
  g_return_val_if_fail(GH_IS_MLS_INVITEE_PICKER(self), FALSE);
  GhMlsInviteeRow *row = find_row(self, pubkey);
  if (!row)
    return FALSE;
  gtk_check_button_set_active(row->check, selected);
  return TRUE;
}

AdwActionRow *
gh_mls_invitee_picker_get_row(GhMlsInviteePicker *self, const gchar *pubkey)
{
  g_return_val_if_fail(GH_IS_MLS_INVITEE_PICKER(self), NULL);
  GhMlsInviteeRow *row = find_row(self, pubkey);
  return row ? ADW_ACTION_ROW(row) : NULL;
}

static void
gh_mls_invitee_picker_dispose(GObject *object)
{
  GhMlsInviteePicker *self = GH_MLS_INVITEE_PICKER(object);
  for (guint i = 0; i < self->rows->len; i++)
    cancel_check(g_ptr_array_index(self->rows, i));
  clear_context(self);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_MLS_INVITEE_PICKER);
  G_OBJECT_CLASS(gh_mls_invitee_picker_parent_class)->dispose(object);
}

static void
gh_mls_invitee_picker_finalize(GObject *object)
{
  GhMlsInviteePicker *self = GH_MLS_INVITEE_PICKER(object);
  g_ptr_array_unref(self->rows);
  G_OBJECT_CLASS(gh_mls_invitee_picker_parent_class)->finalize(object);
}

static void
gh_mls_invitee_picker_class_init(GhMlsInviteePickerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  object_class->dispose = gh_mls_invitee_picker_dispose;
  object_class->finalize = gh_mls_invitee_picker_finalize;
  signals[SIGNAL_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                                         0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-mls-invitee-picker.ui");
  gtk_widget_class_bind_template_child(widget_class, GhMlsInviteePicker, empty_row);
}

static void
gh_mls_invitee_picker_init(GhMlsInviteePicker *self)
{
  self->rows = g_ptr_array_new();
  gtk_widget_init_template(GTK_WIDGET(self));
}
