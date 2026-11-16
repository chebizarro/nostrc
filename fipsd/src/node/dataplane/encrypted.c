#include "node/handlers/internal.h"

static bool path_message(const uint8_t *message, size_t len)
{
    return len && (message[0] == FIPS_LINK_PathProbe || message[0] == FIPS_LINK_PathAck ||
        message[0] == FIPS_LINK_PathClose);
}

static void recycle(fipsd_crypto_job *j)
{
    fipsd_dp_packet *p = j->owner;
    go_refptr_release(&p->snapshot);
    sodium_memzero(p->ad, sizeof p->ad); sodium_memzero(p->input, sizeof p->input);
    sodium_memzero(p->output, sizeof p->output);
    sodium_memzero(&p->job, sizeof p->job);
    atomic_store_explicit(&p->busy, false, memory_order_release);
}
fipsd_dp_packet *fipsd_dp_packet_take(fipsd_dataplane *d)
{
    for (size_t n = 0; n < d->packet_count; ++n) {
        fipsd_dp_packet *p = &d->packets[d->next++ % d->packet_count];
        bool expected = false;
        if (!atomic_compare_exchange_strong_explicit(&p->busy, &expected, true,
                memory_order_acquire, memory_order_relaxed)) continue;
        p->plane = d; p->snapshot = d->current; go_refptr_retain(&p->snapshot);
        p->job = (fipsd_crypto_job){.owner = p, .recycle = recycle};
        p->session = p->ce = p->prompt = p->farewell = p->path_message = false;
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
    if (fipsd_node_dispatch_crypto(h->ctx, &p->job) == 0) return true;
    ++h->dataplane->stats.backpressure;
    fipsd_crypto_job_destroy(&p->job); return false;
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
    /* Disconnect is sealed asynchronously but deliberately outlives table removal. */
    uint32_t timestamp; const uint8_t *message; size_t message_len;
    p->farewell = !session && !prompt &&
        fips_fmp_strip_inner_header(plain, len, &timestamp, &message, &message_len) &&
        message_len && message[0] == FIPS_LINK_Disconnect;
    p->path_message = !session && !prompt &&
        fips_fmp_strip_inner_header(plain, len, &timestamp, &message, &message_len) && message_len &&
        path_message(message, message_len);
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
