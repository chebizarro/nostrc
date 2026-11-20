#include <fips/proto/link.h>
#include <string.h>

static fips_status invalid_type(uint8_t byte, fips_proto_error *error)
{
    if (error) {
        *error = (fips_proto_error){.variant = FIPS_PROTO_InvalidMessageType,
            .as.invalid_message_type = byte};
    }
    return FIPS_ERR_RANGE;
}

fips_status fips_link_message_type_from_u8(uint8_t byte, fips_link_message_type *out,
                                           fips_proto_error *error)
{
    switch (byte) {
    case FIPS_LINK_SessionDatagram: case FIPS_LINK_SenderReport: case FIPS_LINK_ReceiverReport:
    case FIPS_LINK_TreeAnnounce: case FIPS_LINK_FilterAnnounce:
    case FIPS_LINK_LookupRequest: case FIPS_LINK_LookupResponse:
    case FIPS_LINK_Disconnect: case FIPS_LINK_Heartbeat: case FIPS_LINK_PathProbe:
    case FIPS_LINK_PathAck: case FIPS_LINK_PathClose:
        *out = (fips_link_message_type)byte;
        return FIPS_OK;
    default:
        return invalid_type(byte, error);
    }
}

fips_status fips_session_message_type_from_u8(uint8_t byte, fips_session_message_type *out,
                                              fips_proto_error *error)
{
    switch (byte) {
    case FIPS_SESSION_DataPacket: case FIPS_SESSION_SenderReport: case FIPS_SESSION_ReceiverReport:
    case FIPS_SESSION_PathMtuNotification: case FIPS_SESSION_CoordsWarmup:
        *out = (fips_session_message_type)byte;
        return FIPS_OK;
    default:
        return invalid_type(byte, error);
    }
}

const char *fips_link_message_type_name(fips_link_message_type type)
{
    switch (type) {
    case FIPS_LINK_SessionDatagram: return "SessionDatagram";
    case FIPS_LINK_SenderReport: return "SenderReport";
    case FIPS_LINK_ReceiverReport: return "ReceiverReport";
    case FIPS_LINK_TreeAnnounce: return "TreeAnnounce";
    case FIPS_LINK_FilterAnnounce: return "FilterAnnounce";
    case FIPS_LINK_LookupRequest: return "LookupRequest";
    case FIPS_LINK_LookupResponse: return "LookupResponse";
    case FIPS_LINK_Disconnect: return "Disconnect";
    case FIPS_LINK_Heartbeat: return "Heartbeat";
    case FIPS_LINK_PathProbe: return "PathProbe";
    case FIPS_LINK_PathAck: return "PathAck";
    case FIPS_LINK_PathClose: return "PathClose";
    }
    return "Unknown";
}

const char *fips_session_message_type_name(fips_session_message_type type)
{
    switch (type) {
    case FIPS_SESSION_DataPacket: return "DataPacket";
    case FIPS_SESSION_SenderReport: return "SenderReport";
    case FIPS_SESSION_ReceiverReport: return "ReceiverReport";
    case FIPS_SESSION_PathMtuNotification: return "PathMtuNotification";
    case FIPS_SESSION_CoordsWarmup: return "CoordsWarmup";
    }
    return "Unknown";
}

void fips_session_datagram_init(fips_session_datagram *d, fips_node_addr src_addr,
                                fips_node_addr dest_addr, const uint8_t *payload,
                                size_t payload_len)
{
    *d = (fips_session_datagram){src_addr, dest_addr, FIPS_SESSION_DATAGRAM_DEFAULT_TTL,
        FIPS_SESSION_DATAGRAM_DEFAULT_PATH_MTU, payload, payload_len};
}

bool fips_ttl_after_hop(uint8_t ttl, uint8_t *out)
{
    uint8_t left = ttl ? (uint8_t)(ttl - 1) : 0; /* saturating_sub(1) */
    if (!left) return false;
    *out = left;
    return true;
}

bool fips_session_datagram_decrement_ttl(fips_session_datagram *d)
{
    d->ttl = d->ttl ? (uint8_t)(d->ttl - 1) : 0;
    return d->ttl > 0;
}

bool fips_session_datagram_can_forward(const fips_session_datagram *d)
{
    uint8_t left;
    return fips_ttl_after_hop(d->ttl, &left);
}

size_t fips_session_datagram_encoded_len(const fips_session_datagram *d)
{
    return d->payload_len > SIZE_MAX - FIPS_SESSION_DATAGRAM_HEADER_SIZE
        ? SIZE_MAX : FIPS_SESSION_DATAGRAM_HEADER_SIZE + d->payload_len;
}

fips_status fips_session_datagram_encode(const fips_session_datagram *d, fips_writer *w,
                                         fips_proto_error *error)
{
    size_t total = fips_session_datagram_encoded_len(d);
    if (total > fips_writer_remaining(w)) {
        if (error) {
            size_t len = fips_writer_len(w);
            *error = (fips_proto_error){.variant = FIPS_PROTO_MessageTooLong,
                .as.message_too_long = {w->capacity, total > SIZE_MAX - len ? SIZE_MAX : len + total}};
        }
        return FIPS_ERR_RANGE;
    }
    /* Capacity is reserved above, so no step below can fail or write partially. */
    fips_status status = fips_writer_write_u8(w, FIPS_LINK_SessionDatagram, NULL);
    if (status == FIPS_OK) status = fips_writer_write_u8(w, d->ttl, NULL);
    if (status == FIPS_OK) status = fips_writer_write_u16_le(w, d->path_mtu, NULL);
    if (status == FIPS_OK) status = fips_writer_write_bytes(w, d->src_addr.bytes, sizeof(d->src_addr.bytes), NULL);
    if (status == FIPS_OK) status = fips_writer_write_bytes(w, d->dest_addr.bytes, sizeof(d->dest_addr.bytes), NULL);
    if (status == FIPS_OK) status = fips_writer_write_bytes(w, d->payload, d->payload_len, NULL);
    return status;
}

fips_status fips_session_datagram_decode(fips_reader *r, fips_session_datagram *out,
                                         fips_proto_error *error)
{
    if (fips_reader_require(r, FIPS_SESSION_DATAGRAM_BODY_HEADER_SIZE, error) != FIPS_OK) {
        return FIPS_ERR_RANGE;
    }
    /* 35 bytes are present, so the fixed reads below cannot fail; the payload is
     * whatever remains, borrowed from the input exactly as &payload[35..]. */
    fips_session_datagram d;
    fips_status status = fips_reader_read_u8(r, &d.ttl, NULL);
    if (status == FIPS_OK) status = fips_reader_read_u16_le(r, &d.path_mtu, NULL);
    if (status == FIPS_OK) status = fips_reader_read_array(r, d.src_addr.bytes, sizeof(d.src_addr.bytes), NULL);
    if (status == FIPS_OK) status = fips_reader_read_array(r, d.dest_addr.bytes, sizeof(d.dest_addr.bytes), NULL);
    if (status == FIPS_OK) {
        d.payload_len = fips_reader_remaining(r);
        status = fips_reader_read_bytes(r, d.payload_len, &d.payload, NULL);
    }
    if (status != FIPS_OK) return status;
    *out = d;
    return FIPS_OK;
}
