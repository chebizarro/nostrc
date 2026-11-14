#include "proto_fmp.h"
#include <fips/proto/fmp/wire.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

result check_proto_disconnect(diff_context *ctx, const proto_case *c)
{
    const diff_value *v = c->record;
    diff_bytes bytes = BYTES(v, "bytes");
    fips_fmp_disconnect d = {FIPS_FMP_Other}, decoded;
    bool known = false;
    for (unsigned i = 0; i <= 255; ++i) {
        fips_fmp_disconnect_reason reason;
        if (fips_fmp_disconnect_reason_from_byte((uint8_t)i, &reason) &&
            !strcmp(fips_fmp_disconnect_reason_name(reason), TEXT(FIELD(v, "input"), "reason"))) {
            d.reason = reason; known = true; break;
        }
    }
    if (!known || UINT(v, "decode_offset") != 1 || bytes.len != 2)
        return diff_error(ctx, "disconnect reason/offset/length"), FAILED;
    uint8_t encoded[2];
    fips_writer w;
    fips_writer_init(&w, encoded, sizeof(encoded));
    if (fips_fmp_disconnect_encode(&d, &w, NULL) != FIPS_OK ||
        !diff_compare(ctx->id, bytes.data, bytes.len, encoded, w.pos)) return FAILED;
    fips_reader r;
    fips_reader_init(&r, bytes.data + 1, bytes.len - 1);
    if (fips_fmp_disconnect_decode(&r, &decoded, NULL) != FIPS_OK || decoded.reason != d.reason)
        return diff_error(ctx, "disconnect decode"), FAILED;
    char debug[96];
    snprintf(debug, sizeof(debug), "Disconnect { reason: %s }", fips_fmp_disconnect_reason_name(decoded.reason));
    if (strcmp(debug, TEXT(v, "decoded_debug"))) return diff_error(ctx, "disconnect debug"), FAILED;
    fips_proto_error error;
    fips_reader_init(&r, NULL, 0);
    const diff_value *expected = FIELD(v, "empty_input_error");
    if (fips_fmp_disconnect_decode(&r, &decoded, &error) != FIPS_ERR_RANGE ||
        strcmp(fips_proto_error_name(error.variant), TEXT(expected, "variant")) ||
        error.variant != FIPS_PROTO_MessageTooShort ||
        error.as.message_too_short.expected != UINT(expected, "expected") ||
        error.as.message_too_short.got != UINT(expected, "got"))
        return diff_error(ctx, "disconnect empty error"), FAILED;
    return CHECKED;
}

static result handshake(diff_context *ctx, const proto_case *c, unsigned number)
{
    /* Independent Noise fixture supplies opaque payload, NOT bytes cut out of
     * the envelope being checked. This does not claim a C Noise handshake. */
    diff_context nested = *ctx;
    diff_value *noise = diff_load(&nested, "noise");
    if (!noise) return diff_error(ctx, "Noise input: %s", nested.error), FAILED;
    const diff_value *patterns = FIELD(noise, "patterns"), *ik = NULL;
    for (size_t i = 0; i < COUNT(patterns); ++i)
        if (!strcmp(TEXT(ITEM(patterns, i), "pattern"), "IK")) ik = ITEM(patterns, i);
    result result_value = FAILED;
    if (!ik || COUNT(FIELD(ik, "messages")) < number) {
        diff_error(ctx, "missing IK input"); goto done;
    }
    diff_bytes payload = BYTES(ITEM(FIELD(ik, "messages"), number - 1), "bytes");
    diff_bytes expected = BYTES(c->record, "bytes");
    const char *name = number == 1 ? "ik-msg-1" : "ik-msg-2";
    if (strcmp(TEXT(c->record, "noise_case"), name) || UINT(c->record, "sender_idx") > UINT32_MAX) {
        diff_error(ctx, "Noise reference/index"); goto done;
    }
    uint8_t encoded[FIPS_FRAMING_FMP_MSG1_WIRE_SIZE];
    fips_writer w;
    fips_writer_init(&w, encoded, sizeof(encoded));
    fips_session_index sender = {(uint32_t)UINT(c->record, "sender_idx")};
    size_t offset;
    if (number == 1) {
        fips_fmp_msg1_header header;
        if (FIELD(c->record, "receiver_idx")->type != D_NULL ||
            fips_fmp_build_msg1(sender, payload.data, payload.len, &w, NULL) != FIPS_OK ||
            !fips_fmp_msg1_header_parse(expected.data, expected.len, &header) || header.sender_idx.value != sender.value) {
            diff_error(ctx, "msg1 encode/parse"); goto done;
        }
        offset = header.noise_msg1_offset;
    } else {
        fips_fmp_msg2_header header;
        uint64_t receiver = UINT(c->record, "receiver_idx");
        if (receiver > UINT32_MAX ||
            fips_fmp_build_msg2(sender, (fips_session_index){(uint32_t)receiver}, payload.data, payload.len, &w, NULL) != FIPS_OK ||
            !fips_fmp_msg2_header_parse(expected.data, expected.len, &header) ||
            header.sender_idx.value != sender.value || header.receiver_idx.value != receiver) {
            diff_error(ctx, "msg2 encode/parse"); goto done;
        }
        offset = header.noise_msg2_offset;
    }
    if (fips_fmp_validate_frame(expected.data, expected.len, NULL) != FIPS_OK ||
        !diff_compare(ctx->id, expected.data, expected.len, encoded, w.pos) ||
        !diff_compare(ctx->id, payload.data, payload.len, expected.data + offset, expected.len - offset)) goto done;
    result_value = CHECKED;
done:
    diff_free(noise);
    return result_value;
}
result check_proto_fmp_msg_1(diff_context *ctx, const proto_case *c) { return handshake(ctx, c, 1); }
result check_proto_fmp_msg_2(diff_context *ctx, const proto_case *c) { return handshake(ctx, c, 2); }

result check_proto_fmp_encrypted(diff_context *ctx, const proto_case *c)
{
    const diff_value *v = c->record;
    diff_bytes expected = BYTES(v, "bytes"), inner = BYTES(v, "inner_hex"), expected_header = BYTES(v, "header_hex");
    fips_fmp_encrypted_header parsed;
    if (inner.len > UINT16_MAX || UINT(v, "receiver_idx") > UINT32_MAX || UINT(v, "flags") > UINT8_MAX ||
        fips_fmp_validate_frame(expected.data, expected.len, NULL) != FIPS_OK ||
        !fips_fmp_encrypted_header_parse(expected.data, expected.len, &parsed) ||
        parsed.receiver_idx.value != UINT(v, "receiver_idx") || parsed.counter != UINT(v, "counter") ||
        parsed.flags != UINT(v, "flags") || parsed.payload_len != inner.len)
        return diff_error(ctx, "encrypted header/length"), FAILED;
    uint8_t header[FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE];
    fips_fmp_build_established_header((fips_session_index){(uint32_t)UINT(v, "receiver_idx")},
        UINT(v, "counter"), (uint8_t)UINT(v, "flags"), (uint16_t)inner.len, header);
    if (!diff_compare(ctx->id, expected_header.data, expected_header.len, header, sizeof(header)) ||
        memcmp(parsed.header_bytes, header, sizeof(header))) return FAILED;
    uint8_t *encoded = malloc(expected.len);
    if (!encoded) return diff_error(ctx, "allocation"), FAILED;
    fips_writer w;
    fips_writer_init(&w, encoded, expected.len);
    /* Ciphertext is opaque here: session key derivation belongs to Noise. */
    bool ok = fips_fmp_build_encrypted(header, expected.data + sizeof(header), expected.len - sizeof(header), &w, NULL) == FIPS_OK;
    if (ok) ok = diff_compare(ctx->id, expected.data, expected.len, encoded, w.pos);
    free(encoded);
    diff_context nested = *ctx;
    diff_value *proto = diff_load(&nested, "proto");
    if (!proto) return diff_error(ctx, "inner input: %s", nested.error), FAILED;
    const diff_value *cases = FIELD(proto, "wire_cases");
    diff_value *datagram = NULL;
    for (size_t i = 0; i < COUNT(cases); ++i)
        if (!strcmp(TEXT(ITEM(cases, i), "name"), "session-datagram")) datagram = ITEM(cases, i);
    if (!datagram) { diff_free(proto); return diff_error(ctx, "missing inner datagram"), FAILED; }
    proto_case dc = {"session-datagram", datagram, c->common_inputs, c->constants};
    ok &= check_proto_session_datagram(ctx, &dc) == CHECKED;
    diff_bytes body = BYTES(datagram, "bytes");
    encoded = malloc(body.len + 4);
    if (!encoded) { diff_free(proto); return diff_error(ctx, "allocation"), FAILED; }
    fips_writer_init(&w, encoded, body.len + 4);
    ok &= fips_fmp_prepend_inner_header(UINT32_C(0x89abcdef), body.data, body.len, &w, NULL) == FIPS_OK;
    ok &= diff_compare(ctx->id, inner.data, inner.len, encoded, w.pos);
    const uint8_t *payload;
    size_t payload_len;
    uint32_t ts;
    if (!fips_fmp_strip_inner_header(inner.data, inner.len, &ts, &payload, &payload_len) || ts != UINT32_C(0x89abcdef)) ok = false;
    else ok &= diff_compare(ctx->id, body.data, body.len, payload, payload_len);
    free(encoded); diff_free(proto);
    if (!ok) diff_error(ctx, "encrypted envelope/inner framing mismatch (opaque ciphertext)");
    return ok ? CHECKED : FAILED;
}
