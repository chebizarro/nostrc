/*
 * gnostr-thread-card.c - NIP-7D Forum Thread Card Widget Implementation
 *
 * Displays a kind 11 thread root event in a card format.
 */

#include "gnostr-thread-card.h"
#include "gnostr-avatar-cache.h"
#include "../util/nip7d_threads.h"
#include "../util/nip05.h"
#include <glib/gi18n.h>
#include <string.h>

#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

/* Maximum length for content preview */
#define MAX_PREVIEW_LENGTH 200

struct _GnostrThreadCard {
    GtkWidget parent_instance;

    /* Layout widgets */
    GtkWidget *root_box;
    GtkWidget *header_box;
    GtkWidget *content_box;
    GtkWidget *footer_box;

    /* Avatar widgets */
    GtkWidget *btn_avatar;
    GtkWidget *avatar_overlay;
    GtkWidget *avatar_image;
    GtkWidget *avatar_initials;

    /* Author info widgets */
    GtkWidget *author_box;
    GtkWidget *btn_author_name;
    GtkWidget *lbl_author_name;
    GtkWidget *lbl_author_handle;
    GtkWidget *nip05_badge;

    /* Thread info widgets */
    GtkWidget *lbl_subject;
    GtkWidget *lbl_content_preview;
    GtkWidget *lbl_timestamp;

    /* Hashtags flow box */
    GtkWidget *hashtags_flow_box;

    /* Stats widgets */
    GtkWidget *stats_box;
    GtkWidget *reply_count_box;
    GtkWidget *lbl_reply_count;
    GtkWidget *lbl_last_activity;

    /* Action button */
    GtkWidget *btn_reply;

    /* State */
    char *event_id;
    char *pubkey_hex;
    char *subject;
    char *nip05;
    gint64 created_at;
    gint64 last_activity;
    guint reply_count;
    gboolean is_logged_in;

    /* NIP-05 verification */
    GCancellable *nip05_cancellable;

#ifdef HAVE_SOUP3
    GCancellable *avatar_cancellable;
    SoupSession *session;
#endif
};

G_DEFINE_TYPE(GnostrThreadCard, gnostr_thread_card, GTK_TYPE_WIDGET)

enum {
    SIGNAL_THREAD_CLICKED,
    SIGNAL_AUTHOR_CLICKED,
    SIGNAL_REPLY_CLICKED,
    SIGNAL_HASHTAG_CLICKED,
    N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Forward declarations */
static void setup_card_ui(GnostrThreadCard *self);
static void set_avatar_initials(GnostrThreadCard *self, const char *display, const char *handle);

/* ============================================================================
 * GObject Lifecycle
 * ============================================================================ */

static void
gnostr_thread_card_dispose(GObject *obj)
{
    GnostrThreadCard *self = GNOSTR_THREAD_CARD(obj);

    if (self->nip05_cancellable) {
        g_cancellable_cancel(self->nip05_cancellable);
        g_clear_object(&self->nip05_cancellable);
    }

#ifdef HAVE_SOUP3
    if (self->avatar_cancellable) {
        g_cancellable_cancel(self->avatar_cancellable);
        g_clear_object(&self->avatar_cancellable);
    }
    g_clear_object(&self->session);
#endif

    /* Clear child widgets before dispose */
    if (self->root_box) {
        gtk_widget_unparent(self->root_box);
        self->root_box = NULL;
    }

    G_OBJECT_CLASS(gnostr_thread_card_parent_class)->dispose(obj);
}

static void
gnostr_thread_card_finalize(GObject *obj)
{
    GnostrThreadCard *self = GNOSTR_THREAD_CARD(obj);

    g_clear_pointer(&self->event_id, g_free);
    g_clear_pointer(&self->pubkey_hex, g_free);
    g_clear_pointer(&self->subject, g_free);
    g_clear_pointer(&self->nip05, g_free);

    G_OBJECT_CLASS(gnostr_thread_card_parent_class)->finalize(obj);
}

/* ============================================================================
 * Click Handlers
 * ============================================================================ */

static void
on_card_clicked(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data)
{
    GnostrThreadCard *self = GNOSTR_THREAD_CARD(user_data);
    (void)gesture;
    (void)n_press;
    (void)x;
    (void)y;

    if (self->event_id && *self->event_id) {
        g_signal_emit(self, signals[SIGNAL_THREAD_CLICKED], 0, self->event_id);
    }
}

static void
on_avatar_clicked(GtkButton *btn, gpointer user_data)
{
    GnostrThreadCard *self = GNOSTR_THREAD_CARD(user_data);
    (void)btn;

    if (self->pubkey_hex && *self->pubkey_hex) {
        g_signal_emit(self, signals[SIGNAL_AUTHOR_CLICKED], 0, self->pubkey_hex);
    }
}

static void
on_author_name_clicked(GtkButton *btn, gpointer user_data)
{
    GnostrThreadCard *self = GNOSTR_THREAD_CARD(user_data);
    (void)btn;

    if (self->pubkey_hex && *self->pubkey_hex) {
        g_signal_emit(self, signals[SIGNAL_AUTHOR_CLICKED], 0, self->pubkey_hex);
    }
}

static void
on_reply_clicked(GtkButton *btn, gpointer user_data)
{
    GnostrThreadCard *self = GNOSTR_THREAD_CARD(user_data);
    (void)btn;

    if (self->event_id && *self->event_id) {
        g_signal_emit(self, signals[SIGNAL_REPLY_CLICKED], 0, self->event_id);
    }
}

static void
on_hashtag_clicked(GtkButton *btn, gpointer user_data)
{
    GnostrThreadCard *self = GNOSTR_THREAD_CARD(user_data);
    const char *hashtag = g_object_get_data(G_OBJECT(btn), "hashtag");

    if (hashtag && *hashtag) {
        g_signal_emit(self, signals[SIGNAL_HASHTAG_CLICKED], 0, hashtag);
    }
}

/* ============================================================================
 * UI Setup
 * ============================================================================ */

static void
set_avatar_initials(GnostrThreadCard *self, const char *display, const char *handle)
{
    if (!GTK_IS_LABEL(self->avatar_initials)) return;

    const char *src = (display && *display) ? display : (handle && *handle ? handle : "AN");
    char initials[3] = {0};
    int i = 0;

    for (const char *p = src; *p && i < 2; p++) {
        if (g_ascii_isalnum(*p)) {
            initials[i++] = g_ascii_toupper(*p);
        }
    }
    if (i == 0) {
        initials[0] = 'A';
        initials[1] = 'N';
    }

    gtk_label_set_text(GTK_LABEL(self->avatar_initials), initials);
    if (self->avatar_image) gtk_widget_set_visible(self->avatar_image, FALSE);
    gtk_widget_set_visible(self->avatar_initials, TRUE);
}

static GtkWidget *
create_hashtag_pill(GnostrThreadCard *self, const char *hashtag)
{
    GtkWidget *btn = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
    gtk_widget_add_css_class(btn, "hashtag-pill");
    gtk_widget_add_css_class(btn, "flat");

    char *label_text = g_strdup_printf("#%s", hashtag);
    gtk_button_set_label(GTK_BUTTON(btn), label_text);
    g_free(label_text);

    g_object_set_data_full(G_OBJECT(btn), "hashtag", g_strdup(hashtag), g_free);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_hashtag_clicked), self);

    return btn;
}

static void
setup_card_ui(GnostrThreadCard *self)
{
    /* Root container - clickable card */
    self->root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_parent(self->root_box, GTK_WIDGET(self));
    gtk_widget_add_css_class(self->root_box, "thread-card");
    gtk_widget_add_css_class(self->root_box, "card");
    gtk_widget_set_margin_start(self->root_box, 12);
    gtk_widget_set_margin_end(self->root_box, 12);
    gtk_widget_set_margin_top(self->root_box, 8);
    gtk_widget_set_margin_bottom(self->root_box, 8);

    /* Add click gesture to root */
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(on_card_clicked), self);
    gtk_widget_add_controller(self->root_box, GTK_EVENT_CONTROLLER(click));

    /* Header: Avatar + Author info + Timestamp */
    self->header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(self->root_box), self->header_box);

    /* Avatar button */
    self->btn_avatar = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(self->btn_avatar), FALSE);
    gtk_widget_add_css_class(self->btn_avatar, "circular");
    gtk_widget_add_css_class(self->btn_avatar, "avatar-button");
    g_signal_connect(self->btn_avatar, "clicked", G_CALLBACK(on_avatar_clicked), self);

    self->avatar_overlay = gtk_overlay_new();
    gtk_widget_set_size_request(self->avatar_overlay, 40, 40);
    gtk_button_set_child(GTK_BUTTON(self->btn_avatar), self->avatar_overlay);

    /* Avatar image */
    self->avatar_image = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(self->avatar_image), GTK_CONTENT_FIT_COVER);
    gtk_widget_set_size_request(self->avatar_image, 40, 40);
    gtk_widget_add_css_class(self->avatar_image, "avatar");
    gtk_widget_set_visible(self->avatar_image, FALSE);
    gtk_overlay_set_child(GTK_OVERLAY(self->avatar_overlay), self->avatar_image);

    /* Avatar initials fallback */
    self->avatar_initials = gtk_label_new("AN");
    gtk_widget_add_css_class(self->avatar_initials, "avatar-initials");
    gtk_widget_set_halign(self->avatar_initials, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(self->avatar_initials, GTK_ALIGN_CENTER);
    gtk_overlay_add_overlay(GTK_OVERLAY(self->avatar_overlay), self->avatar_initials);

    gtk_box_append(GTK_BOX(self->header_box), self->btn_avatar);

    /* Author info box */
    self->author_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(self->author_box, TRUE);
    gtk_box_append(GTK_BOX(self->header_box), self->author_box);

    /* Author name row */
    GtkWidget *name_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(self->author_box), name_row);

    self->btn_author_name = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(self->btn_author_name), FALSE);
    gtk_widget_add_css_class(self->btn_author_name, "flat");
    g_signal_connect(self->btn_author_name, "clicked", G_CALLBACK(on_author_name_clicked), self);

    self->lbl_author_name = gtk_label_new(_("Anonymous"));
    gtk_label_set_ellipsize(GTK_LABEL(self->lbl_author_name), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(self->lbl_author_name, "author-name");
    gtk_button_set_child(GTK_BUTTON(self->btn_author_name), self->lbl_author_name);
    gtk_box_append(GTK_BOX(name_row), self->btn_author_name);

    /* NIP-05 badge */
    self->nip05_badge = gtk_image_new_from_icon_name("emblem-ok-symbolic");
    gtk_widget_add_css_class(self->nip05_badge, "nip05-badge");
    gtk_widget_set_tooltip_text(self->nip05_badge, _("NIP-05 Verified"));
    gtk_widget_set_visible(self->nip05_badge, FALSE);
    gtk_box_append(GTK_BOX(name_row), self->nip05_badge);

    /* Author handle */
    self->lbl_author_handle = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(self->lbl_author_handle), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(self->lbl_author_handle, "dim-label");
    gtk_widget_add_css_class(self->lbl_author_handle, "author-handle");
    gtk_widget_set_halign(self->lbl_author_handle, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(self->author_box), self->lbl_author_handle);

    /* Timestamp (right side of header) */
    self->lbl_timestamp = gtk_label_new("");
    gtk_widget_add_css_class(self->lbl_timestamp, "dim-label");
    gtk_widget_add_css_class(self->lbl_timestamp, "timestamp");
    gtk_widget_set_valign(self->lbl_timestamp, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(self->header_box), self->lbl_timestamp);

    /* Content: Subject + Preview */
    self->content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append(GTK_BOX(self->root_box), self->content_box);

    /* Subject (thread title) */
    self->lbl_subject = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(self->lbl_subject), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(self->lbl_subject), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(self->lbl_subject), 80);
    gtk_label_set_ellipsize(GTK_LABEL(self->lbl_subject), PANGO_ELLIPSIZE_END);
    gtk_label_set_lines(GTK_LABEL(self->lbl_subject), 2);
    gtk_widget_add_css_class(self->lbl_subject, "thread-subject");
    gtk_widget_add_css_class(self->lbl_subject, "title-3");
    gtk_widget_set_halign(self->lbl_subject, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(self->content_box), self->lbl_subject);

    /* Content preview */
    self->lbl_content_preview = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(self->lbl_content_preview), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(self->lbl_content_preview), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(self->lbl_content_preview), 80);
    gtk_label_set_ellipsize(GTK_LABEL(self->lbl_content_preview), PANGO_ELLIPSIZE_END);
    gtk_label_set_lines(GTK_LABEL(self->lbl_content_preview), 3);
    gtk_widget_add_css_class(self->lbl_content_preview, "thread-content-preview");
    gtk_widget_add_css_class(self->lbl_content_preview, "dim-label");
    gtk_widget_set_halign(self->lbl_content_preview, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(self->content_box), self->lbl_content_preview);

    /* Hashtags flow box */
    self->hashtags_flow_box = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(self->hashtags_flow_box), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(self->hashtags_flow_box), FALSE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->hashtags_flow_box), 10);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(self->hashtags_flow_box), 4);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(self->hashtags_flow_box), 4);
    gtk_widget_set_visible(self->hashtags_flow_box, FALSE);
    gtk_box_append(GTK_BOX(self->content_box), self->hashtags_flow_box);

    /* Footer: Stats + Actions */
    self->footer_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(self->footer_box, 4);
    gtk_box_append(GTK_BOX(self->root_box), self->footer_box);

    /* Stats box */
    self->stats_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_hexpand(self->stats_box, TRUE);
    gtk_box_append(GTK_BOX(self->footer_box), self->stats_box);

    /* Reply count */
    self->reply_count_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *reply_icon = gtk_image_new_from_icon_name("mail-reply-all-symbolic");
    gtk_widget_add_css_class(reply_icon, "dim-label");
    gtk_box_append(GTK_BOX(self->reply_count_box), reply_icon);

    self->lbl_reply_count = gtk_label_new("0");
    gtk_widget_add_css_class(self->lbl_reply_count, "dim-label");
    gtk_box_append(GTK_BOX(self->reply_count_box), self->lbl_reply_count);
    gtk_box_append(GTK_BOX(self->stats_box), self->reply_count_box);

    /* Last activity */
    GtkWidget *activity_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *activity_icon = gtk_image_new_from_icon_name("appointment-soon-symbolic");
    gtk_widget_add_css_class(activity_icon, "dim-label");
    gtk_box_append(GTK_BOX(activity_box), activity_icon);

    self->lbl_last_activity = gtk_label_new("");
    gtk_widget_add_css_class(self->lbl_last_activity, "dim-label");
    gtk_box_append(GTK_BOX(activity_box), self->lbl_last_activity);
    gtk_box_append(GTK_BOX(self->stats_box), activity_box);

    /* Reply button */
    self->btn_reply = gtk_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(self->btn_reply), "mail-reply-sender-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(self->btn_reply), FALSE);
    gtk_widget_add_css_class(self->btn_reply, "flat");
    gtk_widget_set_tooltip_text(self->btn_reply, _("Reply to thread"));
    g_signal_connect(self->btn_reply, "clicked", G_CALLBACK(on_reply_clicked), self);
    gtk_box_append(GTK_BOX(self->footer_box), self->btn_reply);
}

/* ============================================================================
 * GObject Class Init
 * ============================================================================ */

static void
gnostr_thread_card_class_init(GnostrThreadCardClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    GObjectClass *obj_class = G_OBJECT_CLASS(klass);

    obj_class->dispose = gnostr_thread_card_dispose;
    obj_class->finalize = gnostr_thread_card_finalize;

    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);

    /* Signals */
    signals[SIGNAL_THREAD_CLICKED] = g_signal_new("thread-clicked",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_AUTHOR_CLICKED] = g_signal_new("author-clicked",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_REPLY_CLICKED] = g_signal_new("reply-clicked",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_HASHTAG_CLICKED] = g_signal_new("hashtag-clicked",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gnostr_thread_card_init(GnostrThreadCard *self)
{
    gtk_widget_add_css_class(GTK_WIDGET(self), "gnostr-thread-card");

#ifdef HAVE_SOUP3
    self->avatar_cancellable = g_cancellable_new();
    self->session = soup_session_new();
    soup_session_set_timeout(self->session, 30);
#endif

    setup_card_ui(self);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

GnostrThreadCard *
gnostr_thread_card_new(void)
{
    return g_object_new(GNOSTR_TYPE_THREAD_CARD, NULL);
}

void
gnostr_thread_card_set_thread(GnostrThreadCard *self,
                               const char *event_id,
                               const char *subject,
                               const char *content_preview,
                               gint64 created_at)
{
    g_return_if_fail(GNOSTR_IS_THREAD_CARD(self));

    /* Store event ID */
    g_clear_pointer(&self->event_id, g_free);
    self->event_id = g_strdup(event_id);

    /* Store subject */
    g_clear_pointer(&self->subject, g_free);
    self->subject = g_strdup(subject);

    self->created_at = created_at;

    /* Set subject label */
    if (GTK_IS_LABEL(self->lbl_subject)) {
        gtk_label_set_text(GTK_LABEL(self->lbl_subject),
            (subject && *subject) ? subject : _("Untitled Thread"));
    }

    /* Set content preview */
    if (GTK_IS_LABEL(self->lbl_content_preview)) {
        if (content_preview && *content_preview) {
            /* Truncate if too long */
            if (strlen(content_preview) > MAX_PREVIEW_LENGTH) {
                char *truncated = g_strndup(content_preview, MAX_PREVIEW_LENGTH);
                char *preview_text = g_strdup_printf("%s...", truncated);
                gtk_label_set_text(GTK_LABEL(self->lbl_content_preview), preview_text);
                g_free(truncated);
                g_free(preview_text);
            } else {
                gtk_label_set_text(GTK_LABEL(self->lbl_content_preview), content_preview);
            }
            gtk_widget_set_visible(self->lbl_content_preview, TRUE);
        } else {
            gtk_widget_set_visible(self->lbl_content_preview, FALSE);
        }
    }

    /* Set timestamp */
    if (GTK_IS_LABEL(self->lbl_timestamp)) {
        char *ts = gnostr_thread_format_timestamp(created_at);
        gtk_label_set_text(GTK_LABEL(self->lbl_timestamp), ts);
        g_free(ts);
    }

    /* Default last activity to creation time */
    if (self->last_activity == 0) {
        self->last_activity = created_at;
        if (GTK_IS_LABEL(self->lbl_last_activity)) {
            char *activity = gnostr_thread_format_timestamp(created_at);
            gtk_label_set_text(GTK_LABEL(self->lbl_last_activity), activity);
            g_free(activity);
        }
    }
}

void
gnostr_thread_card_set_author(GnostrThreadCard *self,
                               const char *pubkey_hex,
                               const char *display_name,
                               const char *handle,
                               const char *avatar_url)
{
    g_return_if_fail(GNOSTR_IS_THREAD_CARD(self));

    /* Store pubkey */
    g_clear_pointer(&self->pubkey_hex, g_free);
    self->pubkey_hex = g_strdup(pubkey_hex);

    /* Set author name */
    if (GTK_IS_LABEL(self->lbl_author_name)) {
        const char *name = (display_name && *display_name) ? display_name :
                          (handle && *handle) ? handle : _("Anonymous");
        gtk_label_set_text(GTK_LABEL(self->lbl_author_name), name);
    }

    /* Set handle */
    if (GTK_IS_LABEL(self->lbl_author_handle)) {
        if (handle && *handle) {
            char *handle_text = g_strdup_printf("@%s", handle);
            gtk_label_set_text(GTK_LABEL(self->lbl_author_handle), handle_text);
            gtk_widget_set_visible(self->lbl_author_handle, TRUE);
            g_free(handle_text);
        } else {
            gtk_widget_set_visible(self->lbl_author_handle, FALSE);
        }
    }

    /* Set avatar */
    set_avatar_initials(self, display_name, handle);

#ifdef HAVE_SOUP3
    if (avatar_url && *avatar_url && GTK_IS_PICTURE(self->avatar_image)) {
        GdkTexture *cached = gnostr_avatar_try_load_cached(avatar_url);
        if (cached) {
            gtk_picture_set_paintable(GTK_PICTURE(self->avatar_image), GDK_PAINTABLE(cached));
            gtk_widget_set_visible(self->avatar_image, TRUE);
            gtk_widget_set_visible(self->avatar_initials, FALSE);
            g_object_unref(cached);
        } else {
            gnostr_avatar_download_async(avatar_url, self->avatar_image, self->avatar_initials);
        }
    }
#else
    (void)avatar_url;
#endif
}

void
gnostr_thread_card_set_reply_count(GnostrThreadCard *self,
                                    guint count)
{
    g_return_if_fail(GNOSTR_IS_THREAD_CARD(self));

    self->reply_count = count;

    if (GTK_IS_LABEL(self->lbl_reply_count)) {
        char *count_str = g_strdup_printf("%u", count);
        gtk_label_set_text(GTK_LABEL(self->lbl_reply_count), count_str);
        g_free(count_str);
    }
}

void
gnostr_thread_card_set_last_activity(GnostrThreadCard *self,
                                      gint64 timestamp)
{
    g_return_if_fail(GNOSTR_IS_THREAD_CARD(self));

    self->last_activity = timestamp;

    if (GTK_IS_LABEL(self->lbl_last_activity)) {
        char *activity = gnostr_thread_format_timestamp(timestamp);
        gtk_label_set_text(GTK_LABEL(self->lbl_last_activity), activity);
        g_free(activity);
    }
}

void
gnostr_thread_card_set_hashtags(GnostrThreadCard *self,
                                 const char * const *hashtags)
{
    g_return_if_fail(GNOSTR_IS_THREAD_CARD(self));

    gnostr_thread_card_clear_hashtags(self);

    if (!hashtags) return;

    for (int i = 0; hashtags[i] != NULL; i++) {
        gnostr_thread_card_add_hashtag(self, hashtags[i]);
    }
}

void
gnostr_thread_card_add_hashtag(GnostrThreadCard *self,
                                const char *hashtag)
{
    g_return_if_fail(GNOSTR_IS_THREAD_CARD(self));
    g_return_if_fail(hashtag != NULL && *hashtag != '\0');

    if (!GTK_IS_FLOW_BOX(self->hashtags_flow_box)) return;

    GtkWidget *pill = create_hashtag_pill(self, hashtag);
    gtk_flow_box_append(GTK_FLOW_BOX(self->hashtags_flow_box), pill);
    gtk_widget_set_visible(self->hashtags_flow_box, TRUE);
}

void
gnostr_thread_card_clear_hashtags(GnostrThreadCard *self)
{
    g_return_if_fail(GNOSTR_IS_THREAD_CARD(self));

    if (!GTK_IS_FLOW_BOX(self->hashtags_flow_box)) return;

    /* Remove all children */
    GtkWidget *child = gtk_widget_get_first_child(self->hashtags_flow_box);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_flow_box_remove(GTK_FLOW_BOX(self->hashtags_flow_box), child);
        child = next;
    }

    gtk_widget_set_visible(self->hashtags_flow_box, FALSE);
}

/* NIP-05 verification callback */
static void
on_nip05_verified(GnostrNip05Result *result, gpointer user_data)
{
    GnostrThreadCard *self = GNOSTR_THREAD_CARD(user_data);

    if (!GNOSTR_IS_THREAD_CARD(self) || !GTK_IS_IMAGE(self->nip05_badge)) {
        gnostr_nip05_result_free(result);
        return;
    }

    gboolean verified = (result && result->status == GNOSTR_NIP05_STATUS_VERIFIED);
    gtk_widget_set_visible(self->nip05_badge, verified);

    if (verified && result->identifier) {
        gtk_widget_set_tooltip_text(self->nip05_badge, result->identifier);
    }

    gnostr_nip05_result_free(result);
}

void
gnostr_thread_card_set_nip05(GnostrThreadCard *self,
                              const char *nip05,
                              const char *pubkey_hex)
{
    g_return_if_fail(GNOSTR_IS_THREAD_CARD(self));

    g_clear_pointer(&self->nip05, g_free);
    self->nip05 = g_strdup(nip05);

    if (self->nip05_cancellable) {
        g_cancellable_cancel(self->nip05_cancellable);
        g_clear_object(&self->nip05_cancellable);
    }

    if (!nip05 || !*nip05 || !pubkey_hex) {
        gtk_widget_set_visible(self->nip05_badge, FALSE);
        return;
    }

    /* Start async verification */
    self->nip05_cancellable = g_cancellable_new();
    gnostr_nip05_verify_async(nip05, pubkey_hex, on_nip05_verified, self, self->nip05_cancellable);
}

void
gnostr_thread_card_set_logged_in(GnostrThreadCard *self,
                                  gboolean logged_in)
{
    g_return_if_fail(GNOSTR_IS_THREAD_CARD(self));

    self->is_logged_in = logged_in;

    if (GTK_IS_WIDGET(self->btn_reply)) {
        gtk_widget_set_sensitive(self->btn_reply, logged_in);
    }
}

const char *
gnostr_thread_card_get_event_id(GnostrThreadCard *self)
{
    g_return_val_if_fail(GNOSTR_IS_THREAD_CARD(self), NULL);
    return self->event_id;
}

const char *
gnostr_thread_card_get_author_pubkey(GnostrThreadCard *self)
{
    g_return_val_if_fail(GNOSTR_IS_THREAD_CARD(self), NULL);
    return self->pubkey_hex;
}

const char *
gnostr_thread_card_get_subject(GnostrThreadCard *self)
{
    g_return_val_if_fail(GNOSTR_IS_THREAD_CARD(self), NULL);
    return self->subject;
}
