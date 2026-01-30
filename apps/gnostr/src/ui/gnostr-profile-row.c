/**
 * GnostrProfileRow - A row widget for displaying a profile in the Discover list
 *
 * Displays avatar, display name, NIP-05 identifier, and bio preview.
 * Includes action menu for follow/unfollow, mute, and copy npub.
 */

#include "gnostr-profile-row.h"
#include "gnostr-avatar-cache.h"

struct _GnostrProfileRow {
    GtkWidget parent_instance;

    /* Template widgets */
    GtkOverlay *avatar_box;
    GtkPicture *avatar_image;
    GtkLabel *avatar_initials;
    GtkLabel *lbl_display;
    GtkImage *nip05_badge;
    GtkLabel *lbl_nip05;
    GtkLabel *lbl_bio;
    GtkImage *follow_indicator;
    GtkImage *muted_indicator;
    GtkMenuButton *btn_actions;

    /* Actions popover (created on demand) */
    GtkWidget *actions_popover;
    GtkWidget *btn_follow;
    GtkWidget *follow_label;

    /* Data */
    char *pubkey;
    char *avatar_url;
    gboolean is_following;
    gboolean is_muted;
};

G_DEFINE_TYPE(GnostrProfileRow, gnostr_profile_row, GTK_TYPE_WIDGET)

enum {
    SIGNAL_OPEN_PROFILE,
    SIGNAL_FOLLOW_REQUESTED,
    SIGNAL_UNFOLLOW_REQUESTED,
    SIGNAL_MUTE_REQUESTED,
    SIGNAL_COPY_NPUB_REQUESTED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
gnostr_profile_row_dispose(GObject *object)
{
    GnostrProfileRow *self = GNOSTR_PROFILE_ROW(object);

    /* Clean up popover */
    if (self->actions_popover) {
        if (self->btn_actions) {
            gtk_menu_button_set_popover(self->btn_actions, NULL);
        }
        self->actions_popover = NULL;
    }

    /* Unparent template child */
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
    if (child)
        gtk_widget_unparent(child);

    G_OBJECT_CLASS(gnostr_profile_row_parent_class)->dispose(object);
}

static void
gnostr_profile_row_finalize(GObject *object)
{
    GnostrProfileRow *self = GNOSTR_PROFILE_ROW(object);

    g_free(self->pubkey);
    g_free(self->avatar_url);

    G_OBJECT_CLASS(gnostr_profile_row_parent_class)->finalize(object);
}

static void
on_row_clicked(GtkGestureClick *gesture, int n_press, double x, double y, GnostrProfileRow *self)
{
    (void)gesture;
    (void)n_press;

    /* Check if click was on the actions menu button - if so, don't open profile */
    if (self->btn_actions && GTK_IS_WIDGET(self->btn_actions)) {
        graphene_point_t point = GRAPHENE_POINT_INIT((float)x, (float)y);
        graphene_point_t btn_point;
        if (gtk_widget_compute_point(GTK_WIDGET(self), GTK_WIDGET(self->btn_actions),
                                      &point, &btn_point)) {
            /* Check if point is within button bounds */
            int btn_width = gtk_widget_get_width(GTK_WIDGET(self->btn_actions));
            int btn_height = gtk_widget_get_height(GTK_WIDGET(self->btn_actions));
            if (btn_point.x >= 0 && btn_point.x < btn_width &&
                btn_point.y >= 0 && btn_point.y < btn_height) {
                return;  /* Click was on menu button, let it handle the event */
            }
        }
    }

    if (self->pubkey) {
        g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->pubkey);
    }
}

static void
on_follow_clicked(GtkButton *btn, GnostrProfileRow *self)
{
    (void)btn;
    if (!self || !self->pubkey) return;

    /* Hide popover */
    if (self->actions_popover && GTK_IS_POPOVER(self->actions_popover)) {
        gtk_popover_popdown(GTK_POPOVER(self->actions_popover));
    }

    /* Emit appropriate signal based on current follow state */
    if (self->is_following) {
        g_signal_emit(self, signals[SIGNAL_UNFOLLOW_REQUESTED], 0, self->pubkey);
    } else {
        g_signal_emit(self, signals[SIGNAL_FOLLOW_REQUESTED], 0, self->pubkey);
    }
}

static void
on_mute_clicked(GtkButton *btn, GnostrProfileRow *self)
{
    (void)btn;
    if (!self || !self->pubkey) return;

    /* Hide popover */
    if (self->actions_popover && GTK_IS_POPOVER(self->actions_popover)) {
        gtk_popover_popdown(GTK_POPOVER(self->actions_popover));
    }

    g_signal_emit(self, signals[SIGNAL_MUTE_REQUESTED], 0, self->pubkey);
}

static void
on_copy_npub_clicked(GtkButton *btn, GnostrProfileRow *self)
{
    (void)btn;
    if (!self || !self->pubkey || strlen(self->pubkey) != 64) return;

    /* Hide popover */
    if (self->actions_popover && GTK_IS_POPOVER(self->actions_popover)) {
        gtk_popover_popdown(GTK_POPOVER(self->actions_popover));
    }

    g_signal_emit(self, signals[SIGNAL_COPY_NPUB_REQUESTED], 0, self->pubkey);
}

static void
update_follow_button_label(GnostrProfileRow *self)
{
    if (!self->follow_label) return;

    if (self->is_following) {
        gtk_label_set_text(GTK_LABEL(self->follow_label), "Unfollow");
    } else {
        gtk_label_set_text(GTK_LABEL(self->follow_label), "Follow");
    }
}

static void
create_actions_popover(GnostrProfileRow *self)
{
    if (self->actions_popover) return;

    self->actions_popover = gtk_popover_new();

    /* Create a vertical box for the menu items */
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);

    /* Follow/Unfollow button */
    self->btn_follow = gtk_button_new();
    GtkWidget *follow_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *follow_icon = gtk_image_new_from_icon_name("emblem-favorite-symbolic");
    self->follow_label = gtk_label_new(self->is_following ? "Unfollow" : "Follow");
    gtk_box_append(GTK_BOX(follow_box), follow_icon);
    gtk_box_append(GTK_BOX(follow_box), self->follow_label);
    gtk_button_set_child(GTK_BUTTON(self->btn_follow), follow_box);
    gtk_button_set_has_frame(GTK_BUTTON(self->btn_follow), FALSE);
    g_signal_connect(self->btn_follow, "clicked", G_CALLBACK(on_follow_clicked), self);
    gtk_box_append(GTK_BOX(box), self->btn_follow);

    /* Mute button */
    GtkWidget *mute_btn = gtk_button_new();
    GtkWidget *mute_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *mute_icon = gtk_image_new_from_icon_name("action-unavailable-symbolic");
    GtkWidget *mute_label = gtk_label_new("Mute");
    gtk_box_append(GTK_BOX(mute_box), mute_icon);
    gtk_box_append(GTK_BOX(mute_box), mute_label);
    gtk_button_set_child(GTK_BUTTON(mute_btn), mute_box);
    gtk_button_set_has_frame(GTK_BUTTON(mute_btn), FALSE);
    g_signal_connect(mute_btn, "clicked", G_CALLBACK(on_mute_clicked), self);
    gtk_box_append(GTK_BOX(box), mute_btn);

    /* Separator */
    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(separator, 4);
    gtk_widget_set_margin_bottom(separator, 4);
    gtk_box_append(GTK_BOX(box), separator);

    /* Copy npub button */
    GtkWidget *copy_btn = gtk_button_new();
    GtkWidget *copy_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *copy_icon = gtk_image_new_from_icon_name("edit-copy-symbolic");
    GtkWidget *copy_label = gtk_label_new("Copy npub");
    gtk_box_append(GTK_BOX(copy_box), copy_icon);
    gtk_box_append(GTK_BOX(copy_box), copy_label);
    gtk_button_set_child(GTK_BUTTON(copy_btn), copy_box);
    gtk_button_set_has_frame(GTK_BUTTON(copy_btn), FALSE);
    g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_copy_npub_clicked), self);
    gtk_box_append(GTK_BOX(box), copy_btn);

    gtk_popover_set_child(GTK_POPOVER(self->actions_popover), box);
    gtk_menu_button_set_popover(self->btn_actions, self->actions_popover);
}


static void
gnostr_profile_row_class_init(GnostrProfileRowClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = gnostr_profile_row_dispose;
    object_class->finalize = gnostr_profile_row_finalize;

    /* Load template */
    gtk_widget_class_set_template_from_resource(widget_class,
        "/org/gnostr/ui/ui/widgets/gnostr-profile-row.ui");

    /* Bind template children */
    gtk_widget_class_bind_template_child(widget_class, GnostrProfileRow, avatar_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrProfileRow, avatar_image);
    gtk_widget_class_bind_template_child(widget_class, GnostrProfileRow, avatar_initials);
    gtk_widget_class_bind_template_child(widget_class, GnostrProfileRow, lbl_display);
    gtk_widget_class_bind_template_child(widget_class, GnostrProfileRow, nip05_badge);
    gtk_widget_class_bind_template_child(widget_class, GnostrProfileRow, lbl_nip05);
    gtk_widget_class_bind_template_child(widget_class, GnostrProfileRow, lbl_bio);
    gtk_widget_class_bind_template_child(widget_class, GnostrProfileRow, follow_indicator);
    gtk_widget_class_bind_template_child(widget_class, GnostrProfileRow, muted_indicator);
    gtk_widget_class_bind_template_child(widget_class, GnostrProfileRow, btn_actions);

    /* Signals */
    signals[SIGNAL_OPEN_PROFILE] = g_signal_new(
        "open-profile",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_FOLLOW_REQUESTED] = g_signal_new(
        "follow-requested",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_UNFOLLOW_REQUESTED] = g_signal_new(
        "unfollow-requested",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_MUTE_REQUESTED] = g_signal_new(
        "mute-requested",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIGNAL_COPY_NPUB_REQUESTED] = g_signal_new(
        "copy-npub-requested",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    /* CSS */
    gtk_widget_class_set_css_name(widget_class, "profile-row");

    /* Layout */
    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gnostr_profile_row_init(GnostrProfileRow *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));

    self->pubkey = NULL;
    self->avatar_url = NULL;
    self->is_following = FALSE;
    self->is_muted = FALSE;
    self->actions_popover = NULL;
    self->btn_follow = NULL;
    self->follow_label = NULL;

    /* Create the actions popover and set it on the menu button.
     * GtkMenuButton doesn't have a 'clicked' signal - it shows its popover automatically. */
    if (self->btn_actions) {
        create_actions_popover(self);
    }

    /* Click gesture for the whole row (but not on the actions button) */
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(on_row_clicked), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));
}

GnostrProfileRow *
gnostr_profile_row_new(void)
{
    return g_object_new(GNOSTR_TYPE_PROFILE_ROW, NULL);
}

static char *
get_initials(const char *name)
{
    if (!name || !*name)
        return g_strdup("?");

    /* Get first char, handle UTF-8 */
    gunichar c = g_utf8_get_char(name);
    char buf[7];
    int len = g_unichar_to_utf8(g_unichar_toupper(c), buf);
    buf[len] = '\0';
    return g_strdup(buf);
}

void
gnostr_profile_row_set_profile(GnostrProfileRow *self,
                               const char *pubkey_hex,
                               const char *display_name,
                               const char *name,
                               const char *nip05,
                               const char *bio,
                               const char *avatar_url)
{
    g_return_if_fail(GNOSTR_IS_PROFILE_ROW(self));

    g_free(self->pubkey);
    self->pubkey = g_strdup(pubkey_hex);

    g_free(self->avatar_url);
    self->avatar_url = g_strdup(avatar_url);

    /* Determine display name to show */
    const char *shown_name = display_name;
    if (!shown_name || !*shown_name) {
        shown_name = name;
    }
    if (!shown_name || !*shown_name) {
        /* Fallback to truncated pubkey */
        if (pubkey_hex && strlen(pubkey_hex) >= 8) {
            char *truncated = g_strdup_printf("%.8s...", pubkey_hex);
            gtk_label_set_text(self->lbl_display, truncated);
            g_free(truncated);
        } else {
            gtk_label_set_text(self->lbl_display, "Unknown");
        }
        shown_name = pubkey_hex;
    } else {
        gtk_label_set_text(self->lbl_display, shown_name);
    }

    /* Set NIP-05 if available */
    if (nip05 && *nip05) {
        gtk_label_set_text(self->lbl_nip05, nip05);
        gtk_widget_set_visible(GTK_WIDGET(self->lbl_nip05), TRUE);
        gtk_widget_set_visible(GTK_WIDGET(self->nip05_badge), TRUE);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->lbl_nip05), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(self->nip05_badge), FALSE);
    }

    /* Set bio preview */
    if (bio && *bio) {
        /* Clean up bio for single-line preview */
        char *clean_bio = g_strdup(bio);
        /* Replace newlines with spaces */
        for (char *p = clean_bio; *p; p++) {
            if (*p == '\n' || *p == '\r') *p = ' ';
        }
        gtk_label_set_text(self->lbl_bio, clean_bio);
        gtk_widget_set_visible(GTK_WIDGET(self->lbl_bio), TRUE);
        g_free(clean_bio);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->lbl_bio), FALSE);
    }

    /* Set avatar initials */
    char *initials = get_initials(shown_name);
    gtk_label_set_text(self->avatar_initials, initials);
    g_free(initials);

    /* Load avatar image if URL provided */
    if (avatar_url && *avatar_url) {
        gnostr_avatar_download_async(avatar_url, GTK_WIDGET(self->avatar_image), GTK_WIDGET(self->avatar_initials));
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->avatar_image), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(self->avatar_initials), TRUE);
    }
}

void
gnostr_profile_row_set_following(GnostrProfileRow *self, gboolean is_following)
{
    g_return_if_fail(GNOSTR_IS_PROFILE_ROW(self));
    self->is_following = is_following;
    gtk_widget_set_visible(GTK_WIDGET(self->follow_indicator), is_following);
    update_follow_button_label(self);
}

const char *
gnostr_profile_row_get_pubkey(GnostrProfileRow *self)
{
    g_return_val_if_fail(GNOSTR_IS_PROFILE_ROW(self), NULL);
    return self->pubkey;
}

gboolean
gnostr_profile_row_get_is_following(GnostrProfileRow *self)
{
    g_return_val_if_fail(GNOSTR_IS_PROFILE_ROW(self), FALSE);
    return self->is_following;
}

void
gnostr_profile_row_set_muted(GnostrProfileRow *self, gboolean is_muted)
{
    g_return_if_fail(GNOSTR_IS_PROFILE_ROW(self));
    self->is_muted = is_muted;
    gtk_widget_set_visible(GTK_WIDGET(self->muted_indicator), is_muted);

    /* Apply grayed-out styling when muted */
    if (is_muted) {
        gtk_widget_add_css_class(GTK_WIDGET(self), "muted");
    } else {
        gtk_widget_remove_css_class(GTK_WIDGET(self), "muted");
    }
}

gboolean
gnostr_profile_row_get_is_muted(GnostrProfileRow *self)
{
    g_return_val_if_fail(GNOSTR_IS_PROFILE_ROW(self), FALSE);
    return self->is_muted;
}
