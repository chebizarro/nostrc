/**
 * GnostrDmConversationView - NIP-17 DM Conversation Thread View
 *
 * Displays a 1-to-1 encrypted DM conversation with message bubbles
 * and a composer for sending new messages.
 */

#include "gnostr-dm-conversation-view.h"
#include "gnostr-avatar-cache.h"
#include "../util/dm_files.h"
#include <time.h>

struct _GnostrDmConversationView {
    GtkWidget parent_instance;

    /* Template widgets */
    GtkBox *header_box;
    GtkButton *btn_back;
    GtkButton *btn_peer_avatar;
    GtkPicture *peer_avatar_image;
    GtkLabel *peer_avatar_initials;
    GtkLabel *lbl_peer_name;
    GtkScrolledWindow *scroller;
    GtkListBox *message_list;
    GtkStack *content_stack;
    GtkSpinner *loading_spinner;
    GtkBox *composer_box;
    GtkButton *btn_attach;
    GtkTextView *message_entry;
    GtkButton *btn_send;

    /* Data */
    char *peer_pubkey;
    char *user_pubkey;
    guint message_count;
};

G_DEFINE_TYPE(GnostrDmConversationView, gnostr_dm_conversation_view, GTK_TYPE_WIDGET)

enum {
    SIGNAL_SEND_MESSAGE,
    SIGNAL_SEND_FILE,
    SIGNAL_GO_BACK,
    SIGNAL_OPEN_PROFILE,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

/* ---- Helpers ---- */

GnostrDmMessage *
gnostr_dm_message_copy(const GnostrDmMessage *msg)
{
    if (!msg) return NULL;
    GnostrDmMessage *copy = g_new0(GnostrDmMessage, 1);
    copy->event_id = g_strdup(msg->event_id);
    copy->content = g_strdup(msg->content);
    copy->created_at = msg->created_at;
    copy->is_outgoing = msg->is_outgoing;
    copy->file_url = g_strdup(msg->file_url);
    copy->file_type = g_strdup(msg->file_type);
    copy->decryption_key = g_strdup(msg->decryption_key);
    copy->decryption_nonce = g_strdup(msg->decryption_nonce);
    copy->original_hash = g_strdup(msg->original_hash);
    copy->file_size = msg->file_size;
    return copy;
}

void
gnostr_dm_message_free(GnostrDmMessage *msg)
{
    if (!msg) return;
    g_free(msg->event_id);
    g_free(msg->content);
    g_free(msg->file_url);
    g_free(msg->file_type);
    g_free(msg->decryption_key);
    g_free(msg->decryption_nonce);
    g_free(msg->original_hash);
    g_free(msg);
}

static char *
get_initials(const char *name)
{
    if (!name || !*name)
        return g_strdup("?");
    gunichar c = g_utf8_get_char(name);
    char buf[7];
    int len = g_unichar_to_utf8(g_unichar_toupper(c), buf);
    buf[len] = '\0';
    return g_strdup(buf);
}

static char *
format_msg_time(gint64 timestamp)
{
    if (timestamp <= 0) return g_strdup("");

    GDateTime *dt = g_date_time_new_from_unix_local(timestamp);
    if (!dt) return g_strdup("");

    GDateTime *now = g_date_time_new_now_local();
    if (!now) {
        g_date_time_unref(dt);
        return g_strdup("");
    }

    gint64 diff = g_date_time_to_unix(now) - g_date_time_to_unix(dt);

    char *result;
    if (diff < 60)
        result = g_strdup("now");
    else if (diff < 3600)
        result = g_strdup_printf("%" G_GINT64_FORMAT "m ago", diff / 60);
    else if (diff < 86400)
        result = g_date_time_format(dt, "%l:%M %p");
    else
        result = g_date_time_format(dt, "%b %d, %l:%M %p");

    g_date_time_unref(dt);
    g_date_time_unref(now);
    return result;
}

/* ---- File preview async download ---- */

typedef struct {
    GWeakRef picture_ref;
    GnostrDmFileMessage *file_msg;
} FilePreviewCtx;

static void
on_file_preview_downloaded(uint8_t *data, gsize size, GError *error, gpointer user_data)
{
    FilePreviewCtx *ctx = user_data;
    GtkWidget *picture = g_weak_ref_get(&ctx->picture_ref);

    if (picture && data && !error) {
        GBytes *bytes = g_bytes_new_take(data, size);
        GdkTexture *texture = gdk_texture_new_from_bytes(bytes, NULL);
        g_bytes_unref(bytes);

        if (texture) {
            gtk_picture_set_paintable(GTK_PICTURE(picture), GDK_PAINTABLE(texture));
            g_object_unref(texture);
        }
        g_object_unref(picture);
    } else {
        g_free(data);
        if (picture) g_object_unref(picture);
    }

    gnostr_dm_file_message_free(ctx->file_msg);
    g_weak_ref_clear(&ctx->picture_ref);
    g_free(ctx);
}

/* Build a GnostrDmFileMessage from a GnostrDmMessage's file fields */
static GnostrDmFileMessage *
build_file_msg_from_dm_message(const GnostrDmMessage *msg)
{
    if (!msg->file_url) return NULL;

    GnostrDmFileMessage *fm = g_new0(GnostrDmFileMessage, 1);
    fm->file_url = g_strdup(msg->file_url);
    fm->file_type = g_strdup(msg->file_type);
    fm->decryption_key_b64 = g_strdup(msg->decryption_key);
    fm->decryption_nonce_b64 = g_strdup(msg->decryption_nonce);
    fm->original_hash = g_strdup(msg->original_hash);
    fm->size = msg->file_size;
    fm->encryption_algorithm = g_strdup("aes-gcm");
    return fm;
}

/* Download handler for file save */
typedef struct {
    char *save_path;
} FileSaveCtx;

static void
on_file_save_downloaded(uint8_t *data, gsize size, GError *error, gpointer user_data)
{
    FileSaveCtx *ctx = user_data;
    if (data && !error && ctx->save_path) {
        GError *write_err = NULL;
        if (!g_file_set_contents(ctx->save_path, (const char *)data, size, &write_err)) {
            g_warning("[DM] Failed to save file: %s",
                      write_err ? write_err->message : "unknown");
            g_clear_error(&write_err);
        } else {
            g_message("[DM] File saved to %s", ctx->save_path);
        }
    }
    g_free(data);
    g_free(ctx->save_path);
    g_free(ctx);
}

static void
on_file_save_response(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    GnostrDmFileMessage *file_msg = user_data;
    GError *error = NULL;

    g_autoptr(GFile) file = gtk_file_dialog_save_finish(dialog, res, &error);
    if (file) {
        char *path = g_file_get_path(file);
        if (path) {
            FileSaveCtx *ctx = g_new0(FileSaveCtx, 1);
            ctx->save_path = path;
            gnostr_dm_file_download_and_decrypt_async(file_msg,
                                                       on_file_save_downloaded,
                                                       ctx, NULL);
        }
    } else {
        gnostr_dm_file_message_free(file_msg);
    }
    g_clear_error(&error);
}

static void
on_file_bubble_clicked(GtkButton *button, gpointer user_data)
{
    (void)user_data;

    /* Reconstruct file message from button data */
    const char *file_url = g_object_get_data(G_OBJECT(button), "dm-file-url");
    if (!file_url) return;

    GnostrDmFileMessage *fm = g_new0(GnostrDmFileMessage, 1);
    fm->file_url = g_strdup(file_url);
    fm->file_type = g_strdup(g_object_get_data(G_OBJECT(button), "dm-file-type"));
    fm->decryption_key_b64 = g_strdup(g_object_get_data(G_OBJECT(button), "dm-file-key"));
    fm->decryption_nonce_b64 = g_strdup(g_object_get_data(G_OBJECT(button), "dm-file-nonce"));
    fm->original_hash = g_strdup(g_object_get_data(G_OBJECT(button), "dm-file-hash"));
    fm->encryption_algorithm = g_strdup("aes-gcm");

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save File");

    /* Suggest filename from hash + extension */
    const char *ext = "bin";
    if (fm->file_type) {
        if (g_str_has_prefix(fm->file_type, "image/jpeg")) ext = "jpg";
        else if (g_str_has_prefix(fm->file_type, "image/png")) ext = "png";
        else if (g_str_has_prefix(fm->file_type, "image/gif")) ext = "gif";
        else if (g_str_has_prefix(fm->file_type, "image/webp")) ext = "webp";
        else if (g_str_has_prefix(fm->file_type, "video/mp4")) ext = "mp4";
        else if (g_str_has_prefix(fm->file_type, "audio/mp3") ||
                 g_str_has_prefix(fm->file_type, "audio/mpeg")) ext = "mp3";
    }
    char *suggested = g_strdup_printf("dm-file.%s", ext);
    gtk_file_dialog_set_initial_name(dialog, suggested);
    g_free(suggested);

    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(button));
    gtk_file_dialog_save(dialog, GTK_WINDOW(root), NULL,
                          on_file_save_response, fm);
    g_object_unref(dialog);
}

/* Create a text message bubble */
static GtkWidget *
create_text_bubble(const GnostrDmMessage *msg)
{
    GtkWidget *bubble = gtk_label_new(msg->content);
    gtk_label_set_wrap(GTK_LABEL(bubble), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(bubble), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(bubble), 0.0);
    gtk_label_set_selectable(GTK_LABEL(bubble), TRUE);
    gtk_widget_set_margin_start(bubble, 8);
    gtk_widget_set_margin_end(bubble, 8);
    gtk_widget_set_margin_top(bubble, 6);
    gtk_widget_set_margin_bottom(bubble, 6);
    gtk_label_set_max_width_chars(GTK_LABEL(bubble), 50);
    return bubble;
}

/* Create a file attachment bubble */
static GtkWidget *
create_file_bubble(const GnostrDmMessage *msg)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);

    gboolean is_image = msg->file_type &&
                         g_str_has_prefix(msg->file_type, "image/");

    if (is_image) {
        /* Image preview */
        GtkWidget *picture = gtk_picture_new();
        gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
        gtk_widget_set_size_request(picture, 200, 150);
        gtk_widget_add_css_class(picture, "dm-image-preview");
        gtk_box_append(GTK_BOX(box), picture);

        /* Async download+decrypt for preview */
        GnostrDmFileMessage *fm = build_file_msg_from_dm_message(msg);
        if (fm) {
            FilePreviewCtx *ctx = g_new0(FilePreviewCtx, 1);
            g_weak_ref_init(&ctx->picture_ref, picture);
            ctx->file_msg = fm;
            gnostr_dm_file_download_and_decrypt_async(fm,
                                                       on_file_preview_downloaded,
                                                       ctx, NULL);
        }
    } else {
        /* Non-image file: icon + type + size */
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);

        GtkWidget *icon = gtk_image_new_from_icon_name("document-save-symbolic");
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
        gtk_box_append(GTK_BOX(hbox), icon);

        GtkWidget *info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

        const char *type_str = msg->file_type ? msg->file_type : "File";
        GtkWidget *type_label = gtk_label_new(type_str);
        gtk_label_set_xalign(GTK_LABEL(type_label), 0.0);
        gtk_box_append(GTK_BOX(info_box), type_label);

        if (msg->file_size > 0) {
            char *size_str = g_format_size(msg->file_size);
            GtkWidget *size_label = gtk_label_new(size_str);
            gtk_widget_add_css_class(size_label, "dim-label");
            gtk_label_set_xalign(GTK_LABEL(size_label), 0.0);
            gtk_box_append(GTK_BOX(info_box), size_label);
            g_free(size_str);
        }

        gtk_box_append(GTK_BOX(hbox), info_box);
        gtk_box_append(GTK_BOX(box), hbox);
    }

    /* "Tap to save" hint */
    GtkWidget *save_hint = gtk_label_new("Tap to save");
    gtk_widget_add_css_class(save_hint, "dim-label");
    gtk_widget_add_css_class(save_hint, "caption");
    gtk_box_append(GTK_BOX(box), save_hint);

    return box;
}

/* Create a message bubble row widget */
static GtkWidget *
create_message_row(const GnostrDmMessage *msg)
{
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(outer, 12);
    gtk_widget_set_margin_end(outer, 12);
    gtk_widget_set_margin_top(outer, 4);
    gtk_widget_set_margin_bottom(outer, 4);

    /* Determine bubble content based on message type */
    GtkWidget *bubble_content;
    if (msg->file_url) {
        bubble_content = create_file_bubble(msg);
    } else {
        bubble_content = create_text_bubble(msg);
    }

    GtkWidget *bubble_frame;

    if (msg->file_url) {
        /* File bubbles: wrap in a clickable button for save action */
        bubble_frame = gtk_button_new();
        gtk_button_set_has_frame(GTK_BUTTON(bubble_frame), FALSE);
        gtk_button_set_child(GTK_BUTTON(bubble_frame), bubble_content);
        gtk_widget_add_css_class(bubble_frame, "flat");

        /* Store file metadata for download on click */
        g_object_set_data_full(G_OBJECT(bubble_frame), "dm-file-url",
                               g_strdup(msg->file_url), g_free);
        g_object_set_data_full(G_OBJECT(bubble_frame), "dm-file-type",
                               g_strdup(msg->file_type), g_free);
        g_object_set_data_full(G_OBJECT(bubble_frame), "dm-file-key",
                               g_strdup(msg->decryption_key), g_free);
        g_object_set_data_full(G_OBJECT(bubble_frame), "dm-file-nonce",
                               g_strdup(msg->decryption_nonce), g_free);
        g_object_set_data_full(G_OBJECT(bubble_frame), "dm-file-hash",
                               g_strdup(msg->original_hash), g_free);
        g_signal_connect(bubble_frame, "clicked",
                         G_CALLBACK(on_file_bubble_clicked), NULL);
    } else {
        bubble_frame = gtk_frame_new(NULL);
        gtk_frame_set_child(GTK_FRAME(bubble_frame), bubble_content);
    }
    gtk_widget_set_hexpand(bubble_frame, FALSE);

    if (msg->is_outgoing) {
        gtk_widget_set_halign(bubble_frame, GTK_ALIGN_END);
        gtk_widget_add_css_class(bubble_frame, "dm-bubble-outgoing");
        gtk_widget_set_halign(outer, GTK_ALIGN_END);
    } else {
        gtk_widget_set_halign(bubble_frame, GTK_ALIGN_START);
        gtk_widget_add_css_class(bubble_frame, "dm-bubble-incoming");
        gtk_widget_set_halign(outer, GTK_ALIGN_START);
    }

    gtk_box_append(GTK_BOX(outer), bubble_frame);

    /* Timestamp */
    char *time_str = format_msg_time(msg->created_at);
    GtkWidget *time_label = gtk_label_new(time_str);
    g_free(time_str);
    gtk_widget_add_css_class(time_label, "dim-label");
    gtk_widget_add_css_class(time_label, "caption");

    if (msg->is_outgoing) {
        gtk_widget_set_halign(time_label, GTK_ALIGN_END);
    } else {
        gtk_widget_set_halign(time_label, GTK_ALIGN_START);
    }

    gtk_box_append(GTK_BOX(outer), time_label);

    return outer;
}

/* ---- Signal handlers ---- */

static void
on_back_clicked(GtkButton *button, GnostrDmConversationView *self)
{
    (void)button;
    g_signal_emit(self, signals[SIGNAL_GO_BACK], 0);
}

static void
on_peer_avatar_clicked(GtkButton *button, GnostrDmConversationView *self)
{
    (void)button;
    if (self->peer_pubkey)
        g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->peer_pubkey);
}

static void
on_send_clicked(GtkButton *button, GnostrDmConversationView *self)
{
    (void)button;

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->message_entry);
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    char *content = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    if (content && *content) {
        g_signal_emit(self, signals[SIGNAL_SEND_MESSAGE], 0, content);
        gtk_text_buffer_set_text(buffer, "", 0);
    }

    g_free(content);
}

static void
on_attach_file_chosen(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    GnostrDmConversationView *self = GNOSTR_DM_CONVERSATION_VIEW(user_data);

    GError *error = NULL;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(dialog, res, &error);
    if (file) {
        char *path = g_file_get_path(file);
        if (path) {
            g_signal_emit(self, signals[SIGNAL_SEND_FILE], 0, path);
            g_free(path);
        }
    }
    g_clear_error(&error);
}

static void
on_attach_clicked(GtkButton *button, GnostrDmConversationView *self)
{
    (void)button;

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Attach File");

    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
    gtk_file_dialog_open(dialog, GTK_WINDOW(root), NULL,
                          on_attach_file_chosen, self);
    g_object_unref(dialog);
}

static gboolean
on_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
               GdkModifierType state, GnostrDmConversationView *self)
{
    (void)controller;
    (void)keycode;

    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        if (!(state & GDK_SHIFT_MASK)) {
            on_send_clicked(NULL, self);
            return TRUE;
        }
    }

    return FALSE;
}

/* ---- GObject lifecycle ---- */

static void
gnostr_dm_conversation_view_dispose(GObject *object)
{
    GnostrDmConversationView *self = GNOSTR_DM_CONVERSATION_VIEW(object);

    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
    if (child)
        gtk_widget_unparent(child);

    G_OBJECT_CLASS(gnostr_dm_conversation_view_parent_class)->dispose(object);
}

static void
gnostr_dm_conversation_view_finalize(GObject *object)
{
    GnostrDmConversationView *self = GNOSTR_DM_CONVERSATION_VIEW(object);

    g_free(self->peer_pubkey);
    g_free(self->user_pubkey);

    G_OBJECT_CLASS(gnostr_dm_conversation_view_parent_class)->finalize(object);
}

static void
gnostr_dm_conversation_view_class_init(GnostrDmConversationViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = gnostr_dm_conversation_view_dispose;
    object_class->finalize = gnostr_dm_conversation_view_finalize;

    gtk_widget_class_set_template_from_resource(widget_class,
        "/org/gnostr/ui/ui/widgets/gnostr-dm-conversation-view.ui");

    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, header_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, btn_back);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, btn_peer_avatar);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, peer_avatar_image);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, peer_avatar_initials);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, lbl_peer_name);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, scroller);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, message_list);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, content_stack);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, loading_spinner);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, composer_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, btn_attach);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, message_entry);
    gtk_widget_class_bind_template_child(widget_class, GnostrDmConversationView, btn_send);

    signals[SIGNAL_SEND_MESSAGE] = g_signal_new(
        "send-message",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_SEND_FILE] = g_signal_new(
        "send-file",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_GO_BACK] = g_signal_new(
        "go-back",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);

    signals[SIGNAL_OPEN_PROFILE] = g_signal_new(
        "open-profile",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    gtk_widget_class_set_css_name(widget_class, "dm-conversation");
    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gnostr_dm_conversation_view_init(GnostrDmConversationView *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));

    self->peer_pubkey = NULL;
    self->user_pubkey = NULL;
    self->message_count = 0;

    g_signal_connect(self->btn_back, "clicked", G_CALLBACK(on_back_clicked), self);
    g_signal_connect(self->btn_peer_avatar, "clicked", G_CALLBACK(on_peer_avatar_clicked), self);
    g_signal_connect(self->btn_attach, "clicked", G_CALLBACK(on_attach_clicked), self);
    g_signal_connect(self->btn_send, "clicked", G_CALLBACK(on_send_clicked), self);

    GtkEventController *key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(on_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self->message_entry), key_controller);

    gtk_list_box_set_selection_mode(self->message_list, GTK_SELECTION_NONE);

    /* Start with empty state */
    gtk_stack_set_visible_child_name(self->content_stack, "empty");
}

/* ---- Public API ---- */

GnostrDmConversationView *
gnostr_dm_conversation_view_new(void)
{
    return g_object_new(GNOSTR_TYPE_DM_CONVERSATION_VIEW, NULL);
}

void
gnostr_dm_conversation_view_set_peer(GnostrDmConversationView *self,
                                      const char *pubkey_hex,
                                      const char *display_name,
                                      const char *avatar_url)
{
    g_return_if_fail(GNOSTR_IS_DM_CONVERSATION_VIEW(self));

    g_free(self->peer_pubkey);
    self->peer_pubkey = g_strdup(pubkey_hex);

    /* Set display name */
    const char *name = display_name;
    if (!name || !*name) {
        /* Truncate pubkey as fallback name */
        if (pubkey_hex && strlen(pubkey_hex) >= 12) {
            char npub_short[16];
            snprintf(npub_short, sizeof(npub_short), "%.8s...", pubkey_hex);
            gtk_label_set_text(self->lbl_peer_name, npub_short);
        } else {
            gtk_label_set_text(self->lbl_peer_name, pubkey_hex ? pubkey_hex : "Unknown");
        }
    } else {
        gtk_label_set_text(self->lbl_peer_name, name);
    }

    /* Set initials */
    char *initials = get_initials(name);
    gtk_label_set_text(self->peer_avatar_initials, initials);
    g_free(initials);

    /* Load avatar if available */
    if (avatar_url && *avatar_url) {
        gnostr_avatar_download_async(avatar_url,
                                     GTK_WIDGET(self->peer_avatar_image),
                                     GTK_WIDGET(self->peer_avatar_initials));
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->peer_avatar_image), FALSE);
    }
}

const char *
gnostr_dm_conversation_view_get_peer_pubkey(GnostrDmConversationView *self)
{
    g_return_val_if_fail(GNOSTR_IS_DM_CONVERSATION_VIEW(self), NULL);
    return self->peer_pubkey;
}

void
gnostr_dm_conversation_view_set_user_pubkey(GnostrDmConversationView *self,
                                             const char *pubkey_hex)
{
    g_return_if_fail(GNOSTR_IS_DM_CONVERSATION_VIEW(self));
    g_free(self->user_pubkey);
    self->user_pubkey = g_strdup(pubkey_hex);
}

void
gnostr_dm_conversation_view_add_message(GnostrDmConversationView *self,
                                         const GnostrDmMessage *msg)
{
    g_return_if_fail(GNOSTR_IS_DM_CONVERSATION_VIEW(self));
    g_return_if_fail(msg != NULL);

    GtkWidget *row = create_message_row(msg);
    gtk_list_box_append(self->message_list, row);
    self->message_count++;

    if (self->message_count == 1) {
        gtk_stack_set_visible_child_name(self->content_stack, "messages");
    }
}

static gint
compare_messages_by_time(gconstpointer a, gconstpointer b)
{
    const GnostrDmMessage *ma = *(const GnostrDmMessage **)a;
    const GnostrDmMessage *mb = *(const GnostrDmMessage **)b;
    if (ma->created_at < mb->created_at) return -1;
    if (ma->created_at > mb->created_at) return 1;
    return 0;
}

void
gnostr_dm_conversation_view_set_messages(GnostrDmConversationView *self,
                                          GPtrArray *messages)
{
    g_return_if_fail(GNOSTR_IS_DM_CONVERSATION_VIEW(self));

    /* Clear existing */
    gnostr_dm_conversation_view_clear(self);

    if (!messages || messages->len == 0) {
        gtk_stack_set_visible_child_name(self->content_stack, "empty");
        return;
    }

    /* Sort by timestamp */
    g_ptr_array_sort(messages, compare_messages_by_time);

    /* Add all messages */
    for (guint i = 0; i < messages->len; i++) {
        GnostrDmMessage *msg = g_ptr_array_index(messages, i);
        GtkWidget *row = create_message_row(msg);
        gtk_list_box_append(self->message_list, row);
        self->message_count++;
    }

    gtk_stack_set_visible_child_name(self->content_stack, "messages");
}

void
gnostr_dm_conversation_view_clear(GnostrDmConversationView *self)
{
    g_return_if_fail(GNOSTR_IS_DM_CONVERSATION_VIEW(self));

    /* Remove all rows from list box */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->message_list))) != NULL) {
        gtk_list_box_remove(self->message_list, child);
    }
    self->message_count = 0;

    gtk_stack_set_visible_child_name(self->content_stack, "empty");
}

void
gnostr_dm_conversation_view_set_loading(GnostrDmConversationView *self,
                                         gboolean is_loading)
{
    g_return_if_fail(GNOSTR_IS_DM_CONVERSATION_VIEW(self));

    if (is_loading) {
        gtk_spinner_set_spinning(self->loading_spinner, TRUE);
        gtk_stack_set_visible_child_name(self->content_stack, "loading");
    } else {
        gtk_spinner_set_spinning(self->loading_spinner, FALSE);
        if (self->message_count > 0)
            gtk_stack_set_visible_child_name(self->content_stack, "messages");
        else
            gtk_stack_set_visible_child_name(self->content_stack, "empty");
    }
}

void
gnostr_dm_conversation_view_scroll_to_bottom(GnostrDmConversationView *self)
{
    g_return_if_fail(GNOSTR_IS_DM_CONVERSATION_VIEW(self));

    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(self->scroller);
    gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
}
