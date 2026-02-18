#include "gnostr-composer.h"
#include "gnostr-main-window.h"
#include "../util/media_upload.h"
#include "../util/blossom.h"
#include "../util/blossom_settings.h"
#include "../util/gnostr-drafts.h"
#include <time.h>

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-composer.ui"

struct _NostrGtkComposer {
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
  /* NIP-14 Subject input */
  GtkWidget *subject_box;             /* container for subject entry */
  GtkWidget *subject_entry;           /* optional subject text entry */
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
  /* Uploaded media metadata for NIP-92 imeta tags */
  GPtrArray *uploaded_media;          /* array of NostrGtkComposerMedia* */
  /* NIP-40: Expiration timestamp */
  gint64 expiration;                  /* Unix timestamp for expiration (0 = no expiration) */
  /* NIP-36 Content Warning */
  GtkWidget *btn_sensitive;           /* toggle button for sensitive content */
  gboolean is_sensitive;              /* TRUE if note should have content-warning */
  /* NIP-22 Comment context */
  char *comment_root_id;              /* root event ID being commented on (hex) */
  int comment_root_kind;              /* kind of the root event */
  char *comment_root_pubkey;          /* pubkey of root event author (hex) */
  /* NIP-37 Drafts */
  GtkWidget *btn_drafts;              /* menu button for drafts popover */
  GtkWidget *drafts_popover;          /* popover with drafts list */
  GtkWidget *drafts_list;             /* listbox of saved drafts */
  GtkWidget *drafts_empty_label;      /* "No drafts saved" label */
  GtkWidget *btn_save_draft;          /* save draft button */
  char *current_draft_d_tag;          /* d-tag of currently loaded draft (for updates) */
};

G_DEFINE_TYPE(NostrGtkComposer, nostr_gtk_composer, GTK_TYPE_WIDGET)

/* Helper to free a NostrGtkComposerMedia struct */
static void composer_media_free(gpointer p) {
  NostrGtkComposerMedia *m = (NostrGtkComposerMedia *)p;
  if (!m) return;
  g_free(m->url);
  g_free(m->sha256);
  g_free(m->mime_type);
  g_free(m);
}

enum {
  SIGNAL_POST_REQUESTED,
  SIGNAL_DRAFT_SAVED,
  SIGNAL_DRAFT_LOADED,
  SIGNAL_DRAFT_DELETED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = {0};

static void nostr_gtk_composer_dispose(GObject *obj) {
  /* Dispose template children before chaining up so they are unparented first */
  gtk_widget_dispose_template(GTK_WIDGET(obj), NOSTR_GTK_TYPE_COMPOSER);
  NostrGtkComposer *self = NOSTR_GTK_COMPOSER(obj);
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
  /* NIP-14 subject widgets */
  self->subject_box = NULL;
  self->subject_entry = NULL;
  /* NIP-37 drafts widgets */
  self->btn_drafts = NULL;
  self->drafts_popover = NULL;
  self->drafts_list = NULL;
  self->drafts_empty_label = NULL;
  self->btn_save_draft = NULL;
  G_OBJECT_CLASS(nostr_gtk_composer_parent_class)->dispose(obj);
}

static void nostr_gtk_composer_finalize(GObject *obj) {
  NostrGtkComposer *self = NOSTR_GTK_COMPOSER(obj);
  g_clear_pointer(&self->reply_to_id, g_free);
  g_clear_pointer(&self->root_id, g_free);
  g_clear_pointer(&self->reply_to_pubkey, g_free);
  g_clear_pointer(&self->quote_id, g_free);
  g_clear_pointer(&self->quote_pubkey, g_free);
  g_clear_pointer(&self->quote_nostr_uri, g_free);
  /* NIP-22 comment context cleanup */
  g_clear_pointer(&self->comment_root_id, g_free);
  g_clear_pointer(&self->comment_root_pubkey, g_free);
  /* NIP-37 draft context cleanup */
  g_clear_pointer(&self->current_draft_d_tag, g_free);
  if (self->upload_cancellable) {
    g_cancellable_cancel(self->upload_cancellable);
    g_object_unref(self->upload_cancellable);
    self->upload_cancellable = NULL;
  }
  if (self->uploaded_media) {
    g_ptr_array_free(self->uploaded_media, TRUE);
    self->uploaded_media = NULL;
  }
  G_OBJECT_CLASS(nostr_gtk_composer_parent_class)->finalize(obj);
}

static void on_post_clicked(NostrGtkComposer *self, GtkButton *button) {
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

static void on_cancel_reply_clicked(NostrGtkComposer *self, GtkButton *button) {
  (void)button;
  if (!NOSTR_GTK_IS_COMPOSER(self)) return;
  nostr_gtk_composer_clear_reply_context(self);
}

/* Show toast notification via main window */
static void composer_show_toast(NostrGtkComposer *self, const char *message) {
  if (!NOSTR_GTK_IS_COMPOSER(self) || !message) return;

  /* Find the main window by walking up the widget tree */
  GtkWidget *widget = GTK_WIDGET(self);
  while (widget) {
    widget = gtk_widget_get_parent(widget);
    if (widget && G_TYPE_CHECK_INSTANCE_TYPE(widget, gtk_application_window_get_type())) {
      gnostr_main_window_show_toast(widget, message);
      return;
    }
  }
  /* Fallback: just log if no main window found */
  g_warning("composer: could not find main window for toast: %s", message);
}

/* Blossom upload callback */
static void on_blossom_upload_complete(GnostrBlossomBlob *blob, GError *error, gpointer user_data) {
  NostrGtkComposer *self = NOSTR_GTK_COMPOSER(user_data);
  if (!NOSTR_GTK_IS_COMPOSER(self)) {
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
    /* Show error toast via main window toast overlay */
    g_autofree char *toast_msg = g_strdup_printf("Upload failed: %s", error->message);
    composer_show_toast(self, toast_msg);
    return;
  }

  if (!blob || !blob->url) {
    g_warning("Blossom upload returned no URL");
    composer_show_toast(self, "Upload completed but server returned no URL");
    return;
  }

  /* Store media metadata for NIP-92 imeta tags */
  if (!self->uploaded_media) {
    self->uploaded_media = g_ptr_array_new_with_free_func(composer_media_free);
  }
  NostrGtkComposerMedia *media = g_new0(NostrGtkComposerMedia, 1);
  media->url = g_strdup(blob->url);
  media->sha256 = g_strdup(blob->sha256);
  media->mime_type = g_strdup(blob->mime_type);
  media->size = blob->size;
  g_ptr_array_add(self->uploaded_media, media);

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

    g_message("composer: inserted uploaded media URL: %s (sha256=%s, type=%s, size=%" G_GINT64_FORMAT ")",
              blob->url, blob->sha256 ? blob->sha256 : "?",
              blob->mime_type ? blob->mime_type : "?", blob->size);
  }

  gnostr_blossom_blob_free(blob);
}

/* File chooser response callback */
static void on_file_chooser_response(GObject *source, GAsyncResult *res, gpointer user_data) {
  GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
  GError *error = NULL;

  /* Check result BEFORE accessing user_data - if cancelled, composer may be disposed */
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

  /* Now safe to access user_data since dialog wasn't cancelled/dismissed */
  if (user_data == NULL) {
    g_object_unref(file);
    return;
  }
  NostrGtkComposer *self = (NostrGtkComposer*)user_data;
  if (!NOSTR_GTK_IS_COMPOSER(self)) {
    g_object_unref(file);
    return;
  }

  char *path = g_file_get_path(file);
  g_object_unref(file);

  if (!path) {
    g_warning("Could not get file path");
    composer_show_toast(self, "Could not read selected file");
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

  /* nostrc-fs5g: Use unified upload (Blossom with NIP-96 fallback) */
  g_message("composer: starting media upload of %s", path);
  gnostr_media_upload_async(path, NULL,
                             on_blossom_upload_complete, self,
                             self->upload_cancellable);
  g_free(path);
}

/* Attach button clicked - open file chooser */
static void on_attach_clicked(NostrGtkComposer *self, GtkButton *button) {
  (void)button;
  if (!NOSTR_GTK_IS_COMPOSER(self)) return;

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
  gtk_file_filter_add_mime_type(filter_images, "image/svg+xml");
  gtk_file_filter_add_mime_type(filter_images, "image/x-icon");
  gtk_file_filter_add_mime_type(filter_images, "image/vnd.microsoft.icon");
  gtk_file_filter_add_mime_type(filter_images, "image/bmp");
  gtk_file_filter_add_mime_type(filter_images, "image/tiff");

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

  /* Get the window for the dialog.
   * When composer is in an AdwDialog, the parent walk won't find a GtkWindow
   * because AdwDialog is not a GtkWindow. Use gtk_widget_get_root() first,
   * then fall back to walking up the tree. */
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  GtkWindow *parent_window = NULL;

  if (root && GTK_IS_WINDOW(root)) {
    parent_window = GTK_WINDOW(root);
  } else {
    /* Fallback: try to find any active application window */
    GApplication *app = g_application_get_default();
    if (app && GTK_IS_APPLICATION(app)) {
      parent_window = gtk_application_get_active_window(GTK_APPLICATION(app));
    }
  }

  gtk_file_dialog_open(dialog, parent_window, NULL,
                       on_file_chooser_response, self);

  g_object_unref(filters);
  g_object_unref(filter_images);
  g_object_unref(filter_video);
  g_object_unref(filter_all_media);
  g_object_unref(dialog);
}

/* NIP-36: Callback when sensitive toggle button is toggled */
static void on_sensitive_toggled(NostrGtkComposer *self, GtkToggleButton *button) {
  (void)button;
  if (!NOSTR_GTK_IS_COMPOSER(self)) return;

  gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->btn_sensitive));
  self->is_sensitive = active;

  /* Update button styling to indicate active state */
  if (active) {
    gtk_widget_add_css_class(GTK_WIDGET(self->btn_sensitive), "warning");
  } else {
    gtk_widget_remove_css_class(GTK_WIDGET(self->btn_sensitive), "warning");
  }
}

/* ---- NIP-37: Drafts functionality ---- */

/* Forward declarations for draft row click handlers */
static void on_draft_row_load_clicked(GtkButton *btn, gpointer user_data);
static void on_draft_row_delete_clicked(GtkButton *btn, gpointer user_data);

/* Create a row widget for a draft in the list */
static GtkWidget *create_draft_row(NostrGtkComposer *self, GnostrDraft *draft) {
  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_start(box, 6);
  gtk_widget_set_margin_end(box, 6);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);

  /* Draft preview (truncated content) */
  GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(content_box, TRUE);

  /* Content preview */
  const char *content = draft->content ? draft->content : "";
  char *preview = g_strndup(content, 50);
  /* Replace newlines with spaces for preview */
  for (char *p = preview; *p; p++) {
    if (*p == '\n' || *p == '\r') *p = ' ';
  }
  if (strlen(content) > 50) {
    char *tmp = g_strdup_printf("%s...", preview);
    g_free(preview);
    preview = tmp;
  }

  GtkWidget *preview_label = gtk_label_new(preview);
  gtk_label_set_xalign(GTK_LABEL(preview_label), 0);
  gtk_label_set_ellipsize(GTK_LABEL(preview_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(preview_label), 30);
  gtk_box_append(GTK_BOX(content_box), preview_label);
  g_free(preview);

  /* Timestamp */
  GDateTime *dt = g_date_time_new_from_unix_local(draft->updated_at);
  char *time_str = g_date_time_format(dt, "%b %d, %H:%M");
  GtkWidget *time_label = gtk_label_new(time_str);
  gtk_widget_add_css_class(time_label, "dim-label");
  gtk_widget_add_css_class(time_label, "caption");
  gtk_label_set_xalign(GTK_LABEL(time_label), 0);
  gtk_box_append(GTK_BOX(content_box), time_label);
  g_free(time_str);
  g_date_time_unref(dt);

  gtk_box_append(GTK_BOX(box), content_box);

  /* Load button */
  GtkWidget *btn_load = gtk_button_new_from_icon_name("document-open-symbolic");
  gtk_widget_set_tooltip_text(btn_load, "Load draft");
  gtk_widget_add_css_class(btn_load, "flat");
  g_object_set_data_full(G_OBJECT(btn_load), "draft-d-tag", g_strdup(draft->d_tag), g_free);
  g_object_set_data(G_OBJECT(btn_load), "composer", self);
  g_signal_connect(btn_load, "clicked", G_CALLBACK(on_draft_row_load_clicked), self);
  gtk_box_append(GTK_BOX(box), btn_load);

  /* Delete button */
  GtkWidget *btn_delete = gtk_button_new_from_icon_name("user-trash-symbolic");
  gtk_widget_set_tooltip_text(btn_delete, "Delete draft");
  gtk_widget_add_css_class(btn_delete, "flat");
  gtk_widget_add_css_class(btn_delete, "destructive-action");
  g_object_set_data_full(G_OBJECT(btn_delete), "draft-d-tag", g_strdup(draft->d_tag), g_free);
  g_object_set_data(G_OBJECT(btn_delete), "composer", self);
  g_signal_connect(btn_delete, "clicked", G_CALLBACK(on_draft_row_delete_clicked), self);
  gtk_box_append(GTK_BOX(box), btn_delete);

  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  return row;
}

/* Refresh the drafts list in the popover */
static void refresh_drafts_list(NostrGtkComposer *self) {
  if (!NOSTR_GTK_IS_COMPOSER(self)) return;
  if (!self->drafts_list || !GTK_IS_LIST_BOX(self->drafts_list)) return;

  /* Clear existing rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->drafts_list))) != NULL) {
    gtk_list_box_remove(GTK_LIST_BOX(self->drafts_list), child);
  }

  /* Load drafts from local storage */
  GnostrDrafts *drafts_mgr = gnostr_drafts_get_default();
  GPtrArray *drafts = gnostr_drafts_load_local(drafts_mgr);

  if (!drafts || drafts->len == 0) {
    /* Show empty label */
    if (self->drafts_empty_label && GTK_IS_WIDGET(self->drafts_empty_label)) {
      gtk_widget_set_visible(self->drafts_empty_label, TRUE);
    }
    if (drafts) g_ptr_array_free(drafts, TRUE);
    return;
  }

  /* Hide empty label */
  if (self->drafts_empty_label && GTK_IS_WIDGET(self->drafts_empty_label)) {
    gtk_widget_set_visible(self->drafts_empty_label, FALSE);
  }

  /* Add rows for each draft */
  for (guint i = 0; i < drafts->len; i++) {
    GnostrDraft *draft = (GnostrDraft *)g_ptr_array_index(drafts, i);
    GtkWidget *row = create_draft_row(self, draft);
    gtk_list_box_append(GTK_LIST_BOX(self->drafts_list), row);
  }

  g_ptr_array_free(drafts, TRUE);
}

/* Callback when drafts popover is shown */
static void on_drafts_popover_show(GtkPopover *popover, gpointer user_data) {
  (void)popover;
  NostrGtkComposer *self = NOSTR_GTK_COMPOSER(user_data);
  refresh_drafts_list(self);
}

/* Save draft callback */
static void on_draft_saved(GnostrDrafts *drafts, gboolean success,
                           const char *error_message, gpointer user_data) {
  (void)drafts;
  NostrGtkComposer *self = NOSTR_GTK_COMPOSER(user_data);
  if (!NOSTR_GTK_IS_COMPOSER(self)) return;

  if (success) {
    composer_show_toast(self, "Draft saved");
    g_signal_emit(self, signals[SIGNAL_DRAFT_SAVED], 0);
  } else {
    g_autofree char *msg = g_strdup_printf("Failed to save draft: %s",
                                 error_message ? error_message : "unknown error");
    composer_show_toast(self, msg);
  }
}

/* NIP-37: Save draft button clicked */
static void on_save_draft_clicked(NostrGtkComposer *self, GtkButton *button) {
  (void)button;
  if (!NOSTR_GTK_IS_COMPOSER(self)) return;

  /* Get current text */
  if (!self->text_view || !GTK_IS_TEXT_VIEW(self->text_view)) return;

  GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text_view));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buf, &start, &end);
  char *text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);

  /* Don't save empty drafts */
  if (!text || !*text) {
    g_free(text);
    composer_show_toast(self, "Cannot save empty draft");
    return;
  }

  /* Create draft */
  GnostrDraft *draft = gnostr_draft_new();
  draft->content = text; /* Takes ownership */
  draft->target_kind = 1; /* Text note */

  /* Preserve d-tag if editing existing draft */
  if (self->current_draft_d_tag) {
    draft->d_tag = g_strdup(self->current_draft_d_tag);
  }

  /* Get subject if present */
  const char *subject = nostr_gtk_composer_get_subject(self);
  if (subject) {
    draft->subject = g_strdup(subject);
  }

  /* Copy reply context */
  if (self->reply_to_id) {
    draft->reply_to_id = g_strdup(self->reply_to_id);
  }
  if (self->root_id) {
    draft->root_id = g_strdup(self->root_id);
  }
  if (self->reply_to_pubkey) {
    draft->reply_to_pubkey = g_strdup(self->reply_to_pubkey);
  }

  /* Copy quote context */
  if (self->quote_id) {
    draft->quote_id = g_strdup(self->quote_id);
  }
  if (self->quote_pubkey) {
    draft->quote_pubkey = g_strdup(self->quote_pubkey);
  }
  if (self->quote_nostr_uri) {
    draft->quote_nostr_uri = g_strdup(self->quote_nostr_uri);
  }

  /* Copy sensitive flag */
  draft->is_sensitive = self->is_sensitive;

  /* Save draft */
  GnostrDrafts *drafts_mgr = gnostr_drafts_get_default();
  gnostr_drafts_save_async(drafts_mgr, draft, on_draft_saved, self);

  /* Update current draft d-tag for subsequent saves */
  g_free(self->current_draft_d_tag);
  self->current_draft_d_tag = g_strdup(draft->d_tag);

  gnostr_draft_free(draft);
}

/* Load draft into composer */
static void on_draft_row_load_clicked(GtkButton *btn, gpointer user_data) {
  NostrGtkComposer *self = NOSTR_GTK_COMPOSER(user_data);
  if (!NOSTR_GTK_IS_COMPOSER(self)) return;

  const char *d_tag = g_object_get_data(G_OBJECT(btn), "draft-d-tag");
  if (!d_tag) return;

  /* Load drafts and find the one with matching d-tag */
  GnostrDrafts *drafts_mgr = gnostr_drafts_get_default();
  GPtrArray *drafts = gnostr_drafts_load_local(drafts_mgr);
  if (!drafts) return;

  GnostrDraft *found = NULL;
  for (guint i = 0; i < drafts->len; i++) {
    GnostrDraft *draft = (GnostrDraft *)g_ptr_array_index(drafts, i);
    if (draft->d_tag && strcmp(draft->d_tag, d_tag) == 0) {
      found = draft;
      break;
    }
  }

  if (!found) {
    g_ptr_array_free(drafts, TRUE);
    composer_show_toast(self, "Draft not found");
    return;
  }

  /* Load into composer */
  nostr_gtk_composer_load_draft(self, found);

  /* Close popover */
  if (self->drafts_popover && GTK_IS_POPOVER(self->drafts_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->drafts_popover));
  }

  composer_show_toast(self, "Draft loaded");
  g_signal_emit(self, signals[SIGNAL_DRAFT_LOADED], 0);

  g_ptr_array_free(drafts, TRUE);
}

/* Delete draft callback */
static void on_draft_deleted(GnostrDrafts *drafts, gboolean success,
                              const char *error_message, gpointer user_data) {
  (void)drafts;
  (void)error_message;
  NostrGtkComposer *self = NOSTR_GTK_COMPOSER(user_data);
  if (!NOSTR_GTK_IS_COMPOSER(self)) return;

  if (success) {
    composer_show_toast(self, "Draft deleted");
    refresh_drafts_list(self);
    g_signal_emit(self, signals[SIGNAL_DRAFT_DELETED], 0);
  }
}

/* Delete draft from list */
static void on_draft_row_delete_clicked(GtkButton *btn, gpointer user_data) {
  NostrGtkComposer *self = NOSTR_GTK_COMPOSER(user_data);
  if (!NOSTR_GTK_IS_COMPOSER(self)) return;

  const char *d_tag = g_object_get_data(G_OBJECT(btn), "draft-d-tag");
  if (!d_tag) return;

  /* Clear current draft if it's the one being deleted */
  if (self->current_draft_d_tag && strcmp(self->current_draft_d_tag, d_tag) == 0) {
    g_free(self->current_draft_d_tag);
    self->current_draft_d_tag = NULL;
  }

  GnostrDrafts *drafts_mgr = gnostr_drafts_get_default();
  gnostr_drafts_delete_async(drafts_mgr, d_tag, on_draft_deleted, self);
}

static void nostr_gtk_composer_class_init(NostrGtkComposerClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *gobj_class = G_OBJECT_CLASS(klass);
  gobj_class->dispose = nostr_gtk_composer_dispose;
  gobj_class->finalize = nostr_gtk_composer_finalize;
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);
  gtk_widget_class_bind_template_child(widget_class, NostrGtkComposer, root);
  gtk_widget_class_bind_template_child(widget_class, NostrGtkComposer, text_view);
  gtk_widget_class_bind_template_child(widget_class, NostrGtkComposer, btn_post);
  gtk_widget_class_bind_template_child(widget_class, NostrGtkComposer, btn_attach);
  gtk_widget_class_bind_template_child(widget_class, NostrGtkComposer, reply_indicator_box);
  gtk_widget_class_bind_template_child(widget_class, NostrGtkComposer, reply_indicator);
  gtk_widget_class_bind_template_child(widget_class, NostrGtkComposer, btn_cancel_reply);
  gtk_widget_class_bind_template_child(widget_class, NostrGtkComposer, upload_progress_box);
  gtk_widget_class_bind_template_child(widget_class, NostrGtkComposer, upload_spinner);
  gtk_widget_class_bind_template_child(widget_class, NostrGtkComposer, upload_status_label);
  /* NIP-14 Subject input */
  gtk_widget_class_bind_template_child(widget_class, NostrGtkComposer, subject_box);
  gtk_widget_class_bind_template_child(widget_class, NostrGtkComposer, subject_entry);
  /* NIP-36 Sensitive content toggle */
  gtk_widget_class_bind_template_child(widget_class, NostrGtkComposer, btn_sensitive);
  /* NIP-37 Drafts */
  gtk_widget_class_bind_template_child(widget_class, NostrGtkComposer, btn_drafts);
  gtk_widget_class_bind_template_child(widget_class, NostrGtkComposer, drafts_popover);
  gtk_widget_class_bind_template_child(widget_class, NostrGtkComposer, drafts_list);
  gtk_widget_class_bind_template_child(widget_class, NostrGtkComposer, drafts_empty_label);
  gtk_widget_class_bind_template_child(widget_class, NostrGtkComposer, btn_save_draft);
  gtk_widget_class_bind_template_callback(widget_class, on_post_clicked);
  gtk_widget_class_bind_template_callback(widget_class, on_cancel_reply_clicked);
  gtk_widget_class_bind_template_callback(widget_class, on_attach_clicked);
  gtk_widget_class_bind_template_callback(widget_class, on_sensitive_toggled);
  gtk_widget_class_bind_template_callback(widget_class, on_save_draft_clicked);

  signals[SIGNAL_POST_REQUESTED] =
      g_signal_new("post-requested",
                   G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST,
                   0, /* class offset */
                   NULL, NULL,
                   g_cclosure_marshal_VOID__STRING,
                   G_TYPE_NONE, 1, G_TYPE_STRING);

  /* NIP-37: Draft signals */
  signals[SIGNAL_DRAFT_SAVED] =
      g_signal_new("draft-saved",
                   G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST,
                   0, NULL, NULL,
                   g_cclosure_marshal_VOID__VOID,
                   G_TYPE_NONE, 0);

  signals[SIGNAL_DRAFT_LOADED] =
      g_signal_new("draft-loaded",
                   G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST,
                   0, NULL, NULL,
                   g_cclosure_marshal_VOID__VOID,
                   G_TYPE_NONE, 0);

  signals[SIGNAL_DRAFT_DELETED] =
      g_signal_new("draft-deleted",
                   G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST,
                   0, NULL, NULL,
                   g_cclosure_marshal_VOID__VOID,
                   G_TYPE_NONE, 0);
}

static void nostr_gtk_composer_init(NostrGtkComposer *self) {
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
  /* NIP-36: Initialize sensitive content toggle */
  if (self->btn_sensitive) {
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_sensitive),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, "Mark as Sensitive", -1);
  }
  self->is_sensitive = FALSE;
  self->upload_in_progress = FALSE;
  self->upload_cancellable = NULL;
  /* NIP-37: Initialize drafts */
  self->current_draft_d_tag = NULL;
  if (self->btn_drafts) {
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_drafts),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, "Drafts", -1);
  }
  if (self->btn_save_draft) {
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_save_draft),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, "Save Draft", -1);
  }
  /* Connect popover show signal to refresh drafts list */
  if (self->drafts_popover && GTK_IS_POPOVER(self->drafts_popover)) {
    g_signal_connect(self->drafts_popover, "show",
                     G_CALLBACK(on_drafts_popover_show), self);
  }
  g_message("composer init: self=%p root=%p text_view=%p btn_post=%p btn_attach=%p",
            (void*)self,
            (void*)self->root,
            (void*)self->text_view,
            (void*)self->btn_post,
            (void*)self->btn_attach);
}

GtkWidget *nostr_gtk_composer_new(void) {
  return g_object_new(NOSTR_GTK_TYPE_COMPOSER, NULL);
}

void nostr_gtk_composer_clear(NostrGtkComposer *self) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));
  if (!self->text_view || !GTK_IS_TEXT_VIEW(self->text_view)) return;
  GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text_view));
  gtk_text_buffer_set_text(buf, "", 0);
  /* NIP-14: Clear subject entry */
  if (self->subject_entry && GTK_IS_ENTRY(self->subject_entry)) {
    gtk_editable_set_text(GTK_EDITABLE(self->subject_entry), "");
  }
  /* Also clear reply, quote, and comment context */
  nostr_gtk_composer_clear_reply_context(self);
  nostr_gtk_composer_clear_quote_context(self);
  nostr_gtk_composer_clear_comment_context(self);
  /* Clear uploaded media metadata */
  nostr_gtk_composer_clear_uploaded_media(self);
  /* NIP-40: Clear expiration */
  nostr_gtk_composer_clear_expiration(self);
  /* NIP-36: Reset sensitive content toggle */
  self->is_sensitive = FALSE;
  if (GTK_IS_TOGGLE_BUTTON(self->btn_sensitive)) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_sensitive), FALSE);
    gtk_widget_remove_css_class(GTK_WIDGET(self->btn_sensitive), "warning");
  }
}

void nostr_gtk_composer_set_reply_context(NostrGtkComposer *self,
                                       const char *reply_to_id,
                                       const char *root_id,
                                       const char *reply_to_pubkey,
                                       const char *reply_to_display_name) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));

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
    g_autofree char *label = g_strdup_printf("Replying to %s",
                                   reply_to_display_name ? reply_to_display_name : "@user");
    gtk_label_set_text(GTK_LABEL(self->reply_indicator), label);
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

void nostr_gtk_composer_clear_reply_context(NostrGtkComposer *self) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));

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

gboolean nostr_gtk_composer_is_reply(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), FALSE);
  return self->reply_to_id != NULL;
}

const char *nostr_gtk_composer_get_reply_to_id(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), NULL);
  return self->reply_to_id;
}

const char *nostr_gtk_composer_get_root_id(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), NULL);
  return self->root_id;
}

const char *nostr_gtk_composer_get_reply_to_pubkey(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), NULL);
  return self->reply_to_pubkey;
}

/* Quote context for NIP-18 quote posts */
void nostr_gtk_composer_set_quote_context(NostrGtkComposer *self,
                                       const char *quote_id,
                                       const char *quote_pubkey,
                                       const char *nostr_uri,
                                       const char *quoted_author_display_name) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));

  /* Clear any existing reply context first */
  nostr_gtk_composer_clear_reply_context(self);

  /* Store quote context */
  g_free(self->quote_id);
  g_free(self->quote_pubkey);
  g_free(self->quote_nostr_uri);

  self->quote_id = g_strdup(quote_id);
  self->quote_pubkey = g_strdup(quote_pubkey);
  self->quote_nostr_uri = g_strdup(nostr_uri);

  /* Update indicator to show we're quoting */
  if (self->reply_indicator && GTK_IS_LABEL(self->reply_indicator)) {
    g_autofree char *label = g_strdup_printf("Quoting %s",
                                   quoted_author_display_name ? quoted_author_display_name : "@user");
    gtk_label_set_text(GTK_LABEL(self->reply_indicator), label);
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
    g_autofree char *prefill = g_strdup_printf("\n\n%s", nostr_uri);
    gtk_text_buffer_set_text(buf, prefill, -1);
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

void nostr_gtk_composer_clear_quote_context(NostrGtkComposer *self) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));

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

gboolean nostr_gtk_composer_is_quote(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), FALSE);
  return self->quote_id != NULL;
}

const char *nostr_gtk_composer_get_quote_id(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), NULL);
  return self->quote_id;
}

const char *nostr_gtk_composer_get_quote_pubkey(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), NULL);
  return self->quote_pubkey;
}

const char *nostr_gtk_composer_get_quote_nostr_uri(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), NULL);
  return self->quote_nostr_uri;
}

/* Media upload state */
gboolean nostr_gtk_composer_is_uploading(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), FALSE);
  return self->upload_in_progress;
}

void nostr_gtk_composer_cancel_upload(NostrGtkComposer *self) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));

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

/* NIP-92 imeta: Get uploaded media list */
NostrGtkComposerMedia **nostr_gtk_composer_get_uploaded_media(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), NULL);
  if (!self->uploaded_media || self->uploaded_media->len == 0) {
    return NULL;
  }
  /* Return the pdata array directly (NULL-terminated by GPtrArray) */
  return (NostrGtkComposerMedia **)self->uploaded_media->pdata;
}

gsize nostr_gtk_composer_get_uploaded_media_count(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), 0);
  if (!self->uploaded_media) return 0;
  return self->uploaded_media->len;
}

void nostr_gtk_composer_clear_uploaded_media(NostrGtkComposer *self) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));
  if (self->uploaded_media) {
    g_ptr_array_set_size(self->uploaded_media, 0);
  }
}

/* NIP-14: Get subject text from entry
 * Returns the current subject text, or NULL if empty.
 * The returned string is owned by the entry; do not free.
 */
const char *nostr_gtk_composer_get_subject(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), NULL);
  if (!self->subject_entry || !GTK_IS_ENTRY(self->subject_entry)) return NULL;
  const char *text = gtk_editable_get_text(GTK_EDITABLE(self->subject_entry));
  /* Return NULL for empty string */
  if (!text || !*text) return NULL;
  return text;
}

/* NIP-40: Expiration timestamp support */
void nostr_gtk_composer_set_expiration(NostrGtkComposer *self, gint64 expiration_secs) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));
  self->expiration = expiration_secs;
  g_message("composer: set expiration to %" G_GINT64_FORMAT, expiration_secs);
}

gint64 nostr_gtk_composer_get_expiration(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), 0);
  return self->expiration;
}

void nostr_gtk_composer_clear_expiration(NostrGtkComposer *self) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));
  self->expiration = 0;
}

gboolean nostr_gtk_composer_has_expiration(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), FALSE);
  return self->expiration > 0;
}

/* NIP-36: Content warning / sensitive content support */
gboolean nostr_gtk_composer_is_sensitive(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), FALSE);
  return self->is_sensitive;
}

void nostr_gtk_composer_set_sensitive(NostrGtkComposer *self, gboolean sensitive) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));
  self->is_sensitive = sensitive;
  if (GTK_IS_TOGGLE_BUTTON(self->btn_sensitive)) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->btn_sensitive), sensitive);
    if (sensitive) {
      gtk_widget_add_css_class(GTK_WIDGET(self->btn_sensitive), "warning");
    } else {
      gtk_widget_remove_css_class(GTK_WIDGET(self->btn_sensitive), "warning");
    }
  }
}

/* NIP-22: Comment context for kind 1111 events */
void nostr_gtk_composer_set_comment_context(NostrGtkComposer *self,
                                         const char *root_id,
                                         int root_kind,
                                         const char *root_pubkey,
                                         const char *display_name) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));

  /* Clear any existing reply/quote context first */
  nostr_gtk_composer_clear_reply_context(self);
  nostr_gtk_composer_clear_quote_context(self);

  /* Store comment context */
  g_free(self->comment_root_id);
  g_free(self->comment_root_pubkey);

  self->comment_root_id = g_strdup(root_id);
  self->comment_root_kind = root_kind;
  self->comment_root_pubkey = g_strdup(root_pubkey);

  /* Update indicator to show we're commenting */
  if (self->reply_indicator && GTK_IS_LABEL(self->reply_indicator)) {
    g_autofree char *label = g_strdup_printf("Commenting on %s",
                                   display_name ? display_name : "@user");
    gtk_label_set_text(GTK_LABEL(self->reply_indicator), label);
  }
  /* Show the indicator box */
  if (self->reply_indicator_box && GTK_IS_WIDGET(self->reply_indicator_box)) {
    gtk_widget_set_visible(self->reply_indicator_box, TRUE);
  }

  /* Update button label */
  if (self->btn_post && GTK_IS_BUTTON(self->btn_post)) {
    gtk_button_set_label(GTK_BUTTON(self->btn_post), "Comment");
  }

  g_message("composer: set comment context id=%s kind=%d pubkey=%s",
            root_id ? root_id : "(null)",
            root_kind,
            root_pubkey ? root_pubkey : "(null)");
}

void nostr_gtk_composer_clear_comment_context(NostrGtkComposer *self) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));

  g_clear_pointer(&self->comment_root_id, g_free);
  g_clear_pointer(&self->comment_root_pubkey, g_free);
  self->comment_root_kind = 0;

  /* Hide indicator box if no reply/quote context either */
  if (!self->reply_to_id && !self->quote_id) {
    if (self->reply_indicator_box && GTK_IS_WIDGET(self->reply_indicator_box)) {
      gtk_widget_set_visible(self->reply_indicator_box, FALSE);
    }
    /* Reset button label */
    if (self->btn_post && GTK_IS_BUTTON(self->btn_post)) {
      gtk_button_set_label(GTK_BUTTON(self->btn_post), "Post");
    }
  }
}

gboolean nostr_gtk_composer_is_comment(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), FALSE);
  return self->comment_root_id != NULL;
}

const char *nostr_gtk_composer_get_comment_root_id(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), NULL);
  return self->comment_root_id;
}

int nostr_gtk_composer_get_comment_root_kind(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), 0);
  return self->comment_root_kind;
}

const char *nostr_gtk_composer_get_comment_root_pubkey(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), NULL);
  return self->comment_root_pubkey;
}

/* ---- NIP-37: Draft management public API ---- */

void nostr_gtk_composer_load_draft(NostrGtkComposer *self, const GnostrDraft *draft) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));
  g_return_if_fail(draft != NULL);

  /* Clear existing state */
  nostr_gtk_composer_clear(self);

  /* Store d-tag for updates */
  g_free(self->current_draft_d_tag);
  self->current_draft_d_tag = g_strdup(draft->d_tag);

  /* Set content */
  if (draft->content && self->text_view && GTK_IS_TEXT_VIEW(self->text_view)) {
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text_view));
    gtk_text_buffer_set_text(buf, draft->content, -1);
  }

  /* Set subject */
  if (draft->subject && self->subject_entry && GTK_IS_ENTRY(self->subject_entry)) {
    gtk_editable_set_text(GTK_EDITABLE(self->subject_entry), draft->subject);
  }

  /* Set reply context */
  if (draft->reply_to_id) {
    g_free(self->reply_to_id);
    self->reply_to_id = g_strdup(draft->reply_to_id);
  }
  if (draft->root_id) {
    g_free(self->root_id);
    self->root_id = g_strdup(draft->root_id);
  }
  if (draft->reply_to_pubkey) {
    g_free(self->reply_to_pubkey);
    self->reply_to_pubkey = g_strdup(draft->reply_to_pubkey);
    /* Show reply indicator */
    if (self->reply_indicator_box && GTK_IS_WIDGET(self->reply_indicator_box)) {
      gtk_widget_set_visible(self->reply_indicator_box, TRUE);
    }
    if (self->reply_indicator && GTK_IS_LABEL(self->reply_indicator)) {
      gtk_label_set_text(GTK_LABEL(self->reply_indicator), "Replying to @user (from draft)");
    }
    if (self->btn_post && GTK_IS_BUTTON(self->btn_post)) {
      gtk_button_set_label(GTK_BUTTON(self->btn_post), "Reply");
    }
  }

  /* Set quote context */
  if (draft->quote_id) {
    g_free(self->quote_id);
    self->quote_id = g_strdup(draft->quote_id);
  }
  if (draft->quote_pubkey) {
    g_free(self->quote_pubkey);
    self->quote_pubkey = g_strdup(draft->quote_pubkey);
  }
  if (draft->quote_nostr_uri) {
    g_free(self->quote_nostr_uri);
    self->quote_nostr_uri = g_strdup(draft->quote_nostr_uri);
    /* Show quote indicator if no reply context */
    if (!draft->reply_to_pubkey) {
      if (self->reply_indicator_box && GTK_IS_WIDGET(self->reply_indicator_box)) {
        gtk_widget_set_visible(self->reply_indicator_box, TRUE);
      }
      if (self->reply_indicator && GTK_IS_LABEL(self->reply_indicator)) {
        gtk_label_set_text(GTK_LABEL(self->reply_indicator), "Quoting (from draft)");
      }
      if (self->btn_post && GTK_IS_BUTTON(self->btn_post)) {
        gtk_button_set_label(GTK_BUTTON(self->btn_post), "Quote");
      }
    }
  }

  /* Set sensitive flag */
  nostr_gtk_composer_set_sensitive(self, draft->is_sensitive);

  g_message("composer: loaded draft d_tag=%s kind=%d",
            draft->d_tag ? draft->d_tag : "(null)",
            draft->target_kind);
}

const char *nostr_gtk_composer_get_current_draft_d_tag(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), NULL);
  return self->current_draft_d_tag;
}

void nostr_gtk_composer_clear_draft_context(NostrGtkComposer *self) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));
  g_clear_pointer(&self->current_draft_d_tag, g_free);
}

gboolean nostr_gtk_composer_has_draft_loaded(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), FALSE);
  return self->current_draft_d_tag != NULL;
}

char *nostr_gtk_composer_get_text(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), NULL);
  if (!self->text_view || !GTK_IS_TEXT_VIEW(self->text_view)) return NULL;

  GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text_view));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buf, &start, &end);
  return gtk_text_buffer_get_text(buf, &start, &end, FALSE);
}
