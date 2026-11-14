#include <fips/proto/fsp/mod.h>
#include <stdlib.h>
#include <string.h>
static void require(bool ok) { if (!ok) abort(); }
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > UINT16_MAX + (size_t)FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE) return 0;
    fips_fsp_action output[2];
    fips_fsp_action_list actions = {output, 0, 2};
    fips_node_addr src = {{0}};
    require(fips_fsp_receive(src, data, size, &actions) == FIPS_OK && actions.len == 1);
    actions.len = 0;
    require(fips_fsp_receive_plaintext(src, data, size, &actions) == FIPS_OK && actions.len == 1);
    fips_fsp_common_prefix prefix;
    fips_fsp_common_prefix_parse(data, size, &prefix);
    fips_fsp_encrypted_header h;
    if (fips_fsp_encrypted_header_parse(data, size, &h)) {
        uint8_t header[FIPS_FRAMING_FSP_HEADER_SIZE];
        fips_fsp_build_header(h.counter, h.flags, h.payload_len, header);
        require(!memcmp(header, data, sizeof(header)));
    }
    fips_reader r;
    fips_reader_init(&r, data, size);
    fips_fsp_coords s, d;
    fips_fsp_parse_encrypted_coords(&r, &s, &d, NULL);
    fips_reader_init(&r, data, size);
    fips_fsp_port_header ports;
    if (fips_fsp_port_header_decode(&r, &ports, NULL) == FIPS_OK) {
        uint8_t encoded[FIPS_FRAMING_FSP_PORT_HEADER_SIZE]; fips_writer w;
        fips_writer_init(&w, encoded, sizeof(encoded));
        require(fips_fsp_port_header_encode(ports, &w, NULL) == FIPS_OK && r.pos == w.pos && !memcmp(encoded, data, w.pos));
    }
    uint8_t *encoded = malloc(size + 4);
    if (!encoded) return 0;
    for (unsigned phase = 1; phase <= 3; ++phase) {
        fips_writer w; fips_writer_init(&w, encoded, size + 4);
        fips_reader_init(&r, data, size);
        fips_status read, write = FIPS_ERR_RANGE;
        if (phase == 1) {
            fips_fsp_session_setup m;
            read = fips_fsp_session_setup_decode(&r, &m, NULL);
            if (read == FIPS_OK) write = fips_fsp_session_setup_encode(&m, &w, NULL);
        } else if (phase == 2) {
            fips_fsp_session_ack m;
            read = fips_fsp_session_ack_decode(&r, &m, NULL);
            if (read == FIPS_OK) write = fips_fsp_session_ack_encode(&m, &w, NULL);
        } else {
            fips_fsp_session_msg3 m;
            read = fips_fsp_session_msg3_decode(&r, &m, NULL);
            if (read == FIPS_OK) write = fips_fsp_session_msg3_encode(&m, &w, NULL);
        }
        if (read != FIPS_OK) require(r.pos == 0);
        if (write == FIPS_OK) require(w.pos == r.pos + 4 && encoded[0] == phase && !memcmp(encoded + 4, data, r.pos));
        else require(w.pos == 0);
    }
    free(encoded);
    /* Stateful quorum, arbitrary peer/destination/time. Capacity refusal must
     * not alter the tracker; repeats cannot create a second vote. */
    fips_fsp_quorum_entry entries[4]; fips_fsp_link_quorum q;
    fips_fsp_link_quorum_init(&q, entries, 4);
    for (size_t i = 0; i + 2 < size; i += 3) {
        fips_node_addr dest = {{0}}, peer = {{0}}; dest.bytes[0] = data[i]; peer.bytes[0] = data[i + 1];
        fips_fsp_quorum_verdict verdict;
        uint64_t now = (uint64_t)data[i + 2] * 1000;
        fips_status status = fips_fsp_link_quorum_record(&q, dest, peer, now, &verdict);
        require(q.len <= 4);
        if (status == FIPS_OK) require(verdict.kind == FIPS_FSP_Reached || verdict.distinct == 1);
    }
    return 0;
}
