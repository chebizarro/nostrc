/* NIP-42 client authentication session of a NostrRelay (nostrc-7i6w).
 *
 * One session per connection (see nostr-relay.h). This file owns the
 * registered signer, the attempt being signed (NostrRelayAuthRequest), the
 * local verification of a signed kind-22242 event, the ["AUTH",...] write and
 * the correlation of the relay's OK with that event id. relay.c calls in at
 * frame boundaries: a challenge (nostr_relay_auth_after_challenge), an OK
 * (nostr_relay_auth_on_ok), a refusal (nostr_relay_auth_on_refusal), a new
 * connection or challenge (nostr_relay_auth_reset_locked) and teardown.
 *
 * Lifetime of an attempt. relay->priv->auth_request holds one reference and
 * request->relay is non-NULL exactly while it does; both change only with the
 * request mutex and then the relay mutex held, or (stealing) under the relay
 * mutex followed by nostr_relay_auth_request_cancel_stolen(). A completion
 * that finds request->relay set may use the relay under the request mutex:
 * whoever stole the request from the relay waits for that mutex before the
 * relay can go (teardown is the first step of relay_free_impl()). It takes a
 * real reference, under the relay mutex, only if the relay's refcount is
 * still positive. The signer thread holds a request reference only, never the
 * relay. */
#include "nostr-relay.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr-kinds.h"
#include "relay-private.h"
#include "go.h"
#include "channel.h"
#include "error.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define AUTH_MAX_SKEW_SECONDS 600
#define AUTH_REQUIRED_PREFIX "auth-required:"

struct NostrRelayAuthSigner {
    atomic_int refs;
    NostrRelayAuthPolicy policy;
    uint64_t identity_generation;
    char *expected_pubkey;
    NostrRelayAuthSignFunc sign;
    void *user_data;
    NostrRelayDestroyNotify destroy;
};

struct NostrRelayAuthRequest {
    atomic_int refs;
    pthread_mutex_t mu;
    NostrRelay *relay;         /* mu; see the file comment */
    bool cancelled, completed; /* mu */
    NostrRelayAuthCancelFunc cancel_fn; /* mu */
    void *cancel_data;
    struct NostrRelayAuthSigner *signer; /* ref */
    NostrEvent *template;
    char *url;
    char *challenge;
    uint64_t auth_generation;
    uint64_t connection_generation;
};

const char *nostr_relay_auth_state_name(NostrRelayAuthState state) {
    switch (state) {
    case NOSTR_RELAY_AUTH_STATE_NONE: return "none";
    case NOSTR_RELAY_AUTH_STATE_CHALLENGED: return "challenged";
    case NOSTR_RELAY_AUTH_STATE_SIGNING: return "signing";
    case NOSTR_RELAY_AUTH_STATE_SENT: return "sent";
    case NOSTR_RELAY_AUTH_STATE_ACCEPTED: return "accepted";
    case NOSTR_RELAY_AUTH_STATE_REJECTED: return "rejected";
    case NOSTR_RELAY_AUTH_STATE_FAILED: return "failed";
    }
    return "unknown";
}

static void signer_unref(struct NostrRelayAuthSigner *signer) {
    if (!signer || atomic_fetch_sub(&signer->refs, 1) != 1) return;
    if (signer->destroy) signer->destroy(signer->user_data);
    free(signer->expected_pubkey);
    free(signer);
}

static struct NostrRelayAuthSigner *signer_ref(struct NostrRelayAuthSigner *signer) {
    if (signer) atomic_fetch_add(&signer->refs, 1);
    return signer;
}

NostrRelayAuthRequest *nostr_relay_auth_request_ref(NostrRelayAuthRequest *request) {
    if (request) atomic_fetch_add(&request->refs, 1);
    return request;
}

void nostr_relay_auth_request_unref(NostrRelayAuthRequest *request) {
    if (!request || atomic_fetch_sub(&request->refs, 1) != 1) return;
    signer_unref(request->signer);
    nostr_event_free(request->template);
    free(request->url);
    free(request->challenge);
    pthread_mutex_destroy(&request->mu);
    free(request);
}

const NostrEvent *nostr_relay_auth_request_get_event(const NostrRelayAuthRequest *request) {
    return request ? request->template : NULL;
}
const char *nostr_relay_auth_request_get_relay_url(const NostrRelayAuthRequest *request) {
    return request ? request->url : NULL;
}
const char *nostr_relay_auth_request_get_challenge(const NostrRelayAuthRequest *request) {
    return request ? request->challenge : NULL;
}
uint64_t nostr_relay_auth_request_get_auth_generation(const NostrRelayAuthRequest *request) {
    return request ? request->auth_generation : 0;
}
uint64_t nostr_relay_auth_request_get_connection_generation(const NostrRelayAuthRequest *request) {
    return request ? request->connection_generation : 0;
}
uint64_t nostr_relay_auth_request_get_identity_generation(const NostrRelayAuthRequest *request) {
    return request && request->signer ? request->signer->identity_generation : 0;
}

bool nostr_relay_auth_request_is_cancelled(NostrRelayAuthRequest *request) {
    if (!request) return true;
    pthread_mutex_lock(&request->mu);
    bool cancelled = request->cancelled;
    pthread_mutex_unlock(&request->mu);
    return cancelled;
}

void nostr_relay_auth_request_set_cancel_handler(NostrRelayAuthRequest *request,
                                                 NostrRelayAuthCancelFunc cancel,
                                                 void *user_data) {
    if (!request) return;
    pthread_mutex_lock(&request->mu);
    bool now = request->cancelled;
    if (!now && !request->completed) {
        request->cancel_fn = cancel;
        request->cancel_data = user_data;
    }
    pthread_mutex_unlock(&request->mu);
    if (now && cancel) cancel(request, user_data);
}

void nostr_relay_auth_request_cancel_stolen(NostrRelayAuthRequest *request) {
    if (!request) return;
    pthread_mutex_lock(&request->mu);
    request->relay = NULL;
    bool notify = !request->cancelled && !request->completed;
    request->cancelled = true;
    NostrRelayAuthCancelFunc fn = request->cancel_fn;
    void *data = request->cancel_data;
    request->cancel_fn = NULL;
    pthread_mutex_unlock(&request->mu);
    if (notify && fn) fn(request, data);
    nostr_relay_auth_request_unref(request); /* the relay's reference */
}

NostrRelayAuthRequest *nostr_relay_auth_reset_locked(NostrRelay *r, bool new_connection,
                                                    uint64_t sequence) {
    NostrRelayAuthRequest *stolen = r->priv->auth_request;
    r->priv->auth_request = NULL;
    r->priv->auth_state = new_connection ? NOSTR_RELAY_AUTH_STATE_NONE
                                         : NOSTR_RELAY_AUTH_STATE_CHALLENGED;
    r->priv->auth_state_sequence = sequence;
    r->priv->auth_event_id[0] = '\0';
    free(r->priv->auth_event_challenge);
    r->priv->auth_event_challenge = NULL;
    r->priv->auth_identity_generation = 0;
    if (new_connection) r->priv->auth_demanded = false;
    return stolen;
}

void nostr_relay_auth_teardown(NostrRelay *r) {
    if (!r || !r->priv) return;
    nsync_mu_lock(&r->priv->mutex);
    NostrRelayAuthRequest *stolen = r->priv->auth_request;
    r->priv->auth_request = NULL;
    struct NostrRelayAuthSigner *signer = r->priv->auth_signer;
    r->priv->auth_signer = NULL;
    nsync_mu_unlock(&r->priv->mutex);
    nostr_relay_auth_request_cancel_stolen(stolen);
    signer_unref(signer);
}

bool nostr_relay_get_auth_status(NostrRelay *relay, NostrRelayAuthStatus *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!relay || !relay->priv) return false;
    nsync_mu_lock(&relay->priv->mutex);
    out->state = (NostrRelayAuthState)relay->priv->auth_state;
    out->connection_generation = relay->priv->connection_generation;
    out->auth_generation = relay->priv->auth_generation;
    out->sequence = relay->priv->auth_state_sequence;
    memcpy(out->event_id, relay->priv->auth_event_id, sizeof(out->event_id));
    nsync_mu_unlock(&relay->priv->mutex);
    return true;
}

/* ---- verification ---------------------------------------------------- */

static const char *tag_value(const NostrEvent *ev, const char *key, size_t *count) {
    const char *value = NULL;
    *count = 0;
    size_t n = ev->tags ? nostr_tags_size(ev->tags) : 0;
    for (size_t i = 0; i < n; i++) {
        NostrTag *tag = nostr_tags_get(ev->tags, i);
        const char *k = tag ? nostr_tag_get_key(tag) : NULL;
        if (k && strcmp(k, key) == 0) {
            if (!(*count)++) value = nostr_tag_get_value(tag);
        }
    }
    return value;
}

/* Equal, or equal but for one trailing '/'. */
static bool same_relay_url(const char *a, const char *b) {
    if (!a || !b) return false;
    size_t la = strlen(a), lb = strlen(b);
    if (la && a[la - 1] == '/') la--;
    if (lb && b[lb - 1] == '/') lb--;
    return la == lb && strncmp(a, b, la) == 0;
}

/* Returns NULL when @ev is a valid answer to (@url, @challenge), else why. */
static const char *auth_event_problem(const NostrEvent *ev, const char *url,
                                      const char *challenge, const char *expected_pubkey,
                                      char id_out[65]) {
    if (!ev) return "no event";
    if (ev->kind != NOSTR_KIND_CLIENT_AUTHENTICATION) return "not kind 22242";
    if (nostr_event_validate(ev, id_out) != NOSTR_EVENT_VALIDATION_OK)
        return "invalid id or signature";
    size_t n = 0;
    const char *relay = tag_value(ev, "relay", &n);
    if (n != 1 || !same_relay_url(relay, url)) return "relay tag does not name this relay";
    const char *ch = tag_value(ev, "challenge", &n);
    if (n != 1 || !ch || !challenge || strcmp(ch, challenge) != 0)
        return "challenge tag does not answer the current challenge";
    int64_t now = (int64_t)time(NULL);
    if (ev->created_at > now + AUTH_MAX_SKEW_SECONDS || ev->created_at < now - AUTH_MAX_SKEW_SECONDS)
        return "created_at too far from now";
    if (expected_pubkey && (!ev->pubkey || strcmp(ev->pubkey, expected_pubkey) != 0))
        return "signed by another pubkey";
    return NULL;
}

/* ---- results ----------------------------------------------------------- */

static void emit_result(NostrRelay *r, NostrRelayAuthState state, const char *event_id,
                        const char *reason, const char *challenge, uint64_t identity,
                        const NostrRelayFrameInfo *frame) {
    NostrRelayAuthResult result = {
        .state = state,
        .event_id = event_id ? event_id : "",
        .reason = reason ? reason : "",
        .challenge = challenge ? challenge : "",
        .identity_generation = identity,
    };
    if (frame) {
        result.frame = *frame;
    } else {
        nsync_mu_lock(&r->priv->mutex);
        result.frame.connection_generation = r->priv->connection_generation;
        result.frame.auth_generation = r->priv->auth_generation;
        result.frame.auth_state = (NostrRelayAuthState)r->priv->auth_state;
        nsync_mu_unlock(&r->priv->mutex);
    }
    nostr_relay_emit_auth_result(r, &result);
}

/* Writes ["AUTH",ev] for @id. A write the relay refuses at once (closing,
 * queue gone) ends the attempt FAILED; a later socket failure surfaces as a
 * disconnect, which resets the session. Never waits for the socket. */
static bool transmit(NostrRelay *r, const NostrEvent *ev, const char *id,
                     const char *challenge, uint64_t identity) {
    char *json = nostr_event_serialize_compact(ev);
    char *frame = NULL;
    if (json) {
        size_t n = strlen(json) + 16;
        frame = malloc(n);
        if (frame) snprintf(frame, n, "[\"AUTH\",%s]", json);
    }
    free(json);
    const char *why = frame ? NULL : "could not serialize AUTH";
    if (frame) {
        GoChannel *answer = nostr_relay_write(r, frame);
        free(frame);
        void *immediate = NULL;
        if (!answer) why = "AUTH write could not be queued";
        else if (go_channel_try_receive(answer, &immediate) == 0 && immediate) {
            why = "AUTH write refused: connection closed";
            free_error(immediate);
        }
        nostr_relay_write_answer_release(answer);
    }
    if (!why) return true;
    bool current = false;
    nsync_mu_lock(&r->priv->mutex);
    if (r->priv->auth_state == NOSTR_RELAY_AUTH_STATE_SENT &&
        strcmp(r->priv->auth_event_id, id) == 0) {
        r->priv->auth_state = NOSTR_RELAY_AUTH_STATE_FAILED;
        current = true;
    }
    nsync_mu_unlock(&r->priv->mutex);
    if (current) emit_result(r, NOSTR_RELAY_AUTH_STATE_FAILED, id, why, challenge, identity, NULL);
    return false;
}

/* Under the relay mutex: may @ev (answering @challenge of @generation) be
 * sent now? On success the session is SENT and the caller holds a relay
 * reference. @request: the attempt it completes (must still be current), or
 * NULL for a caller-signed event (which supersedes an attempt in flight,
 * returned in *@stolen). */
static bool admit_locked(NostrRelay *r, NostrRelayAuthRequest *request, uint64_t generation,
                         const char *challenge, const char *id, uint64_t identity,
                         NostrRelayAuthRequest **stolen, const char **why) {
    *why = NULL;
    if (r->refcount <= 0 || r->priv->closing) { *why = "relay closing"; return false; }
    if (request && r->priv->auth_request != request) return false; /* stale: silent */
    if (generation != r->priv->auth_generation || !r->priv->challenge ||
        strcmp(r->priv->challenge, challenge) != 0) {
        *why = "challenge is no longer current";
        return false;
    }
    if (!request) {
        *stolen = r->priv->auth_request;
        r->priv->auth_request = NULL;
    }
    r->priv->auth_state = NOSTR_RELAY_AUTH_STATE_SENT;
    memcpy(r->priv->auth_event_id, id, 65);
    free(r->priv->auth_event_challenge);
    r->priv->auth_event_challenge = strdup(challenge);
    r->priv->auth_identity_generation = identity;
    r->refcount++;
    return true;
}

bool nostr_relay_send_auth(NostrRelay *relay, const NostrEvent *signed_event,
                           uint64_t auth_generation, Error **err) {
    if (!relay || !relay->priv || !signed_event) {
        if (err) *err = new_error(1, "relay and signed AUTH event are required");
        return false;
    }
    nsync_mu_lock(&relay->priv->mutex);
    uint64_t generation = auth_generation ? auth_generation : relay->priv->auth_generation;
    char *challenge = relay->priv->challenge ? strdup(relay->priv->challenge) : NULL;
    nsync_mu_unlock(&relay->priv->mutex);
    char id[65];
    const char *why = challenge ? auth_event_problem(signed_event, relay->url, challenge, NULL, id)
                                : "no AUTH challenge on this connection";
    NostrRelayAuthRequest *stolen = NULL;
    bool admitted = false;
    if (!why) {
        nsync_mu_lock(&relay->priv->mutex);
        admitted = admit_locked(relay, NULL, generation, challenge, id, 0, &stolen, &why);
        nsync_mu_unlock(&relay->priv->mutex);
    }
    nostr_relay_auth_request_cancel_stolen(stolen);
    bool sent = admitted && transmit(relay, signed_event, id, challenge, 0);
    if (admitted) {
        if (!sent && !why) why = "AUTH write failed";
        nostr_relay_unref(relay);
    }
    if (!sent && err) *err = new_error(1, "AUTH not sent: %s", why ? why : "unknown");
    free(challenge);
    return sent;
}

bool nostr_relay_auth_request_complete(NostrRelayAuthRequest *request,
                                       const NostrEvent *signed_event,
                                       const char *error_message) {
    if (!request) return false;
    pthread_mutex_lock(&request->mu);
    NostrRelay *r = request->relay;
    if (request->completed || request->cancelled || !r) {
        pthread_mutex_unlock(&request->mu);
        return false;
    }
    request->completed = true;
    request->cancel_fn = NULL;
    const char *expected = request->signer ? request->signer->expected_pubkey : NULL;
    uint64_t identity = nostr_relay_auth_request_get_identity_generation(request);
    char id[65] = "";
    const char *why = signed_event
        ? auth_event_problem(signed_event, request->url, request->challenge, expected, id)
        : (error_message && *error_message ? error_message : "signer refused");
    bool admitted = false, failed = false;
    nsync_mu_lock(&r->priv->mutex);
    if (!why) {
        NostrRelayAuthRequest *unused = NULL;
        const char *stale = NULL;
        admitted = admit_locked(r, request, request->auth_generation, request->challenge,
                                id, identity, &unused, &stale);
    } else if (r->refcount > 0 && r->priv->auth_request == request) {
        r->priv->auth_state = NOSTR_RELAY_AUTH_STATE_FAILED;
        r->refcount++;
        failed = true;
    }
    bool owned = (admitted || failed) && r->priv->auth_request == request;
    if (owned) r->priv->auth_request = NULL;
    nsync_mu_unlock(&r->priv->mutex);
    if (owned) request->relay = NULL;
    pthread_mutex_unlock(&request->mu);
    if (owned) nostr_relay_auth_request_unref(request); /* the relay's reference */

    bool sent = false;
    if (admitted) sent = transmit(r, signed_event, id, request->challenge, identity);
    if (failed) {
        char reason[256];
        snprintf(reason, sizeof(reason), "%s%s", signed_event ? "invalid AUTH event: " : "", why);
        emit_result(r, NOSTR_RELAY_AUTH_STATE_FAILED, signed_event ? id : "", reason,
                    request->challenge, identity, NULL);
    }
    if (admitted || failed) nostr_relay_unref(r);
    return sent;
}

bool nostr_relay_auth_request_complete_json(NostrRelayAuthRequest *request,
                                            const char *signed_event_json,
                                            const char *error_message) {
    if (!signed_event_json) return nostr_relay_auth_request_complete(request, NULL, error_message);
    NostrEvent *ev = nostr_event_new();
    bool parsed = ev && nostr_event_deserialize_signed(ev, signed_event_json, NULL) ==
                            NOSTR_EVENT_VALIDATION_OK;
    bool sent = nostr_relay_auth_request_complete(request, parsed ? ev : NULL,
                                                  parsed ? NULL : "signed AUTH event did not parse");
    nostr_event_free(ev);
    return sent;
}

/* ---- starting an attempt ---------------------------------------------- */

static void *sign_thread(void *arg) {
    NostrRelayAuthRequest *request = arg;
    request->signer->sign(request, request->signer->user_data);
    nostr_relay_auth_request_unref(request);
    return NULL;
}

static NostrRelayAuthRequest *request_new_locked(NostrRelay *r) {
    NostrRelayAuthRequest *request = calloc(1, sizeof(*request));
    NostrEvent *ev = request ? nostr_event_new() : NULL;
    if (!ev) { free(request); return NULL; }
    atomic_init(&request->refs, 1);
    pthread_mutex_init(&request->mu, NULL);
    request->url = strdup(r->url ? r->url : "");
    request->challenge = strdup(r->priv->challenge);
    request->auth_generation = r->priv->auth_generation;
    request->connection_generation = r->priv->connection_generation;
    request->signer = signer_ref(r->priv->auth_signer);
    ev->kind = NOSTR_KIND_CLIENT_AUTHENTICATION;
    ev->created_at = (int64_t)time(NULL);
    nostr_event_set_tags(ev, nostr_tags_new(2, nostr_tag_new("relay", request->url, NULL),
                                            nostr_tag_new("challenge", request->challenge, NULL)));
    nostr_event_set_content(ev, "");
    if (request->signer->expected_pubkey) nostr_event_set_pubkey(ev, request->signer->expected_pubkey);
    request->template = ev;
    request->relay = r;
    if (!request->url || !request->challenge || !ev->tags) {
        nostr_relay_auth_request_unref(request);
        return NULL;
    }
    return request;
}

/* Starts an attempt if the session allows one. @explicit_start: the
 * caller's nostr_relay_auth_start(), which may retry a held REJECTED/FAILED
 * challenge or authenticate a new identity on an ACCEPTED one. */
static bool maybe_start(NostrRelay *r, bool explicit_start, const char **why) {
    *why = NULL;
    nsync_mu_lock(&r->priv->mutex);
    struct NostrRelayAuthSigner *signer = r->priv->auth_signer;
    int state = r->priv->auth_state;
    bool allowed = state == NOSTR_RELAY_AUTH_STATE_CHALLENGED;
    if (explicit_start)
        allowed = allowed || state == NOSTR_RELAY_AUTH_STATE_REJECTED ||
                  state == NOSTR_RELAY_AUTH_STATE_FAILED ||
                  (state == NOSTR_RELAY_AUTH_STATE_ACCEPTED && signer &&
                   signer->identity_generation != r->priv->auth_identity_generation);
    else if (signer && signer->policy == NOSTR_RELAY_AUTH_ON_DEMAND)
        allowed = allowed && r->priv->auth_demanded;
    if (r->priv->closing) *why = "relay closing";
    else if (!signer) *why = "no AUTH signer for this relay";
    else if (!r->priv->challenge) *why = "no AUTH challenge on this connection";
    else if (r->priv->auth_request || state == NOSTR_RELAY_AUTH_STATE_SENT) *why = "AUTH already in flight";
    else if (!allowed) *why = state == NOSTR_RELAY_AUTH_STATE_ACCEPTED ? "already authenticated"
                                                                       : "AUTH held for this challenge";
    NostrRelayAuthRequest *request = NULL;
    if (!*why) {
        request = request_new_locked(r);
        if (!request) *why = "AUTH allocation failed";
    }
    if (request) {
        r->priv->auth_request = request; /* the relay's reference */
        r->priv->auth_state = NOSTR_RELAY_AUTH_STATE_SIGNING;
        r->priv->auth_event_id[0] = '\0';
        r->priv->auth_demanded = false;
        nostr_relay_auth_request_ref(request); /* the signer thread's */
    }
    nsync_mu_unlock(&r->priv->mutex);
    if (!request) return false;

    pthread_attr_t attr;
    pthread_t thread;
    bool spawned = pthread_attr_init(&attr) == 0;
    if (spawned) {
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        spawned = pthread_create(&thread, &attr, sign_thread, request) == 0;
        pthread_attr_destroy(&attr);
    }
    if (!spawned) {
        nostr_relay_auth_request_complete(request, NULL, "could not start the AUTH signer thread");
        nostr_relay_auth_request_unref(request);
    }
    return true;
}

void nostr_relay_auth_after_challenge(NostrRelay *r) {
    if (!r || !r->priv) return;
    const char *why;
    (void)maybe_start(r, false, &why);
}

void nostr_relay_auth_on_refusal(NostrRelay *r, const char *reason) {
    if (!r || !r->priv || !reason ||
        strncmp(reason, AUTH_REQUIRED_PREFIX, sizeof(AUTH_REQUIRED_PREFIX) - 1) != 0)
        return;
    /* A refusal while this connection is authenticated says the identity is
     * not enough; answering again with the same signer cannot help. */
    nsync_mu_lock(&r->priv->mutex);
    if (r->priv->auth_state != NOSTR_RELAY_AUTH_STATE_ACCEPTED) r->priv->auth_demanded = true;
    nsync_mu_unlock(&r->priv->mutex);
    const char *why;
    (void)maybe_start(r, false, &why);
}

bool nostr_relay_auth_on_ok(NostrRelay *r, const char *event_id, bool ok,
                            const char *reason, const NostrRelayFrameInfo *frame) {
    if (!r || !r->priv || !event_id || !*event_id) return false;
    char *challenge = NULL;
    uint64_t identity = 0;
    nsync_mu_lock(&r->priv->mutex);
    bool ours = r->priv->auth_state == NOSTR_RELAY_AUTH_STATE_SENT &&
                strcmp(r->priv->auth_event_id, event_id) == 0;
    if (ours) {
        r->priv->auth_state = ok ? NOSTR_RELAY_AUTH_STATE_ACCEPTED : NOSTR_RELAY_AUTH_STATE_REJECTED;
        r->priv->auth_state_sequence = frame ? frame->sequence : 0;
        challenge = r->priv->auth_event_challenge ? strdup(r->priv->auth_event_challenge) : NULL;
        identity = r->priv->auth_identity_generation;
    }
    nsync_mu_unlock(&r->priv->mutex);
    if (ours) {
        emit_result(r, ok ? NOSTR_RELAY_AUTH_STATE_ACCEPTED : NOSTR_RELAY_AUTH_STATE_REJECTED,
                    event_id, reason, challenge, identity, frame);
        free(challenge);
    }
    return ours;
}

bool nostr_relay_set_auth_signer_full(NostrRelay *relay, NostrRelayAuthPolicy policy,
                                      uint64_t identity_generation, const char *expected_pubkey,
                                      NostrRelayAuthSignFunc sign, void *user_data,
                                      NostrRelayDestroyNotify destroy) {
    if (!relay || !relay->priv) {
        if (destroy) destroy(user_data);
        return false;
    }
    struct NostrRelayAuthSigner *signer = NULL;
    if (sign) {
        signer = calloc(1, sizeof(*signer));
        if (signer && expected_pubkey) {
            signer->expected_pubkey = strdup(expected_pubkey);
            if (!signer->expected_pubkey) { free(signer); signer = NULL; }
        }
        if (!signer) {
            if (destroy) destroy(user_data);
            return false;
        }
        atomic_init(&signer->refs, 1);
        signer->policy = policy;
        signer->identity_generation = identity_generation;
        signer->sign = sign;
        signer->user_data = user_data;
        signer->destroy = destroy;
    } else if (destroy) {
        destroy(user_data);
    }
    nsync_mu_lock(&relay->priv->mutex);
    struct NostrRelayAuthSigner *old = relay->priv->auth_signer;
    relay->priv->auth_signer = signer;
    NostrRelayAuthRequest *stolen = relay->priv->auth_request;
    relay->priv->auth_request = NULL;
    /* An attempt of the old identity, signing or awaiting its OK, is forgotten. */
    if (relay->priv->auth_state == NOSTR_RELAY_AUTH_STATE_SIGNING ||
        relay->priv->auth_state == NOSTR_RELAY_AUTH_STATE_SENT) {
        relay->priv->auth_state = NOSTR_RELAY_AUTH_STATE_CHALLENGED;
        relay->priv->auth_event_id[0] = '\0';
    }
    nsync_mu_unlock(&relay->priv->mutex);
    nostr_relay_auth_request_cancel_stolen(stolen);
    signer_unref(old);
    if (signer && policy == NOSTR_RELAY_AUTH_ON_CHALLENGE) {
        const char *why;
        (void)maybe_start(relay, false, &why);
    }
    return true;
}

bool nostr_relay_auth_start(NostrRelay *relay, Error **err) {
    if (!relay || !relay->priv) {
        if (err) *err = new_error(1, "relay is NULL");
        return false;
    }
    const char *why = NULL;
    bool started = maybe_start(relay, true, &why);
    if (!started && err) *err = new_error(1, "AUTH not started: %s", why ? why : "unknown");
    return started;
}
