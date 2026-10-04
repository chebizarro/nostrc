#include "gh-attachment-sheet.h"

#include <glib/gi18n.h>

/* The photo's largest size on the preview page. */
#define THUMBNAIL_MAX 240
#define SUGGESTIONS_RESOURCE "/org/nostr/Groundhog/blossom-media-suggestions.txt"
#define N_SUGGESTIONS 3

struct _GhAttachmentSheet {
  AdwDialog parent_instance;
  GtkStack *stack;
  AdwEntryRow *server_entry;
  GtkLabel *server_error;
  GtkButton *server_button;
  GtkButton *suggestion_first;
  GtkButton *suggestion_second;
  GtkButton *suggestion_third;
  gchar *suggestion_urls[N_SUGGESTIONS];
  GtkPicture *thumbnail;
  AdwActionRow *file_row;
  AdwActionRow *metadata_row;
  GtkImage *metadata_icon;
  GtkLabel *server_note;
  GtkLabel *timer_note;
  GtkLabel *error_label;
  GtkButton *send_button;
  GtkSpinner *sending_spinner;
  GtkLabel *sending_label;
  GtkButton *sending_cancel;
  GtkLabel *consent_title;
  GtkLabel *consent_body;
  GtkButton *consent_button;
  GtkButton *consent_decline;

  gchar *error; /* on the page shown */
};

enum { SIGNAL_SERVER_CHOSEN, SIGNAL_SEND, SIGNAL_CONSENT, SIGNAL_CANCEL, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhAttachmentSheet, gh_attachment_sheet, ADW_TYPE_DIALOG)

static void
set_error(GhAttachmentSheet *self, GtkLabel *label, const gchar *error)
{
  g_free(self->error);
  self->error = error && *error ? g_strdup(error) : NULL;
  gtk_label_set_text(label, self->error ? self->error : "");
  gtk_widget_set_visible(GTK_WIDGET(label), self->error != NULL);
  if (self->error)
    gtk_accessible_announce(GTK_ACCESSIBLE(self), self->error,
                            GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_HIGH);
}

static void
show_page(GhAttachmentSheet *self, const gchar *page, const gchar *title, GtkWidget *main,
          GtkWidget *focus)
{
  gtk_spinner_set_spinning(self->sending_spinner, g_str_equal(page, "sending"));
  gtk_stack_set_visible_child_name(self->stack, page);
  adw_dialog_set_title(ADW_DIALOG(self), title);
  adw_dialog_set_default_widget(ADW_DIALOG(self), main);
  adw_dialog_set_focus(ADW_DIALOG(self), focus);
}

void
gh_attachment_sheet_set_file(GhAttachmentSheet *self, const gchar *name, guint64 size,
                             const gchar *kind, GdkPaintable *thumbnail)
{
  g_return_if_fail(GH_IS_ATTACHMENT_SHEET(self));
  g_autofree gchar *size_text = g_format_size(size);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->file_row),
                                name && *name ? name : _("Unnamed file"));
  /* TRANSLATORS: a file's size and kind, e.g. "2.1 MB · Photo". */
  g_autofree gchar *subtitle = g_strdup_printf(_("%s · %s"), size_text, kind ? kind : "");
  adw_action_row_set_subtitle(self->file_row, subtitle);
  gtk_picture_set_paintable(self->thumbnail, thumbnail);
  gtk_widget_set_visible(GTK_WIDGET(self->thumbnail), thumbnail != NULL);
  if (thumbnail) {
    gint width = MAX(gdk_paintable_get_intrinsic_width(thumbnail), 1);
    gint height = MAX(gdk_paintable_get_intrinsic_height(thumbnail), 1);
    gdouble scale = MIN(1.0, MIN((gdouble)THUMBNAIL_MAX / width, (gdouble)THUMBNAIL_MAX / height));
    gtk_widget_set_size_request(GTK_WIDGET(self->thumbnail), MAX((gint)(width * scale), 1),
                                MAX((gint)(height * scale), 1));
    gtk_picture_set_alternative_text(self->thumbnail, name);
  }
}

void
gh_attachment_sheet_set_metadata_removed(GhAttachmentSheet *self, gboolean removed)
{
  g_return_if_fail(GH_IS_ATTACHMENT_SHEET(self));
  /* Charter §6 step 2: JPEG and PNG are stripped; anything else says so. */
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->metadata_row),
                                removed ? _("Location and camera data removed")
                                        : _("Files can contain hidden details such as "
                                            "location"));
  adw_action_row_set_subtitle(self->metadata_row,
                              removed ? NULL
                                      : _("Groundhog can remove them only from JPEG and PNG "
                                          "images. This file is sent as it is."));
  gtk_image_set_from_icon_name(self->metadata_icon,
                               removed ? "emblem-ok-symbolic" : "dialog-warning-symbolic");
}

void
gh_attachment_sheet_set_notes(GhAttachmentSheet *self, const gchar *server_note,
                              const gchar *timer_note)
{
  g_return_if_fail(GH_IS_ATTACHMENT_SHEET(self));
  gtk_label_set_text(self->server_note, server_note ? server_note : "");
  gtk_widget_set_visible(GTK_WIDGET(self->timer_note), timer_note && *timer_note);
  gtk_label_set_text(self->timer_note, timer_note ? timer_note : "");
}

void
gh_attachment_sheet_show_servers(GhAttachmentSheet *self, const gchar *error)
{
  g_return_if_fail(GH_IS_ATTACHMENT_SHEET(self));
  show_page(self, "servers", _("Attachment Server"), GTK_WIDGET(self->server_button),
            GTK_WIDGET(self->server_entry));
  set_error(self, self->server_error, error);
  if (error)
    gtk_widget_add_css_class(GTK_WIDGET(self->server_entry), "error");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(self->server_entry), "error");
}

void
gh_attachment_sheet_show_preview(GhAttachmentSheet *self, const gchar *error)
{
  g_return_if_fail(GH_IS_ATTACHMENT_SHEET(self));
  gtk_button_set_label(self->send_button, error ? _("_Try Again") : _("_Send"));
  show_page(self, "preview", _("Send File"), GTK_WIDGET(self->send_button),
            GTK_WIDGET(self->send_button));
  set_error(self, self->error_label, error);
}

void
gh_attachment_sheet_show_sending(GhAttachmentSheet *self, const gchar *text)
{
  g_return_if_fail(GH_IS_ATTACHMENT_SHEET(self));
  gtk_label_set_text(self->sending_label, text ? text : "");
  show_page(self, "sending", _("Sending File"), NULL, GTK_WIDGET(self->sending_cancel));
  g_clear_pointer(&self->error, g_free);
  if (text)
    gtk_accessible_announce(GTK_ACCESSIBLE(self), text,
                            GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
}

void
gh_attachment_sheet_show_consent(GhAttachmentSheet *self, const gchar *host)
{
  g_return_if_fail(GH_IS_ATTACHMENT_SHEET(self));
  g_return_if_fail(host != NULL);
  g_autofree gchar *title = g_strdup_printf(_("%s Only Accepts Files from Accounts It Knows"),
                                            host);
  g_autofree gchar *body = g_strdup_printf(
    _("To upload there, Nostr Signer signs the upload with your account. %s then learns that "
      "the file is yours, when you sent it and its size. It still gets only the encrypted file, "
      "not what it contains or whom it's for."), host);
  gtk_label_set_text(self->consent_title, title);
  gtk_label_set_text(self->consent_body, body);
  /* The choice that tells the server nothing is the one under the keyboard. */
  show_page(self, "consent", _("Upload as Your Account?"), NULL,
            GTK_WIDGET(self->consent_decline));
  g_clear_pointer(&self->error, g_free);
}

const gchar *
gh_attachment_sheet_get_page(GhAttachmentSheet *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENT_SHEET(self), NULL);
  return gtk_stack_get_visible_child_name(self->stack);
}

const gchar *
gh_attachment_sheet_get_error(GhAttachmentSheet *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENT_SHEET(self), NULL);
  return self->error;
}

const gchar *
gh_attachment_sheet_get_metadata_text(GhAttachmentSheet *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENT_SHEET(self), NULL);
  return adw_preferences_row_get_title(ADW_PREFERENCES_ROW(self->metadata_row));
}

const gchar *
gh_attachment_sheet_get_server_note(GhAttachmentSheet *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENT_SHEET(self), NULL);
  return gtk_label_get_text(self->server_note);
}

AdwEntryRow *
gh_attachment_sheet_get_server_entry(GhAttachmentSheet *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENT_SHEET(self), NULL);
  return self->server_entry;
}

gboolean
gh_attachment_sheet_get_has_thumbnail(GhAttachmentSheet *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENT_SHEET(self), FALSE);
  return gtk_widget_get_visible(GTK_WIDGET(self->thumbnail)) &&
         gtk_picture_get_paintable(self->thumbnail) != NULL;
}

/* ---- actions ----------------------------------------------------------------------- */

static void
action_use_server(GtkWidget *widget, const char *name, GVariant *parameter)
{
  GhAttachmentSheet *self = GH_ATTACHMENT_SHEET(widget);
  (void)name;
  (void)parameter;
  g_signal_emit(self, signals[SIGNAL_SERVER_CHOSEN], 0,
                gtk_editable_get_text(GTK_EDITABLE(self->server_entry)));
}

static void
action_use_suggested_server(GtkWidget *widget, const char *name, GVariant *parameter)
{
  GhAttachmentSheet *self = GH_ATTACHMENT_SHEET(widget);
  (void)name;
  gint index = g_variant_get_int32(parameter);
  if (index < 0 || index >= N_SUGGESTIONS || !self->suggestion_urls[index])
    return;
  const gchar *server = self->suggestion_urls[index];
  gtk_editable_set_text(GTK_EDITABLE(self->server_entry), server);
  g_signal_emit(self, signals[SIGNAL_SERVER_CHOSEN], 0, server);
}

static void
action_send(GtkWidget *widget, const char *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  g_signal_emit(widget, signals[SIGNAL_SEND], 0);
}

static void
action_consent(GtkWidget *widget, const char *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  g_signal_emit(widget, signals[SIGNAL_CONSENT], 0);
}

static void
action_cancel(GtkWidget *widget, const char *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  g_signal_emit(widget, signals[SIGNAL_CANCEL], 0);
}

static void
on_entry_changed(GhAttachmentSheet *self)
{
  if (g_strcmp0(gtk_stack_get_visible_child_name(self->stack), "servers") != 0 || !self->error)
    return;
  set_error(self, self->server_error, NULL);
  gtk_widget_remove_css_class(GTK_WIDGET(self->server_entry), "error");
}

/* ---- GObject ---------------------------------------------------------------------- */

GhAttachmentSheet *
gh_attachment_sheet_new(void)
{
  return g_object_new(GH_TYPE_ATTACHMENT_SHEET, NULL);
}

static void
gh_attachment_sheet_dispose(GObject *object)
{
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_ATTACHMENT_SHEET);
  G_OBJECT_CLASS(gh_attachment_sheet_parent_class)->dispose(object);
}

static void
gh_attachment_sheet_finalize(GObject *object)
{
  GhAttachmentSheet *self = GH_ATTACHMENT_SHEET(object);
  g_free(self->error);
  for (guint i = 0; i < N_SUGGESTIONS; i++)
    g_free(self->suggestion_urls[i]);
  G_OBJECT_CLASS(gh_attachment_sheet_parent_class)->finalize(object);
}

static void
gh_attachment_sheet_class_init(GhAttachmentSheetClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  object_class->dispose = gh_attachment_sheet_dispose;
  object_class->finalize = gh_attachment_sheet_finalize;

  signals[SIGNAL_SERVER_CHOSEN] =
    g_signal_new("server-chosen", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                 NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_SEND] = g_signal_new("send", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                                      NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_CONSENT] = g_signal_new("consent", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                                         0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_CANCEL] = g_signal_new("cancel", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                                        NULL, NULL, NULL, G_TYPE_NONE, 0);

  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-attachment-sheet.ui");
#define BIND(name) gtk_widget_class_bind_template_child(widget_class, GhAttachmentSheet, name)
  BIND(stack);
  BIND(server_entry);
  BIND(server_error);
  BIND(server_button);
  BIND(suggestion_first);
  BIND(suggestion_second);
  BIND(suggestion_third);
  BIND(thumbnail);
  BIND(file_row);
  BIND(metadata_row);
  BIND(metadata_icon);
  BIND(server_note);
  BIND(timer_note);
  BIND(error_label);
  BIND(send_button);
  BIND(sending_spinner);
  BIND(sending_label);
  BIND(sending_cancel);
  BIND(consent_title);
  BIND(consent_body);
  BIND(consent_button);
  BIND(consent_decline);
#undef BIND
  gtk_widget_class_install_action(widget_class, "sheet.use-server", NULL, action_use_server);
  gtk_widget_class_install_action(widget_class, "sheet.use-suggested-server", "i",
                                  action_use_suggested_server);
  gtk_widget_class_install_action(widget_class, "sheet.send", NULL, action_send);
  gtk_widget_class_install_action(widget_class, "sheet.consent", NULL, action_consent);
  gtk_widget_class_install_action(widget_class, "sheet.cancel", NULL, action_cancel);
}

static void
load_suggestions(GhAttachmentSheet *self)
{
  GtkButton *buttons[N_SUGGESTIONS] = {
    self->suggestion_first, self->suggestion_second, self->suggestion_third
  };
  g_auto(GStrv) hosts = g_new0(gchar *, N_SUGGESTIONS + 1);
  g_autofree gchar *text = NULL;
  g_auto(GStrv) urls = NULL;
  g_autoptr(GBytes) bytes = g_resources_lookup_data(SUGGESTIONS_RESOURCE,
                                                    G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
  if (!bytes)
    goto unavailable;
  gsize size = 0;
  const gchar *data = g_bytes_get_data(bytes, &size);
  text = g_strndup(data, size);
  urls = g_strsplit(text, "\n", -1);
  if (g_strv_length(urls) != N_SUGGESTIONS + 1 || *urls[N_SUGGESTIONS] != '\0')
    goto unavailable;
  for (guint i = 0; i < N_SUGGESTIONS; i++) {
    g_autoptr(GUri) uri = g_uri_parse(urls[i], G_URI_FLAGS_NONE, NULL);
    if (!uri || g_strcmp0(g_uri_get_scheme(uri), "https") != 0 ||
        !g_uri_get_host(uri) || g_uri_get_userinfo(uri) || g_uri_get_query(uri) ||
        g_uri_get_fragment(uri))
      goto unavailable;
    hosts[i] = g_strdup(g_uri_get_host(uri));
  }
  for (guint i = 0; i < N_SUGGESTIONS; i++) {
    self->suggestion_urls[i] = g_strdup(urls[i]);
    gtk_button_set_label(buttons[i], hosts[i]);
  }
  return;

unavailable:
  for (guint i = 0; i < N_SUGGESTIONS; i++)
    gtk_widget_set_sensitive(GTK_WIDGET(buttons[i]), FALSE);
}

static void
gh_attachment_sheet_init(GhAttachmentSheet *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  load_suggestions(self);
  gtk_widget_set_overflow(GTK_WIDGET(self->thumbnail), GTK_OVERFLOW_HIDDEN);
  g_signal_connect_swapped(self->server_entry, "changed", G_CALLBACK(on_entry_changed), self);
  gh_attachment_sheet_set_metadata_removed(self, FALSE);
}
