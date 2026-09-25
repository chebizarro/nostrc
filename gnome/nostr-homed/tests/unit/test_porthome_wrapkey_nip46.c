/*
 * test_porthome_wrapkey_nip46.c — end-to-end coverage of the wrap-key
 * hand-off orchestrator (bead nostrc-ck6i).
 *
 * SPDX-License-Identifier: MIT
 *
 * Exercises nh_auth_broker_porthome_maybe_enroll_or_unwrap_nip46 via
 * the test-only nip44 hooks (nh_auth_broker_porthome_set_nip44_hooks).
 * No real relay, no real bunker, no real signer — the hooks stand in
 * for nostr_nip46_client_nip44_encrypt_rpc / nip44_decrypt_rpc so this
 * unit can prove the orchestrator's three branches in isolation:
 *
 *   1. UNWRAP — the account already has a wrapped_home_key BLOB. The
 *      hook returns a known plaintext; the seed lands in the wrap-seed
 *      cache; a matching nh_auth_broker_porthome_take_wrap_seed hands
 *      it back byte-identical.
 *
 *   2. ENROLL — the account has no wrapped_home_key and
 *      porthome_enroll_wrap_key=on. A fresh seed is minted, the encrypt
 *      hook is asked to seal it, the returned ciphertext is persisted
 *      on the provider record, and the plaintext seed lands in the
 *      cache.
 *
 *   3. DENIED — the decrypt hook returns non-zero (models a signer
 *      policy denial or transport failure). The orchestrator returns
 *      -1, no seed is deposited, and take_wrap_seed reports absence.
 *      Login itself is unaffected (validated at the caller layer:
 *      provider_nip46 ignores this return code so PROVISION_HOME can
 *      fall back to LIMITED_MODE instead of failing the auth axis).
 */

#define _GNU_SOURCE
#include "auth_broker.h"
#include "auth_porthome.h"
#include "nostr_identity.h"
#include "nh_porthome_wrapkey.h"

#include <assert.h>
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define CK(expr, tag)                                            \
    do {                                                         \
        if (!(expr)) {                                           \
            fprintf(stderr, "FAIL(%s:%d): %s\n", __FILE__,       \
                    __LINE__, tag);                              \
            exit(1);                                             \
        }                                                        \
    } while (0)

/* Fixed 64-char lowercase-hex pubkey. The orchestrator only requires
 * strlen == 64 and does not verify against secp256k1 — the identity
 * store's schema check is length-only. Keeping this synthetic and
 * hard-coded lets the test link without libnostr's crypto surface,
 * which keeps the dep-purity gate quiet. */
static const char PUBKEY_HEX[65] =
    "0102030405060708090a0b0c0d0e0f10"
    "1112131415161718191a1b1c1d1e1f20";

static nh_identity_ownership_result all_free(void *c, const char *n,
                                             uint32_t u, uint32_t g) {
    (void)c; (void)n; (void)u; (void)g;
    return NH_IDENTITY_OWNERSHIP_FREE;
}

static nh_identity_store *open_store(const char *dir) {
    static nh_identity_config config;
    nh_identity_config_defaults(&config);
    snprintf(config.authority_path, sizeof config.authority_path,
             "%s/authority.db", dir);
    snprintf(config.projection_path, sizeof config.projection_path,
             "%s/nss.db", dir);
    snprintf(config.home_root, sizeof config.home_root, "%s/home", dir);
    nh_identity_store_options options = {0};
    options.config = &config;
    options.ownership_probe = all_free;
    options.flags = NH_IDENTITY_STORE_CREATE;
    nh_identity_store *store = NULL;
    CK(nh_identity_store_open(&options, &store) == NH_IDENTITY_OK,
       "open store");
    return store;
}

/* Enroll one account + one enabled NIP-46 provider record. Returns the
 * account struct and the provider_id via out params. Uses a fresh
 * pubkey (derived from SK_BYTES) so the store's UNIQUE(pubkey_hex)
 * constraint holds across the shared-directory lifetime of the test. */
static void seed_account_with_nip46_provider(nh_identity_store *store,
                                             nh_identity_account *acct_out,
                                             char provider_id_out[NH_IDENTITY_UUID_CAP]) {
    nh_identity_operation_state state;
    nh_identity_enroll_request req = {0};
    req.username = "n_wraptest";
    req.pubkey_hex = PUBKEY_HEX;
    req.home_mode = NH_IDENTITY_HOME_CREATE;
    CK(nh_identity_operation_begin_enroll(
           store, "00000000-0000-4000-8000-000000000001", &req, &state) ==
           NH_IDENTITY_OK,
       "begin_enroll");

    nh_identity_home_evidence staged = {11, 4001};
    nh_identity_home_evidence installed = {11, 4101};
    CK(nh_identity_operation_advance_home(
           store, "00000000-0000-4000-8000-000000000001",
           NH_IDENTITY_PHASE_RESERVED, NH_IDENTITY_PHASE_STAGED,
           &staged, &state) == NH_IDENTITY_OK, "stage home");
    CK(nh_identity_operation_advance_home(
           store, "00000000-0000-4000-8000-000000000001",
           NH_IDENTITY_PHASE_STAGED, NH_IDENTITY_PHASE_INSTALLED,
           &installed, &state) == NH_IDENTITY_OK, "install home");

    CK(nh_identity_store_lookup_by_name(store, "n_wraptest", acct_out) ==
           NH_IDENTITY_OK,
       "lookup account");

    /* Stage + activate a NIP-46 bunker provider. secret_blob is a
     * placeholder 32-byte transport key — never used by this test
     * because the orchestrator's nip44 RPC path is hooked. */
    const uint8_t placeholder_transport_secret[32] = {
        0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,
        0xa9,0xaa,0xab,0xac,0xad,0xae,0xaf,0xb0,
        0xb1,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,
        0xb9,0xba,0xbb,0xbc,0xbd,0xbe,0xbf,0xc0,
    };
    char cfg[256];
    snprintf(cfg, sizeof cfg,
             "{\"bunker_uri\":\"bunker://%s?secret=nhtest\"}", PUBKEY_HEX);
    CK(nh_identity_provider_stage(
           store, "00000000-0000-4000-8000-000000000002",
           acct_out->account_id, NH_IDENTITY_PROVIDER_NIP46_BUNKER, 1, cfg,
           placeholder_transport_secret, sizeof placeholder_transport_secret,
           provider_id_out) == NH_IDENTITY_OK,
       "stage nip46 provider");

    nh_identity_proof_attestation att = {0};
    strcpy(att.pubkey_hex, acct_out->pubkey_hex);
    att.key_generation = acct_out->key_generation;
    CK(nh_identity_provider_activate(
           store, "00000000-0000-4000-8000-000000000003",
           provider_id_out, &att) == NH_IDENTITY_OK,
       "activate nip46 provider");

    CK(nh_identity_store_publish_projection(store, NULL) == NH_IDENTITY_OK,
       "publish projection");
    CK(nh_identity_operation_activate(
           store, "00000000-0000-4000-8000-000000000001", &state) ==
           NH_IDENTITY_OK,
       "operation activate");
}

/* ── Hook state (thread-unsafe on purpose: single-thread test) ────── */

static const uint8_t *g_expected_plaintext_bytes;  /* for decrypt */
static size_t         g_expected_plaintext_len;
static int            g_decrypt_should_fail;
static int            g_decrypt_calls;
static char           g_last_peer_pubkey[65];

/* nostrc-8hc9: the hook can drive a specific wrap_err class on failure
 * so the DENIED sub-test can assert every branch of the log-class
 * mapping without spinning a real bunker. Zero means SIGNER_OFFLINE
 * (the pre-8hc9 default, exercised by the legacy DENIED case). */
static nh_auth_porthome_wrap_err g_decrypt_fail_err;

static int hook_nip44_decrypt(void *session,
                              const char *peer_pubkey_hex,
                              const char *in, char **out,
                              nh_auth_porthome_wrap_err *out_err,
                              void *ctx) {
    (void)session; (void)in; (void)ctx;
    g_decrypt_calls++;
    if (peer_pubkey_hex && strlen(peer_pubkey_hex) == 64)
        memcpy(g_last_peer_pubkey, peer_pubkey_hex, 65);
    if (g_decrypt_should_fail) {
        *out = NULL;
        if (out_err) *out_err = g_decrypt_fail_err;
        return -1;
    }
    /* Emit the expected plaintext as a NUL-terminated buffer. The
     * orchestrator uses strlen(pt) to size — the parser accepts 32-raw,
     * 64-hex or 65-with-NUL. We produce 64-hex here to keep the wire
     * strictly ASCII. */
    if (g_expected_plaintext_len == 64) {
        *out = malloc(65);
        if (!*out) return -1;
        memcpy(*out, g_expected_plaintext_bytes, 64);
        (*out)[64] = '\0';
        return 0;
    }
    /* Fallback for other shapes (32-raw). Copy verbatim into a heap
     * buffer with a trailing NUL so strlen is safe (only used for the
     * 32-raw branch where the parser inspects only pt_len == 32). */
    *out = malloc(g_expected_plaintext_len + 1);
    if (!*out) return -1;
    memcpy(*out, g_expected_plaintext_bytes, g_expected_plaintext_len);
    (*out)[g_expected_plaintext_len] = '\0';
    return 0;
}

/* Encrypt hook: stashes the plaintext (assumed to be the 64-hex seed
 * the orchestrator produces) and returns a stable ASCII "ciphertext"
 * that we then look for in the provider row. */
static char *g_last_encrypt_plaintext;    /* strdup'd */
static const char *g_encrypt_ct_out = "MOCK_NIP44_CT:00000000000000000000000000000000";
static int  g_encrypt_calls;
static int  g_encrypt_should_fail;

static int hook_nip44_encrypt(void *session,
                              const char *peer_pubkey_hex,
                              const char *in, char **out,
                              nh_auth_porthome_wrap_err *out_err,
                              void *ctx) {
    (void)session; (void)ctx;
    g_encrypt_calls++;
    if (peer_pubkey_hex && strlen(peer_pubkey_hex) == 64)
        memcpy(g_last_peer_pubkey, peer_pubkey_hex, 65);
    free(g_last_encrypt_plaintext);
    g_last_encrypt_plaintext = strdup(in ? in : "");
    if (g_encrypt_should_fail) {
        *out = NULL;
        if (out_err) *out_err = NH_AUTH_PORTHOME_WRAP_ERR_DENIED;
        return -1;
    }
    *out = strdup(g_encrypt_ct_out);
    return *out ? 0 : -1;
}

static void install_hooks(int with_encrypt, int with_decrypt) {
    nh_auth_broker_porthome_set_nip44_hooks(
        with_encrypt ? hook_nip44_encrypt : NULL, NULL,
        with_decrypt ? hook_nip44_decrypt : NULL, NULL);
}

static int rmtree_cb(const char *path, const struct stat *st, int typeflag,
                     struct FTW *ftw) {
    (void)st; (void)typeflag; (void)ftw;
    remove(path);
    return 0;
}
static void rmtree(const char *dir) {
    nftw(dir, rmtree_cb, 16, FTW_DEPTH | FTW_PHYS);
}

/* Small helper: drain any leftover deposit from a prior sub-test so
 * subsequent sub-tests observe a clean cache line. */
static void drain_cache(const char *account_id) {
    uint8_t tmp[32];
    (void)nh_auth_broker_porthome_take_wrap_seed(account_id, tmp);
}

/* ── Sub-tests ─────────────────────────────────────────────────────── */

static void test_unwrap(const char *dir) {
    nh_identity_store *store = open_store(dir);
    nh_identity_account acct;
    char provider_id[NH_IDENTITY_UUID_CAP];
    seed_account_with_nip46_provider(store, &acct, provider_id);

    /* Persist a placeholder wrapped_home_key blob (ASCII bytes so the
     * NUL-guard in the orchestrator does not trip). Value is opaque
     * to the mock — the hook returns a fixed plaintext regardless. */
    const uint8_t ct[] = "MOCK_NIP44_CT:existing_row";
    CK(nh_identity_provider_set_wrapped_home_key(
           store, provider_id, ct, sizeof ct - 1) == NH_IDENTITY_OK,
       "persist wrapped_home_key");

    /* Broker install: enrollment OFF (we're testing the unwrap arm),
     * decrypt timeout unused because the hook short-circuits. */
    nh_auth_broker_porthome_install(store, /*enroll=*/0, /*timeout=*/0);

    /* Expected plaintext: a known 32-byte seed encoded as 64 lowercase
     * hex — the shape the orchestrator's parser round-trips into the
     * canonical 32-byte seed. */
    static const uint8_t seed_expected[32] = {
        0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
        0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,
        0xf0,0xe0,0xd0,0xc0,0xb0,0xa0,0x90,0x80,
        0x70,0x60,0x50,0x40,0x30,0x20,0x10,0x00,
    };
    static const char seed_hex[65] =
        "0102030405060708090a0b0c0d0e0f10"
        "f0e0d0c0b0a0908070605040302010" "00";
    g_expected_plaintext_bytes = (const uint8_t *)seed_hex;
    g_expected_plaintext_len = 64;
    g_decrypt_should_fail = 0;
    g_decrypt_calls = 0;
    memset(g_last_peer_pubkey, 0, sizeof g_last_peer_pubkey);
    install_hooks(0, 1);

    /* Sentinel session pointer — the hook never dereferences it. */
    int rc = nh_auth_broker_porthome_maybe_enroll_or_unwrap_nip46(
        (void *)(uintptr_t)0xdeadbeef, acct.account_id, PUBKEY_HEX);
    CK(rc == 0, "unwrap: orchestrator OK");
    CK(g_decrypt_calls == 1, "unwrap: decrypt called once");
    CK(strncmp(g_last_peer_pubkey, PUBKEY_HEX, 64) == 0,
       "unwrap: decrypt peer == account pubkey");

    uint8_t got[32];
    CK(nh_auth_broker_porthome_take_wrap_seed(acct.account_id, got) == 0,
       "unwrap: seed present in cache");
    CK(memcmp(got, seed_expected, 32) == 0,
       "unwrap: seed matches expected plaintext");

    install_hooks(0, 0);
    nh_auth_broker_porthome_install(NULL, 0, 0);
    nh_identity_store_close(store);
}

static void test_enroll(const char *dir) {
    nh_identity_store *store = open_store(dir);
    nh_identity_account acct;
    char provider_id[NH_IDENTITY_UUID_CAP];
    seed_account_with_nip46_provider(store, &acct, provider_id);

    /* No wrapped_home_key set — this is the enrollment arm. */
    nh_auth_broker_porthome_install(store, /*enroll=*/1, /*timeout=*/0);

    g_encrypt_calls = 0;
    g_encrypt_should_fail = 0;
    free(g_last_encrypt_plaintext); g_last_encrypt_plaintext = NULL;
    memset(g_last_peer_pubkey, 0, sizeof g_last_peer_pubkey);
    install_hooks(1, 0);

    int rc = nh_auth_broker_porthome_maybe_enroll_or_unwrap_nip46(
        (void *)(uintptr_t)0xdeadbeef, acct.account_id, PUBKEY_HEX);
    CK(rc == 0, "enroll: orchestrator OK");
    CK(g_encrypt_calls == 1, "enroll: encrypt called once");
    CK(strncmp(g_last_peer_pubkey, PUBKEY_HEX, 64) == 0,
       "enroll: encrypt peer == account pubkey");
    /* The plaintext handed to nip44_encrypt is a 64-lowercase-hex
     * seed. Anything else is a regression against design §4.3. */
    CK(g_last_encrypt_plaintext && strlen(g_last_encrypt_plaintext) == 64,
       "enroll: plaintext is 64 chars");
    for (int i = 0; i < 64; i++) {
        char c = g_last_encrypt_plaintext[i];
        CK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'),
           "enroll: plaintext is lowercase hex");
    }

    /* Seed must be in the cache and equal the parsed-back plaintext. */
    uint8_t got[32];
    CK(nh_auth_broker_porthome_take_wrap_seed(acct.account_id, got) == 0,
       "enroll: seed present in cache");
    uint8_t parsed[32];
    CK(nh_porthome_wrap_seed_from_nip44_plaintext(
           (const uint8_t *)g_last_encrypt_plaintext, 64, parsed) ==
           NH_PORTHOME_OK,
       "enroll: parser accepts recorded plaintext");
    CK(memcmp(got, parsed, 32) == 0,
       "enroll: cached seed matches the encrypted plaintext bytes");

    /* Ciphertext persisted on the provider row. */
    nh_identity_provider_record rec;
    memset(&rec, 0, sizeof rec);
    CK(nh_identity_store_provider_get(store, acct.account_id,
                                      NH_IDENTITY_PROVIDER_NIP46_BUNKER,
                                      true, &rec) == NH_IDENTITY_OK,
       "enroll: read back provider");
    CK(rec.wrapped_home_key_len == strlen(g_encrypt_ct_out),
       "enroll: wrapped_home_key length matches mock ciphertext");
    CK(memcmp(rec.wrapped_home_key, g_encrypt_ct_out,
              rec.wrapped_home_key_len) == 0,
       "enroll: wrapped_home_key content matches mock ciphertext");

    install_hooks(0, 0);
    nh_auth_broker_porthome_install(NULL, 0, 0);
    nh_identity_store_close(store);
}

/* Bead nostrc-8hc9: drive each wrap_err class through the decrypt
 * hook and confirm the orchestrator persists the correct outcome
 * enum on the provider row. Log-tag verification is exercised
 * indirectly via the persisted enum since the pvha_log_class output
 * goes to syslog (not easily captured without shimming syslog(3)
 * inside the test binary) — the enum → log-tag mapping is a static
 * table in auth_porthome.c, so a persisted DENIED unambiguously
 * proves the "wrap-key-denied" tag was picked. */
static void test_denied(const char *dir) {
    nh_identity_store *store = open_store(dir);
    nh_identity_account acct;
    char provider_id[NH_IDENTITY_UUID_CAP];
    seed_account_with_nip46_provider(store, &acct, provider_id);

    /* Persist a wrapped_home_key so the orchestrator takes the unwrap
     * arm and hits the decrypt hook (rather than the enroll arm). */
    const uint8_t ct[] = "MOCK_NIP44_CT:existing_row";
    CK(nh_identity_provider_set_wrapped_home_key(
           store, provider_id, ct, sizeof ct - 1) == NH_IDENTITY_OK,
       "persist wrapped_home_key");

    nh_auth_broker_porthome_install(store, /*enroll=*/0, /*timeout=*/0);
    /* nostrc-2mri: disable the skip-cache for this sub-test so each
     * class-drive is a fresh RPC (the cache is exercised in
     * test_denied_cache below). */
    nh_auth_broker_porthome_set_wrap_key_denied_skip_sec(0);

    /* Drain any leftover from previous sub-tests. */
    drain_cache(acct.account_id);

    install_hooks(0, 1);

    struct {
        nh_auth_porthome_wrap_err err;
        nh_identity_wrap_key_outcome persisted;
        const char *tag;
    } cases[] = {
        {NH_AUTH_PORTHOME_WRAP_ERR_SIGNER_OFFLINE,
         NH_IDENTITY_WRAP_KEY_OUTCOME_SIGNER_OFFLINE, "signer-offline"},
        {NH_AUTH_PORTHOME_WRAP_ERR_DENIED,
         NH_IDENTITY_WRAP_KEY_OUTCOME_DENIED, "denied"},
        {NH_AUTH_PORTHOME_WRAP_ERR_DECRYPT_FAILED,
         NH_IDENTITY_WRAP_KEY_OUTCOME_DECRYPT_FAILED, "decrypt-failed"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        g_decrypt_should_fail = 1;
        g_decrypt_fail_err = cases[i].err;
        g_decrypt_calls = 0;

        int rc = nh_auth_broker_porthome_maybe_enroll_or_unwrap_nip46(
            (void *)(uintptr_t)0xdeadbeef, acct.account_id, PUBKEY_HEX);
        CK(rc == -1, "denied: orchestrator surfaces -1");
        CK(g_decrypt_calls == 1, "denied: decrypt called exactly once");

        /* No seed deposited => cache take returns error. */
        uint8_t tmp[32];
        CK(nh_auth_broker_porthome_take_wrap_seed(acct.account_id, tmp) != 0,
           "denied: seed-cache empty for this account");

        /* Provider row should record the expected outcome + a fresh ts. */
        nh_identity_provider_record rec;
        memset(&rec, 0, sizeof rec);
        CK(nh_identity_store_provider_get(
               store, acct.account_id, NH_IDENTITY_PROVIDER_NIP46_BUNKER,
               true, &rec) == NH_IDENTITY_OK,
           "denied: provider row readable");
        CK((int)rec.wrap_key_outcome == (int)cases[i].persisted,
           cases[i].tag);
        CK(rec.wrap_key_outcome_ts > 0,
           "denied: wrap_key_outcome_ts populated");
    }

    /* Also drive the enrollment arm's DENIED classification: no
     * wrapped_home_key + enroll on + encrypt hook fails. Reset the
     * row first. */
    nh_auth_broker_porthome_install(NULL, 0, 0);
    CK(nh_identity_provider_wrap_key_reset(store, provider_id) ==
           NH_IDENTITY_OK,
       "denied: reset cache row for enroll sub-case");
    CK(nh_identity_provider_set_wrapped_home_key(store, provider_id, NULL, 0)
           == NH_IDENTITY_OK,
       "denied: clear wrapped_home_key for enroll sub-case");
    nh_auth_broker_porthome_install(store, /*enroll=*/1, /*timeout=*/0);
    nh_auth_broker_porthome_set_wrap_key_denied_skip_sec(0);
    g_encrypt_calls = 0;
    g_encrypt_should_fail = 1;
    install_hooks(1, 1);
    int rc = nh_auth_broker_porthome_maybe_enroll_or_unwrap_nip46(
        (void *)(uintptr_t)0xdeadbeef, acct.account_id, PUBKEY_HEX);
    CK(rc == -1, "denied: enrollment arm surfaces -1");
    CK(g_encrypt_calls == 1, "denied: enrollment encrypt called once");
    nh_identity_provider_record rec;
    memset(&rec, 0, sizeof rec);
    CK(nh_identity_store_provider_get(
           store, acct.account_id, NH_IDENTITY_PROVIDER_NIP46_BUNKER,
           true, &rec) == NH_IDENTITY_OK,
       "denied: provider row readable after enroll denied");
    CK(rec.wrap_key_outcome == NH_IDENTITY_WRAP_KEY_OUTCOME_DENIED,
       "denied: enrollment failure persists DENIED (hook returned DENIED)");
    g_encrypt_should_fail = 0;

    install_hooks(0, 0);
    nh_auth_broker_porthome_install(NULL, 0, 0);
    nh_auth_broker_porthome_set_wrap_key_denied_skip_sec(
        900u); /* restore default so later sub-tests are unaffected */
    nh_identity_store_close(store);
}

/* Bead nostrc-2mri: exercise the per-provider skip cache directly.
 *
 * Seeds the provider row with wrap_key_ok_last_outcome + ts, then
 * verifies:
 *  (a) DENIED + ts within TTL → orchestrator SKIPS the RPC and
 *      surfaces -1 without calling the hook;
 *  (b) DENIED + ts OUTSIDE TTL → orchestrator DOES call the RPC;
 *  (c) SIGNER_OFFLINE + ts within TTL → orchestrator DOES call the
 *      RPC (transient class never latches);
 *  (d) A signer PAIRING reset clears the row and unblocks the RPC;
 *  (e) TTL=0 disables the cache — DENIED + fresh ts still calls. */
static void test_denied_cache(const char *dir) {
    nh_identity_store *store = open_store(dir);
    nh_identity_account acct;
    char provider_id[NH_IDENTITY_UUID_CAP];
    seed_account_with_nip46_provider(store, &acct, provider_id);

    const uint8_t ct[] = "MOCK_NIP44_CT:existing_row";
    CK(nh_identity_provider_set_wrapped_home_key(
           store, provider_id, ct, sizeof ct - 1) == NH_IDENTITY_OK,
       "cache: persist wrapped_home_key");

    nh_auth_broker_porthome_install(store, /*enroll=*/0, /*timeout=*/0);
    nh_auth_broker_porthome_set_wrap_key_denied_skip_sec(900);
    drain_cache(acct.account_id);
    install_hooks(0, 1);

    uint64_t now = (uint64_t)time(NULL);

    /* (a) DENIED within TTL → skip. */
    CK(nh_identity_provider_set_wrap_key_outcome(
           store, provider_id, NH_IDENTITY_WRAP_KEY_OUTCOME_DENIED,
           now - 100) == NH_IDENTITY_OK,
       "cache: seed DENIED ts=now-100");
    g_decrypt_calls = 0;
    g_decrypt_should_fail = 1;
    g_decrypt_fail_err = NH_AUTH_PORTHOME_WRAP_ERR_DENIED;
    int rc = nh_auth_broker_porthome_maybe_enroll_or_unwrap_nip46(
        (void *)(uintptr_t)0xdeadbeef, acct.account_id, PUBKEY_HEX);
    CK(rc == -1, "cache: skip returns -1");
    CK(g_decrypt_calls == 0,
       "cache: DENIED within TTL SKIPS the hook");

    /* (b) DENIED outside TTL → call. */
    CK(nh_identity_provider_set_wrap_key_outcome(
           store, provider_id, NH_IDENTITY_WRAP_KEY_OUTCOME_DENIED,
           now - 1000) == NH_IDENTITY_OK,
       "cache: seed DENIED ts=now-1000");
    g_decrypt_calls = 0;
    rc = nh_auth_broker_porthome_maybe_enroll_or_unwrap_nip46(
        (void *)(uintptr_t)0xdeadbeef, acct.account_id, PUBKEY_HEX);
    CK(rc == -1, "cache: stale DENIED still returns -1 via RPC");
    CK(g_decrypt_calls == 1,
       "cache: DENIED outside TTL CALLS the hook");

    /* (c) SIGNER_OFFLINE within TTL → call (never latches). */
    CK(nh_identity_provider_set_wrap_key_outcome(
           store, provider_id, NH_IDENTITY_WRAP_KEY_OUTCOME_SIGNER_OFFLINE,
           now - 10) == NH_IDENTITY_OK,
       "cache: seed SIGNER_OFFLINE ts=now-10");
    g_decrypt_calls = 0;
    rc = nh_auth_broker_porthome_maybe_enroll_or_unwrap_nip46(
        (void *)(uintptr_t)0xdeadbeef, acct.account_id, PUBKEY_HEX);
    CK(g_decrypt_calls == 1,
       "cache: SIGNER_OFFLINE never latches; RPC is retried");
    (void)rc;

    /* (d) Reset via the exposed helper clears any latched entry. */
    CK(nh_identity_provider_set_wrap_key_outcome(
           store, provider_id, NH_IDENTITY_WRAP_KEY_OUTCOME_DENIED,
           now - 5) == NH_IDENTITY_OK,
       "cache: seed DENIED for reset test");
    nh_auth_broker_porthome_wrap_key_reset(provider_id);
    nh_identity_provider_record rec;
    memset(&rec, 0, sizeof rec);
    CK(nh_identity_store_provider_get(
           store, acct.account_id, NH_IDENTITY_PROVIDER_NIP46_BUNKER,
           true, &rec) == NH_IDENTITY_OK,
       "cache: read provider row after reset");
    CK(rec.wrap_key_outcome == NH_IDENTITY_WRAP_KEY_OUTCOME_UNSET,
       "cache: reset clears outcome to UNSET");
    CK(rec.wrap_key_outcome_ts == 0,
       "cache: reset clears ts to 0");

    /* (e) TTL=0 disables the cache. */
    nh_auth_broker_porthome_set_wrap_key_denied_skip_sec(0);
    CK(nh_identity_provider_set_wrap_key_outcome(
           store, provider_id, NH_IDENTITY_WRAP_KEY_OUTCOME_DENIED,
           now) == NH_IDENTITY_OK,
       "cache: seed DENIED for TTL=0 case");
    g_decrypt_calls = 0;
    rc = nh_auth_broker_porthome_maybe_enroll_or_unwrap_nip46(
        (void *)(uintptr_t)0xdeadbeef, acct.account_id, PUBKEY_HEX);
    CK(g_decrypt_calls == 1,
       "cache: TTL=0 disables skip; RPC is called");
    (void)rc;

    install_hooks(0, 0);
    nh_auth_broker_porthome_install(NULL, 0, 0);
    nh_auth_broker_porthome_set_wrap_key_denied_skip_sec(900);
    nh_identity_store_close(store);
}

int main(void) {
    /* Each sub-test gets its own scratch directory so the identity
     * store's exclusive-file lock and UNIQUE constraints don't collide
     * across cases. */
    char tmpl[] = "/tmp/nhwrap.XXXXXX";
    char *base = mkdtemp(tmpl);
    CK(base, "mkdtemp");

    char d1[128], d2[128], d3[128], d4[128];
    snprintf(d1, sizeof d1, "%s/unwrap", base);
    snprintf(d2, sizeof d2, "%s/enroll", base);
    snprintf(d3, sizeof d3, "%s/denied", base);
    snprintf(d4, sizeof d4, "%s/cache", base);
    CK(mkdir(d1, 0700) == 0, "mkdir unwrap");
    CK(mkdir(d2, 0700) == 0, "mkdir enroll");
    CK(mkdir(d3, 0700) == 0, "mkdir denied");
    CK(mkdir(d4, 0700) == 0, "mkdir cache");

    test_unwrap(d1);
    printf("PASS: unwrap round-trip\n");

    test_enroll(d2);
    printf("PASS: enroll round-trip\n");

    test_denied(d3);
    printf("PASS: denied class discrimination\n");

    test_denied_cache(d4);
    printf("PASS: denied-cache skip / reset / ttl\n");

    free(g_last_encrypt_plaintext);
    rmtree(base);
    printf("OK test_porthome_wrapkey_nip46\n");
    return 0;
}
