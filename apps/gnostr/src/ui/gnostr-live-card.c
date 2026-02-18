/**
 * gnostr-live-card.c - NIP-53 Live Activity Card Widget Implementation
 *
 * Displays live streams, broadcasts, and events per NIP-53.
 */

#include "gnostr-live-card.h"
#include <string.h>

struct _GnostrLiveCard {
    GtkWidget parent_instance;

    /* Template widgets */
    GtkBox *root_box;
    GtkFrame *card_frame;
    GtkOverlay *image_overlay;
    GtkPicture *cover_image;
    GtkBox *status_badge;
    GtkImage *status_icon;
    GtkLabel *status_label;
    GtkBox *content_box;
    GtkLabel *title_label;
    GtkLabel *summary_label;
    GtkBox *meta_box;
    GtkBox *speakers_box;
    GtkLabel *viewers_label;
    GtkLabel *time_label;
    GtkButton *action_button;
    GtkSpinner *loading_spinner;
    GtkLabel *error_label;

    /* Hashtag box */
    GtkBox *hashtags_box;

    /* Data */
    GnostrLiveActivity *activity;
    gboolean is_compact;
    GCancellable *image_cancellable;
};

G_DEFINE_TYPE(GnostrLiveCard, gnostr_live_card, GTK_TYPE_WIDGET)

enum {
    SIGNAL_WATCH_LIVE,
    SIGNAL_SET_REMINDER,
    SIGNAL_PROFILE_CLICKED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void update_display(GnostrLiveCard *self);
static void update_status_badge(GnostrLiveCard *self);
static void update_speakers(GnostrLiveCard *self);
static void update_action_button(GnostrLiveCard *self);
static void load_cover_image(GnostrLiveCard *self);

static void
gnostr_live_card_dispose(GObject *object)
{
    GnostrLiveCard *self = GNOSTR_LIVE_CARD(object);

    if (self->image_cancellable) {
        g_cancellable_cancel(self->image_cancellable);
        g_clear_object(&self->image_cancellable);
    }

    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
    if (child)
        gtk_widget_unparent(child);

    G_OBJECT_CLASS(gnostr_live_card_parent_class)->dispose(object);
}

static void
gnostr_live_card_finalize(GObject *object)
{
    GnostrLiveCard *self = GNOSTR_LIVE_CARD(object);

    if (self->activity) {
        gnostr_live_activity_free(self->activity);
        self->activity = NULL;
    }

    G_OBJECT_CLASS(gnostr_live_card_parent_class)->finalize(object);
}

static void
on_action_button_clicked(GtkButton *button, GnostrLiveCard *self)
{
    (void)button;

    if (!self->activity) return;

    if (gnostr_live_activity_is_active(self->activity)) {
        g_signal_emit(self, signals[SIGNAL_WATCH_LIVE], 0);
    } else if (self->activity->status == GNOSTR_LIVE_STATUS_PLANNED) {
        g_signal_emit(self, signals[SIGNAL_SET_REMINDER], 0);
    } else if (self->activity->status == GNOSTR_LIVE_STATUS_ENDED) {
        /* For ended events with recordings, could emit watch-recording */
        if (self->activity->recording_urls && self->activity->recording_urls[0]) {
            g_signal_emit(self, signals[SIGNAL_WATCH_LIVE], 0);
        }
    }
}

static void
gnostr_live_card_class_init(GnostrLiveCardClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = gnostr_live_card_dispose;
    object_class->finalize = gnostr_live_card_finalize;

    /* Load UI template */
    gtk_widget_class_set_template_from_resource(widget_class,
        "/org/gnostr/ui/ui/widgets/gnostr-live-card.ui");

    /* Bind template children */
    gtk_widget_class_bind_template_child(widget_class, GnostrLiveCard, root_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrLiveCard, card_frame);
    gtk_widget_class_bind_template_child(widget_class, GnostrLiveCard, image_overlay);
    gtk_widget_class_bind_template_child(widget_class, GnostrLiveCard, cover_image);
    gtk_widget_class_bind_template_child(widget_class, GnostrLiveCard, status_badge);
    gtk_widget_class_bind_template_child(widget_class, GnostrLiveCard, status_icon);
    gtk_widget_class_bind_template_child(widget_class, GnostrLiveCard, status_label);
    gtk_widget_class_bind_template_child(widget_class, GnostrLiveCard, content_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrLiveCard, title_label);
    gtk_widget_class_bind_template_child(widget_class, GnostrLiveCard, summary_label);
    gtk_widget_class_bind_template_child(widget_class, GnostrLiveCard, meta_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrLiveCard, speakers_box);
    gtk_widget_class_bind_template_child(widget_class, GnostrLiveCard, viewers_label);
    gtk_widget_class_bind_template_child(widget_class, GnostrLiveCard, time_label);
    gtk_widget_class_bind_template_child(widget_class, GnostrLiveCard, action_button);
    gtk_widget_class_bind_template_child(widget_class, GnostrLiveCard, loading_spinner);
    gtk_widget_class_bind_template_child(widget_class, GnostrLiveCard, error_label);
    gtk_widget_class_bind_template_child(widget_class, GnostrLiveCard, hashtags_box);

    /* Signals */
    signals[SIGNAL_WATCH_LIVE] = g_signal_new(
        "watch-live",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);

    signals[SIGNAL_SET_REMINDER] = g_signal_new(
        "set-reminder",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);

    signals[SIGNAL_PROFILE_CLICKED] = g_signal_new(
        "profile-clicked",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    /* CSS name */
    gtk_widget_class_set_css_name(widget_class, "live-card");

    /* Layout manager */
    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gnostr_live_card_init(GnostrLiveCard *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));

    self->activity = NULL;
    self->is_compact = FALSE;
    self->image_cancellable = NULL;

    /* Connect action button */
    g_signal_connect(self->action_button, "clicked",
                     G_CALLBACK(on_action_button_clicked), self);

    /* Initial state - hide content, show loading */
    gtk_widget_set_visible(GTK_WIDGET(self->content_box), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->loading_spinner), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->error_label), FALSE);
}

GnostrLiveCard *
gnostr_live_card_new(void)
{
    return g_object_new(GNOSTR_TYPE_LIVE_CARD, NULL);
}

void
gnostr_live_card_set_activity(GnostrLiveCard *self,
                               const GnostrLiveActivity *activity)
{
    g_return_if_fail(GNOSTR_IS_LIVE_CARD(self));

    /* Free previous activity */
    if (self->activity) {
        gnostr_live_activity_free(self->activity);
        self->activity = NULL;
    }

    /* Cancel any pending image load */
    if (self->image_cancellable) {
        g_cancellable_cancel(self->image_cancellable);
        g_clear_object(&self->image_cancellable);
    }

    /* Copy new activity */
    if (activity) {
        self->activity = gnostr_live_activity_copy(activity);
    }

    /* Update UI */
    update_display(self);
}

const GnostrLiveActivity *
gnostr_live_card_get_activity(GnostrLiveCard *self)
{
    g_return_val_if_fail(GNOSTR_IS_LIVE_CARD(self), NULL);
    return self->activity;
}

void
gnostr_live_card_set_loading(GnostrLiveCard *self, gboolean loading)
{
    g_return_if_fail(GNOSTR_IS_LIVE_CARD(self));

    gtk_widget_set_visible(GTK_WIDGET(self->loading_spinner), loading);
    if (loading) {
        gtk_spinner_start(self->loading_spinner);
        gtk_widget_set_visible(GTK_WIDGET(self->content_box), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(self->error_label), FALSE);
    } else {
        gtk_spinner_stop(self->loading_spinner);
    }
}

void
gnostr_live_card_set_error(GnostrLiveCard *self, const char *error_message)
{
    g_return_if_fail(GNOSTR_IS_LIVE_CARD(self));

    if (error_message && *error_message) {
        gtk_label_set_text(self->error_label, error_message);
        gtk_widget_set_visible(GTK_WIDGET(self->error_label), TRUE);
        gtk_widget_set_visible(GTK_WIDGET(self->content_box), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(self->loading_spinner), FALSE);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->error_label), FALSE);
    }
}

void
gnostr_live_card_set_compact(GnostrLiveCard *self, gboolean compact)
{
    g_return_if_fail(GNOSTR_IS_LIVE_CARD(self));

    if (self->is_compact == compact) return;

    self->is_compact = compact;

    if (compact) {
        gtk_widget_add_css_class(GTK_WIDGET(self), "compact");
    } else {
        gtk_widget_remove_css_class(GTK_WIDGET(self), "compact");
    }

    /* Adjust summary visibility in compact mode */
    gtk_widget_set_visible(GTK_WIDGET(self->summary_label), !compact);
}

void
gnostr_live_card_update_participant_info(GnostrLiveCard *self,
                                          const char *pubkey_hex,
                                          const char *display_name,
                                          const char *avatar_url)
{
    g_return_if_fail(GNOSTR_IS_LIVE_CARD(self));
    g_return_if_fail(pubkey_hex != NULL);

    if (!self->activity || !self->activity->participants) return;

    /* Find and update participant */
    for (gsize i = 0; i < self->activity->participant_count; i++) {
        GnostrLiveParticipant *p = self->activity->participants[i];
        if (p && p->pubkey_hex && g_strcmp0(p->pubkey_hex, pubkey_hex) == 0) {
            g_free(p->display_name);
            g_free(p->avatar_url);
            p->display_name = g_strdup(display_name);
            p->avatar_url = g_strdup(avatar_url);
            break;
        }
    }

    /* Refresh speakers display */
    update_speakers(self);
}

const char *
gnostr_live_card_get_event_id(GnostrLiveCard *self)
{
    g_return_val_if_fail(GNOSTR_IS_LIVE_CARD(self), NULL);
    return self->activity ? self->activity->event_id : NULL;
}

const char *
gnostr_live_card_get_streaming_url(GnostrLiveCard *self)
{
    g_return_val_if_fail(GNOSTR_IS_LIVE_CARD(self), NULL);
    return gnostr_live_activity_get_primary_stream(self->activity);
}

/* Internal: Update the entire display */
static void
update_display(GnostrLiveCard *self)
{
    if (!self->activity) {
        gtk_widget_set_visible(GTK_WIDGET(self->content_box), FALSE);
        return;
    }

    /* Show content, hide loading/error */
    gtk_widget_set_visible(GTK_WIDGET(self->content_box), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(self->loading_spinner), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->error_label), FALSE);

    /* Title */
    if (self->activity->title && *self->activity->title) {
        gtk_label_set_text(self->title_label, self->activity->title);
    } else {
        gtk_label_set_text(self->title_label, "Live Activity");
    }

    /* Summary */
    if (self->activity->summary && *self->activity->summary && !self->is_compact) {
        gtk_label_set_text(self->summary_label, self->activity->summary);
        gtk_widget_set_visible(GTK_WIDGET(self->summary_label), TRUE);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->summary_label), FALSE);
    }

    /* Status badge */
    update_status_badge(self);

    /* Speakers */
    update_speakers(self);

    /* Action button */
    update_action_button(self);

    /* Viewers */
    if (self->activity->current_viewers > 0) {
        g_autofree char *viewers_text = g_strdup_printf("%d watching", self->activity->current_viewers);
        gtk_label_set_text(self->viewers_label, viewers_text);
        gtk_widget_set_visible(GTK_WIDGET(self->viewers_label), TRUE);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->viewers_label), FALSE);
    }

    /* Time info */
    g_autofree char *time_text = NULL;
    if (self->activity->status == GNOSTR_LIVE_STATUS_PLANNED) {
        time_text = gnostr_live_activity_format_time_until(self->activity);
    } else {
        time_text = gnostr_live_activity_format_duration(self->activity);
    }
    if (time_text) {
        gtk_label_set_text(self->time_label, time_text);
        gtk_widget_set_visible(GTK_WIDGET(self->time_label), TRUE);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->time_label), FALSE);
    }

    /* Hashtags */
    /* First, clear existing hashtag labels */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->hashtags_box))) != NULL) {
        gtk_box_remove(self->hashtags_box, child);
    }

    if (self->activity->hashtags) {
        for (int i = 0; self->activity->hashtags[i] && i < 5; i++) {
            g_autofree char *tag_text = g_strdup_printf("#%s", self->activity->hashtags[i]);
            GtkWidget *tag_label = gtk_label_new(tag_text);
            gtk_widget_add_css_class(tag_label, "live-hashtag");
            gtk_box_append(self->hashtags_box, tag_label);
        }
        gtk_widget_set_visible(GTK_WIDGET(self->hashtags_box), TRUE);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->hashtags_box), FALSE);
    }

    /* Load cover image */
    load_cover_image(self);
}

/* Internal: Update status badge appearance */
static void
update_status_badge(GnostrLiveCard *self)
{
    if (!self->activity) {
        gtk_widget_set_visible(GTK_WIDGET(self->status_badge), FALSE);
        return;
    }

    gtk_widget_set_visible(GTK_WIDGET(self->status_badge), TRUE);

    /* Remove existing status classes */
    gtk_widget_remove_css_class(GTK_WIDGET(self->status_badge), "live-status-live");
    gtk_widget_remove_css_class(GTK_WIDGET(self->status_badge), "live-status-planned");
    gtk_widget_remove_css_class(GTK_WIDGET(self->status_badge), "live-status-ended");

    switch (self->activity->status) {
        case GNOSTR_LIVE_STATUS_LIVE:
            gtk_widget_add_css_class(GTK_WIDGET(self->status_badge), "live-status-live");
            gtk_label_set_text(self->status_label, "LIVE");
            gtk_image_set_from_icon_name(self->status_icon, "media-record-symbolic");
            break;

        case GNOSTR_LIVE_STATUS_PLANNED:
            gtk_widget_add_css_class(GTK_WIDGET(self->status_badge), "live-status-planned");
            gtk_label_set_text(self->status_label, "SCHEDULED");
            gtk_image_set_from_icon_name(self->status_icon, "alarm-symbolic");
            break;

        case GNOSTR_LIVE_STATUS_ENDED:
            gtk_widget_add_css_class(GTK_WIDGET(self->status_badge), "live-status-ended");
            gtk_label_set_text(self->status_label, "ENDED");
            gtk_image_set_from_icon_name(self->status_icon, "media-playback-stop-symbolic");
            break;

        default:
            gtk_widget_set_visible(GTK_WIDGET(self->status_badge), FALSE);
            break;
    }
}

/* Internal: Update speakers/host display */
static void
update_speakers(GnostrLiveCard *self)
{
    /* Clear existing speaker avatars */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->speakers_box))) != NULL) {
        gtk_box_remove(self->speakers_box, child);
    }

    if (!self->activity || !self->activity->participants) {
        gtk_widget_set_visible(GTK_WIDGET(self->speakers_box), FALSE);
        return;
    }

    /* Get speakers (hosts and speakers) */
    gsize speaker_count = 0;
    GnostrLiveParticipant **speakers = gnostr_live_activity_get_speakers(self->activity, &speaker_count);

    if (!speakers || speaker_count == 0) {
        /* No explicit speakers, show first few participants */
        gsize show_count = MIN(3, self->activity->participant_count);
        for (gsize i = 0; i < show_count; i++) {
            GnostrLiveParticipant *p = self->activity->participants[i];
            if (!p) continue;

            /* Create avatar placeholder */
            GtkWidget *avatar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_add_css_class(avatar, "live-speaker-avatar");
            gtk_widget_set_size_request(avatar, 24, 24);

            /* Show initials or first char of pubkey */
            const char *display = p->display_name ? p->display_name : p->pubkey_hex;
            char initial[5] = {0};
            if (display && *display) {
                g_unichar_to_utf8(g_utf8_get_char(display), initial);
            } else {
                initial[0] = '?';
            }

            GtkWidget *label = gtk_label_new(initial);
            gtk_widget_add_css_class(label, "avatar-initials");
            gtk_box_append(GTK_BOX(avatar), label);
            gtk_box_append(self->speakers_box, avatar);
        }
    } else {
        /* Show speakers */
        gsize show_count = MIN(4, speaker_count);
        for (gsize i = 0; i < show_count; i++) {
            GnostrLiveParticipant *p = speakers[i];
            if (!p) continue;

            GtkWidget *avatar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_add_css_class(avatar, "live-speaker-avatar");
            gtk_widget_set_size_request(avatar, 24, 24);

            /* Add role indicator for host */
            if (p->role && g_ascii_strcasecmp(p->role, "host") == 0) {
                gtk_widget_add_css_class(avatar, "live-host-avatar");
            }

            const char *display = p->display_name ? p->display_name : p->pubkey_hex;
            char initial[5] = {0};
            if (display && *display) {
                g_unichar_to_utf8(g_utf8_get_char(display), initial);
            } else {
                initial[0] = '?';
            }

            GtkWidget *label = gtk_label_new(initial);
            gtk_widget_add_css_class(label, "avatar-initials");
            gtk_box_append(GTK_BOX(avatar), label);
            gtk_box_append(self->speakers_box, avatar);
        }

        /* Show "+N more" if there are more speakers */
        if (speaker_count > 4) {
            g_autofree char *more_text = g_strdup_printf("+%zu", speaker_count - 4);
            GtkWidget *more_label = gtk_label_new(more_text);
            gtk_widget_add_css_class(more_label, "live-more-speakers");
            gtk_box_append(self->speakers_box, more_label);
        }

        g_free(speakers);
    }

    gtk_widget_set_visible(GTK_WIDGET(self->speakers_box), TRUE);
}

/* Internal: Update action button text and visibility */
static void
update_action_button(GnostrLiveCard *self)
{
    if (!self->activity) {
        gtk_widget_set_visible(GTK_WIDGET(self->action_button), FALSE);
        return;
    }

    gtk_widget_set_visible(GTK_WIDGET(self->action_button), TRUE);

    /* Remove existing button classes */
    gtk_widget_remove_css_class(GTK_WIDGET(self->action_button), "live-watch-button");
    gtk_widget_remove_css_class(GTK_WIDGET(self->action_button), "live-reminder-button");
    gtk_widget_remove_css_class(GTK_WIDGET(self->action_button), "live-ended-button");

    switch (self->activity->status) {
        case GNOSTR_LIVE_STATUS_LIVE:
            gtk_button_set_label(self->action_button, "Watch Live");
            gtk_widget_add_css_class(GTK_WIDGET(self->action_button), "live-watch-button");
            gtk_widget_add_css_class(GTK_WIDGET(self->action_button), "suggested-action");
            gtk_widget_set_sensitive(GTK_WIDGET(self->action_button), TRUE);
            break;

        case GNOSTR_LIVE_STATUS_PLANNED:
            gtk_button_set_label(self->action_button, "Set Reminder");
            gtk_widget_add_css_class(GTK_WIDGET(self->action_button), "live-reminder-button");
            gtk_widget_remove_css_class(GTK_WIDGET(self->action_button), "suggested-action");
            gtk_widget_set_sensitive(GTK_WIDGET(self->action_button), TRUE);
            break;

        case GNOSTR_LIVE_STATUS_ENDED:
            if (self->activity->recording_urls && self->activity->recording_urls[0]) {
                gtk_button_set_label(self->action_button, "Watch Recording");
                gtk_widget_add_css_class(GTK_WIDGET(self->action_button), "live-ended-button");
                gtk_widget_set_sensitive(GTK_WIDGET(self->action_button), TRUE);
            } else {
                gtk_button_set_label(self->action_button, "Stream Ended");
                gtk_widget_add_css_class(GTK_WIDGET(self->action_button), "live-ended-button");
                gtk_widget_set_sensitive(GTK_WIDGET(self->action_button), FALSE);
            }
            gtk_widget_remove_css_class(GTK_WIDGET(self->action_button), "suggested-action");
            break;

        default:
            gtk_widget_set_visible(GTK_WIDGET(self->action_button), FALSE);
            break;
    }
}

/* Internal: Load cover image async */
static void
load_cover_image(GnostrLiveCard *self)
{
    if (!self->activity || !self->activity->image || !*self->activity->image) {
        /* No cover image - show placeholder */
        gtk_widget_set_visible(GTK_WIDGET(self->cover_image), FALSE);
        return;
    }

    gtk_widget_set_visible(GTK_WIDGET(self->cover_image), TRUE);

    /* Cancel previous load */
    if (self->image_cancellable) {
        g_cancellable_cancel(self->image_cancellable);
        g_clear_object(&self->image_cancellable);
    }

    /* Load image from URL - gtk_picture_set_file handles async loading */
    g_autoptr(GFile) file = g_file_new_for_uri(self->activity->image);
    gtk_picture_set_file(self->cover_image, file);
}
