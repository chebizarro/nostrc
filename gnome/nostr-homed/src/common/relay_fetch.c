/*
 * relay_fetch.c — Fetch replaceable Nostr events from relays.
 *
 * Uses one long-lived SimplePool for the process and per-call request
 * contexts for result delivery.  This avoids static global result channels
 * and repeated WebSocket setup/teardown for every fetch.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <glib.h>
#include <go.h>
#include "nostr_manifest.h"
#include "nostr-simple-pool.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include <jansson.h>

#define FETCH_TIMEOUT_MS 3000

/* ── Shared fetch service ───────────────────────────────────── */

typedef struct {
    int kind;
    char *author_hex;
    char *namespace_name;
    GoChannel *ch;
    gboolean done;
} RelayFetchRequest;

typedef struct {
    GMutex mutex;
    NostrSimplePool *pool;
    GPtrArray *requests; /* RelayFetchRequest*, owned by callers */
    gboolean started;
} RelayFetchService;

static RelayFetchService g_fetch_service = {0};
static gsize g_fetch_service_inited = 0;

static void relay_fetch_middleware(NostrIncomingEvent *in, void *user_data);

static void
relay_fetch_service_init_once(void)
{
    if (g_once_init_enter(&g_fetch_service_inited)) {
        g_mutex_init(&g_fetch_service.mutex);
        g_fetch_service.pool = nostr_simple_pool_new();
        g_fetch_service.requests = g_ptr_array_new();
        if (g_fetch_service.pool) {
            nostr_simple_pool_set_auto_unsub_on_eose(g_fetch_service.pool, true);
            nostr_simple_pool_set_event_middleware_ex(g_fetch_service.pool,
                                                       relay_fetch_middleware,
                                                       &g_fetch_service);
            nostr_simple_pool_start(g_fetch_service.pool);
            g_fetch_service.started = TRUE;
        }
        g_once_init_leave(&g_fetch_service_inited, 1);
    }
}

static RelayFetchService *
relay_fetch_service_get(void)
{
    relay_fetch_service_init_once();
    return g_fetch_service.pool ? &g_fetch_service : NULL;
}

static gboolean
event_matches_request(NostrEvent *event, RelayFetchRequest *req)
{
    if (!event || !req) return FALSE;
    if (nostr_event_get_kind(event) != req->kind) return FALSE;

    const char *pubkey = nostr_event_get_pubkey(event);
    if (req->author_hex && *req->author_hex && g_strcmp0(pubkey, req->author_hex) != 0)
        return FALSE;

    if (req->namespace_name && *req->namespace_name) {
        NostrTags *tags = (NostrTags *)nostr_event_get_tags(event);
        gboolean found = FALSE;
        if (tags) {
            for (size_t i = 0; i < nostr_tags_size(tags); i++) {
                NostrTag *tag = nostr_tags_get(tags, i);
                if (!tag || nostr_tag_size(tag) < 2) continue;
                if (g_strcmp0(nostr_tag_get_key(tag), "d") == 0 &&
                    g_strcmp0(nostr_tag_get_value(tag), req->namespace_name) == 0) {
                    found = TRUE;
                    break;
                }
            }
        }
        if (!found) return FALSE;
    }

    return TRUE;
}

static void
relay_fetch_middleware(NostrIncomingEvent *in, void *user_data)
{
    RelayFetchService *svc = user_data;
    if (!svc || !in || !in->event) return;

    const char *content = nostr_event_get_content(in->event);
    if (!content) return;

    g_mutex_lock(&svc->mutex);
    for (guint i = 0; i < svc->requests->len; i++) {
        RelayFetchRequest *req = g_ptr_array_index(svc->requests, i);
        if (!req || req->done) continue;
        if (!event_matches_request(in->event, req)) continue;

        char *dup = strdup(content);
        if (dup && go_channel_try_send(req->ch, dup) == 0) {
            req->done = TRUE;
        } else if (dup) {
            free(dup);
        }
        break;
    }
    g_mutex_unlock(&svc->mutex);
}

static RelayFetchRequest *
relay_fetch_request_new(int kind, const char *author_hex, const char *namespace_name)
{
    RelayFetchRequest *req = g_new0(RelayFetchRequest, 1);
    req->kind = kind;
    req->author_hex = g_strdup(author_hex);
    req->namespace_name = g_strdup(namespace_name);
    req->ch = go_channel_create(1);
    return req;
}

static void
relay_fetch_request_free(RelayFetchRequest *req)
{
    if (!req) return;
    if (req->ch) {
        go_channel_close(req->ch);
        go_channel_unref(req->ch);
    }
    g_free(req->author_hex);
    g_free(req->namespace_name);
    g_free(req);
}

static void
relay_fetch_service_add_request(RelayFetchService *svc, RelayFetchRequest *req)
{
    g_mutex_lock(&svc->mutex);
    g_ptr_array_add(svc->requests, req);
    g_mutex_unlock(&svc->mutex);
}

static void
relay_fetch_service_remove_request(RelayFetchService *svc, RelayFetchRequest *req)
{
    g_mutex_lock(&svc->mutex);
    g_ptr_array_remove(svc->requests, req);
    g_mutex_unlock(&svc->mutex);
}

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

static int
fetch_latest_content(const char **relays, size_t num_relays,
                     const char *author_hex, const char *namespace_name,
                     int kind, char **out_json)
{
    if (!out_json || !relays || num_relays == 0 || !author_hex) return -1;
    *out_json = NULL;

    RelayFetchService *svc = relay_fetch_service_get();
    if (!svc) return -1;

    for (size_t i = 0; i < num_relays; i++)
        if (relays[i] && *relays[i])
            nostr_simple_pool_ensure_relay(svc->pool, relays[i]);

    RelayFetchRequest *req = relay_fetch_request_new(kind, author_hex, namespace_name);
    relay_fetch_service_add_request(svc, req);

    NostrFilter *f = nostr_filter_new();
    nostr_filter_add_kind(f, kind);
    nostr_filter_add_author(f, author_hex);
    if (namespace_name && *namespace_name)
        nostr_filter_tags_append(f, "d", namespace_name, NULL);
    nostr_filter_set_limit(f, 1);
    nostr_simple_pool_query_single(svc->pool, relays, num_relays, *f);
    nostr_filter_free(f);

    char *json = channel_receive_string(req->ch, FETCH_TIMEOUT_MS);

    relay_fetch_service_remove_request(svc, req);
    relay_fetch_request_free(req);

    if (json) { *out_json = json; return 0; }
    return -1;
}

/* ── Manifest fetch (kind 30081) ────────────────────────────── */

int nh_fetch_latest_manifest_json(const char **relays, size_t num_relays,
                                  const char *author_hex,
                                  const char *namespace_name,
                                  char **out_json) {
    const char *ns = (namespace_name && *namespace_name) ? namespace_name : "personal";
    return fetch_latest_content(relays, num_relays, author_hex, ns, 30081, out_json);
}

/* ── Secrets fetch (kind 30079) ─────────────────────────────── */

int nh_fetch_latest_secrets_json(const char **relays, size_t num_relays,
                                 const char *author_hex,
                                 const char *namespace_name,
                                 char **out_json) {
    const char *ns = (namespace_name && *namespace_name) ? namespace_name : "personal";
    return fetch_latest_content(relays, num_relays, author_hex, ns, 30079, out_json);
}

/* ── Profile relays fetch (kind 30078) ──────────────────────── */

int nh_fetch_profile_relays(const char **relays, size_t num_relays,
                            const char *author_hex,
                            char ***out_relays, size_t *out_count) {
    if (!out_relays || !out_count) return -1;
    *out_relays = NULL;
    *out_count = 0;

    char *content_json = NULL;
    if (fetch_latest_content(relays, num_relays, author_hex, NULL, 30078, &content_json) != 0)
        return -1;

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
