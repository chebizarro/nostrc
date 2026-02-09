/*
 * gnostr-nip7d-thread-view.c - NIP-7D Forum Thread Full View Implementation
 *
 * Displays a complete NIP-7D forum thread with threaded replies.
 */

#include "gnostr-nip7d-thread-view.h"
#include "gnostr-avatar-cache.h"
#include "gnostr-profile-provider.h"
#include "../util/nip7d_threads.h"
#include "../util/utils.h"
#include "../storage_ndb.h"
#include "../util/relays.h"
#include "nostr_filter.h"
#include "nostr-filter.h"
#include <glib/gi18n.h>
#include <string.h>
#include <time.h>

#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

/* Maximum nesting depth for display */
#define MAX_DISPLAY_DEPTH 8

/* Default number of replies to load */
#define DEFAULT_REPLY_LIMIT 50

/* Indentation per nesting level in pixels */
#define INDENT_PER_LEVEL 24

/* Reply row data attached to widgets */
typedef struct {
    char *event_id;
    char *pubkey_hex;
    char *content;
    char *parent_id;
    gint64 created_at;
    guint depth;
    gboolean is_collapsed;
    char *display_name;
    char *handle;
    char *avatar_url;
} ReplyRowData;

static void
reply_row_data_free(ReplyRowData *data)
{
    if (!data) return;
    g_free(data->event_id);
    g_free(data->pubkey_hex);
    g_free(data->content);
    g_free(data->parent_id);
    g_free(data->display_name);
    g_free(data->handle);
    g_free(data->avatar_url);
    g_free(data);
}

struct _GnostrNip7dThreadView {
    GtkWidget parent_instance;

    /* Header widgets */
    GtkWidget *header_box;
    GtkWidget *btn_back;
    GtkWidget *lbl_title;
    GtkWidget *btn_refresh;

    /* Thread content widgets */
    GtkWidget *scroll_window;
    GtkWidget *content_box;

    /* Thread root display */
    GtkWidget *thread_root_box;
    GtkWidget *thread_subject;
    GtkWidget *thread_author_box;
    GtkWidget *thread_author_avatar;
    GtkWidget *thread_author_initials;
    GtkWidget *thread_author_name;
    GtkWidget *thread_timestamp;
    GtkWidget *thread_content;
    GtkWidget *thread_hashtags_box;

    /* Reply list */
    GtkWidget *replies_box;
    GtkWidget *replies_separator;
    GtkWidget *lbl_replies_header;

    /* Loading/empty states */
    GtkWidget *loading_box;
    GtkWidget *loading_spinner;
    GtkWidget *empty_box;
    GtkWidget *lbl_empty;

    /* Composer widgets */
    GtkWidget *composer_box;
    GtkWidget *composer_reply_indicator;
    GtkWidget *composer_text;
    GtkWidget *btn_submit_reply;

    /* Load more button */
    GtkWidget *btn_load_more;

    /* State */
    GnostrThread *thread;
    GPtrArray *replies;              /* Array of GnostrThreadReply* (owned) */
    GHashTable *reply_widgets;       /* event_id -> GtkWidget* */
    GHashTable *collapsed_replies;   /* event_id -> gboolean */
    char *reply_parent_id;           /* Current reply target */
    gboolean is_logged_in;
    gboolean is_loading;
    guint loaded_reply_count;

    /* Network */
    GCancellable *fetch_cancellable;
    /* Uses gnostr_get_shared_query_pool() instead of per-widget pool */

    /* Profile tracking */
    GHashTable *profiles_requested;  /* pubkey_hex -> gboolean */

    /* nostrc-46g: Track ancestor event IDs we've already attempted to fetch
     * to prevent duplicate requests and enable proper chain traversal */
    GHashTable *ancestors_fetched;   /* event_id_hex -> gboolean */
    guint ancestor_fetch_depth;      /* Current chain traversal depth */

#ifdef HAVE_SOUP3
    SoupSession *session;
#endif
};

G_DEFINE_TYPE(GnostrNip7dThreadView, gnostr_nip7d_thread_view, GTK_TYPE_WIDGET)

enum {
    SIGNAL_CLOSE_REQUESTED,
    SIGNAL_AUTHOR_CLICKED,
    SIGNAL_REPLY_SUBMITTED,
    SIGNAL_HASHTAG_CLICKED,
    SIGNAL_NEED_PROFILE,
    N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Forward declarations */
static void setup_view_ui(GnostrNip7dThreadView *self);
static void rebuild_replies_ui(GnostrNip7dThreadView *self);
static void set_loading_state(GnostrNip7dThreadView *self, gboolean loading);
static void fetch_thread_from_relays(GnostrNip7dThreadView *self);
static void fetch_missing_ancestors(GnostrNip7dThreadView *self);
static GtkWidget *create_reply_row(GnostrNip7dThreadView *self, GnostrThreadReply *reply);
static void apply_profile_to_thread_author(GnostrNip7dThreadView *self, GnostrProfileMeta *meta);
static void apply_profile_to_reply_row(GnostrNip7dThreadView *self, GtkWidget *row, GnostrProfileMeta *meta);

/* ============================================================================
 * GObject Lifecycle
 * ============================================================================ */

static void
gnostr_nip7d_thread_view_dispose(GObject *obj)
{
    GnostrNip7dThreadView *self = GNOSTR_NIP7D_THREAD_VIEW(obj);

    if (self->fetch_cancellable) {
        g_cancellable_cancel(self->fetch_cancellable);
        g_clear_object(&self->fetch_cancellable);
    }

    /* Shared query pool is managed globally - do not clear here */

#ifdef HAVE_SOUP3
    g_clear_object(&self->session);
#endif

    g_clear_pointer(&self->reply_widgets, g_hash_table_unref);
    g_clear_pointer(&self->collapsed_replies, g_hash_table_unref);
    g_clear_pointer(&self->profiles_requested, g_hash_table_unref);
    g_clear_pointer(&self->ancestors_fetched, g_hash_table_unref);

    if (self->header_box) {
        gtk_widget_unparent(self->header_box);
        self->header_box = NULL;
    }

    G_OBJECT_CLASS(gnostr_nip7d_thread_view_parent_class)->dispose(obj);
}

static void
gnostr_nip7d_thread_view_finalize(GObject *obj)
{
    GnostrNip7dThreadView *self = GNOSTR_NIP7D_THREAD_VIEW(obj);

    if (self->thread) {
        gnostr_thread_free(self->thread);
        self->thread = NULL;
    }

    if (self->replies) {
        g_ptr_array_unref(self->replies);
        self->replies = NULL;
    }

    g_clear_pointer(&self->reply_parent_id, g_free);

    G_OBJECT_CLASS(gnostr_nip7d_thread_view_parent_class)->finalize(obj);
}

/* ============================================================================
 * Signal Handlers
 * ============================================================================ */

static void
on_back_clicked(GtkButton *btn, gpointer user_data)
{
    GnostrNip7dThreadView *self = GNOSTR_NIP7D_THREAD_VIEW(user_data);
    (void)btn;
    g_signal_emit(self, signals[SIGNAL_CLOSE_REQUESTED], 0);
}

static void
on_refresh_clicked(GtkButton *btn, gpointer user_data)
{
    GnostrNip7dThreadView *self = GNOSTR_NIP7D_THREAD_VIEW(user_data);
    (void)btn;
    gnostr_nip7d_thread_view_refresh(self);
}

static void
on_author_clicked(GtkButton *btn, gpointer user_data)
{
    GnostrNip7dThreadView *self = GNOSTR_NIP7D_THREAD_VIEW(user_data);
    const char *pubkey = g_object_get_data(G_OBJECT(btn), "pubkey");

    if (pubkey && *pubkey) {
        g_signal_emit(self, signals[SIGNAL_AUTHOR_CLICKED], 0, pubkey);
    }
}

static void
on_hashtag_clicked(GtkButton *btn, gpointer user_data)
{
    GnostrNip7dThreadView *self = GNOSTR_NIP7D_THREAD_VIEW(user_data);
    const char *hashtag = g_object_get_data(G_OBJECT(btn), "hashtag");

    if (hashtag && *hashtag) {
        g_signal_emit(self, signals[SIGNAL_HASHTAG_CLICKED], 0, hashtag);
    }
}

static void
on_reply_button_clicked(GtkButton *btn, gpointer user_data)
{
    GnostrNip7dThreadView *self = GNOSTR_NIP7D_THREAD_VIEW(user_data);
    const char *reply_id = g_object_get_data(G_OBJECT(btn), "reply_id");

    gnostr_nip7d_thread_view_set_reply_parent(self, reply_id);

    /* Focus the composer */
    if (GTK_IS_TEXT_VIEW(self->composer_text)) {
        gtk_widget_grab_focus(self->composer_text);
    }
}

static void
on_collapse_clicked(GtkButton *btn, gpointer user_data)
{
    GnostrNip7dThreadView *self = GNOSTR_NIP7D_THREAD_VIEW(user_data);
    const char *reply_id = g_object_get_data(G_OBJECT(btn), "reply_id");

    if (!reply_id) return;

    gboolean currently_collapsed = GPOINTER_TO_INT(
        g_hash_table_lookup(self->collapsed_replies, reply_id));

    gnostr_nip7d_thread_view_collapse_reply(self, reply_id, !currently_collapsed);
}

static void
on_submit_reply_clicked(GtkButton *btn, gpointer user_data)
{
    GnostrNip7dThreadView *self = GNOSTR_NIP7D_THREAD_VIEW(user_data);
    (void)btn;

    if (!GTK_IS_TEXT_VIEW(self->composer_text)) return;

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->composer_text));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    char *content = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    if (!content || !g_strstrip(content)[0]) {
        g_free(content);
        return;
    }

    /* Determine parent ID */
    const char *parent_id = self->reply_parent_id;
    if (!parent_id && self->thread) {
        parent_id = self->thread->event_id;
    }

    g_signal_emit(self, signals[SIGNAL_REPLY_SUBMITTED], 0, content, parent_id);

    /* Clear composer */
    gtk_text_buffer_set_text(buffer, "", 0);
    gnostr_nip7d_thread_view_set_reply_parent(self, NULL);

    g_free(content);
}

static void
on_load_more_clicked(GtkButton *btn, gpointer user_data)
{
    GnostrNip7dThreadView *self = GNOSTR_NIP7D_THREAD_VIEW(user_data);
    (void)btn;
    gnostr_nip7d_thread_view_load_more_replies(self, DEFAULT_REPLY_LIMIT);
}

/* ============================================================================
 * Helper: Set avatar initials
 * ============================================================================ */

static void
set_avatar_initials(GtkWidget *initials_label, const char *display, const char *handle)
{
    if (!GTK_IS_LABEL(initials_label)) return;

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

    gtk_label_set_text(GTK_LABEL(initials_label), initials);
}

/* ============================================================================
 * UI Setup
 * ============================================================================ */

static GtkWidget *
create_hashtag_pill(GnostrNip7dThreadView *self, const char *hashtag)
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
setup_view_ui(GnostrNip7dThreadView *self)
{
    /* Main vertical layout */
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_parent(main_box, GTK_WIDGET(self));
    self->header_box = main_box;

    /* Header bar */
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(header, "toolbar");
    gtk_widget_set_margin_start(header, 8);
    gtk_widget_set_margin_end(header, 8);
    gtk_widget_set_margin_top(header, 8);
    gtk_widget_set_margin_bottom(header, 8);
    gtk_box_append(GTK_BOX(main_box), header);

    /* Back button */
    self->btn_back = gtk_button_new_from_icon_name("go-previous-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(self->btn_back), FALSE);
    gtk_widget_set_tooltip_text(self->btn_back, _("Back"));
    g_signal_connect(self->btn_back, "clicked", G_CALLBACK(on_back_clicked), self);
    gtk_box_append(GTK_BOX(header), self->btn_back);

    /* Title */
    self->lbl_title = gtk_label_new(_("Thread"));
    gtk_widget_add_css_class(self->lbl_title, "title-3");
    gtk_widget_set_hexpand(self->lbl_title, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(self->lbl_title), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(header), self->lbl_title);

    /* Refresh button */
    self->btn_refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(self->btn_refresh), FALSE);
    gtk_widget_set_tooltip_text(self->btn_refresh, _("Refresh"));
    g_signal_connect(self->btn_refresh, "clicked", G_CALLBACK(on_refresh_clicked), self);
    gtk_box_append(GTK_BOX(header), self->btn_refresh);

    /* Scrolled window for content */
    self->scroll_window = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scroll_window),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(self->scroll_window, TRUE);
    gtk_box_append(GTK_BOX(main_box), self->scroll_window);

    /* Content box inside scroll */
    self->content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(self->content_box, 12);
    gtk_widget_set_margin_end(self->content_box, 12);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scroll_window), self->content_box);

    /* Thread root display box */
    self->thread_root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(self->thread_root_box, "thread-root");
    gtk_widget_set_margin_top(self->thread_root_box, 12);
    gtk_widget_set_margin_bottom(self->thread_root_box, 12);
    gtk_box_append(GTK_BOX(self->content_box), self->thread_root_box);

    /* Thread subject */
    self->thread_subject = gtk_label_new("");
    gtk_widget_add_css_class(self->thread_subject, "title-2");
    gtk_label_set_wrap(GTK_LABEL(self->thread_subject), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(self->thread_subject), PANGO_WRAP_WORD_CHAR);
    gtk_widget_set_halign(self->thread_subject, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(self->thread_root_box), self->thread_subject);

    /* Thread author row */
    self->thread_author_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(self->thread_root_box), self->thread_author_box);

    /* Author avatar */
    GtkWidget *avatar_btn = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(avatar_btn), FALSE);
    gtk_widget_add_css_class(avatar_btn, "circular");

    GtkWidget *avatar_overlay = gtk_overlay_new();
    gtk_widget_set_size_request(avatar_overlay, 32, 32);
    gtk_button_set_child(GTK_BUTTON(avatar_btn), avatar_overlay);

    self->thread_author_avatar = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(self->thread_author_avatar), GTK_CONTENT_FIT_COVER);
    gtk_widget_set_size_request(self->thread_author_avatar, 32, 32);
    gtk_widget_add_css_class(self->thread_author_avatar, "avatar");
    gtk_widget_set_visible(self->thread_author_avatar, FALSE);
    gtk_overlay_set_child(GTK_OVERLAY(avatar_overlay), self->thread_author_avatar);

    self->thread_author_initials = gtk_label_new("AN");
    gtk_widget_add_css_class(self->thread_author_initials, "avatar-initials");
    gtk_widget_set_halign(self->thread_author_initials, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(self->thread_author_initials, GTK_ALIGN_CENTER);
    gtk_overlay_add_overlay(GTK_OVERLAY(avatar_overlay), self->thread_author_initials);

    gtk_box_append(GTK_BOX(self->thread_author_box), avatar_btn);

    /* Author name button */
    GtkWidget *author_name_btn = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(author_name_btn), FALSE);
    self->thread_author_name = gtk_label_new(_("Anonymous"));
    gtk_widget_add_css_class(self->thread_author_name, "author-name");
    gtk_button_set_child(GTK_BUTTON(author_name_btn), self->thread_author_name);
    g_signal_connect(author_name_btn, "clicked", G_CALLBACK(on_author_clicked), self);
    gtk_box_append(GTK_BOX(self->thread_author_box), author_name_btn);

    /* Timestamp */
    self->thread_timestamp = gtk_label_new("");
    gtk_widget_add_css_class(self->thread_timestamp, "dim-label");
    gtk_box_append(GTK_BOX(self->thread_author_box), self->thread_timestamp);

    /* Thread content */
    self->thread_content = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(self->thread_content), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(self->thread_content), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(self->thread_content), TRUE);
    gtk_widget_set_halign(self->thread_content, GTK_ALIGN_START);
    gtk_widget_set_margin_top(self->thread_content, 8);
    gtk_box_append(GTK_BOX(self->thread_root_box), self->thread_content);

    /* Thread hashtags */
    self->thread_hashtags_box = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(self->thread_hashtags_box), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->thread_hashtags_box), 10);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(self->thread_hashtags_box), 4);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(self->thread_hashtags_box), 4);
    gtk_widget_set_visible(self->thread_hashtags_box, FALSE);
    gtk_widget_set_margin_top(self->thread_hashtags_box, 8);
    gtk_box_append(GTK_BOX(self->thread_root_box), self->thread_hashtags_box);

    /* Separator */
    self->replies_separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(self->replies_separator, 8);
    gtk_widget_set_margin_bottom(self->replies_separator, 8);
    gtk_box_append(GTK_BOX(self->content_box), self->replies_separator);

    /* Replies header */
    self->lbl_replies_header = gtk_label_new(_("Replies"));
    gtk_widget_add_css_class(self->lbl_replies_header, "heading");
    gtk_widget_set_halign(self->lbl_replies_header, GTK_ALIGN_START);
    gtk_widget_set_margin_bottom(self->lbl_replies_header, 8);
    gtk_box_append(GTK_BOX(self->content_box), self->lbl_replies_header);

    /* Replies container */
    self->replies_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(self->content_box), self->replies_box);

    /* Load more button */
    self->btn_load_more = gtk_button_new_with_label(_("Load more replies"));
    gtk_widget_add_css_class(self->btn_load_more, "flat");
    gtk_widget_set_margin_top(self->btn_load_more, 8);
    gtk_widget_set_margin_bottom(self->btn_load_more, 8);
    gtk_widget_set_visible(self->btn_load_more, FALSE);
    g_signal_connect(self->btn_load_more, "clicked", G_CALLBACK(on_load_more_clicked), self);
    gtk_box_append(GTK_BOX(self->content_box), self->btn_load_more);

    /* Loading state */
    self->loading_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_halign(self->loading_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(self->loading_box, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(self->loading_box, TRUE);
    gtk_widget_set_visible(self->loading_box, FALSE);
    gtk_box_append(GTK_BOX(self->content_box), self->loading_box);

    self->loading_spinner = gtk_spinner_new();
    gtk_widget_set_size_request(self->loading_spinner, 32, 32);
    gtk_box_append(GTK_BOX(self->loading_box), self->loading_spinner);

    GtkWidget *loading_label = gtk_label_new(_("Loading thread..."));
    gtk_widget_add_css_class(loading_label, "dim-label");
    gtk_box_append(GTK_BOX(self->loading_box), loading_label);

    /* Empty state */
    self->empty_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_halign(self->empty_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(self->empty_box, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(self->empty_box, TRUE);
    gtk_widget_set_visible(self->empty_box, FALSE);
    gtk_box_append(GTK_BOX(self->content_box), self->empty_box);

    GtkWidget *empty_icon = gtk_image_new_from_icon_name("dialog-information-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(empty_icon), 48);
    gtk_widget_add_css_class(empty_icon, "dim-label");
    gtk_box_append(GTK_BOX(self->empty_box), empty_icon);

    self->lbl_empty = gtk_label_new(_("Thread not found"));
    gtk_widget_add_css_class(self->lbl_empty, "dim-label");
    gtk_box_append(GTK_BOX(self->empty_box), self->lbl_empty);

    /* Composer (at bottom, outside scroll) */
    self->composer_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(self->composer_box, "composer-box");
    gtk_widget_set_margin_start(self->composer_box, 12);
    gtk_widget_set_margin_end(self->composer_box, 12);
    gtk_widget_set_margin_top(self->composer_box, 8);
    gtk_widget_set_margin_bottom(self->composer_box, 12);
    gtk_box_append(GTK_BOX(main_box), self->composer_box);

    /* Reply indicator (shows when replying to specific comment) */
    self->composer_reply_indicator = gtk_label_new("");
    gtk_widget_add_css_class(self->composer_reply_indicator, "dim-label");
    gtk_widget_set_halign(self->composer_reply_indicator, GTK_ALIGN_START);
    gtk_widget_set_visible(self->composer_reply_indicator, FALSE);
    gtk_box_append(GTK_BOX(self->composer_box), self->composer_reply_indicator);

    /* Composer row */
    GtkWidget *composer_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(self->composer_box), composer_row);

    /* Text view with scroll */
    GtkWidget *text_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(text_scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(text_scroll), 100);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(text_scroll), TRUE);
    gtk_widget_set_hexpand(text_scroll, TRUE);
    gtk_widget_add_css_class(text_scroll, "card");

    self->composer_text = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(self->composer_text), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(self->composer_text), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(self->composer_text), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(self->composer_text), 8);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(self->composer_text), 8);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(text_scroll), self->composer_text);
    gtk_box_append(GTK_BOX(composer_row), text_scroll);

    /* Submit button */
    self->btn_submit_reply = gtk_button_new_from_icon_name("mail-send-symbolic");
    gtk_widget_add_css_class(self->btn_submit_reply, "suggested-action");
    gtk_widget_set_tooltip_text(self->btn_submit_reply, _("Submit reply"));
    gtk_widget_set_valign(self->btn_submit_reply, GTK_ALIGN_END);
    g_signal_connect(self->btn_submit_reply, "clicked", G_CALLBACK(on_submit_reply_clicked), self);
    gtk_box_append(GTK_BOX(composer_row), self->btn_submit_reply);
}

/* ============================================================================
 * Reply Row Creation
 * ============================================================================ */

static GtkWidget *
create_reply_row(GnostrNip7dThreadView *self, GnostrThreadReply *reply)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(row, "thread-reply");

    /* Apply indentation based on depth */
    guint indent = MIN(reply->depth, MAX_DISPLAY_DEPTH) * INDENT_PER_LEVEL;
    gtk_widget_set_margin_start(row, indent);
    gtk_widget_set_margin_top(row, 8);
    gtk_widget_set_margin_bottom(row, 8);

    /* Store reply data */
    ReplyRowData *data = g_new0(ReplyRowData, 1);
    data->event_id = g_strdup(reply->event_id);
    data->pubkey_hex = g_strdup(reply->pubkey);
    data->content = g_strdup(reply->content);
    data->parent_id = g_strdup(reply->parent_id);
    data->created_at = reply->created_at;
    data->depth = reply->depth;
    g_object_set_data_full(G_OBJECT(row), "reply-data", data, (GDestroyNotify)reply_row_data_free);

    /* Header row: avatar, name, timestamp, collapse/reply buttons */
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(row), header);

    /* Avatar */
    GtkWidget *avatar_btn = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(avatar_btn), FALSE);
    gtk_widget_add_css_class(avatar_btn, "circular");
    g_object_set_data_full(G_OBJECT(avatar_btn), "pubkey", g_strdup(reply->pubkey), g_free);
    g_signal_connect(avatar_btn, "clicked", G_CALLBACK(on_author_clicked), self);

    GtkWidget *avatar_overlay = gtk_overlay_new();
    gtk_widget_set_size_request(avatar_overlay, 28, 28);
    gtk_button_set_child(GTK_BUTTON(avatar_btn), avatar_overlay);

    GtkWidget *avatar_image = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(avatar_image), GTK_CONTENT_FIT_COVER);
    gtk_widget_set_size_request(avatar_image, 28, 28);
    gtk_widget_add_css_class(avatar_image, "avatar");
    gtk_widget_set_visible(avatar_image, FALSE);
    gtk_overlay_set_child(GTK_OVERLAY(avatar_overlay), avatar_image);

    GtkWidget *avatar_initials = gtk_label_new("AN");
    gtk_widget_add_css_class(avatar_initials, "avatar-initials");
    gtk_widget_set_halign(avatar_initials, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(avatar_initials, GTK_ALIGN_CENTER);
    gtk_overlay_add_overlay(GTK_OVERLAY(avatar_overlay), avatar_initials);

    /* Store avatar widgets for profile updates */
    g_object_set_data(G_OBJECT(row), "avatar-image", avatar_image);
    g_object_set_data(G_OBJECT(row), "avatar-initials", avatar_initials);

    gtk_box_append(GTK_BOX(header), avatar_btn);

    /* Name button */
    GtkWidget *name_btn = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(name_btn), FALSE);
    GtkWidget *name_label = gtk_label_new(_("Anonymous"));
    gtk_widget_add_css_class(name_label, "author-name");
    gtk_label_set_ellipsize(GTK_LABEL(name_label), PANGO_ELLIPSIZE_END);
    gtk_button_set_child(GTK_BUTTON(name_btn), name_label);
    g_object_set_data_full(G_OBJECT(name_btn), "pubkey", g_strdup(reply->pubkey), g_free);
    g_signal_connect(name_btn, "clicked", G_CALLBACK(on_author_clicked), self);
    g_object_set_data(G_OBJECT(row), "name-label", name_label);
    gtk_box_append(GTK_BOX(header), name_btn);

    /* Timestamp */
    char *ts = gnostr_thread_format_timestamp(reply->created_at);
    GtkWidget *ts_label = gtk_label_new(ts);
    gtk_widget_add_css_class(ts_label, "dim-label");
    gtk_widget_set_hexpand(ts_label, TRUE);
    gtk_widget_set_halign(ts_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(header), ts_label);
    g_free(ts);

    /* Collapse button */
    GtkWidget *collapse_btn = gtk_button_new_from_icon_name("pan-down-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(collapse_btn), FALSE);
    gtk_widget_set_tooltip_text(collapse_btn, _("Collapse"));
    g_object_set_data_full(G_OBJECT(collapse_btn), "reply_id", g_strdup(reply->event_id), g_free);
    g_signal_connect(collapse_btn, "clicked", G_CALLBACK(on_collapse_clicked), self);
    gtk_box_append(GTK_BOX(header), collapse_btn);

    /* Reply button */
    GtkWidget *reply_btn = gtk_button_new_from_icon_name("mail-reply-sender-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(reply_btn), FALSE);
    gtk_widget_set_tooltip_text(reply_btn, _("Reply"));
    gtk_widget_set_sensitive(reply_btn, self->is_logged_in);
    g_object_set_data_full(G_OBJECT(reply_btn), "reply_id", g_strdup(reply->event_id), g_free);
    g_signal_connect(reply_btn, "clicked", G_CALLBACK(on_reply_button_clicked), self);
    g_object_set_data(G_OBJECT(row), "reply-btn", reply_btn);
    gtk_box_append(GTK_BOX(header), reply_btn);

    /* Content */
    GtkWidget *content_label = gtk_label_new(reply->content);
    gtk_label_set_wrap(GTK_LABEL(content_label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(content_label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(content_label), TRUE);
    gtk_widget_set_halign(content_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(row), content_label);

    /* Load and apply profile immediately if available */
    if (reply->pubkey && *reply->pubkey) {
        if (!g_hash_table_contains(self->profiles_requested, reply->pubkey)) {
            g_hash_table_insert(self->profiles_requested, g_strdup(reply->pubkey), GINT_TO_POINTER(1));
        }
        GnostrProfileMeta *meta = gnostr_profile_provider_get(reply->pubkey);
        if (meta) {
            apply_profile_to_reply_row(self, row, meta);
            gnostr_profile_meta_free(meta);
        } else {
            /* Request fetch from relays for missing profiles */
            g_signal_emit(self, signals[SIGNAL_NEED_PROFILE], 0, reply->pubkey);
        }
    }

    return row;
}

/* ============================================================================
 * Profile Display Helpers
 * ============================================================================ */

static void
apply_profile_to_thread_author(GnostrNip7dThreadView *self, GnostrProfileMeta *meta)
{
    if (!meta) return;

    if (GTK_IS_LABEL(self->thread_author_name)) {
        const char *name = (meta->display_name && *meta->display_name) ?
            meta->display_name : (meta->name ? meta->name : _("Anonymous"));
        gtk_label_set_text(GTK_LABEL(self->thread_author_name), name);
    }

    set_avatar_initials(self->thread_author_initials,
        meta->display_name, meta->name);

#ifdef HAVE_SOUP3
    if (meta->picture && *meta->picture && GTK_IS_PICTURE(self->thread_author_avatar)) {
        GdkTexture *cached = gnostr_avatar_try_load_cached(meta->picture);
        if (cached) {
            gtk_picture_set_paintable(GTK_PICTURE(self->thread_author_avatar),
                GDK_PAINTABLE(cached));
            gtk_widget_set_visible(self->thread_author_avatar, TRUE);
            gtk_widget_set_visible(self->thread_author_initials, FALSE);
            g_object_unref(cached);
        } else {
            gnostr_avatar_download_async(meta->picture,
                self->thread_author_avatar, self->thread_author_initials);
        }
    }
#endif
}

static void
apply_profile_to_reply_row(GnostrNip7dThreadView *self, GtkWidget *row, GnostrProfileMeta *meta)
{
    (void)self; /* unused but kept for API consistency */
    if (!row || !meta) return;

    GtkWidget *name_label = g_object_get_data(G_OBJECT(row), "name-label");
    if (GTK_IS_LABEL(name_label)) {
        const char *name = (meta->display_name && *meta->display_name) ?
            meta->display_name : (meta->name ? meta->name : _("Anonymous"));
        gtk_label_set_text(GTK_LABEL(name_label), name);
    }

    GtkWidget *avatar_initials = g_object_get_data(G_OBJECT(row), "avatar-initials");
    set_avatar_initials(avatar_initials, meta->display_name, meta->name);

#ifdef HAVE_SOUP3
    GtkWidget *avatar_image = g_object_get_data(G_OBJECT(row), "avatar-image");
    if (meta->picture && *meta->picture && GTK_IS_PICTURE(avatar_image)) {
        GdkTexture *cached = gnostr_avatar_try_load_cached(meta->picture);
        if (cached) {
            gtk_picture_set_paintable(GTK_PICTURE(avatar_image),
                GDK_PAINTABLE(cached));
            gtk_widget_set_visible(avatar_image, TRUE);
            if (GTK_IS_WIDGET(avatar_initials)) {
                gtk_widget_set_visible(avatar_initials, FALSE);
            }
            g_object_unref(cached);
        } else {
            gnostr_avatar_download_async(meta->picture, avatar_image, avatar_initials);
        }
    }
#endif
}

/* ============================================================================
 * UI Rebuilding
 * ============================================================================ */

static void
rebuild_replies_ui(GnostrNip7dThreadView *self)
{
    if (!self->replies_box) return;

    /* Clear existing reply widgets */
    GtkWidget *child = gtk_widget_get_first_child(self->replies_box);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(self->replies_box), child);
        child = next;
    }

    g_hash_table_remove_all(self->reply_widgets);

    if (!self->replies || self->replies->len == 0) {
        gtk_widget_set_visible(self->replies_separator, FALSE);
        gtk_widget_set_visible(self->lbl_replies_header, FALSE);
        return;
    }

    gtk_widget_set_visible(self->replies_separator, TRUE);
    gtk_widget_set_visible(self->lbl_replies_header, TRUE);

    /* Update header with count */
    char *header = gnostr_thread_format_reply_count(self->replies->len);
    gtk_label_set_text(GTK_LABEL(self->lbl_replies_header), header);
    g_free(header);

    /* Sort replies in threaded order */
    if (self->thread && self->thread->event_id) {
        gnostr_thread_calculate_depths(self->replies, self->thread->event_id);
        gnostr_thread_sort_replies_threaded(self->replies, self->thread->event_id);
    }

    /* Create reply rows */
    for (guint i = 0; i < self->replies->len; i++) {
        GnostrThreadReply *reply = g_ptr_array_index(self->replies, i);
        if (!reply) continue;

        /* Check if collapsed */
        gboolean is_collapsed = GPOINTER_TO_INT(
            g_hash_table_lookup(self->collapsed_replies, reply->parent_id));

        if (is_collapsed) continue;

        GtkWidget *row = create_reply_row(self, reply);
        gtk_box_append(GTK_BOX(self->replies_box), row);
        g_hash_table_insert(self->reply_widgets, g_strdup(reply->event_id), row);
    }
}

static void
set_loading_state(GnostrNip7dThreadView *self, gboolean loading)
{
    self->is_loading = loading;

    if (self->loading_box) {
        gtk_widget_set_visible(self->loading_box, loading);
    }
    if (self->loading_spinner) {
        if (loading) {
            gtk_spinner_start(GTK_SPINNER(self->loading_spinner));
        } else {
            gtk_spinner_stop(GTK_SPINNER(self->loading_spinner));
        }
    }
    if (self->thread_root_box) {
        gtk_widget_set_visible(self->thread_root_box, !loading);
    }
    if (self->empty_box) {
        gtk_widget_set_visible(self->empty_box, FALSE);
    }
}

/* ============================================================================
 * Relay Fetching
 * ============================================================================ */

/* Static counter for unique stash keys on shared pool */
static gint _nip7d_qf_counter = 0;

static void
on_replies_fetch_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GnostrNip7dThreadView *self = GNOSTR_NIP7D_THREAD_VIEW(user_data);

    if (!GNOSTR_IS_NIP7D_THREAD_VIEW(self)) return;

    GError *error = NULL;
    GPtrArray *results = gnostr_pool_query_finish(GNOSTR_POOL(source), res, &error);

    set_loading_state(self, FALSE);

    if (error) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_warning("[NIP7D] Replies fetch failed: %s", error->message);
        }
        g_error_free(error);
        return;
    }

    gboolean found_new = FALSE;

    if (results && results->len > 0) {
        g_debug("[NIP7D] Received %u reply events", results->len);

        /* Defer NDB ingestion to background (nostrc-mzab) */
        GPtrArray *to_ingest = g_ptr_array_new_with_free_func(g_free);
        for (guint i = 0; i < results->len; i++) {
            const char *json = g_ptr_array_index(results, i);
            if (!json) continue;

            g_ptr_array_add(to_ingest, g_strdup(json));

            /* Parse reply */
            GnostrThreadReply *reply = gnostr_thread_reply_parse_from_json(json);
            if (reply) {
                /* Check if we already have this reply */
                gboolean exists = FALSE;
                for (guint j = 0; j < self->replies->len; j++) {
                    GnostrThreadReply *existing = g_ptr_array_index(self->replies, j);
                    if (existing && g_strcmp0(existing->event_id, reply->event_id) == 0) {
                        exists = TRUE;
                        break;
                    }
                }
                if (!exists) {
                    g_ptr_array_add(self->replies, reply);
                    found_new = TRUE;
                } else {
                    gnostr_thread_reply_free(reply);
                }
            }
        }

        storage_ndb_ingest_events_async(to_ingest); /* takes ownership */
        rebuild_replies_ui(self);
    }

    if (results) g_ptr_array_unref(results);

    /* nostrc-46g: After fetching replies, check if any reference missing parent events
     * and fetch them to complete the thread chain */
    if (found_new) {
        fetch_missing_ancestors(self);
    }
}

static void
on_thread_fetch_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GnostrNip7dThreadView *self = GNOSTR_NIP7D_THREAD_VIEW(user_data);

    if (!GNOSTR_IS_NIP7D_THREAD_VIEW(self)) return;

    GError *error = NULL;
    GPtrArray *results = gnostr_pool_query_finish(GNOSTR_POOL(source), res, &error);

    if (error) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_warning("[NIP7D] Thread fetch failed: %s", error->message);
            set_loading_state(self, FALSE);
            gtk_widget_set_visible(self->empty_box, TRUE);
        }
        g_error_free(error);
        return;
    }

    if (results && results->len > 0) {
        const char *json = g_ptr_array_index(results, 0);
        if (json) {
            /* Ingest into nostrdb */
            storage_ndb_ingest_event_json(json, NULL);

            /* Parse thread */
            GnostrThread *thread = gnostr_thread_parse_from_json(json);
            if (thread) {
                gnostr_nip7d_thread_view_set_thread(self, thread);
                gnostr_thread_free(thread);
            }
        }
    } else {
        set_loading_state(self, FALSE);
        gtk_widget_set_visible(self->empty_box, TRUE);
    }

    if (results) g_ptr_array_unref(results);
}

static void
fetch_thread_from_relays(GnostrNip7dThreadView *self)
{
    if (!self->thread || !self->thread->event_id) return;

    /* Cancel previous fetch */
    if (self->fetch_cancellable) {
        g_cancellable_cancel(self->fetch_cancellable);
        g_clear_object(&self->fetch_cancellable);
    }
    self->fetch_cancellable = g_cancellable_new();

    /* Get relay URLs */
    GPtrArray *relay_arr = gnostr_get_read_relay_urls();
    const char **urls = g_new0(const char*, relay_arr->len);
    for (guint i = 0; i < relay_arr->len; i++) {
        urls[i] = g_ptr_array_index(relay_arr, i);
    }

    /* Query for kind 1111 replies referencing this thread */
    {
      GNostrPool *_pool = gnostr_get_shared_query_pool();
      gnostr_pool_sync_relays(_pool, (const gchar **)urls, relay_arr->len);

      GNostrFilter *gf = gnostr_filter_new();
      gint kinds[1] = { NIP7D_KIND_THREAD_REPLY };
      gnostr_filter_set_kinds(gf, kinds, 1);
      /* NIP-22: Use uppercase E tag for root event reference */
      gnostr_filter_tags_append(gf, "E", self->thread->event_id);
      gnostr_filter_set_limit(gf, DEFAULT_REPLY_LIMIT);
      NostrFilter *filter = gnostr_filter_build(gf);
      g_object_unref(gf);

      NostrFilters *_qf = nostr_filters_new();
      nostr_filters_add(_qf, filter);
      int _qfid = g_atomic_int_add(&_nip7d_qf_counter, 1);
      char _qfk[32]; g_snprintf(_qfk, sizeof(_qfk), "qf-%d", _qfid);
      g_object_set_data_full(G_OBJECT(_pool), _qfk, _qf, (GDestroyNotify)nostr_filters_free);
      gnostr_pool_query_async(_pool, _qf, self->fetch_cancellable, on_replies_fetch_done, self);
      nostr_filter_free(filter);
    }

    /* Also query with lowercase e tag for compatibility */
    {
      GNostrPool *_pool2 = gnostr_get_shared_query_pool();

      GNostrFilter *gf = gnostr_filter_new();
      gint kinds[1] = { NIP7D_KIND_THREAD_REPLY };
      gnostr_filter_set_kinds(gf, kinds, 1);
      gnostr_filter_tags_append(gf, "e", self->thread->event_id);
      gnostr_filter_set_limit(gf, DEFAULT_REPLY_LIMIT);
      NostrFilter *filter2 = gnostr_filter_build(gf);
      g_object_unref(gf);

      NostrFilters *_qf2 = nostr_filters_new();
      nostr_filters_add(_qf2, filter2);
      int _qfid2 = g_atomic_int_add(&_nip7d_qf_counter, 1);
      char _qfk2[32]; g_snprintf(_qfk2, sizeof(_qfk2), "qf-%d", _qfid2);
      g_object_set_data_full(G_OBJECT(_pool2), _qfk2, _qf2, (GDestroyNotify)nostr_filters_free);
      gnostr_pool_query_async(_pool2, _qf2, self->fetch_cancellable, on_replies_fetch_done, self);
      nostr_filter_free(filter2);
    }
    g_free(urls);
    g_ptr_array_unref(relay_arr);
}

/* nostrc-46g: Maximum depth for ancestor chain traversal to prevent infinite loops */
#define MAX_ANCESTOR_FETCH_DEPTH 50

/* nostrc-46g: Callback for missing ancestor fetch completion */
static void
on_missing_ancestors_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GnostrNip7dThreadView *self = GNOSTR_NIP7D_THREAD_VIEW(user_data);

    if (!GNOSTR_IS_NIP7D_THREAD_VIEW(self)) return;

    GError *error = NULL;
    GPtrArray *results = gnostr_pool_query_finish(GNOSTR_POOL(source), res, &error);

    if (error) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_debug("[NIP7D] Missing ancestors fetch failed: %s", error->message);
        }
        g_error_free(error);
        /* Try to continue chain traversal with what we have */
        fetch_missing_ancestors(self);
        return;
    }

    gboolean found_new_events = FALSE;

    if (results && results->len > 0) {
        g_debug("[NIP7D] Fetched %u missing ancestor events", results->len);

        /* Defer NDB ingestion to background (nostrc-mzab) */
        GPtrArray *to_ingest = g_ptr_array_new_with_free_func(g_free);
        for (guint i = 0; i < results->len; i++) {
            const char *json = g_ptr_array_index(results, i);
            if (!json) continue;

            g_ptr_array_add(to_ingest, g_strdup(json));

            /* Parse reply and add to collection */
            GnostrThreadReply *reply = gnostr_thread_reply_parse_from_json(json);
            if (reply) {
                /* Check if we already have this reply */
                gboolean exists = FALSE;
                for (guint j = 0; j < self->replies->len; j++) {
                    GnostrThreadReply *existing = g_ptr_array_index(self->replies, j);
                    if (existing && g_strcmp0(existing->event_id, reply->event_id) == 0) {
                        exists = TRUE;
                        break;
                    }
                }
                if (!exists) {
                    g_ptr_array_add(self->replies, reply);
                    found_new_events = TRUE;
                } else {
                    gnostr_thread_reply_free(reply);
                }
            }
        }

        storage_ndb_ingest_events_async(to_ingest); /* takes ownership */

        /* Rebuild UI with new events */
        rebuild_replies_ui(self);
    }

    if (results) g_ptr_array_unref(results);

    /* Continue chain traversal if we found new events */
    if (found_new_events) {
        fetch_missing_ancestors(self);
    } else {
        g_debug("[NIP7D] No new ancestor events found, chain traversal complete");
    }
}

/* nostrc-46g: Fetch any missing parent events referenced by loaded replies.
 * This ensures the full thread chain is loaded from root to current event. */
static void
fetch_missing_ancestors(GnostrNip7dThreadView *self)
{
    if (!self || !self->replies || self->replies->len == 0) return;

    /* Check depth limit to prevent infinite traversal */
    if (self->ancestor_fetch_depth >= MAX_ANCESTOR_FETCH_DEPTH) {
        g_debug("[NIP7D] Reached max ancestor fetch depth (%d), stopping chain traversal",
                MAX_ANCESTOR_FETCH_DEPTH);
        return;
    }

    /* Build a set of known event IDs */
    GHashTable *known_ids = g_hash_table_new(g_str_hash, g_str_equal);

    /* Add thread root */
    if (self->thread && self->thread->event_id) {
        g_hash_table_insert(known_ids, (gpointer)self->thread->event_id, GINT_TO_POINTER(1));
    }

    /* Add all reply event IDs */
    for (guint i = 0; i < self->replies->len; i++) {
        GnostrThreadReply *reply = g_ptr_array_index(self->replies, i);
        if (reply && reply->event_id) {
            g_hash_table_insert(known_ids, (gpointer)reply->event_id, GINT_TO_POINTER(1));
        }
    }

    /* Collect missing parent IDs */
    GPtrArray *missing_ids = g_ptr_array_new_with_free_func(g_free);

    for (guint i = 0; i < self->replies->len; i++) {
        GnostrThreadReply *reply = g_ptr_array_index(self->replies, i);
        if (!reply) continue;

        /* Check parent_id - need to fetch if not known and not already attempted */
        if (reply->parent_id && strlen(reply->parent_id) == 64 &&
            !g_hash_table_contains(known_ids, reply->parent_id) &&
            !g_hash_table_contains(self->ancestors_fetched, reply->parent_id)) {
            /* Check if already in missing list */
            gboolean found = FALSE;
            for (guint j = 0; j < missing_ids->len; j++) {
                if (g_strcmp0(g_ptr_array_index(missing_ids, j), reply->parent_id) == 0) {
                    found = TRUE;
                    break;
                }
            }
            if (!found) {
                g_ptr_array_add(missing_ids, g_strdup(reply->parent_id));
                g_hash_table_insert(self->ancestors_fetched, g_strdup(reply->parent_id), GINT_TO_POINTER(1));
            }
        }

        /* Check thread_root_id - need to fetch if not known and not already attempted */
        if (reply->thread_root_id && strlen(reply->thread_root_id) == 64 &&
            !g_hash_table_contains(known_ids, reply->thread_root_id) &&
            !g_hash_table_contains(self->ancestors_fetched, reply->thread_root_id)) {
            gboolean found = FALSE;
            for (guint j = 0; j < missing_ids->len; j++) {
                if (g_strcmp0(g_ptr_array_index(missing_ids, j), reply->thread_root_id) == 0) {
                    found = TRUE;
                    break;
                }
            }
            if (!found) {
                g_ptr_array_add(missing_ids, g_strdup(reply->thread_root_id));
                g_hash_table_insert(self->ancestors_fetched, g_strdup(reply->thread_root_id), GINT_TO_POINTER(1));
            }
        }
    }

    g_hash_table_unref(known_ids);

    if (missing_ids->len == 0) {
        g_ptr_array_unref(missing_ids);
        g_debug("[NIP7D] No more missing ancestors to fetch, chain complete");
        return;
    }

    /* Increment depth counter */
    self->ancestor_fetch_depth++;
    g_debug("[NIP7D] Fetching %u missing ancestor events (depth %u)",
            missing_ids->len, self->ancestor_fetch_depth);

    /* Get relay URLs */
    GPtrArray *relay_arr = gnostr_get_read_relay_urls();
    const char **urls = g_new0(const char*, relay_arr->len);
    for (guint i = 0; i < relay_arr->len; i++) {
        urls[i] = g_ptr_array_index(relay_arr, i);
    }

    /* Build filter with missing IDs - include both kind 11 (thread root) and 1111 (replies) */
    GNostrFilter *gf = gnostr_filter_new();
    gint kinds[2] = { NIP7D_KIND_THREAD_ROOT, NIP7D_KIND_THREAD_REPLY };
    gnostr_filter_set_kinds(gf, kinds, 2);

    for (guint i = 0; i < missing_ids->len; i++) {
        gnostr_filter_add_id(gf, g_ptr_array_index(missing_ids, i));
    }
    gnostr_filter_set_limit(gf, DEFAULT_REPLY_LIMIT);
    NostrFilter *filter = gnostr_filter_build(gf);
    g_object_unref(gf);

    g_ptr_array_unref(missing_ids);

    /* Reuse existing cancellable */
    if (!self->fetch_cancellable) {
        self->fetch_cancellable = g_cancellable_new();
    }

    {
      GNostrPool *_pool = gnostr_get_shared_query_pool();
      gnostr_pool_sync_relays(_pool, (const gchar **)urls, relay_arr->len);
      NostrFilters *_qf = nostr_filters_new();
      nostr_filters_add(_qf, filter);
      int _qfid = g_atomic_int_add(&_nip7d_qf_counter, 1);
      char _qfk[32]; g_snprintf(_qfk, sizeof(_qfk), "qf-%d", _qfid);
      g_object_set_data_full(G_OBJECT(_pool), _qfk, _qf, (GDestroyNotify)nostr_filters_free);
      gnostr_pool_query_async(_pool, _qf, self->fetch_cancellable, on_missing_ancestors_done, self);
    }

    nostr_filter_free(filter);
    g_free(urls);
    g_ptr_array_unref(relay_arr);
}

/* ============================================================================
 * GObject Class Init
 * ============================================================================ */

static void
gnostr_nip7d_thread_view_class_init(GnostrNip7dThreadViewClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    GObjectClass *obj_class = G_OBJECT_CLASS(klass);

    obj_class->dispose = gnostr_nip7d_thread_view_dispose;
    obj_class->finalize = gnostr_nip7d_thread_view_finalize;

    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);

    /* Signals */
    signals[SIGNAL_CLOSE_REQUESTED] = g_signal_new("close-requested",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);

    signals[SIGNAL_AUTHOR_CLICKED] = g_signal_new("author-clicked",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_REPLY_SUBMITTED] = g_signal_new("reply-submitted",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);

    signals[SIGNAL_HASHTAG_CLICKED] = g_signal_new("hashtag-clicked",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_NEED_PROFILE] = g_signal_new("need-profile",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gnostr_nip7d_thread_view_init(GnostrNip7dThreadView *self)
{
    gtk_widget_add_css_class(GTK_WIDGET(self), "gnostr-nip7d-thread-view");

    self->replies = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_thread_reply_free);
    self->reply_widgets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    self->collapsed_replies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    self->profiles_requested = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    /* nostrc-46g: Track fetched ancestors to prevent duplicate requests and enable chain traversal */
    self->ancestors_fetched = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    self->ancestor_fetch_depth = 0;
    /* Uses shared query pool from gnostr_get_shared_query_pool() */

#ifdef HAVE_SOUP3
    /* Use shared session to reduce memory overhead */
    self->session = NULL; /* Will use gnostr_get_shared_soup_session() */
#endif

    setup_view_ui(self);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

GtkWidget *
gnostr_nip7d_thread_view_new(void)
{
    return g_object_new(GNOSTR_TYPE_NIP7D_THREAD_VIEW, NULL);
}

void
gnostr_nip7d_thread_view_set_thread(GnostrNip7dThreadView *self,
                                     GnostrThread *thread)
{
    g_return_if_fail(GNOSTR_IS_NIP7D_THREAD_VIEW(self));

    /* Clear existing */
    if (self->thread) {
        gnostr_thread_free(self->thread);
    }
    self->thread = thread ? gnostr_thread_copy(thread) : NULL;

    if (!thread) {
        gtk_widget_set_visible(self->thread_root_box, FALSE);
        return;
    }

    /* Update title */
    if (GTK_IS_LABEL(self->lbl_title)) {
        gtk_label_set_text(GTK_LABEL(self->lbl_title),
            thread->subject ? thread->subject : _("Thread"));
    }

    /* Update subject */
    if (GTK_IS_LABEL(self->thread_subject)) {
        gtk_label_set_text(GTK_LABEL(self->thread_subject),
            thread->subject ? thread->subject : _("Untitled Thread"));
    }

    /* Update content */
    if (GTK_IS_LABEL(self->thread_content)) {
        gtk_label_set_text(GTK_LABEL(self->thread_content),
            thread->content ? thread->content : "");
    }

    /* Update timestamp */
    if (GTK_IS_LABEL(self->thread_timestamp)) {
        char *ts = gnostr_thread_format_timestamp(thread->created_at);
        gtk_label_set_text(GTK_LABEL(self->thread_timestamp), ts);
        g_free(ts);
    }

    /* Store pubkey for author button */
    GtkWidget *author_btn = gtk_widget_get_parent(self->thread_author_name);
    if (GTK_IS_BUTTON(author_btn)) {
        g_object_set_data_full(G_OBJECT(author_btn), "pubkey",
            g_strdup(thread->pubkey), g_free);
    }

    /* Load and apply author profile immediately if available */
    if (thread->pubkey && *thread->pubkey) {
        if (!g_hash_table_contains(self->profiles_requested, thread->pubkey)) {
            g_hash_table_insert(self->profiles_requested, g_strdup(thread->pubkey), GINT_TO_POINTER(1));
        }
        GnostrProfileMeta *meta = gnostr_profile_provider_get(thread->pubkey);
        if (meta) {
            apply_profile_to_thread_author(self, meta);
            gnostr_profile_meta_free(meta);
        } else {
            /* Request fetch from relays for missing profiles */
            g_signal_emit(self, signals[SIGNAL_NEED_PROFILE], 0, thread->pubkey);
        }
    }

    /* Update hashtags */
    /* Clear existing hashtags */
    GtkWidget *child = gtk_widget_get_first_child(self->thread_hashtags_box);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_flow_box_remove(GTK_FLOW_BOX(self->thread_hashtags_box), child);
        child = next;
    }

    if (thread->hashtags && thread->hashtags->len > 0) {
        for (guint i = 0; i < thread->hashtags->len; i++) {
            const char *tag = g_ptr_array_index(thread->hashtags, i);
            GtkWidget *pill = create_hashtag_pill(self, tag);
            gtk_flow_box_append(GTK_FLOW_BOX(self->thread_hashtags_box), pill);
        }
        gtk_widget_set_visible(self->thread_hashtags_box, TRUE);
    } else {
        gtk_widget_set_visible(self->thread_hashtags_box, FALSE);
    }

    gtk_widget_set_visible(self->thread_root_box, TRUE);
    set_loading_state(self, FALSE);

    /* Fetch replies */
    fetch_thread_from_relays(self);
}

void
gnostr_nip7d_thread_view_load_thread(GnostrNip7dThreadView *self,
                                      const char *event_id_hex)
{
    g_return_if_fail(GNOSTR_IS_NIP7D_THREAD_VIEW(self));
    g_return_if_fail(event_id_hex != NULL && strlen(event_id_hex) == 64);

    gnostr_nip7d_thread_view_clear(self);

    /* nostrc-46g: Reset ancestor tracking for new thread load */
    if (self->ancestors_fetched) {
        g_hash_table_remove_all(self->ancestors_fetched);
    }
    self->ancestor_fetch_depth = 0;

    set_loading_state(self, TRUE);

    /* Try to load from nostrdb first */
    void *txn = NULL;
    if (storage_ndb_begin_query(&txn) == 0 && txn) {
        unsigned char id32[32];
        gboolean valid_id = TRUE;

        /* Convert hex to bytes */
        for (int i = 0; i < 32; i++) {
            unsigned int byte;
            if (sscanf(event_id_hex + i*2, "%2x", &byte) != 1) {
                valid_id = FALSE;
                break;
            }
            id32[i] = (unsigned char)byte;
        }

        if (valid_id) {
            char *json = NULL;
            int len = 0;
            if (storage_ndb_get_note_by_id(txn, id32, &json, &len) == 0 && json) {
                GnostrThread *thread = gnostr_thread_parse_from_json(json);
                if (thread) {
                    gnostr_nip7d_thread_view_set_thread(self, thread);
                    gnostr_thread_free(thread);
                    storage_ndb_end_query(txn);
                    return;
                }
            }
        }
        storage_ndb_end_query(txn);
    }

    /* Not in local DB - fetch from relays */
    if (self->fetch_cancellable) {
        g_cancellable_cancel(self->fetch_cancellable);
        g_clear_object(&self->fetch_cancellable);
    }
    self->fetch_cancellable = g_cancellable_new();

    GPtrArray *relay_arr = gnostr_get_read_relay_urls();
    const char **urls = g_new0(const char*, relay_arr->len);
    for (guint i = 0; i < relay_arr->len; i++) {
        urls[i] = g_ptr_array_index(relay_arr, i);
    }

    {
      GNostrFilter *gf = gnostr_filter_new();
      gint kinds[1] = { NIP7D_KIND_THREAD_ROOT };
      gnostr_filter_set_kinds(gf, kinds, 1);
      gnostr_filter_add_id(gf, event_id_hex);
      gnostr_filter_set_limit(gf, 1);
      NostrFilter *filter = gnostr_filter_build(gf);
      g_object_unref(gf);

      GNostrPool *_pool = gnostr_get_shared_query_pool();
      gnostr_pool_sync_relays(_pool, (const gchar **)urls, relay_arr->len);
      NostrFilters *_qf = nostr_filters_new();
      nostr_filters_add(_qf, filter);
      int _qfid = g_atomic_int_add(&_nip7d_qf_counter, 1);
      char _qfk[32]; g_snprintf(_qfk, sizeof(_qfk), "qf-%d", _qfid);
      g_object_set_data_full(G_OBJECT(_pool), _qfk, _qf, (GDestroyNotify)nostr_filters_free);
      gnostr_pool_query_async(_pool, _qf, self->fetch_cancellable, on_thread_fetch_done, self);

      nostr_filter_free(filter);
    }
    g_free(urls);
    g_ptr_array_unref(relay_arr);
}

void
gnostr_nip7d_thread_view_add_reply(GnostrNip7dThreadView *self,
                                    GnostrThreadReply *reply)
{
    g_return_if_fail(GNOSTR_IS_NIP7D_THREAD_VIEW(self));
    g_return_if_fail(reply != NULL);

    g_ptr_array_add(self->replies, gnostr_thread_reply_copy(reply));
    rebuild_replies_ui(self);
}

void
gnostr_nip7d_thread_view_add_replies(GnostrNip7dThreadView *self,
                                      GPtrArray *replies)
{
    g_return_if_fail(GNOSTR_IS_NIP7D_THREAD_VIEW(self));

    if (!replies || replies->len == 0) return;

    for (guint i = 0; i < replies->len; i++) {
        GnostrThreadReply *reply = g_ptr_array_index(replies, i);
        if (reply) {
            g_ptr_array_add(self->replies, gnostr_thread_reply_copy(reply));
        }
    }

    rebuild_replies_ui(self);
}

void
gnostr_nip7d_thread_view_clear(GnostrNip7dThreadView *self)
{
    g_return_if_fail(GNOSTR_IS_NIP7D_THREAD_VIEW(self));

    if (self->fetch_cancellable) {
        g_cancellable_cancel(self->fetch_cancellable);
        g_clear_object(&self->fetch_cancellable);
    }

    if (self->thread) {
        gnostr_thread_free(self->thread);
        self->thread = NULL;
    }

    g_ptr_array_set_size(self->replies, 0);
    g_hash_table_remove_all(self->reply_widgets);
    g_hash_table_remove_all(self->collapsed_replies);
    g_hash_table_remove_all(self->profiles_requested);
    /* nostrc-46g: Clear ancestor tracking on view clear */
    g_hash_table_remove_all(self->ancestors_fetched);
    self->ancestor_fetch_depth = 0;

    g_clear_pointer(&self->reply_parent_id, g_free);

    /* Clear UI */
    GtkWidget *child = gtk_widget_get_first_child(self->replies_box);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(self->replies_box), child);
        child = next;
    }

    set_loading_state(self, FALSE);
    gtk_widget_set_visible(self->thread_root_box, FALSE);
    gtk_widget_set_visible(self->empty_box, FALSE);
}

void
gnostr_nip7d_thread_view_refresh(GnostrNip7dThreadView *self)
{
    g_return_if_fail(GNOSTR_IS_NIP7D_THREAD_VIEW(self));

    if (!self->thread || !self->thread->event_id) return;

    char *event_id = g_strdup(self->thread->event_id);
    gnostr_nip7d_thread_view_load_thread(self, event_id);
    g_free(event_id);
}

const char *
gnostr_nip7d_thread_view_get_thread_id(GnostrNip7dThreadView *self)
{
    g_return_val_if_fail(GNOSTR_IS_NIP7D_THREAD_VIEW(self), NULL);
    return self->thread ? self->thread->event_id : NULL;
}

guint
gnostr_nip7d_thread_view_get_reply_count(GnostrNip7dThreadView *self)
{
    g_return_val_if_fail(GNOSTR_IS_NIP7D_THREAD_VIEW(self), 0);
    return self->replies ? self->replies->len : 0;
}

void
gnostr_nip7d_thread_view_set_reply_parent(GnostrNip7dThreadView *self,
                                           const char *parent_id)
{
    g_return_if_fail(GNOSTR_IS_NIP7D_THREAD_VIEW(self));

    g_clear_pointer(&self->reply_parent_id, g_free);
    self->reply_parent_id = g_strdup(parent_id);

    /* Update reply indicator */
    if (GTK_IS_LABEL(self->composer_reply_indicator)) {
        if (parent_id && self->thread && g_strcmp0(parent_id, self->thread->event_id) != 0) {
            gtk_label_set_text(GTK_LABEL(self->composer_reply_indicator),
                _("Replying to comment..."));
            gtk_widget_set_visible(self->composer_reply_indicator, TRUE);
        } else {
            gtk_widget_set_visible(self->composer_reply_indicator, FALSE);
        }
    }
}

void
gnostr_nip7d_thread_view_set_logged_in(GnostrNip7dThreadView *self,
                                        gboolean logged_in)
{
    g_return_if_fail(GNOSTR_IS_NIP7D_THREAD_VIEW(self));

    self->is_logged_in = logged_in;

    /* Update composer sensitivity */
    if (GTK_IS_WIDGET(self->composer_text)) {
        gtk_widget_set_sensitive(self->composer_text, logged_in);
    }
    if (GTK_IS_WIDGET(self->btn_submit_reply)) {
        gtk_widget_set_sensitive(self->btn_submit_reply, logged_in);
    }

    /* Update reply buttons in existing rows */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, self->reply_widgets);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        GtkWidget *row = GTK_WIDGET(value);
        GtkWidget *reply_btn = g_object_get_data(G_OBJECT(row), "reply-btn");
        if (GTK_IS_WIDGET(reply_btn)) {
            gtk_widget_set_sensitive(reply_btn, logged_in);
        }
    }
}

void
gnostr_nip7d_thread_view_update_profiles(GnostrNip7dThreadView *self)
{
    g_return_if_fail(GNOSTR_IS_NIP7D_THREAD_VIEW(self));

    /* Update thread author */
    if (self->thread && self->thread->pubkey) {
        GnostrProfileMeta *meta = gnostr_profile_provider_get(self->thread->pubkey);
        if (meta) {
            apply_profile_to_thread_author(self, meta);
            gnostr_profile_meta_free(meta);
        }
    }

    /* Update reply rows */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, self->reply_widgets);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        GtkWidget *row = GTK_WIDGET(value);
        ReplyRowData *data = g_object_get_data(G_OBJECT(row), "reply-data");
        if (!data || !data->pubkey_hex) continue;

        GnostrProfileMeta *meta = gnostr_profile_provider_get(data->pubkey_hex);
        if (meta) {
            apply_profile_to_reply_row(self, row, meta);
            gnostr_profile_meta_free(meta);
        }
    }
}

void
gnostr_nip7d_thread_view_collapse_reply(GnostrNip7dThreadView *self,
                                         const char *reply_id,
                                         gboolean collapsed)
{
    g_return_if_fail(GNOSTR_IS_NIP7D_THREAD_VIEW(self));
    g_return_if_fail(reply_id != NULL);

    if (collapsed) {
        g_hash_table_insert(self->collapsed_replies, g_strdup(reply_id), GINT_TO_POINTER(1));
    } else {
        g_hash_table_remove(self->collapsed_replies, reply_id);
    }

    rebuild_replies_ui(self);
}

void
gnostr_nip7d_thread_view_load_more_replies(GnostrNip7dThreadView *self,
                                            guint limit)
{
    g_return_if_fail(GNOSTR_IS_NIP7D_THREAD_VIEW(self));

    if (!self->thread || !self->thread->event_id) return;

    /* Fetch more replies with until timestamp */
    if (self->fetch_cancellable) {
        g_cancellable_cancel(self->fetch_cancellable);
        g_clear_object(&self->fetch_cancellable);
    }
    self->fetch_cancellable = g_cancellable_new();

    GPtrArray *relay_arr = gnostr_get_read_relay_urls();
    const char **urls = g_new0(const char*, relay_arr->len);
    for (guint i = 0; i < relay_arr->len; i++) {
        urls[i] = g_ptr_array_index(relay_arr, i);
    }

    /* Find oldest reply timestamp */
    gint64 oldest = G_MAXINT64;
    for (guint i = 0; i < self->replies->len; i++) {
        GnostrThreadReply *reply = g_ptr_array_index(self->replies, i);
        if (reply && reply->created_at < oldest) {
            oldest = reply->created_at;
        }
    }

    {
      GNostrFilter *gf = gnostr_filter_new();
      gint kinds[1] = { NIP7D_KIND_THREAD_REPLY };
      gnostr_filter_set_kinds(gf, kinds, 1);
      gnostr_filter_tags_append(gf, "E", self->thread->event_id);
      gnostr_filter_set_limit(gf, limit);
      if (oldest < G_MAXINT64) {
          gnostr_filter_set_until(gf, oldest - 1);
      }
      NostrFilter *filter = gnostr_filter_build(gf);
      g_object_unref(gf);

      GNostrPool *_pool = gnostr_get_shared_query_pool();
      gnostr_pool_sync_relays(_pool, (const gchar **)urls, relay_arr->len);
      NostrFilters *_qf = nostr_filters_new();
      nostr_filters_add(_qf, filter);
      int _qfid = g_atomic_int_add(&_nip7d_qf_counter, 1);
      char _qfk[32]; g_snprintf(_qfk, sizeof(_qfk), "qf-%d", _qfid);
      g_object_set_data_full(G_OBJECT(_pool), _qfk, _qf, (GDestroyNotify)nostr_filters_free);
      gnostr_pool_query_async(_pool, _qf, self->fetch_cancellable, on_replies_fetch_done, self);

      nostr_filter_free(filter);
    }
    g_free(urls);
    g_ptr_array_unref(relay_arr);
}

/* nostrc-8w2p: Safe timeout callback to remove highlight CSS class.
 * Takes a g_object_ref'd widget; unrefs after removing class. */
static gboolean
remove_highlight_cb(gpointer data)
{
    GtkWidget *row = GTK_WIDGET(data);
    if (GTK_IS_WIDGET(row))
        gtk_widget_remove_css_class(row, "highlighted");
    g_object_unref(row);
    return G_SOURCE_REMOVE;
}

void
gnostr_nip7d_thread_view_scroll_to_reply(GnostrNip7dThreadView *self,
                                          const char *reply_id)
{
    g_return_if_fail(GNOSTR_IS_NIP7D_THREAD_VIEW(self));

    if (!reply_id) return;

    GtkWidget *row = g_hash_table_lookup(self->reply_widgets, reply_id);
    if (!GTK_IS_WIDGET(row)) return;

    /* Scroll the row into view */
    gtk_widget_grab_focus(row);

    /* LEGITIMATE TIMEOUT - Remove highlight CSS class after animation.
     * nostrc-b0h: Audited - animation timing is appropriate. */
    gtk_widget_add_css_class(row, "highlighted");
    g_timeout_add(2000, remove_highlight_cb, g_object_ref(row));
}
