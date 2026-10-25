/* SPDX-License-Identifier: MIT */
#include "vc.h"
#include <porthome/crypto.h>
#include <nostr-simple-pool.h>
#include <nostr-relay.h>
#include <nostr-envelope.h>
#include <nip42.h>
#include <nostr/nip11/nip11.h>
#include <curl/curl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    atomic_uint refs;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    NostrSimplePool *pool;
    NostrRelay *relay;
    int connected, authed, eose, closed, auth_closed, failed, ok;
    unsigned auth_reissues;
    char *challenge;
    char auth_id[65], publish_id[65];
    NostrEvent *events[VAULT_MAX_HEADS];
    size_t count;
    const NostrFilters *filter;
} session;
static void retain(session *s) { atomic_fetch_add(&s->refs, 1); }
static void release(void *ctx)
{
    session *s = ctx;
    if (atomic_fetch_sub(&s->refs, 1) != 1)
        return;
    for (size_t i = 0; i < s->count; i++)
        nostr_event_free(s->events[i]);
    free(s->challenge);
    pthread_cond_destroy(&s->cv);
    pthread_mutex_destroy(&s->mu);
    free(s);
}
static void state_cb(NostrRelay *r, NostrRelayConnectionState old, NostrRelayConnectionState next,
                     void *ctx)
{
    (void)r;
    (void)old;
    session *s = ctx;
    pthread_mutex_lock(&s->mu);
    s->connected = next == NOSTR_RELAY_STATE_CONNECTED;
    if (!s->connected) {
        s->eose = 0;
        s->authed = 0;
    }
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
}
static void auth_cb(NostrRelay *r, const char *challenge, void *ctx)
{
    (void)r;
    session *s = ctx;
    pthread_mutex_lock(&s->mu);
    char *c = challenge && strlen(challenge) <= 4096 ? strdup(challenge) : NULL;
    free(s->challenge);
    s->challenge = c;
    if (!c)
        s->failed = 1;
    s->authed = 0;
    s->auth_id[0] = 0;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
}
static void ok_cb(const char *id, bool ok, const char *reason, void *ctx)
{
    (void)reason;
    session *s = ctx;
    pthread_mutex_lock(&s->mu);
    if (id && !strcmp(id, s->auth_id)) {
        s->authed = ok;
        if (!ok)
            s->failed = 1;
    }
    if (id && !strcmp(id, s->publish_id))
        s->ok = ok ? 1 : -1;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
}
static void sub_cb(NostrRelay *r, const char *state, const char *reason, void *ctx)
{
    (void)r;
    (void)reason;
    session *s = ctx;
    pthread_mutex_lock(&s->mu);
    if (!strcmp(state, "EOSE"))
        s->eose = 1;
    else if (!strcmp(state, "CLOSED")) {
        if (reason && !strncmp(reason, "auth-required:", 14))
            s->auth_closed = 1;
        else
            s->closed = 1;
    }
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
}
static void event_cb(NostrIncomingEvent *incoming, void *ctx)
{
    session *s = ctx;
    NostrEvent *e = incoming->event;
    char id[65];
    pthread_mutex_lock(&s->mu);
    if (!e || !nostr_event_check_signature(e) || !s->filter ||
        !nostr_filters_match((NostrFilters *)s->filter, e) ||
        nostr_event_compute_id(e, id) != NOSTR_EVENT_VALIDATION_OK) {
        s->failed = 1;
    } else {
        size_t i;
        for (i = 0; i < s->count; i++) {
            char known_id[65];
            if (nostr_event_compute_id(s->events[i], known_id) != NOSTR_EVENT_VALIDATION_OK) {
                s->failed = 1;
                break;
            }
            if (!strcmp(id, known_id))
                break;
        }
        if (!s->failed && i == s->count) {
            if (s->count == VAULT_MAX_HEADS)
                s->failed = 1;
            else {
                s->events[s->count++] = e;
                e = NULL;
            }
        }
    }
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
    nostr_event_free(e);
}
static void close_session(session *s)
{
    if (!s)
        return;
    if (s->pool)
        nostr_simple_pool_stop(s->pool);
    if (s->relay) {
        nostr_relay_set_state_callback_full(s->relay, NULL, NULL, NULL);
        nostr_relay_set_auth_callback_full(s->relay, NULL, NULL, NULL);
        nostr_relay_set_ok_callback_full(s->relay, NULL, NULL, NULL);
    }
    if (s->pool)
        nostr_simple_pool_free(s->pool);
    if (s->relay)
        nostr_relay_unref(s->relay);
    release(s);
}
typedef struct {
    char *p;
    size_t n;
} document;
static size_t receive_doc(char *b, size_t size, size_t count, void *ctx)
{
    document *d = ctx;
    if (size && count > SIZE_MAX / size)
        return 0;
    size_t n = size * count;
    if (n > 65536 - d->n)
        return 0;
    char *p = realloc(d->p, d->n + n + 1);
    if (!p)
        return 0;
    d->p = p;
    memcpy(p + d->n, b, n);
    d->n += n;
    p[d->n] = 0;
    return n;
}
static int relay_info(vc_app *a, const char *url, int *auth)
{
    *auth = 0;
    char http[VAULT_MAX_URL + 8];
    if (!strncmp(url, "wss://", 6))
        snprintf(http, sizeof http, "https://%s", url + 6);
    else if (!strncmp(url, "ws://", 5))
        snprintf(http, sizeof http, "http://%s", url + 5);
    else
        return VAULT_ERR_ARG;
    CURL *c = curl_easy_init();
    if (!c)
        return VAULT_ERR_OOM;
    document d = {0};
    struct curl_slist *h = curl_slist_append(NULL, "Accept: application/nostr+json");
    curl_easy_setopt(c, CURLOPT_URL, http);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)a->options.deadline_ms);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, receive_doc);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &d);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_PROXY, "");
    CURLcode code = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    curl_slist_free_all(h);
    /* Unsupported NIP-11 is UNKNOWN, not proof of a public/private service. */
    if (code != CURLE_OK) {
        free(d.p);
        return VAULT_ERR_IO;
    }
    if (status == 200) {
        blossom_capability_state_t v;
        int rc = vault_vsp_relay_classify(d.p, &v);
        if (rc) {
            free(d.p);
            return rc;
        }
        fprintf(stderr, "relay advertised-private=%d (advertisement only)\n", v);
        /* Classification YES entails auth_required; ordinary auth-required relays
         * without vault:true still need authentication. Parse via NIP-11 below. */
        RelayInformationDocument *info = nostr_nip11_parse_info(d.p);
        if (!info) {
            free(d.p);
            return VAULT_ERR_ARG;
        }
        *auth = info->limitation && info->limitation->auth_required;
        nostr_nip11_free_info(info);
    } else if (status != 404 && status != 406 && status != 501) {
        free(d.p);
        return VAULT_ERR_IO;
    }
    free(d.p);
    return 0;
}
static struct timespec deadline(uint64_t ms)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += (time_t)(ms / 1000);
    t.tv_nsec += (long)(ms % 1000) * 1000000;
    if (t.tv_nsec >= 1000000000) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000;
    }
    return t;
}
static int send_auth(vc_app *a, session *s, const char *url, char *challenge)
{
    char *frame = NULL;
    int rc = VAULT_ERR_AUTH;
    if (!nip42_build_auth_response(challenge, url, a->secret, &frame))
        goto done;
    NostrEnvelope *env = nostr_envelope_parse(frame);
    if (!env || env->type != NOSTR_ENVELOPE_AUTH || !((NostrAuthEnvelope *)env)->event) {
        nostr_envelope_free(env);
        goto done;
    }
    char id[65];
    if (nostr_event_compute_id(((NostrAuthEnvelope *)env)->event, id) != NOSTR_EVENT_VALIDATION_OK) {
        nostr_envelope_free(env);
        goto done;
    }
    pthread_mutex_lock(&s->mu);
    memcpy(s->auth_id, id, 65);
    pthread_mutex_unlock(&s->mu);
    nostr_envelope_free(env);
    GoChannel *ch = nostr_relay_write(s->relay, frame);
    if (ch) {
        go_channel_close(ch);
        void *v = NULL;
        while (go_channel_try_receive(ch, &v) == 0) {
            if (v)
                free_error(v);
            v = NULL;
        }
        go_channel_unref(ch);
        rc = 0;
    }
done:
    free(frame);
    free(challenge);
    return rc;
}
/* Main-thread waiter keeps relay callbacks nonblocking. A deadline is failure,
 * never a substitute for EOSE, AUTH OK, or publish OK. */
static int wait_session(vc_app *a, session *s, const char *url, int mode, int needs_auth)
{
    struct timespec end = deadline(a->options.deadline_ms);
    int rc = 0;
    pthread_mutex_lock(&s->mu);
    for (;;) {
        if (s->failed || s->closed) {
            rc = VAULT_ERR_AUTH;
            break;
        }
        if (s->challenge) {
            char *c = s->challenge;
            s->challenge = NULL;
            pthread_mutex_unlock(&s->mu);
            rc = send_auth(a, s, url, c);
            pthread_mutex_lock(&s->mu);
            if (rc)
                break;
            continue;
        }
        if (mode == 1 && s->auth_closed && s->authed && s->filter) {
            if (s->auth_reissues++ >= 8) {
                rc = VAULT_ERR_AUTH;
                break;
            }
            s->auth_closed = 0;
            s->eose = 0;
            pthread_mutex_unlock(&s->mu);
            nostr_simple_pool_subscribe(s->pool, &url, 1, *s->filter, true);
            pthread_mutex_lock(&s->mu);
            continue;
        }
        if (mode == 0 && s->connected && (!(needs_auth || s->auth_id[0]) || s->authed))
            break;
        if (mode == 1 && s->eose && !s->auth_closed)
            break;
        if (mode == 2 && s->ok) {
            rc = s->ok > 0 ? 0 : VAULT_ERR_AUTH;
            break;
        }
        if (pthread_cond_timedwait(&s->cv, &s->mu, &end) == ETIMEDOUT) {
            rc = VAULT_ERR_IO;
            break;
        }
    }
    pthread_mutex_unlock(&s->mu);
    return rc;
}
static int open_session(vc_app *a, const char *url, session **out)
{
    *out = NULL;
    int auth = 0, rc = relay_info(a, url, &auth);
    if (rc)
        return rc;
    session *s = calloc(1, sizeof *s);
    if (!s)
        return VAULT_ERR_OOM;
    atomic_init(&s->refs, 1);
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    s->pool = nostr_simple_pool_new();
    Error *error = NULL;
    s->relay = nostr_relay_new(NULL, url, &error);
    if (error)
        free_error(error);
    if (!s->pool || !s->relay) {
        close_session(s);
        return VAULT_ERR_OOM;
    }
    retain(s);
    nostr_relay_set_state_callback_full(s->relay, state_cb, s, release);
    retain(s);
    nostr_relay_set_auth_callback_full(s->relay, auth_cb, s, release);
    retain(s);
    nostr_relay_set_ok_callback_full(s->relay, ok_cb, s, release);
    /* add_relay consumes the pool's retained reference in its current ABI. */
    nostr_relay_ref(s->relay);
    nostr_simple_pool_add_relay(s->pool, s->relay);
    nostr_simple_pool_set_subscription_callback(s->pool, sub_cb, s);
    nostr_simple_pool_set_event_middleware_ex(s->pool, event_cb, s);
    nostr_simple_pool_set_auto_unsub_on_eose(s->pool, false);
    error = NULL;
    if (!nostr_relay_connect(s->relay, &error)) {
        if (error)
            free_error(error);
        close_session(s);
        return VAULT_ERR_IO;
    }
    if (error)
        free_error(error);
    rc = wait_session(a, s, url, 0, auth);
    if (rc) {
        close_session(s);
        return rc;
    }
    nostr_simple_pool_start(s->pool);
    *out = s;
    return 0;
}
static int query(vc_app *a, NostrFilter *filter, int exact, const NostrEvent *target,
                 uint64_t *found)
{
    char target_id[65];
    if (exact && nostr_event_compute_id(target, target_id) != NOSTR_EVENT_VALIDATION_OK)
        return VAULT_ERR_AUTH;
    NostrFilters *filters = nostr_filters_new();
    if (!filters || !nostr_filters_add(filters, filter)) {
        nostr_filters_free(filters);
        return VAULT_ERR_OOM;
    }
    int rc = 0;
    for (size_t k = 0; k < a->state.relays_len; k++) {
        session *s = NULL;
        rc = open_session(a, a->state.relays[k], &s);
        if (rc)
            break;
        pthread_mutex_lock(&s->mu);
        s->filter = filters;
        pthread_mutex_unlock(&s->mu);
        const char *url = a->state.relays[k];
        nostr_simple_pool_subscribe(s->pool, &url, 1, *filters, true);
        rc = wait_session(a, s, url, 1, 0);
        /* EOSE notification precedes pool batch delivery. stop() joins the
         * current drain/flush iteration: the actual catch-up barrier. */
        nostr_simple_pool_stop(s->pool);
        pthread_mutex_lock(&s->mu);
        if (s->failed)
            rc = VAULT_ERR_AUTH;
        if (!rc)
            for (size_t i = 0; i < s->count; i++) {
                char id[65];
                if (nostr_event_compute_id(s->events[i], id) != NOSTR_EVENT_VALIDATION_OK) {
                    rc = VAULT_ERR_AUTH;
                    break;
                }
                if (exact) {
                    if (!strcmp(target_id, id))
                        *found |= UINT64_C(1) << k;
                } else {
                    int known = 0;
                    for (size_t j = 0; j < a->state.heads_len; j++) {
                        char known_id[65];
                        if (nostr_event_compute_id(a->state.heads[j].event, known_id) != NOSTR_EVENT_VALIDATION_OK) {
                            rc = VAULT_ERR_AUTH;
                            break;
                        }
                        if (!strcmp(known_id, id)) {
                            known = 1;
                            break;
                        }
                    }
                    if (rc)
                        break;
                    if (known)
                        continue;
                    porthome_ring_t ring;
                    const porthome_dir_node *root = vault_tree_node_at(a->state.tree, 0, &ring);
                    porthome_head *head = NULL;
                    /* Wire does not expose epoch: the query is scoped to the held
                     * current policy. Historical heads are local signed history. */
                    rc = porthome_head_event_open(s->events[i], root->vault->vault_id, &ring,
                                                  root->epoch, &head);
                    if (!rc)
                        rc = vc_head_add(a, s->events[i], head->epoch, NULL);
                    if (head) {
                        porthome_head_dispose(head);
                        free(head);
                    }
                    if (rc)
                        break;
                }
            }
        pthread_mutex_unlock(&s->mu);
        close_session(s);
        if (rc)
            break;
    }
    nostr_filters_free(filters);
    return rc;
}
int vc_query_id(vc_app *a, const NostrEvent *event, uint64_t *found)
{
    *found = 0;
    char id[65];
    if (nostr_event_compute_id(event, id) != NOSTR_EVENT_VALIDATION_OK)
        return VAULT_ERR_AUTH;
    NostrFilter *f = nostr_filter_new();
    if (!f)
        return VAULT_ERR_OOM;
    const char *ids[] = {id};
    nostr_filter_set_ids(f, ids, 1);
    nostr_filter_set_limit(f, 1);
    int rc = query(a, f, 1, event, found);
    nostr_filter_free(f);
    return rc;
}
int vc_query_heads(vc_app *a, const porthome_vault_info *v)
{
    if (v->writers_len + 1 > VAULT_MAX_HEADS)
        return VAULT_ERR_INCOMPLETE;
    char authors[VAULT_MAX_HEADS][65];
    const char *p[VAULT_MAX_HEADS];
    porthome_hex64(v->owner, authors[0]);
    p[0] = authors[0];
    for (size_t i = 0; i < v->writers_len; i++) {
        porthome_hex64(v->writers[i], authors[i + 1]);
        p[i + 1] = authors[i + 1];
    }
    NostrFilter *f = nostr_filter_new();
    if (!f)
        return VAULT_ERR_OOM;
    nostr_filter_set_authors(f, p, v->writers_len + 1);
    int kind = PORTHOME_HEAD_KIND;
    nostr_filter_set_kinds(f, &kind, 1);
    char d[PORTHOME_HEAD_D_LEN];
    porthome_head_d_tag(v->vault_id, d);
    nostr_filter_set_tags(f, nostr_tags_new(1, nostr_tag_new("d", d, NULL)));
    nostr_filter_set_limit(f, VAULT_MAX_HEADS);
    int rc = query(a, f, 0, NULL, NULL);
    nostr_filter_free(f);
    return rc;
}
int vc_publish(vc_app *a)
{
    vc_state *st = &a->state;
    if (!st->outgoing)
        return VAULT_ERR_ARG;
    char publish_id[65];
    if (nostr_event_compute_id(st->outgoing, publish_id) != NOSTR_EVENT_VALIDATION_OK)
        return VAULT_ERR_AUTH;
    int rc = 0;
    uint64_t all = (UINT64_C(1) << st->relays_len) - 1;
    if (st->intent) {
        if (!a->options.reconcile)
            return VAULT_ERR_AGAIN;
        uint64_t found = 0;
        rc = vc_query_id(a, st->outgoing, &found);
        if (rc)
            return rc;
        st->ack |= found;
        st->intent &= ~found;
        if ((rc = vc_state_save(a)))
            return rc;
        if (st->intent)
            return VAULT_ERR_AGAIN;
    }
    for (size_t i = 0; i < st->relays_len; i++) {
        uint64_t bit = UINT64_C(1) << i;
        if (st->ack & bit)
            continue;
        session *s = NULL;
        rc = open_session(a, st->relays[i], &s);
        if (rc)
            return rc;
        pthread_mutex_lock(&s->mu);
        memcpy(s->publish_id, publish_id, 65);
        pthread_mutex_unlock(&s->mu);
        st->intent |= bit;
        st->rejected &= ~bit;
        rc = vc_state_save(a);
        if (!rc) {
            nostr_relay_publish(s->relay, st->outgoing);
            rc = wait_session(a, s, st->relays[i], 2, 0);
            pthread_mutex_lock(&s->mu);
            if (s->ok) {
                st->intent &= ~bit;
                if (s->ok > 0)
                    st->ack |= bit;
                else
                    st->rejected |= bit;
            }
            pthread_mutex_unlock(&s->mu);
            int saved = vc_state_save(a);
            if (saved)
                rc = saved;
        }
        close_session(s);
        if (rc)
            return st->intent ? VAULT_ERR_AGAIN : rc;
    }
    return st->ack == all ? 0 : VAULT_ERR_AGAIN;
}
