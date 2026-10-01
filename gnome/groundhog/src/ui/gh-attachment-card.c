#include "gh-attachment-card.h"
#include "gh-message-row.h"

#include <glib/gi18n.h>
#include <string.h>

#define PROVIDER_DATA "groundhog-attachment-card-provider"
/* Set on a transfer whose plaintext GTK couldn't decode: not tried again. */
#define UNDECODABLE_DATA "groundhog-attachment-undecodable"

/* The inline photo's largest size (charter §7.6: bubbles are capped). */
#define PREVIEW_MAX 280
#define PREVIEW_MAX_COMPACT 200

typedef struct {
  GhAttachmentCardProvider vtable;
  gpointer data;
  GDestroyNotify destroy;
} Provider;

struct _GhAttachmentCard {
  GtkWidget parent_instance;
  GtkImage *type_icon;
  GtkLabel *title_label;
  GtkLabel *detail_label;
  GtkPicture *preview;
  GtkBox *status_box;
  GtkSpinner *spinner;
  GtkImage *status_icon;
  GtkLabel *status_label;
  GtkBox *actions;
  GtkButton *download_button;
  GtkButton *cancel_button;
  GtkButton *retry_button;
  GtkButton *save_button;

  GhMessage *message;
  guint index;                     /* which of its files */
  gboolean described;              /* message has file index: */
  gchar *mime;                     /*   its declared type */
  guint64 size;                    /*   the encrypted size, 0: unknown */
  GhAttachmentTransfer *transfer;  /* a reference while rooted with a provider */
  GhAttachmentState shown;         /* the state last shown (announcements) */
  gboolean compact;
  gchar *summary;
};

enum { PROP_0, PROP_MESSAGE, PROP_INDEX, PROP_COMPACT, PROP_SUMMARY, N_PROPS };
static GParamSpec *props[N_PROPS];

G_DEFINE_FINAL_TYPE(GhAttachmentCard, gh_attachment_card, GTK_TYPE_WIDGET)

static void update(GhAttachmentCard *self);
static void on_transfer_notify(GhAttachmentCard *self);

/* ---- the provider -------------------------------------------------------------------- */

static void
provider_free(gpointer data)
{
  Provider *provider = data;
  if (provider->destroy)
    provider->destroy(provider->data);
  g_free(provider);
}

void
gh_attachment_card_set_provider(GtkWidget *widget, const GhAttachmentCardProvider *provider,
                                gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail(GTK_IS_WIDGET(widget));
  g_return_if_fail(!provider || (provider->lookup && provider->download && provider->cancel &&
                                 provider->save));
  Provider *copy = NULL;
  if (provider) {
    copy = g_new0(Provider, 1);
    copy->vtable = *provider;
    copy->data = data;
    copy->destroy = destroy;
  }
  g_object_set_data_full(G_OBJECT(widget), PROVIDER_DATA, copy, copy ? provider_free : NULL);
}

static Provider *
find_provider(GhAttachmentCard *self)
{
  for (GtkWidget *w = GTK_WIDGET(self); w; w = gtk_widget_get_parent(w)) {
    Provider *provider = g_object_get_data(G_OBJECT(w), PROVIDER_DATA);
    if (provider)
      return provider;
  }
  return NULL;
}

static GhAttachmentTransfer *
provider_lookup(Provider *provider, GhAttachmentCard *self)
{
  if (provider->vtable.lookup_at)
    return provider->vtable.lookup_at(self->message, self->index, provider->data);
  return self->index == 0 ? provider->vtable.lookup(self->message, provider->data) : NULL;
}

/* What the card says before anything is fetched: a kind-15 message's file,
 * or an encrypted group message's file index. */
static void
describe(GhAttachmentCard *self)
{
  self->described = FALSE;
  g_clear_pointer(&self->mime, g_free);
  self->size = 0;
  if (!self->message)
    return;
  g_autoptr(GhNip17File) file = self->index == 0 ? gh_message_dup_file(self->message) : NULL;
  if (file) {
    self->described = TRUE;
    self->mime = g_strdup(file->file_type);
    self->size = file->size;
    return;
  }
  const GhMessageAttachment *a = gh_message_get_attachment(self->message, self->index);
  if (a) {
    self->described = TRUE;
    self->mime = g_strdup(a->media_type);
  }
}

/* ---- text ---------------------------------------------------------------------------- */

gchar *
gh_attachment_card_describe_type(const gchar *mime)
{
  if (mime && g_str_has_prefix(mime, "image/"))
    return g_strdup(_("Photo"));
  if (mime && g_str_has_prefix(mime, "video/"))
    return g_strdup(_("Video"));
  if (mime && g_str_has_prefix(mime, "audio/"))
    return g_strdup(_("Audio"));
  /* Format names, the same in every language. */
  static const struct {
    const gchar *mime;
    const gchar *format;
  } formats[] = {
    { "application/pdf", "PDF" },
    { "application/zip", "ZIP" },
  };
  for (guint i = 0; mime && i < G_N_ELEMENTS(formats); i++)
    if (g_str_equal(mime, formats[i].mime))
      /* TRANSLATORS: a file and its format, e.g. "File (PDF)". */
      return g_strdup_printf(_("File (%s)"), formats[i].format);
  if (mime && g_str_equal(mime, "text/plain"))
    return g_strdup(_("File (Text)"));
  return g_strdup(_("File"));
}

const gchar *
gh_attachment_card_type_icon(const gchar *mime)
{
  if (mime && g_str_has_prefix(mime, "image/"))
    return "image-x-generic-symbolic";
  if (mime && g_str_has_prefix(mime, "video/"))
    return "video-x-generic-symbolic";
  if (mime && g_str_has_prefix(mime, "audio/"))
    return "audio-x-generic-symbolic";
  if (mime && (g_str_equal(mime, "application/pdf") || g_str_has_prefix(mime, "text/")))
    return "x-office-document-symbolic";
  if (mime && g_str_equal(mime, "application/zip"))
    return "package-x-generic-symbolic";
  return "text-x-generic-symbolic";
}

const gchar *
gh_attachment_card_sniff_extension(GBytes *plaintext)
{
  gsize size = 0;
  const guint8 *data = plaintext ? g_bytes_get_data(plaintext, &size) : NULL;
  if (size >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff)
    return "jpg";
  if (size >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0)
    return "png";
  if (size >= 6 && (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0))
    return "gif";
  if (size >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0)
    return "webp";
  if (size >= 5 && memcmp(data, "%PDF-", 5) == 0)
    return "pdf";
  if (size >= 4 && memcmp(data, "PK\x03\x04", 4) == 0)
    return "zip";
  return NULL;
}

static const struct {
  const gchar *mime, *ext;
} declared_extensions[] = {
  { "image/jpeg", "jpg" }, { "image/png", "png" }, { "image/gif", "gif" },
  { "image/webp", "webp" }, { "application/pdf", "pdf" }, { "text/plain", "txt" },
  { "application/zip", "zip" }, { "video/mp4", "mp4" }, { "video/webm", "webm" },
  { "audio/mpeg", "mp3" }, { "audio/ogg", "ogg" }, { "audio/mp4", "m4a" },
};

/* Extensions a sender may keep when the type says nothing better: documents,
 * media and archives a file manager opens in a viewer, never something it
 * runs or launches (.desktop, scripts, executables, installers, links, web
 * pages that run scripts from file://). */
static const gchar *const safe_extensions[] = {
  "txt", "md", "pdf", "rtf", "csv", "odt", "ods", "odp", "doc", "docx", "xls", "xlsx", "ppt",
  "pptx", "epub", "jpg", "jpeg", "png", "gif", "webp", "heic", "heif", "avif", "bmp", "tif",
  "tiff", "mp3", "m4a", "ogg", "oga", "opus", "flac", "wav", "mp4", "m4v", "mov", "webm", "mkv",
  "zip", "7z", "tar", "gz", "xz", "bz2", "ics", "vcf", NULL,
};

gchar *
gh_attachment_card_safe_save_name(const gchar *sender_name, const gchar *mime,
                                  GBytes *plaintext)
{
  /* The bytes decide the extension; the declared type when they can't. */
  const gchar *ext = gh_attachment_card_sniff_extension(plaintext);
  for (guint i = 0; !ext && mime && i < G_N_ELEMENTS(declared_extensions); i++)
    if (g_str_equal(mime, declared_extensions[i].mime))
      ext = declared_extensions[i].ext;
  g_autofree gchar *stem = g_strdup(sender_name && *sender_name ? sender_name : "");
  gchar *dot = strrchr(stem, '.');
  g_autofree gchar *sender_ext = NULL;
  if (dot && dot != stem) {
    sender_ext = g_ascii_strdown(dot + 1, -1);
    *dot = '\0';
  }
  /* Only a harmless extension of the sender's survives, and only when
   * neither the bytes nor the type name one. */
  if (!ext && sender_ext && g_strv_contains(safe_extensions, sender_ext))
    ext = sender_ext;
  if (!*stem) {
    g_free(stem);
    stem = g_strdup(mime && g_str_has_prefix(mime, "image/") ? _("photo") : _("file"));
  }
  return ext ? g_strconcat(stem, ".", ext, NULL) : g_steal_pointer(&stem);
}

gchar *
gh_attachment_card_suggest_name(const gchar *mime, GBytes *plaintext)
{
  gsize size = 0;
  const guint8 *data = plaintext ? g_bytes_get_data(plaintext, &size) : NULL;
  /* The bytes decide what the file is, not the sender's type. */
  if (size >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff)
    return g_strdup(_("photo.jpg"));
  if (size >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0)
    return g_strdup(_("photo.png"));
  if (size >= 5 && memcmp(data, "%PDF-", 5) == 0)
    return g_strdup(_("file.pdf"));
  if (mime && g_str_equal(mime, "text/plain"))
    return g_strdup(_("file.txt"));
  if (size >= 4 && memcmp(data, "PK\x03\x04", 4) == 0)
    return g_strdup(_("file.zip"));
  return g_strdup(_("file"));
}

/* The encrypted file's size is the plaintext's plus the 16-byte tag. */
static gchar *
describe_size(guint64 size)
{
  return size > 16 ? g_format_size(size - 16) : NULL;
}

/* ---- updates ----------------------------------------------------------------------- */

static void
set_summary(GhAttachmentCard *self, gchar *summary)
{
  if (g_strcmp0(self->summary, summary) == 0) {
    g_free(summary);
    return;
  }
  g_free(self->summary);
  self->summary = summary;
  gtk_accessible_update_property(GTK_ACCESSIBLE(self), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 self->summary, -1);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_SUMMARY]);
}

static void
update_header(GhAttachmentCard *self)
{
  const gchar *mime = self->described ? self->mime : NULL;
  g_autofree gchar *kind = gh_attachment_card_describe_type(mime);
  gtk_image_set_from_icon_name(self->type_icon, gh_attachment_card_type_icon(mime));
  gtk_label_set_text(self->title_label, self->described ? kind : "");
  if (!self->message || !self->described) {
    gtk_label_set_text(self->detail_label, "");
    return;
  }
  g_autofree gchar *sender = gh_message_is_self(self->message)
                               ? g_strdup(_("you"))
                               : gh_message_row_sender_name(self->message);
  g_autofree gchar *size = describe_size(self->size);
  g_autofree gchar *detail =
    size ? /* TRANSLATORS: a file's size and who sent it: "2.1 MB · from npub1…". */
           g_strdup_printf(_("%s · from %s"), size, sender)
         : /* TRANSLATORS: who sent a file: "from npub1…". */
           g_strdup_printf(_("from %s"), sender);
  gtk_label_set_text(self->detail_label, detail);
}

/* The photo, decoded once per transfer, and only after the decode guard. */
static gboolean
update_preview(GhAttachmentCard *self)
{
  GhAttachmentTransfer *transfer = self->transfer;
  if (!transfer || gh_attachment_transfer_get_state(transfer) != GH_ATTACHMENT_STATE_READY ||
      !gh_attachment_transfer_get_previewable(transfer)) {
    gtk_picture_set_paintable(self->preview, NULL);
    gtk_widget_set_visible(GTK_WIDGET(self->preview), FALSE);
    return FALSE;
  }
  GObject *preview = gh_attachment_transfer_get_preview(transfer);
  GBytes *plaintext = gh_attachment_transfer_get_plaintext(transfer);
  if (!GDK_IS_TEXTURE(preview)) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GdkTexture) texture =
      g_object_get_data(G_OBJECT(transfer), UNDECODABLE_DATA) != plaintext
        ? gdk_texture_new_from_bytes(plaintext, &error)
        : NULL;
    if (!texture) {
      if (error) {
        g_debug("Attachment: a photo could not be decoded: %s", error->message);
        g_object_set_data(G_OBJECT(transfer), UNDECODABLE_DATA, plaintext);
      }
      gtk_picture_set_paintable(self->preview, NULL);
      gtk_widget_set_visible(GTK_WIDGET(self->preview), FALSE);
      return FALSE;
    }
    /* Kept on the transfer for recycled rows; this card updates itself. */
    g_signal_handlers_block_by_func(transfer, on_transfer_notify, self);
    gh_attachment_transfer_set_preview(transfer, G_OBJECT(texture));
    g_signal_handlers_unblock_by_func(transfer, on_transfer_notify, self);
    preview = G_OBJECT(texture);
  }
  GdkTexture *texture = GDK_TEXTURE(preview);
  gint width = MAX(gdk_texture_get_width(texture), 1);
  gint height = MAX(gdk_texture_get_height(texture), 1);
  gint max = self->compact ? PREVIEW_MAX_COMPACT : PREVIEW_MAX;
  gdouble scale = MIN(1.0, MIN((gdouble)max / width, (gdouble)max / height));
  gtk_widget_set_size_request(GTK_WIDGET(self->preview), MAX((gint)(width * scale), 1),
                              MAX((gint)(height * scale), 1));
  gtk_picture_set_paintable(self->preview, GDK_PAINTABLE(texture));
  g_autofree gchar *sender = self->message ? gh_message_row_sender_name(self->message) : NULL;
  g_autofree gchar *alternative = g_strdup_printf(_("Photo from %s"), sender ? sender : "");
  gtk_picture_set_alternative_text(self->preview, alternative);
  gtk_widget_set_visible(GTK_WIDGET(self->preview), TRUE);
  return TRUE;
}

static void
show_status(GhAttachmentCard *self, const gchar *text, gboolean spinning, gboolean warning)
{
  gtk_widget_set_visible(GTK_WIDGET(self->status_box), text != NULL);
  gtk_label_set_text(self->status_label, text ? text : "");
  gtk_widget_set_visible(GTK_WIDGET(self->spinner), spinning);
  gtk_spinner_set_spinning(self->spinner, spinning);
  gtk_widget_set_visible(GTK_WIDGET(self->status_icon), warning);
}

static void
show_buttons(GhAttachmentCard *self, gboolean download, gboolean cancel, gboolean retry,
             gboolean save)
{
  gtk_widget_set_visible(GTK_WIDGET(self->download_button), download);
  gtk_widget_set_visible(GTK_WIDGET(self->cancel_button), cancel);
  gtk_widget_set_visible(GTK_WIDGET(self->retry_button), retry);
  gtk_widget_set_visible(GTK_WIDGET(self->save_button), save);
  gtk_widget_set_visible(GTK_WIDGET(self->actions), download || cancel || retry || save);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "attachment.download", download || retry);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "attachment.cancel", cancel);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "attachment.save", save);
}

/* Keyboard focus on a button that is about to hide moves to the one that
 * replaces it (Download -> Cancel -> Save), so it is never lost. */
static void
keep_focus(GhAttachmentCard *self, gboolean had_focus)
{
  if (!had_focus)
    return;
  GtkButton *buttons[] = { self->cancel_button, self->save_button, self->retry_button,
                           self->download_button };
  for (guint i = 0; i < G_N_ELEMENTS(buttons); i++)
    if (gtk_widget_get_visible(GTK_WIDGET(buttons[i]))) {
      gtk_widget_grab_focus(GTK_WIDGET(buttons[i]));
      return;
    }
}

static gboolean
focus_in_actions(GhAttachmentCard *self)
{
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
  return focus && gtk_widget_is_ancestor(focus, GTK_WIDGET(self->actions));
}

static void
announce(GhAttachmentCard *self, const gchar *text)
{
  if (gtk_widget_get_mapped(GTK_WIDGET(self)))
    gtk_accessible_announce(GTK_ACCESSIBLE(self), text,
                            GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
}

static void
update(GhAttachmentCard *self)
{
  gboolean had_focus = focus_in_actions(self);
  update_header(self);
  g_autofree gchar *kind = gh_attachment_card_describe_type(self->described ? self->mime
                                                                            : NULL);
  const gchar *state_text = NULL;
  g_autofree gchar *state_owned = NULL;
  GhAttachmentState state = self->transfer ? gh_attachment_transfer_get_state(self->transfer)
                                           : GH_ATTACHMENT_STATE_IDLE;
  if (!self->described) {
    show_status(self, NULL, FALSE, FALSE);
    show_buttons(self, FALSE, FALSE, FALSE, FALSE);
    update_preview(self);
  } else if (!self->transfer) {
    state_text = _("This file can't be downloaded here.");
    show_status(self, state_text, FALSE, FALSE);
    show_buttons(self, FALSE, FALSE, FALSE, FALSE);
    update_preview(self);
  } else {
    switch (state) {
    case GH_ATTACHMENT_STATE_IDLE:
      state_text = _("Not downloaded");
      show_status(self, NULL, FALSE, FALSE);
      show_buttons(self, TRUE, FALSE, FALSE, FALSE);
      update_preview(self);
      break;
    case GH_ATTACHMENT_STATE_DOWNLOADING:
      state_text = _("Downloading…");
      show_status(self, state_text, TRUE, FALSE);
      show_buttons(self, FALSE, TRUE, FALSE, FALSE);
      update_preview(self);
      break;
    case GH_ATTACHMENT_STATE_READY: {
      gboolean shown = update_preview(self);
      state_text = _("Downloaded");
      /* A photo that is not PNG or JPEG (or too large to decode) stays a
       * card: say so rather than show nothing. */
      const gchar *note = NULL;
      if (!shown && self->mime && g_str_has_prefix(self->mime, "image/")) {
        /* W25 review L4: no nudge to open what isn't the image it claims. */
        const gchar *sniffed = gh_attachment_card_sniff_extension(
          gh_attachment_transfer_get_plaintext(self->transfer));
        gboolean image = sniffed && (g_str_equal(sniffed, "jpg") || g_str_equal(sniffed, "png") ||
                                     g_str_equal(sniffed, "gif") || g_str_equal(sniffed, "webp"));
        note = image ? _("This photo can't be shown here. Save it to open it.")
                     : _("This file isn't the photo it says it is, so it isn't shown.");
      }
      show_status(self, note, FALSE, FALSE);
      show_buttons(self, FALSE, FALSE, FALSE, TRUE);
      break;
    }
    case GH_ATTACHMENT_STATE_FAILED:
    default: {
      const gchar *error = gh_attachment_transfer_get_error(self->transfer);
      state_owned = g_strdup_printf(_("Not downloaded: %s"), error ? error : "");
      state_text = state_owned;
      show_status(self, error, FALSE, TRUE);
      show_buttons(self, FALSE, FALSE, gh_attachment_transfer_get_can_retry(self->transfer),
                   FALSE);
      update_preview(self);
      break;
    }
    }
  }
  keep_focus(self, had_focus);

  if (self->transfer && self->shown == GH_ATTACHMENT_STATE_DOWNLOADING &&
      state != GH_ATTACHMENT_STATE_DOWNLOADING) {
    if (state == GH_ATTACHMENT_STATE_READY) {
      /* TRANSLATORS: announced when a download finishes: "Photo downloaded". */
      g_autofree gchar *done = g_strdup_printf(_("%s downloaded"), kind);
      announce(self, done);
    } else if (state == GH_ATTACHMENT_STATE_FAILED) {
      announce(self, gh_attachment_transfer_get_error(self->transfer));
    }
  }
  self->shown = state;

  if (!self->described) {
    set_summary(self, g_strdup(""));
    return;
  }
  g_autofree gchar *detail = g_strdup(gtk_label_get_text(self->detail_label));
  GString *summary = g_string_new(kind);
  if (*detail)
    g_string_append_printf(summary, ", %s", detail);
  if (state_text)
    g_string_append_printf(summary, ". %s.", state_text);
  set_summary(self, g_string_free(summary, FALSE));
}

static void
update_download_note(GhAttachmentCard *self)
{
  Provider *provider = self->transfer ? find_provider(self) : NULL;
  g_autofree gchar *note = provider && provider->vtable.download_note
                             ? provider->vtable.download_note(self->transfer, provider->data)
                             : NULL;
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->download_button), note);
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->retry_button),
                              note ? note : _("Download this file again"));
  if (note)
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->download_button),
                                   GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, note, -1);
  else
    gtk_accessible_reset_property(GTK_ACCESSIBLE(self->download_button),
                                  GTK_ACCESSIBLE_PROPERTY_DESCRIPTION);
}

static void
on_transfer_notify(GhAttachmentCard *self)
{
  update(self);
}

static void
unbind_transfer(GhAttachmentCard *self)
{
  if (!self->transfer)
    return;
  g_signal_handlers_disconnect_by_data(self->transfer, self);
  g_clear_object(&self->transfer);
}

/* The transfer of the message, from the provider of the window the card is
 * in (fetches nothing). */
static void
bind_transfer(GhAttachmentCard *self)
{
  Provider *provider = self->message && self->described && gtk_widget_get_root(GTK_WIDGET(self))
                         ? find_provider(self) : NULL;
  GhAttachmentTransfer *transfer = provider ? provider_lookup(provider, self) : NULL;
  if (transfer != self->transfer) {
    unbind_transfer(self);
    if (transfer) {
      self->transfer = g_object_ref(transfer);
      g_signal_connect_object(transfer, "notify", G_CALLBACK(on_transfer_notify), self,
                              G_CONNECT_SWAPPED);
    }
    self->shown = self->transfer ? gh_attachment_transfer_get_state(self->transfer)
                                 : GH_ATTACHMENT_STATE_IDLE;
  }
  update_download_note(self);
  update(self);
}

/* ---- actions --------------------------------------------------------------------------- */

/* Each action asks the provider again: the transfer may have been replaced
 * (another account's storage opened) since the card was bound. */
static Provider *
act_prepare(GhAttachmentCard *self)
{
  bind_transfer(self);
  return self->transfer ? find_provider(self) : NULL;
}

static void
action_download(GtkWidget *widget, const char *name, GVariant *parameter)
{
  GhAttachmentCard *self = GH_ATTACHMENT_CARD(widget);
  (void)name;
  (void)parameter;
  Provider *provider = act_prepare(self);
  if (provider)
    provider->vtable.download(self->transfer, provider->data);
}

static void
action_cancel(GtkWidget *widget, const char *name, GVariant *parameter)
{
  GhAttachmentCard *self = GH_ATTACHMENT_CARD(widget);
  (void)name;
  (void)parameter;
  Provider *provider = act_prepare(self);
  if (provider)
    provider->vtable.cancel(self->transfer, provider->data);
}

static void
action_save(GtkWidget *widget, const char *name, GVariant *parameter)
{
  GhAttachmentCard *self = GH_ATTACHMENT_CARD(widget);
  (void)name;
  (void)parameter;
  Provider *provider = act_prepare(self);
  if (provider &&
      gh_attachment_transfer_get_state(self->transfer) == GH_ATTACHMENT_STATE_READY)
    provider->vtable.save(self->transfer, GTK_WIDGET(self), provider->data);
}

/* ---- GObject ------------------------------------------------------------------------- */

GtkWidget *
gh_attachment_card_new(void)
{
  return g_object_new(GH_TYPE_ATTACHMENT_CARD, NULL);
}

void
gh_attachment_card_set_message(GhAttachmentCard *self, GhMessage *message)
{
  g_return_if_fail(GH_IS_ATTACHMENT_CARD(self));
  g_return_if_fail(!message || GH_IS_MESSAGE(message));
  if (self->message == message)
    return;
  unbind_transfer(self);
  g_set_object(&self->message, message);
  describe(self);
  bind_transfer(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_MESSAGE]);
}

void
gh_attachment_card_set_index(GhAttachmentCard *self, guint index)
{
  g_return_if_fail(GH_IS_ATTACHMENT_CARD(self));
  if (self->index == index)
    return;
  unbind_transfer(self);
  self->index = index;
  describe(self);
  bind_transfer(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_INDEX]);
}

guint
gh_attachment_card_get_index(GhAttachmentCard *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENT_CARD(self), 0);
  return self->index;
}

GhMessage *
gh_attachment_card_get_message(GhAttachmentCard *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENT_CARD(self), NULL);
  return self->message;
}

void
gh_attachment_card_set_compact(GhAttachmentCard *self, gboolean compact)
{
  g_return_if_fail(GH_IS_ATTACHMENT_CARD(self));
  if (self->compact == !!compact)
    return;
  self->compact = !!compact;
  update_preview(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_COMPACT]);
}

GhAttachmentTransfer *
gh_attachment_card_get_transfer(GhAttachmentCard *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENT_CARD(self), NULL);
  return self->transfer;
}

const gchar *
gh_attachment_card_get_summary(GhAttachmentCard *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENT_CARD(self), NULL);
  return self->summary;
}

static void
gh_attachment_card_root(GtkWidget *widget)
{
  GTK_WIDGET_CLASS(gh_attachment_card_parent_class)->root(widget);
  bind_transfer(GH_ATTACHMENT_CARD(widget));
}

static void
gh_attachment_card_unroot(GtkWidget *widget)
{
  unbind_transfer(GH_ATTACHMENT_CARD(widget));
  GTK_WIDGET_CLASS(gh_attachment_card_parent_class)->unroot(widget);
}

static void
gh_attachment_card_get_property(GObject *object, guint prop_id, GValue *value,
                                GParamSpec *pspec)
{
  GhAttachmentCard *self = GH_ATTACHMENT_CARD(object);
  switch (prop_id) {
  case PROP_MESSAGE: g_value_set_object(value, self->message); break;
  case PROP_INDEX: g_value_set_uint(value, self->index); break;
  case PROP_COMPACT: g_value_set_boolean(value, self->compact); break;
  case PROP_SUMMARY: g_value_set_string(value, self->summary); break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
gh_attachment_card_set_property(GObject *object, guint prop_id, const GValue *value,
                                GParamSpec *pspec)
{
  GhAttachmentCard *self = GH_ATTACHMENT_CARD(object);
  switch (prop_id) {
  case PROP_MESSAGE: gh_attachment_card_set_message(self, g_value_get_object(value)); break;
  case PROP_INDEX: gh_attachment_card_set_index(self, g_value_get_uint(value)); break;
  case PROP_COMPACT: gh_attachment_card_set_compact(self, g_value_get_boolean(value)); break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
gh_attachment_card_dispose(GObject *object)
{
  GhAttachmentCard *self = GH_ATTACHMENT_CARD(object);
  unbind_transfer(self);
  g_clear_object(&self->message);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_ATTACHMENT_CARD);
  /* The template's unnamed children too. */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(object))))
    gtk_widget_unparent(child);
  G_OBJECT_CLASS(gh_attachment_card_parent_class)->dispose(object);
}

static void
gh_attachment_card_finalize(GObject *object)
{
  GhAttachmentCard *self = GH_ATTACHMENT_CARD(object);
  g_free(self->mime);
  g_free(self->summary);
  G_OBJECT_CLASS(gh_attachment_card_parent_class)->finalize(object);
}

static void
gh_attachment_card_class_init(GhAttachmentCardClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  object_class->get_property = gh_attachment_card_get_property;
  object_class->set_property = gh_attachment_card_set_property;
  object_class->dispose = gh_attachment_card_dispose;
  object_class->finalize = gh_attachment_card_finalize;
  widget_class->root = gh_attachment_card_root;
  widget_class->unroot = gh_attachment_card_unroot;

  const GParamFlags rw = G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS;
  props[PROP_MESSAGE] = g_param_spec_object("message", NULL, NULL, GH_TYPE_MESSAGE, rw);
  props[PROP_INDEX] = g_param_spec_uint("index", NULL, NULL, 0, G_MAXUINT, 0, rw);
  props[PROP_COMPACT] = g_param_spec_boolean("compact", NULL, NULL, FALSE, rw);
  props[PROP_SUMMARY] = g_param_spec_string("summary", NULL, NULL, "",
    G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, N_PROPS, props);

  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-attachment-card.ui");
#define BIND(name) gtk_widget_class_bind_template_child(widget_class, GhAttachmentCard, name)
  BIND(type_icon);
  BIND(title_label);
  BIND(detail_label);
  BIND(preview);
  BIND(status_box);
  BIND(spinner);
  BIND(status_icon);
  BIND(status_label);
  BIND(actions);
  BIND(download_button);
  BIND(cancel_button);
  BIND(retry_button);
  BIND(save_button);
#undef BIND
  gtk_widget_class_install_action(widget_class, "attachment.download", NULL, action_download);
  gtk_widget_class_install_action(widget_class, "attachment.cancel", NULL, action_cancel);
  gtk_widget_class_install_action(widget_class, "attachment.save", NULL, action_save);
  gtk_widget_class_set_css_name(widget_class, "groundhog-attachment");
  gtk_widget_class_set_accessible_role(widget_class, GTK_ACCESSIBLE_ROLE_GROUP);
}

static void
gh_attachment_card_init(GhAttachmentCard *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  gtk_widget_set_overflow(GTK_WIDGET(self->preview), GTK_OVERFLOW_HIDDEN);
  self->summary = g_strdup("");
  update(self);
}
