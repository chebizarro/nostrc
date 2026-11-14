#include "diff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compact schema notation: u unsigned, i signed, s text, b bool, o record,
 * a array, x bytes descriptor, h hex bytes, w hex u64; capitals allow null.
 * A '?' after the field name means absent is permitted (not arbitrary null).
 * These are exporter records, not parsers for Rust Debug output. */
static bool type(diff_context *ctx, diff_value *v, char t, const char *name)
{
    if (!v) return diff_error(ctx, "missing %s", name);
    if (t >= 'A' && t <= 'Z') {
        if (v->type == D_NULL) return true;
        t = (char)(t - 'A' + 'a');
    }
    if (t == 'h' || t == 'w') {
        if (v->type != D_TEXT) return diff_error(ctx, "%s must be hex text", name);
        char *text = v->as.text;
        if (t == 'h') {
            diff_bytes bytes;
            if (!diff_hex(ctx, text, &bytes)) return false;
            v->type = D_BYTES; v->as.bytes = bytes;
        } else {
            uint64_t word;
            if (!diff_word(ctx, text, &word)) return false;
            v->type = D_UINT; v->as.number = word;
        }
        free(text);
        return true;
    }
    bool ok = (t == 'u' && v->type == D_UINT) ||
        (t == 'i' && (v->type == D_INT || v->type == D_UINT)) ||
        (t == 's' && v->type == D_TEXT) || (t == 'b' && v->type == D_BOOL) ||
        (t == 'o' && v->type == D_OBJECT) || (t == 'a' && v->type == D_ARRAY) ||
        (t == 'x' && v->type == D_BYTES) || (t == 'n' && v->type == D_NULL);
    return ok || diff_error(ctx, "%s has wrong type (expected %c)", name, t);
}

static bool shape(diff_context *ctx, diff_value *v, const char *fields)
{
    if (!type(ctx, v, 'o', "record")) return false;
    while (*fields) {
        char key[80]; size_t n = 0;
        while (*fields && *fields != ':' && *fields != '?') {
            if (n + 1 >= sizeof(key)) return diff_error(ctx, "schema field too long");
            key[n++] = *fields++;
        }
        key[n] = '\0';
        bool optional = *fields == '?';
        if (optional) ++fields;
        if (*fields++ != ':') return diff_error(ctx, "invalid internal schema");
        char t = *fields++;
        diff_value *field = FIELD(v, key);
        if ((!optional || field) && !type(ctx, field, t, key)) return false;
        if (*fields == ' ') ++fields;
    }
    return true;
}

static bool elements(diff_context *ctx, diff_value *v, char t)
{
    if (!type(ctx, v, 'a', "array")) return false;
    for (size_t i = 0; i < COUNT(v); ++i)
        if (!type(ctx, ITEM(v, i), t, "array element")) return false;
    return true;
}

static bool error_shape(diff_context *ctx, diff_value *v)
{
    if (FIELD(v, "debug")) return shape(ctx, v, "debug:s");
    return shape(ctx, v, "variant:s expected:u got:u");
}

static bool entropy(diff_context *ctx, diff_value *v)
{
    if (!shape(ctx, v, "seed_hex:w stream:x splitmix64_words:a chunk_lengths:a chunks:a chunked_stream:x keypair:o") ||
        !elements(ctx, FIELD(v, "splitmix64_words"), 'w') || !elements(ctx, FIELD(v, "chunk_lengths"), 'u') ||
        !shape(ctx, FIELD(v, "keypair"), "secret:h pubkey_compressed:h pubkey_xonly:h")) return false;
    diff_value *chunks = FIELD(v, "chunks");
    for (size_t i = 0; i < COUNT(chunks); ++i)
        if (!shape(ctx, ITEM(chunks, i), "length:u bytes:x")) return false;
    return true;
}

static bool identity(diff_context *ctx, diff_value *v)
{
    if (!shape(ctx, v, "seed_hex:w secret_hex:h xonly_pubkey:h npub:s nsec:s node_addr:h ipv6_bytes:h ipv6:s auth:o")) return false;
    return shape(ctx, FIELD(v, "auth"), "challenge:x timestamp:u aux_entropy_byte:u signature:x verified_node_addr:h wrong_challenge_error:s");
}

static bool codec(diff_context *ctx, diff_value *v)
{
    if (!shape(ctx, v, "kind:s")) return false;
    const char *kind = TEXT(v, "kind");
    if (strcmp(kind, "cursor") == 0)
        return shape(ctx, v, "input_hex:h advance:u position:u remaining:u rest_hex:h require_3:s require_4:o") &&
            error_shape(ctx, FIELD(v, "require_4"));
    bool scalar = strcmp(kind, "u8") == 0 || strcmp(kind, "u16_le") == 0 ||
        strcmp(kind, "u32_le") == 0 || strcmp(kind, "u64_le") == 0;
    if (!scalar && strcmp(kind, "array") && strcmp(kind, "bytes") && strcmp(kind, "empty"))
        return diff_error(ctx, "unknown codec kind %s", kind);
    if (!shape(ctx, v, "bytes:x truncations:a") || (scalar && !shape(ctx, v, "value:u"))) return false;
    diff_value *truncations = FIELD(v, "truncations");
    for (size_t i = 0; i < COUNT(truncations); ++i) {
        diff_value *t = ITEM(truncations, i);
        if (!shape(ctx, t, "input_hex:h error:o") || !error_shape(ctx, FIELD(t, "error"))) return false;
    }
    return true;
}

static bool noise(diff_context *ctx, diff_value *v)
{
    if (!shape(ctx, v, "patterns:a replay_window_size:u replay_trace:a")) return false;
    diff_value *patterns = FIELD(v, "patterns"), *trace = FIELD(v, "replay_trace");
    for (size_t i = 0; i < COUNT(patterns); ++i) {
        diff_value *p = ITEM(patterns, i);
        if (!shape(ctx, p, "pattern:s entropy_seed_hex:w keypair_seeds:a startup_epoch_hex:h messages:a steps:a post_handshake:o frames:a") ||
            !elements(ctx, FIELD(p, "keypair_seeds"), 'u') ||
            !shape(ctx, FIELD(p, "post_handshake"), "initiator_h:h responder_h:h ck:n")) return false;
        diff_value *messages = FIELD(p, "messages"), *steps = FIELD(p, "steps"), *frames = FIELD(p, "frames");
        for (size_t j = 0; j < COUNT(messages); ++j)
            if (!shape(ctx, ITEM(messages, j), "number:u sender:s bytes:x")) return false;
        for (size_t j = 0; j < COUNT(steps); ++j) {
            diff_value *s = ITEM(steps, j);
            if (!shape(ctx, s, "event:s state:o")) return false;
            diff_value *state = FIELD(s, "state");
            if (!shape(ctx, state, "initiator:o responder:o") ||
                !shape(ctx, FIELD(state, "initiator"), "progress:s h:h") ||
                !shape(ctx, FIELD(state, "responder"), "progress:s h:h")) return false;
        }
        for (size_t j = 0; j < COUNT(frames); ++j)
            if (!shape(ctx, ITEM(frames, j), "direction:s counter:u plaintext_hex:h aad_hex:h sealed:x opened_hex:h replay_error:s corrupt_input_hex:h corrupt_counter:u authentication_error:s failed_counter_still_acceptable:b")) return false;
    }
    for (size_t i = 0; i < COUNT(trace); ++i)
        if (!shape(ctx, ITEM(trace, i), "counter:u accept:b highest_after:u")) return false;
    return true;
}

/* Wire input variants are named by proto.rs, never guessed from Debug text. */
static const struct { const char *name, *input; } wire_inputs[] = {
    {"coords", "addresses_hex:a"},
    {"filter-announce", "sequence:u filter_case:s"},
    {"tree-announce", "declaration_case:s ancestry_node_addrs:a"},
    {"lookup-request", "request_id:u target_seed:u origin_seed:u origin_coord:s ttl:u min_mtu:u"},
    {"lookup-response", "request_id:u target_seed:u target_coord:s proof_hex:h proof_signing_hex:h aux_entropy_byte:u"},
    {"sender-report", "interval_start_counter:u interval_end_counter:u interval_start_timestamp:u interval_end_timestamp:u interval_bytes_sent:u cumulative_packets_sent:u cumulative_bytes_sent:u"},
    {"receiver-report", "highest_counter:u cumulative_packets_recv:u cumulative_bytes_recv:u timestamp_echo:u dwell_time:u max_burst_loss:u mean_burst_loss:u jitter:u ecn_ce_count:u owd_trend:i burst_loss_count:u cumulative_reorder_count:u interval_packets_recv:u interval_bytes_recv:u"},
    {"path-mtu", "path_mtu:u"},
    {"path-probe", "probe_id:u path_id:u active:b aggregate:b len:u"},
    {"path-ack", "probe_id:u active:b path_id:u echo_path_id:u aggregate:b rx_time_us:u rx_frames:u rx_bytes:u len:u"},
    {"path-close", "path_id:u reason:s"}, {"disconnect", "reason:s"},
    {"fsp-setup", "src_coord:s dest_coord:s noise_case:s"},
    {"fsp-ack", "src_coord:s dest_coord:s noise_case:s"}, {"fsp-msg3", "noise_case:s"},
    {"session-datagram", "src_seed:u dest_seed:u ttl:u path_mtu:u payload_case:s"}
};

static bool wire_case(diff_context *ctx, diff_value *v)
{
    if (!shape(ctx, v, "name:s")) return false;
    const char *name = TEXT(v, "name");
    if (strcmp(name, "parent-declaration") == 0)
        return shape(ctx, v, "node_addr:h parent_addr:h sequence:u timestamp:u signing_bytes:x signature_hex:h aux_entropy_byte:u");
    if (!shape(ctx, v, "bytes:x")) return false;
    if (strcmp(name, "coords-empty") == 0)
        return shape(ctx, v, "required_error:o optional:n") && error_shape(ctx, FIELD(v, "required_error"));
    if (strcmp(name, "bloom") == 0)
        return shape(ctx, v, "inserted_node_addrs:a hash_count:u num_bits:u ones:u") && elements(ctx, FIELD(v, "inserted_node_addrs"), 'h');
    if (strcmp(name, "fmp-msg-1") == 0 || strcmp(name, "fmp-msg-2") == 0)
        return shape(ctx, v, "noise_case:s sender_idx:u receiver_idx:U");
    if (strcmp(name, "fsp-encrypted") == 0 || strcmp(name, "fmp-encrypted") == 0)
        return shape(ctx, v, "counter:u flags:u inner_hex:h header_hex:h") &&
            (strcmp(name, "fsp-encrypted") == 0 || shape(ctx, v, "receiver_idx:u"));
    const char *input_name = name;
    if (strcmp(name, "session-sender-report") == 0) input_name = "sender-report";
    if (strcmp(name, "session-receiver-report") == 0) input_name = "receiver-report";
    for (size_t i = 0; i < sizeof(wire_inputs) / sizeof(wire_inputs[0]); ++i) {
        if (strcmp(input_name, wire_inputs[i].name)) continue;
        if (!shape(ctx, v, "input:o") || !shape(ctx, FIELD(v, "input"), wire_inputs[i].input)) return false;
        if (strcmp(name, "coords") == 0)
            return shape(ctx, v, "consumed:u decoded_depth:u") && elements(ctx, FIELD(FIELD(v, "input"), "addresses_hex"), 'h');
        if (strcmp(name, "tree-announce") == 0 && !elements(ctx, FIELD(FIELD(v, "input"), "ancestry_node_addrs"), 'h')) return false;
        return shape(ctx, v, "decode_offset:u decoded_debug:s empty_input_error:o") && error_shape(ctx, FIELD(v, "empty_input_error"));
    }
    return diff_error(ctx, "unknown proto wire case %s", name);
}

static bool proto(diff_context *ctx, diff_value *v)
{
    if (!shape(ctx, v, "common_inputs:o constants:o link_catalog:a session_catalog:a wire_cases:a") ||
        !shape(ctx, FIELD(v, "common_inputs"), "identity_seeds:a src_coord_debug:s dst_coord_debug:s") ||
        !elements(ctx, FIELD(FIELD(v, "common_inputs"), "identity_seeds"), 'u') ||
        !shape(ctx, FIELD(v, "constants"), "FIPS_OVERHEAD:u SESSION_DATAGRAM_HEADER_SIZE:u FMP_FRAME_OVERHEAD:u FMP_ESTABLISHED_HEADER_SIZE:u FMP_MSG1_WIRE_SIZE:u FMP_MSG2_WIRE_SIZE:u FSP_HEADER_SIZE:u FSP_INNER_HEADER_SIZE:u FSP_PORT_HEADER_SIZE:u payload_limit_1280:u")) return false;
    const char *catalogs[] = {"link_catalog", "session_catalog"};
    for (size_t c = 0; c < 2; ++c) {
        diff_value *list = FIELD(v, catalogs[c]);
        for (size_t i = 0; i < COUNT(list); ++i)
            if (!shape(ctx, ITEM(list, i), "name:s value:u")) return false;
    }
    diff_value *cases = FIELD(v, "wire_cases");
    for (size_t i = 0; i < COUNT(cases); ++i)
        if (!wire_case(ctx, ITEM(cases, i))) return false;
    return true;
}

static bool state(diff_context *ctx, diff_value *v)
{
    if (!shape(ctx, v, "identity:s peer_epoch_hex:H last_rx_ms:U link_since_ms:u cycle:s current:O previous:O pending:O candidates:u next_wake_ms:U k:b sent:u")) return false;
    const char *epochs[] = {"current", "previous", "pending"};
    for (size_t i = 0; i < 3; ++i) {
        diff_value *e = FIELD(v, epochs[i]);
        if (e->type != D_NULL && !shape(ctx, e, "tag:u k:b role:s started_ms:u base_ms:U recv:s")) return false;
    }
    return true;
}

static bool rekey(diff_context *ctx, diff_value *v)
{
    if (!shape(ctx, v, "profile:s keypair_seeds:a startup_epoch_hex:h entropy_seed_hex:w initial_handshake:s params_debug:s clock:s initial_state:a events:a final_state:a") ||
        !elements(ctx, FIELD(v, "keypair_seeds"), 'u')) return false;
    const char *states[] = {"initial_state", "final_state"};
    for (size_t s = 0; s < 2; ++s) {
        diff_value *list = FIELD(v, states[s]);
        for (size_t i = 0; i < COUNT(list); ++i) if (!state(ctx, ITEM(list, i))) return false;
    }
    diff_value *events = FIELD(v, "events");
    for (size_t i = 0; i < COUNT(events); ++i) {
        diff_value *e = ITEM(events, i);
        if (!shape(ctx, e, "event:u node:u at_ms:u input:s input_hex:H state_before:o outputs:a state_after:o authenticated_deliveries?:a") ||
            !state(ctx, FIELD(e, "state_before")) || !state(ctx, FIELD(e, "state_after"))) return false;
        diff_value *outputs = FIELD(e, "outputs");
        for (size_t j = 0; j < COUNT(outputs); ++j) {
            diff_value *o = ITEM(outputs, j);
            if (FIELD(o, "debug")) { if (!shape(ctx, o, "debug:s")) return false; continue; }
            if (!shape(ctx, o, "kind:s")) return false;
            const char *kind = TEXT(o, "kind");
            if (strcmp(kind, "Send") == 0) { if (!shape(ctx, o, "message_kind:s route:s bytes:x")) return false; }
            else if (strcmp(kind, "Release") == 0) { if (!shape(ctx, o, "index:n")) return false; }
            else if (strcmp(kind, "Prompt") == 0 || strcmp(kind, "ApplicationFrame") == 0) {
                if (!shape(ctx, o, "counter:u k:b plaintext_hex:h bytes:x") ||
                    (strcmp(kind, "Prompt") == 0 && !shape(ctx, o, "tag:u"))) return false;
            } else return diff_error(ctx, "unknown rekey output %s", kind);
        }
        diff_value *deliveries = FIELD(e, "authenticated_deliveries");
        if (deliveries) for (size_t j = 0; j < COUNT(deliveries); ++j) {
            diff_value *d = ITEM(deliveries, j);
            if (!shape(ctx, d, "slot:s tag:u range:a plaintext_hex:h") || !elements(ctx, FIELD(d, "range"), 'u')) return false;
        }
    }
    return true;
}

bool diff_schema(diff_context *ctx, const char *family, diff_value *cases)
{
    if (strcmp(family, "noise") == 0) return noise(ctx, cases);
    if (strcmp(family, "proto") == 0) return proto(ctx, cases);
    if (strcmp(family, "rekey") == 0) return rekey(ctx, cases);
    if (!type(ctx, cases, 'a', "cases")) return false;
    for (size_t i = 0; i < COUNT(cases); ++i) {
        diff_value *v = ITEM(cases, i);
        bool ok = strcmp(family, "entropy") == 0 ? entropy(ctx, v) :
            strcmp(family, "identity") == 0 ? identity(ctx, v) : codec(ctx, v);
        if (!ok) return false;
    }
    return true;
}
