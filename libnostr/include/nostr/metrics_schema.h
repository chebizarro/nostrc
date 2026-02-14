#ifndef LIBNOSTR_NOSTR_METRICS_SCHEMA_H
#define LIBNOSTR_NOSTR_METRICS_SCHEMA_H

/*
 * Metrics Schema — Formal definitions of all tracked metrics.
 *
 * Metric name strings used as keys in the metrics registry. Use these
 * constants at call sites for consistency and to enable grep-based auditing.
 *
 * Naming: <subsystem>_<measurement>[_<unit>]
 *   e.g.  sub_event_enqueued, event_dispatch_ns, relay_connected
 *
 * Three types:
 *   COUNTER   — monotonically increasing (use nostr_metric_counter_add)
 *   GAUGE     — point-in-time value      (use nostr_metric_gauge_set/inc/dec)
 *   HISTOGRAM — distribution of values   (use nostr_metric_histogram_record)
 */

/* ========================================================================
 * Counters — Monotonically increasing values
 * ======================================================================== */

/* Event flow counters */
#define METRIC_EVENTS_RECEIVED         "events_received"
#define METRIC_EVENTS_DISPATCHED       "events_dispatched"
#define METRIC_EVENTS_DROPPED          "events_dropped"

/* Subscription event queue counters */
#define METRIC_SUB_EVENT_ENQUEUED      "sub_event_enqueued"
#define METRIC_SUB_EVENT_DEQUEUED      "sub_event_dequeued"
#define METRIC_SUB_EVENT_DROP          "sub_event_drop"
#define METRIC_SUB_EVENT_DROP_NOT_LIVE "sub_event_drop_not_live"

/* Subscription lifecycle counters */
#define METRIC_SUB_CREATED             "sub_created"
#define METRIC_SUB_UNSUBSCRIBE         "sub_unsubscribe"
#define METRIC_SUB_EOSE_SIGNAL         "sub_eose_signal"

/* Channel counters */
#define METRIC_CHAN_TRY_SEND_FAILURES  "go_chan_try_send_failures"
#define METRIC_CHAN_SEND_SUCCESSES     "go_chan_send_successes"
#define METRIC_CHAN_SEND_DEPTH_SAMPLES "go_chan_send_depth_samples"
#define METRIC_CHAN_SEND_DEPTH_SUM     "go_chan_send_depth_sum"
#define METRIC_CHAN_INVALID_MAGIC      "go_chan_invalid_magic_send"

/* EventBus counters */
#define METRIC_BUS_EVENTS_EMITTED     "bus_events_emitted"
#define METRIC_BUS_CALLBACKS_INVOKED  "bus_callbacks_invoked"
#define METRIC_BUS_EVENTS_DROPPED     "bus_events_dropped"
#define METRIC_BUS_CACHE_HITS         "bus_pattern_cache_hits"
#define METRIC_BUS_CACHE_MISSES       "bus_pattern_cache_misses"

/* Queue near-capacity warnings */
#define METRIC_QUEUE_NEAR_CAPACITY    "queue_near_capacity"

/* ========================================================================
 * Gauges — Point-in-time values (can increase or decrease)
 * ======================================================================== */

/* Active subscription count */
#define METRIC_ACTIVE_SUBSCRIPTIONS    "active_subscriptions"

/* Current aggregate queue depth across all subscriptions */
#define METRIC_QUEUE_DEPTH             "queue_depth"

/* Number of relays currently connected */
#define METRIC_CONNECTED_RELAYS        "connected_relays"

/* EventBus active subscriber count */
#define METRIC_BUS_SUBSCRIBERS         "bus_subscribers"

/* NDB storage gauges (nostrc-o6w) */
#define METRIC_NDB_NOTE_COUNT          "ndb_note_count"
#define METRIC_NDB_PROFILE_COUNT       "ndb_profile_count"
#define METRIC_NDB_STORAGE_BYTES       "ndb_storage_bytes"
#define METRIC_NDB_KIND_TEXT           "ndb_kind_text"
#define METRIC_NDB_KIND_CONTACTS       "ndb_kind_contacts"
#define METRIC_NDB_KIND_DM             "ndb_kind_dm"
#define METRIC_NDB_KIND_REPOST         "ndb_kind_repost"
#define METRIC_NDB_KIND_REACTION       "ndb_kind_reaction"
#define METRIC_NDB_KIND_ZAP            "ndb_kind_zap"
#define METRIC_NDB_INGEST_COUNT        "ndb_ingest_count"
#define METRIC_NDB_INGEST_BYTES        "ndb_ingest_bytes"

/* ========================================================================
 * Histograms — Distributions (values in nanoseconds unless noted)
 * ======================================================================== */

/* Event dispatch latency: time to route an event from relay to subscription queue */
#define METRIC_DISPATCH_LATENCY_NS     "event_dispatch_ns"

/* Time to first event: from subscription creation to first event received */
#define METRIC_SUB_TTFE_NS             "sub_ttfe_ns"

/* Time to EOSE: from subscription creation to EOSE signal */
#define METRIC_SUB_EOSE_LATENCY_NS     "sub_eose_latency_ns"

/* Channel send wait time */
#define METRIC_CHAN_SEND_WAIT_NS        "go_chan_send_wait_ns"

/* Channel wakeup-to-progress latency */
#define METRIC_CHAN_WAKEUP_NS           "go_chan_send_wakeup_to_progress_ns"

/* EventBus dispatch latency */
#define METRIC_BUS_DISPATCH_NS          "bus_dispatch_ns"
#endif /* LIBNOSTR_NOSTR_METRICS_SCHEMA_H */
