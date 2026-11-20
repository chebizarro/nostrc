#include <fips/proto/fmp/core.h>
#include <fips/proto/math.h>
#include <sodium.h>
#include <string.h>

uint64_t fips_fmp_elapsed_ms(fips_instant now, fips_instant earlier)
{
    fips_duration d = now.ops->saturating_duration_since(now, earlier);
    uint64_t ms = d.nanos / 1000000;
    if (d.secs > (UINT64_MAX - ms) / 1000) return UINT64_MAX;
    return d.secs * 1000 + ms;
}
bool fips_fmp_epochs_differ(fips_fmp_epoch a, fips_fmp_epoch b)
{ return a.present && b.present && memcmp(a.bytes, b.bytes, sizeof(a.bytes)) != 0; }
void fips_fmp_msg1_digest_of(const uint8_t *wire, size_t len, fips_fmp_msg1_digest *out)
{
    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);
    if (len) crypto_hash_sha256_update(&state, wire, len);
    crypto_hash_sha256_final(&state, out->bytes);
}
void fips_fmp_msg1_digest_prefix(const fips_fmp_msg1_digest *d, uint8_t out[4])
{ memcpy(out, d->bytes, 4); }
bool fips_fmp_ephemeral_digest_of_noise_msg1(const uint8_t *noise, size_t len, fips_fmp_ephemeral_digest *out)
{
    if (len < FIPS_FRAMING_NOISE_PUBKEY_SIZE) return false;
    crypto_hash_sha256(out->bytes, noise, FIPS_FRAMING_NOISE_PUBKEY_SIZE);
    return true;
}
bool fips_fmp_cross_connection_winner(const fips_node_addr *ours, const fips_node_addr *theirs, bool outbound)
{ return memcmp(ours->bytes, theirs->bytes, sizeof(ours->bytes)) < 0 ? outbound : !outbound; }
bool fips_fmp_promotion_node_addr(const fips_fmp_promotion_result *r, fips_node_addr *out)
{
    if (r->kind == FIPS_FMP_CrossConnectionLost) return false;
    *out = r->node_addr;
    return true;
}
bool fips_fmp_promotion_should_close_this_connection(const fips_fmp_promotion_result *r)
{ return r->kind == FIPS_FMP_CrossConnectionLost; }
bool fips_fmp_promotion_link_to_close(const fips_fmp_promotion_result *r, fips_link_id *out)
{
    if (r->kind != FIPS_FMP_CrossConnectionWon) return false;
    *out = r->loser_link_id;
    return true;
}
fips_fmp_inbound_decision fips_fmp_establish_inbound(const fips_fmp_establish_snapshot *s, const fips_fmp_wire_outcome *w)
{
    fips_fmp_inbound_decision d = {.kind = FIPS_FMP_IN_Promote, .peer = w->peer_identity.node_addr};
    bool silent = s->has_silent_backoff && !fips_fmp_epochs_differ(s->silent_backoff.epoch, w->remote_epoch);
    if (s->at_max_peers && !s->has_existing_peer) {
        d.kind = FIPS_FMP_IN_Reject; d.reason = FIPS_FMP_AtMaxPeers;
    } else if (s->has_existing_peer && fips_fmp_epochs_differ(s->existing_peer_epoch, w->remote_epoch)) {
        d.kind = FIPS_FMP_IN_RestartThenPromote;
    } else if (s->has_existing_peer && !s->replace) {
        d.kind = FIPS_FMP_IN_Rekey;
    } else if (silent) {
        d.kind = FIPS_FMP_IN_Reject; d.reason = FIPS_FMP_SilentBackoff;
    } else if (s->has_existing_peer) {
        d.kind = FIPS_FMP_IN_ReplaceThenPromote;
    }
    return d;
}
fips_fmp_outbound_decision fips_fmp_establish_outbound(const fips_fmp_outbound_snapshot *s, fips_fmp_epoch epoch)
{
    if (!s->has_existing_peer) return FIPS_FMP_OUT_Promote;
    if (fips_fmp_epochs_differ(s->existing_peer_epoch, epoch)) return FIPS_FMP_OUT_RestartThenPromote;
    return s->own_dial || s->our_outbound_wins ? FIPS_FMP_OUT_CrossConnectionSwap : FIPS_FMP_OUT_CrossConnectionKeep;
}
fips_status fips_fmp_poll_timeouts(const fips_fmp_conn_snapshot *s, size_t count, fips_fmp_action_list *a)
{
    size_t needed = count, available = a->capacity - a->len;
    if (needed > available) return FIPS_ERR_RANGE;
    for (size_t i = 0; i < count; ++i) {
        if (s[i].is_outbound && s[i].has_retry_addr) {
            if (needed == available) return FIPS_ERR_RANGE;
            ++needed;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        if (s[i].is_outbound && s[i].has_retry_addr)
            a->items[a->len++] = (fips_fmp_conn_action){.kind = FIPS_FMP_ScheduleRetry, .peer = s[i].retry_addr};
        a->items[a->len++] = (fips_fmp_conn_action){.kind = FIPS_FMP_Teardown, .link = s[i].link};
    }
    return FIPS_OK;
}
fips_status fips_fmp_poll_resends(const fips_fmp_conn_snapshot *s, size_t count, fips_instant now,
    uint64_t interval, double backoff, fips_fmp_action_list *a)
{
    if (count > a->capacity - a->len) return FIPS_ERR_RANGE;
    for (size_t i = 0; i < count; ++i) {
        uint32_t exponent = s[i].resend_count == UINT32_MAX ? UINT32_MAX : s[i].resend_count + 1;
        double delay = (double)interval * fips_powi(backoff, exponent);
        /* Rust's saturating float cast, including NaN, before host deadline. */
        uint64_t ms = !(delay > 0) ? 0 : delay >= 0x1p64 ? UINT64_MAX : (uint64_t)delay;
        fips_instant next = fips_deadline(now, fips_duration_from_millis(ms));
        a->items[a->len++] = (fips_fmp_conn_action){.kind = FIPS_FMP_ResendMsg1,
            .link = s[i].link, .bytes = s[i].msg1, .next_resend_at = next};
    }
    return FIPS_OK;
}
fips_status fips_fmp_receive(fips_link_id link, const uint8_t *frame, size_t len, fips_fmp_action_list *a)
{
    if (a->len == a->capacity) return FIPS_ERR_RANGE;
    fips_fmp_conn_action action = {.link = link};
    if (fips_fmp_validate_frame(frame, len, &action.error) != FIPS_OK) {
        action.kind = FIPS_FMP_DropFrame;
    } else {
        action.bytes = (fips_fmp_bytes){frame, len};
        switch (frame[0]) {
        case FIPS_FMP_PHASE_MSG1: action.kind = FIPS_FMP_ReceiveMsg1; break;
        case FIPS_FMP_PHASE_MSG2: action.kind = FIPS_FMP_ReceiveMsg2; break;
        default: action.kind = FIPS_FMP_ReceiveEncrypted; break;
        }
    }
    a->items[a->len++] = action;
    return FIPS_OK;
}

fips_status fips_fmp_receive_plaintext(fips_link_id link, const uint8_t *plaintext, size_t len, fips_fmp_action_list *a)
{
    if (a->len == a->capacity) return FIPS_ERR_RANGE;
    fips_fmp_conn_action action = {.link = link};
    if (!fips_fmp_strip_inner_header(plaintext, len, &action.timestamp_ms, &action.bytes.data, &action.bytes.len)) {
        action.kind = FIPS_FMP_DropFrame;
        action.error = (fips_proto_error){.variant = FIPS_PROTO_MessageTooShort,
            .as.message_too_short = {FIPS_FRAMING_FMP_INNER_HEADER_SIZE, len}};
    } else {
        action.kind = FIPS_FMP_DeliverLinkMessage;
    }
    a->items[a->len++] = action;
    return FIPS_OK;
}
