/*
 * Relay performance baseline: handshake, first EVENT and real EOSE timings.
 * Usage: relay_perf_baseline [--timeout-ms N] relay_url [relay_url ...]
 * Exit 0 only when every requested relay completes; 1 on any measurement
 * failure (including no completed relays), 2 on invalid arguments.
 * No default relays: callers must explicitly choose their measurement targets.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nostr-relay.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-subscription.h"
#include "select.h"

#define MAX_RELAYS 8
#define MAX_EVENTS_PER_SUB 200
#define DEFAULT_TIMEOUT_MS 10000

typedef enum {
    CONNECT_FAILURE, COMPLETED, CLOSED, TRANSPORT_LOSS, TIMEOUT, SETUP_FAILURE
} Outcome;

static const char *outcome_names[] = {
    "connect_failure", "completed", "closed", "transport_loss", "timeout", "setup_failure"
};

typedef struct {
    const char *relay_url;
    uint64_t connect_start_ns, connect_done_ns, sub_start_ns;
    uint64_t first_event_ns, eose_ns;
    int events_received;
    int connected;
    Outcome outcome;
    char reason[512];
} RelayMetrics;

static uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* The reader reports transport loss after routing preceding frames. The
 * buffered channel retains loss even if it happens before select starts. */
static void relay_state_changed(NostrRelay *relay, NostrRelayConnectionState old_state,
                                NostrRelayConnectionState new_state, void *data)
{
    (void)relay;
    (void)old_state;
    if (new_state == NOSTR_RELAY_STATE_DISCONNECTED)
        go_channel_try_send(data, (void *)1);
}

static void release_notice(void *data)
{
    go_channel_unref(data);
}

static void record_event(RelayMetrics *rm, void *event)
{
    if (!event) return;
    if (!rm->first_event_ns) rm->first_event_ns = get_time_ns();
    rm->events_received++;
    nostr_event_free(event);
}

static void set_failure(RelayMetrics *rm, Outcome outcome, const Error *err)
{
    rm->outcome = outcome;
    snprintf(rm->reason, sizeof(rm->reason), "%s", err ? err->message : "unknown error");
}

static void benchmark_relay(RelayMetrics *rm, uint32_t timeout_ms)
{
    Error *err = NULL;
    NostrSubscription *sub = NULL;
    GoChannel *loss = go_channel_create(1);
    rm->connect_start_ns = get_time_ns();
    NostrRelay *relay = nostr_relay_new(NULL, rm->relay_url, &err);
    if (!relay || !loss) {
        set_failure(rm, SETUP_FAILURE, err);
        goto cleanup;
    }
    nostr_relay_set_auto_reconnect(relay, false);
    /* Callback ownership keeps its channel alive through in-flight calls. */
    nostr_relay_set_state_callback_full(relay, relay_state_changed,
                                      go_channel_ref(loss), release_notice);
    if (!nostr_relay_connect(relay, &err) ||
        !nostr_relay_wait_established(relay, timeout_ms, &err)) {
        set_failure(rm, CONNECT_FAILURE, err);
        goto cleanup;
    }
    rm->connect_done_ns = get_time_ns();
    rm->connected = 1;

    NostrFilter *filter = nostr_filter_new();
    NostrFilters *filters = nostr_filters_new();
    if (!filter || !filters) {
        nostr_filter_free(filter);
        nostr_filters_free(filters);
        set_failure(rm, SETUP_FAILURE, NULL);
        goto cleanup;
    }
    int kinds[] = { 1 };
    nostr_filter_set_kinds(filter, kinds, 1);
    nostr_filter_set_limit(filter, MAX_EVENTS_PER_SUB);
    bool added = nostr_filters_add(filters, filter);
    nostr_filter_free(filter); /* add moves the contents */
    if (!added) {
        nostr_filters_free(filters);
        set_failure(rm, SETUP_FAILURE, NULL);
        goto cleanup;
    }
    sub = nostr_relay_prepare_subscription(relay, NULL, filters);
    if (!sub) {
        nostr_filters_free(filters);
        set_failure(rm, SETUP_FAILURE, NULL);
        goto cleanup;
    }
    nostr_subscription_set_filters(sub, filters); /* transfer full */
    rm->sub_start_ns = get_time_ns();
    uint64_t deadline = rm->sub_start_ns + (uint64_t)timeout_ms * 1000000ULL;
    if (!nostr_subscription_fire(sub, &err)) {
        set_failure(rm, TRANSPORT_LOSS, err);
        goto cleanup;
    }

    GoChannel *events = nostr_subscription_get_events_channel(sub);
    GoChannel *eose = nostr_subscription_get_eose_channel(sub);
    GoChannel *closed = nostr_subscription_get_closed_channel(sub);
    bool lost = false;
    for (;;) {
        void *message = NULL;
        /* Check protocol boundaries before loss/deadline. A reader may enqueue
         * EOSE and immediately lose the transport; that is still a real sample. */
        if (go_channel_try_receive(closed, &message) == 0) {
            rm->outcome = CLOSED;
            snprintf(rm->reason, sizeof(rm->reason), "%s", message ? (char *)message : "");
            free(message);
            break;
        }
        if (go_channel_try_receive(eose, NULL) == 0) {
            rm->outcome = COMPLETED;
            rm->eose_ns = get_time_ns();
            break;
        }
        if (lost || go_channel_try_receive(loss, NULL) == 0) {
            rm->outcome = TRANSPORT_LOSS;
            break;
        }
        uint64_t now = get_time_ns();
        if (now >= deadline) {
            rm->outcome = TIMEOUT;
            break;
        }
        GoSelectCase cases[] = {
            { .op = GO_SELECT_RECEIVE, .chan = events, .recv_buf = &message },
            { .op = GO_SELECT_RECEIVE, .chan = eose, .recv_buf = &message },
            { .op = GO_SELECT_RECEIVE, .chan = closed, .recv_buf = &message },
            { .op = GO_SELECT_RECEIVE, .chan = loss, .recv_buf = &message },
        };
        GoSelectResult result = go_select_timeout(cases, 4, (deadline - now + 999999) / 1000000);
        if (result.selected_case < 0) continue; /* recheck boundaries, never completion */
        if (!result.ok) { lost = true; continue; }
        if (result.selected_case == 0) record_event(rm, message);
        else if (result.selected_case == 1) {
            rm->outcome = COMPLETED;
            rm->eose_ns = get_time_ns();
            break;
        } else if (result.selected_case == 2) {
            rm->outcome = CLOSED;
            snprintf(rm->reason, sizeof(rm->reason), "%s", message ? (char *)message : "");
            free(message);
            break;
        } else lost = true;
    }

cleanup:
    free_error(err);
    if (sub) {
        nostr_subscription_unsubscribe(sub);
        nostr_subscription_close(sub, NULL);
        nostr_subscription_wait(sub);
        /* EOSE and EVENT use separate channels. Drain the preceding buffered
         * events too, even when select observed the boundary first. */
        void *event = NULL;
        while (go_channel_try_receive(nostr_subscription_get_events_channel(sub), &event) == 0)
            record_event(rm, event);
        nostr_subscription_free(sub);
    }
    if (relay) {
        nostr_relay_disconnect(relay);
        nostr_relay_free(relay);
    }
    go_channel_unref(loss);
    printf("relay=%s outcome=%s events_received=%d eose_samples=%d",
           rm->relay_url, outcome_names[rm->outcome], rm->events_received, rm->eose_ns != 0);
    if (rm->connected)
        printf(" connect_ms=%.2f", (rm->connect_done_ns - rm->connect_start_ns) / 1e6);
    if (rm->first_event_ns)
        printf(" first_event_ms=%.2f", (rm->first_event_ns - rm->sub_start_ns) / 1e6);
    if (rm->eose_ns)
        printf(" eose_ms=%.2f", (rm->eose_ns - rm->sub_start_ns) / 1e6);
    if (*rm->reason) printf(" reason=%s", rm->reason);
    putchar('\n');
}

static int compare_uint64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a, vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

static void print_report(FILE *out, RelayMetrics *relays, int count)
{
    int connected = 0, samples = 0;
    double connect_ms = 0, eose_ms = 0;
    uint64_t latencies[MAX_RELAYS];
    for (int i = 0; i < count; i++) {
        if (relays[i].connected) {
            connected++;
            connect_ms += (relays[i].connect_done_ns - relays[i].connect_start_ns) / 1e6;
        }
        if (relays[i].eose_ns) {
            latencies[samples++] = relays[i].eose_ns - relays[i].sub_start_ns;
            eose_ms += latencies[samples - 1] / 1e6;
        }
    }
    fprintf(out, "relays_attempted=%d\nrelays_connected=%d\nrelays_failed=%d\neose_samples=%d\n",
            count, connected, count - samples, samples);
    if (connected) fprintf(out, "avg_connect_ms=%.2f\n", connect_ms / connected);
    /* Absence is not a zero-duration sample. Omit timings when no EOSE arrived. */
    if (!samples) return;
    qsort(latencies, (size_t)samples, sizeof(*latencies), compare_uint64);
    fprintf(out, "avg_eose_ms=%.2f\np50_eose_ms=%.2f\np95_eose_ms=%.2f\np99_eose_ms=%.2f\n",
            eose_ms / samples, latencies[samples * 50 / 100] / 1e6,
            latencies[samples * 95 / 100] / 1e6, latencies[samples * 99 / 100] / 1e6);
}

int main(int argc, char *argv[])
{
    uint32_t timeout_ms = DEFAULT_TIMEOUT_MS;
    RelayMetrics relays[MAX_RELAYS] = {0};
    int count = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--timeout-ms") == 0) {
            if (++i == argc) goto usage;
            char *end = NULL;
            errno = 0;
            unsigned long value = strtoul(argv[i], &end, 10);
            if (errno || !*argv[i] || *end || !value || value > UINT32_MAX || argv[i][0] == '-') goto usage;
            timeout_ms = (uint32_t)value;
        } else {
            if (argv[i][0] == '-' || count == MAX_RELAYS) goto usage;
            relays[count++].relay_url = argv[i];
        }
    }
    if (!count) goto usage;
    int completed = 0;
    for (int i = 0; i < count; i++) {
        benchmark_relay(&relays[i], timeout_ms);
        completed += relays[i].outcome == COMPLETED;
    }
    print_report(stdout, relays, count);
    FILE *file = fopen("baseline_metrics.txt", "w");
    if (file) {
        print_report(file, relays, count);
        fclose(file);
    } else perror("baseline_metrics.txt");
    return completed == count ? 0 : 1;

usage:
    fprintf(stderr, "Usage: %s [--timeout-ms N] relay_url [relay_url ...] (max %d)\n"
                    "Timeout bounds each handshake and subscription; expiry is failure.\n"
                    "Exit: 0 all relays completed, 1 any relay failed, 2 invalid arguments.\n",
            argv[0], MAX_RELAYS);
    return 2;
}
