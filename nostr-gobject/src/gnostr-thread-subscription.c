/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 gnostr contributors
 *
 * gnostr-thread-subscription.c - Reactive thread subscription manager
 *
 * nostrc-pp64 (Epic 4): Subscribes to EventBus + nostrdb for thread events.
 * Routes kind:1 (notes), kind:7 (reactions), and kind:1111 (NIP-22 comments)
 * to consumers via GObject signals.
 */

#include "gnostr-thread-subscription.h"
#include "gn-ndb-sub-dispatcher.h"
#include "storage_ndb.h"
#include "nostr_event_bus.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include <string.h>

/* Maximum events for the nostrdb subscription filter */
#define THREAD_SUB_NDB_LIMIT 200

struct _GNostrThreadSubscription {
    GObject parent_instance;

    /* Thread identification */
    char *root_event_id;                /* 64-char hex root event ID */
    GHashTable *monitored_ids;          /* event_id -> TRUE: all IDs to watch */

    /* Deduplication */
    GHashTable *seen_events;            /* event_id -> TRUE */

    /* EventBus subscriptions */
    GNostrEventBusHandle *bus_handle_kind1;    /* kind:1 notes */
    GNostrEventBusHandle *bus_handle_kind7;    /* kind:7 reactions */
    GNostrEventBusHandle *bus_handle_kind1111; /* kind:1111 NIP-22 comments */

    /* nostrdb subscription */
    uint64_t ndb_sub_id;

    /* State */
    gboolean active;
    gboolean disposed;
};

enum {
    SIGNAL_REPLY_RECEIVED,
    SIGNAL_REACTION_RECEIVED,
    SIGNAL_COMMENT_RECEIVED,
    SIGNAL_EOSE,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE(GNostrThreadSubscription, gnostr_thread_subscription, G_TYPE_OBJECT)

/* Forward declarations */
static void on_event_bus_kind1(const gchar *topic, gpointer event_data, gpointer user_data);
static void on_event_bus_kind7(const gchar *topic, gpointer event_data, gpointer user_data);
static void on_event_bus_kind1111(const gchar *topic, gpointer event_data, gpointer user_data);
static gboolean filter_thread_note(const gchar *topic, gpointer event_data, gpointer user_data);
static gboolean filter_thread_reaction(const gchar *topic, gpointer event_data, gpointer user_data);
static void on_ndb_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data);

/* ========== Helpers ========== */

/**
 * Check if a NostrEvent references any of our monitored IDs via e/E tags.
 * Iterates the event's tags directly (no JSON parsing needed).
 */
static gboolean event_references_monitored(const NostrEvent *ev, GHashTable *monitored_ids) {
    if (!ev || !monitored_ids) return FALSE;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return FALSE;

    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (!tag || nostr_tag_size(tag) < 2) continue;

        const char *key = nostr_tag_get_key(tag);
        if (!key) continue;

        /* Check for e or E tags (NIP-10 and NIP-22) */
        if (strcmp(key, "e") != 0 && strcmp(key, "E") != 0) continue;

        const char *value = nostr_tag_get_value(tag);
        if (value && strlen(value) == 64 &&
            g_hash_table_contains(monitored_ids, value)) {
            return TRUE;
        }
    }

    return FALSE;
}

/* ========== EventBus filter predicates ========== */

/**
 * Filter for kind:1 and kind:1111 events that are part of this thread.
 * Checks e-tags (including NIP-10 root/reply markers and NIP-22 uppercase E tags)
 * for references to monitored event IDs.
 * event_data is a NostrEvent* for "event::kind::*" topics.
 */
static gboolean filter_thread_note(const gchar *topic, gpointer event_data,
                                    gpointer user_data) {
    (void)topic;
    GNostrThreadSubscription *self = GNOSTR_THREAD_SUBSCRIPTION(user_data);
    NostrEvent *ev = (NostrEvent *)event_data;
    if (!ev) return FALSE;

    /* Fast path: check if event's ID itself is one we monitor (e.g., root event) */
    const char *id = ev->id;
    if (id && strlen(id) == 64) {
        if (g_hash_table_contains(self->monitored_ids, id))
            return TRUE;
        /* Check dedup early */
        if (g_hash_table_contains(self->seen_events, id))
            return FALSE;
    }

    /* Scan all e/E tags for references to monitored IDs */
    return event_references_monitored(ev, self->monitored_ids);
}

/**
 * Filter for kind:7 reactions referencing any monitored event.
 * event_data is a NostrEvent* for "event::kind::*" topics.
 */
static gboolean filter_thread_reaction(const gchar *topic, gpointer event_data,
                                        gpointer user_data) {
    (void)topic;
    GNostrThreadSubscription *self = GNOSTR_THREAD_SUBSCRIPTION(user_data);
    NostrEvent *ev = (NostrEvent *)event_data;
    if (!ev) return FALSE;

    /* Check dedup */
    const char *id = ev->id;
    if (id && strlen(id) == 64) {
        if (g_hash_table_contains(self->seen_events, id))
            return FALSE;
    }

    /* Reactions reference the target event via e-tag */
    return event_references_monitored(ev, self->monitored_ids);
}

/* ========== EventBus callbacks ========== */

static void on_event_bus_kind1(const gchar *topic, gpointer event_data,
                                gpointer user_data) {
    (void)topic;
    GNostrThreadSubscription *self = GNOSTR_THREAD_SUBSCRIPTION(user_data);
    NostrEvent *ev = (NostrEvent *)event_data;
    if (self->disposed || !ev) return;

    const char *eid = ev->id;
    if (!eid || strlen(eid) != 64) return;

    /* Deduplicate */
    if (g_hash_table_contains(self->seen_events, eid))
        return;
    g_hash_table_insert(self->seen_events, g_strdup(eid), GINT_TO_POINTER(TRUE));

    g_debug("[THREAD_SUB] Reply received: %.16s... for root %.16s...",
            eid, self->root_event_id);

    g_signal_emit(self, signals[SIGNAL_REPLY_RECEIVED], 0, ev);
}

static void on_event_bus_kind7(const gchar *topic, gpointer event_data,
                                gpointer user_data) {
    (void)topic;
    GNostrThreadSubscription *self = GNOSTR_THREAD_SUBSCRIPTION(user_data);
    NostrEvent *ev = (NostrEvent *)event_data;
    if (self->disposed || !ev) return;

    const char *eid = ev->id;
    if (!eid || strlen(eid) != 64) return;

    if (g_hash_table_contains(self->seen_events, eid))
        return;
    g_hash_table_insert(self->seen_events, g_strdup(eid), GINT_TO_POINTER(TRUE));

    g_debug("[THREAD_SUB] Reaction received: %.16s... for root %.16s...",
            eid, self->root_event_id);

    g_signal_emit(self, signals[SIGNAL_REACTION_RECEIVED], 0, ev);
}

static void on_event_bus_kind1111(const gchar *topic, gpointer event_data,
                                   gpointer user_data) {
    (void)topic;
    GNostrThreadSubscription *self = GNOSTR_THREAD_SUBSCRIPTION(user_data);
    NostrEvent *ev = (NostrEvent *)event_data;
    if (self->disposed || !ev) return;

    const char *eid = ev->id;
    if (!eid || strlen(eid) != 64) return;

    if (g_hash_table_contains(self->seen_events, eid))
        return;
    g_hash_table_insert(self->seen_events, g_strdup(eid), GINT_TO_POINTER(TRUE));

    g_debug("[THREAD_SUB] NIP-22 comment received: %.16s... for root %.16s...",
            eid, self->root_event_id);

    g_signal_emit(self, signals[SIGNAL_COMMENT_RECEIVED], 0, ev);
}

/* ========== nostrdb subscription callback ========== */

static void on_ndb_batch(uint64_t subid, const uint64_t *note_keys,
                          guint n_keys, gpointer user_data) {
    (void)subid;
    GNostrThreadSubscription *self = GNOSTR_THREAD_SUBSCRIPTION(user_data);
    if (!GNOSTR_IS_THREAD_SUBSCRIPTION(self) || !note_keys || n_keys == 0) return;
    if (self->disposed) return;

    void *txn = NULL;
    if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) return;

    for (guint i = 0; i < n_keys; i++) {
        uint64_t key = note_keys[i];
        storage_ndb_note *note = storage_ndb_get_note_ptr(txn, key);
        if (!note) continue;

        /* Get event ID */
        const unsigned char *id_bin = storage_ndb_note_id(note);
        if (!id_bin) continue;

        char id_hex[65];
        storage_ndb_hex_encode(id_bin, id_hex);

        /* Skip already-seen events */
        if (g_hash_table_contains(self->seen_events, id_hex)) continue;

        /* Get event kind */
        uint32_t kind = storage_ndb_note_kind(note);

        /* Get JSON representation via key lookup */
        char *json = NULL;
        int json_len = 0;
        if (storage_ndb_get_note_json_by_key(key, &json, &json_len) != 0 || !json)
            continue;

        /* Mark as seen */
        g_hash_table_insert(self->seen_events,
                            g_strdup(id_hex), GINT_TO_POINTER(TRUE));

        /* Emit appropriate signal based on kind */
        switch (kind) {
        case 1:
            g_signal_emit(self, signals[SIGNAL_REPLY_RECEIVED], 0, json);
            break;
        case 7:
            g_signal_emit(self, signals[SIGNAL_REACTION_RECEIVED], 0, json);
            break;
        case 1111:
            g_signal_emit(self, signals[SIGNAL_COMMENT_RECEIVED], 0, json);
            break;
        default:
            break;
        }

        free(json);
    }

    storage_ndb_end_query(txn);
}

/* ========== GObject lifecycle ========== */

static void gnostr_thread_subscription_dispose(GObject *object) {
    GNostrThreadSubscription *self = GNOSTR_THREAD_SUBSCRIPTION(object);
    self->disposed = TRUE;
    gnostr_thread_subscription_stop(self);
    G_OBJECT_CLASS(gnostr_thread_subscription_parent_class)->dispose(object);
}

static void gnostr_thread_subscription_finalize(GObject *object) {
    GNostrThreadSubscription *self = GNOSTR_THREAD_SUBSCRIPTION(object);

    g_free(self->root_event_id);
    g_clear_pointer(&self->monitored_ids, g_hash_table_unref);
    g_clear_pointer(&self->seen_events, g_hash_table_unref);

    G_OBJECT_CLASS(gnostr_thread_subscription_parent_class)->finalize(object);
}

static void gnostr_thread_subscription_class_init(GNostrThreadSubscriptionClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = gnostr_thread_subscription_dispose;
    object_class->finalize = gnostr_thread_subscription_finalize;

    /**
     * GNostrThreadSubscription::reply-received:
     * @self: the subscription
     * @event: (type gpointer): the NostrEvent*
     *
     * Emitted when a new kind:1 reply in the thread is received.
     */
    signals[SIGNAL_REPLY_RECEIVED] = g_signal_new(
        "reply-received",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_POINTER);

    /**
     * GNostrThreadSubscription::reaction-received:
     * @self: the subscription
     * @event: (type gpointer): the NostrEvent*
     *
     * Emitted when a new kind:7 reaction to a thread event is received.
     */
    signals[SIGNAL_REACTION_RECEIVED] = g_signal_new(
        "reaction-received",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_POINTER);

    /**
     * GNostrThreadSubscription::comment-received:
     * @self: the subscription
     * @event: (type gpointer): the NostrEvent*
     *
     * Emitted when a new kind:1111 NIP-22 comment in the thread is received.
     */
    signals[SIGNAL_COMMENT_RECEIVED] = g_signal_new(
        "comment-received",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_POINTER);

    /**
     * GNostrThreadSubscription::eose:
     * @self: the subscription
     *
     * Emitted when the initial batch of stored events has been delivered.
     */
    signals[SIGNAL_EOSE] = g_signal_new(
        "eose",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);
}

static void gnostr_thread_subscription_init(GNostrThreadSubscription *self) {
    self->monitored_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    self->seen_events = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    self->active = FALSE;
    self->disposed = FALSE;
}

/* ========== Public API ========== */

GNostrThreadSubscription *gnostr_thread_subscription_new(const char *root_event_id) {
    g_return_val_if_fail(root_event_id != NULL, NULL);
    g_return_val_if_fail(strlen(root_event_id) == 64, NULL);

    GNostrThreadSubscription *self = g_object_new(GNOSTR_TYPE_THREAD_SUBSCRIPTION, NULL);
    self->root_event_id = g_strdup(root_event_id);

    /* The root ID is always monitored */
    g_hash_table_insert(self->monitored_ids,
                        g_strdup(root_event_id), GINT_TO_POINTER(TRUE));

    return self;
}

void gnostr_thread_subscription_start(GNostrThreadSubscription *self) {
    g_return_if_fail(GNOSTR_IS_THREAD_SUBSCRIPTION(self));
    if (self->active) return;

    GNostrEventBus *bus = gnostr_event_bus_get_default();

    /* Subscribe to kind:1 notes with thread filter */
    self->bus_handle_kind1 = gnostr_event_bus_subscribe_filtered(
        bus, "event::kind::1",
        filter_thread_note,
        on_event_bus_kind1,
        self, NULL);

    /* Subscribe to kind:7 reactions with thread filter */
    self->bus_handle_kind7 = gnostr_event_bus_subscribe_filtered(
        bus, "event::kind::7",
        filter_thread_reaction,
        on_event_bus_kind7,
        self, NULL);

    /* Subscribe to kind:1111 NIP-22 comments with thread filter */
    self->bus_handle_kind1111 = gnostr_event_bus_subscribe_filtered(
        bus, "event::kind::1111",
        filter_thread_note,  /* same filter logic as kind:1 */
        on_event_bus_kind1111,
        self, NULL);

    /* Set up nostrdb subscription for local storage events.
     * Guard with begin_query to check if NDB is initialized. */
    void *txn_check = NULL;
    if (storage_ndb_begin_query(&txn_check, NULL) == 0 && txn_check) {
        storage_ndb_end_query(txn_check);

        char filter_json[256];
        snprintf(filter_json, sizeof(filter_json),
                 "{\"kinds\":[1,7,1111],\"#e\":[\"%s\"],\"limit\":%d}",
                 self->root_event_id, THREAD_SUB_NDB_LIMIT);

        self->ndb_sub_id = gn_ndb_subscribe(filter_json, on_ndb_batch, self, NULL);
    }

    self->active = TRUE;

    g_debug("[THREAD_SUB] Started subscription for root %.16s... "
            "(monitoring %u IDs, ndb_sub=%" G_GUINT64_FORMAT ")",
            self->root_event_id,
            g_hash_table_size(self->monitored_ids),
            (guint64)self->ndb_sub_id);
}

void gnostr_thread_subscription_stop(GNostrThreadSubscription *self) {
    g_return_if_fail(GNOSTR_IS_THREAD_SUBSCRIPTION(self));
    if (!self->active) return;

    GNostrEventBus *bus = gnostr_event_bus_get_default();

    /* Unsubscribe from EventBus */
    if (self->bus_handle_kind1) {
        gnostr_event_bus_unsubscribe(bus, self->bus_handle_kind1);
        self->bus_handle_kind1 = NULL;
    }
    if (self->bus_handle_kind7) {
        gnostr_event_bus_unsubscribe(bus, self->bus_handle_kind7);
        self->bus_handle_kind7 = NULL;
    }
    if (self->bus_handle_kind1111) {
        gnostr_event_bus_unsubscribe(bus, self->bus_handle_kind1111);
        self->bus_handle_kind1111 = NULL;
    }

    /* Unsubscribe from nostrdb */
    if (self->ndb_sub_id > 0) {
        gn_ndb_unsubscribe(self->ndb_sub_id);
        self->ndb_sub_id = 0;
    }

    self->active = FALSE;

    g_debug("[THREAD_SUB] Stopped subscription for root %.16s...",
            self->root_event_id);
}

void gnostr_thread_subscription_add_monitored_id(GNostrThreadSubscription *self,
                                                   const char *event_id) {
    g_return_if_fail(GNOSTR_IS_THREAD_SUBSCRIPTION(self));
    g_return_if_fail(event_id != NULL && strlen(event_id) == 64);

    if (!g_hash_table_contains(self->monitored_ids, event_id)) {
        g_hash_table_insert(self->monitored_ids,
                            g_strdup(event_id), GINT_TO_POINTER(TRUE));
        g_debug("[THREAD_SUB] Added monitored ID %.16s... (now %u total)",
                event_id, g_hash_table_size(self->monitored_ids));
    }
}

const char *gnostr_thread_subscription_get_root_id(GNostrThreadSubscription *self) {
    g_return_val_if_fail(GNOSTR_IS_THREAD_SUBSCRIPTION(self), NULL);
    return self->root_event_id;
}

gboolean gnostr_thread_subscription_is_active(GNostrThreadSubscription *self) {
    g_return_val_if_fail(GNOSTR_IS_THREAD_SUBSCRIPTION(self), FALSE);
    return self->active;
}

guint gnostr_thread_subscription_get_seen_count(GNostrThreadSubscription *self) {
    g_return_val_if_fail(GNOSTR_IS_THREAD_SUBSCRIPTION(self), 0);
    return g_hash_table_size(self->seen_events);
}
