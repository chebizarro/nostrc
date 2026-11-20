#include <fips/proto/fsp/core.h>
#include <string.h>
static fips_status append(fips_fsp_action_list *a, const fips_fsp_action *items, size_t n)
{
    if (n > a->capacity - a->len) return FIPS_ERR_RANGE;
    if (n) memcpy(a->items + a->len, items, n * sizeof(*items));
    a->len += n; return FIPS_OK;
}
fips_status fips_fsp_plan_coords_required_lookup(fips_node_addr dest, bool cached, fips_fsp_action_list *a)
{
    const fips_fsp_action action = {.kind = FIPS_FSP_InitiateLookup, .dest = dest};
    return append(a, &action, cached ? 1 : 0);
}
fips_status fips_fsp_plan_path_broken(fips_node_addr dest, bool verified, fips_fsp_quorum_verdict q, fips_fsp_action_list *a)
{
    const fips_fsp_action items[] = {
        {.kind = verified ? FIPS_FSP_DemoteCoords : FIPS_FSP_InvalidateCoords, .dest = dest},
        {.kind = FIPS_FSP_InitiateLookup, .dest = dest}
    };
    bool change = !verified || q.kind == FIPS_FSP_Reached;
    return append(a, items + (change ? 0 : 1), change ? 2 : 1);
}
bool fips_fsp_should_apply_path_mtu(bool has, uint16_t existing, uint16_t candidate)
{ return !has || candidate < existing; }
fips_status fips_fsp_plan_path_mtu_tighten(fips_ipv6_address addr, bool has, uint16_t existing, uint16_t candidate, fips_fsp_action_list *a)
{
    const fips_fsp_action action = {.kind = FIPS_FSP_TightenPathMtuLookup, .fips_addr = addr, .mtu = candidate};
    return append(a, &action, fips_fsp_should_apply_path_mtu(has, existing, candidate) ? 1 : 0);
}
fips_status fips_fsp_push_bounded_pending(fips_fsp_pending_queue *q, fips_fsp_bytes packet)
{
    if (!q->capacity) return FIPS_ERR_RANGE;
    if (q->len == q->capacity) {
        if (q->len > 1) memmove(q->items, q->items + 1, (q->len - 1) * sizeof(*q->items));
        --q->len;
    }
    q->items[q->len++] = packet; return FIPS_OK;
}
static fips_status drop(fips_node_addr src, fips_proto_error error, fips_fsp_action_list *a)
{
    const fips_fsp_action action = {.kind = FIPS_FSP_DropFrame, .dest = src, .error = error};
    return append(a, &action, 1);
}
static fips_proto_error short_frame(size_t expected, size_t got)
{ return (fips_proto_error){.variant = FIPS_PROTO_MessageTooShort, .as.message_too_short = {expected, got}}; }
fips_status fips_fsp_receive(fips_node_addr src, const uint8_t *frame, size_t len, fips_fsp_action_list *a)
{
    fips_fsp_common_prefix p;
    fips_proto_error e;
    fips_fsp_action action = {.dest = src, .bytes = {frame, len}};
    if (!fips_fsp_common_prefix_parse(frame, len, &p)) return drop(src, short_frame(FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE, len), a);
    if (p.version != FIPS_FSP_VERSION) return drop(src, (fips_proto_error){.variant = FIPS_PROTO_UnsupportedVersion, .as.unsupported_version = p.version}, a);
    if (fips_fsp_check_prefix(&p, len - FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE, &e) != FIPS_OK) return drop(src, e, a);
    fips_reader r;
    fips_reader_init(&r, frame + FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE, len - FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE);
    switch (p.phase) {
    case FIPS_FSP_PHASE_MSG1: {
        fips_fsp_session_setup m;
        if (fips_fsp_session_setup_decode_strict(&r, &m, &e) != FIPS_OK) return drop(src, e, a);
        action.kind = FIPS_FSP_ReceiveSetup; break;
    }
    case FIPS_FSP_PHASE_MSG2: {
        fips_fsp_session_ack m;
        if (fips_fsp_session_ack_decode_strict(&r, &m, &e) != FIPS_OK) return drop(src, e, a);
        action.kind = FIPS_FSP_ReceiveAck; break;
    }
    case FIPS_FSP_PHASE_MSG3: {
        fips_fsp_session_msg3 m;
        if (fips_fsp_session_msg3_decode_strict(&r, &m, &e) != FIPS_OK) return drop(src, e, a);
        action.kind = FIPS_FSP_ReceiveMsg3; break;
    }
    case FIPS_FSP_PHASE_ESTABLISHED: {
        if (fips_fsp_common_prefix_is_unencrypted(&p)) { action.kind = FIPS_FSP_ReceiveRoutingSignal; break; }
        fips_fsp_encrypted_header h;
        if (!fips_fsp_encrypted_header_parse(frame, len, &h)) return drop(src, short_frame(FIPS_FRAMING_FSP_ENCRYPTED_MIN_SIZE, len), a);
        if (fips_fsp_check_header(&h, &e) != FIPS_OK) return drop(src, e, a);
        fips_reader_init(&r, frame + FIPS_FRAMING_FSP_HEADER_SIZE, len - FIPS_FRAMING_FSP_HEADER_SIZE);
        if (fips_fsp_encrypted_header_has_coords(&h)) {
            fips_fsp_coords s, d;
            if (fips_fsp_parse_encrypted_coords(&r, &s, &d, &e) != FIPS_OK) return drop(src, e, a);
        }
        if (fips_fsp_check_sealed(&h, fips_reader_remaining(&r), &e) != FIPS_OK) return drop(src, e, a);
        action.kind = FIPS_FSP_ReceiveEncrypted; break;
    }
    default: return drop(src, (fips_proto_error){.variant = FIPS_PROTO_InvalidMessageType, .as.invalid_message_type = p.phase}, a);
    }
    return append(a, &action, 1);
}
fips_status fips_fsp_receive_plaintext(fips_node_addr src, const uint8_t *plaintext, size_t len, fips_fsp_action_list *a)
{
    fips_fsp_action action = {.kind = FIPS_FSP_DeliverSessionMessage, .dest = src};
    fips_proto_error e;
    if (!fips_fsp_strip_inner_header(plaintext, len, &action.inner, &action.bytes)) return drop(src, short_frame(FIPS_FRAMING_FSP_INNER_HEADER_SIZE, len), a);
    if (fips_fsp_check_inner(action.inner.inner_flags, &e) != FIPS_OK) return drop(src, e, a);
    fips_session_message_type type;
    if (fips_session_message_type_from_u8(action.inner.msg_type, &type, &e) != FIPS_OK) return drop(src, e, a);
    return append(a, &action, 1);
}
