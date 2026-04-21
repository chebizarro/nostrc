/*
 * relay_fetch.c — Fetch replaceable Nostr events from relays.
 *
 * Each public function creates a short-lived pool, registers a callback
 * to capture the first matching event, waits up to 3 s, then tears down.
 *
 * Signaling uses GoChannel (capacity 1): the callback pushes the result
 * string through go_channel_try_send, and the caller receives it via
 * go_select_timeout.  Only the first matching event wins; subsequent
 * pushes silently fail (channel already full).
 *
 * NOTE: The middleware API (nostr_simple_pool_set_event_middleware) does
 * not accept user_data, so a static pointer per fetch type is still
 * required to route events to the correct GoChannel.  Concurrent calls
 * to the *same* function are serialized by the channel pattern (first
 * send wins, rest are discarded).  Concurrent calls to *different*
 * functions are fully safe — they use separate globals.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <go.h>
#include "nostr_manifest.h"
#include "nostr-simple-pool.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#include <jansson.h>

/* ── Shared channel-receive helper ──────────────────────────── */

/* Wait up to timeout_ms for a string on ch.  Returns the string
 * (caller-owned) or NULL on timeout/error. */
static char *channel_receive_string(GoChannel *ch, uint64_t timeout_ms) {
    char *result = NULL;
    GoSelectCase cas = {
        .op       = GO_SELECT_RECEIVE,
        .chan      = ch,
        .value    = NULL,
        .recv_buf = (void **)&result,
    };
    GoSelectResult sr = go_select_timeout(&cas, 1, timeout_ms);
    if (sr.selected_case == 0 && sr.ok)
        return result;
    return NULL;
}

/* ── Manifest fetch (kind 30081) ────────────────────────────── */

static GoChannel *g_manifest_ch = NULL;

static void fetch_manifest_cb(NostrIncomingEvent *in) {
    if (!g_manifest_ch || !in || !in->event) return;
    if (nostr_event_get_kind(in->event) != 30081) return;
    const char *content = nostr_event_get_content(in->event);
    if (!content) return;
    char *dup = strdup(content);
    if (dup && !go_channel_try_send(g_manifest_ch, dup))
        free(dup); /* channel full — first event already captured */
}

int nh_fetch_latest_manifest_json(const char **relays, size_t num_relays,
                                  const char *author_hex,
                                  const char *namespace_name,
                                  char **out_json) {
    if (!out_json || !relays || num_relays == 0 || !author_hex) return -1;
    *out_json = NULL;

    NostrSimplePool *pool = nostr_simple_pool_new();
    if (!pool) return -1;

    for (size_t i = 0; i < num_relays; i++)
        if (relays[i] && *relays[i])
            nostr_simple_pool_ensure_relay(pool, relays[i]);

    nostr_simple_pool_set_auto_unsub_on_eose(pool, true);
    nostr_simple_pool_set_event_middleware(pool, fetch_manifest_cb);

    const char *ns = (namespace_name && *namespace_name) ? namespace_name : "personal";
    NostrFilter *f = nostr_filter_new();
    nostr_filter_add_kind(f, 30081);
    nostr_filter_add_author(f, author_hex);
    nostr_filter_tags_append(f, "d", ns, NULL);
    nostr_filter_set_limit(f, 1);
    nostr_simple_pool_query_single(pool, relays, num_relays, *f);
    nostr_filter_free(f);

    GoChannel *ch = go_channel_create(1);
    g_manifest_ch = ch;
    nostr_simple_pool_start(pool);

    char *json = channel_receive_string(ch, 3000);

    nostr_simple_pool_stop(pool);
    nostr_simple_pool_free(pool);
    g_manifest_ch = NULL;
    go_channel_close(ch);
    go_channel_unref(ch);

    if (json) { *out_json = json; return 0; }
    return -1;
}

/* ── Secrets fetch (kind 30079) ─────────────────────────────── */

static GoChannel *g_secrets_ch = NULL;

static void fetch_secrets_cb(NostrIncomingEvent *in) {
    if (!g_secrets_ch || !in || !in->event) return;
    if (nostr_event_get_kind(in->event) != 30079) return;
    const char *content = nostr_event_get_content(in->event);
    if (!content) return;
    char *dup = strdup(content);
    if (dup && !go_channel_try_send(g_secrets_ch, dup))
        free(dup);
}

int nh_fetch_latest_secrets_json(const char **relays, size_t num_relays,
                                 const char *author_hex,
                                 const char *namespace_name,
                                 char **out_json) {
    if (!out_json || !relays || num_relays == 0 || !author_hex) return -1;
    *out_json = NULL;

    const char *ns = (namespace_name && *namespace_name) ? namespace_name : "personal";
    NostrSimplePool *pool = nostr_simple_pool_new();
    if (!pool) return -1;

    for (size_t i = 0; i < num_relays; i++)
        if (relays[i] && *relays[i])
            nostr_simple_pool_ensure_relay(pool, relays[i]);

    nostr_simple_pool_set_auto_unsub_on_eose(pool, true);
    nostr_simple_pool_set_event_middleware(pool, fetch_secrets_cb);

    NostrFilter *f = nostr_filter_new();
    nostr_filter_add_kind(f, 30079);
    nostr_filter_add_author(f, author_hex);
    nostr_filter_tags_append(f, "d", ns, NULL);
    nostr_filter_set_limit(f, 1);
    nostr_simple_pool_query_single(pool, relays, num_relays, *f);
    nostr_filter_free(f);

    GoChannel *ch = go_channel_create(1);
    g_secrets_ch = ch;
    nostr_simple_pool_start(pool);

    char *json = channel_receive_string(ch, 3000);

    nostr_simple_pool_stop(pool);
    nostr_simple_pool_free(pool);
    g_secrets_ch = NULL;
    go_channel_close(ch);
    go_channel_unref(ch);

    if (json) { *out_json = json; return 0; }
    return -1;
}

/* ── Profile relays fetch (kind 30078) ──────────────────────── */

static GoChannel *g_relays_ch = NULL;

static void fetch_relays_cb(NostrIncomingEvent *in) {
    if (!g_relays_ch || !in || !in->event) return;
    if (nostr_event_get_kind(in->event) != 30078) return;
    const char *content = nostr_event_get_content(in->event);
    if (!content) return;
    char *dup = strdup(content);
    if (dup && !go_channel_try_send(g_relays_ch, dup))
        free(dup);
}

int nh_fetch_profile_relays(const char **relays, size_t num_relays,
                            const char *author_hex,
                            char ***out_relays, size_t *out_count) {
    if (!out_relays || !out_count || !relays || num_relays == 0 || !author_hex) return -1;
    *out_relays = NULL;
    *out_count = 0;

    NostrSimplePool *pool = nostr_simple_pool_new();
    if (!pool) return -1;

    for (size_t i = 0; i < num_relays; i++)
        if (relays[i] && *relays[i])
            nostr_simple_pool_ensure_relay(pool, relays[i]);

    nostr_simple_pool_set_auto_unsub_on_eose(pool, true);
    nostr_simple_pool_set_event_middleware(pool, fetch_relays_cb);

    NostrFilter *f = nostr_filter_new();
    nostr_filter_add_kind(f, 30078);
    nostr_filter_add_author(f, author_hex);
    nostr_filter_set_limit(f, 1);
    nostr_simple_pool_query_single(pool, relays, num_relays, *f);
    nostr_filter_free(f);

    GoChannel *ch = go_channel_create(1);
    g_relays_ch = ch;
    nostr_simple_pool_start(pool);

    char *content_json = channel_receive_string(ch, 3000);

    nostr_simple_pool_stop(pool);
    nostr_simple_pool_free(pool);
    g_relays_ch = NULL;
    go_channel_close(ch);
    go_channel_unref(ch);

    int ret = -1;
    if (content_json) {
        /* Parse content: expect {"relays":["wss://...", ...]} */
        json_error_t jerr;
        json_t *root = json_loads(content_json, 0, &jerr);
        if (root) {
            json_t *arr = json_object_get(root, "relays");
            if (arr && json_is_array(arr)) {
                size_t n = json_array_size(arr);
                if (n > 0) {
                    char **vec = (char **)calloc(n, sizeof(char *));
                    if (vec) {
                        size_t outn = 0;
                        for (size_t i = 0; i < n; i++) {
                            json_t *it = json_array_get(arr, i);
                            const char *s = json_is_string(it) ? json_string_value(it) : NULL;
                            if (s) vec[outn++] = strdup(s);
                        }
                        if (outn > 0) {
                            *out_relays = vec;
                            *out_count = outn;
                            ret = 0;
                        } else {
                            for (size_t i = 0; i < n; i++) free(vec[i]);
                            free(vec);
                        }
                    }
                }
            }
            json_decref(root);
        }
        free(content_json);
    }
    return ret;
}
