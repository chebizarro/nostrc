#include "nostr/nip46/nip46_types.h"
#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_bunker.h"
#include "nostr/nip46/nip46_msg.h"
#include "nostr/nip04.h"
#include "nostr/nip44/nip44.h"
#include "nostr-keys.h"
#include "nostr-event.h"
#include "nostr-simple-pool.h"
#include "nostr-relay.h"
#include "nostr-filter.h"
#include "nostr-tag.h"
#include "nostr-subscription.h"
#include "select.h"
#include "error.h"
#include "secure_buf.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>

/* Forward prototypes for local helpers */
static int csv_split(const char *csv, char ***out_vec, size_t *out_n);
static void csv_free(char **vec, size_t n);
static void acl_set_perms(NostrNip46Session *s, const char *client_pk, const char *perms_csv);
static int acl_has_perm(const NostrNip46Session *s, const char *client_pk, const char *method);

/* Pending RPC request entry - waiting for response from signer */
typedef struct PendingRequest {
    char *request_id;           /* RPC request ID to match response */
    GoChannel *response_chan;   /* Channel to send response to waiting caller */
    struct PendingRequest *next;
} PendingRequest;

struct NostrNip46Session {
    /* Session metadata */
    char *note;
    /* parsed URI fields */
    char *remote_pubkey_hex;   /* from bunker:// */
    char *client_pubkey_hex;   /* from nostrconnect:// */
    char *secret;              /* optional */
    char **relays; size_t n_relays;
    /* testing/transport placeholder */
    char *last_reply_json;
    /* bunker callbacks (optional) */
    NostrNip46BunkerCallbacks cbs;
    /* ACL: per-client allowed methods (simple list) */
    struct PermEntry { char *client_pk; char **methods; size_t n_methods; struct PermEntry *next; } *acl_head;

    /* Transport infrastructure for bunker mode */
    NostrSimplePool *pool;                /* relay pool for sending/receiving */
    char *bunker_pubkey_hex;              /* our bunker identity pubkey (x-only hex) */
    char *bunker_secret_hex;              /* our bunker identity secret key (hex) */
    int listening;                        /* whether bunker is actively listening */
    char *current_request_client_pubkey;  /* client pubkey for current request context */

    /* Client mode transport: persistent pool and subscription for RPC */
    NostrSimplePool *client_pool;         /* Persistent pool for client RPC calls */
    int client_pool_started;              /* Whether client pool is running */
    pthread_mutex_t pending_mutex;        /* Protects pending_requests */
    PendingRequest *pending_requests;     /* Linked list of pending RPC requests */
    char *derived_client_pubkey;          /* Client pubkey derived from secret */
};

/* Common helpers */
static NostrNip46Session *session_new(const char *note) {
    NostrNip46Session *s = (NostrNip46Session *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    pthread_mutex_init(&s->pending_mutex, NULL);
    if (note) s->note = strdup(note);
    return s;
}

/* Accept common public key encodings used across modules: 
 * - 64 hex (x-only) 
 * - 66 hex (33B compressed SEC1) 
 * - 130 hex (65B uncompressed SEC1)
 */
static int is_valid_pubkey_hex_relaxed(const char *hex) {
    if (!hex) return 0;
    size_t n = strlen(hex);
    if (!(n == 64 || n == 66 || n == 130)) return 0;
    for (size_t i=0;i<n;++i) {
        char c = hex[i];
        if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) return 0;
    }
    return 1;
}

/* --- Hex helpers and SEC1 -> x-only conversion --- */
static int hex_nibble(char c){
    if (c>='0' && c<='9') return c - '0';
    if (c>='a' && c<='f') return 10 + (c - 'a');
    if (c>='A' && c<='F') return 10 + (c - 'A');
    return -1;
}
static int hex_to_bytes_exact(const char *hex, unsigned char *out, size_t outlen){
    if (!hex || !out) return -1;
    size_t n = strlen(hex);
    if (n != outlen*2) return -1;
    for (size_t i=0;i<outlen;i++){
        int h = hex_nibble(hex[2*i]);
        int l = hex_nibble(hex[2*i+1]);
        if (h<0 || l<0) return -1;
        out[i] = (unsigned char)((h<<4) | l);
    }
    return 0;
}
/* Accept 64/66/130 hex and output 32-byte x-only pubkey */
static int parse_peer_xonly32(const char *hex, unsigned char out32[32]){
    if (!hex || !out32) return -1;
    size_t n = strlen(hex);
    if (n == 64){
        return hex_to_bytes_exact(hex, out32, 32);
    } else if (n == 66){
        unsigned char comp[33];
        if (hex_to_bytes_exact(hex, comp, 33) != 0) return -1;
        /* SEC1 compressed: first byte 0x02 or 0x03, next 32 are x */
        if (!(comp[0] == 0x02 || comp[0] == 0x03)) return -1;
        memcpy(out32, comp+1, 32);
        return 0;
    } else if (n == 130){
        unsigned char uncmp[65];
        if (hex_to_bytes_exact(hex, uncmp, 65) != 0) return -1;
        /* SEC1 uncompressed: first byte 0x04, next 32 are x, next 32 are y */
        if (uncmp[0] != 0x04) return -1;
        memcpy(out32, uncmp+1, 32);
        return 0;
    }
    return -1;
}
static int parse_sk32(const char *hex, unsigned char out32[32]){
    if (!hex || !out32) return -1;
    return hex_to_bytes_exact(hex, out32, 32);
}

void nostr_nip46_session_free(NostrNip46Session *s) {
    if (!s) return;
    if (s->note) { memset(s->note, 0, strlen(s->note)); free(s->note); }
    if (s->remote_pubkey_hex) { free(s->remote_pubkey_hex); }
    if (s->client_pubkey_hex) { free(s->client_pubkey_hex); }
    if (s->secret) { memset(s->secret, 0, strlen(s->secret)); free(s->secret); }
    if (s->relays) { for (size_t i=0;i<s->n_relays;++i) free(s->relays[i]); free(s->relays); }
    if (s->last_reply_json) free(s->last_reply_json);
    /* free ACL */
    struct PermEntry *it = s->acl_head; while (it) { struct PermEntry *nx = it->next; if (it->client_pk) free(it->client_pk); if (it->methods){ for(size_t i=0;i<it->n_methods;++i) free(it->methods[i]); free(it->methods);} free(it); it = nx; }
    /* free transport infrastructure */
    if (s->pool) {
        nostr_simple_pool_stop(s->pool);
        nostr_simple_pool_free(s->pool);
    }
    if (s->bunker_pubkey_hex) { free(s->bunker_pubkey_hex); }
    if (s->bunker_secret_hex) { memset(s->bunker_secret_hex, 0, strlen(s->bunker_secret_hex)); free(s->bunker_secret_hex); }
    if (s->current_request_client_pubkey) { free(s->current_request_client_pubkey); }
    /* free client mode transport */
    if (s->client_pool) {
        nostr_simple_pool_stop(s->client_pool);
        nostr_simple_pool_free(s->client_pool);
    }
    /* cancel and free pending requests */
    pthread_mutex_lock(&s->pending_mutex);
    PendingRequest *pr = s->pending_requests;
    while (pr) {
        PendingRequest *next = pr->next;
        if (pr->response_chan) {
            go_channel_close(pr->response_chan);
            go_channel_free(pr->response_chan);
        }
        free(pr->request_id);
        free(pr);
        pr = next;
    }
    s->pending_requests = NULL;
    pthread_mutex_unlock(&s->pending_mutex);
    pthread_mutex_destroy(&s->pending_mutex);
    if (s->derived_client_pubkey) { free(s->derived_client_pubkey); }
    free(s);
}

/* Client API */
NostrNip46Session *nostr_nip46_client_new(void) {
    return session_new("client");
}

int nostr_nip46_client_connect(NostrNip46Session *s,
                               const char *bunker_uri,
                               const char *requested_perms_csv) {
    (void)requested_perms_csv;
    if (!s || !bunker_uri) return -1;
    /* Reset stored fields */
    if (s->remote_pubkey_hex) { free(s->remote_pubkey_hex); s->remote_pubkey_hex=NULL; }
    if (s->client_pubkey_hex) { free(s->client_pubkey_hex); s->client_pubkey_hex=NULL; }
    if (s->secret) { memset(s->secret,0,strlen(s->secret)); free(s->secret); s->secret=NULL; }
    if (s->relays) { for(size_t i=0;i<s->n_relays;++i) free(s->relays[i]); free(s->relays); s->relays=NULL; s->n_relays=0; }

    if (strncmp(bunker_uri, "bunker://", 9) == 0) {
        NostrNip46BunkerURI u; if (nostr_nip46_uri_parse_bunker(bunker_uri, &u) != 0) return -1;
        if (!is_valid_pubkey_hex_relaxed(u.remote_signer_pubkey_hex)) { nostr_nip46_uri_bunker_free(&u); return -1; }
        s->remote_pubkey_hex = u.remote_signer_pubkey_hex; u.remote_signer_pubkey_hex=NULL;
        s->secret = u.secret; u.secret=NULL;
        s->relays = u.relays; s->n_relays = u.n_relays; u.relays=NULL; u.n_relays=0;
        fprintf(stderr, "[nip46] client_connect: parsed bunker URI, %zu relays:\n", s->n_relays);
        for (size_t i = 0; i < s->n_relays && s->relays; i++) {
            fprintf(stderr, "  relay[%zu]: %s\n", i, s->relays[i] ? s->relays[i] : "(null)");
        }
        nostr_nip46_uri_bunker_free(&u);
        return 0;
    } else if (strncmp(bunker_uri, "nostrconnect://", 15) == 0) {
        NostrNip46ConnectURI u; if (nostr_nip46_uri_parse_connect(bunker_uri, &u) != 0) return -1;
        if (!is_valid_pubkey_hex_relaxed(u.client_pubkey_hex)) { nostr_nip46_uri_connect_free(&u); return -1; }
        s->client_pubkey_hex = u.client_pubkey_hex; u.client_pubkey_hex=NULL;
        s->secret = u.secret; u.secret=NULL;
        s->relays = u.relays; s->n_relays = u.n_relays; u.relays=NULL; u.n_relays=0;
        nostr_nip46_uri_connect_free(&u);
        return 0;
    }
    return -1;
}

/* nostrc-rrfr: Set the signer's pubkey after receiving connect response */
int nostr_nip46_client_set_signer_pubkey(NostrNip46Session *s, const char *signer_pubkey_hex) {
    if (!s || !signer_pubkey_hex) return -1;
    if (strlen(signer_pubkey_hex) != 64) {
        fprintf(stderr, "[nip46] set_signer_pubkey: invalid pubkey length %zu (expected 64)\n",
                strlen(signer_pubkey_hex));
        return -1;
    }
    /* Free existing if any */
    if (s->remote_pubkey_hex) {
        free(s->remote_pubkey_hex);
    }
    s->remote_pubkey_hex = strdup(signer_pubkey_hex);
    if (!s->remote_pubkey_hex) return -1;
    fprintf(stderr, "[nip46] set_signer_pubkey: stored signer pubkey %s\n", signer_pubkey_hex);
    return 0;
}

/* nostrc-1wfi: Set the client's secret key directly for ECDH encryption.
 * This bypasses URI parsing and sets the secret that's used for NIP-04/NIP-44. */
int nostr_nip46_client_set_secret(NostrNip46Session *s, const char *secret_hex) {
    if (!s || !secret_hex) return -1;
    if (strlen(secret_hex) != 64) {
        fprintf(stderr, "[nip46] set_secret: invalid secret length %zu (expected 64)\n",
                strlen(secret_hex));
        return -1;
    }
    /* Validate it's actually a valid hex string */
    for (size_t i = 0; i < 64; i++) {
        char c = secret_hex[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            fprintf(stderr, "[nip46] set_secret: invalid hex character at position %zu\n", i);
            return -1;
        }
    }
    /* Clear and free existing secret */
    if (s->secret) {
        memset(s->secret, 0, strlen(s->secret));
        free(s->secret);
    }
    s->secret = strdup(secret_hex);
    if (!s->secret) return -1;
    fprintf(stderr, "[nip46] set_secret: stored client secret (%.4s...%s)\n",
            secret_hex, secret_hex + 60);
    return 0;
}

int nostr_nip46_client_get_public_key(NostrNip46Session *s, char **out_user_pubkey_hex) {
    if (!s || !out_user_pubkey_hex) return -1;
    /* If a client pubkey was provided (nostrconnect://), prefer it. */
    if (s->client_pubkey_hex) {
        size_t n = strlen(s->client_pubkey_hex);
        char *dup = (char*)malloc(n+1); if (!dup) return -1; memcpy(dup, s->client_pubkey_hex, n+1);
        *out_user_pubkey_hex = dup; return 0;
    }
    /* For bunker:// URIs, the remote_pubkey_hex IS the user's pubkey (the signer's key).
     * The secret= parameter in bunker URIs is an auth token, NOT a private key.
     * So we should return remote_pubkey_hex BEFORE trying to derive from secret. */
    if (s->remote_pubkey_hex) {
        size_t n = strlen(s->remote_pubkey_hex);
        char *dup = (char*)malloc(n+1); if (!dup) return -1; memcpy(dup, s->remote_pubkey_hex, n+1);
        *out_user_pubkey_hex = dup; return 0;
    }
    /* If we have our secret (and no remote pubkey), derive the x-only user pubkey.
     * This only applies when the session was initialized with set_secret() directly. */
    if (s->secret) {
        char *pk = nostr_key_get_public(s->secret);
        if (!pk) return -1;
        *out_user_pubkey_hex = pk; /* already allocated */
        return 0;
    }
    return -1;
}

/* Forward declaration for RPC helper used by sign_event and other calls */
static char *nip46_rpc_call(NostrNip46Session *s, const char *method,
                            const char **params, size_t n_params,
                            char **out_response_pubkey);

/* Request ID counter for unique IDs */
static unsigned int s_nip46_req_counter = 0;

int nostr_nip46_client_sign_event(NostrNip46Session *s, const char *event_json, char **out_signed_event_json) {
    if (!s) {
        fprintf(stderr, "[nip46] sign_event: ERROR -1: session is NULL\n");
        return -1;
    }
    if (!event_json) {
        fprintf(stderr, "[nip46] sign_event: ERROR -1: event_json is NULL\n");
        return -1;
    }
    if (!out_signed_event_json) {
        fprintf(stderr, "[nip46] sign_event: ERROR -1: out param is NULL\n");
        return -1;
    }
    *out_signed_event_json = NULL;

    fprintf(stderr, "[nip46] sign_event: signing event (%.50s...)\n", event_json);

    /* Use the common RPC helper which handles stale response retries */
    const char *params[1] = { event_json };
    char *result = nip46_rpc_call(s, "sign_event", params, 1, NULL);
    if (!result) {
        fprintf(stderr, "[nip46] sign_event: ERROR -1: RPC call failed\n");
        return -1;
    }

    fprintf(stderr, "[nip46] sign_event: SUCCESS - got signed event\n");
    *out_signed_event_json = result;
    return 0;
}

int nostr_nip46_client_ping(NostrNip46Session *s) {
    (void)s; return 0;
}

/* nostrc-nip46-rpc: Event-driven RPC helper using Go-style channel select.
 *
 * This function implements proper relay-based pub/sub messaging:
 * 1. Subscribe FIRST to all relays for responses tagged to our pubkey
 * 2. Publish the request to ALL relays
 * 3. Wait for events using go_select_timeout on subscription channels
 * 4. React to event PRESENCE (response arrived) or ABSENCE (EOSE with no match)
 *
 * This avoids treating relays like REST APIs with arbitrary timeouts.
 * The presence or absence of events drives the flow, not wall-clock time.
 *
 * Returns the "result" field from the response on success, or NULL on error.
 * If out_response_pubkey is non-NULL, it receives the pubkey of the responding event.
 * Caller must free the returned strings. */
static char *nip46_rpc_call(NostrNip46Session *s, const char *method,
                            const char **params, size_t n_params,
                            char **out_response_pubkey) {
    if (out_response_pubkey) *out_response_pubkey = NULL;
    if (!s || !method) return NULL;

    /* Validate session state */
    const char *peer = s->remote_pubkey_hex;
    if (!peer) {
        fprintf(stderr, "[nip46] %s: ERROR: no remote pubkey in session\n", method);
        return NULL;
    }
    if (!s->secret) {
        fprintf(stderr, "[nip46] %s: ERROR: no secret key in session\n", method);
        return NULL;
    }
    if (s->n_relays == 0 || !s->relays) {
        fprintf(stderr, "[nip46] %s: ERROR: no relays in session\n", method);
        return NULL;
    }

    fprintf(stderr, "[nip46] %s: building request\n", method);

    /* Build request JSON with unique ID */
    char req_id[32];
    snprintf(req_id, sizeof(req_id), "%lx_%u", (unsigned long)time(NULL), ++s_nip46_req_counter);
    fprintf(stderr, "[nip46] %s: request id = %s\n", method, req_id);
    char *req = nostr_nip46_request_build(req_id, method, params, n_params);
    if (!req) {
        fprintf(stderr, "[nip46] %s: ERROR: failed to build request JSON\n", method);
        return NULL;
    }

    /* Parse keys */
    unsigned char sk[32];
    if (parse_sk32(s->secret, sk) != 0) {
        fprintf(stderr, "[nip46] %s: ERROR: failed to parse secret key\n", method);
        free(req);
        return NULL;
    }
    unsigned char peer_pk[32];
    if (parse_peer_xonly32(peer, peer_pk) != 0) {
        fprintf(stderr, "[nip46] %s: ERROR: failed to parse peer pubkey\n", method);
        secure_wipe(sk, sizeof(sk));
        free(req);
        return NULL;
    }

    /* Encrypt request using NIP-44 */
    char *cipher = NULL;
    if (nostr_nip44_encrypt_v2(sk, peer_pk, (const uint8_t *)req, strlen(req), &cipher) != 0 || !cipher) {
        fprintf(stderr, "[nip46] %s: ERROR: NIP-44 encryption failed\n", method);
        secure_wipe(sk, sizeof(sk));
        free(req);
        return NULL;
    }
    free(req);

    /* Derive our client pubkey */
    char *client_pubkey = nostr_key_get_public(s->secret);
    if (!client_pubkey) {
        fprintf(stderr, "[nip46] %s: ERROR: failed to derive client pubkey\n", method);
        secure_wipe(sk, sizeof(sk));
        free(cipher);
        return NULL;
    }

    /* Build kind 24133 request event */
    NostrEvent *req_ev = nostr_event_new();
    nostr_event_set_kind(req_ev, NOSTR_EVENT_KIND_NIP46);
    nostr_event_set_content(req_ev, cipher);
    nostr_event_set_created_at(req_ev, (int64_t)time(NULL));
    nostr_event_set_pubkey(req_ev, client_pubkey);

    NostrTags *tags = nostr_tags_new(1, nostr_tag_new("p", peer, NULL));
    nostr_event_set_tags(req_ev, tags);

    /* Sign the request event */
    nostr_secure_buf sb = secure_alloc(32);
    if (!sb.ptr) {
        fprintf(stderr, "[nip46] %s: ERROR: failed to allocate secure buffer\n", method);
        secure_wipe(sk, sizeof(sk));
        free(client_pubkey);
        free(cipher);
        nostr_event_free(req_ev);
        return NULL;
    }
    memcpy(sb.ptr, sk, 32);

    if (nostr_event_sign_secure(req_ev, &sb) != 0) {
        fprintf(stderr, "[nip46] %s: ERROR: failed to sign request event\n", method);
        secure_free(&sb);
        secure_wipe(sk, sizeof(sk));
        free(client_pubkey);
        free(cipher);
        nostr_event_free(req_ev);
        return NULL;
    }
    secure_free(&sb);
    free(cipher);

    fprintf(stderr, "[nip46] %s: signed request, connecting to %zu relay(s)\n", method, s->n_relays);

    /* Build filter for responses: kind 24133 tagged to our pubkey */
    NostrFilters *filters = nostr_filters_new();
    NostrFilter *f = nostr_filter_new();
    int kinds[] = { NOSTR_EVENT_KIND_NIP46 };
    nostr_filter_set_kinds(f, kinds, 1);
    NostrTags *filter_tags = nostr_tags_new(1, nostr_tag_new("p", client_pubkey, NULL));
    nostr_filter_set_tags(f, filter_tags);
    nostr_filter_set_since_i64(f, (int64_t)time(NULL) - 60);  /* 60s clock skew buffer */
    NostrFilter f_copy = *f;
    free(f);
    nostr_filters_add(filters, &f_copy);

    /* Create subscriptions for each relay and collect their channels */
    NostrSubscription **subs = (NostrSubscription **)calloc(s->n_relays, sizeof(NostrSubscription *));
    NostrRelay **relays = (NostrRelay **)calloc(s->n_relays, sizeof(NostrRelay *));
    size_t active_count = 0;

    for (size_t i = 0; i < s->n_relays; i++) {
        Error *relay_err = NULL;
        NostrRelay *relay = nostr_relay_new(NULL, s->relays[i], &relay_err);
        if (!relay) {
            if (relay_err) free_error(relay_err);
            continue;
        }

        /* Connect to relay */
        Error *conn_err = NULL;
        if (!nostr_relay_connect(relay, &conn_err)) {
            fprintf(stderr, "[nip46] %s: failed to connect to %s\n", method, s->relays[i]);
            if (conn_err) free_error(conn_err);
            nostr_relay_free(relay);
            continue;
        }

        /* Wait for handshake with small timeout */
        int wait_ms = 0;
        while (!nostr_relay_is_established(relay) && wait_ms < 3000) {
            usleep(50000);
            wait_ms += 50;
        }
        if (!nostr_relay_is_established(relay)) {
            fprintf(stderr, "[nip46] %s: handshake timeout for %s\n", method, s->relays[i]);
            nostr_relay_disconnect(relay);
            nostr_relay_free(relay);
            continue;
        }

        /* Create subscription on this relay */
        NostrSubscription *sub = nostr_subscription_new(relay, filters);
        if (!sub) {
            nostr_relay_disconnect(relay);
            nostr_relay_free(relay);
            continue;
        }

        /* Fire the subscription to start receiving events */
        Error *err = NULL;
        if (!nostr_subscription_fire(sub, &err)) {
            fprintf(stderr, "[nip46] %s: subscription fire failed for %s\n", method, s->relays[i]);
            nostr_subscription_free(sub);
            nostr_relay_disconnect(relay);
            nostr_relay_free(relay);
            continue;
        }

        relays[active_count] = relay;
        subs[active_count] = sub;
        active_count++;
        fprintf(stderr, "[nip46] %s: subscribed to %s\n", method, s->relays[i]);
    }

    nostr_filters_free(filters);

    if (active_count == 0) {
        fprintf(stderr, "[nip46] %s: ERROR: no relays connected\n", method);
        free(subs);
        free(relays);
        nostr_event_free(req_ev);
        secure_wipe(sk, sizeof(sk));
        free(client_pubkey);
        return NULL;
    }

    /* Publish request to ALL connected relays */
    for (size_t i = 0; i < active_count; i++) {
        fprintf(stderr, "[nip46] %s: publishing to %s\n", method, nostr_relay_get_url_const(relays[i]));
        nostr_relay_publish(relays[i], req_ev);
    }
    nostr_event_free(req_ev);

    /* Build select cases for all subscription event channels and EOSE channels */
    size_t num_cases = active_count * 2;  /* events + eose for each relay */
    GoSelectCase *cases = (GoSelectCase *)calloc(num_cases, sizeof(GoSelectCase));
    void **recv_bufs = (void **)calloc(num_cases, sizeof(void *));

    for (size_t i = 0; i < active_count; i++) {
        /* Events channel */
        cases[i * 2].op = GO_SELECT_RECEIVE;
        cases[i * 2].chan = nostr_subscription_get_events_channel(subs[i]);
        cases[i * 2].recv_buf = &recv_bufs[i * 2];

        /* EOSE channel */
        cases[i * 2 + 1].op = GO_SELECT_RECEIVE;
        cases[i * 2 + 1].chan = nostr_subscription_get_eose_channel(subs[i]);
        cases[i * 2 + 1].recv_buf = &recv_bufs[i * 2 + 1];
    }

    /* Wait for events using go_select_timeout.
     * We react to:
     * - Event arrival: check if it matches our request ID
     * - EOSE: relay has sent all stored events, new events may still arrive
     * - Timeout: no response within reasonable time (only as last resort) */
    char *result = NULL;
    char *response_pubkey = NULL;
    int eose_count = 0;
    int max_events = 20;  /* Process up to 20 events before giving up */

    for (int event_num = 0; event_num < max_events; event_num++) {
        /* Use go_select_timeout to wait for any channel to be ready.
         * After EOSE from all relays, use shorter timeout since we expect
         * the response to arrive quickly as a new event. */
        uint64_t timeout_ms = (eose_count >= (int)active_count) ? 5000 : 30000;

        GoSelectResult sel = go_select_timeout(cases, num_cases, timeout_ms);

        if (sel.selected_case == -1) {
            /* Timeout - no events arrived */
            if (eose_count >= (int)active_count) {
                fprintf(stderr, "[nip46] %s: timeout after EOSE - no matching response\n", method);
            } else {
                fprintf(stderr, "[nip46] %s: timeout waiting for events\n", method);
            }
            break;
        }

        if (!sel.ok) {
            /* Channel closed */
            continue;
        }

        size_t case_idx = (size_t)sel.selected_case;

        /* Check if this is an EOSE notification */
        if (case_idx % 2 == 1) {
            eose_count++;
            fprintf(stderr, "[nip46] %s: EOSE from relay %zu (%d/%zu)\n",
                    method, case_idx / 2, eose_count, active_count);
            continue;
        }

        /* This is an event - process it */
        NostrEvent *ev = (NostrEvent *)recv_bufs[case_idx];
        if (!ev) continue;

        const char *content = nostr_event_get_content(ev);
        const char *sender_pubkey = nostr_event_get_pubkey(ev);

        if (!content || !sender_pubkey) {
            nostr_event_free(ev);
            continue;
        }

        fprintf(stderr, "[nip46] %s: received event from %s\n", method, sender_pubkey);

        /* Decrypt response */
        unsigned char resp_pk[32];
        if (parse_peer_xonly32(sender_pubkey, resp_pk) != 0) {
            fprintf(stderr, "[nip46] %s: invalid sender pubkey, skipping\n", method);
            nostr_event_free(ev);
            continue;
        }

        uint8_t *plaintext = NULL;
        size_t plaintext_len = 0;

        /* Detect encryption format: NIP-04 contains "?iv=" */
        int is_nip04 = (strstr(content, "?iv=") != NULL);

        if (is_nip04) {
            char *plaintext_str = NULL;
            char *error_msg = NULL;
            if (nostr_nip04_decrypt(content, sender_pubkey, s->secret,
                                    &plaintext_str, &error_msg) != 0 || !plaintext_str) {
                fprintf(stderr, "[nip46] %s: NIP-04 decrypt failed, skipping\n", method);
                free(error_msg);
                nostr_event_free(ev);
                continue;
            }
            free(error_msg);
            plaintext_len = strlen(plaintext_str);
            plaintext = (uint8_t *)plaintext_str;
        } else {
            if (nostr_nip44_decrypt_v2(sk, resp_pk, content, &plaintext, &plaintext_len) != 0 || !plaintext) {
                fprintf(stderr, "[nip46] %s: NIP-44 decrypt failed, skipping\n", method);
                nostr_event_free(ev);
                continue;
            }
        }

        nostr_event_free(ev);

        /* Parse JSON response */
        char *response_json = (char *)malloc(plaintext_len + 1);
        if (!response_json) {
            free(plaintext);
            continue;
        }
        memcpy(response_json, plaintext, plaintext_len);
        response_json[plaintext_len] = '\0';
        free(plaintext);

        fprintf(stderr, "[nip46] %s: decrypted: %.100s...\n", method, response_json);

        if (!nostr_json_is_valid(response_json)) {
            fprintf(stderr, "[nip46] %s: invalid JSON, skipping\n", method);
            free(response_json);
            continue;
        }

        /* Check if response ID matches our request */
        char *resp_id = NULL;
        if (nostr_json_get_string(response_json, "id", &resp_id) == 0 && resp_id) {
            if (strcmp(resp_id, req_id) != 0) {
                fprintf(stderr, "[nip46] %s: stale response id '%s' != expected '%s', skipping\n",
                        method, resp_id, req_id);
                free(resp_id);
                free(response_json);
                continue;
            }
            free(resp_id);
        }

        /* Check for error in response */
        char *err_msg = NULL;
        if (nostr_json_has_key(response_json, "error") &&
            nostr_json_get_type(response_json, "error") == NOSTR_JSON_STRING &&
            nostr_json_get_string(response_json, "error", &err_msg) == 0 && err_msg && *err_msg) {
            fprintf(stderr, "[nip46] %s: ERROR: signer error: %s\n", method, err_msg);
            free(err_msg);
            free(response_json);
            /* This is a valid response (matching ID) but it's an error - stop processing */
            break;
        }
        free(err_msg);

        /* Get the result */
        if (nostr_json_get_string(response_json, "result", &result) != 0 || !result) {
            fprintf(stderr, "[nip46] %s: no result field, skipping\n", method);
            free(response_json);
            continue;
        }
        free(response_json);

        /* Success! Save sender pubkey if caller wants it */
        response_pubkey = strdup(sender_pubkey);
        fprintf(stderr, "[nip46] %s: SUCCESS - result: %.50s\n", method, result);
        break;
    }

    /* Cleanup */
    free(cases);
    free(recv_bufs);

    for (size_t i = 0; i < active_count; i++) {
        nostr_subscription_unsubscribe(subs[i]);
        nostr_subscription_free(subs[i]);
        nostr_relay_disconnect(relays[i]);
        nostr_relay_free(relays[i]);
    }
    free(subs);
    free(relays);

    secure_wipe(sk, sizeof(sk));
    free(client_pubkey);

    if (out_response_pubkey) {
        *out_response_pubkey = response_pubkey;
    } else {
        free(response_pubkey);
    }

    return result;
}

/* nostrc-nip46-rpc: Send connect RPC to remote signer.
 * This must be called after parsing bunker:// URI but before other operations.
 * The session must have: remote_pubkey_hex, secret (client key), relays.
 * On success, returns "ack" or the connect secret. Caller must free. */
int nostr_nip46_client_connect_rpc(NostrNip46Session *s,
                                   const char *connect_secret,
                                   const char *perms,
                                   char **out_result) {
    if (!s || !out_result) return -1;
    *out_result = NULL;

    /* Build connect params: [remote_signer_pubkey, optional_secret, optional_perms] */
    const char *params[3];
    size_t n_params = 0;

    if (!s->remote_pubkey_hex) {
        fprintf(stderr, "[nip46] connect_rpc: ERROR: no remote_pubkey_hex\n");
        return -1;
    }
    params[n_params++] = s->remote_pubkey_hex;
    params[n_params++] = connect_secret ? connect_secret : "";
    params[n_params++] = perms ? perms : "";

    /* Note: Do NOT update remote_pubkey_hex here. For bunker:// flow,
     * the signer listens for messages tagged with the URI's pubkey.
     * Only nostrconnect:// flow should update the pubkey (done in login code). */
    char *result = nip46_rpc_call(s, "connect", params, n_params, NULL);
    if (!result) {
        return -1;
    }

    *out_result = result;
    return 0;
}

/* nostrc-nip46-rpc: Send get_public_key RPC to remote signer.
 * Returns the user's actual pubkey (may differ from remote_signer_pubkey).
 * On success, returns hex pubkey. Caller must free. */
int nostr_nip46_client_get_public_key_rpc(NostrNip46Session *s, char **out_user_pubkey_hex) {
    if (!s || !out_user_pubkey_hex) return -1;
    *out_user_pubkey_hex = NULL;

    char *result = nip46_rpc_call(s, "get_public_key", NULL, 0, NULL);
    if (!result) {
        return -1;
    }

    /* Validate it looks like a pubkey (64 hex chars) */
    size_t len = strlen(result);
    if (len != 64) {
        fprintf(stderr, "[nip46] get_public_key_rpc: ERROR: invalid pubkey length %zu\n", len);
        free(result);
        return -1;
    }

    *out_user_pubkey_hex = result;
    return 0;
}

int nostr_nip46_client_nip04_encrypt(NostrNip46Session *s, const char *peer_pubkey_hex, const char *plaintext, char **out_ciphertext) {
    if (!s || !peer_pubkey_hex || !plaintext || !out_ciphertext) return -1;
    if (!s->secret) return -1;
    char *cipher = NULL; char *err = NULL;
    nostr_secure_buf sb = secure_alloc(32);
    if (!sb.ptr || parse_sk32(s->secret, (unsigned char*)sb.ptr) != 0) { if (sb.ptr) secure_free(&sb); return -1; }
    if (nostr_nip04_encrypt_secure(plaintext, peer_pubkey_hex, &sb, &cipher, &err) != 0 || !cipher) {
        secure_free(&sb);
        if (err) free(err);
        return -1;
    }
    secure_free(&sb);
    *out_ciphertext = cipher;
    return 0;
}

int nostr_nip46_client_nip04_decrypt(NostrNip46Session *s, const char *peer_pubkey_hex, const char *ciphertext, char **out_plaintext) {
    if (!s || !peer_pubkey_hex || !ciphertext || !out_plaintext) return -1;
    if (!s->secret) return -1;
    char *plain = NULL; char *err = NULL;
    nostr_secure_buf sb = secure_alloc(32);
    if (!sb.ptr || parse_sk32(s->secret, (unsigned char*)sb.ptr) != 0) { if (sb.ptr) secure_free(&sb); return -1; }
    if (nostr_nip04_decrypt_secure(ciphertext, peer_pubkey_hex, &sb, &plain, &err) != 0 || !plain) {
        secure_free(&sb);
        if (err) free(err);
        return -1;
    }
    secure_free(&sb);
    *out_plaintext = plain;
    return 0;
}

int nostr_nip46_client_nip44_encrypt(NostrNip46Session *s, const char *peer_pubkey_hex, const char *plaintext, char **out_ciphertext) {
    if (!s || !peer_pubkey_hex || !plaintext || !out_ciphertext) return -1;
    *out_ciphertext = NULL;
    if (!s->secret) return -1;
    unsigned char sk[32];
    if (parse_sk32(s->secret, sk) != 0) return -1;
    unsigned char pkx[32];
    if (parse_peer_xonly32(peer_pubkey_hex, pkx) != 0) { secure_wipe(sk, sizeof sk); return -1; }
    char *b64 = NULL;
    if (nostr_nip44_encrypt_v2(sk, pkx, (const uint8_t*)plaintext, strlen(plaintext), &b64) != 0 || !b64) {
        secure_wipe(sk, sizeof sk); return -1;
    }
    secure_wipe(sk, sizeof sk);
    *out_ciphertext = b64;
    return 0;
}
int nostr_nip46_client_nip44_decrypt(NostrNip46Session *s, const char *peer_pubkey_hex, const char *ciphertext, char **out_plaintext) {
    if (!s || !peer_pubkey_hex || !ciphertext || !out_plaintext) return -1;
    *out_plaintext = NULL;
    if (!s->secret) return -1;
    unsigned char sk[32];
    if (parse_sk32(s->secret, sk) != 0) return -1;
    unsigned char peer_x[32];
    if (parse_peer_xonly32(peer_pubkey_hex, peer_x) != 0) { secure_wipe(sk, sizeof sk); return -1; }
    uint8_t *plain = NULL; size_t plain_len = 0;
    if (nostr_nip44_decrypt_v2(sk, peer_x, ciphertext, &plain, &plain_len) != 0 || !plain) {
        secure_wipe(sk, sizeof sk); return -1;
    }
    secure_wipe(sk, sizeof sk);
    /* Ensure NUL-terminated C-string for convenience */
    char *out = (char*)malloc(plain_len + 1);
    if (!out) { free(plain); return -1; }
    memcpy(out, plain, plain_len);
    out[plain_len] = '\0';
    free(plain);
    *out_plaintext = out;
    return 0;
}

/* Bunker API */
NostrNip46Session *nostr_nip46_bunker_new(const NostrNip46BunkerCallbacks *cbs) {
    NostrNip46Session *s = session_new("bunker");
    if (!s) return NULL;
    if (cbs) {
        s->cbs = *cbs; /* shallow copy of function pointers and user_data */
    } else {
        memset(&s->cbs, 0, sizeof(s->cbs));
    }
    return s;
}

/* Callback for incoming NIP-46 events from the relay pool */
static void nip46_event_middleware(NostrIncomingEvent *incoming) {
    /* Note: This callback handles incoming kind 24133 events.
     * The actual request handling is done via nostr_nip46_bunker_handle_cipher
     * which is typically called by higher-level code that receives these events.
     * For now, we log the incoming event for debugging purposes.
     * Full async processing would require storing a session reference and
     * integrating with an event loop (GLib, libevent, etc). */
    if (!incoming || !incoming->event) return;

    NostrEvent *ev = incoming->event;
    int kind = nostr_event_get_kind(ev);

    if (kind == NOSTR_EVENT_KIND_NIP46 && getenv("NOSTR_DEBUG")) {
        const char *id = ev->id;
        const char *pubkey = nostr_event_get_pubkey(ev);
        fprintf(stderr, "[nip46] received kind %d event id=%s from=%s\n",
                kind, id ? id : "(null)", pubkey ? pubkey : "(null)");
    }

    /* Event ownership: the pool will free the event after callback returns */
}

int nostr_nip46_bunker_listen(NostrNip46Session *s, const char *const *relays, size_t n_relays) {
    if (!s || !relays || n_relays == 0) return -1;

    /* Bunker requires a secret key for decryption and signing */
    if (!s->secret) {
        if (getenv("NOSTR_DEBUG")) {
            fprintf(stderr, "[nip46] bunker_listen: no secret set, cannot listen\n");
        }
        return -1;
    }

    /* Derive bunker public key from secret if not already set */
    if (!s->bunker_pubkey_hex) {
        char *pk = nostr_key_get_public(s->secret);
        if (!pk) {
            if (getenv("NOSTR_DEBUG")) {
                fprintf(stderr, "[nip46] bunker_listen: failed to derive pubkey\n");
            }
            return -1;
        }
        s->bunker_pubkey_hex = pk;
    }

    /* Store secret hex for transport operations */
    if (!s->bunker_secret_hex && s->secret) {
        s->bunker_secret_hex = strdup(s->secret);
    }

    /* Create relay pool if not already created */
    if (!s->pool) {
        s->pool = nostr_simple_pool_new();
        if (!s->pool) {
            if (getenv("NOSTR_DEBUG")) {
                fprintf(stderr, "[nip46] bunker_listen: failed to create pool\n");
            }
            return -1;
        }
        /* Set event middleware to receive incoming events */
        nostr_simple_pool_set_event_middleware(s->pool, nip46_event_middleware);
    }

    /* Store relays in session for later use */
    if (s->relays) {
        for (size_t i = 0; i < s->n_relays; ++i) free(s->relays[i]);
        free(s->relays);
    }
    s->relays = (char **)malloc(n_relays * sizeof(char *));
    if (!s->relays) return -1;
    s->n_relays = n_relays;
    for (size_t i = 0; i < n_relays; ++i) {
        s->relays[i] = strdup(relays[i]);
        if (!s->relays[i]) {
            /* Cleanup on failure */
            for (size_t j = 0; j < i; ++j) free(s->relays[j]);
            free(s->relays);
            s->relays = NULL;
            s->n_relays = 0;
            return -1;
        }
    }

    /* Ensure all relays are connected */
    for (size_t i = 0; i < n_relays; ++i) {
        if (relays[i] && *relays[i]) {
            nostr_simple_pool_ensure_relay(s->pool, relays[i]);
        }
    }

    /* Build a filter for kind 24133 events tagged with our pubkey */
    NostrFilters *filters = nostr_filters_new();
    if (!filters) return -1;

    NostrFilter *f = nostr_filter_new();
    if (!f) {
        nostr_filters_free(filters);
        return -1;
    }

    /* Filter for NIP-46 kind */
    int kinds[] = { NOSTR_EVENT_KIND_NIP46 };
    nostr_filter_set_kinds(f, kinds, 1);

    /* Filter for events tagged with our pubkey (p-tag) */
    NostrTags *filter_tags = nostr_tags_new(1, nostr_tag_new("p", s->bunker_pubkey_hex, NULL));
    if (filter_tags) {
        nostr_filter_set_tags(f, filter_tags);
    }

    /* Move filter into filters collection */
    NostrFilter f_copy = *f;
    free(f); /* free the shell, contents moved */
    if (!nostr_filters_add(filters, &f_copy)) {
        nostr_filters_free(filters);
        return -1;
    }

    /* Subscribe to all relays */
    nostr_simple_pool_subscribe(s->pool, (const char **)relays, n_relays, *filters, true /* dedup */);

    /* Start the pool worker thread */
    nostr_simple_pool_start(s->pool);

    /* Free the filters wrapper (subscription made a copy) */
    nostr_filters_free(filters);

    s->listening = 1;

    if (getenv("NOSTR_DEBUG")) {
        fprintf(stderr, "[nip46] bunker_listen: listening on %zu relay(s) for pubkey %s\n",
                n_relays, s->bunker_pubkey_hex);
    }

    return 0;
}
static int is_unreserved(int c) {
    return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='.'||c=='_'||c=='~'||c==':'||c=='/';
}
static char *percent_encode(const char *s) {
    if (!s) return NULL; size_t n=strlen(s);
    char *out=(char*)malloc(n*3+1); if(!out) return NULL; size_t j=0;
    for(size_t i=0;i<n;++i){ unsigned char c=(unsigned char)s[i]; if(is_unreserved(c)){ out[j++]=c; } else { static const char hex[]="0123456789ABCDEF"; out[j++]='%'; out[j++]=hex[(c>>4)&0xF]; out[j++]=hex[c&0xF]; } }
    out[j]='\0'; return out;
}
int nostr_nip46_bunker_issue_bunker_uri(NostrNip46Session *s, const char *remote_signer_pubkey_hex, const char *const *relays, size_t n_relays, const char *secret, char **out_uri) {
    (void)s; if(!remote_signer_pubkey_hex||!out_uri) return -1; *out_uri=NULL;
    size_t cap = 16 + 64 + 1 + (n_relays? n_relays*64:0) + (secret? strlen(secret)+16:0);
    char *buf=(char*)malloc(cap); if(!buf) return -1; size_t len=0;
    len += snprintf(buf+len, cap-len, "bunker://%s", remote_signer_pubkey_hex);
    int first=1;
    if (relays && n_relays>0) {
        for (size_t i=0;i<n_relays;++i) {
            if (!relays[i]) continue; char *enc = percent_encode(relays[i]); if(!enc){ free(buf); return -1; }
            len += snprintf(buf+len, cap-len, "%srelay=%s", first?"?":"&", enc);
            first=0; free(enc);
        }
    }
    if (secret && *secret) {
        char *encs = percent_encode(secret); if(!encs){ free(buf); return -1; }
        len += snprintf(buf+len, cap-len, "%ssecret=%s", first?"?":"&", encs);
        free(encs);
    }
    *out_uri = buf; return 0;
}

/* Helper: publish an encrypted NIP-46 response event to relays.
 * client_pubkey_hex: the recipient's pubkey (for p-tag and encryption)
 * plaintext_json: the response JSON to encrypt
 * Returns 0 on success, -1 on failure
 */
static int nip46_publish_response(NostrNip46Session *s, const char *client_pubkey_hex, const char *plaintext_json) {
    if (!s || !client_pubkey_hex || !plaintext_json) return -1;
    if (!s->pool || !s->bunker_secret_hex || !s->bunker_pubkey_hex) {
        if (getenv("NOSTR_DEBUG")) {
            fprintf(stderr, "[nip46] publish_response: transport not initialized\n");
        }
        return -1;
    }

    /* Encrypt the response JSON using NIP-04 */
    char *cipher = NULL;
    char *err = NULL;
    nostr_secure_buf sb = secure_alloc(32);
    if (!sb.ptr || parse_sk32(s->bunker_secret_hex, (unsigned char*)sb.ptr) != 0) {
        if (sb.ptr) secure_free(&sb);
        return -1;
    }
    if (nostr_nip04_encrypt_secure(plaintext_json, client_pubkey_hex, &sb, &cipher, &err) != 0 || !cipher) {
        secure_free(&sb);
        if (getenv("NOSTR_DEBUG")) {
            fprintf(stderr, "[nip46] publish_response: encrypt failed: %s\n", err ? err : "(no error)");
        }
        if (err) free(err);
        return -1;
    }
    secure_free(&sb);

    /* Build the NIP-46 response event (kind 24133) */
    NostrEvent *ev = nostr_event_new();
    if (!ev) {
        free(cipher);
        return -1;
    }

    nostr_event_set_kind(ev, NOSTR_EVENT_KIND_NIP46);
    nostr_event_set_pubkey(ev, s->bunker_pubkey_hex);
    nostr_event_set_content(ev, cipher);
    nostr_event_set_created_at(ev, (int64_t)time(NULL));

    /* Add p-tag for the client pubkey (recipient) */
    NostrTags *tags = nostr_tags_new(1, nostr_tag_new("p", client_pubkey_hex, NULL));
    if (tags) {
        nostr_event_set_tags(ev, tags);
    }

    /* Sign the event with our bunker key */
    nostr_secure_buf sb_sign = secure_alloc(32);
    if (!sb_sign.ptr || parse_sk32(s->bunker_secret_hex, (unsigned char*)sb_sign.ptr) != 0) {
        if (sb_sign.ptr) secure_free(&sb_sign);
        nostr_event_free(ev);
        free(cipher);
        return -1;
    }

    if (nostr_event_sign_secure(ev, &sb_sign) != 0) {
        secure_free(&sb_sign);
        nostr_event_free(ev);
        free(cipher);
        if (getenv("NOSTR_DEBUG")) {
            fprintf(stderr, "[nip46] publish_response: signing failed\n");
        }
        return -1;
    }
    secure_free(&sb_sign);

    /* Publish to all connected relays in the pool */
    int published = 0;
    pthread_mutex_t *pool_mutex = &s->pool->pool_mutex;
    pthread_mutex_lock(pool_mutex);
    for (size_t i = 0; i < s->pool->relay_count; ++i) {
        NostrRelay *relay = s->pool->relays[i];
        if (relay && nostr_relay_is_connected(relay)) {
            nostr_relay_publish(relay, ev);
            published++;
            if (getenv("NOSTR_DEBUG")) {
                const char *url = nostr_relay_get_url_const(relay);
                fprintf(stderr, "[nip46] published response to relay: %s\n", url ? url : "(unknown)");
            }
        }
    }
    pthread_mutex_unlock(pool_mutex);

    free(cipher);
    nostr_event_free(ev);

    if (getenv("NOSTR_DEBUG")) {
        fprintf(stderr, "[nip46] publish_response: published to %d relay(s)\n", published);
    }

    return published > 0 ? 0 : -1;
}

int nostr_nip46_bunker_reply(NostrNip46Session *s, const NostrNip46Request *req, const char *result_or_json, const char *error_or_null) {
    if (!s || !req || !req->id) return -1;
    char *json = NULL;
    if (error_or_null) {
        json = nostr_nip46_response_build_err(req->id, error_or_null);
    } else {
        if (!result_or_json) return -1;
        json = nostr_nip46_response_build_ok(req->id, result_or_json);
    }
    if (!json) return -1;
    if (s->last_reply_json) { free(s->last_reply_json); }
    s->last_reply_json = strdup(json); /* keep a copy for tests/introspection */

    /* Publish the response over the relay transport if available.
     * We need the client pubkey to encrypt to. Priority:
     * 1. current_request_client_pubkey (set during handle_cipher)
     * 2. client_pubkey_hex (from nostrconnect:// URI)
     * 3. remote_pubkey_hex (from bunker:// URI) */
    int rc = 0;
    const char *recipient = s->current_request_client_pubkey ? s->current_request_client_pubkey :
                           (s->client_pubkey_hex ? s->client_pubkey_hex : s->remote_pubkey_hex);
    if (s->pool && s->listening && recipient) {
        rc = nip46_publish_response(s, recipient, json);
        if (rc != 0 && getenv("NOSTR_DEBUG")) {
            fprintf(stderr, "[nip46] bunker_reply: failed to publish response\n");
        }
    } else if (getenv("NOSTR_DEBUG")) {
        fprintf(stderr, "[nip46] bunker_reply: transport not ready, response stored locally only\n");
    }

    free(json);
    return rc;
}

int nostr_nip46_bunker_handle_cipher(NostrNip46Session *s,
                                     const char *client_pubkey_hex,
                                     const char *ciphertext,
                                     char **out_cipher_reply) {
    if (!s || !client_pubkey_hex || !ciphertext || !out_cipher_reply) return -1;
    *out_cipher_reply = NULL;
    if (!s->secret) return -1; /* need our secret to decrypt/encrypt */

    /* Store client pubkey for response routing (used by bunker_reply if transport is active) */
    if (s->current_request_client_pubkey) {
        free(s->current_request_client_pubkey);
    }
    s->current_request_client_pubkey = strdup(client_pubkey_hex);

    /* 1) Decrypt NIP-04 */
    char *plain = NULL; char *err = NULL;
    nostr_secure_buf sb = secure_alloc(32);
    if (!sb.ptr || parse_sk32(s->secret, (unsigned char*)sb.ptr) != 0) { if (sb.ptr) secure_free(&sb); return -1; }
    if (nostr_nip04_decrypt_secure(ciphertext, client_pubkey_hex, &sb, &plain, &err) != 0 || !plain) {
        secure_free(&sb);
        if (getenv("NOSTR_DEBUG")) {
            fprintf(stderr, "[nip46] decrypt failed: %s\n", err ? err : "(no error)" );
        }
        if (err) free(err);
        return -1;
    }
    secure_free(&sb);
    if (getenv("NOSTR_DEBUG")) {
        fprintf(stderr, "[nip46] decrypted request: %s\n", plain);
    }

    /* 2) Parse request */
    NostrNip46Request req = {0};
    if (nostr_nip46_request_parse(plain, &req) != 0 || !req.id || !req.method) {
        free(plain);
        nostr_nip46_request_free(&req);
        return -1;
    }
    if (getenv("NOSTR_DEBUG")) {
        fprintf(stderr, "[nip46] parsed method: %s, n_params=%zu\n", req.method, req.n_params);
    }

    /* 3) Dispatch */
    char *reply_json = NULL;
    if (strcmp(req.method, "get_public_key") == 0) {
        char *pub = nostr_key_get_public(s->secret);
        if (!pub) { nostr_nip46_request_free(&req); free(plain); return -1; }
        /* Build JSON string token for result: "<hex>" */
        size_t cap = strlen(pub) + 3;
        char *quoted = (char*)malloc(cap);
        if (!quoted) { free(pub); nostr_nip46_request_free(&req); free(plain); return -1; }
        snprintf(quoted, cap, "\"%s\"", pub);
        reply_json = nostr_nip46_response_build_ok(req.id, quoted);
        free(quoted);
        free(pub);
    } else if (strcmp(req.method, "sign_event") == 0) {
        /* enforce ACL: require permission for client */
        if (!acl_has_perm(s, client_pubkey_hex, "sign_event")) {
            reply_json = nostr_nip46_response_build_err(req.id, "forbidden");
        } else {
        if (req.n_params < 1 || !req.params || !req.params[0]) { nostr_nip46_request_free(&req); free(plain); return -1; }
        if (getenv("NOSTR_DEBUG")) {
            fprintf(stderr, "[nip46] sign_event: incoming event JSON param: %s\n", req.params[0]);
        }
        if (s->cbs.sign_cb) {
            char *signed_event_json = s->cbs.sign_cb(req.params[0], s->cbs.user_data);
            if (!signed_event_json) {
                reply_json = nostr_nip46_response_build_err(req.id, "signing_failed");
            } else {
                reply_json = nostr_nip46_response_build_ok(req.id, signed_event_json);
                free(signed_event_json);
            }
        } else {
            /* Real signing path using libnostr */
            if (!s->secret) { nostr_nip46_request_free(&req); free(plain); return -1; }
            NostrEvent *ev = nostr_event_new();
            if (!ev) { nostr_nip46_request_free(&req); free(plain); return -1; }
            int prc = nostr_event_deserialize(ev, req.params[0]);
            if (prc != 0) {
                nostr_event_free(ev);
                reply_json = nostr_nip46_response_build_err(req.id, "invalid_event_json");
            } else {
                /* Ensure pubkey matches bunker secret */
                char *bunker_pk_x = nostr_key_get_public(s->secret);
                if (!bunker_pk_x) {
                    nostr_event_free(ev);
                    nostr_nip46_request_free(&req); free(plain); return -1;
                }
                nostr_event_set_pubkey(ev, bunker_pk_x);
                free(bunker_pk_x);
                /* sign with secure key */
                nostr_secure_buf sb2 = secure_alloc(32);
                if (!sb2.ptr || parse_sk32(s->secret, (unsigned char*)sb2.ptr) != 0) { if (sb2.ptr) secure_free(&sb2); nostr_event_free(ev); nostr_nip46_request_free(&req); free(plain); return -1; }
                if (nostr_event_sign_secure(ev, &sb2) != 0) {
                    secure_free(&sb2);
                    reply_json = nostr_nip46_response_build_err(req.id, "signing_failed");
                } else {
                    secure_free(&sb2);
                    char *signed_json = nostr_event_serialize(ev);
                    if (!signed_json) {
                        reply_json = nostr_nip46_response_build_err(req.id, "serialize_failed");
                    } else {
                        if (getenv("NOSTR_DEBUG")) {
                            fprintf(stderr, "[nip46] sign_event: serialized signed event JSON: %s\n", signed_json);
                        }
                        reply_json = nostr_nip46_response_build_ok(req.id, signed_json);
                        free(signed_json);
                    }
                }
                nostr_event_free(ev);
            }
        }
        }
    } else if (strcmp(req.method, "connect") == 0) {
        /* params: [client_pubkey_hex, perms_csv] */
        int allowed = 1;
        if (s->cbs.authorize_cb) {
            const char *pk = (req.n_params > 0 && req.params) ? req.params[0] : NULL;
            const char *perms = (req.n_params > 1 && req.params) ? req.params[1] : NULL;
            allowed = s->cbs.authorize_cb(pk, perms, s->cbs.user_data);
        }
        if (allowed) {
            const char *pk = (req.n_params > 0 && req.params) ? req.params[0] : NULL;
            const char *perms = (req.n_params > 1 && req.params) ? req.params[1] : NULL;
            if (pk && is_valid_pubkey_hex_relaxed(pk)) {
                acl_set_perms(s, pk, perms);
            }
            reply_json = nostr_nip46_response_build_ok(req.id, "\"ack\"");
        } else {
            reply_json = nostr_nip46_response_build_err(req.id, "denied");
        }
    } else {
        reply_json = nostr_nip46_response_build_err(req.id, "method_not_supported");
    }

    /* Save last reply (plaintext) for tests that may introspect it */
    if (s->last_reply_json) { free(s->last_reply_json); s->last_reply_json=NULL; }
    if (reply_json) {
        s->last_reply_json = strdup(reply_json);
        if (getenv("NOSTR_DEBUG")) {
            fprintf(stderr, "[nip46] reply (plaintext): %s\n", reply_json);
        }
    }

    /* 4) Encrypt reply */
    char *cipher = NULL; char *e2 = NULL;
    int rc = -1;
    if (reply_json) {
        nostr_secure_buf sb3 = secure_alloc(32);
        if (sb3.ptr && parse_sk32(s->secret, (unsigned char*)sb3.ptr) == 0) {
            if (nostr_nip04_encrypt_secure(reply_json, client_pubkey_hex, &sb3, &cipher, &e2) == 0 && cipher) {
                rc = 0;
            }
        }
        secure_free(&sb3);
    }
    if (rc == 0) { *out_cipher_reply = cipher; }
    if (rc != 0 && getenv("NOSTR_DEBUG")) {
        fprintf(stderr, "[nip46] encrypt failed: %s\n", e2 ? e2 : "(no error)" );
    }
    if (e2) free(e2);

    /* 5) Cleanup */
    free(reply_json);
    nostr_nip46_request_free(&req);
    free(plain);
    return rc;
}

/* Getters */
static char *dupstr(const char *s){ if(!s) return NULL; size_t n=strlen(s); char *o=(char*)malloc(n+1); if(!o) return NULL; memcpy(o,s,n+1); return o; }
int nostr_nip46_session_get_remote_pubkey(const NostrNip46Session *s, char **out_hex){ if(!s||!out_hex) return -1; *out_hex = dupstr(s->remote_pubkey_hex); return 0; }
int nostr_nip46_session_get_client_pubkey(const NostrNip46Session *s, char **out_hex){ if(!s||!out_hex) return -1; *out_hex = dupstr(s->client_pubkey_hex); return 0; }
int nostr_nip46_session_get_secret(const NostrNip46Session *s, char **out_secret){ if(!s||!out_secret) return -1; *out_secret = dupstr(s->secret); return 0; }
int nostr_nip46_session_get_relays(const NostrNip46Session *s, char ***out_relays, size_t *out_n){ if(!s||!out_relays||!out_n) return -1; *out_relays=NULL; *out_n=0; if(!s->relays||s->n_relays==0) return 0; char **arr=(char**)malloc(sizeof(char*)*s->n_relays); if(!arr) return -1; for(size_t i=0;i<s->n_relays;++i){ arr[i]=dupstr(s->relays[i]); if(!arr[i]){ for(size_t j=0;j<i;++j) free(arr[j]); free(arr); return -1; } } *out_relays=arr; *out_n=s->n_relays; return 0; }

/* Set relays on a session directly (takes ownership of the relay strings) */
int nostr_nip46_session_set_relays(NostrNip46Session *s, const char *const *relays, size_t n_relays) {
    if (!s) return -1;
    /* Free existing relays */
    if (s->relays) {
        for (size_t i = 0; i < s->n_relays; i++) free(s->relays[i]);
        free(s->relays);
        s->relays = NULL;
        s->n_relays = 0;
    }
    if (!relays || n_relays == 0) return 0;
    /* Copy relays */
    s->relays = (char **)malloc(n_relays * sizeof(char *));
    if (!s->relays) return -1;
    for (size_t i = 0; i < n_relays; i++) {
        s->relays[i] = dupstr(relays[i]);
        if (!s->relays[i]) {
            for (size_t j = 0; j < i; j++) free(s->relays[j]);
            free(s->relays);
            s->relays = NULL;
            s->n_relays = 0;
            return -1;
        }
    }
    s->n_relays = n_relays;
    fprintf(stderr, "[nip46] set_relays: set %zu relays\n", n_relays);
    return 0;
}

int nostr_nip46_session_take_last_reply_json(NostrNip46Session *s, char **out_json){ if(!s||!out_json) return -1; *out_json=NULL; if(!s->last_reply_json) return 0; *out_json = s->last_reply_json; s->last_reply_json=NULL; return 0; }

/* --- ACL helpers --- */
static int csv_split(const char *csv, char ***out_vec, size_t *out_n){ if(out_vec) *out_vec=NULL; if(out_n) *out_n=0; if(!csv||!*csv||!out_vec) return 0; size_t n=1; for(const char *p=csv; *p; ++p){ if(*p==',') n++; }
    char **vec=(char**)calloc(n, sizeof(char*)); if(!vec) return -1; size_t idx=0; const char *start=csv; for(const char *p=csv; ; ++p){ if(*p==','||*p=='\0'){ size_t len=(size_t)(p-start); char *s=(char*)malloc(len+1); if(!s){ csv_free(vec, idx); return -1; } memcpy(s,start,len); s[len]='\0'; vec[idx++]=s; if(*p=='\0') break; start=p+1; } }
    *out_vec=vec; if(out_n) *out_n=idx; return 0; }
static void csv_free(char **vec, size_t n){ if(!vec) return; for(size_t i=0;i<n;++i) free(vec[i]); free(vec); }
static void acl_set_perms(NostrNip46Session *s, const char *client_pk, const char *perms_csv){ if(!s||!client_pk) return; /* remove existing */ struct PermEntry **pp=&s->acl_head; while(*pp){ if(strcmp((*pp)->client_pk, client_pk)==0){ struct PermEntry *old=*pp; *pp=old->next; if(old->methods) { for(size_t i=0;i<old->n_methods;++i) free(old->methods[i]); free(old->methods);} free(old->client_pk); free(old); break; } pp=&(*pp)->next; }
    struct PermEntry *e=(struct PermEntry*)calloc(1,sizeof(*e)); if(!e) return; e->client_pk=strdup(client_pk); if(perms_csv && *perms_csv){ if(csv_split(perms_csv, &e->methods, &e->n_methods)!=0){ e->methods=NULL; e->n_methods=0; } } e->next=s->acl_head; s->acl_head=e; }
static int acl_has_perm(const NostrNip46Session *s, const char *client_pk, const char *method){ if(!s||!client_pk||!method) return 0; for(const struct PermEntry *it=s->acl_head; it; it=it->next){ if(it->client_pk && strcmp(it->client_pk, client_pk)==0){ if(it->n_methods==0) return 0; for(size_t i=0;i<it->n_methods;++i){ if(it->methods[i] && strcmp(it->methods[i], method)==0) return 1; } return 0; } } return 0; }
