#include "diff.h"
#include "../fakes/host.h"
#include "../fakes/rekey_profile.h"
#include <fips/proto/codec.h>
#include <fips/proto/coord.h>
#include <fips/identity/local.h>
#include <fips/noise/handshake.h>
#include <fips/noise/session.h>
#include <fips/identity/encoding.h>
#include <fips/identity/auth.h>
#include <fips/identity/address.h>
#include <sodium.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto_link.h" /* result, proto_case and the nostrc-82g9n.6 checks */
#include "proto_stp.h"
#include "proto_bloom.h"
#include "proto_fmp.h"
#include "proto_lookup.h"
#include "proto_fsp.h"
#include "proto_mmp.h"
#include "proto_path.h"
typedef struct counts { size_t checked, failed, skipped; } counts;
static void report(diff_context *ctx, counts *n, result r, const char *owner);

/* Family views borrow the schema-decoded C tree; all byte fields (including
 * inline hex), u64s, nullable states and arrays are already decoded losslessly.
 * Heterogeneous nested records use diff_value's tagged union, not json_t or
 * a parser for the Rust Debug strings. */
typedef struct entropy_case {
    uint64_t seed;
    diff_bytes stream, chunked, secret, compressed, xonly;
    diff_value *words, *lengths, *chunks;
} entropy_case;
typedef struct identity_case {
    uint64_t seed, timestamp, aux_entropy_byte;
    diff_bytes secret, xonly, node_addr, ipv6, challenge, signature, verified;
    const char *npub, *nsec, *ipv6_text, *wrong_challenge_error;
} identity_case;
typedef struct codec_case {
    const char *kind;
    bool has_value;
    uint64_t value;
    diff_bytes bytes;
    diff_value *truncations, *cursor;
} codec_case;
typedef struct noise_case {
    const char *pattern;
    uint64_t entropy_seed;
    diff_bytes epoch;
    diff_value *keypair_seeds, *messages, *steps, *post_handshake, *frames;
} noise_case;
typedef struct rekey_case {
    const char *profile, *initial_handshake, *params_debug, *clock;
    uint64_t entropy_seed;
    diff_bytes epoch;
    diff_value *keypair_seeds, *initial, *events, *final;
    const char *legacy_release;
    bool legacy_initiates;
} rekey_case;

static bool compare(diff_context *ctx, diff_bytes expected, const unsigned char *actual, size_t len)
{
    return diff_compare(ctx->id, expected.data, expected.len, actual, len);
}

static result check_entropy(diff_context *ctx, const entropy_case *c)
{
    bool ok = true;
    unsigned char *stream = calloc(c->stream.len ? c->stream.len : 1, 1);
    unsigned char *combined = calloc(c->chunked.len ? c->chunked.len : 1, 1);
    if (!stream || !combined) { free(stream); free(combined); diff_error(ctx, "allocation failed"); return FAILED; }
    fips_seeded_entropy state = {c->seed};
    fips_entropy entropy = fips_seeded_entropy_adapter(&state);
    entropy.fill(entropy.ctx, NULL, 0);
    entropy.fill(entropy.ctx, stream, c->stream.len);
    ok &= compare(ctx, c->stream, stream, c->stream.len);
    state.state = c->seed;
    if (COUNT(c->words) != c->stream.len / 8 || c->stream.len % 8) {
        diff_error(ctx, "word/stream length mismatch"); ok = false;
    }
    for (size_t i = 0; i < COUNT(c->words); ++i) {
        uint64_t actual = fips_splitmix64_next(&state), expected = ITEM(c->words, i)->as.number;
        unsigned char a[8], e[8];
        for (size_t j = 0; j < 8; ++j) { a[j] = (unsigned char)(actual >> (8 * j)); e[j] = (unsigned char)(expected >> (8 * j)); }
        ok &= diff_compare(ctx->id, e, 8, a, 8);
    }
    if (COUNT(c->lengths) != COUNT(c->chunks)) { diff_error(ctx, "chunk count mismatch"); ok = false; }
    else {
        size_t offset = 0;
        state.state = c->seed;
        for (size_t i = 0; i < COUNT(c->chunks); ++i) {
            diff_value *chunk = ITEM(c->chunks, i);
            uint64_t n = UINT(chunk, "length");
            diff_bytes bytes = BYTES(chunk, "bytes");
            if (n != ITEM(c->lengths, i)->as.number || n != bytes.len || n > c->chunked.len - offset) {
                diff_error(ctx, "chunk length mismatch at %zu", i); ok = false; break;
            }
            entropy.fill(entropy.ctx, NULL, 0);
            entropy.fill(entropy.ctx, combined + offset, bytes.len);
            ok &= compare(ctx, bytes, combined + offset, bytes.len);
            offset += bytes.len;
        }
        ok &= compare(ctx, c->chunked, combined, offset);
    }
    free(stream); free(combined);

    /* support.rs: seeded_keypair uses a fresh generator, XOR domain separation,
     * four LE words, then secp256k1 SecretKey::from_slice (no hash/retry). */
    unsigned char secret[32], compressed[33], xonly[32];
    state.state = c->seed ^ UINT64_C(0x6b65797061697273);
    entropy.fill(entropy.ctx, secret, sizeof(secret));
    ok &= compare(ctx, c->secret, secret, sizeof(secret));
    secp256k1_context *secp = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!secp) { diff_error(ctx, "secp context allocation failed"); return FAILED; }
    secp256k1_pubkey pub;
    secp256k1_xonly_pubkey xpub;
    size_t len = sizeof(compressed);
    if (!secp256k1_ec_pubkey_create(secp, &pub, secret) ||
        !secp256k1_ec_pubkey_serialize(secp, compressed, &len, &pub, SECP256K1_EC_COMPRESSED) ||
        !secp256k1_xonly_pubkey_from_pubkey(secp, &xpub, NULL, &pub) ||
        !secp256k1_xonly_pubkey_serialize(secp, xonly, &xpub)) {
        diff_error(ctx, "seeded keypair derivation failed"); ok = false;
    } else {
        ok &= compare(ctx, c->compressed, compressed, len);
        ok &= compare(ctx, c->xonly, xonly, sizeof(xonly));
    }
    secp256k1_context_destroy(secp);
    return ok ? CHECKED : FAILED;
}

/* Replace these dispatch bodies when their owning port lands. Returning
 * CHECKED requires real C output/error/state comparisons, not fixture integrity. */
static result check_identity(diff_context *ctx, const identity_case *c)
{
    bool ok = true;
    fips_fixed_entropy fixed = {(uint8_t)c->aux_entropy_byte};
    fips_entropy entropy = fips_fixed_entropy_adapter(&fixed);
    fips_secp *secp = NULL;
    fips_identity *id = NULL;
    uint8_t secret[32], exported[32];
    /* Exactly support.rs seeded_keypair: fresh SplitMix64, domain XOR,
     * four little-endian words, no hashing or reduction. Blinding is separate. */
    fips_seeded_entropy state = {c->seed ^ UINT64_C(0x6b65797061697273)};
    fips_entropy seeded = fips_seeded_entropy_adapter(&state);
    seeded.fill(seeded.ctx, secret, sizeof(secret));
    ok &= compare(ctx, c->secret, secret, sizeof(secret));
    if (fips_identity_init_secp(entropy, &secp) != FIPS_IDENTITY_OK ||
        fips_identity_from_secret_bytes(secp, entropy, secret, &id) != FIPS_IDENTITY_OK) {
        diff_error(ctx, "identity construction failed"); ok = false; goto done;
    }
    fips_identity_export_secret(id, exported);
    ok &= compare(ctx, c->secret, exported, sizeof(exported));
    fips_xonly_pubkey key = fips_identity_pubkey(id), decoded;
    fips_node_addr addr = fips_identity_node_addr(id), verified;
    fips_ipv6_address address = fips_identity_address(id);
    ok &= compare(ctx, c->xonly, key.bytes, sizeof(key.bytes));
    ok &= compare(ctx, c->node_addr, addr.bytes, sizeof(addr.bytes));
    ok &= compare(ctx, c->ipv6, address.bytes, sizeof(address.bytes));
    char npub[64], nsec[64], ipv6[40];
    if (fips_identity_npub(id, npub, sizeof(npub)) != FIPS_IDENTITY_OK ||
        fips_encode_nsec(secp, secret, nsec, sizeof(nsec)) != FIPS_IDENTITY_OK ||
        fips_ipv6_address_format(&address, ipv6, sizeof(ipv6)) != FIPS_IDENTITY_OK) {
        diff_error(ctx, "identity encoding failed"); ok = false;
    } else {
        ok &= !strcmp(npub, c->npub) && !strcmp(nsec, c->nsec) && !strcmp(ipv6, c->ipv6_text);
        if (fips_decode_npub(secp, npub, strlen(npub), &decoded) != FIPS_IDENTITY_OK ||
            fips_decode_nsec(secp, nsec, strlen(nsec), exported) != FIPS_IDENTITY_OK) ok = false;
        else {
            ok &= compare(ctx, c->xonly, decoded.bytes, 32);
            ok &= compare(ctx, c->secret, exported, 32);
        }
    }
    sodium_memzero(nsec, sizeof(nsec));
    if (c->challenge.len != 32) { ok = false; goto done; }
    fips_auth_challenge challenge = fips_auth_challenge_from_bytes(c->challenge.data);
    fips_auth_response response;
    if (fips_identity_sign_challenge(id, &challenge, c->timestamp, &response) != FIPS_IDENTITY_OK) {
        ok = false; goto done;
    }
    ok &= response.timestamp == c->timestamp;
    ok &= compare(ctx, c->xonly, response.pubkey.bytes, 32);
    ok &= compare(ctx, c->signature, response.signature, 64);
    if (fips_auth_challenge_verify(secp, &challenge, &response, &verified) != FIPS_IDENTITY_OK) ok = false;
    else ok &= compare(ctx, c->verified, verified.bytes, 16);
    memset(challenge.bytes, 0, 32);
    fips_identity_status wrong = fips_auth_challenge_verify(secp, &challenge, &response, &verified);
    ok &= wrong == FIPS_IDENTITY_SignatureVerificationFailed &&
          !strcmp(fips_identity_error_name(wrong), c->wrong_challenge_error);
done:
    sodium_memzero(secret, sizeof(secret));
    sodium_memzero(exported, sizeof(exported));
    fips_identity_destroy(id);
    fips_identity_destroy_secp(secp);
    if (!ok && !ctx->error[0]) diff_error(ctx, "identity output/error mismatch");
    return ok ? CHECKED : FAILED;
}
static fips_status codec_read(const codec_case *c, fips_reader *r,
                              uint64_t *value, uint8_t *array,
                              const uint8_t **bytes, fips_proto_error *error)
{
    fips_status status;
    if (!strcmp(c->kind, "u8")) {
        uint8_t v;
        status = fips_reader_read_u8(r, &v, error);
        if (status == FIPS_OK) *value = v;
    } else if (!strcmp(c->kind, "u16_le")) {
        uint16_t v;
        status = fips_reader_read_u16_le(r, &v, error);
        if (status == FIPS_OK) *value = v;
    } else if (!strcmp(c->kind, "u32_le")) {
        uint32_t v;
        status = fips_reader_read_u32_le(r, &v, error);
        if (status == FIPS_OK) *value = v;
    } else if (!strcmp(c->kind, "u64_le")) {
        status = fips_reader_read_u64_le(r, value, error);
    } else if (!strcmp(c->kind, "array")) {
        status = fips_reader_read_array(r, array, c->bytes.len, error);
        if (status == FIPS_OK) *bytes = array;
    } else {
        status = fips_reader_read_bytes(r, c->bytes.len, bytes, error);
    }
    return status;
}

static fips_status codec_write(const codec_case *c, fips_writer *w,
                               fips_proto_error *error)
{
    if (!strcmp(c->kind, "u8")) return fips_writer_write_u8(w, (uint8_t)c->value, error);
    if (!strcmp(c->kind, "u16_le")) return fips_writer_write_u16_le(w, (uint16_t)c->value, error);
    if (!strcmp(c->kind, "u32_le")) return fips_writer_write_u32_le(w, (uint32_t)c->value, error);
    if (!strcmp(c->kind, "u64_le")) return fips_writer_write_u64_le(w, c->value, error);
    return fips_writer_write_bytes(w, c->bytes.data, c->bytes.len, error);
}

static bool codec_error(diff_context *ctx, const diff_value *expected,
                        const fips_proto_error *actual)
{
    if (strcmp(TEXT(expected, "variant"), fips_proto_error_name(actual->variant)) ||
        actual->variant != FIPS_PROTO_MessageTooShort ||
        UINT(expected, "expected") != actual->as.message_too_short.expected ||
        UINT(expected, "got") != actual->as.message_too_short.got) {
        return diff_error(ctx, "error mismatch: got %s, expected %s (expected/got payload must match)",
                          fips_proto_error_name(actual->variant), TEXT(expected, "variant"));
    }
    return true;
}

static result check_codec(diff_context *ctx, const codec_case *c)
{
    fips_reader r;
    fips_proto_error error = {.variant = FIPS_PROTO_InvalidSignature};
    if (c->cursor) {
        diff_value *v = c->cursor;
        diff_bytes input = BYTES(v, "input_hex"), rest = BYTES(v, "rest_hex");
        fips_reader_init(&r, input.data, input.len);
        if (fips_reader_advance(&r, (size_t)UINT(v, "advance"), &error) != FIPS_OK) {
            diff_error(ctx, "cursor advance failed"); return FAILED;
        }
        /* Read the cursor state once; the checker forbids &r in nested argument lists. */
        const uint8_t *rest_ptr = fips_reader_rest(&r);
        size_t rest_len = fips_reader_remaining(&r);
        if (fips_reader_position(&r) != UINT(v, "position") ||
            rest_len != UINT(v, "remaining") ||
            !compare(ctx, rest, rest_ptr, rest_len) ||
            strcmp(TEXT(v, "require_3"), "Ok") ||
            fips_reader_require(&r, 3, &error) != FIPS_OK ||
            fips_reader_require(&r, 4, &error) != FIPS_ERR_RANGE ||
            !codec_error(ctx, FIELD(v, "require_4"), &error) ||
            fips_reader_position(&r) != UINT(v, "position")) {
            diff_error(ctx, "cursor state/require comparison failed"); return FAILED;
        }
        /* Re-encode the consumed prefix and borrowed rest. */
        uint8_t *encoded = malloc(input.len ? input.len : 1);
        if (!encoded) { diff_error(ctx, "allocation failed"); return FAILED; }
        fips_writer w;
        fips_writer_init(&w, encoded, input.len);
        size_t consumed = fips_reader_position(&r);
        rest_ptr = fips_reader_rest(&r);
        rest_len = fips_reader_remaining(&r);
        bool ok = fips_writer_write_bytes(&w, input.data, consumed, &error) == FIPS_OK &&
            fips_writer_write_bytes(&w, rest_ptr, rest_len, &error) == FIPS_OK;
        if (ok) {
            size_t written = fips_writer_len(&w);
            ok = compare(ctx, input, encoded, written);
        }
        free(encoded);
        return ok ? CHECKED : FAILED;
    }
    if (strcmp(c->kind, "u8") && strcmp(c->kind, "u16_le") &&
        strcmp(c->kind, "u32_le") && strcmp(c->kind, "u64_le") &&
        strcmp(c->kind, "array") && strcmp(c->kind, "bytes") && strcmp(c->kind, "empty")) {
        diff_error(ctx, "unknown codec kind %s", c->kind); return FAILED;
    }
    uint8_t *array = malloc(c->bytes.len ? c->bytes.len : 1);
    uint8_t *encoded = malloc(c->bytes.len ? c->bytes.len : 1);
    if (!array || !encoded) {
        free(array); free(encoded); diff_error(ctx, "allocation failed"); return FAILED;
    }
    uint64_t value = 0;
    const uint8_t *bytes = NULL;
    fips_reader_init(&r, c->bytes.data, c->bytes.len);
    bool ok = codec_read(c, &r, &value, array, &bytes, &error) == FIPS_OK;
    if (!ok) diff_error(ctx, "valid codec input rejected");
    if (ok && (fips_reader_position(&r) != c->bytes.len || fips_reader_remaining(&r))) {
        ok = diff_error(ctx, "decoded cursor mismatch");
    }
    if (ok) {
        if (c->has_value) {
            if (value != c->value) ok = diff_error(ctx, "decoded scalar mismatch");
        } else ok = compare(ctx, c->bytes, bytes, c->bytes.len);
    }
    fips_writer w;
    fips_writer_init(&w, encoded, c->bytes.len);
    if (codec_write(c, &w, &error) != FIPS_OK) {
        ok = diff_error(ctx, "encoding failed");
    } else ok &= compare(ctx, c->bytes, encoded, fips_writer_len(&w));
    for (size_t i = 0; i < COUNT(c->truncations); ++i) {
        diff_value *t = ITEM(c->truncations, i);
        diff_bytes input = BYTES(t, "input_hex");
        fips_reader_init(&r, input.data, input.len);
        error.variant = FIPS_PROTO_InvalidSignature;
        if (codec_read(c, &r, &value, array, &bytes, &error) != FIPS_ERR_RANGE ||
            !codec_error(ctx, FIELD(t, "error"), &error) || fips_reader_position(&r) != 0) {
            ok = diff_error(ctx, "truncation %zu error/cursor mismatch", i);
        }
    }
    free(array); free(encoded);
    return ok ? CHECKED : FAILED;
}
/* Count every role's step, emitted message and transport frame separately. */
static void noise_step(diff_context *ctx, const noise_case *c, counts *n, size_t step,
                       const char *event, const fips_handshake_state *i,
                       const fips_handshake_state *r)
{
    const char *roles[] = {"initiator", "responder"};
    const fips_handshake_state *states[] = {i, r};
    diff_value *expected = ITEM(c->steps, step);
    for (size_t role = 0; role < 2; ++role) {
        ctx->error[0] = '\0';
        snprintf(ctx->id, sizeof(ctx->id), "noise/%s/steps/%s/%s", c->pattern, event, roles[role]);
        diff_value *v = FIELD(FIELD(expected, "state"), roles[role]);
        bool ok = !strcmp(TEXT(expected, "event"), event);
        ok &= compare(ctx, BYTES(v, "h"), fips_handshake_hash(states[role]), 32);
        ok &= !strcmp(TEXT(v, "progress"), fips_handshake_progress_name(states[role]->progress));
        if (!ok) diff_error(ctx, "event/hash/progress mismatch at step %zu", step);
        report(ctx, n, ok ? CHECKED : FAILED, NULL);
    }
}
static void noise_message(diff_context *ctx, const noise_case *c, counts *n,
                          size_t index, const uint8_t *wire, size_t len)
{
    ctx->error[0] = '\0';
    snprintf(ctx->id, sizeof(ctx->id), "noise/%s/messages/%zu", c->pattern, index + 1);
    diff_value *v = ITEM(c->messages, index);
    bool ok = UINT(v, "number") == index + 1 &&
        !strcmp(TEXT(v, "sender"), index == 1 ? "responder" : "initiator");
    ok &= compare(ctx, BYTES(v, "bytes"), wire, len);
    report(ctx, n, ok ? CHECKED : FAILED, NULL);
}
/* noise.rs tries corruption at the unused counter BEFORE the authentic frame.
 * A frame is one check covering seal, failed open, successful open and replay. */
static result check_noise_frame(diff_context *ctx, const diff_value *frame,
                               fips_noise_session *initiator, fips_noise_session *responder)
{
    const char *direction = TEXT(frame, "direction");
    bool forward = !strcmp(direction, "initiator_to_responder");
    if (!forward && strcmp(direction, "responder_to_initiator")) {
        diff_error(ctx, "unknown frame direction %s", direction); return FAILED;
    }
    fips_noise_session *send = forward ? initiator : responder;
    fips_noise_session *recv = forward ? responder : initiator;
    uint64_t counter = UINT(frame, "counter"), corrupt_counter = UINT(frame, "corrupt_counter");
    diff_bytes ad = BYTES(frame, "aad_hex"), plain = BYTES(frame, "plaintext_hex");
    diff_bytes sealed = BYTES(frame, "sealed"), corrupt = BYTES(frame, "corrupt_input_hex");
    uint8_t wire[FIPS_NOISE_MAX_MESSAGE_SIZE], opened[FIPS_NOISE_MAX_MESSAGE_SIZE];
    size_t len = SIZE_MAX;
    if (fips_noise_session_current_send_counter(send) != counter ||
        fips_noise_session_seal(send, ad.data, ad.len, plain.data, plain.len,
                               wire, sizeof(wire), &len) != FIPS_NOISE_OK) {
        diff_error(ctx, "frame seal/counter mismatch"); return FAILED;
    }
    bool ok = compare(ctx, sealed, wire, len);
    fips_replay_window before = recv->replay_window;
    len = SIZE_MAX;
    fips_noise_status rc = fips_noise_session_open(recv, corrupt_counter, ad.data, ad.len,
        corrupt.data, corrupt.len, opened, sizeof(opened), &len);
    bool acceptable = fips_noise_session_check_replay(recv, corrupt_counter) == FIPS_NOISE_OK;
    if (rc != FIPS_NOISE_DecryptionFailed || strcmp(TEXT(frame, "authentication_error"), "DecryptionFailed") ||
        acceptable != FIELD(frame, "failed_counter_still_acceptable")->as.boolean ||
        len != SIZE_MAX || memcmp(&before, &recv->replay_window, sizeof(before)))
        ok = diff_error(ctx, "corrupt frame error/window/failed-counter mismatch");
    rc = fips_noise_session_open(recv, counter, ad.data, ad.len,
        sealed.data, sealed.len, opened, sizeof(opened), &len);
    if (rc != FIPS_NOISE_OK) {
        diff_error(ctx, "authentic frame rejected after failed open"); return FAILED;
    }
    ok &= compare(ctx, plain, opened, len);
    ok &= compare(ctx, BYTES(frame, "opened_hex"), opened, len);
    char replay_error[64];
    snprintf(replay_error, sizeof(replay_error), "ReplayDetected(%" PRIu64 ")", counter);
    len = SIZE_MAX;
    if (fips_noise_session_open(recv, counter, ad.data, ad.len,
        sealed.data, sealed.len, opened, sizeof(opened), &len) != FIPS_NOISE_ReplayDetected ||
        strcmp(TEXT(frame, "replay_error"), replay_error) || len != SIZE_MAX)
        ok = diff_error(ctx, "duplicate frame error mismatch");
    return ok ? CHECKED : FAILED;
}
static void check_noise(diff_context *ctx, const noise_case *c, counts *n)
{
    bool xk = !strcmp(c->pattern, "XK");
    fips_fixed_entropy fixed = {0x5a};
    fips_entropy blinding = fips_fixed_entropy_adapter(&fixed);
    /* support.rs seeded_keypair uses an independent domain-separated stream;
     * noise.rs shares ONE ephemeral stream, drawn by initiator write1 then
     * responder write2. Neither construction nor context blinding consumes it. */
    fips_seeded_entropy stream = {c->entropy_seed};
    fips_entropy entropy = fips_seeded_entropy_adapter(&stream);
    fips_secp *ctx_secp = NULL;
    fips_identity *keys[2] = {NULL, NULL};
    fips_handshake_state i = {0}, r = {0}, fork = {0}, before = {0};
    fips_aead_key is = {{0}}, ir = {{0}}, rs = {{0}}, rr = {{0}};
    fips_noise_session initiator = {0}, responder = {0};
    uint8_t pub[2][33], secret[32] = {0}, wire[FIPS_NOISE_HANDSHAKE_MSG1_SIZE];
    size_t len = 0;
    ctx->error[0] = '\0';
    snprintf(ctx->id, sizeof(ctx->id), "noise/%s/setup", c->pattern);
    if ((!xk && strcmp(c->pattern, "IK")) || c->epoch.len != 8 ||
        COUNT(c->keypair_seeds) != 2 || COUNT(c->messages) != (xk ? 3u : 2u) ||
        COUNT(c->steps) != (xk ? 7u : 5u) ||
        fips_identity_init_secp(blinding, &ctx_secp) != FIPS_IDENTITY_OK) goto failed;
    for (size_t k = 0; k < 2; ++k) {
        fips_seeded_entropy seeded = {ITEM(c->keypair_seeds, k)->as.number ^ UINT64_C(0x6b65797061697273)};
        fips_entropy keyed = fips_seeded_entropy_adapter(&seeded);
        keyed.fill(keyed.ctx, secret, sizeof(secret));
        fips_identity_status rc = fips_identity_from_secret_bytes(ctx_secp, blinding, secret, &keys[k]);
        sodium_memzero(secret, sizeof(secret));
        if (rc != FIPS_IDENTITY_OK) goto failed;
        fips_identity_pubkey_full(keys[k], pub[k]);
    }
#define NOISE_TRY(call) do { if ((call) != FIPS_NOISE_OK) goto failed; } while (0)
    NOISE_TRY(xk ? fips_handshake_new_xk_initiator(&i, ctx_secp, keys[0], pub[1], c->epoch.data, entropy)
                 : fips_handshake_new_initiator(&i, ctx_secp, keys[0], pub[1], c->epoch.data, entropy));
    NOISE_TRY(xk ? fips_handshake_new_xk_responder(&r, ctx_secp, keys[1], c->epoch.data, entropy)
                 : fips_handshake_new_responder(&r, ctx_secp, keys[1], c->epoch.data, entropy));
    noise_step(ctx, c, n, 0, "constructed", &i, &r);
    NOISE_TRY(xk ? fips_handshake_write_xk_message_1(&i, wire, sizeof(wire), &len)
                 : fips_handshake_write_message_1(&i, wire, sizeof(wire), &len));
    noise_message(ctx, c, n, 0, wire, len);
    noise_step(ctx, c, n, 1, "initiator_write_1", &i, &r);
    /* Feed fixture bytes to readers, independently of the writer comparison. */
    diff_bytes msg = BYTES(ITEM(c->messages, 0), "bytes");
    NOISE_TRY(xk ? fips_handshake_read_xk_message_1(&r, msg.data, msg.len)
                 : fips_handshake_read_message_1(&r, msg.data, msg.len));
    noise_step(ctx, c, n, 2, "responder_read_1", &i, &r);
    NOISE_TRY(xk ? fips_handshake_write_xk_message_2(&r, wire, sizeof(wire), &len)
                 : fips_handshake_write_message_2(&r, wire, sizeof(wire), &len));
    noise_message(ctx, c, n, 1, wire, len);
    noise_step(ctx, c, n, 3, "responder_write_2", &i, &r);
    msg = BYTES(ITEM(c->messages, 1), "bytes");
    fips_handshake_fork(&i, &before);
    NOISE_TRY(xk ? fips_handshake_fork_xk2(&i, msg.data, msg.len, &fork)
                 : fips_handshake_fork_msg2(&i, msg.data, msg.len, &fork));
    if (memcmp(&i, &before, sizeof(i))) {
        diff_error(ctx, "msg2 fork changed original bytes"); goto failed;
    }
    fips_handshake_destroy(&before);
    fips_handshake_destroy(&i);
    fips_handshake_fork(&fork, &i);
    fips_handshake_destroy(&fork);
    noise_step(ctx, c, n, 4, "initiator_read_2_on_fork", &i, &r);
    if (xk) {
        NOISE_TRY(fips_handshake_write_xk_message_3(&i, wire, sizeof(wire), &len));
        noise_message(ctx, c, n, 2, wire, len);
        noise_step(ctx, c, n, 5, "initiator_write_3", &i, &r);
        msg = BYTES(ITEM(c->messages, 2), "bytes");
        fips_handshake_fork(&r, &before);
        NOISE_TRY(fips_handshake_fork_xk3(&r, msg.data, msg.len, &fork));
        if (memcmp(&r, &before, sizeof(r))) {
            diff_error(ctx, "msg3 fork changed original bytes"); goto failed;
        }
        fips_handshake_destroy(&before);
        fips_handshake_destroy(&r);
        fips_handshake_fork(&fork, &r);
        fips_handshake_destroy(&fork);
        noise_step(ctx, c, n, 6, "responder_read_3_on_fork", &i, &r);
    }
    const char *roles[] = {"initiator_h", "responder_h"};
    const fips_handshake_state *states[] = {&i, &r};
    for (size_t k = 0; k < 2; ++k) {
        ctx->error[0] = '\0';
        snprintf(ctx->id, sizeof(ctx->id), "noise/%s/post_handshake/%s", c->pattern, roles[k]);
        bool ok = compare(ctx, BYTES(c->post_handshake, roles[k]), fips_handshake_hash(states[k]), 32);
        report(ctx, n, ok ? CHECKED : FAILED, NULL);
    }
    diff_value *ck = FIELD(c->post_handshake, "ck");
    if (ck && ck->type == D_BYTES && ck->as.bytes.len) {
        snprintf(ctx->id, sizeof(ctx->id), "noise/%s/post_handshake/ck", c->pattern);
        bool ok = compare(ctx, ck->as.bytes, i.symmetric.ck, 32);
        ok &= compare(ctx, ck->as.bytes, r.symmetric.ck, 32);
        report(ctx, n, ok ? CHECKED : FAILED, NULL);
    }
    ctx->error[0] = '\0';
    snprintf(ctx->id, sizeof(ctx->id), "noise/%s/finish", c->pattern);
    uint8_t ih[32], rh[32], ipub[33], rpub[33], ie[8], re[8];
    NOISE_TRY(fips_handshake_finish(&i, &is, &ir, ih, ipub, ie));
    NOISE_TRY(fips_handshake_finish(&r, &rs, &rr, rh, rpub, re));
    bool ok = !memcmp(is.bytes, rr.bytes, 32) && !memcmp(ir.bytes, rs.bytes, 32) &&
        memcmp(is.bytes, ir.bytes, 32) && !memcmp(ih, rh, 32) &&
        !memcmp(ipub, pub[1], 33) && !memcmp(rpub, pub[0], 33) &&
        !memcmp(ie, c->epoch.data, 8) && !memcmp(re, c->epoch.data, 8);
    report(ctx, n, ok ? CHECKED : FAILED, NULL);
    NOISE_TRY(fips_noise_session_from_handshake(&initiator, FIPS_HANDSHAKE_INITIATOR,
                                               &is, &ir, ih, ipub, ie));
    NOISE_TRY(fips_noise_session_from_handshake(&responder, FIPS_HANDSHAKE_RESPONDER,
                                               &rs, &rr, rh, rpub, re));
    for (size_t frame = 0; frame < COUNT(c->frames); ++frame) {
        ctx->error[0] = '\0';
        snprintf(ctx->id, sizeof(ctx->id), "noise/%s/frames/%zu", c->pattern, frame);
        report(ctx, n, check_noise_frame(ctx, ITEM(c->frames, frame), &initiator, &responder), NULL);
    }
    goto done;
failed:
    if (!ctx->error[0]) diff_error(ctx, "Noise construction/read/write/finish failed");
    report(ctx, n, FAILED, NULL);
done:
#undef NOISE_TRY
    sodium_memzero(secret, sizeof(secret));
    fips_noise_session_destroy(&initiator); fips_noise_session_destroy(&responder);
    fips_handshake_destroy(&i); fips_handshake_destroy(&r);
    fips_handshake_destroy(&fork); fips_handshake_destroy(&before);
    fips_aead_key_destroy(&is); fips_aead_key_destroy(&ir);
    fips_aead_key_destroy(&rs); fips_aead_key_destroy(&rr);
    fips_identity_destroy(keys[0]); fips_identity_destroy(keys[1]);
    fips_identity_destroy_secp(ctx_secp);
}
static result check_replay(diff_context *ctx, const diff_value *step, uint64_t window_size,
                           fips_replay_window *window)
{
    uint64_t counter = UINT(step, "counter");
    bool accept = fips_replay_window_check(window, counter);
    if (accept && !fips_replay_window_mark(window, counter)) {
        diff_error(ctx, "acceptable replay counter could not be marked"); return FAILED;
    }
    if (window_size != FIPS_REPLAY_WINDOW_SIZE ||
        accept != FIELD(step, "accept")->as.boolean ||
        fips_replay_window_highest(window) != UINT(step, "highest_after")) {
        diff_error(ctx, "replay accept/highest/window-size mismatch"); return FAILED;
    }
    return CHECKED;
}
/* Derive the common-input addresses from the declared seeds, never parse the
 * Debug strings or assume the coords vector's addresses are the same inputs. */
static bool check_coord_debug(diff_context *ctx, const proto_case *c,
                              fips_tree_coordinate *scratch)
{
    fips_fixed_entropy fixed = {0x5a};
    fips_entropy entropy = fips_fixed_entropy_adapter(&fixed);
    fips_secp *secp = NULL;
    fips_node_addr addrs[2];
    diff_value *seeds = FIELD(c->common_inputs, "identity_seeds");
    bool ok = COUNT(seeds) == 2;
    if (!ok || fips_identity_init_secp(entropy, &secp) != FIPS_IDENTITY_OK)
        return diff_error(ctx, "common coordinate identity setup failed");
    for (size_t i = 0; i < 2; ++i) {
        fips_seeded_entropy state = {ITEM(seeds, i)->as.number ^ UINT64_C(0x6b65797061697273)};
        fips_entropy seeded = fips_seeded_entropy_adapter(&state);
        uint8_t secret[32];
        fips_identity *identity = NULL;
        seeded.fill(seeded.ctx, secret, sizeof(secret));
        fips_identity_status status = fips_identity_from_secret_bytes(secp, entropy, secret, &identity);
        sodium_memzero(secret, sizeof(secret));
        if (status != FIPS_IDENTITY_OK) { ok = false; break; }
        addrs[i] = fips_identity_node_addr(identity);
        fips_identity_destroy(identity);
    }
    fips_identity_destroy_secp(secp);
    if (!ok) return diff_error(ctx, "common coordinate identity derivation failed");
    char debug[128];
    fips_tree_coordinate_root_with_meta(scratch, addrs[0], UINT64_C(0x0123456789abcdef), 1700000000);
    if (fips_tree_coordinate_debug(scratch, debug, sizeof(debug)) != FIPS_OK ||
        strcmp(debug, TEXT(c->common_inputs, "src_coord_debug")))
        return diff_error(ctx, "src_coord_debug mismatch");
    fips_node_addr dst[] = {addrs[1], addrs[0]};
    if (fips_tree_coordinate_from_addrs(scratch, dst, 2, NULL) != FIPS_OK ||
        fips_tree_coordinate_debug(scratch, debug, sizeof(debug)) != FIPS_OK ||
        strcmp(debug, TEXT(c->common_inputs, "dst_coord_debug")))
        return diff_error(ctx, "dst_coord_debug mismatch");
    puts("PASS common_inputs.src_coord_debug / common_inputs.dst_coord_debug");
    return true;
}
static result check_coords(diff_context *ctx, const proto_case *c)
{
    diff_bytes bytes = BYTES(c->record, "bytes");
    diff_value *addresses = FIELD(FIELD(c->record, "input"), "addresses_hex");
    fips_tree_coordinate *actual = malloc(sizeof(*actual)), *expected = malloc(sizeof(*expected));
    uint8_t *encoded = malloc(bytes.len ? bytes.len : 1);
    bool ok = actual && expected && encoded;
    if (!ok) { diff_error(ctx, "coordinate allocation failed"); goto done; }
    size_t count = COUNT(addresses);
    if (!count || count > FIPS_COORD_MAX_ENTRIES) { ok = diff_error(ctx, "coordinate count out of range"); goto done; }
    expected->len = (uint16_t)count;
    for (size_t i = 0; i < count; ++i) {
        diff_bytes addr = ITEM(addresses, i)->as.bytes;
        if (addr.len != 16) { ok = diff_error(ctx, "bad input address length"); goto done; }
        memcpy(expected->entries[i].node_addr.bytes, addr.data, 16);
        expected->entries[i].sequence = expected->entries[i].timestamp = 0;
    }
    fips_reader r;
    fips_reader_init(&r, bytes.data, bytes.len);
    if (fips_decode_coords(&r, actual, NULL) != FIPS_OK ||
        !fips_tree_coordinate_equal(actual, expected) ||
        fips_tree_coordinate_depth(actual) != UINT(c->record, "decoded_depth") ||
        fips_reader_position(&r) != UINT(c->record, "consumed")) {
        ok = diff_error(ctx, "coordinate decode/structure/depth/consumed mismatch"); goto done;
    }
    fips_writer w;
    fips_writer_init(&w, encoded, bytes.len);
    ok = fips_encode_coords(actual, &w, NULL) == FIPS_OK && compare(ctx, bytes, encoded, fips_writer_len(&w));
    ok &= check_coord_debug(ctx, c, expected);
 done:
    free(actual); free(expected); free(encoded);
    return ok ? CHECKED : FAILED;
}
static result check_coords_empty(diff_context *ctx, const proto_case *c)
{
    diff_bytes bytes = BYTES(c->record, "bytes");
    fips_tree_coordinate *coord = malloc(sizeof(*coord));
    if (!coord) { diff_error(ctx, "coordinate allocation failed"); return FAILED; }
    fips_node_addr addr = {{0}};
    fips_tree_coordinate_root(coord, addr);
    fips_reader r;
    fips_reader_init(&r, bytes.data, bytes.len);
    fips_proto_error e;
    bool ok = fips_decode_coords(&r, coord, &e) == FIPS_ERR_RANGE &&
        e.variant == FIPS_PROTO_Malformed && !strcmp(e.as.malformed, "coordinate with zero entries") &&
        fips_reader_position(&r) == 0;
    char debug[96];
    if (ok) {
        snprintf(debug, sizeof(debug), "Malformed(\"%s\")", e.as.malformed);
        ok = !strcmp(debug, TEXT(FIELD(c->record, "required_error"), "debug"));
    }
    bool present = true;
    ok &= fips_decode_optional_coords(&r, coord, &present, NULL) == FIPS_OK && !present &&
        FIELD(c->record, "optional")->type == D_NULL && fips_reader_position(&r) == 2 && coord->len == 1;
    uint8_t encoded[2];
    fips_writer w;
    fips_writer_init(&w, encoded, sizeof(encoded));
    ok &= fips_encode_empty_coords(&w, NULL) == FIPS_OK && compare(ctx, bytes, encoded, fips_writer_len(&w));
    free(coord);
    if (!ok) diff_error(ctx, "empty coordinate required/optional/encoding mismatch");
    return ok ? CHECKED : FAILED;
}
/* nostrc-82g9n.6 adapters: the catalog/constants cases carry their value in record. */
static result check_constants_case(diff_context *ctx, const proto_case *c) { return check_proto_constants(ctx, c->record); }
static result check_link_catalog_case(diff_context *ctx, const proto_case *c) { return check_proto_link_catalog(ctx, c->record); }
static result check_session_catalog_case(diff_context *ctx, const proto_case *c) { return check_proto_session_catalog(ctx, c->record); }
typedef struct proto_checker {
    const char *name, *owner;
    result (*check)(diff_context *, const proto_case *);
} proto_checker;
static const proto_checker proto_checkers[] = {
    {"coords", "nostrc-82g9n.1", check_coords},
    {"coords-empty", "nostrc-82g9n.1", check_coords_empty},
    {"bloom", "nostrc-82g9n.3", check_proto_bloom}, {"filter-announce", "nostrc-82g9n.3", check_proto_filter_announce},
    {"parent-declaration", "nostrc-82g9n.2", check_proto_parent_declaration}, {"tree-announce", "nostrc-82g9n.2", check_proto_tree_announce},
    {"lookup-request", "nostrc-82g9n.5", check_proto_lookup_request}, {"lookup-response", "nostrc-82g9n.5", check_proto_lookup_response},
    {"sender-report", "nostrc-f12v2.4", check_proto_sender_report}, {"session-sender-report", "nostrc-f12v2.4", check_proto_session_sender_report},
    {"receiver-report", "nostrc-f12v2.4", check_proto_receiver_report}, {"session-receiver-report", "nostrc-f12v2.4", check_proto_session_receiver_report},
    {"path-mtu", "nostrc-f12v2.4", check_proto_path_mtu},
    {"path-probe", "nostrc-f12v2.6", check_proto_path_probe}, {"path-ack", "nostrc-f12v2.6", check_proto_path_ack}, {"path-close", "nostrc-f12v2.6", check_proto_path_close},
    {"disconnect", "nostrc-82g9n.7", check_proto_disconnect}, {"fmp-msg-1", "nostrc-82g9n.7", check_proto_fmp_msg_1},
    {"fmp-msg-2", "nostrc-82g9n.7", check_proto_fmp_msg_2}, {"fmp-encrypted", "nostrc-82g9n.7", check_proto_fmp_encrypted},
    {"fsp-setup", "nostrc-82g9n.8", check_proto_fsp_setup}, {"fsp-ack", "nostrc-82g9n.8", check_proto_fsp_ack},
    {"fsp-msg3", "nostrc-82g9n.8", check_proto_fsp_msg3}, {"fsp-encrypted", "nostrc-82g9n.8", check_proto_fsp_encrypted},
    {"session-datagram", "nostrc-82g9n.6", check_proto_session_datagram},
    {"constants", "nostrc-82g9n.6", check_constants_case}, {"link_catalog", "nostrc-82g9n.6", check_link_catalog_case},
    {"session_catalog", "nostrc-82g9n.6", check_session_catalog_case}
};
static result check_proto(diff_context *ctx, const proto_case *c, const char **owner)
{
    for (size_t i = 0; i < sizeof(proto_checkers) / sizeof(proto_checkers[0]); ++i) {
        const proto_checker *checker = &proto_checkers[i];
        if (strcmp(checker->name, c->name)) continue;
        *owner = checker->owner;
        return checker->check ? checker->check(ctx, c) : SKIPPED;
    }
    diff_error(ctx, "missing proto dispatch for %s", c->name);
    return FAILED;
}

/* The rekey family is one checked trace; every state, output and authenticated
 * delivery in all events is compared. No fixture bytes are used as C outputs. */
static bool rk_number(diff_context *ctx, const diff_value *v, const char *key, uint64_t n) {
    diff_value *f = FIELD(v, key);
    return (f && f->type == D_UINT && f->as.number == n) ||
        diff_error(ctx, "%s: expected %" PRIu64 ", C %" PRIu64, key, f ? f->as.number : 0, n);
}
static bool rk_text(diff_context *ctx, const diff_value *v, const char *key, const char *text) {
    diff_value *f = FIELD(v, key);
    return (f && f->type == D_TEXT && !strcmp(f->as.text, text)) ||
        diff_error(ctx, "%s: expected %s, C %s", key, f && f->type == D_TEXT ? f->as.text : "(missing)", text);
}
static bool rk_bool(diff_context *ctx, const diff_value *v, const char *key, bool b) {
    diff_value *f = FIELD(v, key);
    return (f && f->type == D_BOOL && f->as.boolean == b) || diff_error(ctx, "%s boolean mismatch", key);
}
static bool rk_time(diff_context *ctx, const diff_value *v, const char *key, fips_instant t) {
    diff_value *f = FIELD(v, key);
    if (!t.ops) return (f && f->type == D_NULL) || diff_error(ctx, "%s: C absent", key);
    return rk_number(ctx, v, key, t.opaque[0]);
}
static const char *rk_role(fips_rekey_role role) { return role == FIPS_REKEY_Initiator ? "Initiator" : "Responder"; }
static const char *rk_slot(fips_rekey_slot slot) {
    return slot == FIPS_REKEY_Current ? "Current" : slot == FIPS_REKEY_Previous ? "Previous" : "Pending";
}
static bool rk_epoch(diff_context *ctx, const diff_value *v, fips_rekey_epoch_view e) {
    if (!e.present) return v->type == D_NULL || diff_error(ctx, "unexpected epoch");
    if (v->type != D_OBJECT) return diff_error(ctx, "missing epoch");
    return rk_number(ctx, v, "tag", e.tag) && rk_bool(ctx, v, "k", e.k) &&
        rk_text(ctx, v, "role", rk_role(e.role)) && rk_time(ctx, v, "started_ms", e.started) &&
        rk_time(ctx, v, "base_ms", e.base) && rk_text(ctx, v, "recv", e.recv == FIPS_REKEY_Here ? "Here" : "Delegated");
}
static bool rk_state(diff_context *ctx, const diff_value *expected, const fips_rekey *m) {
    fips_rekey_view v = fips_rekey_get_view(m);
    char identity[96], cycle[48];
    if (v.proven) snprintf(identity, sizeof identity, "Proven(PeerKey(%02x%02x%02x%02x))",
        v.identity.key.bytes[0], v.identity.key.bytes[1], v.identity.key.bytes[2], v.identity.key.bytes[3]);
    else return diff_error(ctx, "trace unexpectedly unproven");
    const char *wait = v.wait == FIPS_REKEY_WAIT_Evidence ? "Evidence" : v.wait == FIPS_REKEY_WAIT_Delay ? "Delay" : "Held";
    if (v.cycle == FIPS_REKEY_PendI) snprintf(cycle, sizeof cycle, "PendI(%s)", wait);
    else snprintf(cycle, sizeof cycle, "%s", v.cycle == FIPS_REKEY_Idle ? "Idle" : v.cycle == FIPS_REKEY_Init ? "Init" : "PendR");
    diff_value *epoch = FIELD(expected, "peer_epoch_hex");
    bool ok = v.has_peer_epoch ? epoch->type == D_BYTES && compare(ctx, epoch->as.bytes, v.peer_epoch, 8) : epoch->type == D_NULL;
    return ok && rk_text(ctx, expected, "identity", identity) && rk_text(ctx, expected, "cycle", cycle) &&
        rk_epoch(ctx, FIELD(expected, "current"), v.current) && rk_epoch(ctx, FIELD(expected, "previous"), v.previous) &&
        rk_epoch(ctx, FIELD(expected, "pending"), v.pending) && rk_number(ctx, expected, "candidates", v.candidates) &&
        rk_time(ctx, expected, "last_rx_ms", v.last_rx) && rk_time(ctx, expected, "link_since_ms", v.link_since) &&
        rk_time(ctx, expected, "next_wake_ms", fips_rekey_next_wake(m)) && rk_bool(ctx, expected, "k", fips_rekey_k(m)) &&
        rk_number(ctx, expected, "sent", fips_rekey_sent(m));
}
static bool rk_debug(diff_context *ctx, const diff_value *v, const fips_rekey_output *o) {
    char text[256];
    switch (o->kind) {
    case FIPS_REKEY_OUT_Bind: {
        char tag[64];
        if (o->has_tag) snprintf(tag, sizeof tag, "Some(EpochTag(%" PRIu64 "))", o->tag);
        else snprintf(tag, sizeof tag, "None");
        snprintf(text, sizeof text, "Bind { transport: TransportId(%" PRIu64 "), index: %u, tag: %s, purpose: %s }",
            (uint64_t)o->transport, o->index, tag, (const char *[]){"Attempt", "Pending", "Current", "Previous"}[o->purpose]); break;
    }
    case FIPS_REKEY_OUT_Unbind:
        snprintf(text, sizeof text, "Unbind { transport: TransportId(%" PRIu64 "), index: %u }", (uint64_t)o->transport, o->index); break;
    case FIPS_REKEY_OUT_Installed: {
        const char *evidence[] = {"Proof", "Frame", "Restart", "Assumed", "Delay", "First"};
        snprintf(text, sizeof text, "Installed { tag: EpochTag(%" PRIu64 "), role: %s, evidence: %s }",
            o->tag, rk_role(o->role), evidence[o->evidence]); break;
    }
    case FIPS_REKEY_OUT_Rekeyed:
        snprintf(text, sizeof text, "Rekeyed { role: %s }", rk_role(o->role)); break;
    case FIPS_REKEY_OUT_Retired:
        snprintf(text, sizeof text, "Retired { tag: EpochTag(%" PRIu64 ") }", o->tag); break;
    case FIPS_REKEY_OUT_Deliver:
        snprintf(text, sizeof text, "Deliver { slot: %s, tag: EpochTag(%" PRIu64 "), range: %zu..%zu }",
            rk_slot(o->slot), o->tag, o->begin, o->end); break;
    default: return diff_error(ctx, "unexpected trace output %u", (unsigned)o->kind);
    }
    return rk_text(ctx, v, "debug", text);
}
static bool rk_params(diff_context *ctx, const rekey_case *c) {
    fips_rekey_config x = strstr(c->profile, "FmpProfile") ? fips_rekey_config_fmp() : fips_rekey_config_fsp();
    char actual[2048]; size_t n = 0;
#define RAPP(...) do { int w = snprintf(actual + n, sizeof actual - n, __VA_ARGS__); \
    if (w < 0 || (size_t)w >= sizeof actual - n) { return false; } n += (size_t)w; } while (0)
#define RD(field) do { if (x.field.nanos) return false; if (x.field.secs) { RAPP(", " #field ": %" PRIu64 "s", x.field.secs); } else { RAPP(", " #field ": 0ns"); } } while (0)
    RAPP("Config { after: %" PRIu64 "s, jitter: %u, messages: %" PRIu64, x.after.secs, x.jitter, x.messages);
    RD(handshake); RD(check); RD(drain); RD(retention); RD(hold); RD(revert); RD(dampening);
    RD(ttl); RD(young); RD(link_age); RD(session_age);
    if (x.floor_link.nanos || x.floor_session.nanos) return false;
    if (x.floor_link.secs) { RD(floor_link); } else { RAPP(", floor_link: 0ns"); }
    if (x.floor_session.secs) { RD(floor_session); } else { RAPP(", floor_session: 0ns"); } RD(floor_cutover); RD(silence);
    RAPP(", resends: %u, soft: %" PRIu64 ", hard: %" PRIu64, x.resends, x.soft, x.hard);
    RD(delay); RAPP(", arrival: %s, reads: %u }", x.arrival ? "true" : "false", (unsigned)x.reads);
#undef RD
#undef RAPP
    return !strcmp(c->params_debug, actual) || diff_error(ctx, "rekey config mismatch");
}
static result check_rekey(diff_context *ctx, const rekey_case *c) {
    const bool legacy = c->legacy_release != NULL;
    const bool fmp = strstr(c->profile, "FmpProfile") != NULL;
    const fips_rekey_profile *profile = fmp ? &fips_fmp_rekey_profile : &fips_fsp_rekey_profile;
    snprintf(ctx->id, sizeof ctx->id, "%s/%s/init", legacy ? c->legacy_release : "rekey", fmp ? "fmp" : "fsp");
    rp_node *nodes = calloc(2, sizeof *nodes);
    bool ok = nodes && c->epoch.len == 8 && COUNT(c->keypair_seeds) == 2 &&
        !strcmp(c->profile, fmp ? "FmpProfile<SeededEntropy> (Noise IK)" : "FspProfile<SeededEntropy> (Noise XK)") &&
        !strcmp(c->initial_handshake, fmp ? "noise.json IK (before frame exports)" : "noise.json XK (before frame exports)") && rk_params(ctx, c) &&
        !strcmp(c->clock, legacy ? "legacy model tick + current next_wake; 10ms channel latency" :
            "TestClock milliseconds, 0 -> 600000 then 10ms per delivered packet, then application traffic and next_wake deadlines") &&
        COUNT(c->initial) == (legacy ? 1u : 2u) && COUNT(c->final) == (legacy ? 1u : 2u);
    fips_seeded_entropy entropy_state = {fmp ? 0x494b : 0x584b};
    fips_entropy entropy = fips_seeded_entropy_adapter(&entropy_state);
    fips_index_allocator indices;
    uint32_t index_storage[16];
    fips_index_allocator_init(&indices, index_storage, 16, FIPS_INDEX_DEFAULT_MAX_ATTEMPTS, entropy);
    for (size_t k = 0; ok && k < 2; ++k)
        ok = rp_init(&nodes[k], ITEM(c->keypair_seeds, k)->as.number, entropy, c->epoch.data);
    if (ok) ok = rp_pair(&nodes[0], &nodes[1], profile);
    if (ok && legacy && fmp) {
        ok = fips_index_allocator_reserve(&indices, fips_session_index_new(10)) == FIPS_INDEX_OK;
        nodes[0].env.indices = fips_rekey_index_source_from_allocator(&indices);
    }
    for (size_t k = 0; ok && k < COUNT(c->initial); ++k) ok = rk_state(ctx, ITEM(c->initial, k), &nodes[k].machine);
    entropy_state.state = c->entropy_seed;
    fips_test_clock clock = {0};
    if (ok && !legacy) ok = fips_test_clock_advance(&clock, 600000) == FIPS_OK;
    fips_clock clock_api = fips_test_clock_adapter(&clock);
    for (size_t event = 0; ok && event < COUNT(c->events); ++event) {
        diff_value *e = ITEM(c->events, event);
        uint64_t at = UINT(e, "at_ms"), who = UINT(e, "node");
        snprintf(ctx->id, sizeof ctx->id, "%s/event/%zu/node/%" PRIu64, legacy ? c->legacy_release : "rekey", event, who);
        if (who > (legacy ? 0u : 1u) || UINT(e, "event") != event || at < clock.millis) { ok = diff_error(ctx, "invalid trace clock/node"); break; }
        const char *label = TEXT(e, "input");
        if (!legacy && !strcmp(label, "Poll (next_wake)")) {
            /* First poll in each pair advances to the actual minimum wake;
             * second is at the same instant, as rekey.rs's two-node loop. */
            if (at != clock.millis) {
                fips_instant a = fips_rekey_next_wake(&nodes[0].machine), b = fips_rekey_next_wake(&nodes[1].machine);
                uint64_t wake = !a.ops ? b.opaque[0] : !b.ops ? a.opaque[0] : a.opaque[0] < b.opaque[0] ? a.opaque[0] : b.opaque[0];
                if (at != wake) { ok = diff_error(ctx, "poll not at actual next_wake"); break; }
            }
        } else if (!legacy && event && at != clock.millis + 10) { ok = diff_error(ctx, "delivery/application must advance 10ms"); break; }
        if (fips_test_clock_advance(&clock, at - clock.millis) != FIPS_OK) { ok = false; break; }
        fips_instant now = clock_api.now(clock_api.ctx);
        rp_node *n = &nodes[who];
        if (!rk_state(ctx, FIELD(e, "state_before"), &n->machine)) { ok = false; break; }
        fips_rekey_outputs_clear(&n->out);
        uint8_t input[RP_PACKET_MAX] = {0}; size_t input_len = 0;
        diff_value *hex = FIELD(e, "input_hex");
        if (hex->type == D_BYTES) {
            if (hex->as.bytes.len > sizeof input) { ok = false; break; }
            input_len = hex->as.bytes.len; memcpy(input, hex->as.bytes.data, input_len);
        }
        if (legacy) {
            diff_value *seed = FIELD(e, "entropy_seed_hex");
            if (!seed || seed->type != D_UINT) { ok = diff_error(ctx, "missing legacy entropy checkpoint"); break; }
            entropy_state.state = seed->as.number;
        }
        rp_packet app = {0};
        bool application = !strcmp(label, "Seal(DataPacket) + DataSent");
        if (!strcmp(label, "start_now (test-hook)")) fips_rekey_start_now(&n->machine, now, &n->env, &n->out);
        else if (application) {
            if (!legacy) ok = rp_data(n, at, &app);
            else {
                diff_value *outputs = FIELD(e, "outputs");
                diff_value *v = COUNT(outputs) ? ITEM(outputs, COUNT(outputs) - 1) : NULL;
                if (!v || !FIELD(v, "plaintext_hex")) { ok = false; break; }
                uint8_t plain[14], seq[8];
                fips_writer seq_writer, plain_writer;
                fips_writer_init(&seq_writer, seq, sizeof seq);
                fips_writer_init(&plain_writer, plain, sizeof plain);
                diff_value *sequence = FIELD(v, "sequence");
                if (!sequence || sequence->type != D_UINT) { ok = false; break; }
                fips_rekey_sealed sealed = {0};
                ok = fips_rekey_seal(&n->machine, now, &sealed) == FIPS_REKEY_SEAL_OK &&
                    fips_writer_write_u64_le(&seq_writer, sequence->as.number, NULL) == FIPS_OK;
                uint32_t elapsed = (uint32_t)(sealed.elapsed.secs * 1000 + sealed.elapsed.nanos / 1000000);
                if (ok && fmp) {
                    uint8_t payload[9] = {FIPS_LINK_SessionDatagram};
                    memcpy(payload + 1, seq, sizeof seq);
                    ok = fips_fmp_prepend_inner_header(elapsed, payload, sizeof payload, &plain_writer, NULL) == FIPS_OK;
                } else if (ok) ok = fips_fsp_prepend_inner_header(elapsed, FIPS_SESSION_DataPacket, 0,
                    seq, sizeof seq, &plain_writer, NULL) == FIPS_OK;
                if (ok) ok = compare(ctx, BYTES(v, "plaintext_hex"), plain, fips_writer_len(&plain_writer)) &&
                    fips_session_encode_sealed(profile, &sealed, plain, fips_writer_len(&plain_writer),
                        app.bytes, sizeof app.bytes, &app.len) == FIPS_OK;
                fips_rekey_sealed_clear(&sealed);
                if (ok && !fmp) ok = fips_rekey_step(&n->machine, now,
                    (fips_rekey_input){.kind = FIPS_REKEY_IN_DataSent}, &n->env, &n->out) == FIPS_OK;
            }
        }
        else {
            fips_rekey_input in = {.bytes = input, .buf = input, .len = input_len,
                .carrier = nodes[1 - who].me, .route = 1};
            if (fmp && !strcmp(label, "Msg1")) {
                fips_fmp_ik_msg1 tok = {0};
                fips_fmp_inbound_decision d = {.kind = FIPS_FMP_IN_Rekey, .peer = n->remote};
                ok = fips_fmp_rekey_read_msg1(&n->local, input, input_len, &tok) &&
                    fips_fmp_rekey_inbound(&n->machine, now, &d, &tok, true, true, &n->env, &n->out) == FIPS_OK;
                fips_fmp_rekey_msg1_clear(&tok);
                goto compared_input;
            }
            if (fmp && (!strcmp(label, "Msg2") || !strcmp(label, "Frame"))) {
                ok = fips_fmp_rekey_receive(&n->machine, now, input, input_len, &n->env, &n->out) == FIPS_OK;
                goto compared_input;
            }
            if (!strcmp(label, "Setup")) in.kind = FIPS_REKEY_IN_Setup;
            else if (!strcmp(label, "Ack")) in.kind = FIPS_REKEY_IN_Reply;
            else if (!strcmp(label, "Msg3")) in.kind = FIPS_REKEY_IN_Final;
            else if (!strcmp(label, "Poll (next_wake)") || !strcmp(label, "Poll (legacy schedule)")) in.kind = FIPS_REKEY_IN_Poll;
            else if (!strcmp(label, "Frame")) {
                fips_fsp_encrypted_header h;
                if (!fips_fsp_encrypted_header_parse(input, input_len, &h)) { ok = false; break; }
                in.kind = FIPS_REKEY_IN_Frame;
                in.hint = (fips_rekey_frame_hint){h.counter, (h.flags & FIPS_FSP_FLAG_K) != 0, 0, FIPS_FRAMING_FSP_HEADER_SIZE};
            } else { ok = diff_error(ctx, "unhandled trace input"); break; }
            ok = fips_rekey_step(&n->machine, now, in, &n->env, &n->out) == FIPS_OK;
        }
compared_input:;
        diff_value *outputs = FIELD(e, "outputs");
        if (ok && COUNT(outputs) != n->out.count + (application ? 1u : 0u))
            ok = diff_error(ctx, "output count: expected %zu, C %zu", COUNT(outputs), n->out.count);
        size_t delivery = 0;
        for (size_t j = 0; ok && j < n->out.count; ++j) {
            fips_rekey_output *o = &n->out.items[j]; diff_value *v = ITEM(outputs, j);
            if (o->kind == FIPS_REKEY_OUT_Send) {
                const char *names[] = {"Msg1", "Msg2", "Setup", "Ack", "Msg3"};
                const char *routes[] = {"Established { fallback: false }", "Established { fallback: true }", "Arrival"};
                ok = rk_text(ctx, v, "kind", "Send") && rk_text(ctx, v, "message_kind", names[o->message_kind]) &&
                    rk_text(ctx, v, "route", routes[o->route]) && compare(ctx, BYTES(v, "bytes"), o->data.msg.bytes, o->data.msg.len);
            } else if (o->kind == FIPS_REKEY_OUT_Prompt) {
                fips_rekey_sealed_prompt prompt = {0}; rp_packet packet = {0};
                ok = fips_rekey_seal_prompt(&n->machine, now, o->tag, &prompt) == FIPS_REKEY_SEAL_OK;
                if (ok) ok = fips_session_encode_sealed(n->machine.profile, &prompt.sealed, prompt.plaintext.bytes,
                    prompt.plaintext.len, packet.bytes, sizeof packet.bytes, &packet.len) == FIPS_OK &&
                    rk_text(ctx, v, "kind", "Prompt") && rk_number(ctx, v, "tag", o->tag) &&
                    rk_number(ctx, v, "counter", prompt.sealed.counter) && rk_bool(ctx, v, "k", prompt.sealed.k) &&
                    compare(ctx, BYTES(v, "plaintext_hex"), prompt.plaintext.bytes, prompt.plaintext.len) &&
                    compare(ctx, BYTES(v, "bytes"), packet.bytes, packet.len);
                fips_rekey_sealed_prompt_clear(&prompt);
            } else if (o->kind == FIPS_REKEY_OUT_Release) {
                uint32_t index;
                ok = fips_rekey_index_lease_release(&o->lease, &index) && rk_text(ctx, v, "kind", "Release") && rk_number(ctx, v, "index", index);
                if (ok && legacy && fmp) ok = fips_index_allocator_free(&indices, fips_session_index_new(index)) == FIPS_INDEX_OK;
            } else {
                ok = rk_debug(ctx, v, o);
                if (ok && o->kind == FIPS_REKEY_OUT_Deliver) {
                    diff_value *records = FIELD(e, "authenticated_deliveries");
                    if (!records || delivery >= COUNT(records) || o->begin > o->end || o->end > input_len) { ok = false; break; }
                    diff_value *d = ITEM(records, delivery++), *range = FIELD(d, "range");
                    ok = rk_text(ctx, d, "slot", rk_slot(o->slot)) && rk_number(ctx, d, "tag", o->tag) &&
                        ITEM(range, 0)->as.number == o->begin && ITEM(range, 1)->as.number == o->end &&
                        compare(ctx, BYTES(d, "plaintext_hex"), input + o->begin, o->end - o->begin);
                }
            }
        }
        diff_value *deliveries = FIELD(e, "authenticated_deliveries");
        if (ok && deliveries && delivery != COUNT(deliveries)) ok = diff_error(ctx, "authenticated delivery count mismatch");
        if (ok && application && legacy) {
            diff_value *v = ITEM(outputs, n->out.count);
            ok = rk_text(ctx, v, "kind", "ApplicationFrame") && compare(ctx, BYTES(v, "bytes"), app.bytes, app.len);
            if (ok && fmp) {
                fips_fmp_encrypted_header h;
                ok = fips_fmp_encrypted_header_parse(app.bytes, app.len, &h) &&
                    rk_number(ctx, v, "counter", h.counter) && rk_bool(ctx, v, "k", (h.flags & 1) != 0);
            } else if (ok) {
                fips_fsp_encrypted_header h;
                ok = fips_fsp_encrypted_header_parse(app.bytes, app.len, &h) &&
                    rk_number(ctx, v, "counter", h.counter) && rk_bool(ctx, v, "k", (h.flags & FIPS_FSP_FLAG_K) != 0);
            }
        }
        if (ok && application && !legacy) {
            diff_value *v = ITEM(outputs, n->out.count);
            fips_fsp_encrypted_header h;
            ok = fips_fsp_encrypted_header_parse(app.bytes, app.len, &h) &&
                rk_text(ctx, v, "kind", "ApplicationFrame") && rk_number(ctx, v, "counter", h.counter) &&
                rk_bool(ctx, v, "k", (h.flags & FIPS_FSP_FLAG_K) != 0) && compare(ctx, BYTES(v, "bytes"), app.bytes, app.len);
            /* Independently trial-open without marking replay, to compare the
             * exported application plaintext as well as every ciphertext byte. */
            uint8_t plain[RP_PACKET_MAX]; size_t plain_len = 0;
            if (ok) ok = fips_noise_session_open_unmarked(fips_rekey_keys(&nodes[1 - who].machine, FIPS_REKEY_Current),
                h.counter, app.bytes, 12, app.bytes + 12, app.len - 12, plain, sizeof plain, &plain_len) == FIPS_NOISE_OK &&
                compare(ctx, BYTES(v, "plaintext_hex"), plain, plain_len);
        }
        if (ok) ok = rk_state(ctx, FIELD(e, "state_after"), &n->machine);
        fips_rekey_outputs_clear(&n->out);
        if (!ok && !ctx->error[0]) diff_error(ctx, "rekey replay mismatch");
    }
    for (size_t k = 0; ok && k < COUNT(c->final); ++k) ok = rk_state(ctx, ITEM(c->final, k), &nodes[k].machine);
    if (nodes && legacy && fmp && nodes[0].machine.live) {
        fips_rekey_outputs_clear(&nodes[0].out);
        fips_rekey_carry carry = {0};
        if (!fips_rekey_close_machine(&nodes[0].machine, FIPS_REKEY_CLOSE_Shutdown, &nodes[0].out, &carry)) ok = false;
        for (size_t j = 0; j < nodes[0].out.count; ++j) if (nodes[0].out.items[j].kind == FIPS_REKEY_OUT_Release) {
            uint32_t index;
            if (!fips_rekey_index_lease_release(&nodes[0].out.items[j].lease, &index) ||
                fips_index_allocator_free(&indices, fips_session_index_new(index)) != FIPS_INDEX_OK) ok = false;
        }
        if (fips_index_allocator_count(&indices)) ok = diff_error(ctx, "legacy replay leaked indices");
        nodes[0].allocated = 0;
    }
    if (nodes) { for (size_t k = 0; k < 2; ++k) rp_destroy(&nodes[k]); free(nodes); }
    if (ok) snprintf(ctx->id, sizeof ctx->id, "%s/%s/%s (%zu events)", legacy ? c->legacy_release : "rekey",
        fmp ? "fmp" : "fsp", legacy && c->legacy_initiates ? "legacy-initiates" : "current-initiates", COUNT(c->events));
    return ok ? CHECKED : FAILED;
}


static void report(diff_context *ctx, counts *n, result r, const char *owner)
{
    if (r == SKIPPED) {
        ++n->skipped;
        printf("SKIP %s: not implemented (owner %s)\n", ctx->id, owner);
    } else if (r == CHECKED) {
        ++n->checked; printf("PASS %s\n", ctx->id);
    } else {
        ++n->failed;
        printf("FAIL %s: %s\n", ctx->id, ctx->error[0] ? ctx->error : "byte comparison failed (see diagnostic)");
    }
}

int diff_dispatch(diff_context *ctx, const char *family, diff_value *cases)
{
    counts n = {0};
    if (!strcmp(family, "entropy") || !strcmp(family, "identity") || !strcmp(family, "codec")) {
        for (size_t i = 0; i < COUNT(cases); ++i) {
            diff_value *v = ITEM(cases, i);
            ctx->error[0] = '\0';
            snprintf(ctx->id, sizeof(ctx->id), "%s/%zu", family, i);
            if (!strcmp(family, "entropy")) {
                diff_value *keypair = FIELD(v, "keypair");
                entropy_case c = {UINT(v, "seed_hex"), BYTES(v, "stream"), BYTES(v, "chunked_stream"),
                    BYTES(keypair, "secret"), BYTES(keypair, "pubkey_compressed"), BYTES(keypair, "pubkey_xonly"),
                    FIELD(v, "splitmix64_words"), FIELD(v, "chunk_lengths"), FIELD(v, "chunks")};
                snprintf(ctx->id, sizeof(ctx->id), "entropy/%016" PRIx64, c.seed);
                report(ctx, &n, check_entropy(ctx, &c), NULL);
            } else if (!strcmp(family, "identity")) {
                diff_value *auth = FIELD(v, "auth");
                identity_case c = {UINT(v, "seed_hex"), UINT(auth, "timestamp"), UINT(auth, "aux_entropy_byte"),
                    BYTES(v, "secret_hex"), BYTES(v, "xonly_pubkey"), BYTES(v, "node_addr"), BYTES(v, "ipv6_bytes"),
                    BYTES(auth, "challenge"), BYTES(auth, "signature"), BYTES(auth, "verified_node_addr"),
                    TEXT(v, "npub"), TEXT(v, "nsec"), TEXT(v, "ipv6"), TEXT(auth, "wrong_challenge_error")};
                report(ctx, &n, check_identity(ctx, &c), "nostrc-u50yr.2");
            } else {
                diff_value *value = FIELD(v, "value"), *bytes = FIELD(v, "bytes");
                codec_case c = {TEXT(v, "kind"), value != NULL, value ? value->as.number : 0,
                    bytes ? bytes->as.bytes : (diff_bytes){0}, FIELD(v, "truncations"),
                    !strcmp(TEXT(v, "kind"), "cursor") ? v : NULL};
                snprintf(ctx->id, sizeof(ctx->id), "codec/%zu/%s", i, c.kind);
                report(ctx, &n, check_codec(ctx, &c), "nostrc-u50yr");
            }
        }
    } else if (!strcmp(family, "noise")) {
        diff_value *patterns = FIELD(cases, "patterns"), *replay = FIELD(cases, "replay_trace");
        for (size_t i = 0; i < COUNT(patterns); ++i) {
            diff_value *p = ITEM(patterns, i);
            noise_case c = {TEXT(p, "pattern"), UINT(p, "entropy_seed_hex"), BYTES(p, "startup_epoch_hex"),
                FIELD(p, "keypair_seeds"), FIELD(p, "messages"), FIELD(p, "steps"), FIELD(p, "post_handshake"), FIELD(p, "frames")};
            counts before = n;
            check_noise(ctx, &c, &n);
            printf("noise/%s: %zu checked, %zu skipped, %zu failed\n", c.pattern,
                n.checked - before.checked, n.skipped - before.skipped, n.failed - before.failed);
        }
        fips_replay_window window;
        fips_replay_window_init(&window);
        counts before = n;
        for (size_t i = 0; i < COUNT(replay); ++i) {
            ctx->error[0] = '\0';
            snprintf(ctx->id, sizeof(ctx->id), "noise/replay/%zu", i);
            report(ctx, &n, check_replay(ctx, ITEM(replay, i), UINT(cases, "replay_window_size"), &window), NULL);
        }
        printf("noise/replay: %zu checked, %zu skipped, %zu failed\n",
            n.checked - before.checked, n.skipped - before.skipped, n.failed - before.failed);
    } else if (!strcmp(family, "proto")) {
        diff_value *wire = FIELD(cases, "wire_cases");
        for (size_t i = 0; i < COUNT(wire); ++i) {
            diff_value *v = ITEM(wire, i);
            proto_case c = {TEXT(v, "name"), v, FIELD(cases, "common_inputs"), FIELD(cases, "constants")};
            snprintf(ctx->id, sizeof(ctx->id), "proto/%s", c.name);
            const char *owner = NULL;
            ctx->error[0] = '\0';
            result checked = check_proto(ctx, &c, &owner);
            report(ctx, &n, checked, owner);
        }
        const char *catalogs[] = {"link_catalog", "session_catalog", "constants"};
        for (size_t i = 0; i < 3; ++i) {
            proto_case c = {catalogs[i], FIELD(cases, catalogs[i]), FIELD(cases, "common_inputs"), FIELD(cases, "constants")};
            snprintf(ctx->id, sizeof(ctx->id), "proto/%s", c.name);
            const char *owner = NULL;
            ctx->error[0] = '\0';
            result checked = check_proto(ctx, &c, &owner);
            report(ctx, &n, checked, owner);
        }
    } else if (!strcmp(family, "rekey")) {
        rekey_case c = {TEXT(cases, "profile"), TEXT(cases, "initial_handshake"), TEXT(cases, "params_debug"), TEXT(cases, "clock"),
            UINT(cases, "entropy_seed_hex"), BYTES(cases, "startup_epoch_hex"), FIELD(cases, "keypair_seeds"),
            FIELD(cases, "initial_state"), FIELD(cases, "events"), FIELD(cases, "final_state"), NULL, false};
        snprintf(ctx->id, sizeof(ctx->id), "rekey/fsp-cycle (%zu events)", COUNT(c.events));
        report(ctx, &n, check_rekey(ctx, &c), "nostrc-f12v2.2");
    }
    if (!strcmp(family, "rekey-legacy")) {
        for (size_t i = 0; i < COUNT(cases); ++i) {
            diff_value *v = ITEM(cases, i);
            rekey_case c = {TEXT(v, "profile"), TEXT(v, "initial_handshake"), TEXT(v, "params_debug"), TEXT(v, "clock"),
                UINT(v, "entropy_seed_hex"), BYTES(v, "startup_epoch_hex"), FIELD(v, "keypair_seeds"),
                FIELD(v, "initial_state"), FIELD(v, "events"), FIELD(v, "final_state"), TEXT(v, "legacy_release"),
                FIELD(v, "legacy_initiates")->as.boolean};
            ctx->error[0] = '\0';
            report(ctx, &n, check_rekey(ctx, &c), "nostrc-f12v2.3");
        }
    }
    if (!n.checked && !n.skipped && !n.failed) {
        ++n.failed; puts("FAIL: empty family is not differential coverage");
    }
    printf("%s: %zu checked, %zu skipped, %zu failed\n", family, n.checked, n.skipped, n.failed);
    return n.failed ? 1 : 0;
}
