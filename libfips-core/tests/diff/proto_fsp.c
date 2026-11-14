#include "proto_fsp.h"
#include "../fakes/host.h"
#include <fips/identity/local.h>
#include <fips/proto/fsp/mod.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool seeded_addr(uint64_t seed, fips_node_addr *out)
{
    fips_seeded_entropy seeded = {seed ^ UINT64_C(0x6b65797061697273)};
    fips_entropy entropy = fips_seeded_entropy_adapter(&seeded);
    uint8_t secret[32];
    entropy.fill(entropy.ctx, secret, sizeof(secret));
    fips_fixed_entropy fixed = {0x5a};
    fips_entropy aux = fips_fixed_entropy_adapter(&fixed);
    fips_secp *secp = NULL;
    fips_identity *identity = NULL;
    bool ok = fips_identity_init_secp(aux, &secp) == FIPS_IDENTITY_OK &&
        fips_identity_from_secret_bytes(secp, aux, secret, &identity) == FIPS_IDENTITY_OK;
    memset(secret, 0, sizeof(secret));
    if (ok) *out = fips_identity_node_addr(identity);
    fips_identity_destroy(identity); fips_identity_destroy_secp(secp);
    return ok;
}
static bool coords_equal(fips_fsp_coords a, fips_fsp_coords b)
{ return a.count == b.count && !memcmp(a.addrs, b.addrs, a.count * 16); }
/* This is Rust Debug formatting of the decoded C value, not a copy of the
 * expected string. Bounded because the replay fixture has at most two addrs. */
static void coord_debug(fips_fsp_coords c, char out[128])
{
    size_t at = (size_t)snprintf(out, 128, "TreeCoordinate(depth=%zu, path=[", c.count - 1);
    for (size_t i = 0; i < c.count; ++i)
        at += (size_t)snprintf(out + at, 128 - at, "%s%02x%02x", i ? " → " : "", c.addrs[16 * i], c.addrs[16 * i + 1]);
    snprintf(out + at, 128 - at, "])");
}
static result handshake(diff_context *ctx, const proto_case *c, unsigned phase)
{
    const char *names[] = {"fsp-setup", "fsp-ack", "fsp-msg3"};
    if (phase < 1 || phase > 3 || strcmp(c->name, names[phase - 1])) return diff_error(ctx, "wrong FSP case"), FAILED;
    const diff_value *v = c->record, *input = FIELD(v, "input");
    diff_bytes wire = BYTES(v, "bytes");
    if (UINT(v, "decode_offset") != FIPS_FRAMING_FSP_COMMON_PREFIX_SIZE || wire.len < 4) return diff_error(ctx, "FSP decode offset"), FAILED;
    diff_context nested = *ctx;
    diff_value *noise = diff_load(&nested, "noise");
    if (!noise) return diff_error(ctx, "Noise fixture: %s", nested.error), FAILED;
    const diff_value *patterns = FIELD(noise, "patterns"), *xk = NULL;
    for (size_t i = 0; i < COUNT(patterns); ++i)
        if (!strcmp(TEXT(ITEM(patterns, i), "pattern"), "XK")) xk = ITEM(patterns, i);
    bool ok = false;
    uint8_t *encoded = NULL;
    char noise_name[24];
    snprintf(noise_name, sizeof(noise_name), "xk-msg-%u", phase);
    if (!xk || COUNT(FIELD(xk, "messages")) < phase || strcmp(TEXT(input, "noise_case"), noise_name)) goto done;
    /* Handshake bytes come from the independent XK fixture, never from the
     * session envelope under test. The XK implementation is a sibling task. */
    diff_bytes hs = BYTES(ITEM(FIELD(xk, "messages"), phase - 1), "bytes");
    fips_fsp_bytes payload = {hs.data, hs.len}, decoded_hs;
    uint8_t decoded_flags;
    fips_fsp_coords src = {0}, dest = {0}, decoded_src = {0}, decoded_dest = {0};
    uint8_t src_bytes[16], dst_bytes[32];
    if (phase != 3) {
        const diff_value *seeds = FIELD(c->common_inputs, "identity_seeds");
        fips_node_addr a, b;
        if (COUNT(seeds) != 2 || !seeded_addr(ITEM(seeds, 0)->as.number, &a) || !seeded_addr(ITEM(seeds, 1)->as.number, &b)) goto done;
        memcpy(src_bytes, a.bytes, 16); memcpy(dst_bytes, b.bytes, 16); memcpy(dst_bytes + 16, a.bytes, 16);
        if (strcmp(TEXT(input, "src_coord"), phase == 1 ? "src" : "dst") ||
            strcmp(TEXT(input, "dest_coord"), phase == 1 ? "dst" : "src")) goto done;
        src = (fips_fsp_coords){phase == 1 ? src_bytes : dst_bytes, phase == 1 ? 1 : 2};
        dest = (fips_fsp_coords){phase == 1 ? dst_bytes : src_bytes, phase == 1 ? 2 : 1};
    }
    encoded = malloc(wire.len);
    if (!encoded) goto done;
    fips_writer w;
    fips_reader r;
    fips_writer_init(&w, encoded, wire.len);
    fips_reader_init(&r, wire.data + 4, wire.len - 4);
    fips_proto_error error = {.variant = FIPS_PROTO_InvalidSignature};
    fips_status status;
    if (phase == 1) {
        fips_fsp_session_setup m = {src, dest, {0}, payload}, decoded;
        status = fips_fsp_session_setup_encode(&m, &w, &error);
        if (fips_fsp_session_setup_decode_strict(&r, &decoded, &error) != FIPS_OK) goto done;
        decoded_hs = decoded.handshake_payload; decoded_flags = decoded.flags.bits;
        decoded_src = decoded.src_coords; decoded_dest = decoded.dest_coords;
    } else if (phase == 2) {
        fips_fsp_session_ack m = {src, dest, 0, payload}, decoded;
        status = fips_fsp_session_ack_encode(&m, &w, &error);
        if (fips_fsp_session_ack_decode_strict(&r, &decoded, &error) != FIPS_OK) goto done;
        decoded_hs = decoded.handshake_payload; decoded_flags = decoded.flags;
        decoded_src = decoded.src_coords; decoded_dest = decoded.dest_coords;
    } else {
        fips_fsp_session_msg3 m = {0, payload}, decoded;
        status = fips_fsp_session_msg3_encode(&m, &w, &error);
        if (fips_fsp_session_msg3_decode_strict(&r, &decoded, &error) != FIPS_OK) goto done;
        decoded_hs = decoded.handshake_payload; decoded_flags = decoded.flags;
    }
    if (status != FIPS_OK || r.pos != r.len || decoded_flags || error.variant != FIPS_PROTO_InvalidSignature ||
        !diff_compare(ctx->id, wire.data, wire.len, encoded, w.pos) ||
        !diff_compare(ctx->id, hs.data, hs.len, decoded_hs.data, decoded_hs.len)) goto done;
    if (phase != 3 && (!coords_equal(src, decoded_src) || !coords_equal(dest, decoded_dest))) goto done;
    fips_fsp_common_prefix prefix;
    if (!fips_fsp_common_prefix_parse(wire.data, wire.len, &prefix) || prefix.version || prefix.phase != phase ||
        fips_fsp_check_prefix(&prefix, wire.len - 4, &error) != FIPS_OK) goto done;
    char debug[4096], sd[128], dd[128];
    size_t at;
    if (phase != 3) {
        coord_debug(decoded_src, sd); coord_debug(decoded_dest, dd);
        at = (size_t)snprintf(debug, sizeof(debug), "%s { src_coords: %s, dest_coords: %s, flags: %s, handshake_payload: [",
            phase == 1 ? "SessionSetup" : "SessionAck", sd, dd, phase == 1 ? "SessionFlags(0)" : "0");
    } else at = (size_t)snprintf(debug, sizeof(debug), "SessionMsg3 { flags: 0, handshake_payload: [");
    for (size_t i = 0; i < decoded_hs.len; ++i) {
        if (at + 10 >= sizeof(debug)) goto done;
        at += (size_t)snprintf(debug + at, sizeof(debug) - at, "%s%u", i ? ", " : "", decoded_hs.data[i]);
    }
    snprintf(debug + at, sizeof(debug) - at, "] }");
    if (strcmp(debug, TEXT(v, "decoded_debug"))) goto done;
    fips_reader_init(&r, NULL, 0);
    if (phase == 1) { fips_fsp_session_setup d; status = fips_fsp_session_setup_decode(&r, &d, &error); }
    else if (phase == 2) { fips_fsp_session_ack d; status = fips_fsp_session_ack_decode(&r, &d, &error); }
    else { fips_fsp_session_msg3 d; status = fips_fsp_session_msg3_decode(&r, &d, &error); }
    const diff_value *expected = FIELD(v, "empty_input_error");
    if (status != FIPS_ERR_RANGE || r.pos || error.variant != FIPS_PROTO_MessageTooShort ||
        strcmp(fips_proto_error_name(error.variant), TEXT(expected, "variant")) ||
        error.as.message_too_short.expected != UINT(expected, "expected") || error.as.message_too_short.got != UINT(expected, "got")) goto done;
    ok = true;
done:
    free(encoded); diff_free(noise);
    if (!ok) diff_error(ctx, "FSP encode/fields/decode/Debug/error mismatch (opaque XK payload)");
    return ok ? CHECKED : FAILED;
}
result check_proto_fsp_setup(diff_context *ctx, const proto_case *c) { return handshake(ctx, c, 1); }
result check_proto_fsp_ack(diff_context *ctx, const proto_case *c) { return handshake(ctx, c, 2); }
result check_proto_fsp_msg3(diff_context *ctx, const proto_case *c) { return handshake(ctx, c, 3); }
result check_proto_fsp_encrypted(diff_context *ctx, const proto_case *c)
{
    if (strcmp(c->name, "fsp-encrypted")) return diff_error(ctx, "wrong FSP encrypted case"), FAILED;
    const diff_value *v = c->record;
    diff_bytes wire = BYTES(v, "bytes"), inner = BYTES(v, "inner_hex"), expected_header = BYTES(v, "header_hex");
    fips_fsp_encrypted_header parsed;
    if (UINT(v, "flags") > UINT8_MAX || inner.len > UINT16_MAX ||
        !fips_fsp_encrypted_header_parse(wire.data, wire.len, &parsed) ||
        fips_fsp_check_header(&parsed, NULL) != FIPS_OK ||
        fips_fsp_check_sealed(&parsed, wire.len - FIPS_FRAMING_FSP_HEADER_SIZE, NULL) != FIPS_OK ||
        parsed.counter != UINT(v, "counter") || parsed.flags != UINT(v, "flags") || parsed.payload_len != inner.len)
        return diff_error(ctx, "FSP encrypted header/length mismatch"), FAILED;
    uint8_t header[FIPS_FRAMING_FSP_HEADER_SIZE], rebuilt_inner[11];
    fips_fsp_build_header(UINT(v, "counter"), (uint8_t)UINT(v, "flags"), (uint16_t)inner.len, header);
    if (!diff_compare(ctx->id, expected_header.data, expected_header.len, header, sizeof(header)) ||
        memcmp(header, parsed.header_bytes, sizeof(header))) return FAILED;
    uint8_t *encoded = malloc(wire.len);
    if (!encoded) return diff_error(ctx, "allocation"), FAILED;
    fips_writer w;
    fips_writer_init(&w, encoded, wire.len);
    /* The exporter does not expose transport keys. Compare assembly, NOT AEAD
     * or session/replay parity, using opaque ciphertext and independently built
     * inner/header bytes from proto.rs's recorded construction. */
    bool ok = fips_fsp_build_encrypted(header, wire.data + sizeof(header), wire.len - sizeof(header), &w, NULL) == FIPS_OK &&
        diff_compare(ctx->id, wire.data, wire.len, encoded, w.pos);
    free(encoded);
    const uint8_t payload[] = {0, 1, 2, 3, 4};
    fips_writer_init(&w, rebuilt_inner, sizeof(rebuilt_inner));
    ok = ok && fips_fsp_prepend_inner_header(UINT32_C(0x12345678), FIPS_SESSION_DataPacket, FIPS_FSP_INNER_FLAG_SP,
        payload, sizeof(payload), &w, NULL) == FIPS_OK && diff_compare(ctx->id, inner.data, inner.len, rebuilt_inner, w.pos);
    fips_fsp_inner_header ih;
    fips_fsp_bytes rest;
    ok = ok && fips_fsp_strip_inner_header(inner.data, inner.len, &ih, &rest) &&
        ih.timestamp_ms == UINT32_C(0x12345678) && ih.msg_type == FIPS_SESSION_DataPacket && ih.inner_flags == FIPS_FSP_INNER_FLAG_SP &&
        fips_fsp_check_inner(ih.inner_flags, NULL) == FIPS_OK && diff_compare(ctx->id, payload, sizeof(payload), rest.data, rest.len);
    return ok ? CHECKED : (diff_error(ctx, "FSP encrypted assembly/inner mismatch"), FAILED);
}
