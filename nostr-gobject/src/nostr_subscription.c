/**
 * SPDX-License-Identifier: MIT
 *
 * GNostrSubscription: GObject wrapper for Nostr subscription lifecycle (nostrc-wjlt)
 *
 * Provides a signal-driven interface for Nostr subscriptions:
 * - Properties with notify signals (id, active, state)
 * - Event/EOSE/Closed signals emitted on main thread
 * - Monitor thread drains core GoChannels
 * - Proper GObject reference counting and cleanup
 */

#define G_LOG_DOMAIN "gnostr-subscription"

/* Core libnostr headers FIRST to get canonical type definitions */
#include "nostr-subscription.h"   /* NostrSubscription, core API */
#include "nostr-relay.h"          /* NostrRelay core */
#include "nostr-event.h"          /* NostrEvent, nostr_event_free */
#include "nostr-filter.h"         /* NostrFilters */
#include "json.h"                 /* nostr_event_serialize */
#include "context.h"              /* GoContext */
#include "channel.h"              /* GoChannel, go_channel_try_receive */
#include "select.h"               /* go_select_timeout */
#include "error.h"                /* Error, free_error */

/* GObject wrapper headers */
#include "nostr_subscription.h"
#include "nostr_relay.h"                /* gnostr_relay_get_nip11_info */
#include "nostr_subscription_registry.h" /* relay subscription count */

/* NIP-11 relay information document */
#ifdef ENABLE_NIP11
#include "nip11.h"
#endif

#include <glib.h>
#include <string.h>

#ifdef GNOSTR_TESTING
#include "nostr_subscription_test_hooks.h"
#endif

/* Property IDs */
enum {
    PROP_0,
    PROP_ID,
    PROP_ACTIVE,
    PROP_STATE,
    PROP_LOSSLESS,
    PROP_MAX_BACKLOG_EVENTS,
    PROP_MAX_BACKLOG_BYTES,
    N_PROPERTIES
};

static GParamSpec *obj_properties[N_PROPERTIES] = { NULL, };
static guint sub_signals[GNOSTR_SUBSCRIPTION_SIGNALS_COUNT] = { 0 };

/* nostrc-mzab / nostrc-kw9r / nostrc-75o3: Max events to emit per main loop
 * iteration.  Prevents startup floods from blocking the UI — between batches
 * the main loop processes GTK redraws and input events.
 *
 * History: 5 (original, too slow -> throttle cascade -> recv_channel overflow)
 *        -> 50 (too aggressive — starves GTK rendering + nostrdb dispatch)
 *        -> 20 (balanced but slow drain at startup)
 *        -> 50 + frame-time guard (nostrc-75o3: drains fast, respects 16.6ms
 *              frame budget via g_get_monotonic_time check every 10 events) */
#define MAX_EVENTS_PER_TICK 50

/* nostrc-75o3: Frame-time budget for event drain.  After every
 * DRAIN_BATCH_CHECK events we sample g_get_monotonic_time(); if the
 * elapsed wall time exceeds DRAIN_TIME_BUDGET_US we yield back to the
 * main loop so GTK can paint and process input within its 16.6 ms frame. */
#define DRAIN_BATCH_CHECK    10
#define DRAIN_TIME_BUDGET_US 8000   /* 8 ms in microseconds */

/* nostrc-75o3: Bound event queue capacity in bounded mode.  At startup
 * 10 relays x 100 events = 1000 events backlog.  In Gnostr, NDB
 * persistence stores all events via the ingest path, so dropping oldest
 * UI-queue entries is safe there — they are already persisted and the UI
 * will catch up on the next query.
 *
 * nostrc-dha5: that holds only for such a caller. Groundhog's relay scope
 * keeps what the "event" handler receives and nothing else, and lost every
 * event of a backfill past 200. Dropping is therefore opt-in (lossless =
 * FALSE); by default nothing is dropped. */
#define EVENT_QUEUE_CAPACITY 200

struct _GNostrSubscription {
    GObject parent_instance;

    NostrSubscription *subscription;      /* core subscription pointer */
    GNostrRelay *relay;                   /* owning relay (ref held) */
    NostrFilters *owned_filters;          /* filters ownership (nostrc-aaf0) */
    GNostrSubscriptionState state;        /* lifecycle state */
    guint event_count;                    /* events received (atomic) */

    /* Monitor thread */
    GThread *monitor_thread;
    gboolean monitor_running;             /* atomic flag */

    /* Batched, ordered delivery to main thread (nostrc-mzab,
     * nostrc-qp24.10.6).  The monitor thread appends EVENT/EOSE/CLOSED
     * items to event_queue and a single coalescing idle source drains
     * them in chunks, in order. */
    GMutex event_queue_mutex;
    GPtrArray *event_queue;               /* pending SubItem*, in arrival order */
    guint queued_events;                  /* SUB_ITEM_EVENT entries in event_queue */
    gboolean event_idle_scheduled;        /* TRUE while drain idle is pending */
    gboolean lossless;                    /* never drop events (nostrc-dha5);
                                           * under event_queue_mutex */
    /* Lossless mode's hard ceiling (nostrc-5rfp), under event_queue_mutex:
     * reaching it ends the subscription with an explicit overflow CLOSED. */
    guint max_backlog_events;
    guint64 max_backlog_bytes;
    guint64 queued_bytes;                 /* JSON bytes of the queued events */
    gboolean overflowed;                  /* the ceiling ended it; nothing more is queued */
    gint core_close_sent;                 /* atomic: CLOSE sent to the relay */
};

G_DEFINE_TYPE(GNostrSubscription, gnostr_subscription, G_TYPE_OBJECT)

/* --- Internal state management --- */

static void
gnostr_subscription_set_state_internal(GNostrSubscription *self,
                                       GNostrSubscriptionState new_state)
{
    if (self->state == new_state)
        return;

    GNostrSubscriptionState old_state = self->state;
    gboolean was_active = (old_state == GNOSTR_SUBSCRIPTION_STATE_ACTIVE ||
                           old_state == GNOSTR_SUBSCRIPTION_STATE_EOSE_RECEIVED);
    gboolean is_active = (new_state == GNOSTR_SUBSCRIPTION_STATE_ACTIVE ||
                          new_state == GNOSTR_SUBSCRIPTION_STATE_EOSE_RECEIVED);

    self->state = new_state;

    /* Emit state-changed signal */
    g_signal_emit(self, sub_signals[GNOSTR_SUBSCRIPTION_SIGNAL_STATE_CHANGED], 0,
                  old_state, new_state);

    /* Notify property changes */
    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_STATE]);

    if (was_active != is_active)
        g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_ACTIVE]);
}

/* --- Ordered delivery to the main thread --- */

/* nostrc-qp24.10.6: EVENT, EOSE and CLOSED share one FIFO so they reach the
 * main loop in the order the relay sent them.  libnostr hands them over on
 * three separate channels, and they used to be delivered by separate idle
 * sources at different priorities (EOSE at DEFAULT, events at DEFAULT_IDLE),
 * so an EOSE could be emitted before stored events received ahead of it —
 * breaking every one-shot "fetch until EOSE" consumer. */
typedef enum {
    SUB_ITEM_EVENT,
    SUB_ITEM_EOSE,
    SUB_ITEM_CLOSED,
} SubItemKind;

typedef struct {
    SubItemKind kind;
    gchar *text;        /* EVENT: serialized event JSON; CLOSED: reason or NULL */
} SubItem;

static void
sub_item_free(SubItem *item)
{
    if (!item) return;
    g_free(item->text);
    g_free(item);
}

/* Emits one queued item on the main thread.  "closed" is terminal: once the
 * subscription is CLOSED (the relay's CLOSED, or gnostr_subscription_close()
 * from a handler), a later EOSE or CLOSED item must not move the state back
 * or announce a second close. */
static void
emit_item(GNostrSubscription *self, SubItem *item)
{
    switch (item->kind) {
    case SUB_ITEM_EVENT:
        __atomic_add_fetch(&self->event_count, 1, __ATOMIC_SEQ_CST);
        g_signal_emit(self, sub_signals[GNOSTR_SUBSCRIPTION_SIGNAL_EVENT], 0, item->text);
        break;
    case SUB_ITEM_EOSE:
        if (self->state == GNOSTR_SUBSCRIPTION_STATE_CLOSED)
            break;
        gnostr_subscription_set_state_internal(self, GNOSTR_SUBSCRIPTION_STATE_EOSE_RECEIVED);
        g_signal_emit(self, sub_signals[GNOSTR_SUBSCRIPTION_SIGNAL_EOSE], 0);
        break;
    case SUB_ITEM_CLOSED:
        if (self->state == GNOSTR_SUBSCRIPTION_STATE_CLOSED)
            break;
        gnostr_subscription_set_state_internal(self, GNOSTR_SUBSCRIPTION_STATE_CLOSED);
        g_signal_emit(self, sub_signals[GNOSTR_SUBSCRIPTION_SIGNAL_CLOSED], 0, item->text);
        break;
    }
}

/* nostrc-mzab / nostrc-75o3: Batched drain with adaptive frame-time
 * guard.  Emits up to MAX_EVENTS_PER_TICK items per main loop iteration,
 * but checks elapsed wall-clock time every DRAIN_BATCH_CHECK items and
 * yields early if over DRAIN_TIME_BUDGET_US, respecting GTK's 16.6 ms
 * frame budget.
 *
 * Items are popped one at a time and event_idle_scheduled stays TRUE until
 * this source sees the queue empty, so there is only ever one drain source
 * per subscription: even a handler that iterates the main context cannot
 * get a later item emitted ahead of an earlier one (nostrc-qp24.10.6).
 *
 * Uses G_PRIORITY_DEFAULT_IDLE (200) so UI painting (priority 120) and
 * input events (priority 0) always take precedence over event ingestion. */
static gboolean
drain_event_queue_on_main(gpointer data)
{
    GNostrSubscription *self = GNOSTR_SUBSCRIPTION(data);
    gint64 t_start = g_get_monotonic_time();

    for (guint emitted = 0;; emitted++) {
        g_mutex_lock(&self->event_queue_mutex);
        if (self->event_queue->len == 0) {
            self->event_idle_scheduled = FALSE;
            g_mutex_unlock(&self->event_queue_mutex);
            return G_SOURCE_REMOVE; /* destroy notify unrefs self */
        }
        if (emitted >= MAX_EVENTS_PER_TICK ||
            (emitted > 0 && emitted % DRAIN_BATCH_CHECK == 0 &&
             g_get_monotonic_time() - t_start > DRAIN_TIME_BUDGET_US)) {
            g_mutex_unlock(&self->event_queue_mutex);
            return G_SOURCE_CONTINUE; /* yield to the main loop */
        }
        SubItem *item = g_ptr_array_steal_index(self->event_queue, 0);
        if (item->kind == SUB_ITEM_EVENT) {
            self->queued_events--;
            self->queued_bytes -= strlen(item->text) + 1;
        }
        g_mutex_unlock(&self->event_queue_mutex);

        emit_item(self, item);
        sub_item_free(item);
    }
}

static void
drain_event_queue_destroy(gpointer data)
{
    g_object_unref(GNOSTR_SUBSCRIPTION(data));
}

/* Sends CLOSE for the core subscription once, whichever of the overflow
 * path (monitor thread) and gnostr_subscription_close() gets there first.
 * nostr_subscription_close() only queues the frame on the relay's write
 * channel and never blocks. */
static void
send_core_close(GNostrSubscription *self, NostrSubscription *sub)
{
    if (sub && g_atomic_int_compare_and_exchange(&self->core_close_sent, 0, 1))
        nostr_subscription_close(sub, NULL);
}

/* Appends @item to the ordered queue (monitor thread).  Returns FALSE once
 * the backlog ceiling has ended the subscription (nostrc-5rfp): @item was
 * not queued (it is freed), and the monitor must stop reading. */
static gboolean
queue_item_for_main(GNostrSubscription *self, SubItem *item)
{
    g_mutex_lock(&self->event_queue_mutex);

    /* After an overflow the queued CLOSED is the last item: whatever the
     * relay sent later is covered by it. */
    if (self->overflowed) {
        g_mutex_unlock(&self->event_queue_mutex);
        sub_item_free(item);
        return FALSE;
    }

    if (item->kind == SUB_ITEM_EVENT) {
        gsize bytes = strlen(item->text) + 1;
        /* nostrc-5rfp: lossless mode is unbounded only up to a ceiling far
         * above any legitimate backfill. A relay that outpaces the main loop
         * past it would otherwise grow the process until it is killed; the
         * subscription ends instead, with an explicit CLOSED queued behind the
         * events already waiting, so nothing is lost silently. */
        if (self->lossless &&
            (self->queued_events >= self->max_backlog_events ||
             self->queued_bytes + bytes > self->max_backlog_bytes)) {
            self->overflowed = TRUE;
            g_warning("subscription %s on %s: %u events (%" G_GUINT64_FORMAT
                      " bytes) wait for the main loop, over its limit of %u events or %"
                      G_GUINT64_FORMAT " bytes; closing it",
                      self->subscription ? nostr_subscription_get_id_const(self->subscription) : "?",
                      self->relay ? gnostr_relay_get_url(self->relay) : "?",
                      self->queued_events, self->queued_bytes,
                      self->max_backlog_events, self->max_backlog_bytes);
            sub_item_free(item);
            item = g_new0(SubItem, 1);
            item->kind = SUB_ITEM_CLOSED;
            item->text = g_strdup(GNOSTR_SUBSCRIPTION_OVERFLOW_REASON);
        }
    }

    if (item->kind == SUB_ITEM_EVENT) {
        /* nostrc-75o3: in bounded mode, drop the oldest *event* when at
         * capacity; the caller persists events elsewhere.  EOSE/CLOSED
         * markers are never dropped.
         *
         * nostrc-dha5: lossless mode queues everything. It does not stop
         * reading the core channel when the queue is long instead: libnostr
         * hands events to that channel with a non-blocking send and drops
         * them when it is full (its capacity adapts down to 256), so
         * backpressure here would only move the loss into libnostr. */
        while (!self->lossless && self->queued_events >= EVENT_QUEUE_CAPACITY) {
            guint i = 0;
            while (i < self->event_queue->len &&
                   ((SubItem *)g_ptr_array_index(self->event_queue, i))->kind != SUB_ITEM_EVENT)
                i++;
            if (i == self->event_queue->len)
                break; /* unreachable: queued_events counts events in the queue */
            SubItem *dropped = g_ptr_array_steal_index(self->event_queue, i);
            self->queued_bytes -= strlen(dropped->text) + 1;
            sub_item_free(dropped);
            self->queued_events--;
        }
        self->queued_events++;
        self->queued_bytes += strlen(item->text) + 1;
    }

    gboolean overflowed = self->overflowed;
    g_ptr_array_add(self->event_queue, item);
    if (!self->event_idle_scheduled) {
        self->event_idle_scheduled = TRUE;
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                        drain_event_queue_on_main,
                        g_object_ref(self),
                        drain_event_queue_destroy);
    }
    g_mutex_unlock(&self->event_queue_mutex);
    return !overflowed;
}

/* FALSE once the backlog ceiling ended the subscription. */
static gboolean
queue_event_for_main(GNostrSubscription *self, NostrEvent *ev)
{
    if (!ev) return TRUE;
    char *json = nostr_event_serialize(ev);
    nostr_event_free(ev);
    if (!json) return TRUE;

    SubItem *item = g_new(SubItem, 1);
    item->kind = SUB_ITEM_EVENT;
    item->text = json; /* g_free-compatible, as before */
    return queue_item_for_main(self, item);
}

/* Moves every event already in the core events channel into the queue;
 * FALSE once the backlog ceiling ended the subscription. */
static gboolean
queue_pending_events(GNostrSubscription *self, GoChannel *ch_events)
{
    void *msg = NULL;
    while (ch_events && go_channel_try_receive(ch_events, &msg) == 0) {
        if (msg && !queue_event_for_main(self, (NostrEvent *)msg))
            return FALSE;
        msg = NULL;
    }
    return TRUE;
}

/* Queues an EOSE (or CLOSED) received from the core channels.
 *
 * Ordering argument (nostrc-qp24.10.6): the relay reader thread dispatches
 * frames one at a time in wire order, so every EVENT received before this
 * EOSE was pushed onto ch_events before the EOSE was pushed onto its own
 * channel.  Having received the EOSE, those events are therefore already in
 * ch_events (or were taken earlier and are queued); draining ch_events first
 * puts all of them ahead of the marker.  An event that arrived just after
 * the EOSE may also be drained ahead of it, which is harmless: it can only
 * make the stored set look larger, never smaller. */
static gboolean
queue_eose_for_main(GNostrSubscription *self, GoChannel *ch_events)
{
    if (!queue_pending_events(self, ch_events))
        return FALSE;
    SubItem *item = g_new0(SubItem, 1);
    item->kind = SUB_ITEM_EOSE;
    return queue_item_for_main(self, item);
}

/* A CLOSED ends the core subscription: queue the events before it and any
 * pending EOSE (a relay cannot send EOSE after CLOSED for the same REQ, so a
 * queued EOSE came first), then the CLOSED itself.  Takes @reason. */
static void
queue_closed_for_main(GNostrSubscription *self, GoChannel *ch_events,
                      GoChannel *ch_eose, char *reason)
{
    gboolean open = TRUE;
    while (open && ch_eose && go_channel_try_receive(ch_eose, NULL) == 0)
        open = queue_eose_for_main(self, ch_events);
    if (open)
        open = queue_pending_events(self, ch_events);
    SubItem *item = g_new0(SubItem, 1);
    item->kind = SUB_ITEM_CLOSED;
    item->text = reason ? g_strdup(reason) : NULL;
    free(reason); /* strdup'd by nostr_subscription_dispatch_closed */
    if (open)
        queue_item_for_main(self, item);
    else
        sub_item_free(item);
}

/* --- Monitor thread --- */

/* go_select_timeout now performs a fast pre-registration check and a
 * post-registration double-check, which fixes the historical missed-wakeup
 * race that forced this monitor to poll every 1ms.  Use a blocking select
 * with a bounded timeout so cancellation via monitor_running is still seen
 * promptly even if a channel close is delayed.
 *
 * go_select picks randomly among ready cases, so the case chosen says
 * nothing about arrival order; queue_eose_for_main() and
 * queue_closed_for_main() restore it (nostrc-qp24.10.6). */
static gpointer
subscription_monitor_thread(gpointer data)
{
    GNostrSubscription *self = GNOSTR_SUBSCRIPTION(data);
    NostrSubscription *sub = self->subscription;

    GoChannel *ch_events = nostr_subscription_get_events_channel(sub);
    GoChannel *ch_eose = nostr_subscription_get_eose_channel(sub);
    GoChannel *ch_closed = nostr_subscription_get_closed_channel(sub);

    while (__atomic_load_n(&self->monitor_running, __ATOMIC_SEQ_CST)) {
        void *selected_event = NULL;
        void *selected_reason = NULL;
        GoSelectCase cases[3];
        enum { CASE_EVENTS, CASE_EOSE, CASE_CLOSED } case_kinds[3];
        size_t n_cases = 0;

        if (ch_events) {
            case_kinds[n_cases] = CASE_EVENTS;
            cases[n_cases++] = (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = ch_events,
                                                .recv_buf = &selected_event };
        }
        if (ch_eose) {
            case_kinds[n_cases] = CASE_EOSE;
            cases[n_cases++] = (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = ch_eose,
                                                .recv_buf = NULL };
        }
        if (ch_closed) {
            case_kinds[n_cases] = CASE_CLOSED;
            cases[n_cases++] = (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = ch_closed,
                                                .recv_buf = &selected_reason };
        }

        if (n_cases == 0)
            break;

        GoSelectResult sel = go_select_timeout(cases, n_cases, 250);
        if (sel.selected_case < 0)
            continue; /* timeout: re-check monitor_running */

        gboolean open = TRUE;
        switch (case_kinds[sel.selected_case]) {
        case CASE_EVENTS:
            open = queue_event_for_main(self, (NostrEvent *)selected_event);
            break;
        case CASE_EOSE:
            open = queue_eose_for_main(self, ch_events);
            break;
        case CASE_CLOSED:
            queue_closed_for_main(self, ch_events, ch_eose, (char *)selected_reason);
            goto monitor_done;
        }

        /* Drain any burst that arrived with the selected item. */
        if (open)
            open = queue_pending_events(self, ch_events);

        while (open && ch_eose && go_channel_try_receive(ch_eose, NULL) == 0)
            open = queue_eose_for_main(self, ch_events);

        if (!open) {
            /* nostrc-5rfp: the backlog ceiling ended the subscription. Stop
             * reading (libnostr's channel is bounded and drops what no one
             * reads) and ask the relay to stop sending. */
            send_core_close(self, sub);
            break;
        }

        void *reason = NULL;
        if (ch_closed && go_channel_try_receive(ch_closed, &reason) == 0) {
            queue_closed_for_main(self, ch_events, ch_eose, (char *)reason);
            break;
        }
    }

monitor_done:
    g_object_unref(self); /* Release monitor thread's ref */
    return NULL;
}

/* nostrc-xkpze: Background thread func for monitor join.
 * g_thread_join blocks until the monitor thread exits.  If called on
 * the main thread (via dispose/finalize chain during app shutdown),
 * it freezes the UI.  Same class of bug as nostrc-ws1 / nostrc-ws3. */
static void
monitor_join_thread_func(GTask *task, gpointer source G_GNUC_UNUSED,
                         gpointer task_data, GCancellable *cancel G_GNUC_UNUSED)
{
    GThread *thread = (GThread *)task_data;
    g_thread_join(thread);
    g_task_return_boolean(task, TRUE);
}

static void
stop_monitor(GNostrSubscription *self)
{
    if (!self->monitor_thread)
        return;

    /* 1. Signal the monitor thread to stop. */
    __atomic_store_n(&self->monitor_running, FALSE, __ATOMIC_SEQ_CST);

    /* 2. Close channels so try_receive returns immediately (-1) and the
     *    monitor loop cannot block.  Defense in depth: the flag alone
     *    should suffice since try_receive is non-blocking, but closing
     *    channels matches the nostrc-ws1 pattern and handles any edge
     *    case where a channel call might stall. */
    if (self->subscription) {
        GoChannel *ch;
        ch = nostr_subscription_get_events_channel(self->subscription);
        if (ch && !go_channel_is_closed(ch))
            go_channel_close(ch);
        ch = nostr_subscription_get_eose_channel(self->subscription);
        if (ch && !go_channel_is_closed(ch))
            go_channel_close(ch);
        ch = nostr_subscription_get_closed_channel(self->subscription);
        if (ch && !go_channel_is_closed(ch))
            go_channel_close(ch);
    }

    /* 3. Dispatch g_thread_join to a background thread so we never
     *    block the main thread.  The monitor thread holds its own ref
     *    on self (taken in fire), which it drops on exit, so the join
     *    completing is the only thing that matters here. */
    GThread *thread = self->monitor_thread;
    self->monitor_thread = NULL;

    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, thread, NULL);
    g_task_run_in_thread(task, monitor_join_thread_func);
    g_object_unref(task);
}

/* --- GObject boilerplate --- */

static void
gnostr_subscription_get_property(GObject    *object,
                                 guint       property_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
    GNostrSubscription *self = GNOSTR_SUBSCRIPTION(object);

    switch (property_id) {
    case PROP_ID:
        if (self->subscription)
            g_value_set_string(value, nostr_subscription_get_id_const(self->subscription));
        else
            g_value_set_string(value, NULL);
        break;
    case PROP_ACTIVE:
        g_value_set_boolean(value,
                            self->state == GNOSTR_SUBSCRIPTION_STATE_ACTIVE ||
                            self->state == GNOSTR_SUBSCRIPTION_STATE_EOSE_RECEIVED);
        break;
    case PROP_STATE:
        g_value_set_enum(value, self->state);
        break;
    case PROP_LOSSLESS:
        g_value_set_boolean(value, gnostr_subscription_get_lossless(self));
        break;
    case PROP_MAX_BACKLOG_EVENTS: {
        guint events = 0;
        gnostr_subscription_get_backlog_limit(self, &events, NULL);
        g_value_set_uint(value, events);
        break;
    }
    case PROP_MAX_BACKLOG_BYTES: {
        guint64 bytes = 0;
        gnostr_subscription_get_backlog_limit(self, NULL, &bytes);
        g_value_set_uint64(value, bytes);
        break;
    }
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
gnostr_subscription_set_property(GObject      *object,
                                 guint         property_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
    GNostrSubscription *self = GNOSTR_SUBSCRIPTION(object);

    switch (property_id) {
    case PROP_LOSSLESS:
        gnostr_subscription_set_lossless(self, g_value_get_boolean(value));
        break;
    case PROP_MAX_BACKLOG_EVENTS: {
        guint64 bytes = 0;
        gnostr_subscription_get_backlog_limit(self, NULL, &bytes);
        gnostr_subscription_set_backlog_limit(self, g_value_get_uint(value), bytes);
        break;
    }
    case PROP_MAX_BACKLOG_BYTES: {
        guint events = 0;
        gnostr_subscription_get_backlog_limit(self, &events, NULL);
        gnostr_subscription_set_backlog_limit(self, events, g_value_get_uint64(value));
        break;
    }
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
gnostr_subscription_finalize(GObject *object)
{
    GNostrSubscription *self = GNOSTR_SUBSCRIPTION(object);

    /* Stop monitor thread first */
    stop_monitor(self);

    /* Close core subscription */
    if (self->subscription) {
        nostr_subscription_close(self->subscription, NULL);
        /* nostrc-ws3: Use async cleanup to avoid blocking the main thread.
         * nostr_subscription_wait() and nostr_subscription_free() both call
         * go_wait_group_wait() which blocks until worker goroutines exit.
         * If finalize runs on the GTK main thread, this freezes the app.
         *
         * nostr_subscription_free_async() spawns a background thread that
         * handles the blocking wait. We abandon the handle since we don't
         * need to track completion — the background thread will free
         * everything including the filters (which the subscription borrows).
         *
         * NOTE: owned_filters must be freed by the async cleanup thread
         * AFTER the subscription is fully destroyed, so we transfer ownership
         * to the async cleanup by NOT freeing them here. The subscription
         * holds a borrowed pointer to filters, so the async thread must
         * ensure filters outlive the subscription. Since we're abandoning
         * the handle, we accept that filters may leak in edge cases — this
         * is preferable to blocking the main thread or use-after-free. */
        AsyncCleanupHandle *handle = nostr_subscription_free_async(self->subscription, 0);
        if (handle) {
            nostr_subscription_cleanup_abandon(handle);
        }
        self->subscription = NULL;

        /* Transfer filter ownership to async cleanup — do NOT free here.
         * The async cleanup thread will handle filter lifetime. In practice,
         * filters are typically owned by the caller (e.g., GNostrPool) and
         * outlive the subscription anyway. */
        self->owned_filters = NULL;
    } else if (self->owned_filters) {
        /* No subscription to clean up — safe to free filters synchronously */
        nostr_filters_free(self->owned_filters);
        self->owned_filters = NULL;
    }

    /* nostrc-mzab: Discard any queued events not yet delivered.
     * The idle source (if pending) holds a ref, so finalize only runs
     * after the source is removed — the queue should already be empty.
     * This is a safety net for edge cases. */
    g_mutex_lock(&self->event_queue_mutex);
    if (self->event_queue) {
        for (guint i = 0; i < self->event_queue->len; i++)
            sub_item_free(g_ptr_array_index(self->event_queue, i));
        g_ptr_array_free(self->event_queue, TRUE);
        self->event_queue = NULL;
    }
    g_mutex_unlock(&self->event_queue_mutex);
    g_mutex_clear(&self->event_queue_mutex);

    /* Release relay reference */
    g_clear_object(&self->relay);

    G_OBJECT_CLASS(gnostr_subscription_parent_class)->finalize(object);
}

static void
gnostr_subscription_class_init(GNostrSubscriptionClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->get_property = gnostr_subscription_get_property;
    object_class->set_property = gnostr_subscription_set_property;
    object_class->finalize = gnostr_subscription_finalize;

    /**
     * GNostrSubscription:id:
     *
     * The subscription ID assigned by the core library. Read-only.
     */
    obj_properties[PROP_ID] =
        g_param_spec_string("id",
                            "ID",
                            "Subscription ID",
                            NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    /**
     * GNostrSubscription:active:
     *
     * Whether the subscription is currently active (live). Read-only.
     * Derived from state: TRUE when ACTIVE or EOSE_RECEIVED.
     */
    obj_properties[PROP_ACTIVE] =
        g_param_spec_boolean("active",
                             "Active",
                             "Whether the subscription is active",
                             FALSE,
                             G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                             G_PARAM_STATIC_STRINGS);

    /**
     * GNostrSubscription:state:
     *
     * The current lifecycle state of the subscription. Read-only.
     */
    obj_properties[PROP_STATE] =
        g_param_spec_enum("state",
                          "State",
                          "Current lifecycle state",
                          GNOSTR_TYPE_SUBSCRIPTION_STATE,
                          GNOSTR_SUBSCRIPTION_STATE_PENDING,
                          G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

    /**
     * GNostrSubscription:lossless:
     *
     * Whether every event reaches the "event" signal (the default), or the
     * queue in front of the main loop is bounded to 200 events and drops the
     * oldest when full. See gnostr_subscription_set_lossless().
     *
     * Since: 2.1
     */
    obj_properties[PROP_LOSSLESS] =
        g_param_spec_boolean("lossless",
                             "Lossless",
                             "Deliver every event instead of bounding the queue",
                             TRUE,
                             G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                             G_PARAM_STATIC_STRINGS);

    /**
     * GNostrSubscription:max-backlog-events:
     *
     * Lossless mode's ceiling on events waiting for the main loop; 0 sets
     * the default. See gnostr_subscription_set_backlog_limit().
     *
     * Since: 2.2
     */
    obj_properties[PROP_MAX_BACKLOG_EVENTS] =
        g_param_spec_uint("max-backlog-events",
                          "Max backlog events",
                          "Events that may wait for the main loop before the subscription closes",
                          0, G_MAXUINT, GNOSTR_SUBSCRIPTION_DEFAULT_MAX_BACKLOG_EVENTS,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

    /**
     * GNostrSubscription:max-backlog-bytes:
     *
     * Lossless mode's ceiling on the serialized bytes of the events waiting
     * for the main loop; 0 sets the default. See
     * gnostr_subscription_set_backlog_limit().
     *
     * Since: 2.2
     */
    obj_properties[PROP_MAX_BACKLOG_BYTES] =
        g_param_spec_uint64("max-backlog-bytes",
                            "Max backlog bytes",
                            "Event bytes that may wait for the main loop before the subscription closes",
                            0, G_MAXUINT64, GNOSTR_SUBSCRIPTION_DEFAULT_MAX_BACKLOG_BYTES,
                            G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                            G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPERTIES, obj_properties);

    /* Signals */

    /**
     * GNostrSubscription::event:
     * @self: the subscription
     * @event_json: the event as a JSON string
     *
     * Emitted when an event is received from the relay.
     * The event is serialized as JSON for safe cross-thread delivery.
     */
    sub_signals[GNOSTR_SUBSCRIPTION_SIGNAL_EVENT] =
        g_signal_new("event",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1, G_TYPE_STRING);

    /**
     * GNostrSubscription::eose:
     * @self: the subscription
     *
     * Emitted when End of Stored Events is received from the relay.
     * After EOSE, only new real-time events will arrive.
     */
    sub_signals[GNOSTR_SUBSCRIPTION_SIGNAL_EOSE] =
        g_signal_new("eose",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 0);

    /**
     * GNostrSubscription::closed:
     * @self: the subscription
     * @reason: (nullable): the close reason from relay, or %NULL
     *
     * Emitted when the subscription is closed, either by the client
     * or by the relay (via CLOSED message).
     */
    sub_signals[GNOSTR_SUBSCRIPTION_SIGNAL_CLOSED] =
        g_signal_new("closed",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1, G_TYPE_STRING);

    /**
     * GNostrSubscription::state-changed:
     * @self: the subscription
     * @old_state: the previous #GNostrSubscriptionState
     * @new_state: the new #GNostrSubscriptionState
     *
     * Emitted when the subscription lifecycle state changes.
     */
    sub_signals[GNOSTR_SUBSCRIPTION_SIGNAL_STATE_CHANGED] =
        g_signal_new("state-changed",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 2,
                     GNOSTR_TYPE_SUBSCRIPTION_STATE,
                     GNOSTR_TYPE_SUBSCRIPTION_STATE);
}

static void
gnostr_subscription_init(GNostrSubscription *self)
{
    self->subscription = NULL;
    self->relay = NULL;
    self->state = GNOSTR_SUBSCRIPTION_STATE_PENDING;
    self->event_count = 0;
    self->monitor_thread = NULL;
    self->monitor_running = FALSE;

    /* nostrc-mzab: Batched event queue */
    g_mutex_init(&self->event_queue_mutex);
    self->event_queue = g_ptr_array_new();
    self->queued_events = 0;
    self->event_idle_scheduled = FALSE;
    self->lossless = TRUE;
    self->max_backlog_events = GNOSTR_SUBSCRIPTION_DEFAULT_MAX_BACKLOG_EVENTS;
    self->max_backlog_bytes = GNOSTR_SUBSCRIPTION_DEFAULT_MAX_BACKLOG_BYTES;
    self->queued_bytes = 0;
    self->overflowed = FALSE;
    self->core_close_sent = 0;
}

/* --- Public API --- */

GNostrSubscription *
gnostr_subscription_new(GNostrRelay *relay, NostrFilters *filters)
{
    g_return_val_if_fail(GNOSTR_IS_RELAY(relay), NULL);
    g_return_val_if_fail(filters != NULL, NULL);

    NostrRelay *core_relay = gnostr_relay_get_core_relay(relay);
    g_return_val_if_fail(core_relay != NULL, NULL);

    GNostrSubscription *self = g_object_new(GNOSTR_TYPE_SUBSCRIPTION, NULL);

    self->relay = g_object_ref(relay);

    /* Take ownership of filters — the core subscription borrows the pointer,
     * so we must keep them alive for the subscription's lifetime (nostrc-aaf0). */
    self->owned_filters = filters;

    /* Prepare the core subscription (allocates channels, generates ID) */
    GoContext *bg = go_context_background();
    self->subscription = nostr_relay_prepare_subscription(core_relay, bg, filters);

    if (!self->subscription) {
        g_warning("Failed to prepare subscription on relay %s",
                  gnostr_relay_get_url(relay));
        self->owned_filters = NULL; /* don't free caller's filters on failure */
        g_clear_object(&self->relay);
        g_object_unref(self);
        return NULL;
    }

    g_debug("Created subscription %s on %s",
            nostr_subscription_get_id_const(self->subscription),
            gnostr_relay_get_url(relay));

    return self;
}

/**
 * gnostr_subscription_detach_filters:
 *
 * Detaches filter ownership so they won't be freed in finalize.
 * Used when the subscription fails to fire and the caller retains
 * filter ownership. (nostrc-aaf0, internal API)
 */
void
gnostr_subscription_detach_filters(GNostrSubscription *self)
{
    if (self) self->owned_filters = NULL;
}

gboolean
gnostr_subscription_fire(GNostrSubscription *self, GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_SUBSCRIPTION(self), FALSE);

    if (self->state != GNOSTR_SUBSCRIPTION_STATE_PENDING) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_STATE,
                            "subscription is not in PENDING state");
        return FALSE;
    }

    if (!self->subscription) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_CONNECTION_FAILED,
                            "no core subscription");
        return FALSE;
    }

#ifdef ENABLE_NIP11
    /* nostrc-23: Enforce NIP-11 relay limitations before subscribing */
    if (self->relay) {
        const GNostrRelayNip11Info *nip11 = gnostr_relay_get_nip11_info(self->relay);
        if (nip11 && nip11->limitation) {
            RelayLimitationDocument *lim = nip11->limitation;

            if (lim->auth_required) {
                g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_AUTH_REQUIRED,
                            "relay %s requires NIP-42 authentication",
                            gnostr_relay_get_url(self->relay));
                return FALSE;
            }
            if (lim->payment_required) {
                g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PAYMENT_REQUIRED,
                            "relay %s requires payment",
                            gnostr_relay_get_url(self->relay));
                return FALSE;
            }
            if (lim->max_subscriptions > 0) {
                const gchar *url = gnostr_relay_get_url(self->relay);
                GNostrSubscriptionRegistry *reg =
                    gnostr_subscription_registry_get_default();
                guint current = gnostr_subscription_registry_get_relay_subscription_count(reg, url);
                if ((int)current >= lim->max_subscriptions) {
                    g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_SUBSCRIPTION_LIMIT,
                                "relay %s max_subscriptions (%d) reached",
                                url, lim->max_subscriptions);
                    return FALSE;
                }
            }
        }
    }
#endif

    /* Send REQ to relay */
    Error *core_err = NULL;
    if (!nostr_subscription_fire(self->subscription, &core_err)) {
        gnostr_subscription_set_state_internal(self, GNOSTR_SUBSCRIPTION_STATE_ERROR);

        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_CONNECTION_FAILED,
                    "failed to fire subscription: %s",
                    core_err && core_err->message ? core_err->message : "unknown");
        if (core_err) free_error(core_err);
        return FALSE;
    }

    /* Transition to ACTIVE */
    gnostr_subscription_set_state_internal(self, GNOSTR_SUBSCRIPTION_STATE_ACTIVE);

    /* Start monitor thread to drain channels and emit signals */
    __atomic_store_n(&self->monitor_running, TRUE, __ATOMIC_SEQ_CST);
    self->monitor_thread = g_thread_new("gnostr-sub-monitor",
                                         subscription_monitor_thread,
                                         g_object_ref(self));

    g_debug("Fired subscription %s",
            nostr_subscription_get_id_const(self->subscription));

    return TRUE;
}

void
gnostr_subscription_close(GNostrSubscription *self)
{
    g_return_if_fail(GNOSTR_IS_SUBSCRIPTION(self));

    if (self->state == GNOSTR_SUBSCRIPTION_STATE_CLOSED)
        return;

    /* Stop monitor thread */
    stop_monitor(self);

    /* Close core subscription */
    send_core_close(self, self->subscription);

    gnostr_subscription_set_state_internal(self, GNOSTR_SUBSCRIPTION_STATE_CLOSED);
    g_signal_emit(self, sub_signals[GNOSTR_SUBSCRIPTION_SIGNAL_CLOSED], 0, NULL);

    g_debug("Closed subscription %s",
            self->subscription ? nostr_subscription_get_id_const(self->subscription) : "(null)");
}

/* --- Property Accessors --- */

const gchar *
gnostr_subscription_get_id(GNostrSubscription *self)
{
    g_return_val_if_fail(GNOSTR_IS_SUBSCRIPTION(self), NULL);
    if (!self->subscription) return NULL;
    return nostr_subscription_get_id_const(self->subscription);
}

gboolean
gnostr_subscription_get_active(GNostrSubscription *self)
{
    g_return_val_if_fail(GNOSTR_IS_SUBSCRIPTION(self), FALSE);
    return self->state == GNOSTR_SUBSCRIPTION_STATE_ACTIVE ||
           self->state == GNOSTR_SUBSCRIPTION_STATE_EOSE_RECEIVED;
}

GNostrSubscriptionState
gnostr_subscription_get_state(GNostrSubscription *self)
{
    g_return_val_if_fail(GNOSTR_IS_SUBSCRIPTION(self), GNOSTR_SUBSCRIPTION_STATE_ERROR);
    return self->state;
}

GNostrRelay *
gnostr_subscription_get_relay(GNostrSubscription *self)
{
    g_return_val_if_fail(GNOSTR_IS_SUBSCRIPTION(self), NULL);
    return self->relay;
}

void
gnostr_subscription_set_lossless(GNostrSubscription *self, gboolean lossless)
{
    g_return_if_fail(GNOSTR_IS_SUBSCRIPTION(self));

    lossless = !!lossless;
    g_mutex_lock(&self->event_queue_mutex);
    gboolean changed = self->lossless != lossless;
    self->lossless = lossless;
    g_mutex_unlock(&self->event_queue_mutex);
    if (changed)
        g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_LOSSLESS]);
}

gboolean
gnostr_subscription_get_lossless(GNostrSubscription *self)
{
    g_return_val_if_fail(GNOSTR_IS_SUBSCRIPTION(self), TRUE);

    g_mutex_lock(&self->event_queue_mutex);
    gboolean lossless = self->lossless;
    g_mutex_unlock(&self->event_queue_mutex);
    return lossless;
}

void
gnostr_subscription_set_backlog_limit(GNostrSubscription *self,
                                      guint max_events, guint64 max_bytes)
{
    g_return_if_fail(GNOSTR_IS_SUBSCRIPTION(self));

    if (max_events == 0)
        max_events = GNOSTR_SUBSCRIPTION_DEFAULT_MAX_BACKLOG_EVENTS;
    if (max_bytes == 0)
        max_bytes = GNOSTR_SUBSCRIPTION_DEFAULT_MAX_BACKLOG_BYTES;
    g_mutex_lock(&self->event_queue_mutex);
    gboolean events_changed = self->max_backlog_events != max_events;
    gboolean bytes_changed = self->max_backlog_bytes != max_bytes;
    self->max_backlog_events = max_events;
    self->max_backlog_bytes = max_bytes;
    g_mutex_unlock(&self->event_queue_mutex);
    if (events_changed)
        g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_MAX_BACKLOG_EVENTS]);
    if (bytes_changed)
        g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_MAX_BACKLOG_BYTES]);
}

void
gnostr_subscription_get_backlog_limit(GNostrSubscription *self,
                                      guint *max_events, guint64 *max_bytes)
{
    g_return_if_fail(GNOSTR_IS_SUBSCRIPTION(self));

    g_mutex_lock(&self->event_queue_mutex);
    if (max_events)
        *max_events = self->max_backlog_events;
    if (max_bytes)
        *max_bytes = self->max_backlog_bytes;
    g_mutex_unlock(&self->event_queue_mutex);
}

gboolean
gnostr_subscription_get_overflowed(GNostrSubscription *self)
{
    g_return_val_if_fail(GNOSTR_IS_SUBSCRIPTION(self), FALSE);

    g_mutex_lock(&self->event_queue_mutex);
    gboolean overflowed = self->overflowed;
    g_mutex_unlock(&self->event_queue_mutex);
    return overflowed;
}

#ifdef GNOSTR_TESTING
guint
gnostr_subscription_test_queued(GNostrSubscription *self, gboolean *eose_queued)
{
    g_mutex_lock(&self->event_queue_mutex);
    guint events = self->queued_events;
    gboolean eose = FALSE;
    for (guint i = 0; self->event_queue && i < self->event_queue->len; i++)
        if (((SubItem *)g_ptr_array_index(self->event_queue, i))->kind == SUB_ITEM_EOSE)
            eose = TRUE;
    g_mutex_unlock(&self->event_queue_mutex);
    if (eose_queued)
        *eose_queued = eose;
    return events;
}
#endif

guint
gnostr_subscription_get_event_count(GNostrSubscription *self)
{
    g_return_val_if_fail(GNOSTR_IS_SUBSCRIPTION(self), 0);
    return __atomic_load_n(&self->event_count, __ATOMIC_SEQ_CST);
}

NostrSubscription *
gnostr_subscription_get_core_subscription(GNostrSubscription *self)
{
    g_return_val_if_fail(GNOSTR_IS_SUBSCRIPTION(self), NULL);
    return self->subscription;
}

/* Alias used by subscription_registry — delegates to close */
void
gnostr_subscription_unsubscribe(GNostrSubscription *self)
{
    gnostr_subscription_close(self);
}

/* Stub for subscription_registry — config is registry-internal */
gconstpointer
gnostr_subscription_get_config(GNostrSubscription *self G_GNUC_UNUSED)
{
    return NULL;
}
