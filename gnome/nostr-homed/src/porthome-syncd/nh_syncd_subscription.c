/*
 * Personal-home kind-30078 subscription. Pool callbacks only validate and
 * queue messages; the daemon drains them on its state-owning main thread.
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include "nh_syncd.h"
#include "nh_syncd_auth.h"
#include "nostr-simple-pool.h"
#include "nostr-relay.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include "error.h"
#include <channel.h>

#include <errno.h>
#include <curl/curl.h>
#include <jansson.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#define POINTER_KIND 30078
#define MAX_POINTER_HEX (16u * 1024u * 1024u)
#define MAX_PENDING 64u
#define SEEN_IDS 256u
#define NIP11_MAX_BYTES (64u * 1024u)

typedef struct { char bytes[NIP11_MAX_BYTES + 1]; size_t len; } nip11_doc;
static size_t nip11_write(char *ptr, size_t size, size_t count, void *ud) {
    nip11_doc *doc = ud;
    if (size && count > (NIP11_MAX_BYTES - doc->len) / size) return 0;
    size_t len = size * count;
    memcpy(doc->bytes + doc->len, ptr, len);
    doc->len += len;
    doc->bytes[doc->len] = '\0';
    return len;
}
static void probe_nip11(const char *url) {
    const char *scheme = !strncmp(url, "wss://", 6) ? "https" :
                         !strncmp(url, "ws://", 5) ? "http" : NULL;
    if (!scheme) return;
    char *http = NULL;
    if (asprintf(&http, "%s%s", scheme, url + (scheme[4] == 's' ? 3 : 2)) < 0)
        return;
    CURL *curl = curl_easy_init();
    if (!curl) { free(http); return; }
    nip11_doc *doc = calloc(1, sizeof *doc);
    struct curl_slist *headers = curl_slist_append(NULL, "Accept: application/nostr+json");
    if (doc && headers) {
        curl_easy_setopt(curl, CURLOPT_URL, http);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nip11_write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, doc);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 750L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1500L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        CURLcode result = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (result == CURLE_OK && status == 200) {
            json_error_t error;
            json_t *info = json_loads(doc->bytes, 0, &error);
            if (json_is_object(info)) {
                json_t *limits = json_object_get(info, "limitation");
                if (json_is_object(limits) &&
                    json_is_true(json_object_get(limits, "auth_required")))
                    fprintf(stderr, "syncd/subscription: %s advertises NIP-42 AUTH\n", url);
            }
            if (info) json_decref(info);
        }
    }
    curl_slist_free_all(headers);
    free(doc);
    curl_easy_cleanup(curl);
    free(http);
}

typedef enum { ITEM_EVENT, ITEM_EOSE, ITEM_CLOSED, ITEM_AUTH, ITEM_OK } item_kind;
typedef struct pending {
    struct pending *next;
    item_kind kind;
    size_t relay_index;
    char *id, *d_tag, *content, *reason;
    int64_t created_at;
    bool accepted;
} pending;
typedef struct {
    struct nh_syncd_subscription *owner;
    NostrRelay *relay;
    bool eose;
    char auth_id[65];
} relay_slot;
struct nh_syncd_subscription {
    NostrSimplePool *pool;
    NostrFilters *filters;
    relay_slot *slots;
    size_t n_relays;
    char **urls;
    char *pubkey, *d_tag, *secret;
    nh_syncd_on_remote_pointer_fn cb;
    void *cb_ud;
    int wake;
    pthread_mutex_t mu;
    pending *head, *tail;
    size_t pending_count;
    bool overflow;
    bool apply_failed;
    char failed_id[65];
    pending *retry;
    uint64_t retry_due_ms;
    uint32_t retry_backoff_ms;
    char seen[SEEN_IDS][65];
    size_t seen_next;
};

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static void free_pending(pending *p) {
    if (!p) return;
    free(p->id); free(p->d_tag); free(p->content); free(p->reason); free(p);
}
static int pointer_order(const pending *a, const pending *b) {
    if (a->created_at != b->created_at)
        return a->created_at > b->created_at ? 1 : -1;
    return strcmp(a->id, b->id);
}

static const char *find_d_tag(NostrEvent *e) {
    NostrTags *tags = nostr_event_get_tags(e);
    if (!tags) return NULL;
    for (size_t i = 0; i < nostr_tags_size(tags); ++i) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (tag && nostr_tag_size(tag) >= 2 &&
            !strcmp(nostr_tag_get_key(tag), "d")) return nostr_tag_get(tag, 1);
    }
    return NULL;
}
static void wake_main(nh_syncd_subscription *s) {
    uint64_t one = 1;
    if (write(s->wake, &one, sizeof one) < 0 && errno != EAGAIN)
        fprintf(stderr, "syncd/subscription: wake failed: %s\n", strerror(errno));
}
static void enqueue(nh_syncd_subscription *s, pending *p) {
    if (!p) return;
    pthread_mutex_lock(&s->mu);
    if (s->pending_count == MAX_PENDING) {
        s->overflow = true;
        pthread_mutex_unlock(&s->mu);
        free(p->id); free(p->d_tag); free(p->content); free(p->reason); free(p);
        wake_main(s);
        return;
    }
    if (s->tail) s->tail->next = p; else s->head = p;
    s->tail = p;
    ++s->pending_count;
    pthread_mutex_unlock(&s->mu);
    wake_main(s);
}
static void event_cb(NostrIncomingEvent *in, void *ud) {
    nh_syncd_subscription *s = ud;
    if (!s || !in || !in->event) return;
    NostrEvent *e = in->event;
    char *id = nostr_event_get_id(e);
    const char *pub = nostr_event_get_pubkey(e);
    const char *tag = find_d_tag(e), *content = nostr_event_get_content(e);
    if (nostr_event_get_kind(e) != POINTER_KIND || !id || strlen(id) != 64 ||
        !pub || strcmp(pub, s->pubkey) || !tag || strcmp(tag, s->d_tag) ||
        !content || strnlen(content, MAX_POINTER_HEX + 1) > MAX_POINTER_HEX ||
        !nostr_event_check_signature(e)) {
        free(id);
        return;
    }
    pending *p = calloc(1, sizeof *p);
    if (!p) { free(id); return; }
    p->kind = ITEM_EVENT;
    p->id = id; p->d_tag = strdup(tag); p->content = strdup(content);
    p->created_at = nostr_event_get_created_at(e);
    if (!p->id || !p->d_tag || !p->content) {
        free(p->id); free(p->d_tag); free(p->content); free(p); return;
    }
    enqueue(s, p);
}
static void state_cb(NostrRelay *relay, const char *state, const char *reason, void *ud) {
    nh_syncd_subscription *s = ud;
    if (!s || !relay || !state) return;
    pending *p = calloc(1, sizeof *p);
    if (!p) return;
    p->kind = !strcmp(state, "EOSE") ? ITEM_EOSE : ITEM_CLOSED;
    if (p->kind == ITEM_CLOSED && strcmp(state, "CLOSED")) { free(p); return; }
    p->reason = reason ? strdup(reason) : NULL;
    for (size_t i = 0; i < s->n_relays; ++i)
        if (s->slots[i].relay == relay) { p->relay_index = i; enqueue(s, p); return; }
    free(p->reason); free(p);
}
static void auth_cb(NostrRelay *relay, const char *challenge, void *ud) {
    relay_slot *slot = ud;
    if (!slot || !relay || !challenge) return;
    pending *p = calloc(1, sizeof *p);
    if (!p) return;
    p->kind = ITEM_AUTH;
    p->relay_index = (size_t)(slot - slot->owner->slots);
    p->reason = strdup(challenge);
    if (!p->reason) { free(p); return; }
    enqueue(slot->owner, p);
}
static void ok_cb(const char *id, bool accepted, const char *reason, void *ud) {
    relay_slot *slot = ud;
    if (!slot || !id) return;
    pending *p = calloc(1, sizeof *p);
    if (!p) return;
    p->kind = ITEM_OK;
    p->relay_index = (size_t)(slot - slot->owner->slots);
    p->id = strdup(id);
    p->accepted = accepted;
    p->reason = reason ? strdup(reason) : NULL;
    if (!p->id) { free(p->reason); free(p); return; }
    enqueue(slot->owner, p);
}
bool nh_syncd_send_auth(NostrRelay *relay, const char *secret,
                        const char *challenge, char auth_id[65]) {
    if (!relay || !secret || !challenge || !auth_id) return false;
    auth_id[0] = '\0';
    NostrEvent *e = nostr_event_new();
    if (!e) return false;
    NostrTags *tags = nostr_tags_new(2,
        nostr_tag_new("challenge", challenge, NULL),
        nostr_tag_new("relay", nostr_relay_get_url_const(relay), NULL));
    if (!tags) { nostr_event_free(e); return false; }
    nostr_event_set_kind(e, 22242);
    nostr_event_set_created_at(e, (int64_t)time(NULL));
    nostr_event_set_content(e, "");
    nostr_event_set_tags(e, tags);
    bool ok = nostr_event_sign(e, secret) == 0;
    char *id = ok ? nostr_event_get_id(e) : NULL;
    if (!id || strlen(id) != 64) ok = false;
    if (ok) {
        char *wire = nostr_event_serialize_compact(e);
        char *frame = NULL;
        if (!wire || asprintf(&frame, "[\"AUTH\",%s]", wire) < 0) ok = false;
        if (ok) {
            memcpy(auth_id, id, 65);
            GoChannel *result = nostr_relay_write(relay, frame);
            ok = result != NULL;
            if (result) {
                go_channel_close(result);
                void *error = NULL;
                while (go_channel_try_receive(result, &error) == 0) free_error(error);
                go_channel_unref(result);
            }
        }
        free(frame); free(wire);
    }
    free(id);
    nostr_event_free(e);
    if (!ok) auth_id[0] = '\0';
    return ok;
}
int nh_syncd_subscription_start(const char *const *relays, size_t n_relays,
                                const char *account_pubkey_hex, const char *d_tag,
                                const char *signer_nsec_hex,
                                nh_syncd_on_remote_pointer_fn cb, void *cb_ud,
                                nh_syncd_subscription **out) {
    if (!relays || !n_relays || !account_pubkey_hex || !d_tag ||
        !signer_nsec_hex || !cb || !out) return NH_SYNCD_ERR_ARG;
    *out = NULL;
    nh_syncd_subscription *s = calloc(1, sizeof *s);
    if (!s) return NH_SYNCD_ERR_OOM;
    s->wake = -1;
    pthread_mutex_init(&s->mu, NULL);
    s->n_relays = n_relays;
    s->cb = cb; s->cb_ud = cb_ud;
    s->pubkey = strdup(account_pubkey_hex);
    s->d_tag = strdup(d_tag);
    s->secret = strdup(signer_nsec_hex);
    s->slots = calloc(n_relays, sizeof *s->slots);
    s->urls = calloc(n_relays, sizeof *s->urls);
    s->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    s->pool = nostr_simple_pool_new();
    if (!s->pubkey || !s->d_tag || !s->secret || !s->slots || !s->urls ||
        s->wake < 0 || !s->pool) goto fail;
    nostr_simple_pool_set_auto_unsub_on_eose(s->pool, false);
    nostr_simple_pool_set_event_middleware_ex(s->pool, event_cb, s);
    nostr_simple_pool_set_subscription_callback(s->pool, state_cb, s);
    for (size_t i = 0; i < n_relays; ++i) {
        s->urls[i] = strdup(relays[i]);
        if (!s->urls[i]) goto fail;
        probe_nip11(relays[i]);
        Error *err = NULL;
        NostrRelay *relay = nostr_relay_new(NULL, relays[i], &err);
        free_error(err);
        if (!relay) goto fail;
        s->slots[i].owner = s;
        s->slots[i].relay = relay;
        nostr_relay_set_auth_callback(relay, auth_cb, &s->slots[i]);
        nostr_relay_set_ok_callback(relay, ok_cb, &s->slots[i]);
        nostr_simple_pool_add_relay(s->pool, relay);
    }
    NostrFilter *f = nostr_filter_new();
    s->filters = nostr_filters_new();
    if (!f || !s->filters) { nostr_filter_free(f); goto fail; }
    nostr_filter_add_kind(f, POINTER_KIND);
    nostr_filter_add_author(f, account_pubkey_hex);
    nostr_filter_tags_append(f, "d", d_tag, NULL);
    nostr_filter_set_limit(f, 1); /* replaceable pointer: latest full manifest */
    bool added = nostr_filters_add(s->filters, f);
    nostr_filter_free(f);
    if (!added) goto fail;
    nostr_simple_pool_start(s->pool);
    nostr_simple_pool_subscribe_async(s->pool, (const char **)s->urls,
                                      n_relays, *s->filters, false);
    *out = s;
    return NH_SYNCD_OK;
fail:
    nh_syncd_subscription_stop(s);
    return NH_SYNCD_ERR_OOM;
}
int nh_syncd_subscription_fd(const nh_syncd_subscription *s) { return s ? s->wake : -1; }
bool nh_syncd_subscription_caught_up(const nh_syncd_subscription *s) {
    if (!s || s->apply_failed) return false;
    for (size_t i = 0; i < s->n_relays; ++i) if (!s->slots[i].eose) return false;
    return true;
}
uint64_t nh_syncd_subscription_retry_delay_ms(const nh_syncd_subscription *s) {
    if (!s || !s->retry) return UINT64_MAX;
    uint64_t now = monotonic_ms();
    return s->retry_due_ms > now ? s->retry_due_ms - now : 0;
}
int nh_syncd_subscription_drain(nh_syncd_subscription *s) {
    if (!s) return NH_SYNCD_ERR_ARG;
    uint64_t count;
    while (read(s->wake, &count, sizeof count) == sizeof count) {}
    pthread_mutex_lock(&s->mu);
    pending *p = s->head;
    s->head = s->tail = NULL;
    s->pending_count = 0;
    bool overflow = s->overflow;
    s->overflow = false;
    pthread_mutex_unlock(&s->mu);
    if (overflow) fprintf(stderr, "syncd/subscription: pending pointer queue overflow\n");
    bool resubscribe = overflow;
    for (; p; ) {
        pending *next = p->next;
        p->next = NULL;
        relay_slot *slot = &s->slots[p->relay_index];
        if (p->kind == ITEM_EVENT) {
            bool seen = false;
            for (size_t i = 0; i < SEEN_IDS; ++i)
                if (!strcmp(s->seen[i], p->id)) { seen = true; break; }
            if (!seen) {
                int applied = s->cb(s->cb_ud, p->id, p->created_at,
                                    p->d_tag, p->content);
                if (applied == NH_SYNCD_OK) {
                    memcpy(s->seen[s->seen_next++ % SEEN_IDS], p->id, 65);
                    if (!s->retry || pointer_order(p, s->retry) >= 0) {
                        free_pending(s->retry);
                        s->retry = NULL;
                        s->apply_failed = false;
                        s->failed_id[0] = '\0';
                        s->retry_backoff_ms = 0;
                    }
                } else {
                    s->apply_failed = true;
                    if (!s->retry || pointer_order(p, s->retry) >= 0) {
                        free_pending(s->retry);
                        s->retry = p;
                        p = NULL;
                        s->retry_backoff_ms = 1000;
                        s->retry_due_ms = monotonic_ms() + s->retry_backoff_ms;
                    }
                    if (strcmp(s->failed_id, s->retry->id)) {
                        memcpy(s->failed_id, s->retry->id, 65);
                        resubscribe = true; /* one immediate backfill attempt */
                    }
                    fprintf(stderr, "syncd/subscription: pointer apply failed rc=%d\n", applied);
                }
            }
        } else if (p->kind == ITEM_EOSE) {
            slot->eose = true;
        } else if (p->kind == ITEM_CLOSED) {
            slot->eose = false;
            fprintf(stderr, "syncd/subscription: CLOSED from %s: %s\n",
                    s->urls[p->relay_index], p->reason ? p->reason : "");
        } else if (p->kind == ITEM_AUTH) {
            slot->eose = false;
            if (!nh_syncd_send_auth(slot->relay, s->secret, p->reason, slot->auth_id))
                fprintf(stderr, "syncd/subscription: AUTH signing failed for %s\n",
                        s->urls[p->relay_index]);
        } else if (p->kind == ITEM_OK && !strcmp(slot->auth_id, p->id)) {
            if (p->accepted) resubscribe = true;
            else fprintf(stderr, "syncd/subscription: AUTH rejected by %s: %s\n",
                         s->urls[p->relay_index], p->reason ? p->reason : "");
        }
        free_pending(p);
        p = next;
    }
    if (s->retry && nh_syncd_subscription_retry_delay_ms(s) == 0) {
        int rc = s->cb(s->cb_ud, s->retry->id, s->retry->created_at,
                       s->retry->d_tag, s->retry->content);
        if (rc == NH_SYNCD_OK) {
            memcpy(s->seen[s->seen_next++ % SEEN_IDS], s->retry->id, 65);
            free_pending(s->retry);
            s->retry = NULL;
            s->apply_failed = false;
            s->failed_id[0] = '\0';
            s->retry_backoff_ms = 0;
        } else {
            s->retry_backoff_ms = s->retry_backoff_ms < 30000 / 2 ?
                s->retry_backoff_ms * 2 : 30000;
            s->retry_due_ms = monotonic_ms() + s->retry_backoff_ms;
            fprintf(stderr, "syncd/subscription: pointer retry failed rc=%d\n", rc);
        }
    }
    if (resubscribe) {
        for (size_t i = 0; i < s->n_relays; ++i) s->slots[i].eose = false;
        nostr_simple_pool_stop(s->pool);
        nostr_simple_pool_subscribe_async(s->pool, (const char **)s->urls,
                                          s->n_relays, *s->filters, false);
        nostr_simple_pool_start(s->pool);
    }
    return overflow ? NH_SYNCD_ERR_IO : NH_SYNCD_OK;
}
void nh_syncd_subscription_stop(nh_syncd_subscription *s) {
    if (!s) return;
    if (s->pool) { nostr_simple_pool_stop(s->pool); nostr_simple_pool_free(s->pool); }
    free_pending(s->retry);
    pending *p = s->head;
    while (p) {
        pending *next = p->next;
        free(p->id); free(p->d_tag); free(p->content); free(p->reason); free(p);
        p = next;
    }
    if (s->filters) nostr_filters_free(s->filters);
    for (size_t i = 0; i < s->n_relays; ++i) free(s->urls ? s->urls[i] : NULL);
    free(s->urls); free(s->slots); free(s->pubkey); free(s->d_tag);
    if (s->secret) { memset(s->secret, 0, strlen(s->secret)); free(s->secret); }
    if (s->wake >= 0) close(s->wake);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

/* Keep the public recorder helper for callers that only track the latest
 * observed generation. The running daemon uses nh_syncd_pull_on_pointer. */
typedef struct {
    char *state_dir;
    int64_t max_seen;
    pthread_mutex_t mu;
} default_recorder_ctx;

static void write_remote_json(default_recorder_ctx *c) {
    char *tmp = NULL, *fin = NULL;
    if (asprintf(&tmp, "%s/remote.json.tmp", c->state_dir) < 0) return;
    if (asprintf(&fin, "%s/remote.json", c->state_dir) < 0) { free(tmp); return; }
    FILE *f = fopen(tmp, "w");
    if (f) {
        fprintf(f, "{\"remote_generation\":%lld}\n", (long long)c->max_seen);
        fflush(f);
        fsync(fileno(f));
        fclose(f);
        rename(tmp, fin);
    }
    free(tmp);
    free(fin);
}

int nh_syncd_default_remote_pointer_recorder(void *ud,
                                              const char *event_id_hex,
                                              int64_t created_at,
                                              const char *d_tag,
                                              const char *content_hex_or_b64) {
    (void)event_id_hex; (void)d_tag; (void)content_hex_or_b64;
    default_recorder_ctx *c = ud;
    if (!c) return NH_SYNCD_ERR_ARG;
    pthread_mutex_lock(&c->mu);
    if (created_at > c->max_seen) {
        c->max_seen = created_at;
        write_remote_json(c);
    }
    pthread_mutex_unlock(&c->mu);
    return NH_SYNCD_OK;
}
