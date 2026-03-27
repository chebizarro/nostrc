#define G_LOG_DOMAIN "gnostr-discover-live"

#include "page-discover-private.h"
#include "gnostr-live-card.h"
#include "../util/nip53_live.h"

#include <nostr-gobject-1.0/gn-ndb-sub-dispatcher.h>
#include <nostr-gobject-1.0/storage_ndb.h>

#include <string.h>

/* Compare function for sorting live activities by start time (ascending) */
static gint
compare_activities_by_start_time(gconstpointer a, gconstpointer b)
{
    const GnostrLiveActivity *act_a = *(const GnostrLiveActivity **)a;
    const GnostrLiveActivity *act_b = *(const GnostrLiveActivity **)b;

    gint64 time_a = act_a->starts_at > 0 ? act_a->starts_at : act_a->created_at;
    gint64 time_b = act_b->starts_at > 0 ? act_b->starts_at : act_b->created_at;

    if (time_a < time_b) return -1;
    if (time_a > time_b) return 1;
    return 0;
}

/* Compare function for sorting live activities by created_at (descending for recency) */
static gint
compare_activities_by_recency(gconstpointer a, gconstpointer b)
{
    const GnostrLiveActivity *act_a = *(const GnostrLiveActivity **)a;
    const GnostrLiveActivity *act_b = *(const GnostrLiveActivity **)b;

    if (act_a->created_at > act_b->created_at) return -1;
    if (act_a->created_at < act_b->created_at) return 1;
    return 0;
}

static gboolean
activity_exists_in_array(GPtrArray *array, const char *pubkey, const char *d_tag)
{
    if (!array || !pubkey || !d_tag)
        return FALSE;

    for (guint i = 0; i < array->len; i++) {
        GnostrLiveActivity *existing = g_ptr_array_index(array, i);
        if (existing->pubkey && existing->d_tag &&
            strcmp(existing->pubkey, pubkey) == 0 &&
            strcmp(existing->d_tag, d_tag) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static void
on_live_card_watch_live(GnostrLiveCard *card, GnostrPageDiscover *self)
{
    const GnostrLiveActivity *activity = gnostr_live_card_get_activity(card);
    if (activity && activity->event_id) {
        gnostr_page_discover_emit_watch_live_internal(self, activity->event_id);
    }
}

static void
on_live_card_profile_clicked(GnostrLiveCard *card, const char *pubkey_hex, GnostrPageDiscover *self)
{
    (void)card;
    gnostr_page_discover_emit_open_profile_internal(self, pubkey_hex);
}

static void
clear_live_flow_boxes(GnostrPageDiscover *self)
{
    if (self->live_flow_box) {
        GtkWidget *child;
        while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->live_flow_box))) != NULL) {
            gtk_flow_box_remove(self->live_flow_box, child);
        }
    }

    if (self->scheduled_flow_box) {
        GtkWidget *child;
        while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->scheduled_flow_box))) != NULL) {
            gtk_flow_box_remove(self->scheduled_flow_box, child);
        }
    }
}

static void
clear_live_activities(GnostrPageDiscover *self)
{
    clear_live_flow_boxes(self);
    g_clear_pointer(&self->live_activities, g_ptr_array_unref);
    g_clear_pointer(&self->scheduled_activities, g_ptr_array_unref);
}

static void
populate_live_activities(GnostrPageDiscover *self)
{
    if (!self->live_flow_box || !self->scheduled_flow_box)
        return;

    clear_live_flow_boxes(self);

    if (self->live_activities && self->live_activities->len > 0) {
        for (guint i = 0; i < self->live_activities->len; i++) {
            GnostrLiveActivity *activity = g_ptr_array_index(self->live_activities, i);
            GnostrLiveCard *card = gnostr_live_card_new();
            gnostr_live_card_set_activity(card, activity);
            gnostr_live_card_set_compact(card, FALSE);

            g_signal_connect(card, "watch-live", G_CALLBACK(on_live_card_watch_live), self);
            g_signal_connect(card, "profile-clicked", G_CALLBACK(on_live_card_profile_clicked), self);

            gtk_flow_box_insert(self->live_flow_box, GTK_WIDGET(card), -1);
        }
        gtk_widget_set_visible(GTK_WIDGET(self->live_now_section), TRUE);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->live_now_section), FALSE);
    }

    if (self->scheduled_activities && self->scheduled_activities->len > 0) {
        for (guint i = 0; i < self->scheduled_activities->len; i++) {
            GnostrLiveActivity *activity = g_ptr_array_index(self->scheduled_activities, i);
            GnostrLiveCard *card = gnostr_live_card_new();
            gnostr_live_card_set_activity(card, activity);
            gnostr_live_card_set_compact(card, TRUE);

            g_signal_connect(card, "watch-live", G_CALLBACK(on_live_card_watch_live), self);
            g_signal_connect(card, "profile-clicked", G_CALLBACK(on_live_card_profile_clicked), self);

            gtk_flow_box_insert(self->scheduled_flow_box, GTK_WIDGET(card), -1);
        }
        gtk_widget_set_visible(GTK_WIDGET(self->scheduled_section), TRUE);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->scheduled_section), FALSE);
    }
}

static void
update_live_content_state(GnostrPageDiscover *self)
{
    gboolean has_live = self->live_activities && self->live_activities->len > 0;
    gboolean has_scheduled = self->scheduled_activities && self->scheduled_activities->len > 0;

    if (has_live || has_scheduled) {
        gtk_stack_set_visible_child_name(self->content_stack, "live");
    } else {
        gtk_stack_set_visible_child_name(self->content_stack, "live-empty");
    }
}

static void
process_live_activity_event(GnostrPageDiscover *self, storage_ndb_note *note)
{
    if (!note)
        return;

    char *tags_json = storage_ndb_note_tags_json(note);
    if (!tags_json)
        return;

    const unsigned char *id_bin = storage_ndb_note_id(note);
    const unsigned char *pubkey_bin = storage_ndb_note_pubkey(note);
    gint64 created_at = (gint64)storage_ndb_note_created_at(note);

    char event_id_hex[65], pubkey_hex[65];
    storage_ndb_hex_encode(id_bin, event_id_hex);
    storage_ndb_hex_encode(pubkey_bin, pubkey_hex);

    GnostrLiveActivity *activity = gnostr_live_activity_parse_tags(
        tags_json, pubkey_hex, event_id_hex, created_at);

    g_free(tags_json);

    if (!activity) {
        g_message("discover: failed to parse live activity event");
        return;
    }

    if (activity->status == GNOSTR_LIVE_STATUS_ENDED) {
        gnostr_live_activity_free(activity);
        return;
    }

    GPtrArray *target_array = (activity->status == GNOSTR_LIVE_STATUS_LIVE)
        ? self->live_activities
        : self->scheduled_activities;

    if (activity_exists_in_array(target_array, activity->pubkey, activity->d_tag)) {
        gnostr_live_activity_free(activity);
        return;
    }

    GPtrArray *other_array = (activity->status == GNOSTR_LIVE_STATUS_LIVE)
        ? self->scheduled_activities
        : self->live_activities;

    if (activity_exists_in_array(other_array, activity->pubkey, activity->d_tag)) {
        gnostr_live_activity_free(activity);
        return;
    }

    if (activity->status == GNOSTR_LIVE_STATUS_LIVE) {
        g_ptr_array_add(self->live_activities, activity);
        g_message("discover: added live activity '%s' (live)", activity->title ? activity->title : "(untitled)");
    } else if (activity->status == GNOSTR_LIVE_STATUS_PLANNED) {
        g_ptr_array_add(self->scheduled_activities, activity);
        g_message("discover: added live activity '%s' (planned)", activity->title ? activity->title : "(untitled)");
    } else {
        if (activity->starts_at > (g_get_real_time() / G_USEC_PER_SEC)) {
            g_ptr_array_add(self->scheduled_activities, activity);
        } else {
            gnostr_live_activity_free(activity);
        }
    }
}

static void
on_live_activities_received(uint64_t subid,
                            const uint64_t *note_keys,
                            guint n_keys,
                            gpointer user_data)
{
    GnostrPageDiscover *self = GNOSTR_PAGE_DISCOVER(user_data);

    if (!self || n_keys == 0)
        return;

    if (self->live_sub_id == 0 || subid != self->live_sub_id)
        return;

    g_message("discover: received %u live activity events", n_keys);

    void *txn = NULL;
    if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) {
        g_warning("discover: failed to begin query for live activities");
        return;
    }

    if (!self->live_activities) {
        self->live_activities = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_live_activity_free);
    }
    if (!self->scheduled_activities) {
        self->scheduled_activities = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_live_activity_free);
    }

    for (guint i = 0; i < n_keys; i++) {
        storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_keys[i]);
        if (!note)
            continue;

        uint32_t kind = storage_ndb_note_kind(note);
        if (kind != 30311)
            continue;

        process_live_activity_event(self, note);
    }

    storage_ndb_end_query(txn);

    if (self->live_activities && self->live_activities->len > 0) {
        g_ptr_array_sort(self->live_activities, compare_activities_by_recency);
    }
    if (self->scheduled_activities && self->scheduled_activities->len > 0) {
        g_ptr_array_sort(self->scheduled_activities, compare_activities_by_start_time);
    }

    self->live_loaded = TRUE;

    if (self->live_loading_spinner) {
        gtk_spinner_stop(self->live_loading_spinner);
    }

    populate_live_activities(self);
    update_live_content_state(self);
}

void
gnostr_page_discover_live_present(GnostrPageDiscover *self)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));

    if (!self->live_loaded) {
        gnostr_page_discover_live_load_internal(self);
    } else {
        update_live_content_state(self);
    }
}

void
gnostr_page_discover_live_reload(GnostrPageDiscover *self)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));

    self->live_loaded = FALSE;
    gnostr_page_discover_live_load_internal(self);
}

void
gnostr_page_discover_live_load_internal(GnostrPageDiscover *self)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));

    if (self->live_cancellable) {
        g_cancellable_cancel(self->live_cancellable);
        g_clear_object(&self->live_cancellable);
    }

    if (self->live_sub_id) {
        gn_ndb_unsubscribe(self->live_sub_id);
        self->live_sub_id = 0;
    }

    if (self->live_loading_spinner) {
        gtk_spinner_start(self->live_loading_spinner);
    }
    gtk_stack_set_visible_child_name(self->content_stack, "live-loading");

    self->live_cancellable = g_cancellable_new();

    clear_live_activities(self);

    self->live_activities = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_live_activity_free);
    self->scheduled_activities = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_live_activity_free);

    const char *filter_json = "[{\"kinds\":[30311],\"limit\":100}]";

    self->live_sub_id = gn_ndb_subscribe(
        filter_json,
        on_live_activities_received,
        self,
        NULL
    );

    if (self->live_sub_id == 0) {
        g_warning("discover: failed to subscribe to live activities");

        if (self->live_loading_spinner) {
            gtk_spinner_stop(self->live_loading_spinner);
        }
        self->live_loaded = TRUE;
        update_live_content_state(self);
        return;
    }

    g_message("discover: subscribed to live activities (subid=%" G_GUINT64_FORMAT ")",
            (guint64)self->live_sub_id);

    void *txn = NULL;
    if (storage_ndb_begin_query(&txn, NULL) == 0 && txn) {
        char **results = NULL;
        int count = 0;

        if (storage_ndb_query(txn, filter_json, &results, &count, NULL) == 0 && results && count > 0) {
            g_message("discover: found %d existing live activity events", count);

            for (int i = 0; i < count; i++) {
                if (results[i]) {
                    GnostrLiveActivity *activity = gnostr_live_activity_parse(results[i]);
                    if (activity) {
                        if (activity->status == GNOSTR_LIVE_STATUS_ENDED) {
                            gnostr_live_activity_free(activity);
                            continue;
                        }

                        GPtrArray *target = (activity->status == GNOSTR_LIVE_STATUS_LIVE)
                            ? self->live_activities
                            : self->scheduled_activities;

                        if (!activity_exists_in_array(target, activity->pubkey, activity->d_tag) &&
                            !activity_exists_in_array(
                                (target == self->live_activities) ? self->scheduled_activities : self->live_activities,
                                activity->pubkey, activity->d_tag)) {

                            if (activity->status == GNOSTR_LIVE_STATUS_LIVE) {
                                g_ptr_array_add(self->live_activities, activity);
                            } else if (activity->status == GNOSTR_LIVE_STATUS_PLANNED ||
                                       activity->starts_at > (g_get_real_time() / G_USEC_PER_SEC)) {
                                g_ptr_array_add(self->scheduled_activities, activity);
                            } else {
                                gnostr_live_activity_free(activity);
                            }
                        } else {
                            gnostr_live_activity_free(activity);
                        }
                    }
                }
            }

            storage_ndb_free_results(results, count);
        }

        storage_ndb_end_query(txn);

        if (self->live_activities && self->live_activities->len > 0) {
            g_ptr_array_sort(self->live_activities, compare_activities_by_recency);
        }
        if (self->scheduled_activities && self->scheduled_activities->len > 0) {
            g_ptr_array_sort(self->scheduled_activities, compare_activities_by_start_time);
        }

        if ((self->live_activities && self->live_activities->len > 0) ||
            (self->scheduled_activities && self->scheduled_activities->len > 0)) {

            self->live_loaded = TRUE;
            if (self->live_loading_spinner) {
                gtk_spinner_stop(self->live_loading_spinner);
            }
            populate_live_activities(self);
            update_live_content_state(self);
            return;
        }
    }

    self->live_loaded = TRUE;
    if (self->live_loading_spinner) {
        gtk_spinner_stop(self->live_loading_spinner);
    }
    update_live_content_state(self);
}

void
gnostr_page_discover_live_dispose(GnostrPageDiscover *self)
{
    g_return_if_fail(GNOSTR_IS_PAGE_DISCOVER(self));

    if (self->live_cancellable) {
        g_cancellable_cancel(self->live_cancellable);
        g_clear_object(&self->live_cancellable);
    }

    if (self->live_sub_id) {
        gn_ndb_unsubscribe(self->live_sub_id);
        self->live_sub_id = 0;
    }

    clear_live_activities(self);
}
