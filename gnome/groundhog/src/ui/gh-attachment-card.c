#include "gh-attachment-card.h"
#include "gh-message-row.h"

#include <glib/gi18n.h>
#include <nostr-gtk-1.0/gn-animated-image.h>
#include <nostr-gtk-1.0/gn-media-viewer.h>
#include <string.h>

#define PROVIDER_DATA "groundhog-attachment-card-provider"
/* Set on a transfer whose plaintext GTK couldn't decode: not tried again. */
#define UNDECODABLE_DATA "groundhog-attachment-undecodable"

/* The inline photo's largest size (charter §7.6: bubbles are capped). */
#define PREVIEW_MAX 280
#define PREVIEW_MAX_COMPACT 200
/* GH_ATTACHMENT_PREVIEW_MAX_DIMENSION (gh-attachment.h, AT-4), for GIFs. */
#define ANIMATION_MAX_DIMENSION 8192
#define ANIMATION_DATA "groundhog-attachment-animation"
#define ANIMATION_BYTES_DATA "groundhog-attachment-animation-bytes"

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
  GtkBox *audio_slot;
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
  GtkMediaStream *audio_stream;
  GBytes *audio_bytes;
  GtkWidget *video_button;         /* "Play Video" for a downloaded video */
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
  /* The stem's own dots never become the extension (W25 re-review R1):
   * without one of ours, "holiday.desktop.bin" would be saved as
   * "holiday.desktop", so every dot left in the stem goes; with one, the
   * last extension is ours and only a leading dot (a hidden file) goes. */
  for (gchar *c = stem; *c; c++)
    if (*c == '.' && (!ext || c == stem))
      *c = '_';
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

static void
hide_preview(GhAttachmentCard *self)
{
  gtk_picture_set_paintable(self->preview, NULL);
  gn_animated_image_attach(GTK_WIDGET(self->preview), NULL);
  gtk_widget_set_visible(GTK_WIDGET(self->preview), FALSE);
}

/* nostrc-p15n5.8: an animated GIF plays inline. It is decoded by
 * GnAnimatedImage (nostr-gtk's bounded GIF decoder, never gdk-pixbuf),
 * whose header check uses the same limit as the PNG/JPEG decode guard;
 * a still or damaged GIF stays a card as before. Kept on the transfer (not
 * as its preview: that is the PNG/JPEG guard's) for these plaintext bytes. */
static GnAnimatedImage *
animated_preview(GhAttachmentTransfer *transfer)
{
  GBytes *plaintext = gh_attachment_transfer_get_plaintext(transfer);
  if (plaintext && g_object_get_data(G_OBJECT(transfer), ANIMATION_BYTES_DATA) == plaintext)
    return g_object_get_data(G_OBJECT(transfer), ANIMATION_DATA);
  if (!plaintext || !gn_animated_image_probe(plaintext, NULL, NULL) ||
      g_object_get_data(G_OBJECT(transfer), UNDECODABLE_DATA) == plaintext)
    return NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GnAnimatedImage) animation =
    gn_animated_image_new_from_bytes(plaintext, ANIMATION_MAX_DIMENSION, &error);
  if (!animation) {
    g_debug("Attachment: no animation shown: %s", error->message);
    g_object_set_data(G_OBJECT(transfer), UNDECODABLE_DATA, plaintext);
    return NULL;
  }
  g_object_set_data_full(G_OBJECT(transfer), ANIMATION_DATA, g_object_ref(animation),
                         g_object_unref);
  g_object_set_data_full(G_OBJECT(transfer), ANIMATION_BYTES_DATA, g_bytes_ref(plaintext),
                         (GDestroyNotify)g_bytes_unref);
  return animation; /* the transfer holds it */
}

/* The photo, decoded once per transfer, and only after the decode guard. */
static gboolean
update_preview(GhAttachmentCard *self)
{
  GhAttachmentTransfer *transfer = self->transfer;
  if (!transfer || gh_attachment_transfer_get_state(transfer) != GH_ATTACHMENT_STATE_READY) {
    hide_preview(self);
    return FALSE;
  }
  GObject *preview = gh_attachment_transfer_get_preview(transfer);
  GBytes *plaintext = gh_attachment_transfer_get_plaintext(transfer);
  if (!gh_attachment_transfer_get_previewable(transfer)) {
    GnAnimatedImage *animation = animated_preview(transfer);
    if (!animation) {
      hide_preview(self);
      return FALSE;
    }
    preview = G_OBJECT(animation);
  } else if (!GDK_IS_TEXTURE(preview)) {
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
      hide_preview(self);
      return FALSE;
    }
    /* Kept on the transfer for recycled rows; this card updates itself. */
    g_signal_handlers_block_by_func(transfer, on_transfer_notify, self);
    gh_attachment_transfer_set_preview(transfer, G_OBJECT(texture));
    g_signal_handlers_unblock_by_func(transfer, on_transfer_notify, self);
    preview = G_OBJECT(texture);
  }
  GdkPaintable *paintable = GDK_PAINTABLE(preview);
  gint width = MAX(gdk_paintable_get_intrinsic_width(paintable), 1);
  gint height = MAX(gdk_paintable_get_intrinsic_height(paintable), 1);
  gint max = self->compact ? PREVIEW_MAX_COMPACT : PREVIEW_MAX;
  gdouble scale = MIN(1.0, MIN((gdouble)max / width, (gdouble)max / height));
  gtk_widget_set_size_request(GTK_WIDGET(self->preview), MAX((gint)(width * scale), 1),
                              MAX((gint)(height * scale), 1));
  gtk_picture_set_paintable(self->preview, paintable);
  gn_animated_image_attach(GTK_WIDGET(self->preview), paintable);
  g_autofree gchar *sender = self->message ? gh_message_row_sender_name(self->message) : NULL;
  g_autofree gchar *alternative = g_strdup_printf(_("Photo from %s"), sender ? sender : "");
  gtk_picture_set_alternative_text(self->preview, alternative);
  gtk_widget_set_visible(GTK_WIDGET(self->preview), TRUE);
  return TRUE;
}

/* ---- the viewer (nostrc-p15n5.6) ------------------------------------------------- */

static gboolean
is_video(const gchar *mime)
{
  return mime && g_ascii_strncasecmp(mime, "video/", 6) == 0 && mime[6];
}

/* What the viewer shows for transfer: its decoded photo or animation, or a
 * video played from the decrypted bytes in memory (new reference), or NULL. */
static GdkPaintable *
viewer_paintable(GhAttachmentTransfer *transfer, const gchar *mime)
{
  if (!transfer || gh_attachment_transfer_get_state(transfer) != GH_ATTACHMENT_STATE_READY)
    return NULL;
  GObject *preview = gh_attachment_transfer_get_preview(transfer);
  if (GDK_IS_PAINTABLE(preview))
    return GDK_PAINTABLE(g_object_ref(preview));
  GnAnimatedImage *animation = animated_preview(transfer);
  if (animation)
    return GDK_PAINTABLE(g_object_ref(animation));
  GBytes *plaintext = gh_attachment_transfer_get_plaintext(transfer);
  if (plaintext && is_video(mime)) {
    g_autoptr(GInputStream) input = g_memory_input_stream_new_from_bytes(plaintext);
    return GDK_PAINTABLE(gtk_media_file_new_for_input_stream(input));
  }
  return NULL;
}

static const gchar *
file_mime(GhMessage *message, guint index, GhNip17File *file)
{
  if (file)
    return file->file_type;
  const GhMessageAttachment *a = gh_message_get_attachment(message, index);
  return a ? a->media_type : NULL;
}

GtkWindow *
gh_attachment_card_open_viewer(GhAttachmentCard *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENT_CARD(self), NULL);
  g_autoptr(GdkPaintable) shown = viewer_paintable(self->transfer, self->mime);
  if (!shown)
    return NULL;
  /* The gallery: every file of this message that is ready to show - a
   * kind-15 message's one file, an encrypted group message's several. */
  Provider *provider = find_provider(self);
  g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
  g_autoptr(GPtrArray) paintables = g_ptr_array_new_with_free_func(g_object_unref);
  guint current = 0;
  guint n = gh_message_get_n_attachments(self->message);
  g_autoptr(GhNip17File) file = gh_message_dup_file(self->message);
  if (file || n == 0)
    n = 1;
  const gchar *id = gh_message_get_rumor_id(self->message);
  for (guint i = 0; i < n && i < 64; i++) {
    g_autoptr(GdkPaintable) paintable = NULL;
    if (i == self->index) {
      paintable = g_object_ref(shown);
      current = paintables->len;
    } else if (provider) {
      GhAttachmentTransfer *transfer = provider->vtable.lookup_at
        ? provider->vtable.lookup_at(self->message, i, provider->data)
        : NULL;
      paintable = viewer_paintable(transfer, file_mime(self->message, i, file));
    }
    if (!paintable)
      continue;
    /* Slot names only; the viewer never fetches them. */
    g_ptr_array_add(urls, g_strdup_printf("attachment:%s/%u", id ? id : "", i));
    g_ptr_array_add(paintables, g_steal_pointer(&paintable));
  }
  g_ptr_array_add(urls, NULL);
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  GnMediaViewer *viewer = gn_media_viewer_new(GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL);
  gn_media_viewer_set_gallery(viewer, (const gchar *const *)urls->pdata, current);
  for (guint i = 0; i < paintables->len; i++)
    gn_media_viewer_set_paintable(viewer, i, g_ptr_array_index(paintables, i));
  gtk_window_present(GTK_WINDOW(viewer));
  return GTK_WINDOW(viewer);
}

static void
action_view(GtkWidget *widget, const char *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  gh_attachment_card_open_viewer(GH_ATTACHMENT_CARD(widget));
}

static void
on_preview_pressed(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y,
                   GhAttachmentCard *self)
{
  (void)gesture; (void)n_press; (void)x; (void)y;
  gh_attachment_card_open_viewer(self);
}

static gboolean
on_preview_key(GtkEventControllerKey *controller, guint keyval, guint keycode,
               GdkModifierType state, GhAttachmentCard *self)
{
  (void)controller; (void)keycode; (void)state;
  if (keyval != GDK_KEY_Return && keyval != GDK_KEY_KP_Enter && keyval != GDK_KEY_space)
    return FALSE;
  return gh_attachment_card_open_viewer(self) != NULL;
}

static void
update_video(GhAttachmentCard *self)
{
  gboolean ready = self->transfer && is_video(self->mime) &&
    gh_attachment_transfer_get_state(self->transfer) == GH_ATTACHMENT_STATE_READY &&
    gh_attachment_transfer_get_plaintext(self->transfer);
  if (ready && !self->video_button) {
    self->video_button = gtk_button_new_with_mnemonic(_("_Play Video"));
    gtk_widget_set_name(self->video_button, "video_button");
    gtk_widget_set_halign(self->video_button, GTK_ALIGN_START);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(self->video_button), "attachment.view");
    gtk_widget_insert_after(self->video_button,
                            gtk_widget_get_parent(GTK_WIDGET(self->audio_slot)),
                            GTK_WIDGET(self->audio_slot));
  }
  if (self->video_button)
    gtk_widget_set_visible(self->video_button, ready);
}

/* Playback uses the decrypted bytes in memory, never a temporary plaintext
 * file.  Recycled cards and cleared transfers stop playback immediately. */
static void
update_audio(GhAttachmentCard *self)
{
  GBytes *plaintext = self->transfer &&
    gh_attachment_transfer_get_state(self->transfer) == GH_ATTACHMENT_STATE_READY &&
    self->mime && g_ascii_strncasecmp(self->mime, "audio/", 6) == 0 && self->mime[6]
      ? gh_attachment_transfer_get_plaintext(self->transfer) : NULL;
  if (plaintext == self->audio_bytes)
    return;
  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->audio_slot));
  if (child)
    gtk_box_remove(self->audio_slot, child);
  if (self->audio_stream) {
    gtk_media_stream_pause(self->audio_stream);
    gtk_media_file_clear(GTK_MEDIA_FILE(self->audio_stream));
    g_clear_object(&self->audio_stream);
  }
  g_clear_pointer(&self->audio_bytes, g_bytes_unref);
  if (plaintext) {
    g_autoptr(GInputStream) input = g_memory_input_stream_new_from_bytes(plaintext);
    self->audio_stream = gtk_media_file_new_for_input_stream(input);
    self->audio_bytes = g_bytes_ref(plaintext);
    GtkWidget *controls = gtk_media_controls_new(self->audio_stream);
    gtk_widget_set_size_request(controls, self->compact ? 200 : 280, -1);
    gtk_box_append(self->audio_slot, controls);
  }
  gtk_widget_set_visible(GTK_WIDGET(self->audio_slot), plaintext != NULL);
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
  update_audio(self);
  update_video(self);
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
  update_audio(self);
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
  gh_attachment_card_maybe_auto_download(self);
}

void
gh_attachment_card_maybe_auto_download(GhAttachmentCard *self)
{
  g_return_if_fail(GH_IS_ATTACHMENT_CARD(self));
  if (!self->transfer || !self->message || !gtk_widget_get_mapped(GTK_WIDGET(self)))
    return;
  Provider *provider = find_provider(self);
  if (provider && provider->vtable.auto_download)
    provider->vtable.auto_download(GTK_WIDGET(self), self->message, self->index,
                                   self->transfer, provider->data);
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
  GtkWidget *controls = gtk_widget_get_first_child(GTK_WIDGET(self->audio_slot));
  if (controls)
    gtk_widget_set_size_request(controls, self->compact ? 200 : 280, -1);
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
gh_attachment_card_map(GtkWidget *widget)
{
  GTK_WIDGET_CLASS(gh_attachment_card_parent_class)->map(widget);
  gh_attachment_card_maybe_auto_download(GH_ATTACHMENT_CARD(widget));
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
  if (self->audio_stream) {
    gtk_media_stream_pause(self->audio_stream);
    gtk_media_file_clear(GTK_MEDIA_FILE(self->audio_stream));
    g_clear_object(&self->audio_stream);
  }
  g_clear_pointer(&self->audio_bytes, g_bytes_unref);
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
  widget_class->map = gh_attachment_card_map;
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
  BIND(audio_slot);
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
  gtk_widget_class_install_action(widget_class, "attachment.view", NULL, action_view);
  gtk_widget_class_set_css_name(widget_class, "groundhog-attachment");
  gtk_widget_class_set_accessible_role(widget_class, GTK_ACCESSIBLE_ROLE_GROUP);
}

static void
gh_attachment_card_init(GhAttachmentCard *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  gtk_widget_set_overflow(GTK_WIDGET(self->preview), GTK_OVERFLOW_HIDDEN);
  /* nostrc-p15n5.6: the photo opens the media viewer (click, Enter, Space). */
  gtk_widget_set_focusable(GTK_WIDGET(self->preview), TRUE);
  gtk_widget_set_cursor_from_name(GTK_WIDGET(self->preview), "zoom-in");
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->preview), _("Open in media viewer"));
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->preview),
                                 GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                 _("Open in media viewer"), -1);
  GtkGesture *open = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(open), GDK_BUTTON_PRIMARY);
  g_signal_connect(open, "pressed", G_CALLBACK(on_preview_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self->preview), GTK_EVENT_CONTROLLER(open));
  GtkEventController *keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_preview_key), self);
  gtk_widget_add_controller(GTK_WIDGET(self->preview), keys);
  self->summary = g_strdup("");
  update(self);
}
