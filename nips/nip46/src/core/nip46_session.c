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
#include <sys/time.h>

/* Forward prototypes for local helpers */
static int csv_split(const char *csv, char ***out_vec, size_t *out_n);
static void csv_free(char **vec, size_t n);
static void acl_set_perms(NostrNip46Session *s, const char *client_pk, const char *perms_csv);
static int acl_has_perm(const NostrNip46Session *s, const char *client_pk, const char *method);

/* nostrc-32yf: Session state machine */
typedef enum {
    NIP46_STATE_DISCONNECTED = 0,  /* No pool, no connection */
    NIP46_STATE_CONNECTING,        /* Pool started, waiting for relay */
    NIP46_STATE_CONNECTED,         /* Relay connected, subscription active */
    NIP46_STATE_STOPPING           /* Shutting down */
} Nip46SessionState;

/* Pending RPC request entry - waiting for response from signer */
typedef struct PendingRequest {
    char *request_id;           /* RPC request ID to match response */
    GoChannel *response_chan;   /* Channel to send response to waiting caller */
    uint32_t timeout_ms;        /* nostrc-32yf: Per-request timeout */
    int64_t submit_time_us;     /* nostrc-32yf: Monotonic submit timestamp (usec) */
    int cancelled;              /* nostrc-32yf: Set to 1 if request was cancelled */
    struct PendingRequest *next;
} PendingRequest;

/* nostrc-13gf: Bounded prioritized RPC work queue.
 * Replaces the old one-detached-pthread-per-async-RPC model, which could
 * spawn unbounded threads during bulk DM hydration. */
typedef struct RpcJob {
    char *method;
    char **params;
    size_t n_params;
    size_t bytes;                /* retained payload size, for the byte cap */
    NostrNip46AsyncCallback callback;
    void *user_data;
    struct RpcJob *next;
} RpcJob;

typedef struct Nip46RpcWorker {
    struct NostrNip46Session *session;
    int dedicated_interactive;   /* worker 0 only serves interactive jobs */
    pthread_t tid;
} Nip46RpcWorker;

#define NIP46_RPC_WORKERS         3
#define NIP46_RPC_QUEUE_MAX_HI    64        /* interactive: sign/connect/get_public_key */
#define NIP46_RPC_QUEUE_MAX_LO    256       /* bulk: nip04/nip44 encrypt/decrypt */
#define NIP46_RPC_QUEUE_MAX_BYTES (8u << 20) /* total retained payload cap (8 MiB) */
#define NIP46_RPC_MAX_INTERVAL_MS 10000u    /* clamp for set_rate_limit pacing */

/* nostrc-kk9f: Session registry for callback context.
 * Maps pool pointers to their owning sessions for event dispatch. */
typedef struct SessionRegistryEntry {
    NostrSimplePool *pool;
    struct NostrNip46Session *session;
    struct SessionRegistryEntry *next;
} SessionRegistryEntry;

static pthread_mutex_t s_session_registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static SessionRegistryEntry *s_session_registry = NULL;

/* Session struct definition */
struct NostrNip46Session {
    /* Session metadata */
    char *note;
    NostrNip46TransportMode transport_mode; /* negotiated per-peer policy */
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

    /* nostrc-5wj9: Configurable request timeout (0 = use default) */
    uint32_t timeout_ms;

    /* nostrc-32yf: Session state machine */
    Nip46SessionState state;

    /* nostrc-prkl: Client RPC rate limiting (signer-relay flood protection).
     * Bulk operations (e.g. nip44_decrypt for every gift-wrapped DM) used to
     * publish kind-24133 requests unbounded, tripping relay rate limits. */
    pthread_mutex_t rpc_gate_mutex;
    pthread_cond_t  rpc_gate_cond;
    int      rpc_inflight;         /* currently active RPC round-trips */
    int      rpc_gate_waiters;     /* callers blocked waiting for a slot */
    int      rpc_gate_closing;     /* session teardown: reject new entrants */
    int      rpc_max_inflight;     /* 0 = NIP46_DEFAULT_MAX_INFLIGHT */
    uint32_t rpc_min_interval_ms;  /* 0 = NIP46_DEFAULT_MIN_INTERVAL_MS */
    int64_t  rpc_next_send_ms;     /* earliest monotonic time (ms) for next publish */

    /* nostrc-13gf: Reference counting + coordinated shutdown.
     * refcount is managed with __atomic builtins; nostr_nip46_session_free()
     * performs the transport/thread shutdown and drops the owner reference,
     * while short-lived borrowers (e.g. signer-service worker tasks) hold
     * refs so the memory outlives their use. */
    int refcount;
    int shutting_down;             /* set once by nostr_nip46_session_free */

    /* nostrc-13gf: bounded prioritized async RPC work queue */
    pthread_mutex_t q_mutex;
    pthread_cond_t  q_cond;
    RpcJob  *q_head_hi, *q_tail_hi;
    RpcJob  *q_head_lo, *q_tail_lo;
    size_t   q_len_hi, q_len_lo;
    size_t   q_bytes;              /* total retained payload bytes across queues */
    int      q_shutdown;
    int      q_started;            /* workers spawned lazily on first async RPC */
    int      q_n_workers;          /* how many workers actually started */
    Nip46RpcWorker q_workers[NIP46_RPC_WORKERS];
};

/* nostrc-prkl: rate limiting defaults */
#define NIP46_DEFAULT_MAX_INFLIGHT    4
#define NIP46_DEFAULT_MIN_INTERVAL_MS 150

/* nostrc-kk9f: Session registry helper functions.
 * Register a session's pool for callback lookup. */
static void session_registry_add(NostrSimplePool *pool, NostrNip46Session *session) {
    if (!pool || !session) return;

    SessionRegistryEntry *entry = (SessionRegistryEntry *)malloc(sizeof(SessionRegistryEntry));
    if (!entry) return;
    entry->pool = pool;
    entry->session = session;

    pthread_mutex_lock(&s_session_registry_mutex);
    entry->next = s_session_registry;
    s_session_registry = entry;
    pthread_mutex_unlock(&s_session_registry_mutex);
}

/* Unregister a session's pool */
static void session_registry_remove(NostrSimplePool *pool) {
    if (!pool) return;

    pthread_mutex_lock(&s_session_registry_mutex);
    SessionRegistryEntry **pp = &s_session_registry;
    while (*pp) {
        if ((*pp)->pool == pool) {
            SessionRegistryEntry *entry = *pp;
            *pp = entry->next;
            free(entry);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&s_session_registry_mutex);
}

/* Look up a session by pool pointer */
static NostrNip46Session *session_registry_find(NostrSimplePool *pool) {
    if (!pool) return NULL;

    NostrNip46Session *result = NULL;
    pthread_mutex_lock(&s_session_registry_mutex);
    for (SessionRegistryEntry *e = s_session_registry; e; e = e->next) {
        if (e->pool == pool) {
            result = e->session;
            break;
        }
    }
    pthread_mutex_unlock(&s_session_registry_mutex);
    return result;
}

/* nostrc-kk9f: Pending request helper functions.
 * Create a new pending request with a response channel.
 * nostrc-32yf: Now records per-request timeout and submit time. */
static PendingRequest *pending_request_new(const char *request_id, uint32_t timeout_ms) {
    if (!request_id) return NULL;

    PendingRequest *pr = (PendingRequest *)calloc(1, sizeof(PendingRequest));
    if (!pr) return NULL;

    pr->request_id = strdup(request_id);
    if (!pr->request_id) {
        free(pr);
        return NULL;
    }

    /* Create channel with capacity 1 - we expect exactly one response */
    pr->response_chan = go_channel_create(1);
    if (!pr->response_chan) {
        free(pr->request_id);
        free(pr);
        return NULL;
    }

    pr->timeout_ms = timeout_ms;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    pr->submit_time_us = (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
    pr->cancelled = 0;
    pr->next = NULL;
    return pr;
}

/* Add a pending request to the session's list. */
static void pending_request_add(NostrNip46Session *s, PendingRequest *req) {
    if (!s || !req) return;

    pthread_mutex_lock(&s->pending_mutex);
    req->next = s->pending_requests;
    s->pending_requests = req;
    pthread_mutex_unlock(&s->pending_mutex);
}

/* Find and remove a pending request by ID. Returns the request or NULL.
 * Caller takes ownership of returned request. */
static PendingRequest *pending_request_find_and_remove(NostrNip46Session *s, const char *id) {
    if (!s || !id) return NULL;

    PendingRequest *result = NULL;
    pthread_mutex_lock(&s->pending_mutex);
    PendingRequest **pp = &s->pending_requests;
    while (*pp) {
        if ((*pp)->request_id && strcmp((*pp)->request_id, id) == 0) {
            result = *pp;
            *pp = result->next;
            result->next = NULL;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&s->pending_mutex);
    return result;
}

/* Cancel and free a pending request by ID. */
static void pending_request_cancel(NostrNip46Session *s, const char *request_id) {
    PendingRequest *pr = pending_request_find_and_remove(s, request_id);
    if (pr) {
        if (pr->response_chan) {
            go_channel_close(pr->response_chan);
            go_channel_free(pr->response_chan);
        }
        free(pr->request_id);
        free(pr);
    }
}

/* Common helpers */
static NostrNip46Session *session_new(const char *note) {
    NostrNip46Session *s = (NostrNip46Session *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    pthread_mutex_init(&s->pending_mutex, NULL);
    /* nostrc-prkl: RPC rate limiting gate */
    pthread_mutex_init(&s->rpc_gate_mutex, NULL);
    pthread_cond_init(&s->rpc_gate_cond, NULL);
    s->rpc_inflight = 0;
    s->rpc_gate_waiters = 0;
    s->rpc_gate_closing = 0;
    s->rpc_max_inflight = 0;      /* use default */
    s->rpc_min_interval_ms = 0;   /* use default */
    s->rpc_next_send_ms = 0;
    /* nostrc-13gf: refcount + async work queue */
    s->refcount = 1;
    s->shutting_down = 0;
    pthread_mutex_init(&s->q_mutex, NULL);
    pthread_cond_init(&s->q_cond, NULL);
    /* Standard NIP-46 uses NIP-44 v2. Compatibility transports require
     * an explicit per-session selection before transport starts. */
    s->transport_mode = NOSTR_NIP46_TRANSPORT_NIP44_V2;
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

/* Bunker sessions retain the identity key used when relay transport starts.
 * Prefer that key thereafter so encryption and event signing cannot drift if
 * the generic session secret is changed. Client/local sessions use secret. */
static const char *nip46_transport_secret_hex(const NostrNip46Session *s) {
    if (!s) return NULL;
    return s->bunker_secret_hex ? s->bunker_secret_hex : s->secret;
}

static int nip46_base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return (int)(c - 'A');
    if (c >= 'a' && c <= 'z') return 26 + (int)(c - 'a');
    if (c >= '0' && c <= '9') return 52 + (int)(c - '0');
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Validate one complete canonical RFC 4648 base64 component without decoding.
 * This prevents a configured NIP-04 mode from accepting a different envelope
 * merely because the lower-level decoder ignores a suffix or unused bits. */
static int nip46_is_canonical_base64(const char *s, size_t len) {
    if (!s || len == 0 || (len & 3u) != 0) return 0;

    size_t padding = 0;
    if (s[len - 1] == '=') padding++;
    if (len > 1 && s[len - 2] == '=') padding++;
    if (padding > 2) return 0;

    const size_t data_len = len - padding;
    for (size_t i = 0; i < data_len; ++i) {
        if (nip46_base64_value((unsigned char)s[i]) < 0) return 0;
    }
    for (size_t i = data_len; i < len; ++i) {
        if (s[i] != '=') return 0;
    }

    if (padding == 1) {
        int v = nip46_base64_value((unsigned char)s[len - 2]);
        if (v < 0 || (v & 0x03) != 0) return 0;
    } else if (padding == 2) {
        int v = nip46_base64_value((unsigned char)s[len - 3]);
        if (v < 0 || (v & 0x0f) != 0) return 0;
    }
    return 1;
}

static int nip46_is_exact_nip04_legacy(const char *ciphertext) {
    if (!ciphertext || strncmp(ciphertext, "v=2:", 4) == 0) return 0;
    const char *q = strstr(ciphertext, "?iv=");
    if (!q || q == ciphertext || strstr(q + 4, "?iv=") != NULL) return 0;
    return nip46_is_canonical_base64(ciphertext, (size_t)(q - ciphertext)) &&
           nip46_is_canonical_base64(q + 4, strlen(q + 4));
}

static int nip46_is_exact_nip04_aead(const char *ciphertext) {
    return ciphertext && strncmp(ciphertext, "v=2:", 4) == 0 &&
           nip46_is_canonical_base64(ciphertext + 4, strlen(ciphertext + 4));
}

/* nostrc-13gf: Final destructor - runs when the last reference drops.
 * All transport and worker threads are already stopped by the shutdown in
 * nostr_nip46_session_free(); only memory and sync primitives remain. */
static void session_destroy(NostrNip46Session *s) {
    if (!s) return;
    if (s->note) { memset(s->note, 0, strlen(s->note)); free(s->note); }
    if (s->remote_pubkey_hex) { free(s->remote_pubkey_hex); }
    if (s->client_pubkey_hex) { free(s->client_pubkey_hex); }
    if (s->secret) { memset(s->secret, 0, strlen(s->secret)); free(s->secret); }
    if (s->relays) { for (size_t i=0;i<s->n_relays;++i) free(s->relays[i]); free(s->relays); }
    if (s->last_reply_json) free(s->last_reply_json);
    /* free ACL */
    struct PermEntry *it = s->acl_head; while (it) { struct PermEntry *nx = it->next; if (it->client_pk) free(it->client_pk); if (it->methods){ for(size_t i=0;i<it->n_methods;++i) free(it->methods[i]); free(it->methods);} free(it); it = nx; }
    if (s->bunker_pubkey_hex) { free(s->bunker_pubkey_hex); }
    if (s->bunker_secret_hex) { memset(s->bunker_secret_hex, 0, strlen(s->bunker_secret_hex)); free(s->bunker_secret_hex); }
    if (s->current_request_client_pubkey) { free(s->current_request_client_pubkey); }
    /* cancel and free leftover pending requests */
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
    pthread_mutex_destroy(&s->rpc_gate_mutex);
    pthread_cond_destroy(&s->rpc_gate_cond);
    pthread_mutex_destroy(&s->q_mutex);
    pthread_cond_destroy(&s->q_cond);
    if (s->derived_client_pubkey) { free(s->derived_client_pubkey); }
    free(s);
}

NostrNip46Session *nostr_nip46_session_ref(NostrNip46Session *s) {
    if (!s) return NULL;
    __atomic_add_fetch(&s->refcount, 1, __ATOMIC_RELAXED);
    return s;
}

void nostr_nip46_session_unref(NostrNip46Session *s) {
    if (!s) return;
    if (__atomic_sub_fetch(&s->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
        session_destroy(s);
    }
}

/* Forward declaration: flush + join the async work queue (defined with the
 * queue implementation below). */
static void nip46_rpc_queue_shutdown(NostrNip46Session *s);

/* nostrc-13gf: Owner shutdown + unref.
 *
 * Shutdown order matters for crash-free teardown while RPCs are active:
 *  1. mark state STOPPING (new RPC entrants reject fast);
 *  2. close the rate-limit gate (queued sync callers bail out);
 *  3. cancel pending requests (in-flight waiters wake via closed channels);
 *  4. flush + join the async work queue (workers exit);
 *  5. drain the gate (wait until every sync entrant has left);
 *  6. only now stop/free the relay pools nobody can be using;
 *  7. drop the owner reference - memory is freed when borrowers finish. */
void nostr_nip46_session_free(NostrNip46Session *s) {
    if (!s) return;

    int already = __atomic_exchange_n(&s->shutting_down, 1, __ATOMIC_ACQ_REL);
    if (already) {
        /* Repeated free(): pure no-op. Only the caller that wins the 0->1
         * transition owns (and below consumes) the owner reference; dropping
         * another ref here could destroy a session that borrowers (workers,
         * signer-service tasks) still hold. */
        fprintf(stderr, "[nip46] session_free: already shutting down (ignored)\n");
        return;
    }

    s->state = NIP46_STATE_STOPPING;

    /* 2. close the gate */
    pthread_mutex_lock(&s->rpc_gate_mutex);
    s->rpc_gate_closing = 1;
    pthread_cond_broadcast(&s->rpc_gate_cond);
    pthread_mutex_unlock(&s->rpc_gate_mutex);

    /* 3. wake in-flight response waiters */
    nostr_nip46_client_cancel_all(s);

    /* 4. stop async workers */
    nip46_rpc_queue_shutdown(s);

    /* 5. drain the gate; periodically re-cancel to catch stragglers that
     * registered a pending request after the first cancel_all pass. */
    pthread_mutex_lock(&s->rpc_gate_mutex);
    while (s->rpc_inflight > 0 || s->rpc_gate_waiters > 0) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 250 * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&s->rpc_gate_cond, &s->rpc_gate_mutex, &deadline);
        if (s->rpc_inflight > 0) {
            pthread_mutex_unlock(&s->rpc_gate_mutex);
            nostr_nip46_client_cancel_all(s);
            pthread_mutex_lock(&s->rpc_gate_mutex);
        }
    }
    pthread_mutex_unlock(&s->rpc_gate_mutex);

    /* 6. transport teardown - nobody is inside the RPC path anymore */
    nostr_nip46_client_stop(s);
    if (s->pool) {
        nostr_simple_pool_stop(s->pool);
        nostr_simple_pool_free(s->pool);
        s->pool = NULL;
    }

    /* 7. drop the owner reference */
    nostr_nip46_session_unref(s);
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

/* nostrc-kk9f: Event callback for persistent client pool.
 * Routes incoming NIP-46 responses to pending RPC requests.
 * Decrypts the event, parses the response, matches by ID, and dispatches via channel. */
static void nip46_persistent_client_cb(NostrIncomingEvent *incoming) {
    if (!incoming || !incoming->event || !incoming->relay) return;

    NostrEvent *ev = incoming->event;
    if (nostr_event_get_kind(ev) != NOSTR_EVENT_KIND_NIP46) return;

    const char *content = nostr_event_get_content(ev);
    const char *sender_pubkey = nostr_event_get_pubkey(ev);
    if (!content || !sender_pubkey) return;

    fprintf(stderr, "[nip46] persistent_cb: received response from %s\n", sender_pubkey);

    /* Find the session for this relay's pool.
     * We need to iterate through pools to find one that contains this relay. */
    NostrNip46Session *session = NULL;
    pthread_mutex_lock(&s_session_registry_mutex);
    for (SessionRegistryEntry *e = s_session_registry; e; e = e->next) {
        if (e->pool) {
            /* Check if this relay belongs to the pool */
            for (size_t i = 0; i < e->pool->relay_count; i++) {
                if (e->pool->relays[i] == incoming->relay) {
                    session = e->session;
                    break;
                }
            }
            if (session) break;
        }
    }
    pthread_mutex_unlock(&s_session_registry_mutex);

    if (!session) {
        fprintf(stderr, "[nip46] persistent_cb: no session found for relay\n");
        return;
    }

    if (!session->secret) {
        fprintf(stderr, "[nip46] persistent_cb: session has no secret for decryption\n");
        return;
    }

    /* nostrc-nip46-fix: Validate sender is the expected signer.
     * Without this check, a response from any signer on the relay could be accepted,
     * leading to "wrong" error responses if multiple signers are on the same relay. */
    if (session->remote_pubkey_hex) {
        if (strcmp(sender_pubkey, session->remote_pubkey_hex) != 0) {
            fprintf(stderr, "[nip46] persistent_cb: ignoring response from unexpected signer %s (expected %s)\n",
                    sender_pubkey, session->remote_pubkey_hex);
            return;
        }
    }

    /* Decrypt only with the session-negotiated transport. Never infer a
     * different mode from attacker-controlled ciphertext shape. */
    char *response_json = NULL;
    if (nostr_nip46_transport_decrypt(
            session, sender_pubkey, content, &response_json) != 0 ||
        !response_json) {
        fprintf(stderr, "[nip46] persistent_cb: decrypt failed (mode=%s)\n",
                nostr_nip46_transport_mode_name(session->transport_mode));
        return;
    }

    if (!nostr_json_is_valid(response_json)) {
        fprintf(stderr, "[nip46] persistent_cb: invalid JSON\n");
        free(response_json);
        return;
    }

    /* Extract response ID */
    char *resp_id = NULL;
    if (nostr_json_get_string(response_json, "id", &resp_id) != 0 || !resp_id) {
        fprintf(stderr, "[nip46] persistent_cb: no id field in response\n");
        free(response_json);
        return;
    }

    /* Find and remove matching pending request */
    PendingRequest *pending = pending_request_find_and_remove(session, resp_id);
    free(resp_id);

    if (!pending) {
        /* Normal: signer relays may echo duplicate responses, or signer apps
         * may send a second response (e.g. error after success) for the same
         * request ID.  The first response was already dispatched. */
        fprintf(stderr, "[nip46] persistent_cb: ignoring duplicate/late response (already handled)\n");
        free(response_json);
        return;
    }

    /* Send response to waiting caller via channel */
    if (pending->response_chan) {
        /* Send the response JSON string (ownership transferred to receiver) */
        if (go_channel_send(pending->response_chan, response_json) != 0) {
            fprintf(stderr, "[nip46] persistent_cb: failed to send response to channel\n");
            free(response_json);
        }
        /* Don't free response_json here - ownership transferred to channel receiver */
    } else {
        free(response_json);
    }

    /* CRITICAL: Do NOT free pending request here!
     * The caller (nip46_rpc_call) owns the pending request and will free it
     * after receiving the response from the channel. Freeing here causes
     * use-after-free race condition. */

    fprintf(stderr, "[nip46] persistent_cb: dispatched response to pending request\n");
}

/* nostrc-4cp/F25: Relay connection readiness is signaled by libnostr's
 * relay state callback instead of a polling monitor thread. */
static void nip46_relay_state_cb(NostrRelay *relay,
                                 NostrRelayConnectionState old_state,
                                 NostrRelayConnectionState new_state,
                                 void *user_data) {
    (void)relay;
    (void)old_state;
    GoChannel *chan = (GoChannel *)user_data;
    if (chan && new_state == NOSTR_RELAY_STATE_CONNECTED) {
        go_channel_try_send(chan, (void *)(intptr_t)1);
    }
}

/* nostrc-32yf: Query session state (internal) */
static Nip46SessionState nip46_get_state(const NostrNip46Session *s) {
    if (!s) return NIP46_STATE_DISCONNECTED;
    return s->state;
}

/* nostrc-32yf: Query session state (public API, maps internal enum) */
NostrNip46State nostr_nip46_client_get_state_public(const NostrNip46Session *s) {
    return (NostrNip46State)nip46_get_state(s);
}

/* nostrc-j2yu: Start the persistent relay connection for efficient RPC calls. */
int nostr_nip46_client_start(NostrNip46Session *s) {
    if (!s) return -1;

    if (s->client_pool_started) {
        fprintf(stderr, "[nip46] client_start: already running\n");
        return 0;
    }

    s->state = NIP46_STATE_CONNECTING;

    if (!s->secret) {
        fprintf(stderr, "[nip46] client_start: ERROR: no secret set\n");
        return -1;
    }
    if (!s->relays || s->n_relays == 0) {
        fprintf(stderr, "[nip46] client_start: ERROR: no relays configured\n");
        return -1;
    }

    /* Derive client pubkey from secret */
    if (!s->derived_client_pubkey) {
        s->derived_client_pubkey = nostr_key_get_public(s->secret);
        if (!s->derived_client_pubkey) {
            fprintf(stderr, "[nip46] client_start: ERROR: failed to derive pubkey\n");
            return -1;
        }
    }

    /* Create persistent pool */
    s->client_pool = nostr_simple_pool_new();
    if (!s->client_pool) {
        fprintf(stderr, "[nip46] client_start: ERROR: failed to create pool\n");
        return -1;
    }

    nostr_simple_pool_set_event_middleware(s->client_pool, nip46_persistent_client_cb);

    GoChannel *connect_chan = go_channel_create(1);
    if (!connect_chan) {
        fprintf(stderr, "[nip46] client_start: ERROR: failed to create connect channel\n");
        nostr_simple_pool_free(s->client_pool);
        s->client_pool = NULL;
        return -1;
    }

    /* Connect to all relays and register event-driven state callbacks before
     * starting the pool so the first CONNECTED transition cannot be missed. */
    for (size_t i = 0; i < s->n_relays; i++) {
        nostr_simple_pool_ensure_relay(s->client_pool, s->relays[i]);
    }
    for (size_t i = 0; i < s->client_pool->relay_count; i++) {
        NostrRelay *relay = s->client_pool->relays[i];
        if (!relay) continue;
        nostr_relay_set_state_callback(relay, nip46_relay_state_cb, connect_chan);
        if (nostr_relay_is_connected(relay)) {
            go_channel_try_send(connect_chan, (void *)(intptr_t)1);
        }
    }

    nostr_simple_pool_start(s->client_pool);

    /* Wait for connection signal via channel select with 5s timeout */
    GoSelectCase connect_cases[1];
    connect_cases[0].op = GO_SELECT_RECEIVE;
    connect_cases[0].chan = connect_chan;
    void *connect_recv = NULL;
    connect_cases[0].recv_buf = &connect_recv;

    GoSelectResult conn_sel = go_select_timeout(connect_cases, 1, 5000);

    int connected = (conn_sel.selected_case == 0 && conn_sel.ok);

    /* nostrc-koso: With multiple relays, waiting only for the FIRST connection
     * races the rest: the response subscription below fails silently on relays
     * whose websocket isn't up yet (nostr_subscription_fire requires a live
     * connection and is never retried), and the RPC publish skips them too.
     * Kind 24133 is ephemeral, so a request published before a signer's relay
     * connects is lost forever — signers that listen on a single relay (e.g.
     * nsec.app on relay.nsec.app) then never see the request. Give the
     * remaining relays a short grace window to finish connecting. */
    if (connected && s->client_pool->relay_count > 1) {
        for (int waited_ms = 0; waited_ms < 3000; waited_ms += 100) {
            size_t up = 0;
            for (size_t i = 0; i < s->client_pool->relay_count; i++) {
                NostrRelay *r = s->client_pool->relays[i];
                if (r && nostr_relay_is_connected(r)) up++;
            }
            if (up >= s->client_pool->relay_count) break;
            /* Use the connect channel as the wait primitive; a received
             * signal or a 100ms timeout both just re-check the counts. */
            connect_recv = NULL;
            (void)go_select_timeout(connect_cases, 1, 100);
        }
    }

    for (size_t i = 0; i < s->client_pool->relay_count; i++) {
        if (s->client_pool->relays[i]) {
            nostr_relay_set_state_callback(s->client_pool->relays[i], NULL, NULL);
        }
    }
    go_channel_close(connect_chan);
    go_channel_free(connect_chan);

    if (!connected) {
        fprintf(stderr, "[nip46] client_start: ERROR: relay connection timeout\n");
        s->state = NIP46_STATE_DISCONNECTED;
        nostr_simple_pool_stop(s->client_pool);
        nostr_simple_pool_free(s->client_pool);
        s->client_pool = NULL;
        return -1;
    }

    /* Set up subscription for responses */
    NostrFilters *filters = nostr_filters_new();
    NostrFilter *f = nostr_filter_new();
    int kinds[] = { NOSTR_EVENT_KIND_NIP46 };
    nostr_filter_set_kinds(f, kinds, 1);

    NostrTags *filter_tags = nostr_tags_new(1,
        nostr_tag_new("p", s->derived_client_pubkey, NULL));
    nostr_filter_set_tags(f, filter_tags);
    nostr_filter_set_since_i64(f, (int64_t)time(NULL) - 60);

    NostrFilter f_copy = *f;
    free(f);
    nostr_filters_add(filters, &f_copy);

    nostr_simple_pool_subscribe(s->client_pool, (const char **)s->relays,
                                s->n_relays, *filters, true);
    nostr_filters_free(filters);

    /* nostrc-kk9f: Register session in registry for callback dispatch */
    session_registry_add(s->client_pool, s);

    s->client_pool_started = 1;
    s->state = NIP46_STATE_CONNECTED;
    fprintf(stderr, "[nip46] client_start: persistent pool started with %zu relay(s)\n",
            s->n_relays);
    return 0;
}

/* nostrc-j2yu: Stop the persistent relay connection */
void nostr_nip46_client_stop(NostrNip46Session *s) {
    if (!s) return;

    s->state = NIP46_STATE_STOPPING;

    if (s->client_pool) {
        /* nostrc-kk9f: Unregister from session registry before cleanup */
        session_registry_remove(s->client_pool);

        nostr_simple_pool_stop(s->client_pool);
        nostr_simple_pool_free(s->client_pool);
        s->client_pool = NULL;
    }
    s->client_pool_started = 0;
    s->state = NIP46_STATE_DISCONNECTED;
    fprintf(stderr, "[nip46] client_stop: persistent pool stopped\n");
}

/* nostrc-j2yu: Check if persistent pool is running */
int nostr_nip46_client_is_running(NostrNip46Session *s) {
    if (!s) return 0;
    return s->client_pool_started;
}

/* nostrc-5wj9: Configurable request timeout */
void nostr_nip46_client_set_timeout(NostrNip46Session *s, uint32_t timeout_ms) {
    if (!s) return;
    s->timeout_ms = timeout_ms;
}

uint32_t nostr_nip46_client_get_timeout(const NostrNip46Session *s) {
    if (!s || s->timeout_ms == 0) return NOSTR_NIP46_DEFAULT_TIMEOUT_MS;
    return s->timeout_ms;
}

/* nostrc-5wj9: Helper to get effective timeout for current session */
static uint32_t nip46_effective_timeout(const NostrNip46Session *s) {
    if (s && s->timeout_ms > 0) return s->timeout_ms;
    return NOSTR_NIP46_DEFAULT_TIMEOUT_MS;
}

/* Forward declaration for RPC helper used by sign_event and other calls */
static char *nip46_rpc_call(NostrNip46Session *s, const char *method,
                            const char **params, size_t n_params,
                            char **out_response_pubkey);

/* Request ID counter for unique IDs.
 * Combined with timestamp and pid to prevent collision with stale responses. */
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

/* nostrc-3l6f: Simplified RPC helper using persistent subscription.
 *
 * This function uses the persistent pool and event dispatch mechanism:
 * 1. Ensure client_start() was called to establish persistent connection
 * 2. Create a PendingRequest with a response channel
 * 3. Register the request with the session's pending list
 * 4. Build and publish the request event via the pool
 * 5. Wait for response on the channel with timeout
 * 6. Return the result or NULL on timeout
 *
 * The persistent subscription callback (nip46_persistent_client_cb) handles
 * incoming responses and dispatches them to the correct pending request.
 *
 * Returns the "result" field from the response on success, or NULL on error.
 * If out_response_pubkey is non-NULL, it receives the pubkey of the responding event.
 * Caller must free the returned strings. */
/* nostrc-prkl: Rate-limit gate helpers.
 *
 * Two mechanisms combined:
 *  - in-flight cap: at most N RPC round-trips concurrently (excess callers
 *    block on the condvar until a slot frees up);
 *  - pacing: successive request publishes are spaced by a minimum interval.
 *    Each acquirer reserves the next send slot under the mutex, so a burst
 *    of callers is serialized into an evenly-paced trickle. */
static int64_t nip46_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Sleep until an absolute monotonic deadline, retrying on EINTR / early wake. */
static void nip46_sleep_until_ms(int64_t deadline_ms) {
    for (;;) {
        int64_t now = nip46_now_ms();
        if (now >= deadline_ms) return;
        int64_t remain = deadline_ms - now;
        struct timespec ts;
        ts.tv_sec = (time_t)(remain / 1000);
        ts.tv_nsec = (long)(remain % 1000) * 1000000L;
        nanosleep(&ts, NULL); /* EINTR or early return: loop re-checks clock */
    }
}

/* nostrc-13gf: Interactive methods get priority over bulk crypto traffic. */
static int nip46_method_is_interactive(const char *method) {
    return method && (strcmp(method, "sign_event") == 0 ||
                      strcmp(method, "connect") == 0 ||
                      strcmp(method, "get_public_key") == 0 ||
                      strcmp(method, "ping") == 0);
}

/* Returns 0 when a slot was acquired, -1 when the session is closing.
 * nostrc-13gf: bulk (non-interactive) callers may only occupy max-1 slots,
 * so an interactive sign_event/connect never queues behind a decrypt storm. */
static int nip46_rpc_gate_acquire(NostrNip46Session *s, const char *method) {
    int interactive = nip46_method_is_interactive(method);
    pthread_mutex_lock(&s->rpc_gate_mutex);
    int max_inflight = s->rpc_max_inflight > 0 ? s->rpc_max_inflight
                                               : NIP46_DEFAULT_MAX_INFLIGHT;
    int my_limit = interactive ? max_inflight
                               : (max_inflight > 1 ? max_inflight - 1 : max_inflight);
    while (!s->rpc_gate_closing && s->rpc_inflight >= my_limit) {
        s->rpc_gate_waiters++;
        pthread_cond_wait(&s->rpc_gate_cond, &s->rpc_gate_mutex);
        s->rpc_gate_waiters--;
    }
    if (s->rpc_gate_closing) {
        /* Wake the teardown drain (it waits on the same condvar). */
        pthread_cond_broadcast(&s->rpc_gate_cond);
        pthread_mutex_unlock(&s->rpc_gate_mutex);
        fprintf(stderr, "[nip46] %s: session closing - RPC rejected\n", method);
        return -1;
    }
    s->rpc_inflight++;

    uint32_t interval = s->rpc_min_interval_ms > 0 ? s->rpc_min_interval_ms
                                                   : NIP46_DEFAULT_MIN_INTERVAL_MS;
    int64_t now = nip46_now_ms();
    int64_t slot = s->rpc_next_send_ms > now ? s->rpc_next_send_ms : now;
    s->rpc_next_send_ms = slot + (int64_t)interval;
    pthread_mutex_unlock(&s->rpc_gate_mutex);

    if (slot > now) {
        fprintf(stderr, "[nip46] %s: rate limit - pacing %lld ms before publish\n",
                method, (long long)(slot - now));
        nip46_sleep_until_ms(slot);
    }
    return 0;
}

static void nip46_rpc_gate_release(NostrNip46Session *s) {
    pthread_mutex_lock(&s->rpc_gate_mutex);
    if (s->rpc_inflight > 0) s->rpc_inflight--;
    /* Broadcast: wakes both queued callers and a draining teardown. */
    pthread_cond_broadcast(&s->rpc_gate_cond);
    pthread_mutex_unlock(&s->rpc_gate_mutex);
}

/* nostrc-prkl: Public tuning knob for the client RPC rate limit.
 * max_inflight <= 0 or min_interval_ms == 0 keep the built-in defaults. */
void nostr_nip46_client_set_rate_limit(NostrNip46Session *s,
                                       int max_inflight,
                                       uint32_t min_interval_ms) {
    if (!s) return;
    /* nostrc-13gf: clamp the interval - pacing sleeps hold a gate slot and
     * are not interruptible, so an absurd interval must not be able to
     * stall teardown for minutes. */
    if (min_interval_ms > NIP46_RPC_MAX_INTERVAL_MS)
        min_interval_ms = NIP46_RPC_MAX_INTERVAL_MS;
    pthread_mutex_lock(&s->rpc_gate_mutex);
    s->rpc_max_inflight = max_inflight > 0 ? max_inflight : 0;
    s->rpc_min_interval_ms = min_interval_ms;
    pthread_cond_broadcast(&s->rpc_gate_cond);
    pthread_mutex_unlock(&s->rpc_gate_mutex);
}

static char *nip46_rpc_call_impl(NostrNip46Session *s, const char *method,
                                 const char **params, size_t n_params,
                                 char **out_response_pubkey);

/* nostrc-prkl: Throttled entry point - every client RPC (sign_event,
 * nip44_encrypt/decrypt, connect, get_public_key, sync and async variants)
 * funnels through here, so the gate bounds total signer-relay traffic. */
static char *nip46_rpc_call(NostrNip46Session *s, const char *method,
                            const char **params, size_t n_params,
                            char **out_response_pubkey) {
    if (out_response_pubkey) *out_response_pubkey = NULL;
    if (!s || !method) return NULL;
    if (nip46_rpc_gate_acquire(s, method) != 0) return NULL;
    char *result = nip46_rpc_call_impl(s, method, params, n_params,
                                       out_response_pubkey);
    nip46_rpc_gate_release(s);
    return result;
}

static char *nip46_rpc_call_impl(NostrNip46Session *s, const char *method,
                                 const char **params, size_t n_params,
                                 char **out_response_pubkey) {
    if (out_response_pubkey) *out_response_pubkey = NULL;
    if (!s || !method) return NULL;

    /* nostrc-32yf: Reject calls during shutdown */
    if (s->state == NIP46_STATE_STOPPING) {
        fprintf(stderr, "[nip46] %s: ERROR: session is stopping\n", method);
        return NULL;
    }

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

    /* nostrc-3l6f: Ensure persistent pool is started (thread-safe).
     * Multiple nip44 decrypt threads can call nip46_rpc_call concurrently.
     * Use pending_mutex to serialize the pool start and prevent double-start.
     * Holding mutex during client_start is safe — other RPC calls need the pool
     * anyway and will wait. No deadlock: pool_mutex is a different lock. */
    if (!s->client_pool_started) {
        pthread_mutex_lock(&s->pending_mutex);
        if (!s->client_pool_started) {
            fprintf(stderr, "[nip46] %s: starting persistent pool\n", method);
            int rc = nostr_nip46_client_start(s);
            pthread_mutex_unlock(&s->pending_mutex);
            if (rc != 0) {
                fprintf(stderr, "[nip46] %s: ERROR: failed to start persistent pool\n", method);
                return NULL;
            }
        } else {
            pthread_mutex_unlock(&s->pending_mutex);
        }
    }

    fprintf(stderr, "[nip46] %s: building request\n", method);

    /* Build request JSON with unique ID.
     * Include pid to prevent collision with stale responses from previous sessions. */
    char req_id[48];
    snprintf(req_id, sizeof(req_id), "%lx_%x_%u",
             (unsigned long)time(NULL), (unsigned)getpid(), ++s_nip46_req_counter);
    fprintf(stderr, "[nip46] %s: request id = %s\n", method, req_id);
    char *req = nostr_nip46_request_build(req_id, method, params, n_params);
    if (!req) {
        fprintf(stderr, "[nip46] %s: ERROR: failed to build request JSON\n", method);
        return NULL;
    }

    char *cipher = NULL;
    if (nostr_nip46_transport_encrypt(
            s, peer, req, &cipher) != 0 || !cipher) {
        fprintf(stderr, "[nip46] %s: transport encryption failed (mode=%s)\n",
                method, nostr_nip46_transport_mode_name(s->transport_mode));
        secure_wipe(req, strlen(req));
        free(req);
        return NULL;
    }
    secure_wipe(req, strlen(req));
    free(req);

    /* nostrc-3l6f: Create pending request with response channel */
    uint32_t timeout = nip46_effective_timeout(s);
    PendingRequest *pr = pending_request_new(req_id, timeout);
    if (!pr) {
        fprintf(stderr, "[nip46] %s: ERROR: failed to create pending request\n", method);
        free(cipher);
        return NULL;
    }

    /* Add pending request to session BEFORE publishing to avoid race */
    pending_request_add(s, pr);

    /* Build kind 24133 request event */
    NostrEvent *req_ev = nostr_event_new();
    nostr_event_set_kind(req_ev, NOSTR_EVENT_KIND_NIP46);
    nostr_event_set_content(req_ev, cipher);
    nostr_event_set_created_at(req_ev, (int64_t)time(NULL));

    /* Use derived client pubkey from persistent pool */
    if (!s->derived_client_pubkey) {
        s->derived_client_pubkey = nostr_key_get_public(s->secret);
        if (!s->derived_client_pubkey) {
            fprintf(stderr, "[nip46] %s: ERROR: failed to derive client pubkey\n", method);
            pending_request_cancel(s, req_id);
            free(cipher);
            nostr_event_free(req_ev);
            return NULL;
        }
    }
    nostr_event_set_pubkey(req_ev, s->derived_client_pubkey);

    NostrTags *tags = nostr_tags_new(1, nostr_tag_new("p", peer, NULL));
    nostr_event_set_tags(req_ev, tags);

    /* Sign the request event */
    unsigned char sk_sign[32];
    if (parse_sk32(s->secret, sk_sign) != 0) {
        fprintf(stderr, "[nip46] %s: ERROR: failed to parse secret for signing\n", method);
        pending_request_cancel(s, req_id);
        free(cipher);
        nostr_event_free(req_ev);
        return NULL;
    }

    nostr_secure_buf sb = secure_alloc(32);
    if (!sb.ptr) {
        fprintf(stderr, "[nip46] %s: ERROR: failed to allocate secure buffer\n", method);
        secure_wipe(sk_sign, sizeof(sk_sign));
        pending_request_cancel(s, req_id);
        free(cipher);
        nostr_event_free(req_ev);
        return NULL;
    }
    memcpy(sb.ptr, sk_sign, 32);
    secure_wipe(sk_sign, sizeof(sk_sign));

    if (nostr_event_sign_secure(req_ev, &sb) != 0) {
        fprintf(stderr, "[nip46] %s: ERROR: failed to sign request event\n", method);
        secure_free(&sb);
        pending_request_cancel(s, req_id);
        free(cipher);
        nostr_event_free(req_ev);
        return NULL;
    }
    secure_free(&sb);
    free(cipher);

    /* nostrc-3l6f: Publish request via persistent pool to all connected relays.
     * nostrc-koso: track per-relay publish state and keep the event alive —
     * relays still connecting get a late publish during the response wait
     * below, so the request reaches signers that listen on a single relay. */
    fprintf(stderr, "[nip46] %s: publishing via persistent pool (%zu relay(s))\n",
            method, s->client_pool->relay_count);

    size_t pool_relay_count = s->client_pool->relay_count;
    unsigned char *published_to =
        (unsigned char *)calloc(pool_relay_count ? pool_relay_count : 1, 1);
    int published = 0;
    for (size_t i = 0; i < pool_relay_count; i++) {
        NostrRelay *relay = s->client_pool->relays[i];
        if (relay && nostr_relay_is_connected(relay)) {
            nostr_relay_publish(relay, req_ev);
            if (published_to) published_to[i] = 1;
            published++;
            fprintf(stderr, "[nip46] %s: published to %s\n", method, nostr_relay_get_url_const(relay));
        }
    }

    /* Wait for response on channel with the configured per-session timeout.
     * nostrc-koso: wait in slices; between slices, publish to relays that
     * finished connecting after the initial publish attempt. */
    fprintf(stderr, "[nip46] %s: waiting for response on relay subscription (timeout=%u ms)\n",
            method, pr->timeout_ms);

    void *recv_buf = NULL;
    GoSelectCase response_cases[1];
    response_cases[0].op = GO_SELECT_RECEIVE;
    response_cases[0].chan = pr->response_chan;
    response_cases[0].recv_buf = &recv_buf;

    GoSelectResult response_sel = { .selected_case = -1, .ok = false };
    uint32_t waited_ms = 0;
    while (waited_ms < pr->timeout_ms) {
        uint32_t slice = pr->timeout_ms - waited_ms;
        if (slice > 500) slice = 500;
        response_sel = go_select_timeout(response_cases, 1, slice);
        if (response_sel.selected_case == 0) break;
        waited_ms += slice;
        /* Late publish to relays that connected after the first attempt */
        for (size_t i = 0; i < pool_relay_count && published_to; i++) {
            NostrRelay *relay = s->client_pool->relays[i];
            if (!published_to[i] && relay && nostr_relay_is_connected(relay)) {
                nostr_relay_publish(relay, req_ev);
                published_to[i] = 1;
                published++;
                fprintf(stderr, "[nip46] %s: late publish to %s\n",
                        method, nostr_relay_get_url_const(relay));
            }
        }
    }
    free(published_to);
    nostr_event_free(req_ev);

    if (response_sel.selected_case < 0) {
        if (published == 0) {
            fprintf(stderr, "[nip46] %s: ERROR: no relay ever connected to publish to\n", method);
        } else {
            fprintf(stderr, "[nip46] %s: timed out waiting for response after %u ms\n",
                    method, pr->timeout_ms);
        }
        pending_request_cancel(s, req_id);
        return NULL;
    }

    char *result = NULL;
    int recv_ok = (response_sel.selected_case == 0 && response_sel.ok) ? 0 : -1;
    if (recv_ok != 0 || !recv_buf) {
        /* Channel closed without response (session shutdown or relay disconnect) */
        fprintf(stderr, "[nip46] %s: channel closed without response\n", method);
        pending_request_cancel(s, req_id);
        return NULL;
    }

    /* Got response JSON from channel */
    char *response_json = (char *)recv_buf;
    if (!response_json) {
        fprintf(stderr, "[nip46] %s: received NULL response\n", method);
        pending_request_cancel(s, req_id);
        return NULL;
    }

    /* Parse response to extract result or error */
    if (!nostr_json_is_valid(response_json)) {
        fprintf(stderr, "[nip46] %s: invalid response JSON\n", method);
        free(response_json);
        /* Clean up channel since we received the response */
        go_channel_close(pr->response_chan);
        go_channel_free(pr->response_chan);
        free(pr->request_id);
        free(pr);
        return NULL;
    }

    /* Check for error */
    char *err_msg = NULL;
    if (nostr_json_has_key(response_json, "error") &&
        nostr_json_get_type(response_json, "error") == NOSTR_JSON_STRING &&
        nostr_json_get_string(response_json, "error", &err_msg) == 0 && err_msg && *err_msg) {
        fprintf(stderr, "[nip46] %s: received error response: %s\n", method, err_msg);
        free(err_msg);
        free(response_json);
        go_channel_close(pr->response_chan);
        go_channel_free(pr->response_chan);
        free(pr->request_id);
        free(pr);
        return NULL;
    }
    free(err_msg);

    /* Extract result */
    if (nostr_json_get_string(response_json, "result", &result) != 0 || !result) {
        fprintf(stderr, "[nip46] %s: no result field in response\n", method);
        free(response_json);
        go_channel_close(pr->response_chan);
        go_channel_free(pr->response_chan);
        free(pr->request_id);
        free(pr);
        return NULL;
    }

    free(response_json);
    fprintf(stderr, "[nip46] %s: SUCCESS - result: %.50s\n", method, result);

    /* Clean up pending request (channel already drained) */
    go_channel_close(pr->response_chan);
    go_channel_free(pr->response_chan);
    free(pr->request_id);
    free(pr);

    /* Note: out_response_pubkey not supported in simplified version.
     * The callback dispatches response JSON, not the sender pubkey.
     * This could be enhanced by including sender info in the channel message. */
    (void)out_response_pubkey;

    return result;
}

/* nostrc-13gf: Async RPC via a bounded, prioritized, per-session work queue.
 *
 * Replaces the old one-detached-pthread-per-request model:
 *  - a fixed pool of NIP46_RPC_WORKERS threads (spawned lazily on first use;
 *    worker 0 serves ONLY interactive jobs so sign/connect never starve);
 *  - two FIFO queues (interactive / bulk) with hard caps - overflow fails
 *    the request immediately instead of growing without bound;
 *  - coordinated shutdown: nip46_rpc_queue_shutdown() flushes queued jobs
 *    (callbacks fire with an error) and joins the workers. */

static void rpc_job_free(RpcJob *job) {
    if (!job) return;
    free(job->method);
    if (job->params) {
        for (size_t i = 0; i < job->n_params; i++)
            free(job->params[i]);
        free(job->params);
    }
    free(job);
}

/* Pop the next eligible job. Interactive first; bulk only for non-dedicated
 * workers. Caller holds q_mutex. */
static RpcJob *rpc_queue_pop_locked(NostrNip46Session *s, int dedicated_interactive) {
    RpcJob *job = NULL;
    if (s->q_head_hi) {
        job = s->q_head_hi;
        s->q_head_hi = job->next;
        if (!s->q_head_hi) s->q_tail_hi = NULL;
        s->q_len_hi--;
    } else if (!dedicated_interactive && s->q_head_lo) {
        job = s->q_head_lo;
        s->q_head_lo = job->next;
        if (!s->q_head_lo) s->q_tail_lo = NULL;
        s->q_len_lo--;
    }
    if (job) {
        job->next = NULL;
        s->q_bytes = s->q_bytes >= job->bytes ? s->q_bytes - job->bytes : 0;
    }
    return job;
}

/* Worker threads are DETACHED and each holds a session reference taken at
 * spawn time.  There is deliberately no pthread_join anywhere:
 *  - teardown cannot deadlock on self-join when a job/flush callback
 *    triggers nostr_nip46_session_free() from a worker thread;
 *  - the session memory stays alive until the last worker exits and drops
 *    its reference (session_destroy only frees memory, never joins).
 * Workers never touch relay pools outside nip46_rpc_call, and every RPC
 * runs inside the rate-limit gate, so the shutdown drain in
 * nostr_nip46_session_free() still guarantees pools are only freed once
 * no worker is inside the transport. */
static void *nip46_rpc_worker_main(void *arg) {
    Nip46RpcWorker *w = (Nip46RpcWorker *)arg;
    NostrNip46Session *s = w->session; /* ref held by spawner on our behalf */

    for (;;) {
        pthread_mutex_lock(&s->q_mutex);
        RpcJob *job = NULL;
        while (!s->q_shutdown &&
               (job = rpc_queue_pop_locked(s, w->dedicated_interactive)) == NULL) {
            pthread_cond_wait(&s->q_cond, &s->q_mutex);
        }
        if (!job && s->q_shutdown) {
            pthread_mutex_unlock(&s->q_mutex);
            break;
        }
        pthread_mutex_unlock(&s->q_mutex);

        char *result = nip46_rpc_call(s, job->method,
                                      (const char **)job->params, job->n_params,
                                      NULL);
        if (job->callback) {
            if (result) {
                job->callback(s, result, NULL, job->user_data);
            } else {
                job->callback(s, NULL, "RPC call failed", job->user_data);
            }
        }
        free(result);
        rpc_job_free(job);
    }

    /* Drop the worker's reference LAST - may run session_destroy. */
    nostr_nip46_session_unref(s);
    return NULL;
}

/* Spawn workers on first use. Caller holds q_mutex. Returns 0 when at least
 * one worker is available. Partial creation failure is tolerated: we keep
 * whatever workers started instead of attempting a racy rollback. */
static int rpc_queue_start_locked(NostrNip46Session *s) {
    if (s->q_started) return s->q_n_workers > 0 ? 0 : -1;
    if (s->q_shutdown) return -1;

    int n = 0;
    for (int i = 0; i < NIP46_RPC_WORKERS; i++) {
        s->q_workers[i].session = s;
        s->q_workers[i].dedicated_interactive = (i == 0);
        nostr_nip46_session_ref(s); /* worker's reference */
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        int rc = pthread_create(&s->q_workers[i].tid, &attr,
                                nip46_rpc_worker_main, &s->q_workers[i]);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            nostr_nip46_session_unref(s); /* undo the worker's reference */
            break;
        }
        n++;
    }
    if (n == 1) {
        /* A lone worker must serve both queues or bulk jobs would starve. */
        s->q_workers[0].dedicated_interactive = 0;
    }
    s->q_n_workers = n;
    s->q_started = 1;
    fprintf(stderr, "[nip46] rpc queue: started %d worker(s)\n", n);
    return n > 0 ? 0 : -1;
}

/* Flush all queued jobs (fail their callbacks) and release the workers.
 * Does NOT join: detached workers observe q_shutdown, exit, and drop their
 * session references on their own schedule. */
static void nip46_rpc_queue_shutdown(NostrNip46Session *s) {
    if (!s) return;

    pthread_mutex_lock(&s->q_mutex);
    s->q_shutdown = 1;
    RpcJob *flush_hi = s->q_head_hi;
    RpcJob *flush_lo = s->q_head_lo;
    s->q_head_hi = s->q_tail_hi = NULL;
    s->q_head_lo = s->q_tail_lo = NULL;
    s->q_len_hi = s->q_len_lo = 0;
    s->q_bytes = 0;
    pthread_cond_broadcast(&s->q_cond);
    pthread_mutex_unlock(&s->q_mutex);

    RpcJob *lists[2] = { flush_hi, flush_lo };
    for (int l = 0; l < 2; l++) {
        RpcJob *job = lists[l];
        while (job) {
            RpcJob *next = job->next;
            if (job->callback) {
                job->callback(s, NULL, "session closing", job->user_data);
            }
            rpc_job_free(job);
            job = next;
        }
    }
}

static void nip46_rpc_call_async(NostrNip46Session *s, const char *method,
                                  const char **params, size_t n_params,
                                  NostrNip46AsyncCallback callback,
                                  void *user_data) {
    if (!s || !method) {
        if (callback) callback(s, NULL, "invalid arguments", user_data);
        return;
    }

    RpcJob *job = (RpcJob *)calloc(1, sizeof(RpcJob));
    if (!job) {
        if (callback) callback(s, NULL, "out of memory", user_data);
        return;
    }
    job->method = strdup(method);
    job->callback = callback;
    job->user_data = user_data;
    if (!job->method) {
        rpc_job_free(job);
        if (callback) callback(s, NULL, "out of memory", user_data);
        return;
    }
    if (params && n_params > 0) {
        job->params = (char **)calloc(n_params, sizeof(char *));
        if (!job->params) {
            rpc_job_free(job);
            if (callback) callback(s, NULL, "out of memory", user_data);
            return;
        }
        job->n_params = n_params;
        for (size_t i = 0; i < n_params; i++) {
            job->params[i] = params[i] ? strdup(params[i]) : NULL;
            if (job->params[i]) job->bytes += strlen(job->params[i]);
        }
    }
    job->bytes += strlen(job->method) + sizeof(*job);

    int interactive = nip46_method_is_interactive(method);

    pthread_mutex_lock(&s->q_mutex);
    if (s->q_shutdown) {
        pthread_mutex_unlock(&s->q_mutex);
        rpc_job_free(job);
        if (callback) callback(s, NULL, "session closing", user_data);
        return;
    }
    if (rpc_queue_start_locked(s) != 0) {
        pthread_mutex_unlock(&s->q_mutex);
        rpc_job_free(job);
        if (callback) callback(s, NULL, "failed to start rpc workers", user_data);
        return;
    }
    size_t cap = interactive ? NIP46_RPC_QUEUE_MAX_HI : NIP46_RPC_QUEUE_MAX_LO;
    size_t len = interactive ? s->q_len_hi : s->q_len_lo;
    if (len >= cap || s->q_bytes + job->bytes > NIP46_RPC_QUEUE_MAX_BYTES) {
        pthread_mutex_unlock(&s->q_mutex);
        fprintf(stderr, "[nip46] %s: rpc queue full (%zu jobs / %zu bytes) - rejecting request\n",
                method, len, s->q_bytes);
        rpc_job_free(job);
        if (callback) callback(s, NULL, "rpc queue full", user_data);
        return;
    }
    s->q_bytes += job->bytes;
    if (interactive) {
        if (s->q_tail_hi) s->q_tail_hi->next = job; else s->q_head_hi = job;
        s->q_tail_hi = job;
        s->q_len_hi++;
    } else {
        if (s->q_tail_lo) s->q_tail_lo->next = job; else s->q_head_lo = job;
        s->q_tail_lo = job;
        s->q_len_lo++;
    }
    /* Broadcast, not signal: a signal could wake only the dedicated
     * interactive worker for a bulk job it will not take, stranding it. */
    pthread_cond_broadcast(&s->q_cond);
    pthread_mutex_unlock(&s->q_mutex);
}

/* nostrc-5wj9: Public async API implementations */

void nostr_nip46_client_sign_event_async(NostrNip46Session *s,
                                          const char *event_json,
                                          NostrNip46AsyncCallback callback,
                                          void *user_data) {
    if (!s || !event_json) {
        if (callback) callback(s, NULL, "session or event_json is NULL", user_data);
        return;
    }
    const char *params[1] = { event_json };
    nip46_rpc_call_async(s, "sign_event", params, 1, callback, user_data);
}

void nostr_nip46_client_connect_rpc_async(NostrNip46Session *s,
                                           const char *connect_secret,
                                           const char *perms,
                                           NostrNip46AsyncCallback callback,
                                           void *user_data) {
    if (!s) {
        if (callback) callback(s, NULL, "session is NULL", user_data);
        return;
    }
    if (!s->remote_pubkey_hex) {
        if (callback) callback(s, NULL, "no remote_pubkey_hex", user_data);
        return;
    }
    const char *params[3];
    params[0] = s->remote_pubkey_hex;
    params[1] = connect_secret ? connect_secret : "";
    params[2] = perms ? perms : "";
    nip46_rpc_call_async(s, "connect", params, 3, callback, user_data);
}

void nostr_nip46_client_get_public_key_rpc_async(NostrNip46Session *s,
                                                  NostrNip46AsyncCallback callback,
                                                  void *user_data) {
    if (!s) {
        if (callback) callback(s, NULL, "session is NULL", user_data);
        return;
    }
    nip46_rpc_call_async(s, "get_public_key", NULL, 0, callback, user_data);
}

/* nostrc-5wj9: Cancel all pending RPC requests.
 * Closes all pending request channels, causing blocked threads to wake
 * with a "channel closed" result. */
void nostr_nip46_client_cancel_all(NostrNip46Session *s) {
    if (!s) return;

    /* nostrc-13gf: Close channels IN PLACE instead of detaching the list.
     * The woken waiter then finds its entry via pending_request_cancel()
     * and frees it (single owner) - the old detach approach leaked every
     * cancelled request because the waiter could no longer find it.
     * go_channel_close() is idempotent, so the waiter's second close of an
     * already-closed channel is harmless. Entries whose waiters never wake
     * are freed by the session destructor's leftover sweep. */
    pthread_mutex_lock(&s->pending_mutex);
    for (PendingRequest *pr = s->pending_requests; pr; pr = pr->next) {
        if (pr->response_chan) {
            go_channel_close(pr->response_chan);
        }
    }
    pthread_mutex_unlock(&s->pending_mutex);
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

/* Local transport crypto for kind-24133 protocol messages. */
static int nip46_transport_encrypt_mode(
    NostrNip46Session *s, NostrNip46TransportMode mode,
    const char *peer_pubkey_hex, const char *plaintext,
    char **out_ciphertext) {
    if (out_ciphertext) *out_ciphertext = NULL;
    const char *local_secret = nip46_transport_secret_hex(s);
    if (!local_secret || !peer_pubkey_hex || !plaintext || !out_ciphertext) {
        return -1;
    }

    if (mode == NOSTR_NIP46_TRANSPORT_NIP44_V2) {
        unsigned char sk[32] = {0};
        unsigned char pk[32] = {0};
        int rc = -1;
        if (parse_sk32(local_secret, sk) == 0 &&
            parse_peer_xonly32(peer_pubkey_hex, pk) == 0) {
            rc = nostr_nip44_encrypt_v2(
                sk, pk, (const uint8_t *)plaintext, strlen(plaintext),
                out_ciphertext);
        }
        secure_wipe(sk, sizeof(sk));
        secure_wipe(pk, sizeof(pk));
        return rc;
    }

    if (mode == NOSTR_NIP46_TRANSPORT_NIP04_LEGACY ||
        mode == NOSTR_NIP46_TRANSPORT_NIP04_AEAD_V2_EXTENSION) {
        nostr_secure_buf secret = secure_alloc(32);
        char *error = NULL;
        int rc = -1;
        if (secret.ptr &&
            parse_sk32(local_secret, (unsigned char *)secret.ptr) == 0) {
            if (mode == NOSTR_NIP46_TRANSPORT_NIP04_LEGACY) {
                rc = nostr_nip04_encrypt_legacy_secure(
                    plaintext, peer_pubkey_hex, &secret,
                    out_ciphertext, &error);
            } else {
                rc = nostr_nip04_encrypt_secure(
                    plaintext, peer_pubkey_hex, &secret,
                    out_ciphertext, &error);
            }
        }
        if (secret.ptr) secure_free(&secret);
        free(error);
        return rc;
    }
    return -1;
}

static int nip46_transport_decrypt_mode(
    NostrNip46Session *s, NostrNip46TransportMode mode,
    const char *peer_pubkey_hex, const char *ciphertext,
    char **out_plaintext) {
    if (out_plaintext) *out_plaintext = NULL;
    const char *local_secret = nip46_transport_secret_hex(s);
    if (!local_secret || !peer_pubkey_hex || !ciphertext || !out_plaintext) {
        return -1;
    }

    if (mode == NOSTR_NIP46_TRANSPORT_NIP44_V2) {
        unsigned char sk[32] = {0};
        unsigned char pk[32] = {0};
        uint8_t *plain = NULL;
        size_t plain_len = 0;
        int rc = -1;
        if (parse_sk32(local_secret, sk) == 0 &&
            parse_peer_xonly32(peer_pubkey_hex, pk) == 0 &&
            nostr_nip44_decrypt_v2(sk, pk, ciphertext,
                                   &plain, &plain_len) == 0 &&
            plain) {
            char *result = (char *)malloc(plain_len + 1);
            if (result) {
                memcpy(result, plain, plain_len);
                result[plain_len] = '\0';
                *out_plaintext = result;
                rc = 0;
            }
        }
        if (plain) {
            secure_wipe(plain, plain_len);
            free(plain);
        }
        secure_wipe(sk, sizeof(sk));
        secure_wipe(pk, sizeof(pk));
        return rc;
    }

    if (mode == NOSTR_NIP46_TRANSPORT_NIP04_LEGACY ||
        mode == NOSTR_NIP46_TRANSPORT_NIP04_AEAD_V2_EXTENSION) {
        const int exact_shape =
            mode == NOSTR_NIP46_TRANSPORT_NIP04_LEGACY
                ? nip46_is_exact_nip04_legacy(ciphertext)
                : nip46_is_exact_nip04_aead(ciphertext);
        if (!exact_shape) {
            return -1;
        }

        nostr_secure_buf secret = secure_alloc(32);
        char *error = NULL;
        int rc = -1;
        if (secret.ptr &&
            parse_sk32(local_secret, (unsigned char *)secret.ptr) == 0) {
            rc = nostr_nip04_decrypt_secure(
                ciphertext, peer_pubkey_hex, &secret,
                out_plaintext, &error);
        }
        if (secret.ptr) secure_free(&secret);
        free(error);
        return rc;
    }
    return -1;
}

int nostr_nip46_transport_encrypt(
    NostrNip46Session *s, const char *peer_pubkey_hex,
    const char *plaintext, char **out_ciphertext) {
    if (out_ciphertext) *out_ciphertext = NULL;
    return s ? nip46_transport_encrypt_mode(
                   s, s->transport_mode, peer_pubkey_hex,
                   plaintext, out_ciphertext) : -1;
}

int nostr_nip46_transport_decrypt(
    NostrNip46Session *s, const char *peer_pubkey_hex,
    const char *ciphertext, char **out_plaintext) {
    if (out_plaintext) *out_plaintext = NULL;
    return s ? nip46_transport_decrypt_mode(
                   s, s->transport_mode, peer_pubkey_hex,
                   ciphertext, out_plaintext) : -1;
}

/* Algorithm-specific compatibility APIs remain explicit and do not mutate the
 * session transport policy. New transport code uses the unified APIs above. */
int nostr_nip46_client_nip04_encrypt(
    NostrNip46Session *s, const char *peer_pubkey_hex,
    const char *plaintext, char **out_ciphertext) {
    return nip46_transport_encrypt_mode(
        s, NOSTR_NIP46_TRANSPORT_NIP04_AEAD_V2_EXTENSION,
        peer_pubkey_hex, plaintext, out_ciphertext);
}

int nostr_nip46_client_nip04_decrypt(
    NostrNip46Session *s, const char *peer_pubkey_hex,
    const char *ciphertext, char **out_plaintext) {
    return nip46_transport_decrypt_mode(
        s, NOSTR_NIP46_TRANSPORT_NIP04_AEAD_V2_EXTENSION,
        peer_pubkey_hex, ciphertext, out_plaintext);
}

int nostr_nip46_client_nip44_encrypt(
    NostrNip46Session *s, const char *peer_pubkey_hex,
    const char *plaintext, char **out_ciphertext) {
    return nip46_transport_encrypt_mode(
        s, NOSTR_NIP46_TRANSPORT_NIP44_V2,
        peer_pubkey_hex, plaintext, out_ciphertext);
}

int nostr_nip46_client_nip44_decrypt(
    NostrNip46Session *s, const char *peer_pubkey_hex,
    const char *ciphertext, char **out_plaintext) {
    return nip46_transport_decrypt_mode(
        s, NOSTR_NIP46_TRANSPORT_NIP44_V2,
        peer_pubkey_hex, ciphertext, out_plaintext);
}

/* nostrc-u1qh: NIP-46 content encrypt/decrypt via REMOTE SIGNER RPC.
 *
 * These delegate to the remote signer which holds the user's actual private key.
 * The client NEVER has the user's key — s->secret is only the NIP-46 transport
 * key for encrypting protocol messages between client and bunker.
 *
 * NIP-46 RPC method signatures:
 *   nip04_encrypt(peer_pubkey_hex, plaintext)  -> ciphertext
 *   nip04_decrypt(peer_pubkey_hex, ciphertext) -> plaintext
 *   nip44_encrypt(peer_pubkey_hex, plaintext)  -> ciphertext
 *   nip44_decrypt(peer_pubkey_hex, ciphertext) -> plaintext
 *
 * IMPORTANT: Do NOT confuse with the transport-level local-crypto functions above
 * (nostr_nip46_client_nip04_encrypt, etc.) which use s->secret for encrypting
 * NIP-46 protocol messages. Those are for internal/test use only. */

int nostr_nip46_client_nip04_encrypt_rpc(NostrNip46Session *s, const char *peer_pubkey_hex, const char *plaintext, char **out_ciphertext) {
    if (!s || !peer_pubkey_hex || !plaintext || !out_ciphertext) return -1;
    *out_ciphertext = NULL;
    const char *params[2] = { peer_pubkey_hex, plaintext };
    char *result = nip46_rpc_call(s, "nip04_encrypt", params, 2, NULL);
    if (!result) return -1;
    *out_ciphertext = result;
    return 0;
}

int nostr_nip46_client_nip04_decrypt_rpc(NostrNip46Session *s, const char *peer_pubkey_hex, const char *ciphertext, char **out_plaintext) {
    if (!s || !peer_pubkey_hex || !ciphertext || !out_plaintext) return -1;
    *out_plaintext = NULL;
    const char *params[2] = { peer_pubkey_hex, ciphertext };
    char *result = nip46_rpc_call(s, "nip04_decrypt", params, 2, NULL);
    if (!result) return -1;
    *out_plaintext = result;
    return 0;
}

int nostr_nip46_client_nip44_encrypt_rpc(NostrNip46Session *s, const char *peer_pubkey_hex, const char *plaintext, char **out_ciphertext) {
    if (!s || !peer_pubkey_hex || !plaintext || !out_ciphertext) return -1;
    *out_ciphertext = NULL;
    const char *params[2] = { peer_pubkey_hex, plaintext };
    char *result = nip46_rpc_call(s, "nip44_encrypt", params, 2, NULL);
    if (!result) return -1;
    *out_ciphertext = result;
    return 0;
}

int nostr_nip46_client_nip44_decrypt_rpc(NostrNip46Session *s, const char *peer_pubkey_hex, const char *ciphertext, char **out_plaintext) {
    if (!s || !peer_pubkey_hex || !ciphertext || !out_plaintext) return -1;
    *out_plaintext = NULL;
    const char *params[2] = { peer_pubkey_hex, ciphertext };
    char *result = nip46_rpc_call(s, "nip44_decrypt", params, 2, NULL);
    if (!result) return -1;
    *out_plaintext = result;
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
    /* Calculate capacity: bunker:// (9) + pubkey (64) + separator (1) +
     * For each relay: "relay=" (6) + encoded_url (up to 3x length) + "&" (1)
     * For secret: "secret=" (7) + encoded_secret (up to 3x length) + "&" (1) */
    size_t cap = 16 + 64 + 1;
    if (relays && n_relays > 0) {
        for (size_t i = 0; i < n_relays; ++i) {
            if (relays[i]) cap += 8 + strlen(relays[i]) * 3;  /* "relay=" + 3x URL + "&" */
        }
    }
    if (secret) cap += 10 + strlen(secret) * 3;  /* "secret=" + 3x secret + "&" */
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

    char *cipher = NULL;
    if (nostr_nip46_transport_encrypt(
            s, client_pubkey_hex, plaintext_json, &cipher) != 0 ||
        !cipher) {
        if (getenv("NOSTR_DEBUG")) {
            fprintf(stderr,
                    "[nip46] publish_response: encrypt failed (mode=%s)\n",
                    nostr_nip46_transport_mode_name(s->transport_mode));
        }
        return -1;
    }

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

    /* 1) Decrypt only with the negotiated session transport. */
    char *plain = NULL;
    if (nostr_nip46_transport_decrypt(
            s, client_pubkey_hex, ciphertext, &plain) != 0 || !plain) {
        if (getenv("NOSTR_DEBUG")) {
            fprintf(stderr, "[nip46] decrypt failed (mode=%s)\n",
                    nostr_nip46_transport_mode_name(s->transport_mode));
        }
        return -1;
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
                        reply_json = nostr_nip46_response_build_ok(req.id, signed_json);
                        free(signed_json);
                    }
                }
                nostr_event_free(ev);
            }
        }
        }
    } else if (strcmp(req.method, "connect") == 0) {
        /* NIP-46 connect params: [remote_signer_pubkey, secret, permissions]
         * ACL should be set for the CLIENT'S pubkey (from event author), not params[0].
         * params[0] = remote signer pubkey (ignored by bunker, it's our own pubkey)
         * params[1] = optional connect secret
         * params[2] = optional permissions CSV */
        const char *perms = (req.n_params > 2 && req.params && req.params[2]) ? req.params[2] : NULL;
        int allowed = 1;
        if (s->cbs.authorize_cb) {
            /* Pass the CLIENT's pubkey (from event) and requested permissions */
            allowed = s->cbs.authorize_cb(client_pubkey_hex, perms, s->cbs.user_data);
        }
        if (allowed) {
            /* Set ACL for the CLIENT's pubkey with the requested permissions */
            if (is_valid_pubkey_hex_relaxed(client_pubkey_hex)) {
                acl_set_perms(s, client_pubkey_hex, perms);
                if (getenv("NOSTR_DEBUG")) {
                    fprintf(stderr, "[nip46] connect: granted perms '%s' to client %s\n",
                            perms ? perms : "(all)", client_pubkey_hex);
                }
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
    }

    /* 4) Encrypt the reply with the same session-owned mode. */
    char *cipher = NULL;
    int rc = reply_json
                 ? nostr_nip46_transport_encrypt(
                       s, client_pubkey_hex, reply_json, &cipher)
                 : -1;
    if (rc == 0 && cipher) {
        *out_cipher_reply = cipher;
    } else {
        free(cipher);
        rc = -1;
        if (getenv("NOSTR_DEBUG")) {
            fprintf(stderr, "[nip46] encrypt failed (mode=%s)\n",
                    nostr_nip46_transport_mode_name(s->transport_mode));
        }
    }

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

int nostr_nip46_session_set_transport_mode(
    NostrNip46Session *s, NostrNip46TransportMode mode) {
    if (!s) return -1;
    if (mode != NOSTR_NIP46_TRANSPORT_NIP44_V2 &&
        mode != NOSTR_NIP46_TRANSPORT_NIP04_LEGACY &&
        mode != NOSTR_NIP46_TRANSPORT_NIP04_AEAD_V2_EXTENSION) {
        return -1;
    }
    /* Strict AEAD-only builds compile the legacy CBC decrypt path out of
     * nip04 entirely; a session negotiated to the legacy transport could
     * encrypt but never decrypt inbound traffic. Refuse the mode up front. */
    if (mode == NOSTR_NIP46_TRANSPORT_NIP04_LEGACY &&
        !nostr_nip04_legacy_decrypt_enabled()) {
        return -1;
    }
    if (s->client_pool_started || s->listening) return -1;
    s->transport_mode = mode;
    return 0;
}

NostrNip46TransportMode nostr_nip46_session_get_transport_mode(
    const NostrNip46Session *s) {
    return s ? s->transport_mode : (NostrNip46TransportMode)0;
}

const char *nostr_nip46_transport_mode_name(NostrNip46TransportMode mode) {
    switch (mode) {
        case NOSTR_NIP46_TRANSPORT_NIP44_V2:
            return "nip44-v2";
        case NOSTR_NIP46_TRANSPORT_NIP04_LEGACY:
            return "nip04-legacy-cbc";
        case NOSTR_NIP46_TRANSPORT_NIP04_AEAD_V2_EXTENSION:
            return "nip04-aead-v2-extension";
        default:
            return "unsupported";
    }
}

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
static void acl_set_perms(NostrNip46Session *s, const char *client_pk, const char *perms_csv){
    if(!s||!client_pk) return;
    /* remove existing */
    struct PermEntry **pp=&s->acl_head;
    while(*pp){
        if(strcmp((*pp)->client_pk, client_pk)==0){
            struct PermEntry *old=*pp;
            *pp=old->next;
            if(old->methods) {
                for(size_t i=0;i<old->n_methods;++i) free(old->methods[i]);
                free(old->methods);
            }
            free(old->client_pk);
            free(old);
            break;
        }
        pp=&(*pp)->next;
    }
    struct PermEntry *e=(struct PermEntry*)calloc(1,sizeof(*e));
    if(!e) return;
    e->client_pk=strdup(client_pk);
    if(perms_csv && *perms_csv){
        /* Client requested specific permissions */
        if(csv_split(perms_csv, &e->methods, &e->n_methods)!=0){
            e->methods=NULL;
            e->n_methods=0;
        }
    } else {
        /* No permissions specified - grant standard NIP-46 methods as default.
         * This matches behavior of most bunker implementations. */
        static const char *default_perms[] = {
            "sign_event", "get_public_key", "nip04_encrypt", "nip04_decrypt",
            "nip44_encrypt", "nip44_decrypt"
        };
        size_t n_default = sizeof(default_perms)/sizeof(default_perms[0]);
        e->methods = (char**)malloc(n_default * sizeof(char*));
        if(e->methods){
            e->n_methods = n_default;
            for(size_t i=0;i<n_default;++i){
                e->methods[i] = strdup(default_perms[i]);
            }
        }
    }
    e->next=s->acl_head;
    s->acl_head=e;
}
static int acl_has_perm(const NostrNip46Session *s, const char *client_pk, const char *method){ if(!s||!client_pk||!method) return 0; for(const struct PermEntry *it=s->acl_head; it; it=it->next){ if(it->client_pk && strcmp(it->client_pk, client_pk)==0){ if(it->n_methods==0) return 0; for(size_t i=0;i<it->n_methods;++i){ if(it->methods[i] && strcmp(it->methods[i], method)==0) return 1; } return 0; } } return 0; }
