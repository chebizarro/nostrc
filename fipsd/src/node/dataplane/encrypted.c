#include "node/handlers/internal.h"

/* Path messages are the only sealed link messages on the transport's control
 * lane (dispatch.c), as upstream (node/mod.rs send_path, Lane::Path). Every
 * other sealed message keeps the data lane's order: one sealed under a new
 * link epoch that overtook old-epoch data would be the peer's evidence that
 * this side moved, starting its 10 s drain of the old epoch while that data
 * still waits behind it (nostrc-3vl7v, UPSTREAM.md). */
static bool path_message(const uint8_t *message, size_t len)
{
    return len && (message[0] == FIPS_LINK_PathProbe || message[0] == FIPS_LINK_PathAck ||
        message[0] == FIPS_LINK_PathClose);
}
/* Per-flow worker affinity (nostrc-u19cz.3): one peer's link jobs keep their
 * order on one worker and its session jobs on the next, so the two AEADs of a
 * session datagram (or the two opens of an arrival) pipeline over two workers
 * while each flow completes in order. Node addresses are already hashes. */
uint64_t fipsd_dp_shard(const fips_node_addr *peer, bool session)
{
    uint32_t key; memcpy(&key, peer->bytes, sizeof key);
    return FIPSD_JOB_SHARD(key + (session ? 1u : 0u));
}
static size_t wire_bound(size_t n) { return n < FIPSD_DP_WIRE ? n : FIPSD_DP_WIRE; }
/* Wipe what a tenant can have left, once (nostrc-u19cz.3; this replaced three
 * full-slab wipes per packet, after destroy had already wiped them): header
 * bytes in ad, the copied plaintext or ciphertext in input, and in output the
 * header copy plus at most input + tag AEAD bytes. Every writer (seal, open,
 * open_retry, restart, the completion's header copy and machine steps over
 * p->output) stays inside these. destroy has already destroyed the key; take
 * reinitializes the job. */
static void recycle(fipsd_crypto_job *j)
{
    fipsd_dp_packet *p = j->owner;
    fipsd_dataplane *d = p->plane;
    go_refptr_release(&p->snapshot);
    size_t in = j->input_size;
    if (p->wire_size > p->body && p->wire_size - p->body > in) in = p->wire_size - p->body;
    in = wire_bound(in);
    size_t ad = wire_bound(p->body);
    sodium_memzero(p->ad, ad); sodium_memzero(p->input, in);
    sodium_memzero(p->output, wire_bound(ad + in + FIPS_NOISE_TAG_SIZE));
    if (p->gate_held) {
        /* After the completion sent or dropped it (or it was never queued):
         * later frames of the slot may leave from the worker again. */
        pthread_mutex_lock(&p->gate->mu); --p->gate->held; pthread_mutex_unlock(&p->gate->mu);
    }
    atomic_store_explicit(&p->busy, false, memory_order_release);
    /* busy=false transfers the slab: do not touch its fields after that. */
    atomic_fetch_sub_explicit(&d->in_flight, 1, memory_order_release);
}
bool fipsd_dp_send_ready(const fipsd_dataplane *d)
{
    size_t taken = atomic_load_explicit(&d->in_flight, memory_order_acquire);
    return taken + FIPSD_DP_SEND_RESERVE < d->packet_count;
}
fipsd_dp_packet *fipsd_dp_packet_take(fipsd_dataplane *d)
{
    for (size_t n = 0; n < d->packet_count; ++n) {
        fipsd_dp_packet *p = &d->packets[d->next++ % d->packet_count];
        bool expected = false;
        if (!atomic_compare_exchange_strong_explicit(&p->busy, &expected, true,
                memory_order_acquire, memory_order_relaxed)) continue;
        atomic_fetch_add_explicit(&d->in_flight, 1, memory_order_relaxed);
        p->plane = d; p->snapshot = d->current; go_refptr_retain(&p->snapshot);
        p->job = (fipsd_crypto_job){.owner = p, .recycle = recycle};
        p->session = p->ce = p->prompt = p->farewell = p->path_message = p->restart = false;
        p->body = p->wire_size = 0; /* recycle's wipe extents start empty */
        p->via = NULL; p->gate = NULL; p->worker_sent = p->gate_held = false;
        p->fsp_tried = p->fsp_ok = false;
        return p;
    }
    ++d->stats.backpressure; return NULL;
}
fipsd_dp_packet *fipsd_dp_acquire(handlers *h)
{
    if (!fipsd_dp_publish(h)) return NULL;
    return fipsd_dp_packet_take(h->dataplane);
}
bool fipsd_dp_submit(handlers *h, fipsd_dp_packet *p)
{
    FIPSD_PERF_JOB_TAG(&p->job, p->session);
    /* A worker buys parallelism only while a backlog waits for the
     * dispatcher. Otherwise the handoff and its completion cost two thread
     * wakes, several times the AEAD (nostrc-u19cz.3: CPU per datagram at a
     * paced 100 Mbit/s rose ~55 -> ~88 us; u19cz.14: a few queued events are
     * not a backlog), so run it here. Either way the completion arrives
     * through the results channel, in shard order. */
    if (!fipsd_node_dispatch_backlog(h->ctx) && fipsd_node_run_crypto_inline(h->ctx, &p->job) == 0) return true;
    if (fipsd_node_dispatch_crypto(h->ctx, &p->job) == 0) return true;
    ++h->dataplane->stats.backpressure;
    fipsd_crypto_job_destroy(&p->job); return false;
}
/* On the executing thread (nostrc-u19cz.3). Sends only under the gate's lock,
 * at the generation captured at submit and with no earlier frame of the slot
 * held for the dispatcher: a bump waits for this send to finish, and a frame
 * that may not leave here is held, so the frames behind it wait in turn.
 * Batched (nostrc-u19cz.4): a run of a worker batch on one gate and transport
 * is decided frame by frame in queue order under one hold of the lock, as
 * single sends would be (a held frame holds every later one), and the frames
 * it may send leave in one transport batch, in that order, before the lock
 * is released. Each keeps its own result for its completion's accounting. */
#define WORKER_SEND_RUN 32u
void fipsd_dp_worker_send_batch(fipsd_crypto_job **jobs, size_t n)
{
    for (size_t i = 0; i < n;) {
        fipsd_dp_packet *first = jobs[i]->owner;
        fipsd_dp_gate *g = first->gate;
        size_t k = i + 1;
        while (k < n && k - i < WORKER_SEND_RUN && ((fipsd_dp_packet *)jobs[k]->owner)->gate == g &&
               ((fipsd_dp_packet *)jobs[k]->owner)->via == first->via) ++k;
        fipsd_transport_frame frames[WORKER_SEND_RUN];
        fipsd_dp_packet *sending[WORKER_SEND_RUN];
        int rc[WORKER_SEND_RUN];
        size_t count = 0;
        pthread_mutex_lock(&g->mu);
        uint64_t generation = atomic_load_explicit(&g->generation, memory_order_relaxed);
        for (size_t m = i; m < k; ++m) {
            fipsd_dp_packet *p = jobs[m]->owner;
            if (jobs[m]->result != FIPS_NOISE_OK) continue; /* nothing sealed: never on the wire */
            if (!g->held && generation == p->gate_generation) {
                frames[count] = (fipsd_transport_frame){.to = &p->endpoint, .bytes = p->output,
                    .len = p->body + jobs[m]->output_size};
                sending[count++] = p;
            } else { ++g->held; p->gate_held = true; }
        }
        if (count) {
            (void)fipsd_transport_send_batch(first->via, frames, count, rc);
            for (size_t m = 0; m < count; ++m) { sending[m]->send_rc = rc[m]; sending[m]->worker_sent = true; }
        }
        pthread_mutex_unlock(&g->mu);
        i = k;
    }
}
void fipsd_dp_worker_send(fipsd_crypto_job *j) { fipsd_dp_worker_send_batch(&j, 1); }
/* A data-lane frame of the slot the completion will send (or drop): hold the
 * worker's later sends of the slot behind it, from submit until recycle. */
static void gate_hold(handlers *h, const peer_rec *rec, fipsd_dp_packet *p)
{
    fipsd_dataplane *d = h->dataplane;
    size_t slot = (size_t)(rec - h->recs);
    if (slot >= d->gate_count) return;
    p->gate = &d->gates[slot]; p->gate_held = true;
    pthread_mutex_lock(&p->gate->mu); ++p->gate->held; pthread_mutex_unlock(&p->gate->mu);
}
/* Worker send for a data-lane link frame, as upstream's encrypt workers do:
 * on a transport that allows it, to the active pair the completion would
 * resolve now, if that pair's MTU (the completion's own check) admits the
 * sealed frame. Everything else keeps the completion-time send. */
static void worker_route(handlers *h, peer_rec *rec, fipsd_dp_packet *p, size_t wire)
{
    fipsd_dataplane *d = h->dataplane;
    size_t slot = (size_t)(rec - h->recs);
    if (slot >= d->gate_count) return;
    const fipsd_peer_info *info = fipsd_active_peer_info(rec->peer);
    fipsd_transport *t = hs_transport(h, info->transport);
    if (!t || !t->ops->send_concurrent || !t->ops->send_concurrent(t, &info->addr) ||
        wire > fipsd_dp_link_mtu(h, rec, info->transport, &info->addr)) return;
    p->via = t; p->transport = info->transport; p->endpoint = info->addr;
    p->gate = &d->gates[slot];
    p->gate_generation = atomic_load_explicit(&p->gate->generation, memory_order_relaxed);
    /* A transport that batches sends (Linux UDP) gets the worker's run in one
     * batch; any other sends each frame as its AEAD finishes, as before. */
    if (t->ops->send_batch) p->job.after_batch = fipsd_dp_worker_send_batch;
    else p->job.after = fipsd_dp_worker_send;
}
/* On the executing thread (nostrc-u19cz.3). Nothing is accepted here: the
 * completion runs the link step (FMP replay window), then fipsd_dp_preopened
 * the session's stale-epoch check and authenticated-frame step (FSP window),
 * all on the dispatcher; a failed pre-open falls back to the ordinary open
 * there. The packet's snapshot is immutable and retained, so
 * its session keys are used in place, read-only, tried in snapshot order as
 * open_candidate and fipsd_dp_open_retry do. The plaintext overwrites input,
 * the link ciphertext no longer needed, inside recycle's wipe extent. */
void fipsd_dp_worker_preopen(fipsd_crypto_job *j)
{
    fipsd_dp_packet *p = j->owner;
    const fipsd_dp_snapshot *s = p->snapshot.ptr;
    uint32_t timestamp; const uint8_t *message; size_t n;
    if (j->result != FIPS_NOISE_OK || !s ||
        !fips_fmp_strip_inner_header(p->output + p->body, j->output_size, &timestamp, &message, &n) ||
        n < 2 || message[0] != FIPS_LINK_SessionDatagram) return;
    fips_reader rd; fips_reader_init(&rd, message + 1, n - 1);
    fips_session_datagram dg;
    if (fips_session_datagram_decode(&rd, &dg, NULL) != FIPS_OK || memcmp(dg.dest_addr.bytes, s->me.bytes, 16)) return;
    fips_fsp_encrypted_header header;
    if (!fips_fsp_encrypted_header_parse(dg.payload, dg.payload_len, &header)) return;
    size_t body = FIPS_FRAMING_FSP_HEADER_SIZE;
    if (fips_fsp_encrypted_header_has_coords(&header)) {
        fips_reader cr; fips_reader_init(&cr, dg.payload + body, dg.payload_len - body);
        fips_fsp_coords src, dest;
        if (fips_fsp_parse_encrypted_coords(&cr, &src, &dest, NULL) != FIPS_OK) return;
        body += fips_reader_position(&cr);
    }
    if (body > dg.payload_len || dg.payload_len - body < FIPS_NOISE_TAG_SIZE) return;
    FIPSD_PERF_BEGIN(fsp_start);
    for (size_t i = 0; i < s->epoch_count && !p->fsp_ok; ++i) {
        const fipsd_dp_epoch *e = &s->epochs[i];
        if (!e->session || memcmp(e->peer.bytes, dg.src_addr.bytes, 16)) continue;
        size_t out = 0;
        p->fsp_tried = true; p->fsp_tag = e->tag;
        if (fips_cipher_state_open(&e->recv, header.counter, dg.payload, FIPS_FRAMING_FSP_HEADER_SIZE,
                dg.payload + body, dg.payload_len - body, p->input, FIPSD_DP_WIRE, &out) == FIPS_NOISE_OK) {
            p->fsp_ok = true; p->fsp_plain = out;
            if (j->worker_counts) atomic_fetch_add(&j->worker_counts[j->worker], 1);
        }
    }
    FIPSD_PERF_END(fsp_start, FSP_DECRYPT);
    if (!p->fsp_tried) return; /* no session epoch: the ordinary path drops it */
    p->fsp_src = dg.src_addr; p->fsp_frame = dg.payload_len; p->fsp_body = body;
    p->fsp_counter = header.counter; p->fsp_k = (header.flags & FIPS_FSP_FLAG_K) != 0;
}
void fipsd_dp_link_moved(handlers *h, const peer_rec *rec)
{
    fipsd_dataplane *d = h->dataplane;
    size_t slot = (size_t)(rec - h->recs);
    if (!d || slot >= d->gate_count) return;
    fipsd_dp_gate *g = &d->gates[slot];
    pthread_mutex_lock(&g->mu);
    atomic_store_explicit(&g->generation, atomic_load_explicit(&g->generation, memory_order_relaxed) + 1,
        memory_order_relaxed);
    pthread_mutex_unlock(&g->mu);
}
bool fipsd_dp_seal(handlers *h, peer_rec *rec, fips_session_entry *entry,
    const fips_rekey_sealed *s, const uint8_t *plain, size_t len, bool prompt,
    fips_transport_id transport, const fips_transport_addr *to, bool ce)
{
    fipsd_dp_packet *p = fipsd_dp_acquire(h);
    if (!p) return false;
    bool session = entry != NULL;
    size_t aad = session ? FIPS_FRAMING_FSP_HEADER_SIZE : FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE;
    p->body = aad; p->session = session; p->peer = session ? entry->peer : rec->addr;
    p->transport = transport; if (to) p->endpoint = *to;
    p->tag = s->tag; p->k = s->k; p->ce = ce; p->prompt = prompt;
    p->timestamp = (uint32_t)(s->elapsed.secs * 1000 + s->elapsed.nanos / 1000000);
    if (len > UINT16_MAX || len + aad + FIPS_NOISE_TAG_SIZE > FIPSD_DP_WIRE) goto refuse;
    if (session) {
        uint8_t flags = s->k ? FIPS_FSP_FLAG_K : 0;
        session_ext *x = hs_session_ext(h, entry);
        if (prompt && x && x->has_dest_coords && h->my_coord_count && x->dest_count) {
            size_t nc = h->my_coord_count, nd = x->dest_count;
            if (aad + 4 + (nc + nd) * 16 + len + FIPS_NOISE_TAG_SIZE > FIPSD_DP_WIRE) goto refuse;
            flags |= FIPS_FSP_FLAG_CP;
            fips_writer w; fips_writer_init(&w, p->ad + aad, FIPSD_DP_WIRE - aad);
            if (fips_writer_write_u16_le(&w, (uint16_t)nc, NULL) != FIPS_OK ||
                fips_writer_write_bytes(&w, h->my_coord_addrs[0].bytes, nc * 16, NULL) != FIPS_OK ||
                fips_writer_write_u16_le(&w, (uint16_t)nd, NULL) != FIPS_OK ||
                fips_writer_write_bytes(&w, x->dest_coords[0].bytes, nd * 16, NULL) != FIPS_OK) goto refuse;
            p->body += fips_writer_len(&w);
        }
        fips_fsp_build_header(s->counter, flags, (uint16_t)len, p->ad);
    } else {
        if (!s->theirs.some) goto refuse;
        fips_fmp_build_established_header((fips_session_index){s->theirs.value}, s->counter,
            (s->k ? FIPS_FMP_FLAG_KEY_EPOCH : 0) | (ce ? FIPS_FMP_FLAG_CE : 0), (uint16_t)len, p->ad);
    }
    memcpy(p->output, p->ad, p->body);
    memcpy(p->input, plain, len);
    p->job.operation = FIPSD_CRYPTO_SEAL; p->job.key = s->key; p->job.counter = s->counter;
    p->job.ad = p->ad; p->job.ad_size = aad; p->job.input = p->input; p->job.input_size = len;
    p->job.output = p->output + p->body; p->job.capacity = FIPSD_DP_WIRE - p->body;
    p->job.worker_counts = h->dataplane->stats.sealed;
    p->job.shard = fipsd_dp_shard(&p->peer, session);
    /* Disconnect is sealed asynchronously but deliberately outlives table removal. */
    uint32_t timestamp; const uint8_t *message; size_t message_len;
    if (!session && !prompt && fips_fmp_strip_inner_header(plain, len, &timestamp, &message, &message_len)) {
        p->farewell = message_len && message[0] == FIPS_LINK_Disconnect;
        p->path_message = path_message(message, message_len);
    }
    /* Farewells (which outlive the table) and addressed path control stay
     * on the completion's send. A data-lane frame left to the completion
     * holds the slot's later worker sends behind it (wire order). Path
     * messages take the control lane, outside data-lane order. */
    if (!session && rec && !p->path_message) {
        if (!p->farewell) worker_route(h, rec, p, p->body + len + FIPS_NOISE_TAG_SIZE);
        if (!p->job.after && !p->job.after_batch) gate_hold(h, rec, p);
    }
    return fipsd_dp_submit(h, p);
refuse:
    fipsd_crypto_job_destroy(&p->job); return false;
}
bool fipsd_dp_link_send_ce(handlers *h, peer_rec *rec, fips_transport_id transport,
    const fips_transport_addr *to, const uint8_t *message, size_t len, bool ce)
{
    fipsd_transport *t = hs_transport(h, transport);
    /* Discovery must be able to prove a size above the current path bound. */
    uint16_t mtu = t && path_message(message, len) ? t->ops->mtu(t, to) : fipsd_dp_link_mtu(h, rec, transport, to);
    if (!t || len + (FIPS_FRAMING_FMP_INNER_HEADER_SIZE - 1) + FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE +
        FIPS_NOISE_TAG_SIZE > mtu) {
        fipsd_dp_reject_frame(h, FIPSD_DP_MTU); return false;
    }
    /* A data-lane frame the lane would refuse now is refused before it takes a
     * counter or an AEAD. Under saturation nearly every sealed frame was
     * discarded at the lane, and that dispatcher and worker time starved
     * rekey and receive processing. Farewells keep their attempt. */
    if (!path_message(message, len) && !(len && message[0] == FIPS_LINK_Disconnect) &&
        fipsd_transport_lane_admit(t, FIPSD_TRANSPORT_LANE_DATA, to)) {
        ++h->counters.send_failed; return false;
    }
    fips_rekey_sealed s = {0};
    if (fipsd_active_peer_link_seal(rec->peer, hs_now(h), &s) != FIPS_REKEY_SEAL_OK) {
        ++h->counters.send_failed; return false;
    }
    uint32_t ts = (uint32_t)(s.elapsed.secs * 1000 + s.elapsed.nanos / 1000000);
    fips_writer w; fips_writer_init(&w, h->link_plain, sizeof h->link_plain);
    bool ok = fips_fmp_prepend_inner_header(ts, message, len, &w, NULL) == FIPS_OK &&
        fipsd_dp_seal(h, rec, NULL, &s, h->link_plain, fips_writer_len(&w), false, transport, to, ce);
    /* Reservations are burned even on backpressure. Never roll a nonce back. */
    fips_rekey_sealed_clear(&s);
    if (!ok) ++h->counters.send_failed;
    return ok;
}
static bool open_candidate(handlers *h, fipsd_dp_packet *p, size_t start)
{
    const fipsd_dp_snapshot *s = p->snapshot.ptr;
    for (size_t i = start; i < s->epoch_count; ++i) {
        const fipsd_dp_epoch *e = &s->epochs[i];
        if (e->session != p->session || memcmp(e->peer.bytes, p->peer.bytes, 16)) continue;
        /* Same rule as the dispatcher's hs_index_lookup: a multipath Bundle's
         * frames may arrive on any transport; the AEAD and the dispatcher's
         * replay check still authenticate them. Single-path peers stay strict. */
        if (!p->session && (e->index != p->job.token ||
            (!e->multipath && !fips_transport_id_equal(e->transport, p->transport)))) continue;
        p->candidate = i; p->tag = e->tag; p->job.key = e->recv;
        p->job.operation = FIPSD_CRYPTO_OPEN; p->job.ad = p->ad;
        p->job.ad_size = p->session ? FIPS_FRAMING_FSP_HEADER_SIZE : FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE;
        p->job.input = p->input; p->job.input_size = p->wire_size - p->body;
        p->job.output = p->output + p->body; p->job.capacity = FIPSD_DP_WIRE - p->body;
        p->job.worker_counts = h->dataplane->stats.opened;
        p->job.shard = fipsd_dp_shard(&p->peer, p->session);
        if (!p->session) p->job.after = fipsd_dp_worker_preopen;
        return fipsd_dp_submit(h, p);
    }
    fipsd_crypto_job_destroy(&p->job); return false;
}
bool fipsd_dp_open(handlers *h, peer_rec *rec, fips_session_entry *entry,
    fips_transport_id transport, const fips_transport_addr *from, const uint8_t *frame, size_t len)
{
    if (len > FIPSD_DP_WIRE) return false;
    fipsd_dp_packet *p = fipsd_dp_acquire(h);
    if (!p) return false;
    p->session = entry != NULL; p->peer = entry ? entry->peer : rec->addr;
    p->carrier = rec->addr; p->transport = transport; p->endpoint = *from; p->wire_size = len;
    if (entry) {
        fips_fsp_encrypted_header header;
        if (!fips_fsp_encrypted_header_parse(frame, len, &header)) goto refuse;
        p->body = FIPS_FRAMING_FSP_HEADER_SIZE; p->job.counter = header.counter;
        p->k = (header.flags & FIPS_FSP_FLAG_K) != 0;
        if (fips_fsp_encrypted_header_has_coords(&header)) {
            fips_reader rd; fips_reader_init(&rd, frame + p->body, len - p->body);
            fips_fsp_coords src, dest;
            if (fips_fsp_parse_encrypted_coords(&rd, &src, &dest, NULL) != FIPS_OK) goto refuse;
            p->body += fips_reader_position(&rd);
        }
    } else {
        fips_fmp_encrypted_header header;
        if (!fips_fmp_encrypted_header_parse(frame, len, &header)) goto refuse;
        p->body = FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE; p->job.counter = header.counter;
        p->job.token = header.receiver_idx.value; p->k = (header.flags & FIPS_FMP_FLAG_KEY_EPOCH) != 0;
        p->ce = (header.flags & FIPS_FMP_FLAG_CE) != 0;
    }
    if (p->body > len || len - p->body < FIPS_NOISE_TAG_SIZE) goto refuse;
    memcpy(p->ad, frame, p->body); memcpy(p->input, frame + p->body, len - p->body);
    return open_candidate(h, p, 0);
refuse:
    fipsd_crypto_job_destroy(&p->job); fipsd_dp_reject_frame(h, FIPSD_DP_DECODE); return false;
}
/* Retry FSP's bounded current/pending/previous trial sequence using another
 * owned slab. The node destroys the original completion after callback return. */
bool fipsd_dp_open_retry(handlers *h, const fipsd_dp_packet *old)
{
    fipsd_dp_packet *p = fipsd_dp_packet_take(h->dataplane);
    if (!p) return false;
    go_refptr_release(&p->snapshot); p->snapshot = old->snapshot; go_refptr_retain(&p->snapshot);
    p->session = old->session; p->peer = old->peer; p->carrier = old->carrier;
    p->transport = old->transport; p->endpoint = old->endpoint; p->wire_size = old->wire_size;
    p->body = old->body; p->k = old->k; p->ce = old->ce;
    p->job.counter = old->job.counter; p->job.token = old->job.token;
    memcpy(p->ad, old->ad, p->body); memcpy(p->input, old->input, old->job.input_size);
    return open_candidate(h, p, old->candidate + 1);
}

bool fipsd_dp_link_send(handlers *h, peer_rec *rec, fips_transport_id transport,
    const fips_transport_addr *to, const uint8_t *message, size_t len)
{ return fipsd_dp_link_send_ce(h, rec, transport, to, message, len, false); }
