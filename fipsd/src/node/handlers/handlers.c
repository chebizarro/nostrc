#define _POSIX_C_SOURCE 200809L
#include "node/handlers/internal.h"
#include <stdatomic.h>
#include <time.h>

void fipsd_node_handlers_config_default(fipsd_node_handlers_config *c)
{
    *c = (fipsd_node_handlers_config){.peer_capacity = 256, .tree_peer_capacity = 32,
        .session_capacity = 256, .notice_capacity = 64, .command_capacity = 64,
        .pending_per_destination = 8, .tick_ms = 1000, .tree_announce_min_interval_ms = 500,
        .tree_reeval_ms = 60000, .bloom_debounce_ms = 500, .lookup_recent_expiry_ms = 30000,
        .lookup_attempt_timeouts_secs = {1, 2, 4, 8}, .lookup_attempt_count = 4,
        .timing = fipsd_peer_timing_default(), .parent_hysteresis = 0.2};
}
fips_instant hs_now(handlers *h) { return h->clock.now(h->clock.ctx); }
uint64_t hs_elapsed_ms(fips_instant now, fips_instant earlier)
{
    if (!now.ops || !earlier.ops || now.ops->compare(now, earlier) <= 0) return 0;
    fips_duration d = now.ops->saturating_duration_since(now, earlier);
    uint64_t fraction = d.nanos / 1000000u;
    return d.secs > (UINT64_MAX - fraction) / 1000 ? UINT64_MAX : d.secs * 1000 + fraction;
}
uint64_t hs_until_ms(fips_instant at, fips_instant now)
{
    if (!at.ops) return UINT64_MAX;
    if (at.ops->compare(at, now) <= 0) return 0;
    fips_duration d = at.ops->saturating_duration_since(at, now);
    uint64_t fraction = (d.nanos + 999999u) / 1000000u;
    uint64_t ms = d.secs > (UINT64_MAX - fraction) / 1000 ? UINT64_MAX : d.secs * 1000 + fraction;
    return ms > FIPS_DEADLINE_CEILING_SECS * 1000 ? FIPS_DEADLINE_CEILING_SECS * 1000 : ms;
}
uint64_t hs_min_wait(uint64_t a, fips_instant at, fips_instant now)
{ uint64_t b = hs_until_ms(at, now); return b < a ? b : a; }
uint64_t hs_now_ms(handlers *h) { return hs_elapsed_ms(hs_now(h), h->origin); }
uint64_t hs_wall_ms(handlers *h) { return h->wall_offset_ms + hs_now_ms(h); }
uint64_t hs_wall_secs(handlers *h) { return hs_wall_ms(h) / 1000; }
static uint64_t wall_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
void hs_mark_dirty(handlers *h) { h->dirty = true; }
#ifdef FIPSD_TEST_HOOKS
fipsd_wipe_hook fipsd_test_wipe_hook;
void *fipsd_test_wipe_user;
#endif
void fipsd_node_notice_free(fipsd_node_notice *n)
{
    if (!n) return;
    hs_wipe(NULL, FIPSD_WIPE_NOTICE, n, sizeof *n + n->len);
    free(n);
}
fipsd_transport *hs_transport(handlers *h, fips_transport_id id)
{ return h->registry ? fipsd_transport_find(h->registry, id) : NULL; }
bool hs_peer_identity(handlers *h, const fips_peer_key *key, fips_peer_identity *out)
{
    fips_xonly_pubkey x;
    if (fips_peer_key_to_xonly(h->secp, key, &x) != FIPS_IDENTITY_OK) return false;
    return fips_peer_identity_from_pubkey(h->secp, &x, out) == FIPS_IDENTITY_OK;
}
peer_rec *hs_rec_find(handlers *h, const fips_node_addr *addr)
{
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i)
        if (h->recs[i].used && !memcmp(h->recs[i].addr.bytes, addr->bytes, 16)) return &h->recs[i];
    return NULL;
}
peer_rec *hs_rec_by_peer(handlers *h, const fipsd_active_peer *peer)
{
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i)
        if (h->recs[i].used && h->recs[i].peer == peer) return &h->recs[i];
    return NULL;
}
peer_rec *hs_rec_alloc(handlers *h, fipsd_active_peer *peer)
{
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i) if (!h->recs[i].used) {
        peer_rec *r = &h->recs[i];
        fips_path_bundle *path = r->path;
        memset(r, 0, sizeof(*r));
        r->path = path; r->used = true; r->peer = peer;
        r->addr = fipsd_active_peer_info(peer)->identity.node_addr;
        h->dataplane->needs_publish = true;
        return r;
    }
    return NULL;
}
void hs_rec_free(handlers *h, peer_rec *rec)
{ fipsd_dp_link_moved(h, rec); h->dataplane->needs_publish = true; rec->used = false; rec->peer = NULL; }
void hs_notice_full(handlers *h, const fipsd_node_notice *notice, const uint8_t *payload, size_t len)
{
    if (!h->notices) return;
    /* A state-change notice is observable only after the control snapshot that
     * reflects it; per-datagram notices do not pay for a snapshot. */
    if (notice->kind != FIPSD_NOTICE_DATAGRAM && h->attached) hs_publish(h, hs_now(h));
    fipsd_node_notice *n = malloc(sizeof(*n) + len);
    if (!n) { ++h->counters.notices_dropped; return; }
    *n = *notice; n->len = len;
    FIPSD_PERF_STAMP(n);
    if (len) memcpy(n->payload, payload, len);
    if (go_channel_try_send(h->notices, n) != 0) { fipsd_node_notice_free(n); ++h->counters.notices_dropped; }
}
void hs_notice(handlers *h, fipsd_notice_kind kind, const fips_node_addr *peer, const uint8_t *payload, size_t len)
{
    fipsd_node_notice n = {.kind = kind};
    if (peer) n.peer = *peer;
    hs_notice_full(h, &n, payload, len);
}

fipsd_node_handlers *fipsd_node_handlers_create(const fipsd_node_handlers_config *cfg)
{
    handlers *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    if (cfg) h->cfg = *cfg; else fipsd_node_handlers_config_default(&h->cfg);
    fipsd_node_handlers_config d; fipsd_node_handlers_config_default(&d);
    if (!h->cfg.peer_capacity) h->cfg.peer_capacity = d.peer_capacity;
    if (!h->cfg.tree_peer_capacity) h->cfg.tree_peer_capacity = d.tree_peer_capacity;
    if (h->cfg.tree_peer_capacity > h->cfg.peer_capacity) h->cfg.tree_peer_capacity = h->cfg.peer_capacity;
    if (!h->cfg.session_capacity) h->cfg.session_capacity = d.session_capacity;
    if (!h->cfg.notice_capacity) h->cfg.notice_capacity = d.notice_capacity;
    if (!h->cfg.command_capacity) h->cfg.command_capacity = d.command_capacity;
    if (!h->cfg.pending_per_destination) h->cfg.pending_per_destination = d.pending_per_destination;
    if (h->cfg.pending_per_destination > FIPSD_HANDLERS_PENDING_MAX) h->cfg.pending_per_destination = FIPSD_HANDLERS_PENDING_MAX;
    if (!h->cfg.tick_ms) h->cfg.tick_ms = d.tick_ms;
    if (!h->cfg.tree_announce_min_interval_ms) h->cfg.tree_announce_min_interval_ms = d.tree_announce_min_interval_ms;
    if (!h->cfg.bloom_debounce_ms) h->cfg.bloom_debounce_ms = d.bloom_debounce_ms;
    if (!h->cfg.lookup_recent_expiry_ms) h->cfg.lookup_recent_expiry_ms = d.lookup_recent_expiry_ms;
    if (!h->cfg.lookup_attempt_count) { memcpy(h->cfg.lookup_attempt_timeouts_secs, d.lookup_attempt_timeouts_secs, sizeof d.lookup_attempt_timeouts_secs); h->cfg.lookup_attempt_count = d.lookup_attempt_count; }
    if (!fipsd_peer_timing_valid(h->cfg.timing)) h->cfg.timing = d.timing;
    if (h->cfg.parent_hysteresis <= 0) h->cfg.parent_hysteresis = d.parent_hysteresis;
    h->notices = go_channel_create(h->cfg.notice_capacity);
    h->commands = go_channel_create(h->cfg.command_capacity);
    if (!h->notices || !h->commands || !hs_provider_init(h)) { fipsd_node_handlers_destroy(h); return NULL; }
    return h;
}
static void free_storage(handlers *h)
{
    if (h->recs) for (size_t i = 0; i < h->cfg.peer_capacity; ++i) { free(h->recs[i].path); free(h->recs[i].listing); }
    free(h->recs); free(h->legs); free(h->index_map); free(h->configured); free(h->index_storage);
    free(h->silent_storage); free(h->tree); free(h->tree_peers); free(h->announce_scratch); free(h->coord_scratch);
    free(h->bloom_peers); free(h->leaves); free(h->recent); free(h->pending_lookups); free(h->backoff_entries);
    free(h->forward_entries); free(h->sign_entries); free(h->request_scratch); free(h->response_scratch);
    free(h->cache_slots); free(h->cache_index); free(h->identity_entries); free(h->session_entries);
    if (h->session_ext) for (size_t i = 0; i < h->cfg.session_capacity; ++i) {
        if (h->session_ext[i].pending) hs_wipe(h, FIPSD_WIPE_TEARDOWN, h->session_ext[i].pending, h->session_ext[i].pending_capacity * sizeof(pending_datagram));
        free(h->session_ext[i].pending);
    }
    free(h->session_ext); free(h->setup_entries);
    if (h->held) for (size_t i = 0; i < h->cfg.session_capacity; ++i) {
        if (h->held[i].items) hs_wipe(h, FIPSD_WIPE_TEARDOWN, h->held[i].items, h->cfg.pending_per_destination * sizeof(pending_datagram));
        free(h->held[i].items);
    }
    free(h->held); free(h->history); free(h->history_peers); free(h->history_samples);
    free(h->bloom_actions); free(h->path_outputs); free(h->probe_facts);
    h->bloom_actions = NULL; h->path_outputs = NULL; h->probe_facts = NULL;
    for (size_t i = 0; i < FIPSD_HANDLERS_PROBE_JOBS; ++i) { free(h->probes[i].probe); free(h->probes[i].report); }
    h->recs = NULL; h->legs = NULL; h->index_map = NULL; h->configured = NULL; h->index_storage = NULL;
    h->silent_storage = NULL; h->tree = NULL; h->tree_peers = NULL; h->announce_scratch = NULL; h->coord_scratch = NULL;
    h->bloom_peers = NULL; h->leaves = NULL; h->recent = NULL; h->pending_lookups = NULL; h->backoff_entries = NULL;
    h->forward_entries = NULL; h->sign_entries = NULL; h->request_scratch = NULL; h->response_scratch = NULL;
    h->cache_slots = NULL; h->cache_index = NULL; h->identity_entries = NULL; h->session_entries = NULL;
    h->session_ext = NULL; h->setup_entries = NULL; h->held = NULL; h->history = NULL; h->history_peers = NULL; h->history_samples = NULL;
    memset(h->probes, 0, sizeof h->probes);
}
static void command_discard(handler_command *c);
void fipsd_node_handlers_destroy(fipsd_node_handlers *h)
{
    if (!h) return;
    fipsd_dp_destroy(h->dataplane);
    free_storage(h);
    if (h->commands) {
        void *m;
        while (go_channel_try_receive(h->commands, &m) == 0) command_discard(m);
        go_channel_unref(h->commands);
    }
    if (h->notices) {
        void *m;
        while (go_channel_try_receive(h->notices, &m) == 0) fipsd_node_notice_free(m);
        go_channel_unref(h->notices);
    }
    hs_provider_free(h);
    sodium_memzero(h, sizeof *h);
    free(h);
}
GoChannel *fipsd_node_handlers_notices(fipsd_node_handlers *h) { return h->notices; }

/* M7 (nostrc-f66sr.9): a live control command's waiting timeout is an
 * execution deadline. The queued command and the waiting connection fiber share
 * one retained ticket. The dispatcher claims it before any state mutation; the
 * waiter abandons it when its deadline or shutdown fires. Exactly one of those
 * PENDING transitions wins: a command whose waiter already gave up is dropped
 * unexecuted and counted, and a claimed command always delivers one signal
 * (execution is synchronous on the dispatcher), so execution and reply stay
 * paired. The ticket owns the JSON response until the last owner releases it;
 * the done channel carries only a wake-up token, never ownership. */
typedef enum ticket_state { TICKET_PENDING, TICKET_CLAIMED, TICKET_ABANDONED, TICKET_DROPPED } ticket_state;
struct hs_control_ticket {
    _Atomic int refs;           /* the queued command and the waiting fiber */
    _Atomic int state;          /* ticket_state; leaves PENDING exactly once */
    _Atomic(json_t *) response; /* owned; stolen by the waiter or released with the ticket */
    GoChannel *done;            /* capacity 1, one signal per ticket, owned by the ticket */
};
static hs_control_ticket *ticket_new(void)
{
    hs_control_ticket *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->done = go_channel_create(1);
    if (!t->done) { free(t); return NULL; }
    atomic_init(&t->refs, 2); atomic_init(&t->state, TICKET_PENDING); atomic_init(&t->response, NULL);
    return t;
}
static void ticket_unref(hs_control_ticket *t)
{
    if (!t || atomic_fetch_sub(&t->refs, 1) != 1) return;
    json_t *left = atomic_exchange(&t->response, NULL);
    if (left) json_decref(left);
    go_channel_unref(t->done);
    free(t);
}
static bool ticket_leave_pending(hs_control_ticket *t, ticket_state to)
{
    int expected = TICKET_PENDING;
    return atomic_compare_exchange_strong(&t->state, &expected, (int)to);
}
/* Dispatcher side, after CLAIMED or DROPPED: publish the outcome and wake the
 * waiter. The capacity-one send cannot fail, since each ticket is resolved once
 * and nobody else sends; the token is not owned by the channel. */
static void ticket_resolve(hs_control_ticket *t, json_t *response)
{
    atomic_store(&t->response, response);
    (void)go_channel_try_send(t->done, t);
}
/* Every exit of a queued command (executed, refused, cancelled, enqueue
 * failure, shutdown drain) releases its HCMD_SEND payload here. */
static void command_free(handler_command *c)
{
    if (c->params) json_decref(c->params);
    ticket_unref(c->ticket);
    if (c->len) hs_wipe(NULL, FIPSD_WIPE_COMMAND, c->payload, c->len);
    free(c);
}
/* A queued command that will never execute (node not running or destroyed):
 * wake a still-waiting fiber with no reply before releasing the command. */
static void command_discard(handler_command *c)
{
    if (c->ticket && ticket_leave_pending(c->ticket, TICKET_DROPPED)) ticket_resolve(c->ticket, NULL);
    command_free(c);
}

static bool allocate(handlers *h)
{
    size_t cap = h->cfg.peer_capacity, scap = h->cfg.session_capacity;
    h->recs = calloc(cap, sizeof(*h->recs));
    h->legs = calloc(cap, sizeof(*h->legs));
    h->index_map_capacity = cap * 4;
    h->index_map = calloc(h->index_map_capacity, sizeof(*h->index_map));
    h->configured = calloc(cap, sizeof(*h->configured));
    h->index_storage = calloc(cap * 4, sizeof(*h->index_storage));
    h->silent_storage = calloc(cap, sizeof(*h->silent_storage));
    h->tree = calloc(1, sizeof(*h->tree));
    h->tree_peers = calloc(h->cfg.tree_peer_capacity, sizeof(*h->tree_peers));
    h->announce_scratch = calloc(1, sizeof(*h->announce_scratch));
    h->coord_scratch = calloc(1, sizeof(*h->coord_scratch));
    h->bloom_peers = calloc(cap, sizeof(*h->bloom_peers));
    h->leaves = calloc(cap, sizeof(*h->leaves));
    h->recent = calloc(FIPS_MIN_RECENT_PER_PEER * 2, sizeof(*h->recent));
    h->pending_lookups = calloc(scap, sizeof(*h->pending_lookups));
    h->backoff_entries = calloc(scap, sizeof(*h->backoff_entries));
    h->forward_entries = calloc(FIPS_MIN_RECENT_PER_PEER, sizeof(*h->forward_entries));
    h->sign_entries = calloc(cap, sizeof(*h->sign_entries));
    h->request_scratch = calloc(1, sizeof(*h->request_scratch));
    h->response_scratch = calloc(1, sizeof(*h->response_scratch));
    size_t slots = FIPS_COORD_CACHE_SLOTS(h->cfg.cache_entries) + 8;
    h->cache_slots = calloc(slots, sizeof(*h->cache_slots));
    h->cache_index = calloc(FIPS_COORD_CACHE_INDEX_WORDS(slots), sizeof(*h->cache_index));
    h->identity_entries = calloc(scap, sizeof(*h->identity_entries));
    h->session_entries = calloc(scap, sizeof(*h->session_entries));
    h->session_ext = calloc(scap, sizeof(*h->session_ext));
    h->setup_entries = calloc(cap, sizeof(*h->setup_entries));
    h->held = calloc(scap, sizeof(*h->held));
    h->bloom_actions = calloc(cap + 1, sizeof(*h->bloom_actions));
    h->path_outputs = calloc(FIPS_PATH_OUTPUTS_MAX, sizeof(*h->path_outputs));
    h->probe_facts = calloc(1, sizeof(*h->probe_facts));
    h->history = calloc(1, sizeof(*h->history));
    size_t hcap = cap;
    h->history_samples = calloc(cap, sizeof(*h->history_samples));
    h->history_peers = calloc(hcap, sizeof(*h->history_peers));
    if (!h->recs || !h->legs || !h->index_map || !h->configured || !h->index_storage || !h->silent_storage ||
        !h->tree || !h->tree_peers || !h->announce_scratch || !h->coord_scratch || !h->bloom_peers || !h->leaves ||
        !h->recent || !h->pending_lookups || !h->backoff_entries || !h->forward_entries || !h->sign_entries ||
        !h->request_scratch || !h->response_scratch || !h->cache_slots || !h->cache_index || !h->identity_entries ||
        !h->session_entries || !h->session_ext || !h->setup_entries || !h->held || !h->history || !h->history_peers ||
        !h->bloom_actions || !h->path_outputs || !h->probe_facts || !h->history_samples)
        return false;
    for (size_t i = 0; i < scap; ++i) {
        h->session_ext[i].pending = calloc(h->cfg.pending_per_destination, sizeof(pending_datagram));
        h->session_ext[i].pending_capacity = h->cfg.pending_per_destination;
        if (!h->session_ext[i].pending) return false;
    }
    fips_stats_history_init(h->history, h->history_peers, hcap);
    fipsd_stats_views_invalidate(&h->stats_views);
    return true;
}
static void load_core(handlers *h)
{
    if (!h->cfg.has_core) {
        GoRefPtr cfg = fipsd_node_config(h->ctx);
        if (cfg.ptr) {
            fipsd_config_core_params(cfg.ptr, &h->cfg.core);
            h->cfg.has_core = true;
            const fipsd_config *c = cfg.ptr;
            if (!h->cfg.link_dead_timeout_ms)
                h->cfg.link_dead_timeout_ms = fipsd_config_uint(fipsd_config_get(c, "node.link_dead_timeout_secs")) * 1000;
            if (!h->cfg.session_idle_timeout_ms) {
                const fipsd_config_value *v = fipsd_config_get(c, "node.session.idle_timeout_secs");
                if (v) h->cfg.session_idle_timeout_ms = fipsd_config_uint(v) * 1000;
            }
            if (!h->cfg.cache_entries) h->cfg.cache_entries = h->cfg.core.coord_size;
            if (!h->cfg.path_enabled) h->cfg.path_enabled = fipsd_config_bool(fipsd_config_get(c, "node.path.enabled"));
            if (!h->cfg.leaf_only) h->cfg.leaf_only = fipsd_config_bool(fipsd_config_get(c, "node.bloom.leaf_only"));
        }
        go_refptr_release(&cfg);
    }
    if (!h->cfg.has_core) {
        h->cfg.core.link_rekey = fips_rekey_config_fmp(); h->cfg.core.session_rekey = fips_rekey_config_fsp();
        h->cfg.core.handshake = (fips_bucket_params){FIPS_DEFAULT_BURST_CAPACITY, FIPS_DEFAULT_REFILL_RATE};
        h->cfg.core.established = h->cfg.core.handshake; h->cfg.core.session_setup = h->cfg.core.handshake;
        h->cfg.core.max_pending_inbound = 64; h->cfg.core.coord_ttl_ms = FIPS_DEFAULT_COORD_CACHE_TTL_MS;
        h->cfg.core.mmp_mode = FIPS_MMP_Full; h->cfg.core.session_mmp_mode = FIPS_MMP_Full;
        h->cfg.core.mmp_window = FIPS_MMP_DEFAULT_OWD_WINDOW_SIZE; h->cfg.core.session_mmp_window = FIPS_MMP_DEFAULT_OWD_WINDOW_SIZE;
        h->cfg.core.mmp_log_interval_secs = FIPS_MMP_DEFAULT_LOG_INTERVAL_SECS;
        h->cfg.core.session_mmp_log_interval_secs = FIPS_MMP_DEFAULT_LOG_INTERVAL_SECS;
    }
    if (!h->cfg.cache_entries) h->cfg.cache_entries = 1024;
    if (!h->cfg.link_dead_timeout_ms) h->cfg.link_dead_timeout_ms = 60000;
    if (!h->cfg.core.coord_ttl_ms) h->cfg.core.coord_ttl_ms = FIPS_DEFAULT_COORD_CACHE_TTL_MS;
    if (fips_rekey_params_new(h->cfg.core.link_rekey, &h->link_params) != FIPS_REKEY_PARAMS_OK) {
        FIPSD_WARN(HMOD, "link rekey parameters refused; using the FMP defaults");
        h->link_params = fips_rekey_params_fmp();
    }
    if (fips_rekey_params_new(h->cfg.core.session_rekey, &h->session_params) != FIPS_REKEY_PARAMS_OK) {
        FIPSD_WARN(HMOD, "session rekey parameters refused; using the FSP defaults");
        h->session_params = fips_rekey_params_fsp();
    }
}
static bool attach(void *user, const fipsd_node_attach *a)
{
    handlers *h = user;
    if (h->attached) return false;
    h->ctx = a->context; h->peers = a->peers; h->registry = a->registry; h->runtime = a->runtime; h->cancel = a->cancel;
    h->clock = a->peers->clock;
    h->entropy = h->cfg.entropy.fill ? h->cfg.entropy : fipsd_host_os_entropy();
    h->identity = fipsd_node_identity(h->ctx);
    h->secp = (fips_secp *)fipsd_node_secp(h->ctx);
    if (!h->identity || !h->secp) return false;
    h->me = fips_identity_node_addr(h->identity);
    fips_xonly_pubkey key = fips_identity_pubkey(h->identity);
    h->my_key = fips_peer_key_from_xonly(&key);
    memcpy(h->epoch, fipsd_node_startup_epoch(h->ctx), 8);
    (void)fips_identity_npub(h->identity, h->npub, sizeof h->npub);
    h->local = (fips_rekey_node_keys){.secp = h->secp, .keypair = h->identity, .entropy = h->entropy};
    memcpy(h->local.epoch, h->epoch, 8);
    h->origin = h->started_at = hs_now(h);
    h->wall_offset_ms = wall_now_ms();
    fipsd_diag_init(&h->diag, h->origin);
    fipsd_metrics_init(&h->metrics);
    h->next_tick = fips_deadline(h->origin, fips_duration_from_millis(h->cfg.tick_ms));
    h->publish_allowed = h->origin;
    h->timers = (hs_timers){.stale = true};
    h->next_link_id = 1;
    load_core(h);
    if (!allocate(h) || !fipsd_dp_init(h)) return false;
    fips_index_allocator_init(&h->indices, h->index_storage, h->cfg.peer_capacity * 4, FIPS_INDEX_DEFAULT_MAX_ATTEMPTS, h->entropy);
    fips_token_bucket stranger, established;
    fips_token_bucket_init(&stranger, h->cfg.core.handshake, h->origin);
    fips_token_bucket_init(&established, h->cfg.core.established, h->origin);
    fips_handshake_init(&h->hs_limiter, stranger, established, h->cfg.core.max_pending_inbound ? h->cfg.core.max_pending_inbound : 64);
    fips_fmp_silent_sessions_init(&h->silent, h->silent_storage, h->cfg.peer_capacity, h->origin);
    fips_session_setup_init(&h->setup_limiter, h->setup_entries, h->cfg.peer_capacity, h->cfg.core.session_setup, h->cfg.core.session_setup);
    fips_lookup_sign_init(&h->sign_limiter, h->sign_entries, h->cfg.peer_capacity, FIPS_DEFAULT_SIGN_BURST, FIPS_DEFAULT_SIGN_RATE);
    fipsd_identity_cache_init(&h->identities, h->identity_entries, h->cfg.session_capacity);
    fips_router_init(&h->router, h->origin);
    size_t slots = FIPS_COORD_CACHE_SLOTS(h->cfg.cache_entries) + 8;
    if (fips_coord_cache_init(&h->cache, h->cache_slots, slots, h->cache_index, FIPS_COORD_CACHE_INDEX_WORDS(slots),
            h->cfg.cache_entries, h->cfg.core.coord_ttl_ms, h->origin, h->entropy) != FIPS_OK) return false;
    hs_provider_cache_reset(h);
    if (!hs_tree_init(h) || !hs_bloom_init(h) || !hs_lookup_init(h) || !hs_session_init(h)) return false;
    h->attached = h->running = true;
    FIPSD_INFO(HMOD, "handlers attached: %s", h->npub);
    hs_publish(h, h->origin);
    return true;
}
static void detach(void *user)
{
    handlers *h = user;
    if (!h->attached) return;
    h->running = false;
    hs_probe_close_all(h);
    hs_session_close_all(h);
    hs_close_all(h);
    hs_publish(h, hs_now(h));
    /* Every lease must have returned through Release outputs. */
    if (fips_index_allocator_count(&h->indices))
        FIPSD_WARN(HMOD, "%zu session indices still allocated at detach", fips_index_allocator_count(&h->indices));
}
static bool on_transport_event(void *user, const fipsd_transport_event *e)
{
    handlers *h = user;
    fipsd_transport *t = hs_transport(h, e->id);
    if (!t || !h->running) return true;
    hs_timers_touch(h);
    /* .7: packet classification and owned authentication jobs live in dataplane. */
    if (e->kind == FIPSD_TRANSPORT_PACKET) return fipsd_dp_rx(h, e);
    hs_timers_invalidate(h); /* discovery, connect and disconnect start/end legs, retries and peers */
    if (e->kind == FIPSD_TRANSPORT_DISCOVERED) {
        if (!strcmp(t->type, "ethernet") && t->auto_connect && e->size == 32) {
            fips_xonly_pubkey key; fips_peer_identity peer;
            memcpy(key.bytes, e->bytes, 32);
            if (fips_peer_identity_from_pubkey(h->secp, &key, &peer) == FIPS_IDENTITY_OK &&
                memcmp(peer.node_addr.bytes, h->me.bytes, 16) && !hs_rec_find(h, &peer.node_addr))
                (void)hs_dial(h, &peer, e->id, &e->address, true);
        }
        for (size_t i = 0; i < h->cfg.peer_capacity; ++i) {
            configured_peer *c = &h->configured[i];
            if (c->used && fips_transport_id_equal(c->transport, e->id) && fips_transport_addr_equal(&c->addr, &e->address) &&
                !hs_rec_find(h, &c->identity.node_addr) && !c->dialing)
                (void)hs_dial(h, &c->identity, c->transport, &c->addr, true);
        }
        /* Upstream poll_transport_discovery: a beacon carrying the peer's
         * x-only key (DiscoveredPeer::pubkey_hint) is dialled on an
         * auto-connect transport; hs_dial skips self, known peers and legs. */
        if (e->size == 32 && t->auto_connect) {
            fips_xonly_pubkey key; fips_peer_identity identity;
            memcpy(key.bytes, e->bytes, 32);
            if (fips_peer_identity_from_pubkey(h->secp, &key, &identity) == FIPS_IDENTITY_OK)
                (void)hs_dial(h, &identity, e->id, &e->address, false);
        }
        return true;
    }
    if (e->kind == FIPSD_TRANSPORT_CONNECTED || e->kind == FIPSD_TRANSPORT_DISCONNECTED) {
        for (size_t i = 0; i < h->cfg.peer_capacity; ++i) {
            pending_leg *leg = &h->legs[i];
            if (!leg->used || !fips_transport_id_equal(leg->transport, e->id) || !fips_transport_addr_equal(&leg->addr, &e->address)) continue;
            fipsd_peer_event ev = {.kind = e->kind == FIPSD_TRANSPORT_CONNECTED ? FIPSD_PEER_CONNECTED : FIPSD_PEER_CONNECT_FAILED};
            fipsd_peer_actions actions;
            fipsd_peer_machine_step(&leg->machine, hs_now(h), &ev, &actions);
            if (!hs_leg_effects(h, leg, &actions)) return true;
        }
        if (e->kind == FIPSD_TRANSPORT_DISCONNECTED) {
            fipsd_active_peer *p = fipsd_peer_table_endpoint(h->peers->table, e->id, &e->address);
            peer_rec *rec = p ? hs_rec_by_peer(h, p) : NULL;
            if (rec) hs_remove_peer(h, rec, FIPS_REKEY_CLOSE_LinkDead, false, FIPS_FMP_TransportFailure);
        }
    }
    return true;
}
static bool on_crypto_result(void *user, const fipsd_crypto_job *job)
{
    hs_timers_touch(user);
    return fipsd_dp_result(user, job);
}
static void on_network_event(void *user, const fipsd_netmon_event *e)
{
    handlers *h = user;
    if (!h->running) return;
    hs_timers_invalidate(h); /* presence and medium changes re-step every Bundle */
    h->dataplane->needs_publish = true; hs_netmon_event(h, e);
}
static bool peer_effects(void *user, fipsd_active_peer *peer, const fipsd_peer_actions *actions)
{ hs_timers_touch(user); return hs_peer_effects(user, peer, actions); }
static uint64_t wait_ms(void *user, fips_instant now) { return hs_wait_ms(user, now); }
static bool poll(void *user, fips_instant now)
{
    handlers *h = user;
    /* Catch timer-driven topology/cost/cache expiry changes, not every packet. */
    if (hs_until_ms(h->next_tick, now) == 0 || hs_timers_due(h, HS_TIMER_PATHS, now))
        h->dataplane->needs_publish = true;
    return hs_poll(h, now) && fipsd_dp_publish(h);
}
static bool on_command(void *user, void *message)
{
    handlers *h = user;
    handler_command *c = message;
    if (!h->running) { command_discard(c); return true; }
    /* Execution deadline: a waiter that already gave up (timeout or shutdown)
     * abandoned the ticket, so nothing below runs: no mutation, no publish. */
    if (c->kind == HCMD_CONTROL && !ticket_leave_pending(c->ticket, TICKET_CLAIMED)) {
        ++h->counters.commands_cancelled;
        command_free(c);
        return true;
    }
    hs_timers_touch(h);
    if (c->kind != HCMD_SEND) { h->dataplane->needs_publish = true; hs_timers_invalidate(h); }
    switch (c->kind) {
    case HCMD_CONNECT: (void)hs_dial(h, &c->identity, c->transport, &c->addr, false); break;
    case HCMD_DISCONNECT: (void)hs_disconnect(h, &c->node, FIPS_FMP_Shutdown, true); break;
    case HCMD_SEND:
        FIPSD_PERF_WAIT(c, ENDPOINT_COMMAND_WAIT);
        FIPSD_PERF_BLOCK(ENDPOINT_SEND, (void)hs_session_send(h, &c->node, c->src_port, c->dst_port, c->payload, c->len));
        break;
    case HCMD_LEARN_IDENTITY: (void)fipsd_identity_cache_put(&h->identities, c->identity, hs_wall_ms(h)); break;
    case HCMD_DIAL_CONFIGURED: (void)hs_dial_configured(h); break;
    case HCMD_PATH_INTAKE: hs_path_intake(h, &c->node, c->feed, c->transport, &c->addr); break;
    case HCMD_CONTROL: {
        /* A completed control command is a snapshot barrier, including for
         * earlier mailbox mutations (e.g. DNS identity seeding). Publish before
         * waking the waiter, not at the end of a coalesced dispatcher batch. */
        json_t *reply = hs_control_command(h, c->name, c->params);
        hs_publish(h, hs_now(h));
        ticket_resolve(c->ticket, reply);
        break;
    }
    }
    /* Originated datagrams change counters only; those are published from the
     * tick. Every other command may have changed control-visible state. */
    if (c->kind != HCMD_SEND && c->kind != HCMD_CONTROL) hs_mark_dirty(h);
    command_free(c);
    return true;
}
/* Dispatcher admission for the command mailbox: hold originated sends while
 * the packet pool lacks headroom for inbound opens (fipsd_dp_send_ready). */
static bool command_ready(void *user)
{
    handlers *h = user;
    return !h->running || !h->dataplane || fipsd_dp_send_ready(h->dataplane);
}
void fipsd_node_handlers_install(fipsd_node_handlers *h, fipsd_node_options *o)
{
    o->user = h;
    o->transport_event = on_transport_event; o->crypto_result = on_crypto_result; o->network_event = on_network_event;
    o->attach = attach; o->detach = detach; o->wait_ms = wait_ms; o->poll = poll;
    o->peer_effects = peer_effects; o->peer_result = NULL;
    o->commands = h->commands; o->command = on_command; o->command_ready = command_ready;
    /* Every slab the pool can hand out must find a worker queue slot. */
    if (o->crypto_queue_capacity < FIPSD_DP_PACKETS) o->crypto_queue_capacity = FIPSD_DP_PACKETS;
    o->clock = h->cfg.clock;
    if (!o->peer_capacity) o->peer_capacity = h->cfg.peer_capacity;
}
static int enqueue(handlers *h, handler_command *c)
{
    if (go_channel_try_send(h->commands, c) != 0) { command_free(c); return -1; }
    return 0;
}
static handler_command *command_new(handler_command_kind kind, size_t len)
{
    handler_command *c = calloc(1, sizeof(*c) + len);
    if (c) { c->kind = kind; c->len = len; FIPSD_PERF_STAMP(c); }
    return c;
}
int fipsd_node_handlers_connect(fipsd_node_handlers *h, const fips_peer_identity *peer,
                                fips_transport_id transport, const fips_transport_addr *addr)
{
    handler_command *c = command_new(HCMD_CONNECT, 0);
    if (!c) return -1;
    c->identity = *peer; c->transport = transport; c->addr = *addr;
    return enqueue(h, c);
}
int fipsd_node_handlers_disconnect(fipsd_node_handlers *h, const fips_node_addr *peer)
{
    handler_command *c = command_new(HCMD_DISCONNECT, 0);
    if (!c) return -1;
    c->node = *peer;
    return enqueue(h, c);
}
int fipsd_node_handlers_send(fipsd_node_handlers *h, const fips_node_addr *dest,
                             uint16_t src_port, uint16_t dst_port, const uint8_t *payload, size_t len)
{
    if (len > FIPSD_HANDLERS_DATAGRAM_MAX) return -1;
    handler_command *c = command_new(HCMD_SEND, len);
    if (!c) return -1;
    c->node = *dest; c->src_port = src_port; c->dst_port = dst_port;
    if (len) memcpy(c->payload, payload, len);
    return enqueue(h, c);
}
int fipsd_node_handlers_send_wait(fipsd_node_handlers *h, const fips_node_addr *dest, uint16_t src_port,
                                  uint16_t dst_port, const uint8_t *payload, size_t len, GoChannel *cancel)
{
    if (len > FIPSD_HANDLERS_DATAGRAM_MAX) return -1;
    handler_command *c = command_new(HCMD_SEND, len);
    if (!c) return -1;
    c->node = *dest; c->src_port = src_port; c->dst_port = dst_port;
    if (len) memcpy(c->payload, payload, len);
    if (go_channel_try_send(h->commands, c) == 0) return 0;
    if (go_channel_is_closed(h->commands)) { command_free(c); return -1; }
    /* Park until the dispatcher drains the mailbox (it wakes this select) or
     * the caller's cancellation fires: back-pressure, never a timed retry. */
    void *token = NULL;
    GoSelectCase cases[] = {{GO_SELECT_SEND, h->commands, c, NULL}, {GO_SELECT_RECEIVE, cancel, NULL, &token}};
    if (go_select(cases, cancel ? 2 : 1) == 0) return 0;
    command_free(c); return -1;
}
int fipsd_node_handlers_learn_identity(fipsd_node_handlers *h, const fips_peer_identity *peer)
{
    handler_command *c = command_new(HCMD_LEARN_IDENTITY, 0);
    if (!c) return -1;
    c->identity = *peer;
    return enqueue(h, c);
}
int fipsd_node_handlers_path_intake(fipsd_node_handlers *h, const fips_node_addr *peer, fipsd_path_feed feed,
                                    fips_transport_id transport, const fips_transport_addr *addr)
{
    handler_command *c = command_new(HCMD_PATH_INTAKE, 0);
    if (!c) return -1;
    c->node = *peer; c->feed = feed; c->transport = transport; c->addr = *addr;
    return enqueue(h, c);
}
int fipsd_node_handlers_dial_configured(fipsd_node_handlers *h)
{
    handler_command *c = command_new(HCMD_DIAL_CONFIGURED, 0);
    return c ? enqueue(h, c) : -1;
}
/* Live control commands run on connection fibers (provider.c builds them). */
json_t *hs_control_enqueue(handlers *h, const char *name, const json_t *params, const fipsd_control_request_context *rc);
json_t *hs_control_enqueue(handlers *h, const char *name, const json_t *params, const fipsd_control_request_context *rc)
{
    handler_command *c = command_new(HCMD_CONTROL, 0);
    if (!c) return NULL;
    snprintf(c->name, sizeof c->name, "%s", name);
    c->params = params ? json_deep_copy(params) : NULL;
    /* Two references: the queued command's and this waiter's. */
    hs_control_ticket *t = ticket_new();
    if (!t) { command_free(c); return NULL; }
    c->ticket = t;
    if (go_channel_try_send(h->commands, c) != 0) { command_free(c); ticket_unref(t); return NULL; }
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
    uint64_t budget_ms = rc && rc->deadline_ns > now_ns ? (rc->deadline_ns - now_ns) / 1000000u : 0;
    void *token = NULL, *cancel = NULL;
    GoSelectCase cases[] = {
        {GO_SELECT_RECEIVE, t->done, NULL, &token},
        {GO_SELECT_RECEIVE, rc && rc->shutdown ? go_context_done(rc->shutdown) : NULL, NULL, &cancel}};
    GoSelectResult r = go_select_timeout(cases, 2, budget_ms ? budget_ms : 1);
    if (r.selected_case != 0) {
        /* Deadline or shutdown. If the dispatcher has not reached the command
         * it now never will: abandoned, dropped unexecuted, counted there. */
        if (ticket_leave_pending(t, TICKET_ABANDONED)) { ticket_unref(t); return NULL; }
        /* Claimed (or dropped) in the same instant: the dispatcher is committed
         * to exactly one signal, so take it rather than orphan a reply that may
         * reflect a mutation. One bounded wait on that signal, not a poll. */
        (void)go_channel_receive(t->done, &token);
    }
    json_t *out = atomic_exchange(&t->response, NULL);
    ticket_unref(t);
    return out;
}
