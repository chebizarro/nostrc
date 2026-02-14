/**
 * GnostrCommunityView - NIP-72 Moderated Community Feed View
 *
 * Displays a community's approved posts with header and moderation actions.
 */

#include "gnostr-community-view.h"
#include "gnostr-community-card.h"
#include "gnostr-avatar-cache.h"
#include <nostr-gtk-1.0/nostr-note-card-row.h>
#include <glib/gi18n.h>
#include <time.h>

/* Post row item for GListStore */
typedef struct {
    GObject parent_instance;
    GnostrCommunityPost *post;
    gboolean is_pending;
    char *author_name;
    char *author_avatar;
} CommunityPostItem;

typedef struct {
    GObjectClass parent_class;
} CommunityPostItemClass;

static GType community_post_item_get_type(void);
G_DEFINE_TYPE(CommunityPostItem, community_post_item, G_TYPE_OBJECT)

/* Property IDs for CommunityPostItem */
enum {
    POST_ITEM_PROP_0,
    POST_ITEM_PROP_AUTHOR_NAME,
    POST_ITEM_PROP_AUTHOR_AVATAR,
    POST_ITEM_N_PROPS
};

static GParamSpec *post_item_props[POST_ITEM_N_PROPS];

static void
community_post_item_get_property(GObject *object,
                                  guint property_id,
                                  GValue *value,
                                  GParamSpec *pspec)
{
    CommunityPostItem *self = (CommunityPostItem *)object;

    switch (property_id) {
    case POST_ITEM_PROP_AUTHOR_NAME:
        g_value_set_string(value, self->author_name);
        break;
    case POST_ITEM_PROP_AUTHOR_AVATAR:
        g_value_set_string(value, self->author_avatar);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void
community_post_item_set_property(GObject *object,
                                  guint property_id,
                                  const GValue *value,
                                  GParamSpec *pspec)
{
    CommunityPostItem *self = (CommunityPostItem *)object;

    switch (property_id) {
    case POST_ITEM_PROP_AUTHOR_NAME:
        g_free(self->author_name);
        self->author_name = g_value_dup_string(value);
        break;
    case POST_ITEM_PROP_AUTHOR_AVATAR:
        g_free(self->author_avatar);
        self->author_avatar = g_value_dup_string(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void
community_post_item_finalize(GObject *object)
{
    CommunityPostItem *self = (CommunityPostItem *)object;
    if (self->post)
        gnostr_community_post_free(self->post);
    g_free(self->author_name);
    g_free(self->author_avatar);
    G_OBJECT_CLASS(community_post_item_parent_class)->finalize(object);
}

static void
community_post_item_class_init(CommunityPostItemClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = community_post_item_finalize;
    object_class->get_property = community_post_item_get_property;
    object_class->set_property = community_post_item_set_property;

    post_item_props[POST_ITEM_PROP_AUTHOR_NAME] =
        g_param_spec_string("author-name", NULL, NULL,
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    post_item_props[POST_ITEM_PROP_AUTHOR_AVATAR] =
        g_param_spec_string("author-avatar", NULL, NULL,
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, POST_ITEM_N_PROPS, post_item_props);
}

static void
community_post_item_init(CommunityPostItem *self)
{
    self->post = NULL;
    self->is_pending = FALSE;
    self->author_name = NULL;
    self->author_avatar = NULL;
}

static CommunityPostItem *
community_post_item_new(const GnostrCommunityPost *post, gboolean is_pending)
{
    CommunityPostItem *item = g_object_new(community_post_item_get_type(), NULL);
    item->post = gnostr_community_post_copy(post);
    item->is_pending = is_pending;
    return item;
}

/* Main view structure */
struct _GnostrCommunityView {
    GtkWidget parent_instance;

    /* Widgets */
    GtkWidget *root_box;
    GtkWidget *scroller;
    GtkWidget *content_box;

    /* Header widgets */
    GtkWidget *header_box;
    GtkWidget *image_frame;
    GtkWidget *community_image;
    GtkWidget *image_initials;
    GtkWidget *info_box;
    GtkWidget *lbl_name;
    GtkWidget *lbl_description;
    GtkWidget *rules_expander;
    GtkWidget *lbl_rules;
    GtkWidget *stats_box;
    GtkWidget *lbl_members;
    GtkWidget *lbl_posts;
    GtkWidget *moderators_box;

    /* Action bar */
    GtkWidget *action_bar;
    GtkWidget *btn_compose;
    GtkWidget *btn_join;
    GtkWidget *btn_pending;

    /* Content stack */
    GtkWidget *content_stack;
    GtkWidget *posts_box;
    GtkWidget *pending_box;
    GtkWidget *empty_box;
    GtkWidget *loading_spinner;

    /* Lists */
    GListStore *approved_posts;
    GListStore *pending_posts;
    GtkWidget *approved_list;
    GtkWidget *pending_list;

    /* Data */
    char *a_tag;
    char *d_tag;
    char *name;
    char *creator_pubkey;
    guint post_count;
    guint member_count;
    GPtrArray *moderators;  /* Array of pubkey strings */

    /* State */
    char *user_pubkey;
    gboolean is_joined;
    gboolean is_moderator;
    gboolean show_pending;
    GHashTable *author_profiles;  /* pubkey -> name */
    GHashTable *author_avatars;   /* pubkey -> avatar_url */
};

G_DEFINE_TYPE(GnostrCommunityView, gnostr_community_view, GTK_TYPE_WIDGET)

enum {
    SIGNAL_OPEN_PROFILE,
    SIGNAL_OPEN_NOTE,
    SIGNAL_COMPOSE_POST,
    SIGNAL_APPROVE_POST,
    SIGNAL_REJECT_POST,
    SIGNAL_JOIN_COMMUNITY,
    SIGNAL_LEAVE_COMMUNITY,
    SIGNAL_ZAP_REQUESTED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
gnostr_community_view_dispose(GObject *object)
{
    GnostrCommunityView *self = GNOSTR_COMMUNITY_VIEW(object);

    g_clear_object(&self->approved_posts);
    g_clear_object(&self->pending_posts);

    if (self->root_box) {
        gtk_widget_unparent(self->root_box);
        self->root_box = NULL;
    }

    G_OBJECT_CLASS(gnostr_community_view_parent_class)->dispose(object);
}

static void
gnostr_community_view_finalize(GObject *object)
{
    GnostrCommunityView *self = GNOSTR_COMMUNITY_VIEW(object);

    g_free(self->a_tag);
    g_free(self->d_tag);
    g_free(self->name);
    g_free(self->creator_pubkey);
    g_free(self->user_pubkey);

    if (self->moderators)
        g_ptr_array_unref(self->moderators);

    g_hash_table_destroy(self->author_profiles);
    g_hash_table_destroy(self->author_avatars);

    G_OBJECT_CLASS(gnostr_community_view_parent_class)->finalize(object);
}

static void
on_compose_clicked(GtkButton *button, GnostrCommunityView *self)
{
    (void)button;
    if (self->a_tag) {
        g_signal_emit(self, signals[SIGNAL_COMPOSE_POST], 0, self->a_tag);
    }
}

static void
on_join_clicked(GtkButton *button, GnostrCommunityView *self)
{
    (void)button;
    if (!self->a_tag) return;

    if (self->is_joined) {
        g_signal_emit(self, signals[SIGNAL_LEAVE_COMMUNITY], 0, self->a_tag);
    } else {
        g_signal_emit(self, signals[SIGNAL_JOIN_COMMUNITY], 0, self->a_tag);
    }
}

static void
on_pending_clicked(GtkButton *button, GnostrCommunityView *self)
{
    (void)button;
    self->show_pending = !self->show_pending;

    if (self->show_pending) {
        gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "pending");
        gtk_button_set_label(GTK_BUTTON(self->btn_pending), _("View Approved"));
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "posts");
        gtk_button_set_label(GTK_BUTTON(self->btn_pending), _("View Pending"));
    }
}

static void
update_join_button(GnostrCommunityView *self)
{
    if (!GTK_IS_BUTTON(self->btn_join)) return;

    if (self->is_joined) {
        gtk_button_set_label(GTK_BUTTON(self->btn_join), _("Leave"));
        gtk_widget_remove_css_class(self->btn_join, "suggested-action");
        gtk_widget_add_css_class(self->btn_join, "destructive-action");
    } else {
        gtk_button_set_label(GTK_BUTTON(self->btn_join), _("Join"));
        gtk_widget_remove_css_class(self->btn_join, "destructive-action");
        gtk_widget_add_css_class(self->btn_join, "suggested-action");
    }

    gboolean logged_in = (self->user_pubkey != NULL);
    gtk_widget_set_sensitive(self->btn_join, logged_in);
}

static void
update_compose_button(GnostrCommunityView *self)
{
    if (!GTK_IS_WIDGET(self->btn_compose)) return;

    gboolean logged_in = (self->user_pubkey != NULL);
    gtk_widget_set_sensitive(self->btn_compose, logged_in && self->is_joined);
}

static void
update_pending_button(GnostrCommunityView *self)
{
    if (!GTK_IS_WIDGET(self->btn_pending)) return;

    gtk_widget_set_visible(self->btn_pending, self->is_moderator);
}

static void
update_stats_display(GnostrCommunityView *self)
{
    if (GTK_IS_LABEL(self->lbl_members)) {
        char *text = g_strdup_printf(g_dngettext(NULL, "%u member", "%u members",
                                                  self->member_count), self->member_count);
        gtk_label_set_text(GTK_LABEL(self->lbl_members), text);
        g_free(text);
    }

    if (GTK_IS_LABEL(self->lbl_posts)) {
        char *text = g_strdup_printf(g_dngettext(NULL, "%u post", "%u posts",
                                                  self->post_count), self->post_count);
        gtk_label_set_text(GTK_LABEL(self->lbl_posts), text);
        g_free(text);
    }
}

static char *
get_initials(const char *name)
{
    if (!name || !*name)
        return g_strdup("C");

    gunichar c1 = g_utf8_get_char(name);
    char buf[14];
    int len = g_unichar_to_utf8(g_unichar_toupper(c1), buf);

    const char *space = strchr(name, ' ');
    if (space && *(space + 1)) {
        gunichar c2 = g_utf8_get_char(space + 1);
        len += g_unichar_to_utf8(g_unichar_toupper(c2), buf + len);
    }

    buf[len] = '\0';
    return g_strdup(buf);
}

static char *
format_relative_time(gint64 timestamp)
{
    gint64 now = (gint64)time(NULL);
    gint64 diff = now - timestamp;

    if (diff < 0)
        return g_strdup(_("just now"));
    if (diff < 60)
        return g_strdup(_("just now"));
    if (diff < 3600)
        return g_strdup_printf("%" G_GINT64_FORMAT "m", diff / 60);
    if (diff < 86400)
        return g_strdup_printf("%" G_GINT64_FORMAT "h", diff / 3600);
    if (diff < 604800)
        return g_strdup_printf("%" G_GINT64_FORMAT "d", diff / 86400);

    time_t t = (time_t)timestamp;
    struct tm *tm = localtime(&t);
    char buf[32];
    strftime(buf, sizeof(buf), "%b %d", tm);
    return g_strdup(buf);
}

/* Signal handlers for note cards */
static void
on_note_open_profile(NostrGtkNoteCardRow *row, const char *pubkey, GnostrCommunityView *self)
{
    (void)row;
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, pubkey);
}

static void
on_note_open_thread(NostrGtkNoteCardRow *row, const char *event_id, GnostrCommunityView *self)
{
    (void)row;
    g_signal_emit(self, signals[SIGNAL_OPEN_NOTE], 0, event_id);
}

static void
on_note_zap_requested(NostrGtkNoteCardRow *row, const char *event_id, const char *pubkey, const char *lud16, GnostrCommunityView *self)
{
    (void)row;
    g_signal_emit(self, signals[SIGNAL_ZAP_REQUESTED], 0, event_id, pubkey, lud16);
}

/* Approve button handler for pending posts */
static void
on_approve_clicked(GtkButton *button, gpointer user_data)
{
    GnostrCommunityView *self = g_object_get_data(G_OBJECT(button), "view");
    const char *event_id = g_object_get_data(G_OBJECT(button), "event_id");
    const char *author = g_object_get_data(G_OBJECT(button), "author");

    if (self && event_id && author && self->a_tag) {
        g_signal_emit(self, signals[SIGNAL_APPROVE_POST], 0, event_id, author, self->a_tag);
    }
    (void)user_data;
}

static void
on_reject_clicked(GtkButton *button, gpointer user_data)
{
    GnostrCommunityView *self = g_object_get_data(G_OBJECT(button), "view");
    const char *event_id = g_object_get_data(G_OBJECT(button), "event_id");

    if (self && event_id) {
        g_signal_emit(self, signals[SIGNAL_REJECT_POST], 0, event_id);
    }
    (void)user_data;
}

/* Factory functions for list views */
static void
setup_post_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
    (void)factory;
    (void)user_data;

    NostrGtkNoteCardRow *row = nostr_gtk_note_card_row_new();
    gtk_list_item_set_child(list_item, GTK_WIDGET(row));
}

static void
bind_post_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
    (void)factory;
    GnostrCommunityView *self = GNOSTR_COMMUNITY_VIEW(user_data);

    CommunityPostItem *item = gtk_list_item_get_item(list_item);
    if (!item || !item->post) return;

    NostrGtkNoteCardRow *row = NOSTR_GTK_NOTE_CARD_ROW(gtk_list_item_get_child(list_item));
    if (!row) return;

    GnostrCommunityPost *post = item->post;

    /* Set content */
    nostr_gtk_note_card_row_set_content(row, post->content);

    /* Set IDs */
    nostr_gtk_note_card_row_set_ids(row, post->event_id, NULL, post->author_pubkey);

    /* Set timestamp */
    nostr_gtk_note_card_row_set_timestamp(row, post->created_at, NULL);

    /* Set author info from cache */
    const char *author_name = item->author_name;
    const char *author_avatar = item->author_avatar;

    if (!author_name) {
        author_name = g_hash_table_lookup(self->author_profiles, post->author_pubkey);
    }
    if (!author_avatar) {
        author_avatar = g_hash_table_lookup(self->author_avatars, post->author_pubkey);
    }

    if (author_name) {
        nostr_gtk_note_card_row_set_author(row, author_name, NULL, author_avatar);
    } else {
        /* Fallback to truncated pubkey */
        char *short_pubkey = g_strndup(post->author_pubkey, 8);
        char *handle = g_strdup_printf("@%s...", short_pubkey);
        nostr_gtk_note_card_row_set_author(row, _("Anonymous"), handle, NULL);
        g_free(short_pubkey);
        g_free(handle);
    }

    /* Set login state */
    nostr_gtk_note_card_row_set_logged_in(row, self->user_pubkey != NULL);

    /* Connect signals (store handler IDs for unbind cleanup) */
    gulong h1 = g_signal_connect(row, "open-profile", G_CALLBACK(on_note_open_profile), self);
    gulong h2 = g_signal_connect(row, "view-thread-requested", G_CALLBACK(on_note_open_thread), self);
    gulong h3 = g_signal_connect(row, "zap-requested", G_CALLBACK(on_note_zap_requested), self);

    /* Store handler IDs on the list_item for cleanup in unbind */
    g_object_set_data(G_OBJECT(list_item), "handler-open-profile", GUINT_TO_POINTER(h1));
    g_object_set_data(G_OBJECT(list_item), "handler-view-thread", GUINT_TO_POINTER(h2));
    g_object_set_data(G_OBJECT(list_item), "handler-zap", GUINT_TO_POINTER(h3));
}

static void
unbind_post_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
    (void)factory;
    (void)user_data;

    NostrGtkNoteCardRow *row = NOSTR_GTK_NOTE_CARD_ROW(gtk_list_item_get_child(list_item));
    if (!row) return;

    /* nostrc-b3b0: Cancel async ops and clear labels before dispose to prevent
     * Pango SEGV when g_list_store_remove_all triggers unbind without teardown. */
    nostr_gtk_note_card_row_prepare_for_unbind(row);

    /* Disconnect signal handlers */
    gulong h1 = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(list_item), "handler-open-profile"));
    gulong h2 = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(list_item), "handler-view-thread"));
    gulong h3 = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(list_item), "handler-zap"));

    if (h1 > 0) g_signal_handler_disconnect(row, h1);
    if (h2 > 0) g_signal_handler_disconnect(row, h2);
    if (h3 > 0) g_signal_handler_disconnect(row, h3);

    g_object_set_data(G_OBJECT(list_item), "handler-open-profile", NULL);
    g_object_set_data(G_OBJECT(list_item), "handler-view-thread", NULL);
    g_object_set_data(G_OBJECT(list_item), "handler-zap", NULL);
}

static void
setup_pending_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
    (void)factory;
    (void)user_data;

    /* Pending posts have approve/reject buttons */
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

    NostrGtkNoteCardRow *row = nostr_gtk_note_card_row_new();
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(row));

    /* Action buttons */
    GtkWidget *action_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(action_box, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(box), action_box);

    GtkWidget *btn_reject = gtk_button_new_with_label(_("Reject"));
    gtk_widget_add_css_class(btn_reject, "destructive-action");
    gtk_box_append(GTK_BOX(action_box), btn_reject);

    GtkWidget *btn_approve = gtk_button_new_with_label(_("Approve"));
    gtk_widget_add_css_class(btn_approve, "suggested-action");
    gtk_box_append(GTK_BOX(action_box), btn_approve);

    /* Store references */
    g_object_set_data(G_OBJECT(box), "note_row", row);
    g_object_set_data(G_OBJECT(box), "btn_approve", btn_approve);
    g_object_set_data(G_OBJECT(box), "btn_reject", btn_reject);

    gtk_list_item_set_child(list_item, box);
}

static void
bind_pending_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
    (void)factory;
    GnostrCommunityView *self = GNOSTR_COMMUNITY_VIEW(user_data);

    CommunityPostItem *item = gtk_list_item_get_item(list_item);
    if (!item || !item->post) return;

    GtkWidget *box = gtk_list_item_get_child(list_item);
    NostrGtkNoteCardRow *row = g_object_get_data(G_OBJECT(box), "note_row");
    GtkWidget *btn_approve = g_object_get_data(G_OBJECT(box), "btn_approve");
    GtkWidget *btn_reject = g_object_get_data(G_OBJECT(box), "btn_reject");

    if (!row) return;

    GnostrCommunityPost *post = item->post;

    /* Set content */
    nostr_gtk_note_card_row_set_content(row, post->content);

    /* Set IDs */
    nostr_gtk_note_card_row_set_ids(row, post->event_id, NULL, post->author_pubkey);

    /* Set timestamp */
    nostr_gtk_note_card_row_set_timestamp(row, post->created_at, NULL);

    /* Set author info from item cache first, then hash table fallback */
    const char *author_name = item->author_name;
    const char *author_avatar = item->author_avatar;

    if (!author_name) {
        author_name = g_hash_table_lookup(self->author_profiles, post->author_pubkey);
    }
    if (!author_avatar) {
        author_avatar = g_hash_table_lookup(self->author_avatars, post->author_pubkey);
    }

    if (author_name) {
        nostr_gtk_note_card_row_set_author(row, author_name, NULL, author_avatar);
    } else {
        char *short_pubkey = g_strndup(post->author_pubkey, 8);
        char *handle = g_strdup_printf("@%s...", short_pubkey);
        nostr_gtk_note_card_row_set_author(row, _("Anonymous"), handle, NULL);
        g_free(short_pubkey);
        g_free(handle);
    }

    /* Configure approve/reject buttons */
    g_object_set_data(G_OBJECT(btn_approve), "view", self);
    g_object_set_data_full(G_OBJECT(btn_approve), "event_id",
                           g_strdup(post->event_id), g_free);
    g_object_set_data_full(G_OBJECT(btn_approve), "author",
                           g_strdup(post->author_pubkey), g_free);
    gulong h_approve = g_signal_connect(btn_approve, "clicked", G_CALLBACK(on_approve_clicked), NULL);

    g_object_set_data(G_OBJECT(btn_reject), "view", self);
    g_object_set_data_full(G_OBJECT(btn_reject), "event_id",
                           g_strdup(post->event_id), g_free);
    gulong h_reject = g_signal_connect(btn_reject, "clicked", G_CALLBACK(on_reject_clicked), NULL);

    /* Connect signals (store handler IDs for unbind cleanup) */
    gulong h_profile = g_signal_connect(row, "open-profile", G_CALLBACK(on_note_open_profile), self);

    g_object_set_data(G_OBJECT(list_item), "handler-open-profile", GUINT_TO_POINTER(h_profile));
    g_object_set_data(G_OBJECT(list_item), "handler-approve", GUINT_TO_POINTER(h_approve));
    g_object_set_data(G_OBJECT(list_item), "handler-reject", GUINT_TO_POINTER(h_reject));
}

static void
unbind_pending_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
    (void)factory;
    (void)user_data;

    GtkWidget *box = gtk_list_item_get_child(list_item);
    if (!box) return;

    NostrGtkNoteCardRow *row = g_object_get_data(G_OBJECT(box), "note_row");
    GtkWidget *btn_approve = g_object_get_data(G_OBJECT(box), "btn_approve");
    GtkWidget *btn_reject = g_object_get_data(G_OBJECT(box), "btn_reject");

    /* nostrc-b3b0: Cancel async ops and clear labels before dispose to prevent
     * Pango SEGV when g_list_store_remove_all triggers unbind without teardown. */
    if (row)
        nostr_gtk_note_card_row_prepare_for_unbind(row);

    /* Disconnect signal handlers */
    gulong h_profile = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(list_item), "handler-open-profile"));
    gulong h_approve = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(list_item), "handler-approve"));
    gulong h_reject = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(list_item), "handler-reject"));

    if (row && h_profile > 0) g_signal_handler_disconnect(row, h_profile);
    if (btn_approve && h_approve > 0) g_signal_handler_disconnect(btn_approve, h_approve);
    if (btn_reject && h_reject > 0) g_signal_handler_disconnect(btn_reject, h_reject);

    g_object_set_data(G_OBJECT(list_item), "handler-open-profile", NULL);
    g_object_set_data(G_OBJECT(list_item), "handler-approve", NULL);
    g_object_set_data(G_OBJECT(list_item), "handler-reject", NULL);
}

/* nostrc-b3b0: Teardown safety nets. During g_list_store_remove_all, GTK may
 * teardown rows whose unbind already ran (prepare_for_unbind is idempotent via
 * the disposed flag). But if teardown fires without a prior unbind (edge case
 * during rapid model changes), this prevents Pango SEGV. */
static void
teardown_post_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
    (void)factory;
    (void)user_data;

    GtkWidget *child = gtk_list_item_get_child(list_item);
    if (child && NOSTR_GTK_IS_NOTE_CARD_ROW(child)) {
        nostr_gtk_note_card_row_prepare_for_unbind(NOSTR_GTK_NOTE_CARD_ROW(child));
    }
}

static void
teardown_pending_row(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
    (void)factory;
    (void)user_data;

    GtkWidget *box = gtk_list_item_get_child(list_item);
    if (!box) return;

    NostrGtkNoteCardRow *row = g_object_get_data(G_OBJECT(box), "note_row");
    if (row) {
        nostr_gtk_note_card_row_prepare_for_unbind(row);
    }
}

static void
build_ui(GnostrCommunityView *self)
{
    /* Root container */
    self->root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_parent(self->root_box, GTK_WIDGET(self));

    /* Scrolled window */
    self->scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scroller),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(self->scroller, TRUE);
    gtk_box_append(GTK_BOX(self->root_box), self->scroller);

    self->content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scroller), self->content_box);

    /* === Header Section === */
    self->header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_widget_add_css_class(self->header_box, "community-header");
    gtk_widget_set_margin_start(self->header_box, 16);
    gtk_widget_set_margin_end(self->header_box, 16);
    gtk_widget_set_margin_top(self->header_box, 16);
    gtk_widget_set_margin_bottom(self->header_box, 16);
    gtk_box_append(GTK_BOX(self->content_box), self->header_box);

    /* Community image */
    self->image_frame = gtk_frame_new(NULL);
    gtk_widget_add_css_class(self->image_frame, "community-avatar");
    gtk_widget_set_size_request(self->image_frame, 80, 80);
    gtk_box_append(GTK_BOX(self->header_box), self->image_frame);

    GtkWidget *image_overlay = gtk_overlay_new();
    gtk_frame_set_child(GTK_FRAME(self->image_frame), image_overlay);

    self->image_initials = gtk_label_new("C");
    gtk_widget_add_css_class(self->image_initials, "avatar-initials");
    gtk_widget_add_css_class(self->image_initials, "title-1");
    gtk_overlay_set_child(GTK_OVERLAY(image_overlay), self->image_initials);

    self->community_image = gtk_picture_new();
    gtk_widget_set_visible(self->community_image, FALSE);
    gtk_picture_set_content_fit(GTK_PICTURE(self->community_image), GTK_CONTENT_FIT_COVER);
    gtk_overlay_add_overlay(GTK_OVERLAY(image_overlay), self->community_image);

    /* Info box */
    self->info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(self->info_box, TRUE);
    gtk_box_append(GTK_BOX(self->header_box), self->info_box);

    self->lbl_name = gtk_label_new(_("Community"));
    gtk_label_set_xalign(GTK_LABEL(self->lbl_name), 0);
    gtk_widget_add_css_class(self->lbl_name, "title-2");
    gtk_box_append(GTK_BOX(self->info_box), self->lbl_name);

    self->lbl_description = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(self->lbl_description), 0);
    gtk_label_set_wrap(GTK_LABEL(self->lbl_description), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(self->lbl_description), PANGO_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(self->lbl_description, "dim-label");
    gtk_widget_set_visible(self->lbl_description, FALSE);
    gtk_box_append(GTK_BOX(self->info_box), self->lbl_description);

    /* Stats row */
    self->stats_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_widget_set_margin_top(self->stats_box, 8);
    gtk_box_append(GTK_BOX(self->info_box), self->stats_box);

    self->lbl_members = gtk_label_new("0 members");
    gtk_widget_add_css_class(self->lbl_members, "caption");
    gtk_box_append(GTK_BOX(self->stats_box), self->lbl_members);

    self->lbl_posts = gtk_label_new("0 posts");
    gtk_widget_add_css_class(self->lbl_posts, "caption");
    gtk_box_append(GTK_BOX(self->stats_box), self->lbl_posts);

    /* Rules expander */
    self->rules_expander = gtk_expander_new(_("Rules"));
    gtk_widget_set_margin_start(self->rules_expander, 16);
    gtk_widget_set_margin_end(self->rules_expander, 16);
    gtk_widget_set_visible(self->rules_expander, FALSE);
    gtk_box_append(GTK_BOX(self->content_box), self->rules_expander);

    self->lbl_rules = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(self->lbl_rules), 0);
    gtk_label_set_wrap(GTK_LABEL(self->lbl_rules), TRUE);
    gtk_widget_add_css_class(self->lbl_rules, "dim-label");
    gtk_expander_set_child(GTK_EXPANDER(self->rules_expander), self->lbl_rules);

    /* Separator */
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(self->content_box), sep);

    /* === Action Bar === */
    self->action_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(self->action_bar, 16);
    gtk_widget_set_margin_end(self->action_bar, 16);
    gtk_widget_set_margin_top(self->action_bar, 8);
    gtk_widget_set_margin_bottom(self->action_bar, 8);
    gtk_box_append(GTK_BOX(self->content_box), self->action_bar);

    self->btn_compose = gtk_button_new_with_label(_("New Post"));
    gtk_widget_add_css_class(self->btn_compose, "suggested-action");
    gtk_widget_set_sensitive(self->btn_compose, FALSE);
    g_signal_connect(self->btn_compose, "clicked", G_CALLBACK(on_compose_clicked), self);
    gtk_box_append(GTK_BOX(self->action_bar), self->btn_compose);

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(self->action_bar), spacer);

    self->btn_pending = gtk_button_new_with_label(_("View Pending"));
    gtk_widget_add_css_class(self->btn_pending, "flat");
    gtk_widget_set_visible(self->btn_pending, FALSE);
    g_signal_connect(self->btn_pending, "clicked", G_CALLBACK(on_pending_clicked), self);
    gtk_box_append(GTK_BOX(self->action_bar), self->btn_pending);

    self->btn_join = gtk_button_new_with_label(_("Join"));
    gtk_widget_add_css_class(self->btn_join, "suggested-action");
    gtk_widget_set_sensitive(self->btn_join, FALSE);
    g_signal_connect(self->btn_join, "clicked", G_CALLBACK(on_join_clicked), self);
    gtk_box_append(GTK_BOX(self->action_bar), self->btn_join);

    /* === Content Stack === */
    self->content_stack = gtk_stack_new();
    gtk_widget_set_vexpand(self->content_stack, TRUE);
    gtk_box_append(GTK_BOX(self->content_box), self->content_stack);

    /* Approved posts view */
    self->posts_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_stack_add_named(GTK_STACK(self->content_stack), self->posts_box, "posts");

    /* Create list view for approved posts */
    self->approved_posts = g_list_store_new(community_post_item_get_type());
    GtkNoSelection *approved_selection = gtk_no_selection_new(G_LIST_MODEL(self->approved_posts));

    GtkListItemFactory *approved_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(approved_factory, "setup", G_CALLBACK(setup_post_row), self);
    g_signal_connect(approved_factory, "bind", G_CALLBACK(bind_post_row), self);
    g_signal_connect(approved_factory, "unbind", G_CALLBACK(unbind_post_row), self);
    g_signal_connect(approved_factory, "teardown", G_CALLBACK(teardown_post_row), self);

    self->approved_list = gtk_list_view_new(GTK_SELECTION_MODEL(approved_selection), approved_factory);
    gtk_widget_add_css_class(self->approved_list, "navigation-sidebar");
    gtk_box_append(GTK_BOX(self->posts_box), self->approved_list);

    /* Pending posts view (moderators only) */
    self->pending_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_stack_add_named(GTK_STACK(self->content_stack), self->pending_box, "pending");

    GtkWidget *pending_header = gtk_label_new(_("Pending Posts"));
    gtk_widget_add_css_class(pending_header, "title-4");
    gtk_widget_set_margin_start(pending_header, 16);
    gtk_widget_set_margin_top(pending_header, 16);
    gtk_label_set_xalign(GTK_LABEL(pending_header), 0);
    gtk_box_append(GTK_BOX(self->pending_box), pending_header);

    self->pending_posts = g_list_store_new(community_post_item_get_type());
    GtkNoSelection *pending_selection = gtk_no_selection_new(G_LIST_MODEL(self->pending_posts));

    GtkListItemFactory *pending_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(pending_factory, "setup", G_CALLBACK(setup_pending_row), self);
    g_signal_connect(pending_factory, "bind", G_CALLBACK(bind_pending_row), self);
    g_signal_connect(pending_factory, "unbind", G_CALLBACK(unbind_pending_row), self);
    g_signal_connect(pending_factory, "teardown", G_CALLBACK(teardown_pending_row), self);

    self->pending_list = gtk_list_view_new(GTK_SELECTION_MODEL(pending_selection), pending_factory);
    gtk_widget_add_css_class(self->pending_list, "navigation-sidebar");
    gtk_box_append(GTK_BOX(self->pending_box), self->pending_list);

    /* Empty state */
    self->empty_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_valign(self->empty_box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(self->empty_box, GTK_ALIGN_CENTER);
    gtk_stack_add_named(GTK_STACK(self->content_stack), self->empty_box, "empty");

    GtkWidget *empty_icon = gtk_image_new_from_icon_name("view-list-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(empty_icon), 64);
    gtk_widget_add_css_class(empty_icon, "dim-label");
    gtk_box_append(GTK_BOX(self->empty_box), empty_icon);

    GtkWidget *empty_label = gtk_label_new(_("No posts yet"));
    gtk_widget_add_css_class(empty_label, "title-3");
    gtk_widget_add_css_class(empty_label, "dim-label");
    gtk_box_append(GTK_BOX(self->empty_box), empty_label);

    GtkWidget *empty_sublabel = gtk_label_new(_("Be the first to post in this community!"));
    gtk_widget_add_css_class(empty_sublabel, "dim-label");
    gtk_box_append(GTK_BOX(self->empty_box), empty_sublabel);

    /* Loading state */
    GtkWidget *loading_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_valign(loading_box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(loading_box, GTK_ALIGN_CENTER);
    gtk_stack_add_named(GTK_STACK(self->content_stack), loading_box, "loading");

    self->loading_spinner = gtk_spinner_new();
    gtk_spinner_set_spinning(GTK_SPINNER(self->loading_spinner), TRUE);
    gtk_widget_set_size_request(self->loading_spinner, 32, 32);
    gtk_box_append(GTK_BOX(loading_box), self->loading_spinner);

    GtkWidget *loading_label = gtk_label_new(_("Loading posts..."));
    gtk_widget_add_css_class(loading_label, "dim-label");
    gtk_box_append(GTK_BOX(loading_box), loading_label);

    /* Default to posts view */
    gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "posts");
}

static void
gnostr_community_view_class_init(GnostrCommunityViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = gnostr_community_view_dispose;
    object_class->finalize = gnostr_community_view_finalize;

    /* Signals */
    signals[SIGNAL_OPEN_PROFILE] = g_signal_new(
        "open-profile",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_OPEN_NOTE] = g_signal_new(
        "open-note",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_COMPOSE_POST] = g_signal_new(
        "compose-post",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_APPROVE_POST] = g_signal_new(
        "approve-post",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

    signals[SIGNAL_REJECT_POST] = g_signal_new(
        "reject-post",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_JOIN_COMMUNITY] = g_signal_new(
        "join-community",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_LEAVE_COMMUNITY] = g_signal_new(
        "leave-community",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_ZAP_REQUESTED] = g_signal_new(
        "zap-requested",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

    /* CSS */
    gtk_widget_class_set_css_name(widget_class, "community-view");

    /* Layout */
    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gnostr_community_view_init(GnostrCommunityView *self)
{
    self->a_tag = NULL;
    self->d_tag = NULL;
    self->name = NULL;
    self->creator_pubkey = NULL;
    self->user_pubkey = NULL;
    self->post_count = 0;
    self->member_count = 0;
    self->is_joined = FALSE;
    self->is_moderator = FALSE;
    self->show_pending = FALSE;
    self->moderators = g_ptr_array_new_with_free_func(g_free);
    self->author_profiles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    self->author_avatars = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    build_ui(self);
}

GnostrCommunityView *
gnostr_community_view_new(void)
{
    return g_object_new(GNOSTR_TYPE_COMMUNITY_VIEW, NULL);
}

void
gnostr_community_view_set_community(GnostrCommunityView *self,
                                     const GnostrCommunity *community)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_VIEW(self));
    g_return_if_fail(community != NULL);

    /* Store data */
    g_free(self->a_tag);
    self->a_tag = gnostr_community_get_a_tag(community);

    g_free(self->d_tag);
    self->d_tag = g_strdup(community->d_tag);

    g_free(self->name);
    self->name = g_strdup(community->name);

    g_free(self->creator_pubkey);
    self->creator_pubkey = g_strdup(community->creator_pubkey);

    self->post_count = community->post_count;
    self->member_count = community->member_count;

    /* Copy moderators */
    g_ptr_array_set_size(self->moderators, 0);
    for (guint i = 0; i < community->moderators->len; i++) {
        GnostrCommunityModerator *mod = g_ptr_array_index(community->moderators, i);
        if (mod->pubkey) {
            g_ptr_array_add(self->moderators, g_strdup(mod->pubkey));
        }
    }

    /* Update UI */
    const char *display_name = community->name;
    if (!display_name || !*display_name) {
        display_name = community->d_tag;
    }

    if (GTK_IS_LABEL(self->lbl_name)) {
        gtk_label_set_text(GTK_LABEL(self->lbl_name),
                           display_name ? display_name : _("Unnamed Community"));
    }

    if (GTK_IS_LABEL(self->image_initials)) {
        char *initials = get_initials(display_name);
        gtk_label_set_text(GTK_LABEL(self->image_initials), initials);
        g_free(initials);
    }

    if (GTK_IS_LABEL(self->lbl_description)) {
        if (community->description && *community->description) {
            gtk_label_set_text(GTK_LABEL(self->lbl_description), community->description);
            gtk_widget_set_visible(self->lbl_description, TRUE);
        } else {
            gtk_widget_set_visible(self->lbl_description, FALSE);
        }
    }

    /* Rules */
    if (community->rules && *community->rules) {
        gtk_label_set_text(GTK_LABEL(self->lbl_rules), community->rules);
        gtk_widget_set_visible(self->rules_expander, TRUE);
    } else {
        gtk_widget_set_visible(self->rules_expander, FALSE);
    }

    update_stats_display(self);

    /* Load community image */
    if (community->image && *community->image) {
        gnostr_avatar_download_async(community->image,
                                      self->community_image,
                                      self->image_initials);
    } else {
        gtk_widget_set_visible(self->community_image, FALSE);
        gtk_widget_set_visible(self->image_initials, TRUE);
    }

    /* Check if current user is moderator */
    if (self->user_pubkey) {
        self->is_moderator = gnostr_community_is_moderator(community, self->user_pubkey);
        update_pending_button(self);
    }
}

const char *
gnostr_community_view_get_a_tag(GnostrCommunityView *self)
{
    g_return_val_if_fail(GNOSTR_IS_COMMUNITY_VIEW(self), NULL);
    return self->a_tag;
}

void
gnostr_community_view_add_post(GnostrCommunityView *self,
                                const GnostrCommunityPost *post)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_VIEW(self));
    g_return_if_fail(post != NULL);

    CommunityPostItem *item = community_post_item_new(post, FALSE);

    /* Check for cached author info */
    if (post->author_pubkey) {
        const char *name = g_hash_table_lookup(self->author_profiles, post->author_pubkey);
        const char *avatar = g_hash_table_lookup(self->author_avatars, post->author_pubkey);
        if (name) item->author_name = g_strdup(name);
        if (avatar) item->author_avatar = g_strdup(avatar);
    }

    g_list_store_append(self->approved_posts, item);
    g_object_unref(item);

    self->post_count++;
    update_stats_display(self);

    /* Show posts view if we have posts */
    if (g_list_model_get_n_items(G_LIST_MODEL(self->approved_posts)) > 0) {
        gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "posts");
    }
}

void
gnostr_community_view_add_pending_post(GnostrCommunityView *self,
                                        const GnostrCommunityPost *post)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_VIEW(self));
    g_return_if_fail(post != NULL);

    CommunityPostItem *item = community_post_item_new(post, TRUE);
    g_list_store_append(self->pending_posts, item);
    g_object_unref(item);
}

void
gnostr_community_view_remove_post(GnostrCommunityView *self,
                                   const char *event_id)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_VIEW(self));
    g_return_if_fail(event_id != NULL);

    /* Search in approved posts */
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->approved_posts));
    for (guint i = 0; i < n; i++) {
        CommunityPostItem *item = g_list_model_get_item(G_LIST_MODEL(self->approved_posts), i);
        if (item && item->post && g_strcmp0(item->post->event_id, event_id) == 0) {
            g_list_store_remove(self->approved_posts, i);
            g_object_unref(item);
            return;
        }
        if (item) g_object_unref(item);
    }

    /* Search in pending posts */
    n = g_list_model_get_n_items(G_LIST_MODEL(self->pending_posts));
    for (guint i = 0; i < n; i++) {
        CommunityPostItem *item = g_list_model_get_item(G_LIST_MODEL(self->pending_posts), i);
        if (item && item->post && g_strcmp0(item->post->event_id, event_id) == 0) {
            g_list_store_remove(self->pending_posts, i);
            g_object_unref(item);
            return;
        }
        if (item) g_object_unref(item);
    }
}

void
gnostr_community_view_mark_approved(GnostrCommunityView *self,
                                     const char *event_id,
                                     const char *approval_id)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_VIEW(self));
    g_return_if_fail(event_id != NULL);

    /* Find in pending and move to approved */
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->pending_posts));
    for (guint i = 0; i < n; i++) {
        CommunityPostItem *item = g_list_model_get_item(G_LIST_MODEL(self->pending_posts), i);
        if (item && item->post && g_strcmp0(item->post->event_id, event_id) == 0) {
            /* Mark as approved */
            item->post->is_approved = TRUE;
            g_free(item->post->approval_id);
            item->post->approval_id = g_strdup(approval_id);
            item->is_pending = FALSE;

            /* Move to approved list */
            g_object_ref(item);
            g_list_store_remove(self->pending_posts, i);
            g_list_store_append(self->approved_posts, item);
            g_object_unref(item);
            g_object_unref(item);

            self->post_count++;
            update_stats_display(self);
            return;
        }
        if (item) g_object_unref(item);
    }
}

void
gnostr_community_view_clear_posts(GnostrCommunityView *self)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_VIEW(self));

    g_list_store_remove_all(self->approved_posts);
    g_list_store_remove_all(self->pending_posts);
}

void
gnostr_community_view_set_loading(GnostrCommunityView *self,
                                   gboolean is_loading)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_VIEW(self));

    if (is_loading) {
        gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "loading");
    } else {
        guint n = g_list_model_get_n_items(G_LIST_MODEL(self->approved_posts));
        if (n > 0) {
            gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "posts");
        } else {
            gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "empty");
        }
    }
}

void
gnostr_community_view_set_empty(GnostrCommunityView *self,
                                 gboolean is_empty)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_VIEW(self));

    if (is_empty) {
        gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "empty");
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "posts");
    }
}

void
gnostr_community_view_set_user_pubkey(GnostrCommunityView *self,
                                       const char *pubkey)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_VIEW(self));

    g_free(self->user_pubkey);
    self->user_pubkey = g_strdup(pubkey);

    /* Check if user is moderator */
    if (pubkey) {
        gboolean is_mod = FALSE;
        if (self->creator_pubkey && g_strcmp0(self->creator_pubkey, pubkey) == 0) {
            is_mod = TRUE;
        } else {
            for (guint i = 0; i < self->moderators->len; i++) {
                const char *mod_pk = g_ptr_array_index(self->moderators, i);
                if (g_strcmp0(mod_pk, pubkey) == 0) {
                    is_mod = TRUE;
                    break;
                }
            }
        }
        self->is_moderator = is_mod;
    } else {
        self->is_moderator = FALSE;
    }

    update_join_button(self);
    update_compose_button(self);
    update_pending_button(self);
}

void
gnostr_community_view_set_joined(GnostrCommunityView *self,
                                  gboolean is_joined)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_VIEW(self));

    self->is_joined = is_joined;
    update_join_button(self);
    update_compose_button(self);
}

void
gnostr_community_view_set_is_moderator(GnostrCommunityView *self,
                                        gboolean is_moderator)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_VIEW(self));

    self->is_moderator = is_moderator;
    update_pending_button(self);
}

void
gnostr_community_view_set_show_pending(GnostrCommunityView *self,
                                        gboolean show_pending)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_VIEW(self));

    self->show_pending = show_pending;

    if (show_pending) {
        gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "pending");
        gtk_button_set_label(GTK_BUTTON(self->btn_pending), _("View Approved"));
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "posts");
        gtk_button_set_label(GTK_BUTTON(self->btn_pending), _("View Pending"));
    }
}

void
gnostr_community_view_update_author_profile(GnostrCommunityView *self,
                                              const char *pubkey,
                                              const char *display_name,
                                              const char *avatar_url)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_VIEW(self));
    g_return_if_fail(pubkey != NULL);

    if (display_name) {
        g_hash_table_insert(self->author_profiles, g_strdup(pubkey), g_strdup(display_name));
    }
    if (avatar_url) {
        g_hash_table_insert(self->author_avatars, g_strdup(pubkey), g_strdup(avatar_url));
    }

    /* Refresh rows matching this pubkey by updating item properties.
     * Using g_object_set() triggers notify signals which cause GTK to rebind rows. */
    GListStore *stores[] = { self->approved_posts, self->pending_posts };
    for (guint s = 0; s < G_N_ELEMENTS(stores); s++) {
        if (!stores[s]) continue;
        guint n_items = g_list_model_get_n_items(G_LIST_MODEL(stores[s]));
        for (guint i = 0; i < n_items; i++) {
            CommunityPostItem *item = g_list_model_get_item(G_LIST_MODEL(stores[s]), i);
            if (item && item->post && g_strcmp0(item->post->author_pubkey, pubkey) == 0) {
                /* Update properties via g_object_set to emit notify signals */
                g_object_set(item,
                             "author-name", display_name,
                             "author-avatar", avatar_url,
                             NULL);
                /* Also emit items-changed to ensure list view rebinds the row */
                g_list_model_items_changed(G_LIST_MODEL(stores[s]), i, 1, 1);
            }
            g_clear_object(&item);
        }
    }
}

GtkWidget *
gnostr_community_view_get_scrolled_window(GnostrCommunityView *self)
{
    g_return_val_if_fail(GNOSTR_IS_COMMUNITY_VIEW(self), NULL);
    return self->scroller;
}
