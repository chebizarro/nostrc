#include "gnostr-composer.h"
#include "../util/blossom.h"
#include "../util/blossom_settings.h"

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-composer.ui"

struct _GnostrComposer {
  GtkWidget parent_instance;
  GtkWidget *root;
  GtkWidget *text_view; /* bound as widget; cast to GtkTextView when used */
  GtkWidget *btn_post;
  GtkWidget *btn_attach;              /* attachment/upload button */
  GtkWidget *reply_indicator_box;     /* container for reply indicator */
  GtkWidget *reply_indicator;         /* label showing "Replying to @user" */
  GtkWidget *btn_cancel_reply;        /* button to cancel reply mode */
  GtkWidget *upload_progress_box;     /* container for upload progress */
  GtkWidget *upload_spinner;          /* spinner during upload */
  GtkWidget *upload_status_label;     /* upload status text */
  /* Reply context for NIP-10 threading */
  char *reply_to_id;       /* event ID being replied to (hex) */
  char *root_id;           /* thread root event ID (hex), may equal reply_to_id */
  char *reply_to_pubkey;   /* pubkey of author being replied to (hex) */
  /* Quote context for NIP-18 quote posts */
  char *quote_id;          /* event ID being quoted (hex) */
  char *quote_pubkey;      /* pubkey of author being quoted (hex) */
  char *quote_nostr_uri;   /* nostr:note1... URI for the quoted note */
  /* Upload state */
  GCancellable *upload_cancellable;   /* cancellable for ongoing upload */
  gboolean upload_in_progress;        /* TRUE while uploading */
};

G_DEFINE_TYPE(GnostrComposer, gnostr_composer, GTK_TYPE_WIDGET)

enum {
  SIGNAL_POST_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = {0};

static void gnostr_composer_dispose(GObject *obj) {
  /* Dispose template children before chaining up so they are unparented first */
  gtk_widget_dispose_template(GTK_WIDGET(obj), GNOSTR_TYPE_COMPOSER);
  GnostrComposer *self = GNOSTR_COMPOSER(obj);
  self->root = NULL;
  self->text_view = NULL;
  self->btn_post = NULL;
  self->btn_attach = NULL;
  self->reply_indicator_box = NULL;
  self->reply_indicator = NULL;
  self->btn_cancel_reply = NULL;
  self->upload_progress_box = NULL;
  self->upload_spinner = NULL;
  self->upload_status_label = NULL;
  G_OBJECT_CLASS(gnostr_composer_parent_class)->dispose(obj);
}

static void gnostr_composer_finalize(GObject *obj) {
  GnostrComposer *self = GNOSTR_COMPOSER(obj);
  g_clear_pointer(&self->reply_to_id, g_free);
  g_clear_pointer(&self->root_id, g_free);
  g_clear_pointer(&self->reply_to_pubkey, g_free);
  g_clear_pointer(&self->quote_id, g_free);
  g_clear_pointer(&self->quote_pubkey, g_free);
  g_clear_pointer(&self->quote_nostr_uri, g_free);
  if (self->upload_cancellable) {
    g_cancellable_cancel(self->upload_cancellable);
    g_object_unref(self->upload_cancellable);
    self->upload_cancellable = NULL;
  }
  G_OBJECT_CLASS(gnostr_composer_parent_class)->finalize(obj);
}

static void on_post_clicked(GnostrComposer *self, GtkButton *button) {
  (void)button;
  // Read text from GtkTextView and emit signal to controller
  if (!self || !GTK_IS_WIDGET(self)) {
    g_warning("composer instance invalid in on_post_clicked");
    return;
  }
  if (!self->text_view || !GTK_IS_TEXT_VIEW(self->text_view)) {
    g_warning("composer text_view not ready (self=%p root=%p text_view=%p btn_post=%p)",
              (void*)self,
              (void*)self->root,
              (void*)self->text_view,
              (void*)self->btn_post);
    return;
  }
  GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text_view));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buf, &start, &end);
  char *text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
  if (signals[SIGNAL_POST_REQUESTED] > 0)
    g_signal_emit(self, signals[SIGNAL_POST_REQUESTED], 0, text);
  g_free(text);
}

static void on_cancel_reply_clicked(GnostrComposer *self, GtkButton *button) {
  (void)button;
  if (!GNOSTR_IS_COMPOSER(self)) return;
  gnostr_composer_clear_reply_context(self);
}

/* Blossom upload callback */
static void on_blossom_upload_complete(GnostrBlossomBlob *blob, GError *error, gpointer user_data) {
  GnostrComposer *self = GNOSTR_COMPOSER(user_data);
  if (!GNOSTR_IS_COMPOSER(self)) {
    if (blob) gnostr_blossom_blob_free(blob);
    return;
  }

  self->upload_in_progress = FALSE;

  /* Hide progress indicator */
  if (self->upload_progress_box && GTK_IS_WIDGET(self->upload_progress_box)) {
    gtk_widget_set_visible(self->upload_progress_box, FALSE);
  }
  if (self->upload_spinner && GTK_IS_SPINNER(self->upload_spinner)) {
    gtk_spinner_set_spinning(GTK_SPINNER(self->upload_spinner), FALSE);
  }

  /* Re-enable attach button */
  if (self->btn_attach && GTK_IS_WIDGET(self->btn_attach)) {
    gtk_widget_set_sensitive(self->btn_attach, TRUE);
  }

  if (error) {
    g_warning("Blossom upload failed: %s", error->message);
    /* Show error toast - for now just update status label briefly */
    if (self->upload_status_label && GTK_IS_LABEL(self->upload_status_label)) {
      gtk_label_set_text(GTK_LABEL(self->upload_status_label), "Upload failed");
      gtk_widget_set_visible(self->upload_progress_box, TRUE);
      /* Hide after 3 seconds */
      /* TODO: Use GtkToast when available */
    }
    return;
  }

  if (!blob || !blob->url) {
    g_warning("Blossom upload returned no URL");
    return;
  }

  /* Insert the URL into the text view at cursor position */
  if (self->text_view && GTK_IS_TEXT_VIEW(self->text_view)) {
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text_view));
    GtkTextIter cursor;
    gtk_text_buffer_get_iter_at_mark(buf, &cursor, gtk_text_buffer_get_insert(buf));

    /* Add newline before URL if not at start of line */
    GtkTextIter line_start = cursor;
    gtk_text_iter_set_line_offset(&line_start, 0);
    if (!gtk_text_iter_equal(&cursor, &line_start)) {
      gtk_text_buffer_insert(buf, &cursor, "\n", 1);
    }

    /* Insert the URL */
    gtk_text_buffer_insert(buf, &cursor, blob->url, -1);
    gtk_text_buffer_insert(buf, &cursor, "\n", 1);

    g_message("composer: inserted uploaded media URL: %s", blob->url);
  }

  gnostr_blossom_blob_free(blob);
}

/* File chooser response callback */
static void on_file_chooser_response(GObject *source, GAsyncResult *res, gpointer user_data) {
  GnostrComposer *self = GNOSTR_COMPOSER(user_data);
  GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
  GError *error = NULL;

  GFile *file = gtk_file_dialog_open_finish(dialog, res, &error);

  if (error) {
    if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) &&
        !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) {
      g_warning("File chooser error: %s", error->message);
    }
    g_error_free(error);
    return;
  }

  if (!file) return;

  char *path = g_file_get_path(file);
  g_object_unref(file);

  if (!path) {
    g_warning("Could not get file path");
    return;
  }

  if (!GNOSTR_IS_COMPOSER(self)) {
    g_free(path);
    return;
  }

  /* Get Blossom server URL from settings */
  const char *server_url = gnostr_blossom_settings_get_default_server();
  if (!server_url || !*server_url) {
    g_warning("No Blossom server configured");
    g_free(path);
    return;
  }

  /* Show upload progress */
  self->upload_in_progress = TRUE;
  if (self->upload_progress_box && GTK_IS_WIDGET(self->upload_progress_box)) {
    gtk_widget_set_visible(self->upload_progress_box, TRUE);
  }
  if (self->upload_spinner && GTK_IS_SPINNER(self->upload_spinner)) {
    gtk_spinner_set_spinning(GTK_SPINNER(self->upload_spinner), TRUE);
  }
  if (self->upload_status_label && GTK_IS_LABEL(self->upload_status_label)) {
    gtk_label_set_text(GTK_LABEL(self->upload_status_label), "Uploading...");
  }

  /* Disable attach button during upload */
  if (self->btn_attach && GTK_IS_WIDGET(self->btn_attach)) {
    gtk_widget_set_sensitive(self->btn_attach, FALSE);
  }

  /* Create cancellable for this upload */
  if (self->upload_cancellable) {
    g_object_unref(self->upload_cancellable);
  }
  self->upload_cancellable = g_cancellable_new();

  /* Start async upload */
  g_message("composer: starting upload of %s to %s", path, server_url);
  gnostr_blossom_upload_async(server_url, path, NULL,
                               on_blossom_upload_complete, self,
                               self->upload_cancellable);
  g_free(path);
}

/* Attach button clicked - open file chooser */
static void on_attach_clicked(GnostrComposer *self, GtkButton *button) {
  (void)button;
  if (!GNOSTR_IS_COMPOSER(self)) return;

  /* Don't allow another upload while one is in progress */
  if (self->upload_in_progress) {
    g_message("composer: upload already in progress");
    return;
  }

  GtkFileDialog *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Select Media to Upload");
  gtk_file_dialog_set_modal(dialog, TRUE);

  /* Set up file filters for images and videos */
  GtkFileFilter *filter_images = gtk_file_filter_new();
  gtk_file_filter_set_name(filter_images, "Images");
  gtk_file_filter_add_mime_type(filter_images, "image/png");
  gtk_file_filter_add_mime_type(filter_images, "image/jpeg");
  gtk_file_filter_add_mime_type(filter_images, "image/gif");
  gtk_file_filter_add_mime_type(filter_images, "image/webp");
  gtk_file_filter_add_mime_type(filter_images, "image/avif");

  GtkFileFilter *filter_video = gtk_file_filter_new();
  gtk_file_filter_set_name(filter_video, "Videos");
  gtk_file_filter_add_mime_type(filter_video, "video/mp4");
  gtk_file_filter_add_mime_type(filter_video, "video/webm");
  gtk_file_filter_add_mime_type(filter_video, "video/quicktime");

  GtkFileFilter *filter_all_media = gtk_file_filter_new();
  gtk_file_filter_set_name(filter_all_media, "All Media");
  gtk_file_filter_add_mime_type(filter_all_media, "image/*");
  gtk_file_filter_add_mime_type(filter_all_media, "video/*");

  GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
  g_list_store_append(filters, filter_all_media);
  g_list_store_append(filters, filter_images);
  g_list_store_append(filters, filter_video);

  gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
  gtk_file_dialog_set_default_filter(dialog, filter_all_media);

  /* Get the window for the dialog */
  GtkWidget *toplevel = GTK_WIDGET(self);
  while (toplevel && !GTK_IS_WINDOW(toplevel)) {
    toplevel = gtk_widget_get_parent(toplevel);
  }

  gtk_file_dialog_open(dialog, GTK_WINDOW(toplevel), NULL,
                       on_file_chooser_response, self);

  g_object_unref(filters);
  g_object_unref(filter_images);
  g_object_unref(filter_video);
  g_object_unref(filter_all_media);
  g_object_unref(dialog);
}

static void gnostr_composer_class_init(GnostrComposerClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *gobj_class = G_OBJECT_CLASS(klass);
  gobj_class->dispose = gnostr_composer_dispose;
  gobj_class->finalize = gnostr_composer_finalize;
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);
  gtk_widget_class_bind_template_child(widget_class, GnostrComposer, root);
  gtk_widget_class_bind_template_child(widget_class, GnostrComposer, text_view);
  gtk_widget_class_bind_template_child(widget_class, GnostrComposer, btn_post);
  gtk_widget_class_bind_template_child(widget_class, GnostrComposer, btn_attach);
  gtk_widget_class_bind_template_child(widget_class, GnostrComposer, reply_indicator_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrComposer, reply_indicator);
  gtk_widget_class_bind_template_child(widget_class, GnostrComposer, btn_cancel_reply);
  gtk_widget_class_bind_template_child(widget_class, GnostrComposer, upload_progress_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrComposer, upload_spinner);
  gtk_widget_class_bind_template_child(widget_class, GnostrComposer, upload_status_label);
  gtk_widget_class_bind_template_callback(widget_class, on_post_clicked);
  gtk_widget_class_bind_template_callback(widget_class, on_cancel_reply_clicked);
  gtk_widget_class_bind_template_callback(widget_class, on_attach_clicked);

  signals[SIGNAL_POST_REQUESTED] =
      g_signal_new("post-requested",
                   G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST,
                   0, /* class offset */
                   NULL, NULL,
                   g_cclosure_marshal_VOID__STRING,
                   G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void gnostr_composer_init(GnostrComposer *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->text_view),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Composer", -1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_post),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Composer Post", -1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_cancel_reply),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Composer Cancel Reply", -1);
  if (self->btn_attach) {
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_attach),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, "Composer Attach Media", -1);
  }
  self->upload_in_progress = FALSE;
  self->upload_cancellable = NULL;
  g_message("composer init: self=%p root=%p text_view=%p btn_post=%p btn_attach=%p",
            (void*)self,
            (void*)self->root,
            (void*)self->text_view,
            (void*)self->btn_post,
            (void*)self->btn_attach);
}

GtkWidget *gnostr_composer_new(void) {
  return g_object_new(GNOSTR_TYPE_COMPOSER, NULL);
}

void gnostr_composer_clear(GnostrComposer *self) {
  g_return_if_fail(GNOSTR_IS_COMPOSER(self));
  if (!self->text_view || !GTK_IS_TEXT_VIEW(self->text_view)) return;
  GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text_view));
  gtk_text_buffer_set_text(buf, "", 0);
  /* Also clear reply and quote context */
  gnostr_composer_clear_reply_context(self);
  gnostr_composer_clear_quote_context(self);
}

void gnostr_composer_set_reply_context(GnostrComposer *self,
                                       const char *reply_to_id,
                                       const char *root_id,
                                       const char *reply_to_pubkey,
                                       const char *reply_to_display_name) {
  g_return_if_fail(GNOSTR_IS_COMPOSER(self));

  /* Store reply context */
  g_free(self->reply_to_id);
  g_free(self->root_id);
  g_free(self->reply_to_pubkey);

  self->reply_to_id = g_strdup(reply_to_id);
  /* If no root_id provided, use reply_to_id as root (direct reply to root) */
  self->root_id = g_strdup(root_id ? root_id : reply_to_id);
  self->reply_to_pubkey = g_strdup(reply_to_pubkey);

  /* Update reply indicator if present */
  if (self->reply_indicator && GTK_IS_LABEL(self->reply_indicator)) {
    char *label = g_strdup_printf("Replying to %s",
                                   reply_to_display_name ? reply_to_display_name : "@user");
    gtk_label_set_text(GTK_LABEL(self->reply_indicator), label);
    g_free(label);
  }
  /* Show the reply indicator box */
  if (self->reply_indicator_box && GTK_IS_WIDGET(self->reply_indicator_box)) {
    gtk_widget_set_visible(self->reply_indicator_box, TRUE);
  }

  /* Update button label */
  if (self->btn_post && GTK_IS_BUTTON(self->btn_post)) {
    gtk_button_set_label(GTK_BUTTON(self->btn_post), "Reply");
  }

  g_message("composer: set reply context id=%s root=%s pubkey=%s",
            reply_to_id ? reply_to_id : "(null)",
            self->root_id ? self->root_id : "(null)",
            reply_to_pubkey ? reply_to_pubkey : "(null)");
}

void gnostr_composer_clear_reply_context(GnostrComposer *self) {
  g_return_if_fail(GNOSTR_IS_COMPOSER(self));

  g_clear_pointer(&self->reply_to_id, g_free);
  g_clear_pointer(&self->root_id, g_free);
  g_clear_pointer(&self->reply_to_pubkey, g_free);

  /* Hide reply indicator box */
  if (self->reply_indicator_box && GTK_IS_WIDGET(self->reply_indicator_box)) {
    gtk_widget_set_visible(self->reply_indicator_box, FALSE);
  }

  /* Reset button label */
  if (self->btn_post && GTK_IS_BUTTON(self->btn_post)) {
    gtk_button_set_label(GTK_BUTTON(self->btn_post), "Post");
  }
}

gboolean gnostr_composer_is_reply(GnostrComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_COMPOSER(self), FALSE);
  return self->reply_to_id != NULL;
}

const char *gnostr_composer_get_reply_to_id(GnostrComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_COMPOSER(self), NULL);
  return self->reply_to_id;
}

const char *gnostr_composer_get_root_id(GnostrComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_COMPOSER(self), NULL);
  return self->root_id;
}

const char *gnostr_composer_get_reply_to_pubkey(GnostrComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_COMPOSER(self), NULL);
  return self->reply_to_pubkey;
}

/* Quote context for NIP-18 quote posts */
void gnostr_composer_set_quote_context(GnostrComposer *self,
                                       const char *quote_id,
                                       const char *quote_pubkey,
                                       const char *nostr_uri,
                                       const char *quoted_author_display_name) {
  g_return_if_fail(GNOSTR_IS_COMPOSER(self));

  /* Clear any existing reply context first */
  gnostr_composer_clear_reply_context(self);

  /* Store quote context */
  g_free(self->quote_id);
  g_free(self->quote_pubkey);
  g_free(self->quote_nostr_uri);

  self->quote_id = g_strdup(quote_id);
  self->quote_pubkey = g_strdup(quote_pubkey);
  self->quote_nostr_uri = g_strdup(nostr_uri);

  /* Update indicator to show we're quoting */
  if (self->reply_indicator && GTK_IS_LABEL(self->reply_indicator)) {
    char *label = g_strdup_printf("Quoting %s",
                                   quoted_author_display_name ? quoted_author_display_name : "@user");
    gtk_label_set_text(GTK_LABEL(self->reply_indicator), label);
    g_free(label);
  }
  /* Show the indicator box */
  if (self->reply_indicator_box && GTK_IS_WIDGET(self->reply_indicator_box)) {
    gtk_widget_set_visible(self->reply_indicator_box, TRUE);
  }

  /* Update button label */
  if (self->btn_post && GTK_IS_BUTTON(self->btn_post)) {
    gtk_button_set_label(GTK_BUTTON(self->btn_post), "Quote");
  }

  /* Pre-fill text with nostr: URI at the end */
  if (nostr_uri && self->text_view && GTK_IS_TEXT_VIEW(self->text_view)) {
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text_view));
    char *prefill = g_strdup_printf("\n\n%s", nostr_uri);
    gtk_text_buffer_set_text(buf, prefill, -1);
    g_free(prefill);
    /* Move cursor to the start for user to type their comment */
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buf, &start);
    gtk_text_buffer_place_cursor(buf, &start);
  }

  g_message("composer: set quote context id=%s pubkey=%s uri=%s",
            quote_id ? quote_id : "(null)",
            quote_pubkey ? quote_pubkey : "(null)",
            nostr_uri ? nostr_uri : "(null)");
}

void gnostr_composer_clear_quote_context(GnostrComposer *self) {
  g_return_if_fail(GNOSTR_IS_COMPOSER(self));

  g_clear_pointer(&self->quote_id, g_free);
  g_clear_pointer(&self->quote_pubkey, g_free);
  g_clear_pointer(&self->quote_nostr_uri, g_free);

  /* Hide indicator box if no reply context either */
  if (!self->reply_to_id) {
    if (self->reply_indicator_box && GTK_IS_WIDGET(self->reply_indicator_box)) {
      gtk_widget_set_visible(self->reply_indicator_box, FALSE);
    }
    /* Reset button label */
    if (self->btn_post && GTK_IS_BUTTON(self->btn_post)) {
      gtk_button_set_label(GTK_BUTTON(self->btn_post), "Post");
    }
  }
}

gboolean gnostr_composer_is_quote(GnostrComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_COMPOSER(self), FALSE);
  return self->quote_id != NULL;
}

const char *gnostr_composer_get_quote_id(GnostrComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_COMPOSER(self), NULL);
  return self->quote_id;
}

const char *gnostr_composer_get_quote_pubkey(GnostrComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_COMPOSER(self), NULL);
  return self->quote_pubkey;
}

const char *gnostr_composer_get_quote_nostr_uri(GnostrComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_COMPOSER(self), NULL);
  return self->quote_nostr_uri;
}

/* Media upload state */
gboolean gnostr_composer_is_uploading(GnostrComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_COMPOSER(self), FALSE);
  return self->upload_in_progress;
}

void gnostr_composer_cancel_upload(GnostrComposer *self) {
  g_return_if_fail(GNOSTR_IS_COMPOSER(self));

  if (!self->upload_in_progress) return;

  if (self->upload_cancellable) {
    g_cancellable_cancel(self->upload_cancellable);
  }

  self->upload_in_progress = FALSE;

  /* Hide progress indicator */
  if (self->upload_progress_box && GTK_IS_WIDGET(self->upload_progress_box)) {
    gtk_widget_set_visible(self->upload_progress_box, FALSE);
  }
  if (self->upload_spinner && GTK_IS_SPINNER(self->upload_spinner)) {
    gtk_spinner_set_spinning(GTK_SPINNER(self->upload_spinner), FALSE);
  }

  /* Re-enable attach button */
  if (self->btn_attach && GTK_IS_WIDGET(self->btn_attach)) {
    gtk_widget_set_sensitive(self->btn_attach, TRUE);
  }
}
