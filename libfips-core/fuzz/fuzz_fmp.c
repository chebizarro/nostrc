#include <fips/proto/fmp/mod.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fips_fmp_common_prefix prefix;
    fips_fmp_msg1_header m1;
    fips_fmp_msg2_header m2;
    fips_fmp_encrypted_header encrypted;
    fips_proto_error error;
    fips_fmp_common_prefix_parse(data, size, &prefix);
    if (size) {
        fips_fmp_handshake_message_type phase; fips_fmp_disconnect_reason reason;
        if (fips_fmp_handshake_message_type_from_byte(data[0], &phase) && (uint8_t)phase != data[0]) abort();
        if (fips_fmp_disconnect_reason_from_byte(data[0], &reason) && (uint8_t)reason != data[0]) abort();
    }
    uint16_t expected;
    if (size) fips_fmp_expected_payload_len(data[0] & 15, size, &expected);
    uint8_t out[UINT16_MAX + FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE];
    fips_writer w;
    fips_writer_init(&w, out, sizeof(out));
    bool valid = fips_fmp_validate_frame(data, size, &error) == FIPS_OK;
    fips_fmp_conn_action action;
    fips_fmp_action_list actions = {&action, 0, 1};
    if (fips_fmp_receive((fips_link_id){1}, data, size, &actions) != FIPS_OK ||
        actions.len != 1 || (action.kind == FIPS_FMP_DropFrame) == valid) abort();
    if (fips_fmp_msg1_header_parse(data, size, &m1)) {
        if (fips_fmp_build_msg1(m1.sender_idx, data + m1.noise_msg1_offset,
            size - m1.noise_msg1_offset, &w, &error) != FIPS_OK) abort();
    } else if (fips_fmp_msg2_header_parse(data, size, &m2)) {
        if (fips_fmp_build_msg2(m2.sender_idx, m2.receiver_idx, data + m2.noise_msg2_offset,
            size - m2.noise_msg2_offset, &w, &error) != FIPS_OK) abort();
    } else if (fips_fmp_encrypted_header_parse(data, size, &encrypted) && valid) {
        uint8_t header[FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE];
        fips_fmp_build_established_header(encrypted.receiver_idx, encrypted.counter,
            encrypted.flags, encrypted.payload_len, header);
        if (memcmp(header, encrypted.header_bytes, sizeof(header))) abort();
        if (fips_fmp_build_encrypted(header, data + sizeof(header), size - sizeof(header), &w, &error) != FIPS_OK) abort();
    }
    if (valid && (w.pos != size || memcmp(data, out, size))) abort();
    actions.len = 0;
    if (fips_fmp_receive_plaintext((fips_link_id){1}, data, size, &actions) != FIPS_OK ||
        actions.len != 1 || (action.kind == FIPS_FMP_DropFrame) != (size < FIPS_FRAMING_FMP_INNER_HEADER_SIZE)) abort();
    uint32_t timestamp; const uint8_t *payload; size_t payload_len;
    if (fips_fmp_strip_inner_header(data, size, &timestamp, &payload, &payload_len)) {
        fips_writer_init(&w, out, sizeof(out));
        if (fips_fmp_prepend_inner_header(timestamp, payload, payload_len, &w, &error) == FIPS_OK &&
            (w.pos != size || memcmp(data, out, size))) abort();
    }
    fips_reader r; fips_reader_init(&r, data, size); fips_fmp_disconnect d;
    if (fips_fmp_disconnect_decode(&r, &d, &error) == FIPS_OK) {
        fips_writer_init(&w, out, 2);
        if (fips_fmp_disconnect_encode(&d, &w, &error) != FIPS_OK || w.pos != 2 ||
            out[0] != FIPS_LINK_Disconnect || out[1] != (uint8_t)d.reason) abort();
    }
    /* Arbitrary encoder input lengths and capacities, not just valid frames. */
    size_t cap = size ? data[0] : 0;
    fips_writer_init(&w, out, cap);
    fips_fmp_build_msg1((fips_session_index){0}, data, size, &w, &error);
    fips_writer_init(&w, out, cap);
    fips_fmp_build_msg2((fips_session_index){0}, (fips_session_index){0}, data, size, &w, &error);
    if (size >= FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE) {
        fips_writer_init(&w, out, cap);
        fips_fmp_build_encrypted(data, data + FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE,
            size - FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE, &w, &error);
    }
    return 0;
}
