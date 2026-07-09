/*
 * libmarmot - MDK interoperability test suite
 *
 * Validates libmarmot against test vectors captured from the MDK
 * (Rust reference implementation). Vectors are loaded from JSON
 * files in the vectors/mdk/ directory.
 *
 * When no MDK vectors are found, the test suite generates and
 * validates self-consistency vectors to exercise the same code paths.
 *
 * Interop scenarios:
 *   1. KeyPackage TLS serialization round-trip
 *   2. GroupData extension serialization round-trip
 *   3. Group creation → Welcome → Join lifecycle
 *   4. Message encrypt → decrypt round-trip
 *   5. Exporter secret derivation consistency
 *   6. NIP-44 conversation key from exporter secret
 *
 * SPDX-License-Identifier: MIT
 */

#include <marmot/marmot.h>
#include "marmot-internal.h"
#include "mls/mls-internal.h"
#include "mls/mls_key_schedule.h"
#include "mls/mls_key_package.h"
#include "mls/mls_framing.h"
#include "mls/mls_welcome.h"
#include "mls/mls_group.h"
#include "mdk_vector_loader.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sodium.h>

#define TEST(name) do { printf("  %-55s", #name); name(); printf("PASS\n"); } while(0)

/* ──────────────────────────────────────────────────────────────────────── */

static void
hex_encode(char *out, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        int written = snprintf(out + 2 * i, 3, "%02x", data[i]);
        assert(written == 2 && "hex_encode formatting error");
    }
    out[2 * len] = '\0';
}

/* ══════════════════════════════════════════════════════════════════════════
 * Protocol vector helpers (for protocol-vectors.json)
 * ══════════════════════════════════════════════════════════════════════════ */

static char *
proto_read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

static bool
hex_decode(uint8_t *out, const char *hex, size_t out_len)
{
    for (size_t i = 0; i < out_len; i++) {
        unsigned int b;
        if (sscanf(hex + i * 2, "%2x", &b) != 1) return false;
        out[i] = (uint8_t)b;
    }
    return true;
}

static uint8_t *
proto_base64_decode(const char *b64, size_t b64_len, size_t *out_len)
{
    size_t max_bin = (b64_len / 4) * 3 + 3;
    uint8_t *out = malloc(max_bin);
    if (!out) return NULL;
    size_t bin_len = 0;
    if (sodium_base642bin(out, max_bin, b64, b64_len,
                          NULL, &bin_len, NULL,
                          sodium_base64_VARIANT_ORIGINAL) != 0) {
        free(out);
        return NULL;
    }
    *out_len = bin_len;
    return out;
}

/* Extract a JSON string value for a given key.
 * Returns a malloc'd copy of the string value, or NULL.
 * Only finds the FIRST occurrence after 'start'. */
static char *
json_extract_string(const char *start, const char *end, const char *key)
{
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = start;
    while (pos < end) {
        pos = strstr(pos, pattern);
        if (!pos || pos >= end) return NULL;
        pos += strlen(pattern);
        /* skip whitespace and colon */
        while (pos < end && (*pos == ' ' || *pos == ':' || *pos == '\t' || *pos == '\n' || *pos == '\r')) pos++;
        if (pos >= end || *pos != '"') return NULL;
        pos++; /* skip opening quote */
        const char *val_start = pos;
        while (pos < end && *pos != '"') pos++;
        if (pos >= end) return NULL;
        size_t len = (size_t)(pos - val_start);
        char *result = malloc(len + 1);
        memcpy(result, val_start, len);
        result[len] = '\0';
        return result;
    }
    return NULL;
}

/* Find matching brace end from a '{' */
static const char *
json_find_object_end(const char *start)
{
    int depth = 0;
    const char *p = start;
    while (*p) {
        if (*p == '{') depth++;
        else if (*p == '}') { depth--; if (depth == 0) return p; }
        p++;
    }
    return NULL;
}

/* Find the start of a JSON object for a given key */
static const char *
json_find_object(const char *start, const char *end, const char *key)
{
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(start, pattern);
    if (!pos || pos >= end) return NULL;
    pos += strlen(pattern);
    while (pos < end && *pos != '{') pos++;
    return (pos < end) ? pos : NULL;
}

/* Find the start of a JSON array for a given key */
static const char *
json_find_array(const char *start, const char *end, const char *key)
{
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(start, pattern);
    if (!pos || pos >= end) return NULL;
    pos += strlen(pattern);
    while (pos < end && *pos != '[') pos++;
    return (pos < end) ? pos : NULL;
}

/* ══════════════════════════════════════════════════════════════════════════
 * MDK Protocol Vector Tests (protocol-vectors.json)
 *
 * These validate libmarmot's ability to parse and process data
 * generated by the MDK Rust reference implementation.
 * ══════════════════════════════════════════════════════════════════════════ */

static const char *
find_protocol_vectors(const char *vector_dir)
{
    static char path[512];
    snprintf(path, sizeof(path), "%s/protocol-vectors.json", vector_dir);
    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
        return path;
    return NULL;
}

/*
 * Test: Decode base64 event content and verify it matches the hex serialization.
 * This confirms the event_content_base64 ↔ serialized_key_package_hex relationship.
 */
static void
test_mdk_protocol_kp_base64_hex_match(const char *json, size_t json_len)
{
    const char *kp_section = json_find_object(json, json + json_len, "key_package");
    assert(kp_section && "key_package section not found");
    const char *kp_end = json_find_object_end(kp_section);
    assert(kp_end);

    const char *cases = json_find_array(kp_section, kp_end, "cases");
    assert(cases && "cases array not found");

    int case_count = 0;
    const char *pos = cases + 1;
    while (pos < kp_end) {
        pos = strchr(pos, '{');
        if (!pos || pos >= kp_end) break;
        const char *case_end = json_find_object_end(pos);
        if (!case_end) break;

        char *content_b64 = json_extract_string(pos, case_end, "event_content_base64");
        char *expected_hex = json_extract_string(pos, case_end, "serialized_key_package_hex");
        assert(content_b64 && expected_hex);

        /* Decode base64 → raw bytes */
        size_t kp_len = 0;
        uint8_t *kp_data = proto_base64_decode(content_b64, strlen(content_b64), &kp_len);
        assert(kp_data && kp_len > 0 && "base64 decode failed");

        /* Encode to hex and compare */
        char *actual_hex = malloc(kp_len * 2 + 1);
        hex_encode(actual_hex, kp_data, kp_len);
        assert(strcmp(actual_hex, expected_hex) == 0 &&
               "base64 content doesn't match serialized hex");

        case_count++;
        free(actual_hex);
        free(kp_data);
        free(content_b64);
        free(expected_hex);
        pos = case_end + 1;
    }

    assert(case_count == 3);
    printf("PASS (%d cases)\n", case_count);
}

/*
 * Test: Compute KeyPackageRef directly from MDK's serialized bytes using
 * RefHash("MLS 1.0 KeyPackage Reference", serialized_kp).
 * This validates our RefHash implementation without requiring TLS deserialization.
 */
static void
test_mdk_protocol_kp_ref(const char *json, size_t json_len)
{
    const char *kp_section = json_find_object(json, json + json_len, "key_package");
    const char *kp_end = json_find_object_end(kp_section);
    const char *cases = json_find_array(kp_section, kp_end, "cases");

    int verified = 0;
    const char *pos = cases + 1;
    while (pos < kp_end) {
        pos = strchr(pos, '{');
        if (!pos || pos >= kp_end) break;
        const char *case_end = json_find_object_end(pos);
        if (!case_end) break;

        char *kp_hex = json_extract_string(pos, case_end, "serialized_key_package_hex");
        char *ref_hex = json_extract_string(pos, case_end, "key_package_ref");
        assert(kp_hex && ref_hex);

        /* The ref in protocol-vectors.json is prefixed with "20" (the hash length byte).
         * The actual 32-byte hash starts after that prefix. */
        const char *ref_hex_data = ref_hex;
        if (strlen(ref_hex) == 66 && ref_hex[0] == '2' && ref_hex[1] == '0') {
            ref_hex_data = ref_hex + 2; /* skip the length prefix */
        }
        assert(strlen(ref_hex_data) == 64);

        uint8_t expected_ref[32];
        assert(hex_decode(expected_ref, ref_hex_data, 32));

        /* Decode the hex KP bytes */
        size_t kp_len = strlen(kp_hex) / 2;
        uint8_t *kp_data = malloc(kp_len);
        assert(hex_decode(kp_data, kp_hex, kp_len));

        /* Compute RefHash directly from raw bytes */
        uint8_t computed_ref[32];
        assert(mls_crypto_ref_hash(computed_ref, "MLS 1.0 KeyPackage Reference",
                                    kp_data, kp_len) == 0);

        assert(memcmp(computed_ref, expected_ref, 32) == 0 &&
               "KeyPackageRef mismatch with MDK");

        verified++;
        free(kp_data);
        free(kp_hex);
        free(ref_hex);
        pos = case_end + 1;
    }

    assert(verified == 3);
    printf("PASS (%d refs verified)\n", verified);
}

/*
 * Test: Extract nostr pubkey from MDK KeyPackage raw bytes.
 *
 * MDK/OpenMLS uses 1-byte length prefixes for HPKE keys (opaque<0..255>)
 * while our implementation uses 2-byte prefixes per RFC 9420 §2.
 * This test manually walks the MDK wire format to extract the credential.
 *
 * MDK KeyPackage layout (ciphersuite 0x0001):
 *   [0..1]   version (uint16 = 0x0001)
 *   [2..3]   cipher_suite (uint16 = 0x0001)
 *   [4]      init_key_len (uint8 = 0x20)
 *   [5..36]  init_key (32 bytes, X25519)
 *   -- LeafNode --
 *   [37]     encryption_key_len (uint8 = 0x20)
 *   [38..69] encryption_key (32 bytes)
 *   [70]     signature_key_len (uint8 = 0x20)
 *   [71..102] signature_key (32 bytes)
 *   [103..104] credential_type (uint16 = 0x0001, basic)
 *   [105]    identity_len (uint8 = 0x20)
 *   [106..137] identity (32 bytes = nostr pubkey)
 */
static void
test_mdk_protocol_kp_credential(const char *json, size_t json_len)
{
    const char *kp_section = json_find_object(json, json + json_len, "key_package");
    const char *kp_end = json_find_object_end(kp_section);
    const char *cases = json_find_array(kp_section, kp_end, "cases");

    int verified = 0;
    const char *pos = cases + 1;
    while (pos < kp_end) {
        pos = strchr(pos, '{');
        if (!pos || pos >= kp_end) break;
        const char *case_end = json_find_object_end(pos);
        if (!case_end) break;

        char *kp_hex = json_extract_string(pos, case_end, "serialized_key_package_hex");
        char *pubkey_hex = json_extract_string(pos, case_end, "nostr_pubkey");
        assert(kp_hex && pubkey_hex);
        assert(strlen(pubkey_hex) == 64);

        uint8_t expected_pubkey[32];
        assert(hex_decode(expected_pubkey, pubkey_hex, 32));

        /* Decode raw bytes */
        size_t kp_len = strlen(kp_hex) / 2;
        uint8_t *kp_data = malloc(kp_len);
        assert(hex_decode(kp_data, kp_hex, kp_len));
        assert(kp_len > 138); /* minimum size for credential at offset 106..137 */

        /* Verify structure: version=1, cs=1 */
        assert(kp_data[0] == 0x00 && kp_data[1] == 0x01); /* version */
        assert(kp_data[2] == 0x00 && kp_data[3] == 0x01); /* ciphersuite */

        /* Walk MDK format: 1-byte prefix for HPKE keys */
        assert(kp_data[4] == 0x20);   /* init_key_len = 32 */
        assert(kp_data[37] == 0x20);  /* encryption_key_len = 32 */
        assert(kp_data[70] == 0x20);  /* signature_key_len = 32 */

        /* Credential: type=basic (0x0001), identity_len=32 */
        assert(kp_data[103] == 0x00 && kp_data[104] == 0x01); /* basic credential */
        assert(kp_data[105] == 0x20); /* identity_len = 32 */

        /* Extract identity and compare to expected nostr pubkey */
        assert(memcmp(&kp_data[106], expected_pubkey, 32) == 0 &&
               "Credential identity does not match nostr pubkey");

        verified++;
        free(kp_data);
        free(kp_hex);
        free(pubkey_hex);
        pos = case_end + 1;
    }

    assert(verified == 3);
    printf("PASS (%d credentials verified)\n", verified);
}

/*
 * Test: Verify MDK KP version and ciphersuite match Marmot constants.
 * This validates header parsing without full TLS deserialization.
 */
static void
test_mdk_protocol_kp_header(const char *json, size_t json_len)
{
    const char *kp_section = json_find_object(json, json + json_len, "key_package");
    const char *kp_end = json_find_object_end(kp_section);
    const char *cases = json_find_array(kp_section, kp_end, "cases");

    int verified = 0;
    const char *pos = cases + 1;
    while (pos < kp_end) {
        pos = strchr(pos, '{');
        if (!pos || pos >= kp_end) break;
        const char *case_end = json_find_object_end(pos);
        if (!case_end) break;

        char *kp_hex = json_extract_string(pos, case_end, "serialized_key_package_hex");
        assert(kp_hex && strlen(kp_hex) >= 8);

        uint8_t header[4];
        assert(hex_decode(header, kp_hex, 4));

        uint16_t version = ((uint16_t)header[0] << 8) | header[1];
        uint16_t cs = ((uint16_t)header[2] << 8) | header[3];

        assert(version == 1 && "version must be mls10");
        assert(cs == MARMOT_CIPHERSUITE && "ciphersuite must be 0x0001");

        verified++;
        free(kp_hex);
        pos = case_end + 1;
    }

    assert(verified == 3);
    printf("PASS (%d headers verified)\n", verified);
}

/*
 * Test: Validate event tag structure matches expected Marmot protocol tags.
 */
static void
test_mdk_protocol_event_tags(const char *json, size_t json_len)
{
    const char *kp_section = json_find_object(json, json + json_len, "key_package");
    const char *kp_end = json_find_object_end(kp_section);
    const char *cases = json_find_array(kp_section, kp_end, "cases");

    int verified = 0;
    const char *pos = cases + 1;
    while (pos < kp_end) {
        pos = strchr(pos, '{');
        if (!pos || pos >= kp_end) break;
        const char *case_end = json_find_object_end(pos);
        if (!case_end) break;

        /* Find event_tags array and check for required tag keys */
        const char *tags = json_find_array(pos, case_end, "event_tags");
        assert(tags && "event_tags not found");

        /* Check required tags exist in the tags text */
        assert(strstr(tags, "\"mls_protocol_version\"") && "missing mls_protocol_version tag");
        assert(strstr(tags, "\"1.0\"") && "missing version value 1.0");
        assert(strstr(tags, "\"mls_ciphersuite\"") && "missing mls_ciphersuite tag");
        assert(strstr(tags, "\"0x0001\"") && "ciphersuite should be 0x0001");
        assert(strstr(tags, "\"mls_extensions\"") && "missing mls_extensions tag");
        assert(strstr(tags, "\"0x000a\"") && "missing required_capabilities extension");
        assert(strstr(tags, "\"0xf2ee\"") && "missing marmot extension type");
        assert(strstr(tags, "\"relays\"") && "missing relays tag");
        assert(strstr(tags, "\"wss://relay.example.com\"") && "missing relay URL");
        assert(strstr(tags, "\"encoding\"") && "missing encoding tag");
        assert(strstr(tags, "\"base64\"") && "encoding should be base64");
        assert(strstr(tags, "\"d\"") && "missing d tag");
        assert(strstr(tags, "\"i\"") && "missing i (ref) tag");
        assert(strstr(tags, "\"client\"") && "missing client tag");

        verified++;
        pos = case_end + 1;
    }

    assert(verified == 3);
    printf("PASS (%d tag sets validated)\n", verified);
}

/*
 * Test: Verify "i" tag value matches computed KeyPackageRef.
 * Computes ref directly from raw serialized bytes (no TLS deserialization needed).
 */
static void
test_mdk_protocol_kp_ref_tag(const char *json, size_t json_len)
{
    const char *kp_section = json_find_object(json, json + json_len, "key_package");
    const char *kp_end = json_find_object_end(kp_section);
    const char *cases = json_find_array(kp_section, kp_end, "cases");

    int verified = 0;
    const char *pos = cases + 1;
    while (pos < kp_end) {
        pos = strchr(pos, '{');
        if (!pos || pos >= kp_end) break;
        const char *case_end = json_find_object_end(pos);
        if (!case_end) break;

        char *kp_hex = json_extract_string(pos, case_end, "serialized_key_package_hex");
        assert(kp_hex);

        /* Compute ref from raw bytes */
        size_t kp_len = strlen(kp_hex) / 2;
        uint8_t *kp_data = malloc(kp_len);
        assert(hex_decode(kp_data, kp_hex, kp_len));

        uint8_t computed_ref[32];
        assert(mls_crypto_ref_hash(computed_ref, "MLS 1.0 KeyPackage Reference",
                                    kp_data, kp_len) == 0);
        char ref_hex_str[65];
        hex_encode(ref_hex_str, computed_ref, 32);

        /* Find the "i" tag value in event_tags.
         * The tags JSON looks like: ["i", "<hex>"] */
        const char *tags = json_find_array(pos, case_end, "event_tags");
        assert(tags);
        const char *i_tag = strstr(tags, "\"i\"");
        assert(i_tag && "missing i tag");
        /* Next string after "i" is the ref value */
        const char *q1 = strchr(i_tag + 3, '"');
        assert(q1);
        q1++;
        const char *q2 = strchr(q1, '"');
        assert(q2);
        size_t tag_len = (size_t)(q2 - q1);
        assert(tag_len == 64 && "i tag value should be 64 hex chars");

        assert(strncmp(ref_hex_str, q1, 64) == 0 &&
               "i tag does not match computed KeyPackageRef");

        verified++;
        free(kp_data);
        free(kp_hex);
        pos = case_end + 1;
    }

    assert(verified == 3);
    printf("PASS (%d ref tags verified)\n", verified);
}

/*
 * Test: Validate group lifecycle metadata from protocol vectors.
 */
static void
test_mdk_protocol_lifecycle_metadata(const char *json, size_t json_len)
{
    const char *gl = json_find_object(json, json + json_len, "group_lifecycle");
    assert(gl && "group_lifecycle section not found");
    const char *gl_end = json_find_object_end(gl);
    assert(gl_end);

    /* Verify alice and bob pubkeys are present and 64 hex chars */
    char *alice = json_extract_string(gl, gl_end, "alice_pubkey");
    char *bob = json_extract_string(gl, gl_end, "bob_pubkey");
    assert(alice && strlen(alice) == 64);
    assert(bob && strlen(bob) == 64);
    assert(strcmp(alice, bob) != 0 && "alice and bob should be different");

    /* Find steps array */
    const char *steps = json_find_array(gl, gl_end, "steps");
    assert(steps);

    /* Count steps */
    int step_count = 0;
    const char *sp = steps + 1;
    while (sp < gl_end) {
        sp = strchr(sp, '{');
        if (!sp || sp >= gl_end) break;
        const char *se = json_find_object_end(sp);
        if (!se) break;

        char *name = json_extract_string(sp, se, "name");
        assert(name);

        /* Validate specific steps */
        if (strcmp(name, "bob_key_package") == 0) {
            /* Should reference kind 30443 */
            assert(strstr(sp, "30443") && "bob_key_package should have kind 30443");
        } else if (strcmp(name, "alice_creates_group") == 0) {
            /* Should have group IDs and member_count */
            char *ngid = json_extract_string(sp, se, "nostr_group_id");
            char *mgid = json_extract_string(sp, se, "mls_group_id");
            assert(ngid && strlen(ngid) == 64 && "nostr_group_id should be 64 hex");
            assert(mgid && strlen(mgid) == 32 && "mls_group_id should be 32 hex");
            assert(strstr(sp, "\"member_count\""));
            free(ngid);
            free(mgid);
        } else if (strcmp(name, "alice_sends_message") == 0) {
            char *content = json_extract_string(sp, se, "message_content");
            assert(content && strcmp(content, "Hello from Alice!") == 0);
            /* Verify message pubkey matches alice */
            char *mpk = json_extract_string(sp, se, "message_pubkey");
            assert(mpk && strcmp(mpk, alice) == 0 &&
                   "message_pubkey should be alice's");
            free(content);
            free(mpk);
        } else if (strcmp(name, "bob_decrypts_message") == 0) {
            char *content = json_extract_string(sp, se, "message_content");
            assert(content && strcmp(content, "Hello from Alice!") == 0 &&
                   "decrypted content must match original");
            char *mpk = json_extract_string(sp, se, "message_pubkey");
            assert(mpk && strcmp(mpk, alice) == 0 &&
                   "decrypted message should attribute to alice");
            free(content);
            free(mpk);
        }

        step_count++;
        free(name);
        sp = se + 1;
    }

    assert(step_count == 6 && "expected 6 lifecycle steps");
    free(alice);
    free(bob);
    printf("PASS (6 lifecycle steps validated)\n");
}

/*
 * Test: Extract bob's pubkey from lifecycle's bob_key_package step
 * by walking the MDK wire format directly.
 */
static void
test_mdk_protocol_lifecycle_kp(const char *json, size_t json_len)
{
    const char *gl = json_find_object(json, json + json_len, "group_lifecycle");
    const char *gl_end = json_find_object_end(gl);

    char *bob_hex = json_extract_string(gl, gl_end, "bob_pubkey");
    assert(bob_hex && strlen(bob_hex) == 64);
    uint8_t bob_pubkey[32];
    assert(hex_decode(bob_pubkey, bob_hex, 32));

    /* Find bob_key_package step */
    const char *steps = json_find_array(gl, gl_end, "steps");
    const char *bkp = strstr(steps, "\"bob_key_package\"");
    assert(bkp);
    /* Back up to find the enclosing object */
    const char *obj = bkp;
    while (obj > steps && *obj != '{') obj--;
    const char *obj_end = json_find_object_end(obj);

    char *content_b64 = json_extract_string(obj, obj_end, "event_content");
    assert(content_b64 && "bob_key_package event_content not found");

    size_t kp_len = 0;
    uint8_t *kp_data = proto_base64_decode(content_b64, strlen(content_b64), &kp_len);
    assert(kp_data && kp_len > 0);

    /* Walk MDK wire format to extract credential identity */
    assert(kp_len > 138);
    assert(kp_data[0] == 0x00 && kp_data[1] == 0x01); /* version */
    assert(kp_data[2] == 0x00 && kp_data[3] == 0x01); /* ciphersuite */
    assert(kp_data[4] == 0x20);   /* init_key_len */
    assert(kp_data[37] == 0x20);  /* encryption_key_len */
    assert(kp_data[70] == 0x20);  /* signature_key_len */
    assert(kp_data[103] == 0x00 && kp_data[104] == 0x01); /* basic credential */
    assert(kp_data[105] == 0x20); /* identity_len */

    /* Verify credential is bob's pubkey */
    assert(memcmp(&kp_data[106], bob_pubkey, 32) == 0 &&
           "lifecycle KP credential should be bob's pubkey");

    free(kp_data);
    free(content_b64);
    free(bob_hex);
    printf("PASS\n");
}

/*
 * Test: Deserialize MDK KeyPackages through our full TLS deserializer.
 * This validates that our Capabilities struct handles all 5 vectors
 * (versions, ciphersuites, extensions, proposals, credentials).
 */
static void
test_mdk_protocol_kp_deserialize(const char *json, size_t json_len)
{
    const char *kp_section = json_find_object(json, json + json_len, "key_package");
    const char *kp_end = json_find_object_end(kp_section);
    const char *cases = json_find_array(kp_section, kp_end, "cases");

    int deserialized = 0;
    const char *pos = cases + 1;
    while (pos < kp_end) {
        pos = strchr(pos, '{');
        if (!pos || pos >= kp_end) break;
        const char *case_end = json_find_object_end(pos);
        if (!case_end) break;

        char *hex = json_extract_string(pos, case_end, "serialized_key_package_hex");
        if (!hex) { pos = case_end + 1; continue; }

        size_t hex_len = strlen(hex);
        size_t kp_len = hex_len / 2;
        uint8_t *kp_data = malloc(kp_len);
        assert(kp_data);
        assert(hex_decode(kp_data, hex, kp_len));

        /* Deserialize through our TLS parser */
        MlsKeyPackage kp;
        MlsTlsReader reader;
        mls_tls_reader_init(&reader, kp_data, kp_len);
        int rc = mls_key_package_deserialize(&reader, &kp);
        assert(rc == 0 && "MDK KeyPackage should deserialize successfully");

        /* All bytes consumed */
        assert(mls_tls_reader_remaining(&reader) == 0 &&
               "all MDK KP bytes should be consumed");

        /* Basic field validation */
        assert(kp.version == 1);
        assert(kp.cipher_suite == MARMOT_CIPHERSUITE);
        assert(kp.leaf_node.credential_type == MLS_CREDENTIAL_BASIC);
        assert(kp.leaf_node.credential_identity_len == 32);

        /* Capabilities should have at least version and ciphersuite vectors */
        assert(kp.leaf_node.version_count >= 1);
        assert(kp.leaf_node.ciphersuite_count >= 1);

        /* Verify re-serialization round-trips correctly */
        {
            MlsTlsBuf rebuf;
            assert(mls_tls_buf_init(&rebuf, 512) == 0);
            assert(mls_key_package_serialize(&kp, &rebuf) == 0);
            assert(rebuf.len == kp_len &&
                   "re-serialized KP should be same length");
            assert(memcmp(rebuf.data, kp_data, kp_len) == 0 &&
                   "re-serialized KP should match original bytes");
            mls_tls_buf_free(&rebuf);
        }

        mls_key_package_clear(&kp);
        free(kp_data);
        free(hex);
        deserialized++;
        pos = case_end + 1;
    }

    assert(deserialized == 3);
    printf("PASS (%d KPs deserialized)\n", deserialized);
}

/* Run all protocol vector tests */
static void
run_protocol_vector_tests(const char *vector_dir)
{
    const char *path = find_protocol_vectors(vector_dir);
    if (!path) {
        printf("  Protocol vectors not found — skipping\n");
        return;
    }

    size_t json_len = 0;
    char *json = proto_read_file(path, &json_len);
    if (!json) {
        printf("  Failed to read protocol-vectors.json — skipping\n");
        return;
    }

    printf("\n─ MDK Protocol Vector Validation ─\n");

    printf("  %-55s", "protocol: KP base64/hex match");
    test_mdk_protocol_kp_base64_hex_match(json, json_len);

    printf("  %-55s", "protocol: KP ref matches MDK");
    test_mdk_protocol_kp_ref(json, json_len);

    printf("  %-55s", "protocol: KP credential matches nostr pubkey");
    test_mdk_protocol_kp_credential(json, json_len);

    printf("  %-55s", "protocol: KP header (version + ciphersuite)");
    test_mdk_protocol_kp_header(json, json_len);

    printf("  %-55s", "protocol: event tag structure");
    test_mdk_protocol_event_tags(json, json_len);

    printf("  %-55s", "protocol: KP ref matches i tag");
    test_mdk_protocol_kp_ref_tag(json, json_len);

    printf("  %-55s", "protocol: lifecycle metadata");
    test_mdk_protocol_lifecycle_metadata(json, json_len);

    printf("  %-55s", "protocol: lifecycle KP belongs to bob");
    test_mdk_protocol_lifecycle_kp(json, json_len);

    printf("  %-55s", "protocol: MDK KP deserializes via TLS parser");
    test_mdk_protocol_kp_deserialize(json, json_len);

    free(json);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Self-consistency interop vectors
 *
 * These run when MDK vectors aren't available, exercising the same
 * serialization and protocol paths that would be tested with MDK vectors.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── 1. KeyPackage TLS round-trip ─────────────────────────────────────── */

static void
test_key_package_serialize_roundtrip(void)
{
    /* Generate a key package using the actual API */
    uint8_t identity[32]; /* Nostr pubkey as credential identity */
    randombytes_buf(identity, 32);

    MlsKeyPackage kp;
    MlsKeyPackagePrivate priv;
    memset(&kp, 0, sizeof(kp));
    memset(&priv, 0, sizeof(priv));

    assert(mls_key_package_create(&kp, &priv,
                                   identity, sizeof(identity),
                                   NULL, 0) == 0);

    assert(kp.version == 1);  /* mls10 */
    assert(kp.cipher_suite == MARMOT_CIPHERSUITE);

    /* Serialize to TLS wire format */
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 256) == 0);
    assert(mls_key_package_serialize(&kp, &buf) == 0);
    assert(buf.len > 0);

    /* Deserialize from the serialized bytes */
    MlsKeyPackage kp2;
    memset(&kp2, 0, sizeof(kp2));
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, buf.data, buf.len);
    assert(mls_key_package_deserialize(&reader, &kp2) == 0);

    /* Validate fields match */
    assert(kp2.version == kp.version);
    assert(kp2.cipher_suite == kp.cipher_suite);
    assert(memcmp(kp2.init_key, kp.init_key, MLS_KEM_PK_LEN) == 0);

    /* Validate the signature */
    assert(mls_key_package_validate(&kp2) == 0);

    /* Compute KeyPackageRef */
    uint8_t ref1[32], ref2[32];
    assert(mls_key_package_ref(&kp, ref1) == 0);
    assert(mls_key_package_ref(&kp2, ref2) == 0);
    assert(memcmp(ref1, ref2, 32) == 0);

    mls_key_package_clear(&kp);
    mls_key_package_clear(&kp2);
    mls_key_package_private_clear(&priv);
    mls_tls_buf_free(&buf);
}

/* ── 2. GroupData extension round-trip ─────────────────────────────────── */

static void
test_group_data_extension_roundtrip(void)
{
    MarmotGroupDataExtension *ext = marmot_group_data_extension_new();
    assert(ext != NULL);

    ext->version = MARMOT_EXTENSION_VERSION;
    randombytes_buf(ext->nostr_group_id, 32);
    ext->name = strdup("Interop Test Group");
    ext->description = strdup("Testing round-trip serialization");

    /* Add 2 admins */
    ext->admin_count = 2;
    ext->admins = calloc(2 * 32, 1);
    randombytes_buf(ext->admins, 32);
    randombytes_buf(ext->admins + 32, 32);

    /* Add 2 relays */
    ext->relay_count = 2;
    ext->relays = calloc(2, sizeof(char *));
    ext->relays[0] = strdup("wss://relay1.example.com");
    ext->relays[1] = strdup("wss://relay2.example.com");

    /* Serialize */
    uint8_t *ser_data = NULL;
    size_t ser_len = 0;
    assert(marmot_group_data_extension_serialize(ext, &ser_data, &ser_len) == 0);
    assert(ser_data != NULL && ser_len > 0);

    /* Deserialize */
    MarmotGroupDataExtension *parsed =
        marmot_group_data_extension_deserialize(ser_data, ser_len);
    assert(parsed != NULL);

    /* Validate */
    assert(parsed->version == MARMOT_EXTENSION_VERSION);
    assert(memcmp(parsed->nostr_group_id, ext->nostr_group_id, 32) == 0);
    assert(strcmp(parsed->name, "Interop Test Group") == 0);
    assert(strcmp(parsed->description, "Testing round-trip serialization") == 0);
    assert(parsed->admin_count == 2);
    assert(memcmp(parsed->admins[0], ext->admins[0], 32) == 0);
    assert(memcmp(parsed->admins[1], ext->admins[1], 32) == 0);
    assert(parsed->relay_count == 2);
    assert(strcmp(parsed->relays[0], "wss://relay1.example.com") == 0);
    assert(strcmp(parsed->relays[1], "wss://relay2.example.com") == 0);

    /* Re-serialize and compare bytes (must be identical) */
    uint8_t *ser2_data = NULL;
    size_t ser2_len = 0;
    assert(marmot_group_data_extension_serialize(parsed, &ser2_data, &ser2_len) == 0);
    assert(ser2_len == ser_len);
    assert(memcmp(ser2_data, ser_data, ser_len) == 0);

    free(ser_data);
    free(ser2_data);
    marmot_group_data_extension_free(ext);
    marmot_group_data_extension_free(parsed);
}

/* ── 3. Extension with optional fields ────────────────────────────────── */

static void
test_group_data_extension_with_image(void)
{
    MarmotGroupDataExtension *ext = marmot_group_data_extension_new();
    ext->version = MARMOT_EXTENSION_VERSION;
    randombytes_buf(ext->nostr_group_id, 32);
    ext->name = strdup("Image Group");

    /* Add optional image fields */
    ext->image_hash = malloc(32);
    if (!ext->image_hash) {
        marmot_group_data_extension_free(ext);
        assert(0 && "malloc failed");
    }
    randombytes_buf(ext->image_hash, 32);
    ext->image_key = malloc(32);
    if (!ext->image_key) {
        marmot_group_data_extension_free(ext);
        assert(0 && "malloc failed");
    }
    randombytes_buf(ext->image_key, 32);
    ext->image_nonce = malloc(12);
    if (!ext->image_nonce) {
        marmot_group_data_extension_free(ext);
        assert(0 && "malloc failed");
    }
    randombytes_buf(ext->image_nonce, 12);

    uint8_t *ser_data = NULL;
    size_t ser_len = 0;
    assert(marmot_group_data_extension_serialize(ext, &ser_data, &ser_len) == 0);

    MarmotGroupDataExtension *parsed =
        marmot_group_data_extension_deserialize(ser_data, ser_len);
    assert(parsed != NULL);
    assert(parsed->image_hash != NULL);
    assert(memcmp(parsed->image_hash, ext->image_hash, 32) == 0);
    assert(parsed->image_key != NULL);
    assert(memcmp(parsed->image_key, ext->image_key, 32) == 0);
    assert(parsed->image_nonce != NULL);
    assert(memcmp(parsed->image_nonce, ext->image_nonce, 12) == 0);

    free(ser_data);
    marmot_group_data_extension_free(ext);
    marmot_group_data_extension_free(parsed);
}

/* ── 4. Exporter secret derivation ────────────────────────────────────── */

static void
test_exporter_nip44_consistency(void)
{
    /*
     * Verify that the Marmot NIP-44 conversation key derivation is
     * consistent: given the same exporter_secret and context,
     * we always get the same key.
     *
     * MIP-03: conversation_key = MLS-Exporter("marmot-nip44-key", group_id, 32)
     */
    uint8_t exporter_secret[32];
    randombytes_buf(exporter_secret, 32);

    uint8_t group_id[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t key1[32], key2[32];

    assert(mls_exporter(exporter_secret, "marmot-nip44-key",
                         group_id, sizeof(group_id), key1, 32) == 0);
    assert(mls_exporter(exporter_secret, "marmot-nip44-key",
                         group_id, sizeof(group_id), key2, 32) == 0);
    assert(memcmp(key1, key2, 32) == 0);

    /* Different group_id → different key */
    uint8_t other_gid[] = {0x05, 0x06, 0x07, 0x08};
    uint8_t key3[32];
    assert(mls_exporter(exporter_secret, "marmot-nip44-key",
                         other_gid, sizeof(other_gid), key3, 32) == 0);
    assert(memcmp(key1, key3, 32) != 0);
}

/* ── 5. Media key derivation consistency ──────────────────────────────── */

static void
test_exporter_media_key_consistency(void)
{
    /*
     * MIP-04: media_key = MLS-Exporter("marmot-media-key", "", 32)
     *
     * Actually media.c uses HMAC-SHA256 directly, but the label is the same.
     * Verify the derivation is consistent.
     */
    uint8_t exporter_secret[32];
    randombytes_buf(exporter_secret, 32);

    uint8_t key1[32], key2[32];
    assert(mls_exporter(exporter_secret, "marmot-media-key",
                         NULL, 0, key1, 32) == 0);
    assert(mls_exporter(exporter_secret, "marmot-media-key",
                         NULL, 0, key2, 32) == 0);
    assert(memcmp(key1, key2, 32) == 0);
}

/* ── 6. Full key schedule → exporter secret chain ─────────────────────── */

static void
test_full_epoch_to_exporter(void)
{
    /*
     * Complete chain: init_secret → key_schedule → exporter_secret → nip44_key
     *
     * This validates the full path that a message encryption key takes.
     */
    uint8_t commit_secret[32];
    randombytes_buf(commit_secret, 32);

    uint8_t group_id[] = {0xAA, 0xBB};
    uint8_t tree_hash[32], transcript_hash[32];
    randombytes_buf(tree_hash, 32);
    randombytes_buf(transcript_hash, 32);

    uint8_t *gc_data = NULL;
    size_t gc_len = 0;
    assert(mls_group_context_serialize(group_id, sizeof(group_id), 0,
                                        tree_hash, transcript_hash,
                                        NULL, 0, &gc_data, &gc_len) == 0);

    /* Derive epoch secrets */
    MlsEpochSecrets secrets;
    assert(mls_key_schedule_derive(NULL, commit_secret,
                                    gc_data, gc_len, NULL, &secrets) == 0);

    /* Derive NIP-44 conversation key from exporter_secret */
    uint8_t nip44_key[32];
    assert(mls_exporter(secrets.exporter_secret, "marmot-nip44-key",
                         group_id, sizeof(group_id), nip44_key, 32) == 0);

    /* Key must be non-zero */
    uint8_t zero[32] = {0};
    assert(memcmp(nip44_key, zero, 32) != 0);

    /* Run again with same inputs — must produce same key */
    MlsEpochSecrets secrets2;
    assert(mls_key_schedule_derive(NULL, commit_secret,
                                    gc_data, gc_len, NULL, &secrets2) == 0);
    uint8_t nip44_key2[32];
    assert(mls_exporter(secrets2.exporter_secret, "marmot-nip44-key",
                         group_id, sizeof(group_id), nip44_key2, 32) == 0);
    assert(memcmp(nip44_key, nip44_key2, 32) == 0);

    free(gc_data);
}

/* ── 7. Cross-epoch key isolation ─────────────────────────────────────── */

static void
test_cross_epoch_key_isolation(void)
{
    /*
     * Keys derived from different epochs must be completely different,
     * even with the same commit_secret. This is because GroupContext
     * includes the epoch number.
     */
    uint8_t commit_secret[32];
    randombytes_buf(commit_secret, 32);

    uint8_t group_id[] = {0xCC};
    uint8_t tree_hash[32], transcript_hash[32];
    memset(tree_hash, 0, 32);
    memset(transcript_hash, 0, 32);

    uint8_t nip44_keys[3][32];

    for (uint64_t epoch = 0; epoch < 3; epoch++) {
        uint8_t *gc_data = NULL;
        size_t gc_len = 0;
        assert(mls_group_context_serialize(group_id, 1, epoch,
                                            tree_hash, transcript_hash,
                                            NULL, 0, &gc_data, &gc_len) == 0);

        MlsEpochSecrets secrets;
        assert(mls_key_schedule_derive(NULL, commit_secret,
                                        gc_data, gc_len, NULL, &secrets) == 0);

        assert(mls_exporter(secrets.exporter_secret, "marmot-nip44-key",
                             group_id, 1, nip44_keys[epoch], 32) == 0);
        free(gc_data);
    }

    /* All three keys must be different */
    assert(memcmp(nip44_keys[0], nip44_keys[1], 32) != 0);
    assert(memcmp(nip44_keys[1], nip44_keys[2], 32) != 0);
    assert(memcmp(nip44_keys[0], nip44_keys[2], 32) != 0);
}

/* ── 8. Nostr event kind validation ───────────────────────────────────── */

static void
test_nostr_event_kinds(void)
{
    /* Verify that the event kind constants match the Marmot spec */
    assert(MARMOT_KIND_KEY_PACKAGE == 443);
    assert(MARMOT_KIND_WELCOME == 444);
    assert(MARMOT_KIND_GROUP_MESSAGE == 445);

    /* Extension type */
    assert(MARMOT_EXTENSION_TYPE == 0xF2EE);

    /* Ciphersuite */
    assert(MARMOT_CIPHERSUITE == 0x0001);
}

/* ── 9. Self-vector dump (for future MDK comparison) ──────────────────── */

static void
test_dump_self_vectors(void)
{
    /*
     * Generate a set of test vectors from our implementation.
     * These can be compared against MDK output for cross-validation.
     *
     * We don't write to disk here — just verify the format is correct.
     */

    /* Generate a key package using proper API */
    uint8_t identity[32];
    randombytes_buf(identity, 32);

    MlsKeyPackage kp;
    MlsKeyPackagePrivate priv;
    memset(&kp, 0, sizeof(kp));
    memset(&priv, 0, sizeof(priv));
    assert(mls_key_package_create(&kp, &priv,
                                   identity, sizeof(identity),
                                   NULL, 0) == 0);

    /* Serialize to TLS wire format */
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 256) == 0);
    assert(mls_key_package_serialize(&kp, &buf) == 0);

    /* Compute ref */
    uint8_t kp_ref[32];
    assert(mls_key_package_ref(&kp, kp_ref) == 0);

    /* Encode to hex for future comparison */
    char *kp_hex = malloc(buf.len * 2 + 1);
    if (!kp_hex) {
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&priv);
        mls_tls_buf_free(&buf);
        assert(0 && "malloc failed");
    }
    hex_encode(kp_hex, buf.data, buf.len);
    assert(strlen(kp_hex) == buf.len * 2);

    char ref_hex[65];
    hex_encode(ref_hex, kp_ref, 32);
    assert(strlen(ref_hex) == 64);

    free(kp_hex);
    mls_key_package_clear(&kp);
    mls_key_package_private_clear(&priv);
    mls_tls_buf_free(&buf);
}

/* ══════════════════════════════════════════════════════════════════════════
 * MDK Vector Validation
 * ══════════════════════════════════════════════════════════════════════════ */

static size_t g_mdk_asserted = 0;
static size_t g_mdk_deferred = 0;

static void
assert_bytes_eq(const char *label, const uint8_t *actual,
                const uint8_t *expected, size_t len)
{
    if (memcmp(actual, expected, len) != 0) {
        fprintf(stderr, "\nFAIL %s\n  got:      ", label);
        for (size_t i = 0; i < len; i++) fprintf(stderr, "%02x", actual[i]);
        fprintf(stderr, "\n  expected: ");
        for (size_t i = 0; i < len; i++) fprintf(stderr, "%02x", expected[i]);
        fprintf(stderr, "\n");
        assert(0 && "MDK vector byte mismatch");
    }
    g_mdk_asserted++;
}

static void
assert_mls_message_roundtrip(const char *label, const uint8_t *data, size_t len)
{
    MlsMLSMessage msg;
    MlsTlsReader reader;
    memset(&msg, 0, sizeof(msg));
    mls_tls_reader_init(&reader, data, len);
    int rc = mls_message_deserialize(&reader, &msg);
    assert(rc == 0 && "MDK MLSMessage must deserialize");
    assert(mls_tls_reader_remaining(&reader) == 0 && "MDK MLSMessage must consume all bytes");

    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, len + 64) == 0);
    assert(mls_message_serialize(&msg, &buf) == 0 && "MDK MLSMessage must reserialize");
    assert(buf.len == len && "MDK MLSMessage roundtrip length mismatch");
    assert_bytes_eq(label, buf.data, data, len);
    mls_tls_buf_free(&buf);
    mls_message_clear(&msg);
}

static void
assert_welcome_roundtrip(const char *label, const uint8_t *data, size_t len)
{
    MlsWelcome w;
    MlsTlsReader reader;
    memset(&w, 0, sizeof(w));
    mls_tls_reader_init(&reader, data, len);
    assert(mls_welcome_deserialize(&reader, &w) == 0 && "MDK Welcome must deserialize");
    assert(mls_tls_reader_remaining(&reader) == 0 && "MDK Welcome must consume all bytes");
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, len + 64) == 0);
    assert(mls_welcome_serialize(&w, &buf) == 0 && "MDK Welcome must reserialize");
    assert(buf.len == len && "MDK Welcome roundtrip length mismatch");
    assert_bytes_eq(label, buf.data, data, len);
    mls_tls_buf_free(&buf);
    mls_welcome_clear(&w);
}

static int
authenticated_content_confirmed_len(const uint8_t *data, size_t len,
                                    size_t *out_len)
{
    if (!data || !out_len) return -1;
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, data, len);

    uint16_t wire_format = 0;
    if (mls_tls_read_u16(&reader, &wire_format) != 0) return -1;
    if (wire_format != MLS_WIRE_FORMAT_PUBLIC_MESSAGE &&
        wire_format != MLS_WIRE_FORMAT_PRIVATE_MESSAGE)
        return -1;

    uint8_t *tmp = NULL;
    size_t tmp_len = 0;
    if (mls_tls_read_opaque8(&reader, &tmp, &tmp_len) != 0) return -1;
    free(tmp);
    uint64_t epoch = 0;
    (void)epoch;
    if (mls_tls_read_u64(&reader, &epoch) != 0) return -1;
    uint8_t sender_type = 0;
    if (mls_tls_read_u8(&reader, &sender_type) != 0) return -1;
    if (sender_type == MLS_SENDER_TYPE_MEMBER) {
        uint32_t leaf = 0;
        if (mls_tls_read_u32(&reader, &leaf) != 0) return -1;
    }
    if (mls_tls_read_opaque32(&reader, &tmp, &tmp_len) != 0) return -1;
    free(tmp);
    tmp = NULL;

    uint8_t content_type = 0;
    if (mls_tls_read_u8(&reader, &content_type) != 0) return -1;
    switch (content_type) {
    case MLS_CONTENT_TYPE_APPLICATION:
        if (mls_tls_read_opaque32(&reader, &tmp, &tmp_len) != 0) return -1;
        free(tmp);
        break;
    case MLS_CONTENT_TYPE_COMMIT: {
        /* Commit = proposals<V> || optional<UpdatePath>.  The MDK
         * transcript vector carries a no-path commit; for the transcript
         * boundary we only need to scan to the auth signature. */
        if (mls_tls_read_opaque32(&reader, &tmp, &tmp_len) != 0) return -1;
        free(tmp);
        uint8_t has_path = 0;
        if (mls_tls_read_u8(&reader, &has_path) != 0) return -1;
        if (has_path) return -1;
        break;
    }
    default:
        return -1;
    }

    uint8_t *sig = NULL;
    size_t sig_len = 0;
    if (mls_tls_read_opaque16(&reader, &sig, &sig_len) != 0) return -1;
    free(sig);

    /* RFC 9420 confirmed transcript hashes AuthenticatedContentTBM,
     * which ends after FramedContentAuthData.signature.  For commits the
     * confirmation_tag is authenticated later into the interim hash and is
     * intentionally not part of the confirmed transcript input. */
    *out_len = reader.pos;

    if (content_type == MLS_CONTENT_TYPE_COMMIT) {
        uint8_t *tag = NULL;
        size_t tag_len = 0;
        if (mls_tls_read_opaque32(&reader, &tag, &tag_len) != 0) return -1;
        free(tag);
    }

    return mls_tls_reader_done(&reader) ? 0 : -1;
}

static void
assert_key_package_roundtrip(const char *label, const uint8_t *data, size_t len)
{
    MlsKeyPackage kp;
    MlsTlsReader reader;
    memset(&kp, 0, sizeof(kp));
    mls_tls_reader_init(&reader, data, len);
    assert(mls_key_package_deserialize(&reader, &kp) == 0 && "MDK KeyPackage must deserialize");
    assert(mls_tls_reader_remaining(&reader) == 0 && "MDK KeyPackage must consume all bytes");
    assert(mls_key_package_validate(&kp) == 0 && "MDK KeyPackage must validate");
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, len + 64) == 0);
    assert(mls_key_package_serialize(&kp, &buf) == 0 && "MDK KeyPackage must reserialize");
    assert(buf.len == len && "MDK KeyPackage roundtrip length mismatch");
    assert_bytes_eq(label, buf.data, data, len);
    mls_tls_buf_free(&buf);
    mls_key_package_clear(&kp);
}

static void
test_mdk_crypto_basics_vectors(const char *vector_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/crypto-basics.json", vector_dir);
    MdkCryptoBasicsVector vectors[MAX_CRYPTO_TESTS];
    size_t count = 0;
    assert(mdk_load_crypto_basics_vectors(path, vectors, &count, MAX_CRYPTO_TESTS) == 0);
    assert(count > 0 && "crypto-basics file exists but yielded zero ciphersuite-1 cases");

    for (size_t i = 0; i < count; i++) {
        uint8_t out[32];
        assert(mls_crypto_expand_with_label(out, vectors[i].expand_length,
                                            vectors[i].expand_secret,
                                            vectors[i].expand_label,
                                            vectors[i].expand_context, 32) == 0);
        assert_bytes_eq("crypto-basics.expand_with_label", out,
                        vectors[i].expand_out, vectors[i].expand_length);

        assert(mls_crypto_derive_secret(out, vectors[i].derive_secret,
                                        vectors[i].derive_label) == 0);
        assert_bytes_eq("crypto-basics.derive_secret", out,
                        vectors[i].derive_out, 32);
    }
    printf("PASS (%zu ciphersuite-1 cases asserted)\n", count);
}

static void
test_mdk_key_schedule_vectors(const char *vector_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/key-schedule.json", vector_dir);
    MdkKeyScheduleVector vectors[10];
    size_t count = 0;
    assert(mdk_load_key_schedule_vectors(path, vectors, &count, 10) == 0);
    assert(count > 0 && "key-schedule file exists but yielded zero ciphersuite-1 cases");

    for (size_t i = 0; i < count; i++) {
        uint8_t init[MLS_HASH_LEN];
        memcpy(init, vectors[i].initial_init_secret, MLS_HASH_LEN);
        for (size_t e = 0; e < vectors[i].epoch_count; e++) {
            MdkEpochVector *epoch = &vectors[i].epochs[e];
            MlsEpochSecrets sec;
            assert(mls_key_schedule_derive(init, epoch->commit_secret,
                                           epoch->group_context, epoch->group_context_len,
                                           epoch->psk_secret, &sec) == 0);
            assert_bytes_eq("key-schedule.joiner_secret", sec.joiner_secret, epoch->joiner_secret, MLS_HASH_LEN);
            assert_bytes_eq("key-schedule.welcome_secret", sec.welcome_secret, epoch->welcome_secret, MLS_HASH_LEN);
            assert_bytes_eq("key-schedule.sender_data_secret", sec.sender_data_secret, epoch->sender_data_secret, MLS_HASH_LEN);
            assert_bytes_eq("key-schedule.encryption_secret", sec.encryption_secret, epoch->encryption_secret, MLS_HASH_LEN);
            assert_bytes_eq("key-schedule.exporter_secret", sec.exporter_secret, epoch->exporter_secret, MLS_HASH_LEN);
            assert_bytes_eq("key-schedule.external_secret", sec.external_secret, epoch->external_secret, MLS_HASH_LEN);
            assert_bytes_eq("key-schedule.confirmation_key", sec.confirmation_key, epoch->confirmation_key, MLS_HASH_LEN);
            assert_bytes_eq("key-schedule.membership_key", sec.membership_key, epoch->membership_key, MLS_HASH_LEN);
            assert_bytes_eq("key-schedule.resumption_psk", sec.resumption_psk, epoch->resumption_psk, MLS_HASH_LEN);
            assert_bytes_eq("key-schedule.epoch_authenticator", sec.epoch_authenticator, epoch->epoch_authenticator, MLS_HASH_LEN);
            assert_bytes_eq("key-schedule.init_secret", sec.init_secret, epoch->init_secret, MLS_HASH_LEN);

            if (epoch->exporter_length > 0) {
                uint8_t exported[64];
                assert(epoch->exporter_length <= sizeof(exported));
                assert(mls_exporter(sec.exporter_secret, epoch->exporter_label,
                                    epoch->exporter_context, 32,
                                    exported, epoch->exporter_length) == 0);
                assert_bytes_eq("key-schedule.exporter", exported,
                                epoch->exporter_secret_out, epoch->exporter_length);
            }
            memcpy(init, sec.init_secret, MLS_HASH_LEN);
        }
    }
    printf("PASS (%zu ciphersuite-1 chains asserted)\n", count);
}

static void
test_mdk_tree_math_vectors(const char *vector_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/tree-math.json", vector_dir);
    MdkTreeMathVector vectors[50];
    size_t count = 0;
    assert(mdk_load_tree_math_vectors(path, vectors, &count, 50) == 0);
    assert(count > 0 && "tree-math file exists but yielded zero cases");

    for (size_t v = 0; v < count; v++) {
        assert(mls_tree_node_width(vectors[v].n_leaves) == vectors[v].n_nodes);
        assert(mls_tree_root(vectors[v].n_leaves) == vectors[v].root);
        g_mdk_asserted += 2;
        for (size_t i = 0; i < vectors[v].array_size; i++) {
            if (vectors[v].left[i] != UINT32_MAX) {
                assert(mls_tree_left((uint32_t)i) == vectors[v].left[i]);
                g_mdk_asserted++;
            }
            if (vectors[v].right[i] != UINT32_MAX) {
                assert(mls_tree_right((uint32_t)i) == vectors[v].right[i]);
                g_mdk_asserted++;
            }
            if (vectors[v].parent[i] != UINT32_MAX) {
                assert(mls_tree_parent((uint32_t)i, vectors[v].n_leaves) == vectors[v].parent[i]);
                g_mdk_asserted++;
            }
            if (vectors[v].sibling[i] != UINT32_MAX) {
                assert(mls_tree_sibling((uint32_t)i, vectors[v].n_leaves) == vectors[v].sibling[i]);
                g_mdk_asserted++;
            }
        }
        mdk_free_tree_math_vector(&vectors[v]);
    }
    printf("PASS (%zu cases asserted)\n", count);
}

static void
test_mdk_secret_tree_vectors(const char *vector_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/secret-tree.json", vector_dir);
    MdkSecretTreeVector vectors[20];
    size_t count = 0;
    assert(mdk_load_secret_tree_vectors(path, vectors, &count, 20) == 0);
    assert(count > 0 && "secret-tree file exists but yielded zero ciphersuite-1 cases");

    for (size_t v = 0; v < count; v++) {
        uint8_t sample[MLS_HASH_LEN] = {0};
        size_t sample_len = vectors[v].sender_data_ciphertext_len < MLS_HASH_LEN ?
                            vectors[v].sender_data_ciphertext_len : MLS_HASH_LEN;
        memcpy(sample, vectors[v].sender_data_ciphertext, sample_len);
        uint8_t key[MLS_AEAD_KEY_LEN], nonce[MLS_AEAD_NONCE_LEN];
        assert(mls_crypto_expand_with_label(key, sizeof(key), vectors[v].sender_data_secret,
                                            "key", sample, MLS_HASH_LEN) == 0);
        assert(mls_crypto_expand_with_label(nonce, sizeof(nonce), vectors[v].sender_data_secret,
                                            "nonce", sample, MLS_HASH_LEN) == 0);
        assert_bytes_eq("secret-tree.sender_data_key", key, vectors[v].sender_data_key, sizeof(key));
        assert_bytes_eq("secret-tree.sender_data_nonce", nonce, vectors[v].sender_data_nonce, sizeof(nonce));

        MlsSecretTree st;
        assert(mls_secret_tree_init(&st, vectors[v].encryption_secret, (uint32_t)vectors[v].n_leaves) == 0);
        for (uint32_t leaf = 0; leaf < vectors[v].n_leaves; leaf++) {
            for (size_t gi = 0; gi < vectors[v].generation_count[leaf]; gi++) {
                MdkSecretTreeGenerationVector *g = &vectors[v].generations[leaf][gi];
                MlsMessageKeys mk;
                assert(mls_secret_tree_get_keys_for_generation(&st, leaf, false,
                                                               g->generation, 1000, &mk) == 0);
                assert_bytes_eq("secret-tree.application_key", mk.key, g->application_key, sizeof(g->application_key));
                assert_bytes_eq("secret-tree.application_nonce", mk.nonce, g->application_nonce, sizeof(g->application_nonce));
                assert(mls_secret_tree_get_keys_for_generation(&st, leaf, true,
                                                               g->generation, 1000, &mk) == 0);
                assert_bytes_eq("secret-tree.handshake_key", mk.key, g->handshake_key, sizeof(g->handshake_key));
                assert_bytes_eq("secret-tree.handshake_nonce", mk.nonce, g->handshake_nonce, sizeof(g->handshake_nonce));
            }
        }
        mls_secret_tree_free(&st);
    }
    printf("PASS (%zu ciphersuite-1 cases asserted)\n", count);
}

static void
test_mdk_transcript_hashes_vectors(const char *vector_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/transcript-hashes.json", vector_dir);
    MdkTranscriptHashesVector vectors[20];
    size_t count = 0;
    assert(mdk_load_transcript_hashes_vectors(path, vectors, &count, 20) == 0);
    assert(count > 0 && "transcript-hashes file exists but yielded zero ciphersuite-1 cases");

    for (size_t i = 0; i < count; i++) {
        size_t confirmed_content_len = 0;
        assert(authenticated_content_confirmed_len(
                   vectors[i].authenticated_content,
                   vectors[i].authenticated_content_len,
                   &confirmed_content_len) == 0);
        size_t conf_input_len = MLS_HASH_LEN + confirmed_content_len;
        uint8_t *conf_input = malloc(conf_input_len);
        assert(conf_input);
        memcpy(conf_input, vectors[i].interim_transcript_hash_before, MLS_HASH_LEN);
        memcpy(conf_input + MLS_HASH_LEN, vectors[i].authenticated_content,
               confirmed_content_len);
        uint8_t confirmed[MLS_HASH_LEN];
        assert(mls_crypto_hash(confirmed, conf_input, conf_input_len) == 0);
        free(conf_input);
        assert_bytes_eq("transcript.confirmed", confirmed,
                        vectors[i].confirmed_transcript_hash_after, MLS_HASH_LEN);

        uint8_t tag[MLS_HASH_LEN];
        assert(mls_compute_confirmation_tag(vectors[i].confirmation_key, confirmed, tag) == 0);
        MlsTlsBuf interim_buf;
        assert(mls_tls_buf_init(&interim_buf, MLS_HASH_LEN * 2 + 1) == 0);
        assert(mls_tls_buf_append(&interim_buf, confirmed, MLS_HASH_LEN) == 0);
        assert(mls_tls_write_opaque32(&interim_buf, tag, MLS_HASH_LEN) == 0);
        uint8_t interim[MLS_HASH_LEN];
        assert(mls_crypto_hash(interim, interim_buf.data, interim_buf.len) == 0);
        mls_tls_buf_free(&interim_buf);
        assert_bytes_eq("transcript.interim", interim,
                        vectors[i].interim_transcript_hash_after, MLS_HASH_LEN);
    }
    printf("PASS (%zu ciphersuite-1 cases asserted)\n", count);
}

static void
test_mdk_deserialization_vectors(const char *vector_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/deserialization.json", vector_dir);
    MdkDeserializationVector vectors[50];
    size_t count = 0;
    assert(mdk_load_deserialization_vectors(path, vectors, &count, 50) == 0);
    assert(count > 0 && "deserialization file exists but yielded zero cases");
    for (size_t i = 0; i < count; i++) {
        MlsTlsReader reader;
        size_t len = 0;
        mls_tls_reader_init(&reader, vectors[i].vlbytes_header, vectors[i].vlbytes_header_len);
        assert(mls_tls_read_vli(&reader, &len) == 0);
        assert(len == vectors[i].length);
        assert(mls_tls_reader_done(&reader));
        g_mdk_asserted++;
    }
    printf("PASS (%zu VLBytes headers asserted)\n", count);
}

static void
test_mdk_messages_vectors(const char *vector_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/messages.json", vector_dir);
    MdkMessagesVector vectors[10];
    size_t count = 0;
    assert(mdk_load_messages_vectors(path, vectors, &count, 10) == 0);
    assert(count > 0 && "messages file exists but yielded zero cases");
    for (size_t i = 0; i < count; i++) {
        assert_key_package_roundtrip("messages.key_package", vectors[i].mls_key_package, vectors[i].mls_key_package_len);
        assert_welcome_roundtrip("messages.welcome", vectors[i].mls_welcome, vectors[i].mls_welcome_len);
        assert_mls_message_roundtrip("messages.public_application", vectors[i].public_message_application, vectors[i].public_message_application_len);
        assert_mls_message_roundtrip("messages.public_proposal", vectors[i].public_message_proposal, vectors[i].public_message_proposal_len);
        assert_mls_message_roundtrip("messages.public_commit", vectors[i].public_message_commit, vectors[i].public_message_commit_len);
        assert_mls_message_roundtrip("messages.private", vectors[i].private_message, vectors[i].private_message_len);
    }
    printf("PASS (%zu message cases asserted)\n", count);
}

static void
test_mdk_welcome_vectors(const char *vector_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/welcome.json", vector_dir);
    MdkWelcomeVector vectors[20];
    size_t count = 0;
    assert(mdk_load_welcome_vectors(path, vectors, &count, 20) == 0);
    assert(count > 0 && "welcome file exists but yielded zero ciphersuite-1 cases");
    for (size_t i = 0; i < count; i++) {
        assert_key_package_roundtrip("welcome.key_package", vectors[i].key_package, vectors[i].key_package_len);
        assert_welcome_roundtrip("welcome.welcome", vectors[i].welcome, vectors[i].welcome_len);
    }
    printf("PASS (%zu ciphersuite-1 cases asserted)\n", count);
}

static void
test_mdk_message_protection_vectors(const char *vector_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/message-protection.json", vector_dir);
    MdkMessageProtectionVector vectors[20];
    size_t count = 0;
    assert(mdk_load_message_protection_vectors(path, vectors, &count, 20) == 0);
    assert(count > 0 && "message-protection file exists but yielded zero ciphersuite-1 cases");
    for (size_t i = 0; i < count; i++) {
        assert_mls_message_roundtrip("message-protection.proposal_pub", vectors[i].proposal_pub, vectors[i].proposal_pub_len);
        assert_mls_message_roundtrip("message-protection.proposal_priv", vectors[i].proposal_priv, vectors[i].proposal_priv_len);
        assert_mls_message_roundtrip("message-protection.application_priv", vectors[i].application_priv, vectors[i].application_priv_len);
    }
    printf("PASS (%zu ciphersuite-1 protection cases asserted)\n", count);
}

static void
print_deferred_loaded(const char *label, size_t count, const char *reason)
{
    (void)label;
    assert(count > 0 && "vector file exists but yielded zero parsed cases");
    g_mdk_deferred += count;
    printf("XFAIL/DEFERRED (loaded %zu cases; %s)\n", count, reason);
}

static void
test_mdk_psk_secret_vectors(const char *vector_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/psk_secret.json", vector_dir);
    MdkPskSecretVector vectors[20];
    size_t count = 0;
    assert(mdk_load_psk_secret_vectors(path, vectors, &count, 20) == 0);
    assert(count > 0 && "psk_secret file exists but yielded zero ciphersuite-1 cases");

    for (size_t v = 0; v < count; v++) {
        MlsPskInput psks[16];
        assert(vectors[v].psk_count <= 16);
        for (size_t i = 0; i < vectors[v].psk_count; i++) {
            psks[i].psk_id = vectors[v].psks[i].psk_id;
            psks[i].psk_id_len = vectors[v].psks[i].psk_id_len;
            psks[i].psk = vectors[v].psks[i].psk;
            psks[i].psk_len = 32;
            psks[i].psk_nonce = vectors[v].psks[i].psk_nonce;
            psks[i].psk_nonce_len = 32;
        }
        uint8_t actual[MLS_HASH_LEN];
        assert(mls_psk_secret_compute(psks, vectors[v].psk_count, actual) == 0);
        assert_bytes_eq("psk_secret.combined", actual, vectors[v].psk_secret, MLS_HASH_LEN);
    }
    printf("PASS (%zu ciphersuite-1 PSK chains asserted)\n", count);
}

static int
mdk_deserialize_proposal(const uint8_t *data, size_t len, MlsProposal *proposal)
{
    if (!data || !proposal) return -1;
    MlsTlsReader reader;
    memset(proposal, 0, sizeof(*proposal));
    mls_tls_reader_init(&reader, data, len);
    if (mls_tls_read_u16(&reader, &proposal->type) != 0) return -1;
    switch (proposal->type) {
    case MLS_PROPOSAL_ADD:
        if (mls_key_package_deserialize(&reader, &proposal->add.key_package) != 0) goto fail;
        break;
    case MLS_PROPOSAL_UPDATE:
        if (mls_leaf_node_deserialize(&reader, &proposal->update.leaf_node) != 0) goto fail;
        break;
    case MLS_PROPOSAL_REMOVE:
        if (mls_tls_read_u32(&reader, &proposal->remove.removed_leaf) != 0) goto fail;
        break;
    default:
        goto fail;
    }
    if (!mls_tls_reader_done(&reader)) goto fail;
    return 0;
fail:
    mls_proposal_clear(proposal);
    return -1;
}

static int
mdk_subtree_is_blank(const MlsRatchetTree *tree, uint32_t node_idx)
{
    if (!tree || node_idx >= tree->n_nodes) return 1;
    if (tree->nodes[node_idx].type != MLS_NODE_BLANK) return 0;
    if (mls_tree_is_leaf(node_idx)) return 1;
    return mdk_subtree_is_blank(tree, mls_tree_left(node_idx)) &&
           mdk_subtree_is_blank(tree, mls_tree_right(node_idx));
}

static int
mdk_truncate_blank_right_edge(MlsRatchetTree *tree)
{
    if (!tree) return -1;
    while (tree->n_leaves > 1) {
        uint32_t root = mls_tree_root(tree->n_leaves);
        uint32_t right = mls_tree_right(root);
        if (!mdk_subtree_is_blank(tree, right)) break;

        uint32_t new_leaves = tree->n_leaves / 2;
        uint32_t new_nodes_count = mls_tree_node_width(new_leaves);
        for (uint32_t i = new_nodes_count; i < tree->n_nodes; i++)
            mls_tree_blank_node(&tree->nodes[i]);
        MlsNode *new_nodes = realloc(tree->nodes, (size_t)new_nodes_count * sizeof(MlsNode));
        if (!new_nodes && new_nodes_count > 0) return -1;
        tree->nodes = new_nodes;
        tree->n_leaves = new_leaves;
        tree->n_nodes = new_nodes_count;
    }
    return 0;
}

static int
mdk_apply_tree_proposal(MlsRatchetTree *tree, const MlsProposal *proposal,
                        uint32_t proposal_sender)
{
    if (!tree || !proposal) return -1;
    switch (proposal->type) {
    case MLS_PROPOSAL_ADD: {
        if (mls_key_package_validate(&proposal->add.key_package) != 0) return -1;
        uint32_t new_leaf = UINT32_MAX;
        for (uint32_t leaf = 0; leaf < tree->n_leaves; leaf++) {
            uint32_t node_idx = mls_tree_leaf_to_node(leaf);
            if (node_idx < tree->n_nodes && tree->nodes[node_idx].type == MLS_NODE_BLANK) {
                new_leaf = node_idx;
                break;
            }
        }
        if (new_leaf == UINT32_MAX) {
            uint32_t old_leaves = tree->n_leaves;
            uint32_t new_leaves = old_leaves ? old_leaves * 2 : 1;
            uint32_t new_nodes_count = mls_tree_node_width(new_leaves);
            if (new_nodes_count <= tree->n_nodes) return -1;
            MlsNode *new_nodes = realloc(tree->nodes, (size_t)new_nodes_count * sizeof(MlsNode));
            if (!new_nodes) return -1;
            tree->nodes = new_nodes;
            for (uint32_t i = tree->n_nodes; i < new_nodes_count; i++) {
                memset(&tree->nodes[i], 0, sizeof(MlsNode));
                tree->nodes[i].type = MLS_NODE_BLANK;
            }
            tree->n_leaves = new_leaves;
            tree->n_nodes = new_nodes_count;
            new_leaf = mls_tree_leaf_to_node(old_leaves);
        }
        MlsNode *n = &tree->nodes[new_leaf];
        mls_tree_blank_node(n);
        n->type = MLS_NODE_LEAF;
        return mls_leaf_node_clone(&n->leaf, &proposal->add.key_package.leaf_node);
    }
    case MLS_PROPOSAL_UPDATE: {
        if (proposal_sender >= tree->n_leaves) return -1;
        uint32_t sender_node = mls_tree_leaf_to_node(proposal_sender);
        mls_tree_blank_node(&tree->nodes[sender_node]);
        tree->nodes[sender_node].type = MLS_NODE_LEAF;
        if (mls_leaf_node_clone(&tree->nodes[sender_node].leaf,
                                &proposal->update.leaf_node) != 0)
            return -1;
        uint32_t dp[128];
        uint32_t dp_len = 0;
        if (mls_tree_direct_path(sender_node, tree->n_leaves, dp, 128, &dp_len) != 0)
            return -1;
        for (uint32_t i = 0; i < dp_len; i++)
            mls_tree_blank_node(&tree->nodes[dp[i]]);
        return 0;
    }
    case MLS_PROPOSAL_REMOVE: {
        if (proposal->remove.removed_leaf >= tree->n_leaves) return -1;
        uint32_t rm_node = mls_tree_leaf_to_node(proposal->remove.removed_leaf);
        mls_tree_blank_node(&tree->nodes[rm_node]);
        uint32_t dp[128];
        uint32_t dp_len = 0;
        if (mls_tree_direct_path(rm_node, tree->n_leaves, dp, 128, &dp_len) != 0)
            return -1;
        for (uint32_t i = 0; i < dp_len; i++)
            mls_tree_blank_node(&tree->nodes[dp[i]]);
        return mdk_truncate_blank_right_edge(tree);
    }
    default:
        return -1;
    }
}

static void
test_mdk_tree_operations_vectors(const char *vector_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/tree-operations.json", vector_dir);
    MdkTreeOperationsVector vectors[50];
    size_t count = 0;
    assert(mdk_load_tree_operations_vectors(path, vectors, &count, 50) == 0);
    assert(count > 0 && "tree-operations file exists but yielded zero ciphersuite-1 cases");

    for (size_t v = 0; v < count; v++) {
        MlsRatchetTree tree;
        assert(mls_ratchet_tree_deserialize(vectors[v].tree_before,
                                            vectors[v].tree_before_len,
                                            &tree) == 0);
        uint8_t before_hash[MLS_HASH_LEN];
        assert(mls_tree_root_hash(&tree, before_hash) == 0);
        assert_bytes_eq("tree-operations.tree_hash_before", before_hash,
                        vectors[v].tree_hash_before, MLS_HASH_LEN);

        MlsRatchetTree expected_tree;
        assert(mls_ratchet_tree_deserialize(vectors[v].tree_after,
                                            vectors[v].tree_after_len,
                                            &expected_tree) == 0);
        uint8_t expected_tree_hash[MLS_HASH_LEN];
        assert(mls_tree_root_hash(&expected_tree, expected_tree_hash) == 0);
        assert_bytes_eq("tree-operations.expected_tree_after_hash",
                        expected_tree_hash, vectors[v].tree_hash_after, MLS_HASH_LEN);
        mls_tree_free(&expected_tree);

        MlsProposal proposal;
        assert(mdk_deserialize_proposal(vectors[v].proposal, vectors[v].proposal_len,
                                        &proposal) == 0);
        assert(mdk_apply_tree_proposal(&tree, &proposal, vectors[v].proposal_sender) == 0);

        uint8_t *serialized = NULL;
        size_t serialized_len = 0;
        assert(mls_ratchet_tree_serialize(&tree, &serialized, &serialized_len) == 0);
        uint8_t after_hash[MLS_HASH_LEN];
        assert(mls_tree_root_hash(&tree, after_hash) == 0);
        assert_bytes_eq("tree-operations.tree_hash_after", after_hash,
                        vectors[v].tree_hash_after, MLS_HASH_LEN);
        assert(serialized_len == vectors[v].tree_after_len && "tree-operations serialized tree length mismatch");
        assert_bytes_eq("tree-operations.tree_after", serialized,
                        vectors[v].tree_after, vectors[v].tree_after_len);
        free(serialized);
        mls_proposal_clear(&proposal);
        mls_tree_free(&tree);
    }
    printf("PASS (%zu tree operation replays asserted)\n", count);
}

static void
test_mdk_tree_validation_vectors(const char *vector_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/tree-validation.json", vector_dir);
    MdkTreeValidationVector vectors[20];
    size_t count = 0;
    assert(mdk_load_tree_validation_vectors(path, vectors, &count, 20) == 0);
    assert(count > 0 && "tree-validation file exists but yielded zero ciphersuite-1 cases");

    for (size_t v = 0; v < count; v++) {
        MlsRatchetTree tree;
        assert(mls_ratchet_tree_deserialize(vectors[v].tree, vectors[v].tree_len, &tree) == 0);
        assert(vectors[v].tree_hash_count == tree.n_nodes);
        assert(vectors[v].resolution_count == tree.n_nodes);
        assert(mls_tree_verify_parent_hashes(&tree) == 0 && "tree-validation parent hashes must verify");
        g_mdk_asserted++;

        uint8_t *serialized = NULL;
        size_t serialized_len = 0;
        assert(mls_ratchet_tree_serialize(&tree, &serialized, &serialized_len) == 0);
        assert(serialized_len == vectors[v].tree_len && "tree-validation serialized tree length mismatch");
        assert_bytes_eq("tree-validation.tree_roundtrip", serialized,
                        vectors[v].tree, vectors[v].tree_len);
        free(serialized);

        for (uint32_t node = 0; node < tree.n_nodes; node++) {
            uint8_t hash[MLS_HASH_LEN];
            assert(mls_tree_hash(&tree, node, hash) == 0);
            assert_bytes_eq("tree-validation.tree_hash", hash,
                            vectors[v].tree_hashes[node], MLS_HASH_LEN);

            uint32_t resolution[512];
            uint32_t resolution_len = 0;
            assert(tree.n_nodes <= 512);
            assert(mls_tree_resolution(&tree, node, resolution, 512, &resolution_len) == 0);
            assert(resolution_len == vectors[v].resolutions[node].count &&
                   "tree-validation resolution length mismatch");
            for (uint32_t i = 0; i < resolution_len; i++) {
                assert(resolution[i] == vectors[v].resolutions[node].nodes[i] &&
                       "tree-validation resolution node mismatch");
                g_mdk_asserted++;
            }
        }
        mls_tree_free(&tree);
        mdk_free_tree_validation_vector(&vectors[v]);
    }
    printf("PASS (%zu ratchet trees validated)\n", count);
}

static void
test_mdk_treekem_vectors(const char *vector_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/treekem.json", vector_dir);
    MdkTreeKEMVector vectors[20];
    size_t count = 0;
    assert(mdk_load_treekem_vectors(path, vectors, &count, 20) == 0);
    print_deferred_loaded("treekem", count, "full UpdatePath/commit-auth TreeKEM replay is not implemented");
}

static void
test_mdk_passive_client_all_vectors(const char *vector_dir)
{
    const char *files[] = {
        "passive-client-welcome.json",
        "passive-client-handling-commit.json", 
        "passive-client-random.json"
    };
    size_t total = 0;
    for (size_t i = 0; i < 3; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", vector_dir, files[i]);
        MdkPassiveClientVector vectors[10];
        size_t count = 0;
        assert(mdk_load_passive_client_vectors(path, vectors, &count, 10) == 0);
        total += count;
    }
    print_deferred_loaded("passive-client", total, "passive client Welcome/commit processing is not implemented end-to-end");
}

/* ──────────────────────────────────────────────────────────────────────── */

int main(void)
{
    if (sodium_init() < 0) {
        fprintf(stderr, "Failed to initialize libsodium\n");
        return 1;
    }

    printf("libmarmot: Interoperability test suite\n");

    /* Check for MDK vector files - try multiple possible locations */
    struct stat st;
    const char *vector_paths[] = {
        "tests/vectors/mdk",
        "libmarmot/tests/vectors/mdk",
        "../libmarmot/tests/vectors/mdk",
        "../../libmarmot/tests/vectors/mdk",
        "../tests/vectors/mdk",
        "./vectors/mdk"
    };
    const char *vector_dir = NULL;
    bool found_vectors = false;
    for (size_t i = 0; i < sizeof(vector_paths) / sizeof(vector_paths[0]); i++) {
        if (stat(vector_paths[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            found_vectors = true;
            vector_dir = vector_paths[i];
            break;
        }
    }
    
    if (found_vectors) {
        printf("  MDK vector directory found at: %s\n", vector_dir);
        printf("\n─ MDK Cross-Implementation Validation ─\n");
        
        /* Core cryptographic operations */
        printf("  %-55s", "MDK crypto-basics vectors");
        test_mdk_crypto_basics_vectors(vector_dir);
        printf("  %-55s", "MDK key-schedule vectors");
        test_mdk_key_schedule_vectors(vector_dir);
        
        /* Tree mathematics */
        printf("  %-55s", "MDK tree-math vectors");
        test_mdk_tree_math_vectors(vector_dir);
        
        /* Secret tree and encryption */
        printf("  %-55s", "MDK secret-tree vectors");
        test_mdk_secret_tree_vectors(vector_dir);
        
        /* Pre-shared keys */
        printf("  %-55s", "MDK psk_secret vectors");
        test_mdk_psk_secret_vectors(vector_dir);
        
        /* Message handling */
        printf("  %-55s", "MDK message-protection vectors");
        test_mdk_message_protection_vectors(vector_dir);
        printf("  %-55s", "MDK messages vectors");
        test_mdk_messages_vectors(vector_dir);
        printf("  %-55s", "MDK transcript-hashes vectors");
        test_mdk_transcript_hashes_vectors(vector_dir);
        
        /* Tree operations */
        printf("  %-55s", "MDK tree-operations vectors");
        test_mdk_tree_operations_vectors(vector_dir);
        printf("  %-55s", "MDK tree-validation vectors");
        test_mdk_tree_validation_vectors(vector_dir);
        printf("  %-55s", "MDK treekem vectors");
        test_mdk_treekem_vectors(vector_dir);
        
        /* Welcome and passive client scenarios */
        printf("  %-55s", "MDK welcome vectors");
        test_mdk_welcome_vectors(vector_dir);
        printf("  %-55s", "MDK passive-client vectors");
        test_mdk_passive_client_all_vectors(vector_dir);
        
        /* Utilities */
        printf("  %-55s", "MDK deserialization vectors");
        test_mdk_deserialization_vectors(vector_dir);

        /* Protocol-level vectors (Marmot-specific, from protocol-vectors.json) */
        run_protocol_vector_tests(vector_dir);
    } else {
        printf("  No MDK vectors found — running self-consistency tests only\n");
    }

    printf("\n─ TLS Serialization ─\n");
    TEST(test_key_package_serialize_roundtrip);

    printf("\n─ Extension Serialization ─\n");
    TEST(test_group_data_extension_roundtrip);
    TEST(test_group_data_extension_with_image);

    printf("\n─ Key Derivation Consistency ─\n");
    TEST(test_exporter_nip44_consistency);
    TEST(test_exporter_media_key_consistency);
    TEST(test_full_epoch_to_exporter);
    TEST(test_cross_epoch_key_isolation);

    printf("\n─ Protocol Constants ─\n");
    TEST(test_nostr_event_kinds);

    printf("\n─ Self-Vectors ─\n");
    TEST(test_dump_self_vectors);

    if (found_vectors) {
        printf("\nInterop self-tests passed; MDK asserted checks: %zu; deferred vector cases: %zu.\n",
               g_mdk_asserted, g_mdk_deferred);
        printf("Remaining deferred vector classes are printed as XFAIL/DEFERRED above and are not green coverage.\n");
    } else {
        printf("\nAll interop self-tests passed (9 self-tests; no MDK vectors loaded).\n");
        printf("\nNOTE: For full cross-implementation validation, capture MDK vectors\n");
        printf("      and place them in tests/vectors/mdk/.\n");
    }
    
    return 0;
}
