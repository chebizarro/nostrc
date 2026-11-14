#include "diff.h"
#include "../fakes/host.h"
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum result { CHECKED, FAILED, SKIPPED } result;
typedef struct counts { size_t checked, failed, skipped; } counts;

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
typedef struct proto_case {
    const char *name;
    diff_value *record, *common_inputs, *constants;
} proto_case;
typedef struct rekey_case {
    const char *profile, *initial_handshake, *params_debug, *clock;
    uint64_t entropy_seed;
    diff_bytes epoch;
    diff_value *keypair_seeds, *initial, *events, *final;
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
static result check_identity(const identity_case *c) { (void)c; return SKIPPED; }
static result check_codec(const codec_case *c) { (void)c; return SKIPPED; }
static result check_noise(const noise_case *c) { (void)c; return SKIPPED; }
static result check_replay(const diff_value *step, uint64_t window_size)
{ (void)step; (void)window_size; return SKIPPED; }
static result check_proto(const proto_case *c) { (void)c; return SKIPPED; }
static result check_rekey(const rekey_case *c) { (void)c; return SKIPPED; }

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
                report(ctx, &n, check_identity(&c), "nostrc-u50yr");
            } else {
                diff_value *value = FIELD(v, "value"), *bytes = FIELD(v, "bytes");
                codec_case c = {TEXT(v, "kind"), value != NULL, value ? value->as.number : 0,
                    bytes ? bytes->as.bytes : (diff_bytes){0}, FIELD(v, "truncations"),
                    !strcmp(TEXT(v, "kind"), "cursor") ? v : NULL};
                snprintf(ctx->id, sizeof(ctx->id), "codec/%zu/%s", i, c.kind);
                report(ctx, &n, check_codec(&c), "nostrc-u50yr");
            }
        }
    } else if (!strcmp(family, "noise")) {
        diff_value *patterns = FIELD(cases, "patterns"), *replay = FIELD(cases, "replay_trace");
        for (size_t i = 0; i < COUNT(patterns); ++i) {
            diff_value *p = ITEM(patterns, i);
            noise_case c = {TEXT(p, "pattern"), UINT(p, "entropy_seed_hex"), BYTES(p, "startup_epoch_hex"),
                FIELD(p, "keypair_seeds"), FIELD(p, "messages"), FIELD(p, "steps"), FIELD(p, "post_handshake"), FIELD(p, "frames")};
            snprintf(ctx->id, sizeof(ctx->id), "noise/%s (messages/steps/frames)", c.pattern);
            report(ctx, &n, check_noise(&c), "nostrc-pwo3h");
        }
        for (size_t i = 0; i < COUNT(replay); ++i) {
            snprintf(ctx->id, sizeof(ctx->id), "noise/replay/%zu", i);
            report(ctx, &n, check_replay(ITEM(replay, i), UINT(cases, "replay_window_size")), "nostrc-pwo3h");
        }
    } else if (!strcmp(family, "proto")) {
        diff_value *wire = FIELD(cases, "wire_cases");
        for (size_t i = 0; i < COUNT(wire); ++i) {
            diff_value *v = ITEM(wire, i);
            proto_case c = {TEXT(v, "name"), v, FIELD(cases, "common_inputs"), FIELD(cases, "constants")};
            snprintf(ctx->id, sizeof(ctx->id), "proto/%s", c.name);
            report(ctx, &n, check_proto(&c), "nostrc-82g9n");
        }
        const char *catalogs[] = {"link_catalog", "session_catalog", "constants"};
        for (size_t i = 0; i < 3; ++i) {
            proto_case c = {catalogs[i], FIELD(cases, catalogs[i]), FIELD(cases, "common_inputs"), FIELD(cases, "constants")};
            snprintf(ctx->id, sizeof(ctx->id), "proto/%s", c.name);
            report(ctx, &n, check_proto(&c), "nostrc-82g9n");
        }
    } else if (!strcmp(family, "rekey")) {
        rekey_case c = {TEXT(cases, "profile"), TEXT(cases, "initial_handshake"), TEXT(cases, "params_debug"), TEXT(cases, "clock"),
            UINT(cases, "entropy_seed_hex"), BYTES(cases, "startup_epoch_hex"), FIELD(cases, "keypair_seeds"),
            FIELD(cases, "initial_state"), FIELD(cases, "events"), FIELD(cases, "final_state")};
        snprintf(ctx->id, sizeof(ctx->id), "rekey/fsp-cycle (%zu events)", COUNT(c.events));
        report(ctx, &n, check_rekey(&c), "nostrc-f12v2");
    }
    if (!n.checked && !n.skipped && !n.failed) {
        ++n.failed; puts("FAIL: empty family is not differential coverage");
    }
    printf("%s: %zu checked, %zu skipped, %zu failed\n", family, n.checked, n.skipped, n.failed);
    return n.failed ? 1 : 0;
}
