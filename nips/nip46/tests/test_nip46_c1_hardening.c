/* C1 (beads nostrc-ot2c.2): NIP-46 secret role separation and hardened
 * bunker-URI / message parsing / request-identity coverage.
 *
 * This file consolidates the acceptance-criteria negatives per beads
 * nostrc-ot2c.2. Complementary coverage lives in test_caller_hardening.c,
 * test_uri_comprehensive.c, test_msg_comprehensive.c, test_session_management.c,
 * and test_response_validation.c.
 *
 * Invariants asserted here:
 *  - bunker:// URI secret= is a connect-authorization token; the session
 *    transport private key is generated independently and never derived
 *    from the token.
 *  - Percent-encoded control bytes (including %00 NUL) are rejected.
 *  - Duplicate query keys (secret=, relay=, nostrconnect fields) are rejected.
 *  - Oversize/malformed URI, message and response payloads are rejected
 *    with partial-parse cleanup (getters return NULL after a failing parse).
 *  - Response id/error strings survive escaping without smuggling.
 *  - Random request IDs are 256-bit lowercase hex with no observable
 *    collisions in a large batch.
 */

#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_msg.h"
#include "nostr/nip46/nip46_types.h"
#include "nostr/nip46/nip46_uri.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *PK64 =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static int fail(const char *what) {
    fprintf(stderr, "FAIL: %s\n", what);
    return 1;
}

/* --- Token vs transport-key role separation ---------------------------- */

static int test_token_is_not_transport_key(void) {
    char uri[512];
    snprintf(uri, sizeof(uri),
             "bunker://%s?relay=wss%%3A%%2F%%2Fnos.lol&secret=connect-token", PK64);

    NostrNip46Session *s = nostr_nip46_client_new();
    if (!s) return fail("client_new");
    if (nostr_nip46_client_connect(s, uri, NULL) != 0) return fail("connect");

    char *token = NULL, *sk = NULL;
    if (nostr_nip46_session_get_connect_token(s, &token) != 0 || !token ||
        strcmp(token, "connect-token") != 0)
        return fail("connect token must be preserved as-is");
    free(token);

    if (nostr_nip46_session_get_secret(s, &sk) != 0 || !sk)
        return fail("transport secret must be generated");
    if (strlen(sk) != 64) return fail("transport secret must be 64-hex private key");
    if (strcmp(sk, "connect-token") == 0)
        return fail("REGRESSION: URI secret was reused as transport private key");
    free(sk);

    nostr_nip46_session_free(s);
    return 0;
}

static int test_bunker_uri_without_secret_still_gets_transport_key(void) {
    char uri[256];
    snprintf(uri, sizeof(uri), "bunker://%s?relay=wss%%3A%%2F%%2Fnos.lol", PK64);
    NostrNip46Session *s = nostr_nip46_client_new();
    if (!s) return fail("client_new");
    if (nostr_nip46_client_connect(s, uri, NULL) != 0) return fail("connect");
    char *token = NULL, *sk = NULL;
    (void)nostr_nip46_session_get_connect_token(s, &token);
    if (token) return fail("no token on URI means no token stored");
    if (nostr_nip46_session_get_secret(s, &sk) != 0 || !sk || strlen(sk) != 64)
        return fail("transport secret still generated when URI has no secret=");
    free(sk); free(token);
    nostr_nip46_session_free(s);
    return 0;
}

/* --- URI hardening negatives ------------------------------------------- */

static int expect_bunker_reject(const char *uri, const char *label) {
    NostrNip46BunkerURI u = {0};
    if (nostr_nip46_uri_parse_bunker(uri, &u) == 0) {
        nostr_nip46_uri_bunker_free(&u);
        return fail(label);
    }
    /* Failure path must leave the struct clean. */
    if (u.remote_signer_pubkey_hex || u.relays || u.secret) return fail(label);
    return 0;
}

static int test_uri_hardening_negatives(void) {
    char uri[16384 + 128];
    /* Percent-encoded NUL byte in secret. */
    snprintf(uri, sizeof(uri), "bunker://%s?secret=%%00", PK64);
    if (expect_bunker_reject(uri, "reject %00 in secret")) return 1;
    /* Percent-encoded ESC (control char). */
    snprintf(uri, sizeof(uri), "bunker://%s?secret=%%1b", PK64);
    if (expect_bunker_reject(uri, "reject control byte in secret")) return 1;
    /* Malformed percent escape. */
    snprintf(uri, sizeof(uri), "bunker://%s?secret=abc%%zz", PK64);
    if (expect_bunker_reject(uri, "reject malformed %escape")) return 1;
    /* Duplicate secret. */
    snprintf(uri, sizeof(uri), "bunker://%s?secret=a&secret=b", PK64);
    if (expect_bunker_reject(uri, "reject duplicate secret")) return 1;
    /* Duplicate relay. */
    snprintf(uri, sizeof(uri),
             "bunker://%s?relay=wss%%3A%%2F%%2Fnos.lol&relay=wss%%3A%%2F%%2Fnos.lol", PK64);
    if (expect_bunker_reject(uri, "reject duplicate relay")) return 1;
    /* Empty relay value. */
    snprintf(uri, sizeof(uri), "bunker://%s?relay=", PK64);
    if (expect_bunker_reject(uri, "reject empty relay")) return 1;
    /* Empty secret value. */
    snprintf(uri, sizeof(uri), "bunker://%s?secret=", PK64);
    if (expect_bunker_reject(uri, "reject empty secret")) return 1;
    /* URI far past URI_MAX (16384). */
    size_t sz = sizeof(uri);
    size_t used = (size_t)snprintf(uri, sz, "bunker://%s?secret=", PK64);
    for (size_t i = used; i + 1 < sz - 8; ++i) uri[i] = 'a';
    uri[sz - 8] = '\0';
    if (expect_bunker_reject(uri, "reject oversize URI")) return 1;
    /* Too many relays (>16). */
    used = (size_t)snprintf(uri, sz, "bunker://%s?", PK64);
    for (int i = 0; i < 17; i++)
        used += (size_t)snprintf(uri + used, sz - used,
            "%srelay=wss%%3A%%2F%%2Fr%d.example", i ? "&" : "", i);
    if (expect_bunker_reject(uri, "reject >16 relays")) return 1;
    /* Zero-length key. */
    snprintf(uri, sz, "bunker://%s?=v", PK64);
    if (expect_bunker_reject(uri, "reject empty key")) return 1;
    return 0;
}

/* --- Message parsing / response admission negatives -------------------- */

static int test_message_hardening_negatives(void) {
    NostrNip46Request req;
    NostrNip46Response res;

    /* Numeric id in request is not a string -> reject */
    if (nostr_nip46_request_parse(
            "{\"id\":1,\"method\":\"ping\",\"params\":[]}", &req) == 0)
        return fail("request with numeric id must be rejected");

    /* Missing method -> reject */
    if (nostr_nip46_request_parse(
            "{\"id\":\"a\",\"params\":[]}", &req) == 0)
        return fail("missing method");

    /* Params not array -> reject */
    if (nostr_nip46_request_parse(
            "{\"id\":\"a\",\"method\":\"ping\",\"params\":\"x\"}", &req) == 0)
        return fail("params not array");

    /* Duplicate keys -> reject (JSON_REJECT_DUPLICATES) */
    if (nostr_nip46_request_parse(
            "{\"id\":\"a\",\"id\":\"b\",\"method\":\"p\",\"params\":[]}", &req) == 0)
        return fail("duplicate id keys");

    /* Response with numeric id -> reject */
    if (nostr_nip46_response_parse(
            "{\"id\":42,\"result\":\"ok\"}", &res) == 0)
        return fail("response with numeric id");

    /* Response with error as non-string -> reject */
    if (nostr_nip46_response_parse(
            "{\"id\":\"a\",\"error\":42}", &res) == 0)
        return fail("response with numeric error");

    /* Response with neither result nor error -> reject */
    if (nostr_nip46_response_parse(
            "{\"id\":\"a\"}", &res) == 0)
        return fail("response missing result and error");

    /* Intermediate response form: non-string result becomes a JSON text.
     * This is the expected shape for e.g. auth_url or object-typed results. */
    if (nostr_nip46_response_parse(
            "{\"id\":\"a\",\"result\":{\"auth_url\":\"https://x\"}}", &res) != 0)
        return fail("object result must parse to JSON text");
    if (!res.result || strstr(res.result, "\"auth_url\"") == NULL)
        return fail("object result content preserved as JSON");
    nostr_nip46_response_free(&res);

    /* Malicious id/error survive escape round-trip. */
    const char *evil_id = "id\"with\\quotes\nand-newline";
    const char *evil_err = "error \"quoted\" \\ \n injected";
    char *encoded = nostr_nip46_response_build_err(evil_id, evil_err);
    if (!encoded) return fail("build err with special chars");
    if (nostr_nip46_response_parse(encoded, &res) != 0)
        return fail("parse escaped err response");
    if (strcmp(res.id, evil_id) != 0) return fail("id round-trip preserved");
    if (strcmp(res.error, evil_err) != 0) return fail("error round-trip preserved");
    nostr_nip46_response_free(&res);
    free(encoded);

    return 0;
}

/* --- Request ID entropy / collision resistance ------------------------- */

static int test_request_id_entropy(void) {
    enum { N = 4096 };
    char **ids = (char **)calloc(N, sizeof(*ids));
    if (!ids) return fail("calloc");
    int rc = 0;
    for (int i = 0; i < N; i++) {
        ids[i] = nostr_nip46_request_id_generate();
        if (!ids[i] || strlen(ids[i]) != 64) { rc = fail("id length"); goto done; }
        for (size_t j = 0; j < 64; j++) {
            char c = ids[i][j];
            if (!(isdigit((unsigned char)c) || (c >= 'a' && c <= 'f'))) {
                rc = fail("id charset (lowercase hex only)"); goto done;
            }
        }
    }
    /* No collisions among N random 256-bit ids. */
    for (int i = 0; i < N && !rc; i++) {
        for (int j = i + 1; j < N && !rc; j++) {
            if (strcmp(ids[i], ids[j]) == 0) rc = fail("request-id collision");
        }
    }
done:
    for (int i = 0; i < N; i++) free(ids[i]);
    free(ids);
    return rc;
}

int main(void) {
    int rc = 0;
    rc |= test_token_is_not_transport_key();
    rc |= test_bunker_uri_without_secret_still_gets_transport_key();
    rc |= test_uri_hardening_negatives();
    rc |= test_message_hardening_negatives();
    rc |= test_request_id_entropy();
    if (rc == 0) puts("test_nip46_c1_hardening: OK");
    return rc;
}
