#include "gh-add-contact-dialog.h"
#include "gh-recipient.h"
#include "gh-window.h"

#include <glib/gi18n.h>

struct _GhAddContactDialog {
  AdwDialog parent_instance;
  AdwEntryRow *input_row;
  AdwPreferencesGroup *result_group;
  AdwActionRow *result_row;
  AdwAvatar *result_avatar;
  AdwActionRow *lookup_row;
  GtkSpinner *lookup_spinner;
  GtkLabel *error_label;
  GtkButton *add_button;

  GhAddContactConfig config;
  gchar *resolved_pubkey;   /* hex, once resolved */
  GCancellable *lookup_cancellable;
};

enum { SIGNAL_CONTACT_ADDED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhAddContactDialog, gh_add_contact_dialog, ADW_TYPE_DIALOG)

static void
show_error(GhAddContactDialog *self, const gchar *message)
{
  gtk_label_set_text(self->error_label, message);
  gtk_widget_set_visible(GTK_WIDGET(self->error_label), message != NULL);
  gtk_widget_set_visible(GTK_WIDGET(self->result_group), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->add_button), FALSE);
}

static void
show_person(GhAddContactDialog *self, const gchar *pubkey, const gchar *display)
{
  g_free(self->resolved_pubkey);
  self->resolved_pubkey = g_strdup(pubkey);
  g_autofree gchar *npub_short = gh_recipient_npub_short(pubkey);
  const gchar *title = (display && *display) ? display : npub_short;
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->result_row), title);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(self->result_row),
                              (display && *display) ? npub_short : "");
  adw_avatar_set_text(self->result_avatar, title);
  gtk_widget_set_visible(GTK_WIDGET(self->result_group), TRUE);
  gtk_widget_set_visible(GTK_WIDGET(self->lookup_row), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(self->error_label), FALSE);

  /* Refuse to add yourself. */
  const gchar *account =
    gh_conversation_store_get_account(self->config.conversations);
  if (account && g_ascii_strcasecmp(pubkey, account) == 0) {
    show_error(self, _("That is your own public key."));
    return;
  }
  gtk_widget_set_sensitive(GTK_WIDGET(self->add_button), TRUE);
}

/* NIP-05 lookup done. */
static void
nip05_done(GObject *source, GAsyncResult *result, gpointer data)
{
  GhAddContactDialog *self = data;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *pubkey = gh_nip05_lookup_finish(GH_NIP05(source), result, &error);
  gtk_spinner_set_spinning(self->lookup_spinner, FALSE);
  gtk_widget_set_visible(GTK_WIDGET(self->lookup_spinner), FALSE);
  if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  if (!pubkey) {
    show_error(self, error ? error->message
                           : _("No public key found for that address."));
    return;
  }
  const gchar *display = self->config.display_name
    ? self->config.display_name(self->config.names_data, pubkey) : NULL;
  show_person(self, pubkey, display);
}

static void
start_lookup(GhAddContactDialog *self, const gchar *address)
{
  if (self->lookup_cancellable)
    g_cancellable_cancel(self->lookup_cancellable);
  g_clear_object(&self->lookup_cancellable);
  self->lookup_cancellable = g_cancellable_new();
  gtk_widget_set_visible(GTK_WIDGET(self->lookup_spinner), TRUE);
  gtk_spinner_set_spinning(self->lookup_spinner, TRUE);
  gh_nip05_lookup_async(self->config.nip05, address, self->lookup_cancellable,
                        nip05_done, self);
}

static void
on_lookup_activated(AdwActionRow *row, gpointer data)
{
  (void)row;
  GhAddContactDialog *self = data;
  const gchar *text = gtk_editable_get_text(GTK_EDITABLE(self->input_row));
  g_autoptr(GhRecipientInput) input = gh_recipient_input_parse(text);
  if (input->kind == GH_RECIPIENT_INPUT_NIP05 && input->nip05)
    start_lookup(self, input->nip05);
}

static void
on_input_apply(AdwEntryRow *entry, gpointer data)
{
  GhAddContactDialog *self = data;
  const gchar *text = gtk_editable_get_text(GTK_EDITABLE(entry));
  g_autoptr(GhRecipientInput) input = gh_recipient_input_parse(text);

  /* Cancel any running lookup. */
  if (self->lookup_cancellable)
    g_cancellable_cancel(self->lookup_cancellable);
  g_clear_object(&self->lookup_cancellable);
  g_clear_pointer(&self->resolved_pubkey, g_free);

  switch (input->kind) {
  case GH_RECIPIENT_INPUT_EMPTY:
    gtk_widget_set_visible(GTK_WIDGET(self->result_group), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->error_label), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(self->add_button), FALSE);
    return;
  case GH_RECIPIENT_INPUT_SECRET:
    show_error(self, _("That looks like a secret key. Paste a public identifier instead."));
    return;
  case GH_RECIPIENT_INPUT_OTHER_ENTITY:
    show_error(self, _("That identifies a note or event, not a person."));
    return;
  case GH_RECIPIENT_INPUT_INVALID:
    show_error(self, _("That looks like an npub but doesn't decode. Check for typos."));
    return;
  case GH_RECIPIENT_INPUT_PUBKEY: {
    const gchar *display = self->config.display_name
      ? self->config.display_name(self->config.names_data, input->pubkey) : NULL;
    show_person(self, input->pubkey, display);
    return;
  }
  case GH_RECIPIENT_INPUT_NIP05:
    if (!self->config.nip05) {
      show_error(self, _("Address lookups are not available in this build."));
      return;
    }
    /* Show the consent row: the user chooses to look it up. */
    gtk_widget_set_visible(GTK_WIDGET(self->result_group), TRUE);
    {
      g_autofree gchar *subtitle =
        g_strdup_printf(_("Connects to %s to find their public key"),
                        input->nip05_domain);
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->lookup_row),
                                    _("Look Up This Address"));
      adw_action_row_set_subtitle(ADW_ACTION_ROW(self->lookup_row), subtitle);
    }
    gtk_widget_set_visible(GTK_WIDGET(self->lookup_row), TRUE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->result_row), input->nip05);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(self->result_row), "");
    adw_avatar_set_text(self->result_avatar, input->nip05_local);
    gtk_widget_set_sensitive(GTK_WIDGET(self->add_button), FALSE);
    return;
  case GH_RECIPIENT_INPUT_TEXT:
    show_error(self,
      _("Paste an npub (starting with npub1), a hex public key, or a name@domain address."));
    return;
  }
}

static void
on_add(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  GhAddContactDialog *self = GH_ADD_CONTACT_DIALOG(widget);
  if (!self->resolved_pubkey)
    return;
  /* "Adding a contact" = opening an accepted DM conversation with them.
   * The conversation store creates the room if it doesn't exist and marks
   * it accepted, which the contact directory picks up. */
  g_autoptr(GError) error = NULL;
  const gchar *peers[] = { self->resolved_pubkey, NULL };
  GhConversation *room = gh_conversation_store_open_room(
    self->config.conversations, peers, &error);
  if (!room) {
    show_error(self, error ? error->message : _("Could not add contact."));
    return;
  }
  g_signal_emit(self, signals[SIGNAL_CONTACT_ADDED], 0, room);
  adw_dialog_close(ADW_DIALOG(self));
}

static void
gh_add_contact_dialog_dispose(GObject *object)
{
  GhAddContactDialog *self = GH_ADD_CONTACT_DIALOG(object);
  if (self->lookup_cancellable) {
    g_cancellable_cancel(self->lookup_cancellable);
    g_clear_object(&self->lookup_cancellable);
  }
  g_clear_object(&self->config.conversations);
  g_clear_object(&self->config.nip05);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_ADD_CONTACT_DIALOG);
  G_OBJECT_CLASS(gh_add_contact_dialog_parent_class)->dispose(object);
}

static void
gh_add_contact_dialog_finalize(GObject *object)
{
  GhAddContactDialog *self = GH_ADD_CONTACT_DIALOG(object);
  g_free(self->resolved_pubkey);
  G_OBJECT_CLASS(gh_add_contact_dialog_parent_class)->finalize(object);
}

static void
gh_add_contact_dialog_class_init(GhAddContactDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  object_class->dispose = gh_add_contact_dialog_dispose;
  object_class->finalize = gh_add_contact_dialog_finalize;
  signals[SIGNAL_CONTACT_ADDED] = g_signal_new(
    "contact-added", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
    NULL, NULL, G_TYPE_NONE, 1, GH_TYPE_CONVERSATION);
  gtk_widget_class_set_template_from_resource(
    widget_class, "/org/nostr/Groundhog/ui/gh-add-contact-dialog.ui");
  gtk_widget_class_bind_template_child(widget_class, GhAddContactDialog, input_row);
  gtk_widget_class_bind_template_child(widget_class, GhAddContactDialog, result_group);
  gtk_widget_class_bind_template_child(widget_class, GhAddContactDialog, result_row);
  gtk_widget_class_bind_template_child(widget_class, GhAddContactDialog, result_avatar);
  gtk_widget_class_bind_template_child(widget_class, GhAddContactDialog, lookup_row);
  gtk_widget_class_bind_template_child(widget_class, GhAddContactDialog, lookup_spinner);
  gtk_widget_class_bind_template_child(widget_class, GhAddContactDialog, error_label);
  gtk_widget_class_bind_template_child(widget_class, GhAddContactDialog, add_button);
  gtk_widget_class_install_action(widget_class, "add-contact.add", NULL, on_add);
}

static void
gh_add_contact_dialog_init(GhAddContactDialog *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  g_signal_connect(self->input_row, "apply", G_CALLBACK(on_input_apply), self);
  g_signal_connect(self->lookup_row, "activated", G_CALLBACK(on_lookup_activated), self);
}

GhAddContactDialog *
gh_add_contact_dialog_new(const GhAddContactConfig *config)
{
  g_return_val_if_fail(config != NULL, NULL);
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(config->conversations), NULL);
  GhAddContactDialog *self = g_object_new(GH_TYPE_ADD_CONTACT_DIALOG, NULL);
  self->config = *config;
  g_object_ref(config->conversations);
  if (config->nip05)
    g_object_ref(config->nip05);
  return self;
}

/* ---- win.add-contact attachment ------------------------------------------- */

typedef struct {
  GhAddContactConfig config;
  GhWindow *window;
} AddContactAttachment;

static void
add_contact_attachment_free(gpointer data)
{
  AddContactAttachment *a = data;
  g_clear_object(&a->config.conversations);
  g_clear_object(&a->config.nip05);
  g_free(a);
}

static void
on_contact_added(GhAddContactDialog *dialog, GhConversation *room, gpointer data)
{
  (void)dialog;
  AddContactAttachment *a = data;
  gh_window_open_item(a->window, room);
}

static void
present_add_contact(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action;
  (void)parameter;
  AddContactAttachment *a = data;
  GhAddContactDialog *dialog = gh_add_contact_dialog_new(&a->config);
  g_signal_connect(dialog, "contact-added", G_CALLBACK(on_contact_added), a);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(a->window));
}

static void
sync_add_contact_enabled(GhWindow *window, GParamSpec *pspec, gpointer data)
{
  (void)pspec;
  GhConversationStore *store = data;
  const gchar *account = gh_conversation_store_get_account(store);
  GAction *action = g_action_map_lookup_action(G_ACTION_MAP(window), "add-contact");
  if (action)
    g_simple_action_set_enabled(G_SIMPLE_ACTION(action), account != NULL);
}

void
gh_add_contact_attach(GhWindow *window, const GhAddContactConfig *config)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  g_return_if_fail(config != NULL && GH_IS_CONVERSATION_STORE(config->conversations));
  AddContactAttachment *a = g_new0(AddContactAttachment, 1);
  a->config = *config;
  a->window = window;
  g_object_ref(config->conversations);
  if (config->nip05)
    g_object_ref(config->nip05);

  GSimpleAction *action = g_simple_action_new("add-contact", NULL);
  g_signal_connect(action, "activate", G_CALLBACK(present_add_contact), a);
  g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(action));
  g_object_set_data_full(G_OBJECT(window), "gh-add-contact-attachment", a,
                         add_contact_attachment_free);
  g_signal_connect_object(config->conversations, "notify::account",
                          G_CALLBACK(sync_add_contact_enabled), window,
                          G_CONNECT_SWAPPED);
  sync_add_contact_enabled(window, NULL, config->conversations);
  g_object_unref(action);
}
