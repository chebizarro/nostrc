/* gnostr-composer.c — Nostr event composition widget (library version)
 *
 * Decoupled from app-specific services: media upload, draft persistence,
 * and toast notifications are all routed through GObject signals.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gnostr-composer.h"
#include <time.h>

#define UI_RESOURCE "/org/nostr/gtk/ui/gnostr-composer.ui"

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
  SIGNAL_TOAST_REQUESTED,
  SIGNAL_UPLOAD_REQUESTED,
  SIGNAL_SAVE_DRAFT_REQUESTED,
  SIGNAL_LOAD_DRAFTS_REQUESTED,
  SIGNAL_DRAFT_LOAD_REQUESTED,
  SIGNAL_DRAFT_DELETE_REQUESTED,
  SIGNAL_DRAFT_SAVED,
  SIGNAL_DRAFT_LOADED,
  SIGNAL_DRAFT_DELETED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = {0};

/* Emit toast-requested signal instead of calling app-specific toast */
static void composer_show_toast(NostrGtkComposer *self, const char *message) {
  if (!NOSTR_GTK_IS_COMPOSER(self) || !message) return;
  g_signal_emit(self, signals[SIGNAL_TOAST_REQUESTED], 0, message);
}

static void nostr_gtk_composer_dispose(GObject *obj) {
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
  self->subject_box = NULL;
  self->subject_entry = NULL;
  self->btn_sensitive = NULL;
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
  g_clear_pointer(&self->comment_root_id, g_free);
  g_clear_pointer(&self->comment_root_pubkey, g_free);
  g_clear_pointer(&self->current_draft_d_tag, g_free);
  if (self->upload_cancellable)
    g_cancellable_cancel(self->upload_cancellable);
  g_clear_object(&self->upload_cancellable);
  if (self->uploaded_media) {
    g_ptr_array_free(self->uploaded_media, TRUE);
    self->uploaded_media = NULL;
  }
  G_OBJECT_CLASS(nostr_gtk_composer_parent_class)->finalize(obj);
}

static void on_post_clicked(NostrGtkComposer *self, GtkButton *button) {
  (void)button;
  if (!self || !GTK_IS_WIDGET(self)) return;
  if (!self->text_view || !GTK_IS_TEXT_VIEW(self->text_view)) return;
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

/* File chooser response callback — emits upload-requested signal */
static void on_file_chooser_response(GObject *source, GAsyncResult *res, gpointer user_data) {
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
  if (self->btn_attach && GTK_IS_WIDGET(self->btn_attach)) {
    gtk_widget_set_sensitive(self->btn_attach, FALSE);
  }

  /* Create cancellable for this upload */
  g_clear_object(&self->upload_cancellable);
  self->upload_cancellable = g_cancellable_new();

  /* Emit signal for app to handle the actual upload */
  g_message("composer: upload requested for %s", path);
  g_signal_emit(self, signals[SIGNAL_UPLOAD_REQUESTED], 0, path);
  g_free(path);
}

/* Attach button clicked - open file chooser */
static void on_attach_clicked(NostrGtkComposer *self, GtkButton *button) {
  (void)button;
  if (!NOSTR_GTK_IS_COMPOSER(self)) return;

  if (self->upload_in_progress) {
    g_message("composer: upload already in progress");
    return;
  }

  GtkFileDialog *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Select Media to Upload");
  gtk_file_dialog_set_modal(dialog, TRUE);

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

  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  GtkWindow *parent_window = NULL;

  if (root && GTK_IS_WINDOW(root)) {
    parent_window = GTK_WINDOW(root);
  } else {
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

  if (active) {
    gtk_widget_add_css_class(GTK_WIDGET(self->btn_sensitive), "warning");
  } else {
    gtk_widget_remove_css_class(GTK_WIDGET(self->btn_sensitive), "warning");
  }
}

/* ---- NIP-37: Drafts UI (decoupled from persistence) ---- */

/* Forward declarations for draft row click handlers */
static void on_draft_row_load_clicked(GtkButton *btn, gpointer user_data);
static void on_draft_row_delete_clicked(GtkButton *btn, gpointer user_data);

/* Callback when drafts popover is shown — emit signal for app to populate */
static void on_drafts_popover_show(GtkPopover *popover, gpointer user_data) {
  (void)popover;
  NostrGtkComposer *self = NOSTR_GTK_COMPOSER(user_data);
  if (!NOSTR_GTK_IS_COMPOSER(self)) return;
  g_signal_emit(self, signals[SIGNAL_LOAD_DRAFTS_REQUESTED], 0);
}

/* NIP-37: Save draft button clicked — emit signal */
static void on_save_draft_clicked(NostrGtkComposer *self, GtkButton *button) {
  (void)button;
  if (!NOSTR_GTK_IS_COMPOSER(self)) return;
  if (!self->text_view || !GTK_IS_TEXT_VIEW(self->text_view)) return;

  /* Check for empty text */
  GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text_view));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buf, &start, &end);
  char *text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);

  if (!text || !*text) {
    g_free(text);
    composer_show_toast(self, "Cannot save empty draft");
    return;
  }
  g_free(text);

  /* Emit signal — the app reads composer state and persists */
  g_signal_emit(self, signals[SIGNAL_SAVE_DRAFT_REQUESTED], 0);
}

/* Load draft row clicked — emit signal with d-tag */
static void on_draft_row_load_clicked(GtkButton *btn, gpointer user_data) {
  NostrGtkComposer *self = NOSTR_GTK_COMPOSER(user_data);
  if (!NOSTR_GTK_IS_COMPOSER(self)) return;

  const char *d_tag = g_object_get_data(G_OBJECT(btn), "draft-d-tag");
  if (!d_tag) return;

  /* Close popover */
  if (self->drafts_popover && GTK_IS_POPOVER(self->drafts_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->drafts_popover));
  }

  g_signal_emit(self, signals[SIGNAL_DRAFT_LOAD_REQUESTED], 0, d_tag);
}

/* Delete draft row clicked — emit signal with d-tag */
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

  g_signal_emit(self, signals[SIGNAL_DRAFT_DELETE_REQUESTED], 0, d_tag);
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
  gtk_widget_class_bind_template_child(widget_class, NostrGtkComposer, subject_box);
  gtk_widget_class_bind_template_child(widget_class, NostrGtkComposer, subject_entry);
  gtk_widget_class_bind_template_child(widget_class, NostrGtkComposer, btn_sensitive);
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
                   0, NULL, NULL,
                   g_cclosure_marshal_VOID__STRING,
                   G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_TOAST_REQUESTED] =
      g_signal_new("toast-requested",
                   G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST,
                   0, NULL, NULL,
                   g_cclosure_marshal_VOID__STRING,
                   G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_UPLOAD_REQUESTED] =
      g_signal_new("upload-requested",
                   G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST,
                   0, NULL, NULL,
                   g_cclosure_marshal_VOID__STRING,
                   G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_SAVE_DRAFT_REQUESTED] =
      g_signal_new("save-draft-requested",
                   G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST,
                   0, NULL, NULL,
                   g_cclosure_marshal_VOID__VOID,
                   G_TYPE_NONE, 0);

  signals[SIGNAL_LOAD_DRAFTS_REQUESTED] =
      g_signal_new("load-drafts-requested",
                   G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST,
                   0, NULL, NULL,
                   g_cclosure_marshal_VOID__VOID,
                   G_TYPE_NONE, 0);

  signals[SIGNAL_DRAFT_LOAD_REQUESTED] =
      g_signal_new("draft-load-requested",
                   G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST,
                   0, NULL, NULL,
                   g_cclosure_marshal_VOID__STRING,
                   G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_DRAFT_DELETE_REQUESTED] =
      g_signal_new("draft-delete-requested",
                   G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST,
                   0, NULL, NULL,
                   g_cclosure_marshal_VOID__STRING,
                   G_TYPE_NONE, 1, G_TYPE_STRING);

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
  if (self->btn_sensitive) {
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_sensitive),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, "Mark as Sensitive", -1);
  }
  self->is_sensitive = FALSE;
  self->upload_in_progress = FALSE;
  self->upload_cancellable = NULL;
  self->current_draft_d_tag = NULL;
  if (self->btn_drafts) {
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_drafts),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, "Drafts", -1);
  }
  if (self->btn_save_draft) {
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_save_draft),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, "Save Draft", -1);
  }
  /* Connect popover show signal to emit load-drafts-requested */
  if (self->drafts_popover && GTK_IS_POPOVER(self->drafts_popover)) {
    g_signal_connect(self->drafts_popover, "show",
                     G_CALLBACK(on_drafts_popover_show), self);
  }
}

GtkWidget *nostr_gtk_composer_new(void) {
  return g_object_new(NOSTR_GTK_TYPE_COMPOSER, NULL);
}

void nostr_gtk_composer_clear(NostrGtkComposer *self) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));
  if (!self->text_view || !GTK_IS_TEXT_VIEW(self->text_view)) return;
  GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text_view));
  gtk_text_buffer_set_text(buf, "", 0);
  if (self->subject_entry && GTK_IS_ENTRY(self->subject_entry)) {
    gtk_editable_set_text(GTK_EDITABLE(self->subject_entry), "");
  }
  nostr_gtk_composer_clear_reply_context(self);
  nostr_gtk_composer_clear_quote_context(self);
  nostr_gtk_composer_clear_comment_context(self);
  nostr_gtk_composer_clear_uploaded_media(self);
  nostr_gtk_composer_clear_expiration(self);
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

  g_free(self->reply_to_id);
  g_free(self->root_id);
  g_free(self->reply_to_pubkey);

  self->reply_to_id = g_strdup(reply_to_id);
  self->root_id = g_strdup(root_id ? root_id : reply_to_id);
  self->reply_to_pubkey = g_strdup(reply_to_pubkey);

  if (self->reply_indicator && GTK_IS_LABEL(self->reply_indicator)) {
    char *label = g_strdup_printf("Replying to %s",
                                   reply_to_display_name ? reply_to_display_name : "@user");
    gtk_label_set_text(GTK_LABEL(self->reply_indicator), label);
    g_free(label);
  }
  if (self->reply_indicator_box && GTK_IS_WIDGET(self->reply_indicator_box)) {
    gtk_widget_set_visible(self->reply_indicator_box, TRUE);
  }
  if (self->btn_post && GTK_IS_BUTTON(self->btn_post)) {
    gtk_button_set_label(GTK_BUTTON(self->btn_post), "Reply");
  }
}

void nostr_gtk_composer_clear_reply_context(NostrGtkComposer *self) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));

  g_clear_pointer(&self->reply_to_id, g_free);
  g_clear_pointer(&self->root_id, g_free);
  g_clear_pointer(&self->reply_to_pubkey, g_free);

  if (self->reply_indicator_box && GTK_IS_WIDGET(self->reply_indicator_box)) {
    gtk_widget_set_visible(self->reply_indicator_box, FALSE);
  }
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

void nostr_gtk_composer_set_quote_context(NostrGtkComposer *self,
                                       const char *quote_id,
                                       const char *quote_pubkey,
                                       const char *nostr_uri,
                                       const char *quoted_author_display_name) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));

  nostr_gtk_composer_clear_reply_context(self);

  g_free(self->quote_id);
  g_free(self->quote_pubkey);
  g_free(self->quote_nostr_uri);

  self->quote_id = g_strdup(quote_id);
  self->quote_pubkey = g_strdup(quote_pubkey);
  self->quote_nostr_uri = g_strdup(nostr_uri);

  if (self->reply_indicator && GTK_IS_LABEL(self->reply_indicator)) {
    char *label = g_strdup_printf("Quoting %s",
                                   quoted_author_display_name ? quoted_author_display_name : "@user");
    gtk_label_set_text(GTK_LABEL(self->reply_indicator), label);
    g_free(label);
  }
  if (self->reply_indicator_box && GTK_IS_WIDGET(self->reply_indicator_box)) {
    gtk_widget_set_visible(self->reply_indicator_box, TRUE);
  }
  if (self->btn_post && GTK_IS_BUTTON(self->btn_post)) {
    gtk_button_set_label(GTK_BUTTON(self->btn_post), "Quote");
  }

  if (nostr_uri && self->text_view && GTK_IS_TEXT_VIEW(self->text_view)) {
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text_view));
    char *prefill = g_strdup_printf("\n\n%s", nostr_uri);
    gtk_text_buffer_set_text(buf, prefill, -1);
    g_free(prefill);
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buf, &start);
    gtk_text_buffer_place_cursor(buf, &start);
  }
}

void nostr_gtk_composer_clear_quote_context(NostrGtkComposer *self) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));

  g_clear_pointer(&self->quote_id, g_free);
  g_clear_pointer(&self->quote_pubkey, g_free);
  g_clear_pointer(&self->quote_nostr_uri, g_free);

  if (!self->reply_to_id) {
    if (self->reply_indicator_box && GTK_IS_WIDGET(self->reply_indicator_box)) {
      gtk_widget_set_visible(self->reply_indicator_box, FALSE);
    }
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

  if (self->upload_progress_box && GTK_IS_WIDGET(self->upload_progress_box)) {
    gtk_widget_set_visible(self->upload_progress_box, FALSE);
  }
  if (self->upload_spinner && GTK_IS_SPINNER(self->upload_spinner)) {
    gtk_spinner_set_spinning(GTK_SPINNER(self->upload_spinner), FALSE);
  }
  if (self->btn_attach && GTK_IS_WIDGET(self->btn_attach)) {
    gtk_widget_set_sensitive(self->btn_attach, TRUE);
  }
}

NostrGtkComposerMedia **nostr_gtk_composer_get_uploaded_media(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), NULL);
  if (!self->uploaded_media || self->uploaded_media->len == 0) {
    return NULL;
  }
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

const char *nostr_gtk_composer_get_subject(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), NULL);
  if (!self->subject_entry || !GTK_IS_ENTRY(self->subject_entry)) return NULL;
  const char *text = gtk_editable_get_text(GTK_EDITABLE(self->subject_entry));
  if (!text || !*text) return NULL;
  return text;
}

void nostr_gtk_composer_set_expiration(NostrGtkComposer *self, gint64 expiration_secs) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));
  self->expiration = expiration_secs;
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

/* NIP-22: Comment context */
void nostr_gtk_composer_set_comment_context(NostrGtkComposer *self,
                                         const char *root_id,
                                         int root_kind,
                                         const char *root_pubkey,
                                         const char *display_name) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));

  nostr_gtk_composer_clear_reply_context(self);
  nostr_gtk_composer_clear_quote_context(self);

  g_free(self->comment_root_id);
  g_free(self->comment_root_pubkey);

  self->comment_root_id = g_strdup(root_id);
  self->comment_root_kind = root_kind;
  self->comment_root_pubkey = g_strdup(root_pubkey);

  if (self->reply_indicator && GTK_IS_LABEL(self->reply_indicator)) {
    char *label = g_strdup_printf("Commenting on %s",
                                   display_name ? display_name : "@user");
    gtk_label_set_text(GTK_LABEL(self->reply_indicator), label);
    g_free(label);
  }
  if (self->reply_indicator_box && GTK_IS_WIDGET(self->reply_indicator_box)) {
    gtk_widget_set_visible(self->reply_indicator_box, TRUE);
  }
  if (self->btn_post && GTK_IS_BUTTON(self->btn_post)) {
    gtk_button_set_label(GTK_BUTTON(self->btn_post), "Comment");
  }
}

void nostr_gtk_composer_clear_comment_context(NostrGtkComposer *self) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));

  g_clear_pointer(&self->comment_root_id, g_free);
  g_clear_pointer(&self->comment_root_pubkey, g_free);
  self->comment_root_kind = 0;

  if (!self->reply_to_id && !self->quote_id) {
    if (self->reply_indicator_box && GTK_IS_WIDGET(self->reply_indicator_box)) {
      gtk_widget_set_visible(self->reply_indicator_box, FALSE);
    }
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

/* ---- Media upload completion (called by signal handler) ---- */

void nostr_gtk_composer_upload_complete(NostrGtkComposer *self,
                                     const char *url,
                                     const char *sha256,
                                     const char *mime_type,
                                     gint64 size) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));
  g_return_if_fail(url != NULL);

  self->upload_in_progress = FALSE;

  if (self->upload_progress_box && GTK_IS_WIDGET(self->upload_progress_box)) {
    gtk_widget_set_visible(self->upload_progress_box, FALSE);
  }
  if (self->upload_spinner && GTK_IS_SPINNER(self->upload_spinner)) {
    gtk_spinner_set_spinning(GTK_SPINNER(self->upload_spinner), FALSE);
  }
  if (self->btn_attach && GTK_IS_WIDGET(self->btn_attach)) {
    gtk_widget_set_sensitive(self->btn_attach, TRUE);
  }

  /* Store media metadata for NIP-92 imeta tags */
  if (!self->uploaded_media) {
    self->uploaded_media = g_ptr_array_new_with_free_func(composer_media_free);
  }
  NostrGtkComposerMedia *media = g_new0(NostrGtkComposerMedia, 1);
  media->url = g_strdup(url);
  media->sha256 = g_strdup(sha256);
  media->mime_type = g_strdup(mime_type);
  media->size = size;
  g_ptr_array_add(self->uploaded_media, media);

  /* Insert the URL into the text view */
  if (self->text_view && GTK_IS_TEXT_VIEW(self->text_view)) {
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text_view));
    GtkTextIter cursor;
    gtk_text_buffer_get_iter_at_mark(buf, &cursor, gtk_text_buffer_get_insert(buf));

    GtkTextIter line_start = cursor;
    gtk_text_iter_set_line_offset(&line_start, 0);
    if (!gtk_text_iter_equal(&cursor, &line_start)) {
      gtk_text_buffer_insert(buf, &cursor, "\n", 1);
    }

    gtk_text_buffer_insert(buf, &cursor, url, -1);
    gtk_text_buffer_insert(buf, &cursor, "\n", 1);

    g_message("composer: inserted uploaded media URL: %s (sha256=%s, type=%s, size=%" G_GINT64_FORMAT ")",
              url, sha256 ? sha256 : "?",
              mime_type ? mime_type : "?", size);
  }
}

void nostr_gtk_composer_upload_failed(NostrGtkComposer *self,
                                   const char *message) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));

  self->upload_in_progress = FALSE;

  if (self->upload_progress_box && GTK_IS_WIDGET(self->upload_progress_box)) {
    gtk_widget_set_visible(self->upload_progress_box, FALSE);
  }
  if (self->upload_spinner && GTK_IS_SPINNER(self->upload_spinner)) {
    gtk_spinner_set_spinning(GTK_SPINNER(self->upload_spinner), FALSE);
  }
  if (self->btn_attach && GTK_IS_WIDGET(self->btn_attach)) {
    gtk_widget_set_sensitive(self->btn_attach, TRUE);
  }

  char *toast_msg = g_strdup_printf("Upload failed: %s", message ? message : "unknown error");
  composer_show_toast(self, toast_msg);
  g_free(toast_msg);
}

/* ---- NIP-37: Draft management ---- */

void nostr_gtk_composer_load_draft(NostrGtkComposer *self,
                                const NostrGtkComposerDraftInfo *info) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));
  g_return_if_fail(info != NULL);

  nostr_gtk_composer_clear(self);

  g_free(self->current_draft_d_tag);
  self->current_draft_d_tag = g_strdup(info->d_tag);

  if (info->content && self->text_view && GTK_IS_TEXT_VIEW(self->text_view)) {
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text_view));
    gtk_text_buffer_set_text(buf, info->content, -1);
  }

  if (info->subject && self->subject_entry && GTK_IS_ENTRY(self->subject_entry)) {
    gtk_editable_set_text(GTK_EDITABLE(self->subject_entry), info->subject);
  }

  if (info->reply_to_id) {
    g_free(self->reply_to_id);
    self->reply_to_id = g_strdup(info->reply_to_id);
  }
  if (info->root_id) {
    g_free(self->root_id);
    self->root_id = g_strdup(info->root_id);
  }
  if (info->reply_to_pubkey) {
    g_free(self->reply_to_pubkey);
    self->reply_to_pubkey = g_strdup(info->reply_to_pubkey);
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

  if (info->quote_id) {
    g_free(self->quote_id);
    self->quote_id = g_strdup(info->quote_id);
  }
  if (info->quote_pubkey) {
    g_free(self->quote_pubkey);
    self->quote_pubkey = g_strdup(info->quote_pubkey);
  }
  if (info->quote_nostr_uri) {
    g_free(self->quote_nostr_uri);
    self->quote_nostr_uri = g_strdup(info->quote_nostr_uri);
    if (!info->reply_to_pubkey) {
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

  nostr_gtk_composer_set_sensitive(self, info->is_sensitive);

  g_message("composer: loaded draft d_tag=%s kind=%d",
            info->d_tag ? info->d_tag : "(null)",
            info->target_kind);

  composer_show_toast(self, "Draft loaded");
  g_signal_emit(self, signals[SIGNAL_DRAFT_LOADED], 0);
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

void nostr_gtk_composer_add_draft_row(NostrGtkComposer *self,
                                   const char *d_tag,
                                   const char *preview_text,
                                   gint64 updated_at) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));
  if (!self->drafts_list || !GTK_IS_LIST_BOX(self->drafts_list)) return;

  /* Hide empty label */
  if (self->drafts_empty_label && GTK_IS_WIDGET(self->drafts_empty_label)) {
    gtk_widget_set_visible(self->drafts_empty_label, FALSE);
  }

  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_start(box, 6);
  gtk_widget_set_margin_end(box, 6);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);

  GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(content_box, TRUE);

  /* Content preview */
  GtkWidget *preview_label = gtk_label_new(preview_text);
  gtk_label_set_xalign(GTK_LABEL(preview_label), 0);
  gtk_label_set_ellipsize(GTK_LABEL(preview_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(preview_label), 30);
  gtk_box_append(GTK_BOX(content_box), preview_label);

  /* Timestamp */
  if (updated_at > 0) {
    GDateTime *dt = g_date_time_new_from_unix_local(updated_at);
    if (dt) {
      char *time_str = g_date_time_format(dt, "%b %d, %H:%M");
      GtkWidget *time_label = gtk_label_new(time_str);
      gtk_widget_add_css_class(time_label, "dim-label");
      gtk_widget_add_css_class(time_label, "caption");
      gtk_label_set_xalign(GTK_LABEL(time_label), 0);
      gtk_box_append(GTK_BOX(content_box), time_label);
      g_free(time_str);
      g_date_time_unref(dt);
    }
  }

  gtk_box_append(GTK_BOX(box), content_box);

  /* Load button */
  GtkWidget *btn_load = gtk_button_new_from_icon_name("document-open-symbolic");
  gtk_widget_set_tooltip_text(btn_load, "Load draft");
  gtk_widget_add_css_class(btn_load, "flat");
  g_object_set_data_full(G_OBJECT(btn_load), "draft-d-tag", g_strdup(d_tag), g_free);
  g_signal_connect(btn_load, "clicked", G_CALLBACK(on_draft_row_load_clicked), self);
  gtk_box_append(GTK_BOX(box), btn_load);

  /* Delete button */
  GtkWidget *btn_delete = gtk_button_new_from_icon_name("user-trash-symbolic");
  gtk_widget_set_tooltip_text(btn_delete, "Delete draft");
  gtk_widget_add_css_class(btn_delete, "flat");
  gtk_widget_add_css_class(btn_delete, "destructive-action");
  g_object_set_data_full(G_OBJECT(btn_delete), "draft-d-tag", g_strdup(d_tag), g_free);
  g_signal_connect(btn_delete, "clicked", G_CALLBACK(on_draft_row_delete_clicked), self);
  gtk_box_append(GTK_BOX(box), btn_delete);

  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  gtk_list_box_append(GTK_LIST_BOX(self->drafts_list), row);
}

void nostr_gtk_composer_clear_draft_rows(NostrGtkComposer *self) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));
  if (!self->drafts_list || !GTK_IS_LIST_BOX(self->drafts_list)) return;

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->drafts_list))) != NULL) {
    gtk_list_box_remove(GTK_LIST_BOX(self->drafts_list), child);
  }

  /* Show empty label by default */
  if (self->drafts_empty_label && GTK_IS_WIDGET(self->drafts_empty_label)) {
    gtk_widget_set_visible(self->drafts_empty_label, TRUE);
  }
}

void nostr_gtk_composer_draft_save_complete(NostrGtkComposer *self,
                                         gboolean success,
                                         const char *error_message,
                                         const char *d_tag) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));

  if (success) {
    if (d_tag) {
      g_free(self->current_draft_d_tag);
      self->current_draft_d_tag = g_strdup(d_tag);
    }
    composer_show_toast(self, "Draft saved");
    g_signal_emit(self, signals[SIGNAL_DRAFT_SAVED], 0);
  } else {
    char *msg = g_strdup_printf("Failed to save draft: %s",
                                 error_message ? error_message : "unknown error");
    composer_show_toast(self, msg);
    g_free(msg);
  }
}

void nostr_gtk_composer_draft_delete_complete(NostrGtkComposer *self,
                                           const char *d_tag,
                                           gboolean success) {
  g_return_if_fail(NOSTR_GTK_IS_COMPOSER(self));
  (void)d_tag;

  if (success) {
    composer_show_toast(self, "Draft deleted");
    /* Request refresh of drafts list */
    g_signal_emit(self, signals[SIGNAL_LOAD_DRAFTS_REQUESTED], 0);
    g_signal_emit(self, signals[SIGNAL_DRAFT_DELETED], 0);
  }
}

char *nostr_gtk_composer_get_text(NostrGtkComposer *self) {
  g_return_val_if_fail(NOSTR_GTK_IS_COMPOSER(self), NULL);
  if (!self->text_view || !GTK_IS_TEXT_VIEW(self->text_view)) return NULL;

  GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text_view));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buf, &start, &end);
  return gtk_text_buffer_get_text(buf, &start, &end, FALSE);
}
