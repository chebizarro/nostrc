/*
 * libmarmot - adopted-spec KeyPackage profile (nostrc-prqu.9)
 *
 * SPDX-License-Identifier: MIT
 */
#include <marmot/marmot.h>
#include "marmot-internal.h"
#include "kp_profile.h"
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-tag.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, msg); failures++; } \
} while (0)

static void hex_to(const char *hex, uint8_t *out, size_t n) {
    if (marmot_hex_decode(hex, out, n) != 0) { printf("bad hex\n"); exit(2); }
}

/* app-components/account-identity-proof-v2.md "Signing test vector". */
static void test_proof_spec_vector(void) {
    printf("proof spec vector\n");
    uint8_t pk[32], key[32], proof[104];
    hex_to("f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9", pk, 32);
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    char *id = marmot_account_proof_template_id(pk, 1700000000, 0x0001, 0x0807, key, 32);
    CHECK(id && strcmp(id, "b7e9a15dd85990fb0f49c33db3cc9875f73986207b038404ceb6b7fec4e0af6b") == 0,
          "template event id");
    free(id);
    hex_to("f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9000000006553f100"
           "c5315d3c85b9d4907cb03395a2a97b3ba2eab393f8e45b13a5d5233acedac60a"
           "51d2a295e1b1b5ee372d18a49bdb8041a7dba9dedce722c7c6f712f78bbdfb5d", proof, 104);
    CHECK(marmot_account_proof_verify(proof, 104, pk, 0x0001, 0x0807, key, 32) == MARMOT_OK,
          "vector proof verifies");
    key[0] ^= 1;
    CHECK(marmot_account_proof_verify(proof, 104, pk, 0x0001, 0x0807, key, 32) != MARMOT_OK,
          "other MLS signature key rejected");
    key[0] ^= 1;
    CHECK(marmot_account_proof_verify(proof, 104, pk, 0x0002, 0x0807, key, 32) != MARMOT_OK,
          "other ciphersuite rejected");
    proof[39] ^= 1; /* created_at */
    CHECK(marmot_account_proof_verify(proof, 104, pk, 0x0001, 0x0807, key, 32) != MARMOT_OK,
          "tampered created_at rejected");
    proof[39] ^= 1;
    uint8_t other[32];
    memcpy(other, pk, 32);
    other[5] ^= 1;
    CHECK(marmot_account_proof_verify(proof, 104, other, 0x0001, 0x0807, key, 32) != MARMOT_OK,
          "signer_pubkey != credential identity rejected");
    CHECK(marmot_account_proof_verify(proof, 103, pk, 0x0001, 0x0807, key, 32) != MARMOT_OK,
          "short proof rejected");
    CHECK(marmot_account_proof_verify(proof, 104, pk, 0x0001, 0x0807, key, 31) != MARMOT_OK,
          "non-Ed25519 leaf key length rejected");
    CHECK(marmot_account_proof_verify(proof, 104, pk, 0x0001, 0x0804, key, 32) != MARMOT_OK,
          "unsupported MLS signature scheme rejected");
    uint8_t sk[32] = {0}, old_proof[104];
    sk[31] = 3; /* account key from the primary spec vector */
    CHECK(marmot_account_proof_create(pk, sk, NULL, NULL, 0x0001, 0x0807,
                                      key, 32, 1, old_proof) == MARMOT_OK &&
          marmot_account_proof_verify(old_proof, sizeof(old_proof), pk,
                                      0x0001, 0x0807, key, 32) == MARMOT_OK,
          "valid old proof does not expire by receiver clock");
    CHECK(marmot_account_proof_create(pk, sk, NULL, NULL, 0x0001, 0x0807,
                                      key, 32, 0, old_proof) == MARMOT_ERR_VALIDATION,
          "zero signing timestamp rejected");
    CHECK(marmot_account_proof_create(pk, sk, NULL, NULL, 0x0001, 0x0807,
                                      key, 32, 9007199254740992ULL, old_proof) == MARMOT_ERR_VALIDATION,
          "timestamp outside exact JSON integer range rejected");
    CHECK(marmot_account_proof_create(pk, sk, NULL, NULL, 0x0001, 0x0807,
                                      key, 31, 1, old_proof) == MARMOT_ERR_VALIDATION,
          "producer rejects non-Ed25519 leaf key length");
}

typedef struct {
    char sk_hex[65];
    int mode; /* 0 sign, 1 refuse, 2 alter created_at, 3 wrong key */
    int calls;
} SignCtx;

static int sign_cb(void *ud, const char *unsigned_json, char **out) {
    SignCtx *c = ud;
    c->calls++;
    if (c->mode == 1) return -1;
    NostrEvent *ev = nostr_event_new();
    if (!nostr_event_deserialize_compact(ev, unsigned_json, NULL)) return -1;
    if (c->mode == 2) nostr_event_set_created_at(ev, nostr_event_get_created_at(ev) + 1);
    const char *sk = c->sk_hex;
    char *other = NULL;
    if (c->mode == 3) sk = other = nostr_key_generate_private();
    int rc = nostr_event_sign(ev, sk);
    free(other);
    *out = rc == 0 ? nostr_event_serialize_compact(ev) : NULL;
    nostr_event_free(ev);
    return rc == 0 ? 0 : -1;
}

static void keypair(uint8_t sk[32], uint8_t pk[32], char sk_hex[65]) {
    char *s = nostr_key_generate_private();
    char *p = nostr_key_get_public(s);
    snprintf(sk_hex, 65, "%s", s);
    hex_to(s, sk, 32);
    hex_to(p, pk, 32);
    free(s);
    free(p);
}

static NostrTag *first_tag(NostrEvent *ev, const char *key) {
    for (size_t i = 0; i < nostr_tags_size(ev->tags); i++) {
        NostrTag *t = nostr_tags_get(ev->tags, i);
        if (strcmp(nostr_tag_get_key(t), key) == 0) return t;
    }
    return NULL;
}

/* Re-sign @json after @mutate so only the profile rule under test fails. */
static char *mutate_and_resign(const char *json, const char *sk_hex,
                               void (*mutate)(NostrEvent *)) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_deserialize_compact(ev, json, NULL);
    mutate(ev);
    free(ev->id); ev->id = NULL;
    nostr_event_sign(ev, sk_hex);
    char *out = nostr_event_serialize_compact(ev);
    nostr_event_free(ev);
    return out;
}
static void add_encoding(NostrEvent *ev) { nostr_tags_append(ev->tags, nostr_tag_new("encoding", "base64", NULL)); }
static void add_relays(NostrEvent *ev) { nostr_tags_append(ev->tags, nostr_tag_new("relays", "wss://r.example", NULL)); }
static NostrTag *copy_tag(NostrTag *t) {
    NostrTag *c = nostr_tag_new(nostr_tag_get_key(t), NULL);
    for (size_t i = 1; i < nostr_tag_size(t); i++) nostr_tag_append(c, nostr_tag_get(t, i));
    return c;
}
static void drop_8009(NostrEvent *ev) {
    NostrTags *nt = nostr_tags_new(0);
    for (size_t i = 0; i < nostr_tags_size(ev->tags); i++) {
        NostrTag *t = nostr_tags_get(ev->tags, i);
        if (strcmp(nostr_tag_get_key(t), "app_components") == 0)
            nostr_tags_append(nt, nostr_tag_new("app_components", "0x0001", NULL));
        else
            nostr_tags_append(nt, copy_tag(t));
    }
    nostr_event_set_tags(ev, nt);
}

static void replace_tag(NostrEvent *ev, const char *key, NostrTag *with) {
    NostrTags *nt = nostr_tags_new(0);
    for (size_t i = 0; i < nostr_tags_size(ev->tags); i++) {
        NostrTag *t = nostr_tags_get(ev->tags, i);
        nostr_tags_append(nt, strcmp(nostr_tag_get_key(t), key) == 0 ? with : copy_tag(t));
    }
    nostr_event_set_tags(ev, nt);
}
static void add_component(NostrEvent *ev) {
    replace_tag(ev, "app_components", nostr_tag_new("app_components", "0x0001", "0x8003", "0x8009", NULL));
}
static void wrong_suite(NostrEvent *ev) {
    replace_tag(ev, "mls_ciphersuite", nostr_tag_new("mls_ciphersuite", "0x0003", NULL));
}

static void test_create_and_validate(void) {
    printf("adopted create + validate\n");
    uint8_t sk[32], pk[32];
    char sk_hex[65];
    keypair(sk, pk, sk_hex);
    Marmot *m = marmot_new(marmot_storage_memory_new());
    const char *relays[] = {"wss://relay.example"};
    MarmotKeyPackageResult r;
#ifndef MARMOT_ENABLE_ADOPTED_KEY_PACKAGE_PRODUCER
    CHECK(marmot_create_key_package_for_profile(m, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, pk, sk,
                                                NULL, NULL, relays, 1, &r) == MARMOT_ERR_UNSUPPORTED,
          "public ADOPTED producer is build-gated off");
#else
    /* On by default since 0.12.0 (nostrc-lf62): the public producer makes
     * what the internal one does. */
    CHECK(marmot_create_key_package_for_profile(m, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, pk, sk,
                                                NULL, NULL, relays, 1, &r) == MARMOT_OK,
          "public ADOPTED producer");
    CHECK(marmot_validate_key_package_event_json(r.event_json, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED,
                                                 0, NULL, NULL) == MARMOT_OK,
          "public ADOPTED producer output validates");
    marmot_key_package_result_free(&r);
#endif
    CHECK(marmot_create_key_package_adopted_internal(m, pk, sk, NULL, NULL, &r) == MARMOT_OK,
          "create adopted");
    NostrEvent *ev = nostr_event_new();
    CHECK(nostr_event_deserialize_compact(ev, r.event_json, NULL), "parse event");
    CHECK(ev->kind == 30443, "kind");
    CHECK(!first_tag(ev, "encoding") && !first_tag(ev, "relays"), "no encoding/relays tags");
    /* The leaf's private-use components only, as MDK 0.11 requires: the
     * adopted components libmarmot supports, the White Noise set
     * (nostrc-qp24.5.1, nostrc-qp24.5.2). */
    static const char *const comps[] = {"0x8001", "0x8003", "0x8004", "0x8006",
                                        "0x8009", "0x800b", "0x800c"};
    NostrTag *t = first_tag(ev, "app_components");
    bool comps_ok = t && nostr_tag_size(t) == 8;
    for (size_t i = 0; comps_ok && i < 7; i++)
        comps_ok = strcmp(nostr_tag_get(t, i + 1), comps[i]) == 0;
    CHECK(comps_ok, "app_components [0x8001,0x8003,0x8004,0x8006,0x8009,0x800b,0x800c]");
    t = first_tag(ev, "mls_extensions");
    CHECK(t && nostr_tag_size(t) == 3 && strcmp(nostr_tag_get(t, 1), "0x0006") == 0 &&
          strcmp(nostr_tag_get(t, 2), "0xf2d1") == 0,
          "mls_extensions from leaf caps (app_data_dictionary, agent-stream receive role)");
    t = first_tag(ev, "mls_proposals");
    CHECK(t && nostr_tag_size(t) == 3 && strcmp(nostr_tag_get(t, 1), "0x0008") == 0 &&
          strcmp(nostr_tag_get(t, 2), "0x000a") == 0,
          "mls_proposals from leaf caps (app_data_update, self_remove)");
    size_t clen = strlen(ev->content), len = 0;
    uint8_t *raw = malloc(clen);
    CHECK(raw && sodium_base642bin(raw, clen, ev->content, clen, NULL, &len, NULL,
                                   sodium_base64_VARIANT_ORIGINAL) == 0 &&
          len > 4 && raw[0] == 0 && raw[1] == 1 && raw[2] == 0 && raw[3] == 5,
          "MLSMessage(mls10, mls_key_package) framing");
    free(raw);
    nostr_event_free(ev);

    uint8_t owner[32], ref[32];
    CHECK(marmot_validate_key_package_event_json(r.event_json, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED,
                                                 0, owner, ref) == MARMOT_OK, "validates (adopted)");
    CHECK(memcmp(owner, pk, 32) == 0, "owner");
    CHECK(memcmp(ref, r.key_package_ref, 32) == 0, "ref over the inner KeyPackage");
    CHECK(marmot_validate_key_package_event_json(r.event_json, MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8,
                                                 0, NULL, NULL) != MARMOT_OK,
          "MDK 0.8 profile rejects it");
    /* Valid adopted KeyPackages cannot enter the legacy group engine: it
     * cannot emit or process the required adopted GroupContext components. */
    MarmotGroupConfig config = {0};
    MarmotCreateGroupResult group_result;
    CHECK(marmot_create_group(m, pk, (const char *[]){r.event_json}, 1,
                              &config, &group_result) != MARMOT_OK && !group_result.group,
          "group creation fails closed on adopted KeyPackage");
    marmot_create_group_result_free(&group_result);

    /* Lifetime bounds */
    int64_t now = (int64_t)time(NULL);
    CHECK(marmot_validate_key_package_event_json(r.event_json, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED,
                                                 now + 90 * 86400, NULL, NULL) == MARMOT_ERR_KEY_PACKAGE,
          "expired Lifetime rejected");
    CHECK(marmot_validate_key_package_event_json(r.event_json, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED,
                                                 now - 2 * 3600, NULL, NULL) == MARMOT_ERR_KEY_PACKAGE,
          "not-yet-valid Lifetime rejected");

    /* Forbidden / missing tags (event re-signed so only the rule fails). */
    char *enc = mutate_and_resign(r.event_json, sk_hex, add_encoding);
    char *rel = mutate_and_resign(r.event_json, sk_hex, add_relays);
    char *no89 = mutate_and_resign(r.event_json, sk_hex, drop_8009);
    CHECK(marmot_validate_key_package_event_json(enc, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, 0, NULL, NULL) == MARMOT_ERR_VALIDATION, "encoding tag rejected");
    CHECK(marmot_validate_key_package_event_json(rel, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, 0, NULL, NULL) == MARMOT_ERR_VALIDATION, "relays tag rejected");
    CHECK(marmot_validate_key_package_event_json(no89, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, 0, NULL, NULL) == MARMOT_ERR_VALIDATION, "app_components without 0x8009 rejected");
    char *extra = mutate_and_resign(r.event_json, sk_hex, add_component);
    char *suite = mutate_and_resign(r.event_json, sk_hex, wrong_suite);
    CHECK(marmot_validate_key_package_event_json(extra, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, 0, NULL, NULL) == MARMOT_ERR_VALIDATION, "app_components claiming more than the leaf rejected");
    CHECK(marmot_validate_key_package_event_json(suite, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, 0, NULL, NULL) == MARMOT_ERR_VALIDATION, "ciphersuite tag not naming the KP suite rejected");
    free(extra); free(suite);

    /* An MDK 0.8 event is not an adopted one. */
    MarmotKeyPackageResult legacy;
    CHECK(marmot_create_key_package(m, pk, sk, relays, 1, &legacy) == MARMOT_OK, "create mdk");
    CHECK(marmot_validate_key_package_event_json(legacy.event_json, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED,
                                                 0, NULL, NULL) != MARMOT_OK, "adopted rejects MDK 0.8 event");
    CHECK(marmot_validate_key_package_event_json(legacy.event_json, MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8,
                                                 0, NULL, NULL) == MARMOT_OK, "MDK 0.8 event still valid");

    /* Selection by profile. Distinct accounts: within one (pubkey, d) slot
     * the newest event wins regardless of profile, and two events made in
     * the same second tie-break on the event id. */
    uint8_t sk2[32], pk2[32];
    char sk2_hex[65];
    keypair(sk2, pk2, sk2_hex);
    Marmot *m2 = marmot_new(marmot_storage_memory_new());
    MarmotKeyPackageResult other;
    CHECK(marmot_create_key_package(m2, pk2, sk2, relays, 1, &other) == MARMOT_OK, "create mdk (B)");
    const char *cands[] = {other.event_json, r.event_json};
    size_t idx = 99;
    CHECK(marmot_select_key_package_event_for_profile(cands, 2, NULL, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, &idx) == MARMOT_OK && idx == 1,
          "adopted select picks the adopted event");
    idx = 99;
    CHECK(marmot_select_key_package_event(cands, 2, NULL, &idx) == MARMOT_OK && idx == 0,
          "MDK select picks the MDK event");
    idx = 99;
    CHECK(marmot_select_key_package_event_for_profile(cands, 2, pk2, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, &idx) == MARMOT_ERR_KEY_PACKAGE,
          "adopted select finds nothing among MDK-only candidates");
    marmot_key_package_result_free(&other);
    marmot_free(m2);

    free(enc); free(rel); free(no89);
    marmot_key_package_result_free(&r);
    marmot_key_package_result_free(&legacy);
    marmot_free(m);
}

static void test_signer_callback(void) {
    printf("adopted via account signer callback\n");
    uint8_t sk[32], pk[32];
    SignCtx c = {.mode = 0};
    keypair(sk, pk, c.sk_hex);
    Marmot *m = marmot_new(marmot_storage_memory_new());
    MarmotKeyPackageResult r;
    CHECK(marmot_create_key_package_adopted_internal(m, pk, NULL, NULL, NULL, &r) ==
              MARMOT_ERR_INVALID_ARG, "no key and no signer refused");
    CHECK(marmot_create_key_package_adopted_internal(m, pk, NULL, sign_cb, &c, &r) == MARMOT_OK &&
              c.calls == 1, "signer-only create");
    /* The kind:30443 itself comes back unsigned; sign it as the caller would. */
    NostrEvent *ev = nostr_event_new();
    nostr_event_deserialize_compact(ev, r.event_json, NULL);
    CHECK(ev->sig == NULL || ev->sig[0] == '\0', "30443 left unsigned");
    nostr_event_sign(ev, c.sk_hex);
    char *signed_json = nostr_event_serialize_compact(ev);
    nostr_event_free(ev);
    CHECK(marmot_validate_key_package_event_json(signed_json, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED,
                                                 0, NULL, NULL) == MARMOT_OK, "signer-made KP validates");
    free(signed_json);
    marmot_key_package_result_free(&r);

    for (int mode = 1; mode <= 3; mode++) {
        c.mode = mode;
        MarmotKeyPackageResult bad;
        memset(&bad, 0, sizeof(bad));
        MarmotError e = marmot_create_key_package_adopted_internal(m, pk, NULL, sign_cb, &c, &bad);
        CHECK(e != MARMOT_OK, mode == 1 ? "refusing signer fails"
                              : mode == 2 ? "signer altering the template fails"
                                          : "signature by another key fails");
        marmot_key_package_result_free(&bad);
    }
    marmot_free(m);
}

int main(void) {
    if (sodium_init() < 0) return 1;
    test_proof_spec_vector();
    test_create_and_validate();
    test_signer_callback();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("All KeyPackage profile tests passed.\n");
    return 0;
}
