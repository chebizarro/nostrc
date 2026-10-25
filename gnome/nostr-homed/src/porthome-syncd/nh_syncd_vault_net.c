/* SPDX-License-Identifier: MIT */
#include "nh_syncd_vault_net.h"
#include <nostr-simple-pool.h>
#include <nostr-keys.h>
#include <json.h>
#include <nostr/bud02/auth.h>
#include <nip42.h>
#include <nip11.h>
#include <channel.h>
#include <jansson.h>
#include <openssl/crypto.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    struct nh_vault_net *net;
    NostrRelay *relay;
    char *challenge;
    char auth_id[65];
    int auth_required, authenticated, eose;
    uint64_t auth_generation, sent_generation;
    char publish_id[65];
    int publish_state, sent; /* 0 pending, 1 OK true, -1 rejected, -2 needs AUTH. */
} relay_slot;
struct nh_vault_net {
    const char *secret;
    const char *const *urls, *const *servers;
    size_t read_limit;
    int test_mode, cache_only;
    size_t nrelays, nblossom, replication;
    NostrSimplePool *pool;
    NostrFilters *filters;
    relay_slot slots[VAULT_MAX_HINTS];
    blossom_client_t *blossom[VAULT_MAX_HINTS];
    blossom_signer_t signer;
    char *pubkey;
    pthread_mutex_t mutex;
    int wake, error, resubscribe, running;
    NostrEvent **events;
    size_t count;
};
static void wake_net(nh_vault_net *n) {
    uint64_t one = 1;
    if (n->wake >= 0) {
        ssize_t sent;
        do {
            sent = write(n->wake, &one, sizeof one);
        } while (sent < 0 && errno == EINTR);
        if (sent < 0 && errno != EAGAIN) {
            pthread_mutex_lock(&n->mutex);
            n->error = VAULT_ERR_IO;
            pthread_mutex_unlock(&n->mutex);
        }
    }
}
static void event_cb(NostrIncomingEvent *incoming, void *ctx) {
    nh_vault_net *n = ctx;
    if (!nostr_event_check_signature(incoming->event)) {
        nostr_event_free(incoming->event);
        return;
    }
    pthread_mutex_lock(&n->mutex);
    for (size_t i = 0; i < n->count; ++i) {
        if (!strcmp(n->events[i]->id, incoming->event->id)) {
            nostr_event_free(incoming->event);
            pthread_mutex_unlock(&n->mutex);
            return;
        }
    }
    if (n->count == VAULT_MAX_HEADS * 2) {
        n->error = PORTHOME_ERR_TOO_LARGE;
        nostr_event_free(incoming->event);
    } else
        n->events[n->count++] = incoming->event;
    pthread_mutex_unlock(&n->mutex);
    wake_net(n);
}
static void state_cb(NostrRelay *relay, const char *state, const char *reason, void *ctx) {
    nh_vault_net *n = ctx;
    pthread_mutex_lock(&n->mutex);
    for (size_t i = 0; i < n->nrelays; ++i)
        if (n->slots[i].relay == relay) {
            relay_slot *s = &n->slots[i];
            if (!strcmp(state, "EOSE"))
                s->eose = !s->auth_required || s->authenticated;
            else if (!strcmp(state, "CLOSED")) {
                s->eose = 0;
                if (reason && !strncmp(reason, "auth-required:", 14)) {
                    s->auth_required = 1;
                    n->cache_only = 1;
                } else
                    n->error = VAULT_ERR_AUTH;
            }
        }
    pthread_mutex_unlock(&n->mutex);
    wake_net(n);
}
static void auth_cb(NostrRelay *relay, const char *challenge, void *ctx) {
    relay_slot *s = ctx;
    nh_vault_net *n = s->net;
    (void)relay;
    pthread_mutex_lock(&n->mutex);
    free(s->challenge);
    s->challenge = strdup(challenge);
    s->authenticated = s->eose = 0;
    ++s->auth_generation;
    s->auth_id[0] = 0;
    if (!s->challenge)
        n->error = VAULT_ERR_OOM;
    pthread_mutex_unlock(&n->mutex);
    wake_net(n);
}
static void ok_cb(const char *id, bool accepted, const char *reason, void *ctx) {
    relay_slot *s = ctx;
    nh_vault_net *n = s->net;
    (void)reason;
    pthread_mutex_lock(&n->mutex);
    if (id && *s->auth_id && !strcmp(id, s->auth_id)) {
        s->authenticated = accepted;
        if (accepted)
            n->resubscribe = 1;
        else
            n->error = VAULT_ERR_AUTH;
    } else if (id && *s->publish_id && !strcmp(id, s->publish_id)) {
        s->publish_state = accepted                                           ? 1
                           : reason && !strncmp(reason, "auth-required:", 14) ? -2
                                                                              : -1;
        if (s->publish_state == -2)
            s->auth_required = n->cache_only = 1;
    }
    pthread_mutex_unlock(&n->mutex);
    wake_net(n);
}
static int protocol_work(nh_vault_net *n) {
    for (size_t i = 0; i < n->nrelays; ++i) {
        pthread_mutex_lock(&n->mutex);
        char *challenge = n->slots[i].challenge;
        n->slots[i].challenge = NULL;
        pthread_mutex_unlock(&n->mutex);
        if (!challenge)
            continue;
        char *frame = NULL;
        int ok = nip42_build_auth_response(challenge, n->urls[i], n->secret, &frame);
        free(challenge);
        if (!ok)
            return VAULT_ERR_AUTH;
        json_t *j = json_loads(frame, JSON_REJECT_DUPLICATES, NULL);
        const char *id = json_string_value(json_object_get(json_array_get(j, 1), "id"));
        if (!id || strlen(id) != 64) {
            json_decref(j);
            free(frame);
            return VAULT_ERR_AUTH;
        }
        pthread_mutex_lock(&n->mutex);
        memcpy(n->slots[i].auth_id, id, 65);
        pthread_mutex_unlock(&n->mutex);
        json_decref(j);
        GoChannel *result = nostr_relay_write(n->slots[i].relay, frame);
        free(frame);
        if (!result)
            return VAULT_ERR_IO;
        go_channel_close(result);
        void *error = NULL;
        while (go_channel_try_receive(result, &error) == 0)
            free_error(error);
        go_channel_unref(result);
    }
    pthread_mutex_lock(&n->mutex);
    int again = n->resubscribe, error = n->error;
    n->resubscribe = 0;
    pthread_mutex_unlock(&n->mutex);
    if (again && n->filters && n->running) {
        nostr_simple_pool_stop(n->pool);
        pthread_mutex_lock(&n->mutex);
        for (size_t i = 0; i < n->nrelays; ++i)
            n->slots[i].eose = 0;
        pthread_mutex_unlock(&n->mutex);
        nostr_simple_pool_subscribe(n->pool, (const char **)n->urls, n->nrelays, *n->filters,
                                    false);
        nostr_simple_pool_start(n->pool);
    }
    return error;
}
static int sign_json(const char *wire, char **out, void *ctx) {
    nh_vault_net *n = ctx;
    *out = NULL;
    NostrEvent *e = nostr_event_new();
    if (!e)
        return BLOSSOM_ERR_NOMEM;
    int rc = nostr_event_deserialize(e, wire);
    if (!rc)
        rc = nostr_event_sign(e, n->secret);
    if (!rc)
        *out = nostr_event_serialize(e);
    nostr_event_free(e);
    return !rc && *out ? 0 : BLOSSOM_ERR_NOSTR;
}
static int blob_read(void *ctx, const uint8_t hash[32], uint8_t **out, size_t *len) {
    nh_vault_net *n = ctx;
    char hex[65];
    porthome_hex64(hash, hex);
    *out = NULL;
    *len = 0;
    for (size_t i = 0; i < n->nblossom; ++i) {
        blossom_client_t *limited = NULL;
        blossom_client_t *client = n->blossom[i];
        if (n->read_limit) {
            blossom_client_opts_t opts = {.endpoint = n->servers[i],
                                          .timeout_seconds = 15,
                                          .max_blob_bytes = n->read_limit,
                                          .allow_insecure = n->test_mode != 0};
            if (blossom_client_new(&opts, &n->signer, &limited))
                return VAULT_ERR_IO;
            client = limited;
        }
        int rc = blossom_client_get(client, hex, out, len);
        blossom_client_free(limited);
        if (!rc)
            return 0;
        if (rc == BLOSSOM_ERR_AUTH)
            return VAULT_ERR_AUTH;
        if (rc != BLOSSOM_ERR_NOT_FOUND && rc != BLOSSOM_ERR_NETWORK && rc != BLOSSOM_ERR_TIMEOUT)
            return VAULT_ERR_IO;
    }
    return VAULT_ERR_IO;
}
int nh_vault_net_reconcile(void *ctx, const uint8_t hash[32], const uint8_t *expected,
                           size_t nbytes) {
    nh_vault_net *n = ctx;
    char hex[65];
    porthome_hex64(hash, hex);
    size_t accepted = 0;
    for (size_t i = 0; i < n->nblossom; ++i) {
        blossom_client_t *client = NULL;
        blossom_client_opts_t opts = {.endpoint = n->servers[i],
                                      .timeout_seconds = 15,
                                      .max_blob_bytes = nbytes,
                                      .allow_insecure = n->test_mode != 0};
        if (blossom_client_new(&opts, &n->signer, &client))
            return VAULT_ERR_IO;
        uint8_t *bytes = NULL;
        size_t len = 0;
        int rc = blossom_client_get(client, hex, &bytes, &len);
        blossom_client_free(client);
        int same = !rc && len == nbytes && !CRYPTO_memcmp(bytes, expected, len);
        free(bytes);
        if (rc == BLOSSOM_ERR_AUTH)
            return VAULT_ERR_AUTH;
        if (same && ++accepted >= n->replication)
            return 0;
    }
    return VAULT_ERR_INCOMPLETE;
}
static int blob_put(void *ctx, const uint8_t *bytes, size_t len, const uint8_t hash[32]) {
    nh_vault_net *n = ctx;
    char hex[65];
    porthome_hex64(hash, hex);
    size_t accepted = 0;
    for (size_t i = 0; i < n->nblossom; ++i) {
        NostrEvent *auth = nostr_bud02_create_auth_event(NOSTR_BUD02_ACTION_UPLOAD, hex, 0, NULL);
        if (!auth)
            return VAULT_ERR_OOM;
        int rc = nostr_event_sign(auth, n->secret);
        char *wire = rc ? NULL : nostr_event_serialize(auth);
        nostr_event_free(auth);
        if (!wire)
            return VAULT_ERR_AUTH;
        blossom_blob_descriptor_t *desc = NULL;
        rc = blossom_client_put_signed(n->blossom[i], bytes, len, hex, wire, &desc);
        free(wire);
        blossom_blob_descriptor_free(desc);
        if (!rc)
            ++accepted;
        else if (rc == BLOSSOM_ERR_AUTH)
            return VAULT_ERR_AUTH;
    }
    return accepted >= n->replication ? 0 : VAULT_ERR_IO;
}
int nh_vault_net_open(const char *secret, const char *const *relays, size_t nr,
                      const char *const *servers, size_t nb, size_t replication, int test,
                      nh_vault_net **out) {
    *out = NULL;
    if (!secret || !nr || nr > VAULT_MAX_HINTS || !nb || nb > VAULT_MAX_HINTS || !replication ||
        replication > nb)
        return VAULT_ERR_ARG;
    nh_vault_net *n = calloc(1, sizeof *n);
    if (!n)
        return VAULT_ERR_OOM;
    n->wake = -1;
    pthread_mutex_init(&n->mutex, NULL);
    n->secret = secret;
    n->urls = relays;
    n->servers = servers;
    n->nrelays = nr;
    n->test_mode = test;
    n->replication = replication;
    n->pubkey = nostr_key_get_public(secret);
    n->pool = nostr_simple_pool_new();
    n->events = calloc(VAULT_MAX_HEADS * 2, sizeof *n->events);
    n->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (!n->pubkey || !n->pool || !n->events || n->wake < 0)
        goto fail;
    n->signer = (blossom_signer_t){n->pubkey, sign_json, n};
    for (size_t i = 0; i < nb; ++i) {
        blossom_client_opts_t opts = {.endpoint = servers[i],
                                      .timeout_seconds = 15,
                                      .max_blob_bytes = 64u * 1024u * 1024u,
                                      .allow_insecure = test != 0};
        if (blossom_client_new(&opts, &n->signer, &n->blossom[i]))
            goto fail;
        ++n->nblossom;
        blossom_capability_state_t state = BLOSSOM_CAP_UNKNOWN;
        (void)vault_vsp_probe_blossom(n->blossom[i], &state);
        if (state == BLOSSOM_CAP_YES)
            n->cache_only = 1;
    }
    nostr_simple_pool_set_event_middleware_ex(n->pool, event_cb, n);
    nostr_simple_pool_set_subscription_callback(n->pool, state_cb, n);
    nostr_simple_pool_set_auto_unsub_on_eose(n->pool, false);
    for (size_t i = 0; i < nr; ++i) {
        if (strncmp(relays[i], "wss://", 6) && (!test || strncmp(relays[i], "ws://", 5)))
            goto fail;
        Error *error = NULL;
        NostrRelay *r = nostr_relay_new(NULL, relays[i], &error);
        free_error(error);
        if (!r)
            goto fail;
        relay_slot *s = &n->slots[i];
        s->net = n;
        s->relay = r;
        RelayInformationDocument *info = nostr_nip11_fetch_info(relays[i]);
        s->auth_required = info && info->limitation && info->limitation->auth_required;
        if (s->auth_required)
            n->cache_only = 1;
        nostr_nip11_free_info(info);
        nostr_relay_set_auth_callback(r, auth_cb, s);
        nostr_relay_set_ok_callback(r, ok_cb, s);
        nostr_simple_pool_add_relay(n->pool, r);
    }
    *out = n;
    return 0;
fail:
    nh_vault_net_close(n);
    return VAULT_ERR_IO;
}
static int subscribe(nh_vault_net *n, const uint8_t id[32], const uint8_t (*writers)[32],
                     size_t nw) {
    if (nw > VAULT_MAX_HEADS)
        return PORTHOME_ERR_TOO_LARGE;
    NostrFilters *filters = nostr_filters_new();
    char tag[PORTHOME_HEAD_D_LEN];
    if (!filters || porthome_head_d_tag(id, tag)) {
        nostr_filters_free(filters);
        return VAULT_ERR_OOM;
    }
    json_t *f = json_pack("{s:[i],s:[s],s:i,s:[]}", "kinds", PORTHOME_HEAD_KIND, "#d", tag, "limit",
                          VAULT_MAX_HEADS, "authors");
    if (!f) {
        nostr_filters_free(filters);
        return VAULT_ERR_OOM;
    }
    for (size_t i = 0; i < nw; ++i) {
        char hex[65];
        porthome_hex64(writers[i], hex);
        json_array_append_new(json_object_get(f, "authors"), json_string(hex));
    }
    NostrFilter *filter = nostr_filter_new();
    char *wire = json_dumps(f, JSON_COMPACT);
    json_decref(f);
    int rc = 0;
    if (nw && (!wire || !filter || nostr_filter_deserialize(filter, wire) ||
               !nostr_filters_add(filters, filter)))
        rc = VAULT_ERR_ARG;
    free(wire);
    nostr_filter_free(filter);
    filter = NULL;
    f = json_pack("{s:[i],s:[s],s:i}", "kinds", 1059, "#p", n->pubkey, "limit", VAULT_MAX_HEADS);
    wire = f ? json_dumps(f, JSON_COMPACT) : NULL;
    json_decref(f);
    filter = nostr_filter_new();
    if (!wire || !filter || nostr_filter_deserialize(filter, wire) ||
        !nostr_filters_add(filters, filter))
        rc = VAULT_ERR_ARG;
    free(wire);
    nostr_filter_free(filter);
    filter = NULL;
    if (rc) {
        nostr_filters_free(filters);
        return rc;
    }
    nostr_simple_pool_stop(n->pool);
    n->running = 0;
    nostr_filters_free(n->filters);
    n->filters = filters;
    pthread_mutex_lock(&n->mutex);
    for (size_t i = 0; i < n->nrelays; ++i)
        n->slots[i].eose = 0;
    pthread_mutex_unlock(&n->mutex);
    nostr_simple_pool_subscribe(n->pool, (const char **)n->urls, n->nrelays, *filters, false);
    nostr_simple_pool_start(n->pool);
    n->running = 1;
    return 0;
}
int nh_vault_net_listen(nh_vault_net *n, const uint8_t id[32], const uint8_t (*writers)[32],
                        size_t count) {
    return subscribe(n, id, writers, count);
}
int nh_vault_net_fd(const nh_vault_net *n) { return n ? n->wake : -1; }
int nh_vault_net_drain(nh_vault_net *n, NostrEvent ***out, size_t *count) {
    *out = NULL;
    *count = 0;
    uint64_t ticks;
    while (read(n->wake, &ticks, sizeof ticks) > 0) {
    }
    int rc = protocol_work(n);
    pthread_mutex_lock(&n->mutex);
    if (!rc && n->count) {
        *out = calloc(n->count, sizeof **out);
        if (!*out)
            rc = VAULT_ERR_OOM;
        else {
            memcpy(*out, n->events, n->count * sizeof **out);
            *count = n->count;
            n->count = 0;
        }
    }
    pthread_mutex_unlock(&n->mutex);
    return rc;
}
int nh_vault_net_catchup(nh_vault_net *n, const uint8_t id[32], const uint8_t (*writers)[32],
                         size_t nw, NostrEvent ***out, size_t *count) {
    *out = NULL;
    *count = 0;
    int rc = subscribe(n, id, writers, nw);
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!rc) {
        rc = protocol_work(n);
        pthread_mutex_lock(&n->mutex);
        int done = 1;
        for (size_t i = 0; i < n->nrelays; ++i)
            if (!n->slots[i].eose)
                done = 0;
        pthread_mutex_unlock(&n->mutex);
        if (rc || done)
            break;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int remaining = 30000 - (int)((now.tv_sec - start.tv_sec) * 1000 +
                                      (now.tv_nsec - start.tv_nsec) / 1000000);
        if (remaining <= 0) {
            rc = VAULT_ERR_IO;
            break;
        }
        struct pollfd p = {n->wake, POLLIN, 0};
        int r = poll(&p, 1, remaining);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            rc = VAULT_ERR_IO;
            break;
        }
        uint64_t tick;
        while (read(n->wake, &tick, sizeof tick) > 0) {
        }
    }
    nostr_simple_pool_stop(n->pool);
    n->running = 0;
    if (!rc)
        rc = nh_vault_net_drain(n, out, count);
    return rc;
}
int nh_vault_net_publish(nh_vault_net *n, NostrEvent *event) {
    if (!event || !nostr_event_check_signature(event))
        return VAULT_ERR_AUTH;
    pthread_mutex_lock(&n->mutex);
    for (size_t i = 0; i < n->nrelays; ++i) {
        relay_slot *s = &n->slots[i];
        memcpy(s->publish_id, event->id, 65);
        s->publish_state = s->sent = 0;
        s->sent_generation = s->auth_generation;
    }
    pthread_mutex_unlock(&n->mutex);
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int rc = 0, accepted = 0;
    while (!rc && !accepted) {
        rc = protocol_work(n);
        int pending = 0;
        for (size_t i = 0; !rc && i < n->nrelays; ++i) {
            pthread_mutex_lock(&n->mutex);
            relay_slot *s = &n->slots[i];
            if (s->publish_state == -2 && s->authenticated &&
                s->auth_generation > s->sent_generation)
                s->publish_state = s->sent =
                    0; /* Explicit auth-required rejection: retry this exact ID. */
            if (s->publish_state == 1)
                accepted = 1;
            if (s->publish_state == 0 || s->publish_state == -2)
                pending = 1;
            int send = !s->sent && s->publish_state == 0 && (!s->auth_required || s->authenticated);
            if (send) {
                s->sent = 1;
                s->sent_generation = s->auth_generation;
            }
            pthread_mutex_unlock(&n->mutex);
            if (send)
                nostr_relay_publish(s->relay, event);
        }
        if (rc || accepted)
            break;
        if (!pending) {
            rc = VAULT_ERR_AUTH;
            break;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        int remaining = 15000 - (int)((now.tv_sec - start.tv_sec) * 1000 +
                                      (now.tv_nsec - start.tv_nsec) / 1000000);
        if (remaining <= 0) {
            rc = VAULT_ERR_IO;
            break;
        }
        struct pollfd p = {n->wake, POLLIN, 0};
        int ready = poll(&p, 1, remaining);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0) {
            rc = VAULT_ERR_IO;
            break;
        }
        uint64_t tick;
        while (read(n->wake, &tick, sizeof tick) > 0) {
        }
        /* Continue AUTH/OK/CLOSED processing while awaiting the matching OK.
         * The pool continues owning EVENT delivery; no application timeout
         * turns an unacknowledged publish into success. */
    }
    pthread_mutex_lock(&n->mutex);
    for (size_t i = 0; i < n->nrelays; ++i)
        n->slots[i].publish_id[0] = 0;
    int queued = n->count != 0;
    pthread_mutex_unlock(&n->mutex);
    if (queued)
        wake_net(n);
    return rc ? rc : accepted ? 0 : VAULT_ERR_IO;
}
void nh_vault_net_close(nh_vault_net *n) {
    if (!n)
        return;
    if (n->pool) {
        nostr_simple_pool_stop(n->pool);
        nostr_simple_pool_free(n->pool);
    }
    for (size_t i = 0; i < n->count; ++i)
        nostr_event_free(n->events[i]);
    for (size_t i = 0; i < n->nrelays; ++i)
        free(n->slots[i].challenge);
    for (size_t i = 0; i < n->nblossom; ++i)
        blossom_client_free(n->blossom[i]);
    nostr_filters_free(n->filters);
    free(n->events);
    free(n->pubkey);
    if (n->wake >= 0)
        close(n->wake);
    pthread_mutex_destroy(&n->mutex);
    free(n);
}
int nh_vault_net_cache_only(const nh_vault_net *n) {
    nh_vault_net *mutable = (nh_vault_net *)n;
    pthread_mutex_lock(&mutable->mutex);
    int result = n->cache_only;
    pthread_mutex_unlock(&mutable->mutex);
    return result;
}
void nh_vault_net_read_limit(nh_vault_net *n, size_t limit) { n->read_limit = limit; }
vault_blob_io nh_vault_net_blobs(nh_vault_net *n) {
    return (vault_blob_io){n, blob_read, blob_put};
}
