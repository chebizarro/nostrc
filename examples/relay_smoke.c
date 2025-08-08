#include "nostr.h"
#include "nostr_jansson.h"
#include "envelope.h"
#include "subscription.h"
#include "ticker.h"
#include "timestamp.h"
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
    fprintf(stderr, "Usage: %s [relay_url] [--timeout ms] [--limit n] [--since secs_ago] [--kinds list] [--authors list] [--raw]\n", prog);
    fprintf(stderr, "  kinds: comma-separated ints (e.g., 1,30023)\n");
    fprintf(stderr, "  authors: comma-separated hex pubkeys\n");
}

int main(int argc, char **argv) {
    const char *url = "wss://relay.damus.io";
    long timeout_ms = 20000;
    int limit = 10;
    long since_secs_ago = 3600;
    int enable_raw = 0;
    if (argc > 1 && strncmp(argv[1], "--", 2) != 0) {
        url = argv[1];
    }

    // Configure JSON backend (jansson-based)
    nostr_set_json_interface(jansson_impl);
    nostr_json_init();

    Error *err = NULL;
    GoContext *ctx = go_context_background();

    fprintf(stderr, "[relay_smoke] Connecting to %s...\n", url);
    Relay *relay = new_relay(ctx, url, &err);
    if (!relay || err) {
        fprintf(stderr, "[relay_smoke] new_relay failed: %s\n", err ? err->message : "unknown");
        if (err) free_error(err);
        return 1;
    }

    if (!relay_connect(relay, &err)) {
        fprintf(stderr, "[relay_smoke] relay_connect failed: %s\n", err ? err->message : "unknown");
        if (err) free_error(err);
        free_relay(relay);
        go_context_free(ctx);
        return 2;
    }

    if (!relay_is_connected(relay)) {
        fprintf(stderr, "[relay_smoke] relay_is_connected returned false\n");
        relay_close(relay, &err);
        if (err) { free_error(err); err = NULL; }
        free_relay(relay);
        go_context_free(ctx);
        return 3;
    }

    // Build filter and parse CLI flags
    Filter *filter = create_filter();
    int have_kinds = 0;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strncmp(arg, "--", 2) != 0) continue; // skip positional url
        if (strcmp(arg, "--timeout") == 0 && i + 1 < argc) {
            timeout_ms = strtol(argv[++i], NULL, 10);
        } else if (strcmp(arg, "--limit") == 0 && i + 1 < argc) {
            limit = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(arg, "--since") == 0 && i + 1 < argc) {
            since_secs_ago = strtol(argv[++i], NULL, 10);
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
        } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }
    if (!have_kinds) {
        int_array_add(&filter->kinds, 1); // default kind 1
    }
    filter->since = Now() - since_secs_ago;
    filter->limit = limit;

    // Optionally enable relay debug channel for concise raw summaries
    GoChannel *raw_msgs = NULL; // of char*
    if (enable_raw) {
        relay_enable_debug_raw(relay, 1);
        raw_msgs = relay_get_debug_raw_channel(relay);
    }

    // Use Subscription directly for events and lifecycle signals
    Filters filters = { .filters = filter, .count = 1, .capacity = 1 };
    Subscription *sub = relay_prepare_subscription(relay, ctx, &filters);
    if (!sub) {
        fprintf(stderr, "[relay_smoke] relay_prepare_subscription failed\n");
    } else {
        if (!subscription_fire(sub, &err) || err) {
            fprintf(stderr, "[relay_smoke] subscription_fire failed: %s\n", err ? err->message : "unknown");
            if (err) { free_error(err); err = NULL; }
        } else {
            fprintf(stderr, "[relay_smoke] Subscribing and processing events...\n");
            Ticker *timeout = create_ticker((int)timeout_ms);
        for (;;) {
            NostrEvent *ev = NULL;
            char *raw = NULL;
            GoSelectCase cases[] = {
                (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = sub->events, .value = NULL, .recv_buf = (void **)&ev },
                (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = sub->end_of_stored_events, .value = NULL, .recv_buf = NULL },
                (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = sub->closed_reason, .value = NULL, .recv_buf = (void **)&raw },
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
                    free_event(ev);
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
        subscription_unsub(sub);
        sub = NULL;
    }

    // Disable debug channel first to stop emitters before closing connection
    if (enable_raw) {
        relay_enable_debug_raw(relay, 0);
    }

    fprintf(stderr, "[relay_smoke] Closing...\n");
    relay_close(relay, &err);
    if (err) { fprintf(stderr, "[relay_smoke] relay_close error: %s\n", err->message); free_error(err); }

    // Debug channel was disabled above; nothing to drain here to avoid race with worker shutdown.

    free_filter(filter);
    free_relay(relay);
    go_context_free(ctx);

    // Cleanup JSON backend
    nostr_json_cleanup();

    fprintf(stderr, "[relay_smoke] Done.\n");
    return 0;
}
