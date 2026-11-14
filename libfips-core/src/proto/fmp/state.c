#include <fips/proto/fmp/state.h>
#include <string.h>
void fips_fmp_connection_inbound(fips_fmp_connection_state *s, fips_link_id link, fips_fmp_leg_stamp at)
{
    *s = (fips_fmp_connection_state){.link_id = link, .direction = FIPS_LINK_INBOUND,
        .started_at = at.wall_ms, .last_activity = at.wall_ms, .active_at = at.interval};
}
void fips_fmp_connection_outbound(fips_fmp_connection_state *s, fips_link_id link, fips_peer_identity id, fips_fmp_leg_stamp at)
{
    fips_fmp_connection_inbound(s, link, at);
    s->direction = FIPS_LINK_OUTBOUND;
    s->has_expected_identity = true;
    s->expected_identity = id;
}
void fips_fmp_connection_inbound_with_transport(fips_fmp_connection_state *s, fips_link_id link,
    fips_transport_id transport, fips_transport_addr source, fips_fmp_leg_stamp at)
{
    fips_fmp_connection_inbound(s, link, at);
    s->has_transport_id = s->has_source_addr = true;
    s->transport_id = transport;
    s->source_addr = source;
}
bool fips_fmp_connection_set_our_index(fips_fmp_connection_state *s, fips_session_index index, fips_session_index *previous)
{
    bool had = s->has_our_index;
    if (had) *previous = s->our_index;
    s->has_our_index = true; s->our_index = index;
    return had;
}
bool fips_fmp_connection_take_our_index(fips_fmp_connection_state *s, fips_session_index *out)
{
    if (!s->has_our_index) return false;
    *out = s->our_index; s->has_our_index = false;
    return true;
}
fips_status fips_fmp_connection_set_handshake_msg1(fips_fmp_connection_state *s, const uint8_t *bytes, size_t len)
{
    if (len > sizeof(s->handshake_msg1)) return FIPS_ERR_RANGE;
    if (len) memmove(s->handshake_msg1, bytes, len);
    s->has_handshake_msg1 = true; s->handshake_msg1_len = len; s->resend_count = 0;
    return FIPS_OK;
}
fips_status fips_fmp_connection_set_handshake_msg2(fips_fmp_connection_state *s, const uint8_t *bytes, size_t len)
{
    if (len > sizeof(s->handshake_msg2)) return FIPS_ERR_RANGE;
    if (len) memmove(s->handshake_msg2, bytes, len);
    s->has_handshake_msg2 = true; s->handshake_msg2_len = len;
    return FIPS_OK;
}
void fips_fmp_connection_record_resend(fips_fmp_connection_state *s)
{ if (s->resend_count != UINT32_MAX) ++s->resend_count; }
void fips_fmp_connection_touch(fips_fmp_connection_state *s, fips_fmp_leg_stamp at)
{ s->last_activity = at.wall_ms; s->active_at = at.interval; }
uint64_t fips_fmp_connection_duration(const fips_fmp_connection_state *s, uint64_t wall_ms)
{ return wall_ms > s->started_at ? wall_ms - s->started_at : 0; }
uint64_t fips_fmp_connection_idle_time(const fips_fmp_connection_state *s, fips_instant now)
{ return fips_fmp_elapsed_ms(now, s->active_at); }
bool fips_fmp_connection_is_timed_out(const fips_fmp_connection_state *s, fips_instant now, uint64_t timeout_ms)
{ return fips_fmp_connection_idle_time(s, now) > timeout_ms; }
