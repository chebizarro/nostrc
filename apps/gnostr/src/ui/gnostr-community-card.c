/**
 * GnostrCommunityCard - NIP-72 Moderated Community Card Widget
 *
 * Displays a community card with info and action buttons.
 */

#include "gnostr-community-card.h"
#include "gnostr-avatar-cache.h"
#include <glib/gi18n.h>
#include <time.h>

struct _GnostrCommunityCard {
    GtkWidget parent_instance;

    /* Widgets - manually created since no UI template */
    GtkWidget *root_box;
    GtkWidget *header_box;
    GtkWidget *image_frame;
    GtkWidget *community_image;
    GtkWidget *image_initials;
    GtkWidget *info_box;
    GtkWidget *lbl_name;
    GtkWidget *lbl_description;
    GtkWidget *lbl_rules;
    GtkWidget *stats_box;
    GtkWidget *lbl_members;
    GtkWidget *lbl_posts;
    GtkWidget *lbl_moderators;
    GtkWidget *action_box;
    GtkWidget *btn_view;
    GtkWidget *btn_join;
    GtkWidget *btn_post;

    /* Data */
    char *a_tag;
    char *d_tag;
    char *name;
    char *description;
    char *image_url;
    char *rules;
    char *creator_pubkey;
    guint post_count;
    guint member_count;
    guint moderator_count;
    gboolean is_joined;
    gboolean is_moderator;
    gboolean is_logged_in;
};

G_DEFINE_TYPE(GnostrCommunityCard, gnostr_community_card, GTK_TYPE_WIDGET)

enum {
    SIGNAL_COMMUNITY_SELECTED,
    SIGNAL_OPEN_PROFILE,
    SIGNAL_JOIN_COMMUNITY,
    SIGNAL_LEAVE_COMMUNITY,
    SIGNAL_CREATE_POST,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
gnostr_community_card_dispose(GObject *object)
{
    GnostrCommunityCard *self = GNOSTR_COMMUNITY_CARD(object);

    /* Unparent the root widget */
    if (self->root_box) {
        gtk_widget_unparent(self->root_box);
        self->root_box = NULL;
    }

    G_OBJECT_CLASS(gnostr_community_card_parent_class)->dispose(object);
}

static void
gnostr_community_card_finalize(GObject *object)
{
    GnostrCommunityCard *self = GNOSTR_COMMUNITY_CARD(object);

    g_free(self->a_tag);
    g_free(self->d_tag);
    g_free(self->name);
    g_free(self->description);
    g_free(self->image_url);
    g_free(self->rules);
    g_free(self->creator_pubkey);

    G_OBJECT_CLASS(gnostr_community_card_parent_class)->finalize(object);
}

static void
on_view_clicked(GtkButton *button, GnostrCommunityCard *self)
{
    (void)button;
    if (self->a_tag) {
        g_signal_emit(self, signals[SIGNAL_COMMUNITY_SELECTED], 0, self->a_tag);
    }
}

static void
on_join_clicked(GtkButton *button, GnostrCommunityCard *self)
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
on_post_clicked(GtkButton *button, GnostrCommunityCard *self)
{
    (void)button;
    if (self->a_tag) {
        g_signal_emit(self, signals[SIGNAL_CREATE_POST], 0, self->a_tag);
    }
}

static void
on_card_clicked(GtkGestureClick *gesture, int n_press, double x, double y, GnostrCommunityCard *self)
{
    (void)gesture;
    (void)n_press;
    (void)x;
    (void)y;

    if (self->a_tag) {
        g_signal_emit(self, signals[SIGNAL_COMMUNITY_SELECTED], 0, self->a_tag);
    }
}

static void
update_join_button(GnostrCommunityCard *self)
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

    gtk_widget_set_sensitive(self->btn_join, self->is_logged_in);
}

static void
update_stats_display(GnostrCommunityCard *self)
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

    if (GTK_IS_LABEL(self->lbl_moderators)) {
        char *text = g_strdup_printf(g_dngettext(NULL, "%u moderator", "%u moderators",
                                                  self->moderator_count), self->moderator_count);
        gtk_label_set_text(GTK_LABEL(self->lbl_moderators), text);
        g_free(text);
    }
}

static char *
get_initials(const char *name)
{
    if (!name || !*name)
        return g_strdup("C");

    /* Get first two chars, handle UTF-8 */
    gunichar c1 = g_utf8_get_char(name);
    char buf[14];
    int len = g_unichar_to_utf8(g_unichar_toupper(c1), buf);

    /* Try to get second word initial */
    const char *space = strchr(name, ' ');
    if (space && *(space + 1)) {
        gunichar c2 = g_utf8_get_char(space + 1);
        len += g_unichar_to_utf8(g_unichar_toupper(c2), buf + len);
    }

    buf[len] = '\0';
    return g_strdup(buf);
}

static void
build_ui(GnostrCommunityCard *self)
{
    /* Root container */
    self->root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_parent(self->root_box, GTK_WIDGET(self));
    gtk_widget_add_css_class(self->root_box, "card");
    gtk_widget_add_css_class(self->root_box, "community-card");
    gtk_widget_set_margin_start(self->root_box, 12);
    gtk_widget_set_margin_end(self->root_box, 12);
    gtk_widget_set_margin_top(self->root_box, 8);
    gtk_widget_set_margin_bottom(self->root_box, 8);

    /* Header with image and name */
    self->header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(self->root_box), self->header_box);

    /* Community image/avatar frame */
    self->image_frame = gtk_frame_new(NULL);
    gtk_widget_add_css_class(self->image_frame, "community-avatar");
    gtk_widget_set_size_request(self->image_frame, 64, 64);
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

    /* Info box (name, description) */
    self->info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(self->info_box, TRUE);
    gtk_box_append(GTK_BOX(self->header_box), self->info_box);

    self->lbl_name = gtk_label_new(_("Community"));
    gtk_label_set_xalign(GTK_LABEL(self->lbl_name), 0);
    gtk_widget_add_css_class(self->lbl_name, "title-3");
    gtk_label_set_ellipsize(GTK_LABEL(self->lbl_name), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(self->info_box), self->lbl_name);

    self->lbl_description = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(self->lbl_description), 0);
    gtk_label_set_wrap(GTK_LABEL(self->lbl_description), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(self->lbl_description), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(self->lbl_description), 60);
    gtk_label_set_ellipsize(GTK_LABEL(self->lbl_description), PANGO_ELLIPSIZE_END);
    gtk_label_set_lines(GTK_LABEL(self->lbl_description), 2);
    gtk_widget_add_css_class(self->lbl_description, "dim-label");
    gtk_widget_set_visible(self->lbl_description, FALSE);
    gtk_box_append(GTK_BOX(self->info_box), self->lbl_description);

    /* Rules (collapsed by default) */
    self->lbl_rules = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(self->lbl_rules), 0);
    gtk_label_set_wrap(GTK_LABEL(self->lbl_rules), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(self->lbl_rules), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(self->lbl_rules), 80);
    gtk_widget_add_css_class(self->lbl_rules, "caption");
    gtk_widget_add_css_class(self->lbl_rules, "dim-label");
    gtk_widget_set_visible(self->lbl_rules, FALSE);
    gtk_box_append(GTK_BOX(self->root_box), self->lbl_rules);

    /* Statistics row */
    self->stats_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_box_append(GTK_BOX(self->root_box), self->stats_box);

    self->lbl_members = gtk_label_new("0 members");
    gtk_widget_add_css_class(self->lbl_members, "caption");
    gtk_box_append(GTK_BOX(self->stats_box), self->lbl_members);

    self->lbl_posts = gtk_label_new("0 posts");
    gtk_widget_add_css_class(self->lbl_posts, "caption");
    gtk_box_append(GTK_BOX(self->stats_box), self->lbl_posts);

    self->lbl_moderators = gtk_label_new("0 moderators");
    gtk_widget_add_css_class(self->lbl_moderators, "caption");
    gtk_box_append(GTK_BOX(self->stats_box), self->lbl_moderators);

    /* Action buttons */
    self->action_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(self->action_box, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(self->root_box), self->action_box);

    self->btn_view = gtk_button_new_with_label(_("View"));
    gtk_widget_add_css_class(self->btn_view, "flat");
    g_signal_connect(self->btn_view, "clicked", G_CALLBACK(on_view_clicked), self);
    gtk_box_append(GTK_BOX(self->action_box), self->btn_view);

    self->btn_post = gtk_button_new_with_label(_("Post"));
    gtk_widget_add_css_class(self->btn_post, "flat");
    gtk_widget_set_sensitive(self->btn_post, FALSE);
    g_signal_connect(self->btn_post, "clicked", G_CALLBACK(on_post_clicked), self);
    gtk_box_append(GTK_BOX(self->action_box), self->btn_post);

    self->btn_join = gtk_button_new_with_label(_("Join"));
    gtk_widget_add_css_class(self->btn_join, "suggested-action");
    gtk_widget_set_sensitive(self->btn_join, FALSE);
    g_signal_connect(self->btn_join, "clicked", G_CALLBACK(on_join_clicked), self);
    gtk_box_append(GTK_BOX(self->action_box), self->btn_join);

    /* Click gesture for the card */
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(on_card_clicked), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));
}

static void
gnostr_community_card_class_init(GnostrCommunityCardClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = gnostr_community_card_dispose;
    object_class->finalize = gnostr_community_card_finalize;

    /* Signals */
    signals[SIGNAL_COMMUNITY_SELECTED] = g_signal_new(
        "community-selected",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_OPEN_PROFILE] = g_signal_new(
        "open-profile",
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

    signals[SIGNAL_CREATE_POST] = g_signal_new(
        "create-post",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    /* CSS */
    gtk_widget_class_set_css_name(widget_class, "community-card");

    /* Layout */
    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gnostr_community_card_init(GnostrCommunityCard *self)
{
    self->a_tag = NULL;
    self->d_tag = NULL;
    self->name = NULL;
    self->description = NULL;
    self->image_url = NULL;
    self->rules = NULL;
    self->creator_pubkey = NULL;
    self->post_count = 0;
    self->member_count = 0;
    self->moderator_count = 0;
    self->is_joined = FALSE;
    self->is_moderator = FALSE;
    self->is_logged_in = FALSE;

    build_ui(self);
}

GnostrCommunityCard *
gnostr_community_card_new(void)
{
    return g_object_new(GNOSTR_TYPE_COMMUNITY_CARD, NULL);
}

void
gnostr_community_card_set_community(GnostrCommunityCard *self,
                                     const GnostrCommunity *community)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_CARD(self));
    g_return_if_fail(community != NULL);

    /* Store data */
    g_free(self->a_tag);
    self->a_tag = gnostr_community_get_a_tag(community);

    g_free(self->d_tag);
    self->d_tag = g_strdup(community->d_tag);

    g_free(self->name);
    self->name = g_strdup(community->name);

    g_free(self->description);
    self->description = g_strdup(community->description);

    g_free(self->image_url);
    self->image_url = g_strdup(community->image);

    g_free(self->rules);
    self->rules = g_strdup(community->rules);

    g_free(self->creator_pubkey);
    self->creator_pubkey = g_strdup(community->creator_pubkey);

    self->post_count = community->post_count;
    self->member_count = community->member_count;
    self->moderator_count = community->moderators ? community->moderators->len : 0;

    /* Update UI */
    const char *display_name = community->name;
    if (!display_name || !*display_name) {
        display_name = community->d_tag;
    }

    if (GTK_IS_LABEL(self->lbl_name)) {
        gtk_label_set_text(GTK_LABEL(self->lbl_name),
                           display_name ? display_name : _("Unnamed Community"));
    }

    /* Set initials */
    if (GTK_IS_LABEL(self->image_initials)) {
        char *initials = get_initials(display_name);
        gtk_label_set_text(GTK_LABEL(self->image_initials), initials);
        g_free(initials);
    }

    /* Set description */
    if (GTK_IS_LABEL(self->lbl_description)) {
        if (community->description && *community->description) {
            gtk_label_set_text(GTK_LABEL(self->lbl_description), community->description);
            gtk_widget_set_visible(self->lbl_description, TRUE);
        } else {
            gtk_widget_set_visible(self->lbl_description, FALSE);
        }
    }

    /* Set rules */
    if (GTK_IS_LABEL(self->lbl_rules)) {
        if (community->rules && *community->rules) {
            char *rules_text = g_strdup_printf(_("Rules: %s"), community->rules);
            gtk_label_set_text(GTK_LABEL(self->lbl_rules), rules_text);
            gtk_widget_set_visible(self->lbl_rules, TRUE);
            g_free(rules_text);
        } else {
            gtk_widget_set_visible(self->lbl_rules, FALSE);
        }
    }

    /* Update stats */
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
}

const char *
gnostr_community_card_get_a_tag(GnostrCommunityCard *self)
{
    g_return_val_if_fail(GNOSTR_IS_COMMUNITY_CARD(self), NULL);
    return self->a_tag;
}

const char *
gnostr_community_card_get_d_tag(GnostrCommunityCard *self)
{
    g_return_val_if_fail(GNOSTR_IS_COMMUNITY_CARD(self), NULL);
    return self->d_tag;
}

const char *
gnostr_community_card_get_name(GnostrCommunityCard *self)
{
    g_return_val_if_fail(GNOSTR_IS_COMMUNITY_CARD(self), NULL);
    return self->name;
}

const char *
gnostr_community_card_get_description(GnostrCommunityCard *self)
{
    g_return_val_if_fail(GNOSTR_IS_COMMUNITY_CARD(self), NULL);
    return self->description;
}

const char *
gnostr_community_card_get_creator_pubkey(GnostrCommunityCard *self)
{
    g_return_val_if_fail(GNOSTR_IS_COMMUNITY_CARD(self), NULL);
    return self->creator_pubkey;
}

void
gnostr_community_card_set_joined(GnostrCommunityCard *self,
                                  gboolean is_joined)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_CARD(self));

    self->is_joined = is_joined;
    update_join_button(self);

    /* Enable post button if joined and logged in */
    if (GTK_IS_WIDGET(self->btn_post)) {
        gtk_widget_set_sensitive(self->btn_post, is_joined && self->is_logged_in);
    }
}

gboolean
gnostr_community_card_get_joined(GnostrCommunityCard *self)
{
    g_return_val_if_fail(GNOSTR_IS_COMMUNITY_CARD(self), FALSE);
    return self->is_joined;
}

void
gnostr_community_card_set_logged_in(GnostrCommunityCard *self,
                                     gboolean logged_in)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_CARD(self));

    self->is_logged_in = logged_in;
    update_join_button(self);

    /* Update post button sensitivity */
    if (GTK_IS_WIDGET(self->btn_post)) {
        gtk_widget_set_sensitive(self->btn_post, self->is_joined && logged_in);
    }
}

void
gnostr_community_card_set_is_moderator(GnostrCommunityCard *self,
                                        gboolean is_moderator)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_CARD(self));

    self->is_moderator = is_moderator;

    /* Could add a moderator badge or special styling here */
}

void
gnostr_community_card_set_post_count(GnostrCommunityCard *self,
                                      guint count)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_CARD(self));

    self->post_count = count;
    update_stats_display(self);
}

void
gnostr_community_card_set_member_count(GnostrCommunityCard *self,
                                        guint count)
{
    g_return_if_fail(GNOSTR_IS_COMMUNITY_CARD(self));

    self->member_count = count;
    update_stats_display(self);
}
