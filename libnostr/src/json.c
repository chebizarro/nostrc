#include "json.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-json-parse.h"
#include "security_limits_runtime.h"
#include "nostr/metrics.h"
#include "nostr_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

NostrJsonInterface * _Atomic json_interface = NULL;
/* nostrc-val0v: the lazy -1 -> getenv -> set initialization raced with itself
 * and with nostr_json_force_fallback() from worker threads. Atomic access
 * keeps it defined; concurrent initializers compute the same value. */
static _Atomic int g_json_force_fallback = -1; /* -1 = uninitialized, 0/1 set */

void nostr_set_json_interface(NostrJsonInterface *iface) {
    atomic_store_explicit(&json_interface, iface, memory_order_release);
}

void nostr_json_init(void) {
    /* Snapshot once per call: the pointer may be swapped concurrently
     * (nostrc-val0v review), so multi-load expressions like
     * "json_interface && json_interface->init" can still dereference NULL. */
    NostrJsonInterface *ji = atomic_load_explicit(&json_interface, memory_order_acquire);
    if (ji && ji->init) {
        ji->init();
    }
}

void nostr_json_cleanup(void) {
    NostrJsonInterface *ji = atomic_load_explicit(&json_interface, memory_order_acquire);
    if (ji && ji->cleanup) {
        ji->cleanup();
    }
}

void nostr_json_force_fallback(bool enable) {
    atomic_store_explicit(&g_json_force_fallback, enable ? 1 : 0, memory_order_release);
}

static inline int json_force_fallback(void) {
    int v = atomic_load_explicit(&g_json_force_fallback, memory_order_acquire);
    if (v == -1) {
        const char *e = getenv("NOSTR_JSON_FORCE_FALLBACK");
        v = (e && (*e == '1' || *e == 't' || *e == 'T' || *e == 'y' || *e == 'Y')) ? 1 : 0;
        atomic_store_explicit(&g_json_force_fallback, v, memory_order_release);
    }
    return v;
}

char *nostr_event_serialize(const NostrEvent *event) {
    if (!json_force_fallback()) {
        // Prefer compact fast path
        char *s = nostr_event_serialize_compact(event);
        if (s) return s;
    }
    // Fallback to configured backend, if any
    NostrJsonInterface *ji = atomic_load_explicit(&json_interface, memory_order_acquire);
    if (ji && ji->serialize_event) {
        return ji->serialize_event(event);
    }
    return NULL;
}

/* Internal: clear owned fields without freeing the struct itself */
static void nostr_event_clear_fields(NostrEvent *e) {
    if (!e) return;
    if (e->id) { free(e->id); e->id = NULL; }
    if (e->pubkey) { free(e->pubkey); e->pubkey = NULL; }
    if (e->tags) { nostr_tags_free(e->tags); e->tags = NULL; }
    if (e->content) { free(e->content); e->content = NULL; }
    if (e->sig) { free(e->sig); e->sig = NULL; }
}

int nostr_event_deserialize(NostrEvent *event, const char *json_str) {
    if (!event || !json_str) return -1;
    size_t in_len = strlen(json_str);
    if (in_len > (size_t)nostr_limit_max_event_size()) {
        nostr_rl_log(NLOG_WARN, "json", "event reject: oversize %zu > %lld", in_len, (long long)nostr_limit_max_event_size());
        nostr_metric_counter_add("json_event_oversize_reject", 1);
        return -1;
    }
    int compact_evt_err_code = NOSTR_JSON_OK;
    int compact_evt_err_offset = -1;
    if (!json_force_fallback()) {
        // Try compact fast path first using a stack-allocated shadow to avoid partial mutation
        NostrEvent shadow = {0};
        NostrJsonErrorInfo err = {0, -1};
        if (nostr_event_deserialize_compact(&shadow, json_str, &err)) {
            nostr_metric_counter_add("json_event_compact_ok", 1);
            // Replace fields in destination
            nostr_event_clear_fields(event);
            event->id = shadow.id; shadow.id = NULL;
            event->pubkey = shadow.pubkey; shadow.pubkey = NULL;
            event->created_at = shadow.created_at;
            event->kind = shadow.kind;
            event->tags = shadow.tags; shadow.tags = NULL;
            event->content = shadow.content; shadow.content = NULL;
            event->sig = shadow.sig; shadow.sig = NULL;
            return 0;
        } else {
            // Clean any partial allocations in shadow before falling back
            nostr_metric_counter_add("json_event_compact_fail", 1);
            compact_evt_err_code = err.code;
            compact_evt_err_offset = err.offset;
            nostr_event_clear_fields(&shadow);
        }
    }
    // Fallback to configured backend
    NostrJsonInterface *ji = atomic_load_explicit(&json_interface, memory_order_acquire);
    if (ji && ji->deserialize_event) {
        nostr_metric_counter_add("json_event_backend_used", 1);
        int rc = ji->deserialize_event(event, json_str);
        if (rc != 0 && compact_evt_err_code != NOSTR_JSON_OK) {
            nostr_rl_log(NLOG_WARN, "json", "event parse failed: compact: %s (offset %d)",
                         nostr_json_error_string(compact_evt_err_code), compact_evt_err_offset);
        }
        return rc;
    }
    if (compact_evt_err_code != NOSTR_JSON_OK) {
        nostr_rl_log(NLOG_WARN, "json", "event parse failed (no backend): %s (offset %d)",
                     nostr_json_error_string(compact_evt_err_code), compact_evt_err_offset);
    }
    return -1;
}

char *nostr_envelope_serialize(const NostrEnvelope *envelope) {
    if (!json_force_fallback()) {
        // Prefer compact fast path
        char *s = nostr_envelope_serialize_compact(envelope);
        if (s) return s;
    }
    // Fallback to configured backend, if any
    NostrJsonInterface *ji = atomic_load_explicit(&json_interface, memory_order_acquire);
    if (ji && ji->serialize_envelope) {
        return ji->serialize_envelope(envelope);
    }
    return NULL;
}

int nostr_envelope_deserialize(NostrEnvelope *envelope, const char *json) {
    if (!envelope || !json) return -1;
    size_t in_len = strlen(json);
    if (in_len > (size_t)nostr_limit_max_event_size()) {
        nostr_rl_log(NLOG_WARN, "json", "envelope reject: oversize %zu > %lld", in_len, (long long)nostr_limit_max_event_size());
        nostr_metric_counter_add("json_envelope_oversize_reject", 1);
        return -1;
    }
    int compact_err_code = NOSTR_JSON_OK;
    int compact_err_offset = -1;
    if (!json_force_fallback()) {
        // Try compact fast path first
        NostrJsonErrorInfo env_err = {0, -1};
        if (nostr_envelope_deserialize_compact(envelope, json, &env_err)) {
            nostr_metric_counter_add("json_envelope_compact_ok", 1);
            return 0;
        }
        compact_err_code = env_err.code;
        compact_err_offset = env_err.offset;
    }
    // Fallback to configured backend
    NostrJsonInterface *ji = atomic_load_explicit(&json_interface, memory_order_acquire);
    if (ji && ji->deserialize_envelope) {
        nostr_metric_counter_add("json_envelope_backend_used", 1);
        int rc = ji->deserialize_envelope(envelope, json);
        if (rc != 0 && compact_err_code != NOSTR_JSON_OK) {
            /* Both compact and backend failed - log at WARN */
            nostr_rl_log(NLOG_WARN, "json", "envelope parse failed: compact: %s (offset %d)",
                         nostr_json_error_string(compact_err_code), compact_err_offset);
        }
        return rc;
    }
    /* No backend available - log compact failure if any */
    if (compact_err_code != NOSTR_JSON_OK) {
        nostr_rl_log(NLOG_WARN, "json", "envelope parse failed (no backend): %s (offset %d)",
                     nostr_json_error_string(compact_err_code), compact_err_offset);
    }
    return -1;
}

char *nostr_filter_serialize(const NostrFilter *filter) {
    if (!json_force_fallback()) {
        // Prefer compact fast path
        char *s = nostr_filter_serialize_compact(filter);
        if (s) return s;
    }
    // Fallback to configured backend, if any
    NostrJsonInterface *ji = atomic_load_explicit(&json_interface, memory_order_acquire);
    if (ji && ji->serialize_filter) {
        return ji->serialize_filter(filter);
    }
    return NULL;
}

int nostr_filter_deserialize(NostrFilter *filter, const char *json) {
    if (!filter || !json) return -1;
    size_t in_len = strlen(json);
    if (in_len > (size_t)nostr_limit_max_event_size()) {
        nostr_rl_log(NLOG_WARN, "json", "filter reject: oversize %zu > %lld", in_len, (long long)nostr_limit_max_event_size());
        nostr_metric_counter_add("json_filter_oversize_reject", 1);
        return -1;
    }
    int compact_filt_err_code = NOSTR_JSON_OK;
    int compact_filt_err_offset = -1;
    if (!json_force_fallback()) {
        // Try compact fast path first
        NostrJsonErrorInfo filt_err = {0, -1};
        if (nostr_filter_deserialize_compact(filter, json, &filt_err)) {
            nostr_metric_counter_add("json_filter_compact_ok", 1);
            return 0;
        }
        compact_filt_err_code = filt_err.code;
        compact_filt_err_offset = filt_err.offset;
    }
    // Fallback to configured backend
    NostrJsonInterface *ji = atomic_load_explicit(&json_interface, memory_order_acquire);
    if (ji && ji->deserialize_filter) {
        nostr_metric_counter_add("json_filter_backend_used", 1);
        int rc = ji->deserialize_filter(filter, json);
        if (rc != 0 && compact_filt_err_code != NOSTR_JSON_OK) {
            nostr_rl_log(NLOG_WARN, "json", "filter parse failed: compact: %s (offset %d)",
                         nostr_json_error_string(compact_filt_err_code), compact_filt_err_offset);
        }
        return rc;
    }
    if (compact_filt_err_code != NOSTR_JSON_OK) {
        nostr_rl_log(NLOG_WARN, "json", "filter parse failed (no backend): %s (offset %d)",
                     nostr_json_error_string(compact_filt_err_code), compact_filt_err_offset);
    }
    return -1;
}

