#include "nostr.h"
#include "nostr-event.h"
#include "nostr-relay.h"
#include "nostr_jansson.h"
#include "nostr-envelope.h"
#include "nostr-subscription.h"
#include "nostr-filter.h"
#include "nostr-filter.h"
#include "ticker.h"
#include "nostr-timestamp.h"
#include "go.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// No ad-hoc raw reader: use relay's built-in safe debug channel instead.

// Simple real-relay smoke test:
// 1) Set JSON interface
// 2) Connect to relay (arg[1] or default)
// 3) Subscribe with an empty filter
// 4) Wait briefly
// 5) Cleanly close

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [relay_url] [--timeout ms] [--limit n] [--since epoch_secs] [--kinds list] [--authors list] [--raw] [--count] [--multi[=kinds]] [--debug-filter]\n", prog);
    fprintf(stderr, "  kinds: comma-separated ints (e.g., 1,30023)\n");
    fprintf(stderr, "  authors: comma-separated hex pubkeys\n");
    fprintf(stderr, "  --since: absolute Unix epoch seconds (e.g., 1742062112)\n");
}

int main(int argc, char **argv) {
    const char *url = "wss://relay.damus.io";
    long timeout_ms = 20000;
    int limit = 10;
    long since_abs = -1;     // absolute epoch seconds
    int enable_raw = 0;
    int do_count = 0;
    int do_multi = 0;
    int debug_filter = 0;
    IntArray multi_kinds = {0};
    if (argc > 1 && strncmp(argv[1], "--", 2) != 0) {
        url = argv[1];
    }

    // Configure JSON backend (jansson-based)
    nostr_set_json_interface(jansson_impl);
    nostr_json_init();

    Error *err = NULL;
    GoContext *ctx = go_context_background();

    fprintf(stderr, "[relay_smoke] Connecting to %s...\n", url);
    NostrRelay *relay = nostr_relay_new(ctx, url, &err);
    if (!relay || err) {
        fprintf(stderr, "[relay_smoke] nostr_relay_new failed: %s\n", err ? err->message : "unknown");
        if (err) free_error(err);
        return 1;
    }

    if (!nostr_relay_connect(relay, &err)) {
        fprintf(stderr, "[relay_smoke] nostr_relay_connect failed: %s\n", err ? err->message : "unknown");
        if (err) { free_error(err); err = NULL; }
        nostr_relay_free(relay);
        go_context_free(ctx);
        return 2;
    }

    if (!nostr_relay_is_connected(relay)) {
        fprintf(stderr, "[relay_smoke] nostr_relay_is_connected returned false\n");
        nostr_relay_close(relay, &err);
        if (err) { free_error(err); err = NULL; }
        nostr_relay_free(relay);
        go_context_free(ctx);
        return 3;
    }

    // Build filter and parse CLI flags
    NostrFilter *filter = nostr_filter_new();
    int have_kinds = 0;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strncmp(arg, "--", 2) != 0) continue; // skip positional url
        if (strcmp(arg, "--timeout") == 0 && i + 1 < argc) {
            timeout_ms = strtol(argv[++i], NULL, 10);
        } else if (strcmp(arg, "--limit") == 0 && i + 1 < argc) {
            limit = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(arg, "--since") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --since requires an absolute epoch seconds value\n");
                print_usage(argv[0]);
                return 2;
            }
            const char *val = argv[i + 1];
            if (strncmp(val, "--", 2) == 0) {
                fprintf(stderr, "error: --since requires an absolute epoch seconds value\n");
                print_usage(argv[0]);
                return 2;
            }
            char *endp = NULL;
            long v = strtol(val, &endp, 10);
            if (val == endp || (endp && *endp != '\0')) {
                fprintf(stderr, "error: --since value must be a number (epoch seconds)\n");
                print_usage(argv[0]);
                return 2;
            }
            since_abs = v;
            i++; // consume value
        } else if (strcmp(arg, "--kinds") == 0 && i + 1 < argc) {
            char *tmp = strdup(argv[++i]);
            char *saveptr = NULL;
            for (char *tok = strtok_r(tmp, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr)) {
                int k = (int)strtol(tok, NULL, 10);
                int_array_add(&filter->kinds, k);
                have_kinds = 1;
            }
            free(tmp);
        } else if (strcmp(arg, "--authors") == 0 && i + 1 < argc) {
            char *tmp = strdup(argv[++i]);
            char *saveptr = NULL;
            for (char *tok = strtok_r(tmp, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr)) {
                string_array_add(&filter->authors, tok);
            }
            free(tmp);
        } else if (strcmp(arg, "--raw") == 0) {
            enable_raw = 1;
        } else if (strcmp(arg, "--count") == 0) {
            do_count = 1;
        } else if (strcmp(arg, "--debug-filter") == 0) {
            debug_filter = 1;
        } else if (strncmp(arg, "--multi", 7) == 0) {
            do_multi = 1;
            // Optional: --multi=kind_list
            const char *eq = strchr(arg, '=');
            if (eq && *(eq+1)) {
                char *tmp = strdup(eq+1);
                char *saveptr = NULL;
                for (char *tok = strtok_r(tmp, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr)) {
                    int k = (int)strtol(tok, NULL, 10);
                    int_array_add(&multi_kinds, k);
                }
                free(tmp);
            }
        } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }
    if (!have_kinds) {
        int_array_add(&filter->kinds, 1); // default kind 1
    }
    // Set since: use absolute epoch seconds if provided; otherwise leave unset
    if (since_abs >= 0) {
        filter->since = (NostrTimestamp)since_abs;
    }
    filter->limit = limit;

    // Optionally enable relay debug channel for concise raw summaries
    GoChannel *raw_msgs = NULL; // of char*
    if (enable_raw) {
        nostr_relay_enable_debug_raw(relay, 1);
        raw_msgs = nostr_relay_get_debug_raw_channel(relay);
    }

    // Build Filters: single or multi
    NostrFilters filters = {0};
    NostrFilter second = {0};
    if (do_multi) {
        filters.count = 2; filters.capacity = 2;
        filters.filters = (NostrFilter*)calloc(2, sizeof(NostrFilter));
        // First filter is the parsed one
        memcpy(&filters.filters[0], filter, sizeof(NostrFilter));
        // Second filter: default kinds if not specified via --multi=, else use provided
        second = (NostrFilter){0};
        if (multi_kinds.size == 0) {
            int_array_add(&second.kinds, 5); // default to kind 5 notes if none provided
        } else {
            for (int i = 0; i < multi_kinds.size; ++i) int_array_add(&second.kinds, multi_kinds.data[i]);
        }
        second.since = filter->since;
        second.limit = filter->limit;
        filters.filters[1] = second;
    } else {
        filters.filters = filter; filters.count = 1; filters.capacity = 1;
    }

    // Optional filter debug
    if (debug_filter) {
        char *fs = nostr_filter_serialize(filter);
        if (fs) {
            fprintf(stderr, "[relay_smoke] Filter JSON: %s\n", fs);
            free(fs);
        }
    }

    // Optional COUNT demo using the first filter
    if (do_count) {
        int64_t c = nostr_relay_count(relay, ctx, filter, &err);
        if (err) {
            fprintf(stderr, "[relay_smoke] COUNT error: %s\n", err->message);
            free_error(err); err = NULL;
        } else {
            printf("COUNT result: %lld\n", (long long)c);
        }
    }
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, ctx, &filters);
    if (!sub) {
        fprintf(stderr, "[relay_smoke] relay_prepare_subscription failed\n");
    } else {
        if (!nostr_subscription_fire(sub, &err) || err) {
            fprintf(stderr, "[relay_smoke] nostr_subscription_fire failed: %s\n", err ? err->message : "unknown");
            if (err) { free_error(err); err = NULL; }
        } else {
            fprintf(stderr, "[relay_smoke] Subscribing and processing events...\n");
            Ticker *timeout = create_ticker((int)timeout_ms);
        for (;;) {
            GoChannel *ev_ch = nostr_subscription_get_events_channel(sub);
            GoChannel *eose_ch = nostr_subscription_get_eose_channel(sub);
            GoChannel *closed_ch = nostr_subscription_get_closed_channel(sub);
            NostrEvent *ev = NULL;
            char *raw = NULL;
            GoSelectCase cases[] = {
                (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = ev_ch, .value = NULL, .recv_buf = (void **)&ev },
                (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = eose_ch, .value = NULL, .recv_buf = NULL },
                (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = closed_ch, .value = NULL, .recv_buf = (void **)&raw },
                (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = timeout->c, .value = NULL, .recv_buf = NULL },
                (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = raw_msgs, .value = NULL, .recv_buf = (void **)&raw },
            };
            int idx = go_select(cases, enable_raw ? 5 : 4);
            if (idx == 0) {
                if (ev) {
                    // Print a compact summary per NIP-01 fields
                    printf("EVENT kind=%d pubkey=%.8s content=%.64s id=%.8s\n",
                            ev->kind,
                            ev->pubkey ? ev->pubkey : "",
                            ev->content ? ev->content : "",
                            ev->id ? ev->id : "");
                    nostr_event_free(ev);
                }
            } else if (idx == 1) {
                // EOSE via public channel
                printf("EOSE detected\n");
            } else if (idx == 2) {
                // CLOSED via public channel
                if (raw) {
                    printf("CLOSED reason=%s\n", raw);
                }
            } else if (idx == 3) {
                // timeout ticker fired
                stop_ticker(timeout);
                timeout = NULL;
                break;
            } else if (enable_raw && idx == 4) {
                if (raw) {
                    // Debug strings are concise summaries emitted by relay
                    printf("DBG: %s\n", raw);
                    free(raw);
                }
            }
        }
            // Ensure ticker is stopped if we exited for a reason other than timeout
            if (timeout) {
                stop_ticker(timeout);
                timeout = NULL;
            }
        }
    }

    // Proactively unsubscribe to let subscription lifecycle goroutine exit cleanly.
    // Do not free here; lifecycle goroutine may still be running and will finish after context cancel.
    if (sub) {
        nostr_subscription_unsubscribe(sub);
        sub = NULL;
    }

    // Disable debug channel first to stop emitters before closing connection
    if (enable_raw) {
        nostr_relay_enable_debug_raw(relay, 0);
    }

    fprintf(stderr, "[relay_smoke] Closing...\n");
    nostr_relay_close(relay, &err);
    if (err) { fprintf(stderr, "[relay_smoke] nostr_relay_close error: %s\n", err->message); free_error(err); }

    // Debug channel was disabled above; nothing to drain here to avoid race with worker shutdown.

    // Free allocated filters
    if (do_multi) {
        // filters.filters[0] is a memcpy of filter; free both
        nostr_filter_free(&filters.filters[0]);
        nostr_filter_free(&filters.filters[1]);
        free(filters.filters);
        nostr_filter_free(filter); // original allocated
    } else {
        nostr_filter_free(filter);
    }
    nostr_relay_free(relay);
    go_context_free(ctx);

    // Cleanup JSON backend
    nostr_json_cleanup();

    fprintf(stderr, "[relay_smoke] Done.\n");
    return 0;
}
