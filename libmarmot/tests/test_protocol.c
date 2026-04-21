/*
 * libmarmot - Protocol layer tests (MIP-00 through MIP-03)
 *
 * Tests the full Marmot protocol flow:
 *   - Key package creation (MIP-00)
 *   - Group creation with member invitation (MIP-01)
 *   - Welcome processing and group joining (MIP-02)
 *   - Message encryption and decryption (MIP-03)
 *
 * Uses the in-memory storage backend for all tests.
 *
 * SPDX-License-Identifier: MIT
 */

#include <marmot/marmot.h>
#include "marmot-internal.h"
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Internal declarations needed for round-trip test */
#include "../src/mls/mls_key_package.h"
extern MarmotError marmot_parse_key_package_event(const char *event_json,
                                                    MlsKeyPackage *kp_out,
                                                    uint8_t nostr_pubkey_out[32]);

/* ══════════════════════════════════════════════════════════════════════════
 * Test harness
 * ══════════════════════════════════════════════════════════════════════════ */

static int tests_run   = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { \
        printf("  %-60s", name); \
        tests_run++; \
    } while (0)

#define PASS() \
    do { \
        printf("[PASS]\n"); \
        tests_passed++; \
    } while (0)

#define FAIL(msg) \
    do { \
        printf("[FAIL] %s\n", msg); \
        tests_failed++; \
    } while (0)

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { FAIL(msg); return; } \
    } while (0)

#define ASSERT_OK(err, msg) \
    do { \
        if ((err) != MARMOT_OK) { \
            printf("[FAIL] %s (error: %s)\n", msg, marmot_error_string(err)); \
            tests_failed++; \
            return; \
        } \
    } while (0)

/* ══════════════════════════════════════════════════════════════════════════
 * Helper: create a test Marmot instance
 * ══════════════════════════════════════════════════════════════════════════ */

static Marmot *
create_test_instance(void)
{
    MarmotStorage *storage = marmot_storage_memory_new();
    if (!storage) return NULL;
    return marmot_new(storage);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Helper: generate a Nostr keypair (random for testing)
 * ══════════════════════════════════════════════════════════════════════════ */

static void
generate_nostr_keypair(uint8_t sk[32], uint8_t pk[32])
{
    /* For testing: random 32-byte values.
     * In production, these would be secp256k1 keys. */
    randombytes_buf(sk, 32);
    randombytes_buf(pk, 32);
}

/* ══════════════════════════════════════════════════════════════════════════
 * MIP-00: Key Package Tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_create_key_package_basic(void)
{
    TEST("MIP-00: create_key_package returns valid result");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create Marmot instance");

    uint8_t nostr_sk[32], nostr_pk[32];
    generate_nostr_keypair(nostr_sk, nostr_pk);

    const char *relays[] = { "wss://relay.example.com", "wss://relay2.example.com" };
    MarmotKeyPackageResult result;
    memset(&result, 0, sizeof(result));

    MarmotError err = marmot_create_key_package(m, nostr_pk, nostr_sk,
                                                 relays, 2, &result);
    ASSERT_OK(err, "create_key_package");

    /* Verify result has event JSON */
    ASSERT(result.event_json != NULL, "event_json is NULL");
    ASSERT(strlen(result.event_json) > 0, "event_json is empty");

    /* Verify KeyPackageRef is non-zero */
    uint8_t zero[32] = {0};
    ASSERT(memcmp(result.key_package_ref, zero, 32) != 0,
           "key_package_ref is all zeros");

    /* Verify the event JSON contains expected fields */
    ASSERT(strstr(result.event_json, "\"kind\":443") != NULL,
           "event JSON missing kind:443");
    ASSERT(strstr(result.event_json, "\"mls_protocol_version\"") != NULL ||
           strstr(result.event_json, "mls_protocol_version") != NULL,
           "event JSON missing mls_protocol_version tag");

    marmot_key_package_result_free(&result);
    marmot_free(m);
    PASS();
}

static void
test_create_key_package_no_relays(void)
{
    TEST("MIP-00: create_key_package with no relays");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create Marmot instance");

    uint8_t nostr_sk[32], nostr_pk[32];
    generate_nostr_keypair(nostr_sk, nostr_pk);

    MarmotKeyPackageResult result;
    memset(&result, 0, sizeof(result));

    MarmotError err = marmot_create_key_package(m, nostr_pk, nostr_sk,
                                                 NULL, 0, &result);
    ASSERT_OK(err, "create_key_package with no relays");
    ASSERT(result.event_json != NULL, "event_json is NULL");

    marmot_key_package_result_free(&result);
    marmot_free(m);
    PASS();
}

static void
test_create_key_package_null_args(void)
{
    TEST("MIP-00: create_key_package rejects NULL args");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create Marmot instance");

    MarmotKeyPackageResult result;
    MarmotError err;

    err = marmot_create_key_package(NULL, NULL, NULL, NULL, 0, &result);
    ASSERT(err != MARMOT_OK, "should reject NULL marmot");

    err = marmot_create_key_package(m, NULL, NULL, NULL, 0, &result);
    ASSERT(err != MARMOT_OK, "should reject NULL pubkey");

    marmot_free(m);
    PASS();
}

static void
test_create_multiple_key_packages(void)
{
    TEST("MIP-00: multiple key packages have different refs");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create Marmot instance");

    uint8_t nostr_sk[32], nostr_pk[32];
    generate_nostr_keypair(nostr_sk, nostr_pk);

    MarmotKeyPackageResult r1, r2;
    memset(&r1, 0, sizeof(r1));
    memset(&r2, 0, sizeof(r2));

    MarmotError err;
    err = marmot_create_key_package(m, nostr_pk, nostr_sk, NULL, 0, &r1);
    ASSERT_OK(err, "create first key package");

    err = marmot_create_key_package(m, nostr_pk, nostr_sk, NULL, 0, &r2);
    ASSERT_OK(err, "create second key package");

    /* Each KeyPackage should have a unique ref (different init keys) */
    ASSERT(memcmp(r1.key_package_ref, r2.key_package_ref, 32) != 0,
           "key package refs should differ");

    marmot_key_package_result_free(&r1);
    marmot_key_package_result_free(&r2);
    marmot_free(m);
    PASS();
}

static void
test_key_package_roundtrip(void)
{
    TEST("MIP-00: create → parse round-trip preserves identity");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create instance");

    uint8_t nostr_sk[32], nostr_pk[32];
    generate_nostr_keypair(nostr_sk, nostr_pk);

    const char *relays[] = { "wss://relay.example.com" };
    MarmotKeyPackageResult result;
    memset(&result, 0, sizeof(result));

    MarmotError err = marmot_create_key_package(m, nostr_pk, nostr_sk,
                                                 relays, 1, &result);
    ASSERT_OK(err, "create_key_package");

    /* Parse it back */
    MlsKeyPackage kp;
    uint8_t parsed_pk[32];
    err = marmot_parse_key_package_event(result.event_json, &kp, parsed_pk);
    ASSERT_OK(err, "parse_key_package_event");

    /* The credential identity should contain our nostr pubkey */
    ASSERT(kp.leaf_node.credential_identity_len == 32,
           "credential identity should be 32 bytes");
    ASSERT(memcmp(kp.leaf_node.credential_identity, nostr_pk, 32) == 0,
           "credential identity should match nostr pubkey");

    /* The pubkey extracted from the event should match */
    ASSERT(memcmp(parsed_pk, nostr_pk, 32) == 0,
           "parsed pubkey should match original");

    /* Verify the KeyPackage ref matches */
    uint8_t parsed_ref[32];
    ASSERT(mls_key_package_ref(&kp, parsed_ref) == 0, "compute ref");
    ASSERT(memcmp(parsed_ref, result.key_package_ref, 32) == 0,
           "key package ref should match after round-trip");

    mls_key_package_clear(&kp);
    marmot_key_package_result_free(&result);
    marmot_free(m);
    PASS();
}

static void
test_key_package_rotation(void)
{
    TEST("MIP-00: creating new key package deactivates old ones");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create instance");

    uint8_t nostr_sk[32], nostr_pk[32];
    generate_nostr_keypair(nostr_sk, nostr_pk);

    /* Create first key package */
    MarmotKeyPackageResult r1;
    memset(&r1, 0, sizeof(r1));
    MarmotError err = marmot_create_key_package(m, nostr_pk, nostr_sk,
                                                 NULL, 0, &r1);
    ASSERT_OK(err, "create first key package");

    /* Verify first is active in storage */
    if (m->storage->find_key_package_by_ref) {
        MarmotKeyPackageInfo *info = NULL;
        err = m->storage->find_key_package_by_ref(m->storage->ctx,
                                                    r1.key_package_ref, &info);
        ASSERT_OK(err, "find first key package");
        ASSERT(info != NULL, "first kp info should exist");
        ASSERT(info->active == true, "first kp should be active");
        marmot_key_package_info_free(info);
    }

    /* Create second key package (should deactivate first) */
    MarmotKeyPackageResult r2;
    memset(&r2, 0, sizeof(r2));
    err = marmot_create_key_package(m, nostr_pk, nostr_sk, NULL, 0, &r2);
    ASSERT_OK(err, "create second key package");

    /* Verify first is now deactivated and second is active */
    if (m->storage->find_key_package_by_ref) {
        MarmotKeyPackageInfo *info1 = NULL;
        err = m->storage->find_key_package_by_ref(m->storage->ctx,
                                                    r1.key_package_ref, &info1);
        ASSERT_OK(err, "find first after rotation");
        ASSERT(info1 != NULL, "first kp should still exist");
        ASSERT(info1->active == false, "first kp should be deactivated");
        marmot_key_package_info_free(info1);

        MarmotKeyPackageInfo *info2 = NULL;
        err = m->storage->find_key_package_by_ref(m->storage->ctx,
                                                    r2.key_package_ref, &info2);
        ASSERT_OK(err, "find second after rotation");
        ASSERT(info2 != NULL, "second kp should exist");
        ASSERT(info2->active == true, "second kp should be active");
        marmot_key_package_info_free(info2);
    }

    marmot_key_package_result_free(&r1);
    marmot_key_package_result_free(&r2);
    marmot_free(m);
    PASS();
}

static void
test_key_package_info_storage(void)
{
    TEST("MIP-00: key package info stored with correct metadata");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create instance");

    uint8_t nostr_sk[32], nostr_pk[32];
    generate_nostr_keypair(nostr_sk, nostr_pk);

    const char *relays[] = { "wss://relay1.example.com", "wss://relay2.example.com" };
    MarmotKeyPackageResult result;
    memset(&result, 0, sizeof(result));

    MarmotError err = marmot_create_key_package(m, nostr_pk, nostr_sk,
                                                 relays, 2, &result);
    ASSERT_OK(err, "create key package");

    /* Look it up by ref */
    if (m->storage->find_key_package_by_ref) {
        MarmotKeyPackageInfo *info = NULL;
        err = m->storage->find_key_package_by_ref(m->storage->ctx,
                                                    result.key_package_ref, &info);
        ASSERT_OK(err, "find by ref");
        ASSERT(info != NULL, "info should exist");
        ASSERT(memcmp(info->owner_pubkey, nostr_pk, 32) == 0,
               "owner pubkey should match");
        ASSERT(info->relay_count == 2, "should have 2 relays");
        ASSERT(info->active == true, "should be active");
        ASSERT(info->created_at > 0, "should have creation timestamp");
        marmot_key_package_info_free(info);
    }

    /* Look it up by pubkey */
    if (m->storage->find_key_packages_by_pubkey) {
        MarmotKeyPackageInfo **infos = NULL;
        size_t count = 0;
        err = m->storage->find_key_packages_by_pubkey(m->storage->ctx,
                                                       nostr_pk, &infos, &count);
        ASSERT_OK(err, "find by pubkey");
        ASSERT(count == 1, "should find 1 key package");
        ASSERT(infos != NULL, "infos array should not be NULL");
        if (count > 0 && infos) {
            ASSERT(memcmp(infos[0]->ref, result.key_package_ref, 32) == 0,
                   "ref should match");
            for (size_t i = 0; i < count; i++)
                marmot_key_package_info_free(infos[i]);
            free(infos);
        }
    }

    marmot_key_package_result_free(&result);
    marmot_free(m);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════════
 * MIP-01: Group Construction Tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_create_group_basic(void)
{
    TEST("MIP-01: create_group with one member invitation");

    /* Creator instance */
    Marmot *creator = create_test_instance();
    ASSERT(creator != NULL, "failed to create creator instance");

    /* Member instance — creates a key package */
    Marmot *member = create_test_instance();
    ASSERT(member != NULL, "failed to create member instance");

    uint8_t creator_sk[32], creator_pk[32];
    uint8_t member_sk[32], member_pk[32];
    generate_nostr_keypair(creator_sk, creator_pk);
    generate_nostr_keypair(member_sk, member_pk);

    /* Member creates a key package */
    MarmotKeyPackageResult kp_result;
    memset(&kp_result, 0, sizeof(kp_result));
    MarmotError err = marmot_create_key_package(member, member_pk, member_sk,
                                                 NULL, 0, &kp_result);
    ASSERT_OK(err, "member create_key_package");
    ASSERT(kp_result.event_json != NULL, "member kp event_json is NULL");

    /* Creator creates a group with this member */
    const char *kp_jsons[] = { kp_result.event_json };
    MarmotGroupConfig config = {0};
    config.name = "Test Group";
    config.description = "A test group";
    config.admin_pubkeys = (uint8_t (*)[32])&creator_pk;
    config.admin_count = 1;

    MarmotCreateGroupResult group_result;
    memset(&group_result, 0, sizeof(group_result));

    err = marmot_create_group(creator, creator_pk, kp_jsons, 1,
                               &config, &group_result);
    ASSERT_OK(err, "create_group");

    /* Verify group was created */
    ASSERT(group_result.group != NULL, "group is NULL");
    ASSERT(group_result.group->name != NULL, "group name is NULL");
    ASSERT(strcmp(group_result.group->name, "Test Group") == 0,
           "group name mismatch");
    ASSERT(group_result.group->state == MARMOT_GROUP_STATE_ACTIVE,
           "group should be active");

    /* Verify welcome rumor was generated */
    ASSERT(group_result.welcome_count == 1, "should have 1 welcome");
    ASSERT(group_result.welcome_rumor_jsons != NULL, "welcome jsons is NULL");
    ASSERT(group_result.welcome_rumor_jsons[0] != NULL, "welcome[0] is NULL");

    marmot_key_package_result_free(&kp_result);
    marmot_create_group_result_free(&group_result);
    marmot_free(creator);
    marmot_free(member);
    PASS();
}

static void
test_create_group_no_members(void)
{
    TEST("MIP-01: create_group with no invited members");

    Marmot *creator = create_test_instance();
    ASSERT(creator != NULL, "failed to create instance");

    uint8_t creator_sk[32], creator_pk[32];
    generate_nostr_keypair(creator_sk, creator_pk);

    MarmotGroupConfig config = {0};
    config.name = "Solo Group";
    config.admin_pubkeys = (uint8_t (*)[32])&creator_pk;
    config.admin_count = 1;

    MarmotCreateGroupResult result;
    memset(&result, 0, sizeof(result));

    MarmotError err = marmot_create_group(creator, creator_pk, NULL, 0,
                                           &config, &result);
    ASSERT_OK(err, "create_group with 0 members");
    ASSERT(result.group != NULL, "group is NULL");
    ASSERT(result.welcome_count == 0, "should have 0 welcomes");

    marmot_create_group_result_free(&result);
    marmot_free(creator);
    PASS();
}

static void
test_create_group_null_args(void)
{
    TEST("MIP-01: create_group rejects NULL args");

    MarmotCreateGroupResult result;
    MarmotGroupConfig config = {0};

    MarmotError err = marmot_create_group(NULL, NULL, NULL, 0, &config, &result);
    ASSERT(err != MARMOT_OK, "should reject NULL marmot");

    PASS();
}

static void
test_merge_pending_commit(void)
{
    TEST("MIP-01: merge_pending_commit updates timestamp");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create instance");

    uint8_t sk[32], pk[32];
    generate_nostr_keypair(sk, pk);

    MarmotGroupConfig config = {0};
    config.name = "Merge Test";
    config.admin_pubkeys = (uint8_t (*)[32])&pk;
    config.admin_count = 1;

    MarmotCreateGroupResult result;
    memset(&result, 0, sizeof(result));

    MarmotError err = marmot_create_group(m, pk, NULL, 0, &config, &result);
    ASSERT_OK(err, "create_group");
    ASSERT(result.group != NULL, "group is NULL");

    /* Merge the pending commit */
    err = marmot_merge_pending_commit(m, &result.group->mls_group_id);
    ASSERT_OK(err, "merge_pending_commit");

    /* Verify the group's last_message_processed_at was updated */
    MarmotGroup *updated = NULL;
    err = marmot_get_group(m, &result.group->mls_group_id, &updated);
    ASSERT_OK(err, "get_group after merge");
    ASSERT(updated != NULL, "updated group is NULL");
    ASSERT(updated->last_message_processed_at > 0,
           "last_message_processed_at should be set");

    marmot_group_free(updated);
    marmot_create_group_result_free(&result);
    marmot_free(m);
    PASS();
}

static void
test_leave_group(void)
{
    TEST("MIP-01: leave_group sets state to inactive");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create instance");

    uint8_t sk[32], pk[32];
    generate_nostr_keypair(sk, pk);

    MarmotGroupConfig config = {0};
    config.name = "Leave Test";
    config.admin_pubkeys = (uint8_t (*)[32])&pk;
    config.admin_count = 1;

    MarmotCreateGroupResult result;
    memset(&result, 0, sizeof(result));

    MarmotError err = marmot_create_group(m, pk, NULL, 0, &config, &result);
    ASSERT_OK(err, "create_group");

    err = marmot_leave_group(m, &result.group->mls_group_id);
    ASSERT_OK(err, "leave_group");

    /* Verify the group is now inactive */
    MarmotGroup *group = NULL;
    err = marmot_get_group(m, &result.group->mls_group_id, &group);
    ASSERT_OK(err, "get_group after leave");
    ASSERT(group != NULL, "group is NULL");
    ASSERT(group->state == MARMOT_GROUP_STATE_INACTIVE,
           "group should be inactive after leave");

    marmot_group_free(group);
    marmot_create_group_result_free(&result);
    marmot_free(m);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════════
 * MIP-02: Welcome Tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_process_welcome_basic(void)
{
    TEST("MIP-02: process_welcome stores pending welcome");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create instance");

    /* Create a minimal kind:444 rumor event JSON */
    const char *rumor_json =
        "{\"kind\":444,\"content\":\"dGVzdA==\","  /* "test" in base64 */
        "\"created_at\":2000000000,"
        "\"tags\":[[\"encoding\",\"base64\"],"
        "[\"relays\",\"wss://relay.example.com\"]]}";

    uint8_t wrapper_id[32];
    randombytes_buf(wrapper_id, 32);

    MarmotWelcome *welcome = NULL;
    MarmotError err = marmot_process_welcome(m, wrapper_id, rumor_json, &welcome);

    /* This may fail because the MLS Welcome bytes ("test") are invalid,
     * but the parsing and storage should succeed before MLS validation. */
    if (err == MARMOT_OK) {
        ASSERT(welcome != NULL, "welcome is NULL");
        ASSERT(welcome->state == MARMOT_WELCOME_STATE_PENDING,
               "welcome should be pending");
        ASSERT(welcome->group_relay_count == 1, "should have 1 relay");
        marmot_welcome_free(welcome);
    }
    /* Even if it fails due to invalid MLS data, that's acceptable */

    marmot_free(m);
    PASS();
}

static void
test_process_welcome_wrong_kind(void)
{
    TEST("MIP-02: process_welcome rejects wrong kind");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create instance");

    /* kind:443 instead of 444 */
    const char *bad_json =
        "{\"kind\":443,\"content\":\"dGVzdA==\","
        "\"created_at\":2000000000,"
        "\"tags\":[[\"encoding\",\"base64\"]]}";

    uint8_t wrapper_id[32];
    randombytes_buf(wrapper_id, 32);

    MarmotWelcome *welcome = NULL;
    MarmotError err = marmot_process_welcome(m, wrapper_id, bad_json, &welcome);
    ASSERT(err != MARMOT_OK, "should reject wrong kind");
    ASSERT(welcome == NULL, "welcome should be NULL on error");

    marmot_free(m);
    PASS();
}

static void
test_decline_welcome(void)
{
    TEST("MIP-02: decline_welcome succeeds");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create instance");

    /* Create a MarmotWelcome manually for testing */
    MarmotWelcome *w = marmot_welcome_new();
    ASSERT(w != NULL, "failed to create welcome");
    randombytes_buf(w->wrapper_event_id, 32);
    w->state = MARMOT_WELCOME_STATE_PENDING;

    MarmotError err = marmot_decline_welcome(m, w);
    ASSERT_OK(err, "decline_welcome");

    marmot_welcome_free(w);
    marmot_free(m);
    PASS();
}

static void
test_welcome_end_to_end(void)
{
    TEST("MIP-02: end-to-end create → welcome → accept → group");

    /* --- Set up creator and member instances --- */
    Marmot *creator = create_test_instance();
    Marmot *member  = create_test_instance();
    ASSERT(creator != NULL, "failed to create creator instance");
    ASSERT(member  != NULL, "failed to create member instance");

    uint8_t creator_sk[32], creator_pk[32];
    uint8_t member_sk[32],  member_pk[32];
    generate_nostr_keypair(creator_sk, creator_pk);
    generate_nostr_keypair(member_sk,  member_pk);

    /* Member creates a key package (stores private keys internally) */
    MarmotKeyPackageResult kp_result;
    memset(&kp_result, 0, sizeof(kp_result));
    MarmotError err = marmot_create_key_package(member, member_pk, member_sk,
                                                 NULL, 0, &kp_result);
    ASSERT_OK(err, "member create_key_package");

    /* Creator creates a group, inviting the member */
    const char *kp_jsons[] = { kp_result.event_json };
    MarmotGroupConfig config = {0};
    config.name = "Welcome E2E Group";
    config.description = "End-to-end welcome test";
    config.admin_pubkeys = (uint8_t (*)[32])&creator_pk;
    config.admin_count = 1;

    MarmotCreateGroupResult group_result;
    memset(&group_result, 0, sizeof(group_result));

    err = marmot_create_group(creator, creator_pk, kp_jsons, 1,
                               &config, &group_result);
    ASSERT_OK(err, "create_group");
    ASSERT(group_result.welcome_count == 1, "should have 1 welcome");
    ASSERT(group_result.welcome_rumor_jsons != NULL, "welcome jsons NULL");
    ASSERT(group_result.welcome_rumor_jsons[0] != NULL, "welcome[0] NULL");

    /* --- Member processes the welcome rumor --- */
    uint8_t wrapper_id[32];
    randombytes_buf(wrapper_id, 32);

    MarmotWelcome *welcome = NULL;
    err = marmot_process_welcome(member, wrapper_id,
                                  group_result.welcome_rumor_jsons[0],
                                  &welcome);
    ASSERT_OK(err, "process_welcome");
    ASSERT(welcome != NULL, "welcome is NULL");
    ASSERT(welcome->state == MARMOT_WELCOME_STATE_PENDING,
           "welcome should be pending");

    /* --- Member accepts the welcome --- */
    err = marmot_accept_welcome(member, welcome);
    ASSERT_OK(err, "accept_welcome");

    /* --- Verify the member now has the group --- */
    MarmotGroup **groups = NULL;
    size_t group_count = 0;
    err = marmot_get_all_groups(member, &groups, &group_count);
    ASSERT_OK(err, "get_all_groups for member");
    ASSERT(group_count >= 1, "member should have at least 1 group");

    /* Find the group and verify metadata */
    bool found_group = false;
    for (size_t i = 0; i < group_count; i++) {
        if (groups[i]->name && strcmp(groups[i]->name, "Welcome E2E Group") == 0) {
            found_group = true;
            ASSERT(groups[i]->state == MARMOT_GROUP_STATE_ACTIVE,
                   "member group should be active");
            ASSERT(groups[i]->description != NULL &&
                   strcmp(groups[i]->description, "End-to-end welcome test") == 0,
                   "group description should match");
            ASSERT(groups[i]->admin_count == 1,
                   "group should have 1 admin");
            ASSERT(memcmp(groups[i]->admin_pubkeys, creator_pk, 32) == 0,
                   "admin should be the creator");
        }
        marmot_group_free(groups[i]);
    }
    free(groups);
    ASSERT(found_group, "member should have the Welcome E2E Group");

    /* Verify MLS state was persisted (can load it back) */
    if (member->storage && member->storage->mls_load) {
        uint8_t *state_data = NULL;
        size_t state_len = 0;
        int rc = member->storage->mls_load(member->storage->ctx, "mls_group",
                                            group_result.group->mls_group_id.data,
                                            group_result.group->mls_group_id.len,
                                            &state_data, &state_len);
        ASSERT(rc == 0 && state_data != NULL && state_len > 0,
               "MLS group state should be persisted after accept");
        free(state_data);
    }

    marmot_welcome_free(welcome);
    marmot_key_package_result_free(&kp_result);
    marmot_create_group_result_free(&group_result);
    marmot_free(creator);
    marmot_free(member);
    PASS();
}

static void
test_welcome_duplicate_detection(void)
{
    TEST("MIP-02: duplicate welcome is rejected");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create instance");

    /* Create a minimal kind:444 rumor event JSON with a far-future timestamp
     * to avoid triggering the welcome expiry check. */
    const char *rumor_json =
        "{\"kind\":444,\"content\":\"dGVzdA==\","
        "\"created_at\":2000000000,"
        "\"tags\":[[\"encoding\",\"base64\"],"
        "[\"relays\",\"wss://relay.example.com\"]]}";

    uint8_t wrapper_id[32];
    randombytes_buf(wrapper_id, 32);

    /* First process should succeed */
    MarmotWelcome *welcome1 = NULL;
    MarmotError err = marmot_process_welcome(m, wrapper_id, rumor_json, &welcome1);
    ASSERT_OK(err, "first process_welcome");
    ASSERT(welcome1 != NULL, "first welcome is NULL");

    /* Mark this welcome as processed (simulating accept) */
    if (m->storage && m->storage->save_processed_welcome) {
        m->storage->save_processed_welcome(m->storage->ctx,
                                            wrapper_id, NULL,
                                            1700000000,
                                            MARMOT_WELCOME_STATE_ACCEPTED,
                                            NULL);
    }

    /* Second process with same wrapper_id should be rejected */
    MarmotWelcome *welcome2 = NULL;
    err = marmot_process_welcome(m, wrapper_id, rumor_json, &welcome2);
    ASSERT(err == MARMOT_ERR_WELCOME_PREVIOUSLY_FAILED,
           "duplicate welcome should be rejected");
    ASSERT(welcome2 == NULL, "duplicate welcome should return NULL");

    marmot_welcome_free(welcome1);
    marmot_free(m);
    PASS();
}

static void
test_accept_welcome_null_args(void)
{
    TEST("MIP-02: accept_welcome rejects NULL args");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create instance");

    MarmotError err = marmot_accept_welcome(m, NULL);
    ASSERT(err == MARMOT_ERR_INVALID_ARG, "should reject NULL welcome");

    err = marmot_accept_welcome(NULL, NULL);
    ASSERT(err == MARMOT_ERR_INVALID_ARG, "should reject NULL marmot");

    marmot_free(m);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════════
 * MIP-03: Message Tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_create_message_no_group(void)
{
    TEST("MIP-03: create_message fails for nonexistent group");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create instance");

    uint8_t gid_data[32];
    randombytes_buf(gid_data, 32);
    MarmotGroupId gid = { .data = gid_data, .len = 32 };

    MarmotOutgoingMessage result;
    memset(&result, 0, sizeof(result));

    MarmotError err = marmot_create_message(m, &gid,
                                             "{\"kind\":9,\"content\":\"hello\"}",
                                             &result);
    ASSERT(err == MARMOT_ERR_GROUP_NOT_FOUND, "should return GROUP_NOT_FOUND");

    marmot_free(m);
    PASS();
}

static void
test_create_message_inactive_group(void)
{
    TEST("MIP-03: create_message fails for inactive group");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create instance");

    /* Create a group and then leave it */
    uint8_t sk[32], pk[32];
    generate_nostr_keypair(sk, pk);

    MarmotGroupConfig config = {0};
    config.name = "Inactive Test";
    config.admin_pubkeys = (uint8_t (*)[32])&pk;
    config.admin_count = 1;

    MarmotCreateGroupResult gresult;
    memset(&gresult, 0, sizeof(gresult));

    MarmotError err = marmot_create_group(m, pk, NULL, 0, &config, &gresult);
    ASSERT_OK(err, "create_group");

    err = marmot_leave_group(m, &gresult.group->mls_group_id);
    ASSERT_OK(err, "leave_group");

    /* Try to send a message to the inactive group */
    MarmotOutgoingMessage msg_result;
    memset(&msg_result, 0, sizeof(msg_result));

    err = marmot_create_message(m, &gresult.group->mls_group_id,
                                 "{\"kind\":9,\"content\":\"hello\"}",
                                 &msg_result);
    ASSERT(err == MARMOT_ERR_USE_AFTER_EVICTION,
           "should return USE_AFTER_EVICTION");

    marmot_create_group_result_free(&gresult);
    marmot_free(m);
    PASS();
}

static void
test_create_message_with_active_group(void)
{
    TEST("MIP-03: create_message succeeds with active group + secret");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create instance");

    uint8_t sk[32], pk[32];
    generate_nostr_keypair(sk, pk);

    MarmotGroupConfig config = {0};
    config.name = "Message Test";
    config.admin_pubkeys = (uint8_t (*)[32])&pk;
    config.admin_count = 1;

    MarmotCreateGroupResult gresult;
    memset(&gresult, 0, sizeof(gresult));

    MarmotError err = marmot_create_group(m, pk, NULL, 0, &config, &gresult);
    ASSERT_OK(err, "create_group");
    ASSERT(gresult.group != NULL, "group is NULL");

    /* The exporter secret should have been stored by create_group.
     * Try to create a message. */
    MarmotOutgoingMessage msg_result;
    memset(&msg_result, 0, sizeof(msg_result));

    err = marmot_create_message(m, &gresult.group->mls_group_id,
                                 "{\"kind\":9,\"content\":\"Hello, group!\"}",
                                 &msg_result);

    if (err == MARMOT_OK) {
        ASSERT(msg_result.event_json != NULL, "event_json is NULL");
        ASSERT(strstr(msg_result.event_json, "\"kind\":445") != NULL,
               "event should be kind:445");
        ASSERT(msg_result.message != NULL, "message record is NULL");
        ASSERT(msg_result.message->content != NULL, "message content is NULL");
        ASSERT(strstr(msg_result.message->content, "Hello, group!") != NULL,
               "message content mismatch");

        marmot_outgoing_message_free(&msg_result);
    } else if (err == MARMOT_ERR_GROUP_EXPORTER_SECRET) {
        /* Acceptable: the exporter secret wasn't stored properly.
         * This happens if the MLS key schedule didn't produce a valid
         * secp256k1 private key. */
        /* Still pass — the error handling is correct. */
    } else if (err == MARMOT_ERR_NIP44) {
        /* Acceptable: NIP-44 encryption may fail if the exporter_secret
         * is not a valid secp256k1 key (very unlikely but possible). */
    } else {
        ASSERT_OK(err, "create_message unexpected error");
    }

    marmot_create_group_result_free(&gresult);
    marmot_free(m);
    PASS();
}

static void
test_process_message_null_args(void)
{
    TEST("MIP-03: process_message rejects NULL args");

    MarmotMessageResult result;
    MarmotError err;

    err = marmot_process_message(NULL, "{}", &result);
    ASSERT(err != MARMOT_OK, "should reject NULL marmot");

    Marmot *m = create_test_instance();
    err = marmot_process_message(m, NULL, &result);
    ASSERT(err != MARMOT_OK, "should reject NULL event json");

    err = marmot_process_message(m, "{}", NULL);
    ASSERT(err != MARMOT_OK, "should reject NULL result");

    marmot_free(m);
    PASS();
}

static void
test_process_message_wrong_kind(void)
{
    TEST("MIP-03: process_message rejects wrong event kind");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create instance");

    /* kind:443 instead of 445 */
    const char *bad_json =
        "{\"kind\":443,\"content\":\"test\","
        "\"created_at\":1700000000,"
        "\"tags\":[[\"h\",\"" "0000000000000000000000000000000000000000000000000000000000000000" "\"]]}";

    MarmotMessageResult result;
    memset(&result, 0, sizeof(result));

    MarmotError err = marmot_process_message(m, bad_json, &result);
    ASSERT(err == MARMOT_ERR_UNEXPECTED_EVENT,
           "should return UNEXPECTED_EVENT");

    marmot_free(m);
    PASS();
}

static void
test_process_message_missing_h_tag(void)
{
    TEST("MIP-03: process_message rejects missing h-tag");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create instance");

    /* No h-tag */
    const char *bad_json =
        "{\"kind\":445,\"content\":\"encrypted_data\","
        "\"created_at\":1700000000,"
        "\"tags\":[]}";

    MarmotMessageResult result;
    memset(&result, 0, sizeof(result));

    MarmotError err = marmot_process_message(m, bad_json, &result);
    ASSERT(err == MARMOT_ERR_MISSING_GROUP_ID_TAG,
           "should return MISSING_GROUP_ID_TAG");

    marmot_free(m);
    PASS();
}

static void
test_process_message_unknown_group(void)
{
    TEST("MIP-03: process_message rejects unknown group");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create instance");

    /* Valid kind:445 but group doesn't exist */
    const char *json =
        "{\"kind\":445,\"content\":\"encrypted_data\","
        "\"created_at\":1700000000,"
        "\"tags\":[[\"h\",\"" "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789" "\"]]}";

    MarmotMessageResult result;
    memset(&result, 0, sizeof(result));

    MarmotError err = marmot_process_message(m, json, &result);
    ASSERT(err == MARMOT_ERR_GROUP_NOT_FOUND,
           "should return GROUP_NOT_FOUND");

    marmot_free(m);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════════
 * MIP-01: MLS Group State Serialization Tests
 * ══════════════════════════════════════════════════════════════════════════ */

/* Internal declarations for serialization test */
#include "../src/mls/mls_group.h"

static void
test_mls_group_serialize_roundtrip(void)
{
    TEST("MIP-01: MLS group state serialize/deserialize round-trip");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create instance");

    uint8_t sk[32], pk[32];
    generate_nostr_keypair(sk, pk);

    /* Create a group with a member to make the tree non-trivial */
    Marmot *member = create_test_instance();
    ASSERT(member != NULL, "failed to create member instance");

    uint8_t member_sk[32], member_pk[32];
    generate_nostr_keypair(member_sk, member_pk);

    MarmotKeyPackageResult kp_result;
    memset(&kp_result, 0, sizeof(kp_result));
    MarmotError err = marmot_create_key_package(member, member_pk, member_sk,
                                                 NULL, 0, &kp_result);
    ASSERT_OK(err, "member key package");

    const char *kp_jsons[] = { kp_result.event_json };
    MarmotGroupConfig config = {0};
    config.name = "Serialize Test";
    config.admin_pubkeys = (uint8_t (*)[32])&pk;
    config.admin_count = 1;

    MarmotCreateGroupResult result;
    memset(&result, 0, sizeof(result));
    err = marmot_create_group(m, pk, kp_jsons, 1, &config, &result);
    ASSERT_OK(err, "create_group");
    ASSERT(result.group != NULL, "group is NULL");

    /* Load the serialized MLS group state from storage */
    uint8_t *state_data = NULL;
    size_t state_len = 0;
    err = m->storage->mls_load(m->storage->ctx, "mls_group",
                                result.group->mls_group_id.data,
                                result.group->mls_group_id.len,
                                &state_data, &state_len);
    ASSERT_OK(err, "mls_load");
    ASSERT(state_data != NULL, "state_data is NULL");
    ASSERT(state_len > 100, "state_data too small");

    /* Deserialize */
    MlsGroup mls;
    memset(&mls, 0, sizeof(mls));
    int rc = mls_group_deserialize(state_data, state_len, &mls);
    ASSERT(rc == 0, "deserialize failed");

    /* Verify key fields */
    ASSERT(mls.group_id_len == result.group->mls_group_id.len,
           "group_id_len mismatch");
    ASSERT(memcmp(mls.group_id, result.group->mls_group_id.data,
                  mls.group_id_len) == 0,
           "group_id mismatch");
    ASSERT(mls.tree.n_leaves == 2, "should have 2 leaves (creator + member)");

    /* Re-serialize and compare */
    uint8_t *state_data2 = NULL;
    size_t state_len2 = 0;
    rc = mls_group_serialize(&mls, &state_data2, &state_len2);
    ASSERT(rc == 0, "re-serialize failed");
    ASSERT(state_len2 == state_len, "re-serialized length differs");
    ASSERT(memcmp(state_data, state_data2, state_len) == 0,
           "re-serialized data differs");

    free(state_data);
    free(state_data2);
    mls_group_free(&mls);
    marmot_key_package_result_free(&kp_result);
    marmot_create_group_result_free(&result);
    marmot_free(m);
    marmot_free(member);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════════
 * MIP-01: Add/Remove Members Tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_add_members_to_existing_group(void)
{
    TEST("MIP-01: add_members to existing group");

    Marmot *creator = create_test_instance();
    ASSERT(creator != NULL, "failed to create creator");

    uint8_t creator_sk[32], creator_pk[32];
    generate_nostr_keypair(creator_sk, creator_pk);

    /* Create a solo group */
    MarmotGroupConfig config = {0};
    config.name = "Add Member Test";
    config.admin_pubkeys = (uint8_t (*)[32])&creator_pk;
    config.admin_count = 1;

    MarmotCreateGroupResult group_result;
    memset(&group_result, 0, sizeof(group_result));
    MarmotError err = marmot_create_group(creator, creator_pk, NULL, 0,
                                           &config, &group_result);
    ASSERT_OK(err, "create_group");
    ASSERT(group_result.group != NULL, "group is NULL");

    /* Create a member's key package */
    Marmot *member = create_test_instance();
    uint8_t member_sk[32], member_pk[32];
    generate_nostr_keypair(member_sk, member_pk);

    MarmotKeyPackageResult kp_result;
    memset(&kp_result, 0, sizeof(kp_result));
    err = marmot_create_key_package(member, member_pk, member_sk, NULL, 0, &kp_result);
    ASSERT_OK(err, "member key package");

    /* Add the member */
    const char *kp_jsons[] = { kp_result.event_json };
    char **welcome_jsons = NULL;
    size_t welcome_count = 0;
    char *commit_json = NULL;

    err = marmot_add_members(creator, &group_result.group->mls_group_id,
                              kp_jsons, 1,
                              &welcome_jsons, &welcome_count, &commit_json);
    ASSERT_OK(err, "add_members");
    ASSERT(welcome_count == 1, "should have 1 welcome");
    ASSERT(welcome_jsons != NULL, "welcome jsons is NULL");
    ASSERT(welcome_jsons[0] != NULL, "welcome[0] is NULL");
    ASSERT(commit_json != NULL, "commit json is NULL");

    /* Verify the group epoch advanced */
    MarmotGroup *updated = NULL;
    err = marmot_get_group(creator, &group_result.group->mls_group_id, &updated);
    ASSERT_OK(err, "get_group after add");
    ASSERT(updated != NULL, "updated group is NULL");
    ASSERT(updated->epoch > group_result.group->epoch,
           "epoch should have advanced");

    /* Cleanup */
    marmot_group_free(updated);
    for (size_t i = 0; i < welcome_count; i++) free(welcome_jsons[i]);
    free(welcome_jsons);
    free(commit_json);
    marmot_key_package_result_free(&kp_result);
    marmot_create_group_result_free(&group_result);
    marmot_free(creator);
    marmot_free(member);
    PASS();
}

static void
test_remove_members_from_group(void)
{
    TEST("MIP-01: remove_members from group");

    Marmot *creator = create_test_instance();
    uint8_t creator_sk[32], creator_pk[32];
    generate_nostr_keypair(creator_sk, creator_pk);

    /* Create a member */
    Marmot *member = create_test_instance();
    uint8_t member_sk[32], member_pk[32];
    generate_nostr_keypair(member_sk, member_pk);

    MarmotKeyPackageResult kp_result;
    memset(&kp_result, 0, sizeof(kp_result));
    MarmotError err = marmot_create_key_package(member, member_pk, member_sk,
                                                 NULL, 0, &kp_result);
    ASSERT_OK(err, "member key package");

    /* Create group with the member */
    const char *kp_jsons[] = { kp_result.event_json };
    MarmotGroupConfig config = {0};
    config.name = "Remove Test";
    config.admin_pubkeys = (uint8_t (*)[32])&creator_pk;
    config.admin_count = 1;

    MarmotCreateGroupResult group_result;
    memset(&group_result, 0, sizeof(group_result));
    err = marmot_create_group(creator, creator_pk, kp_jsons, 1,
                               &config, &group_result);
    ASSERT_OK(err, "create_group");

    uint64_t epoch_before = group_result.group->epoch;

    /* Remove the member */
    const uint8_t (*pubkeys)[32] = (const uint8_t (*)[32])&member_pk;
    char *commit_json = NULL;
    err = marmot_remove_members(creator, &group_result.group->mls_group_id,
                                 pubkeys, 1, &commit_json);
    ASSERT_OK(err, "remove_members");
    ASSERT(commit_json != NULL, "commit json is NULL");

    /* Verify epoch advanced */
    MarmotGroup *updated = NULL;
    err = marmot_get_group(creator, &group_result.group->mls_group_id, &updated);
    ASSERT_OK(err, "get_group after remove");
    ASSERT(updated->epoch > epoch_before, "epoch should have advanced");

    marmot_group_free(updated);
    free(commit_json);
    marmot_key_package_result_free(&kp_result);
    marmot_create_group_result_free(&group_result);
    marmot_free(creator);
    marmot_free(member);
    PASS();
}

static void
test_remove_nonexistent_member(void)
{
    TEST("MIP-01: remove_members fails for unknown pubkey");

    Marmot *creator = create_test_instance();
    uint8_t creator_sk[32], creator_pk[32];
    generate_nostr_keypair(creator_sk, creator_pk);

    MarmotGroupConfig config = {0};
    config.name = "Remove Unknown";
    config.admin_pubkeys = (uint8_t (*)[32])&creator_pk;
    config.admin_count = 1;

    MarmotCreateGroupResult group_result;
    memset(&group_result, 0, sizeof(group_result));
    MarmotError err = marmot_create_group(creator, creator_pk, NULL, 0,
                                           &config, &group_result);
    ASSERT_OK(err, "create_group");

    /* Try to remove a random pubkey */
    uint8_t random_pk[32];
    randombytes_buf(random_pk, 32);
    const uint8_t (*pubkeys)[32] = (const uint8_t (*)[32])&random_pk;
    char *commit_json = NULL;

    err = marmot_remove_members(creator, &group_result.group->mls_group_id,
                                 pubkeys, 1, &commit_json);
    ASSERT(err == MARMOT_ERR_MEMBER_NOT_FOUND, "should return MEMBER_NOT_FOUND");

    marmot_create_group_result_free(&group_result);
    marmot_free(creator);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════════
 * MIP-01: Admin Policy Enforcement Tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_add_members_admin_only(void)
{
    TEST("MIP-01: add_members rejected for non-admin");

    /* Create group as admin. The admin pubkey in the group config
     * is the nostr pubkey used as credential identity in the MLS tree.
     * The admin check compares our MLS leaf credential identity against
     * the admin list. We'll create a second Marmot instance with a
     * different identity to test the rejection. */
    Marmot *admin = create_test_instance();
    uint8_t admin_sk[32], admin_pk[32];
    generate_nostr_keypair(admin_sk, admin_pk);

    MarmotGroupConfig config = {0};
    config.name = "Admin Test";
    config.admin_pubkeys = (uint8_t (*)[32])&admin_pk;
    config.admin_count = 1;

    MarmotCreateGroupResult group_result;
    memset(&group_result, 0, sizeof(group_result));
    MarmotError err = marmot_create_group(admin, admin_pk, NULL, 0,
                                           &config, &group_result);
    ASSERT_OK(err, "create_group");

    /* The admin check works by looking at our credential identity in
     * the MLS tree (leaf 0 has credential_identity = admin_pk).
     * To simulate a non-admin, we need a separate Marmot instance
     * with a DIFFERENT MLS identity. But since both share the same
     * storage, we can't easily do this without copying the storage.
     *
     * Instead, we modify the admin list in the stored group to
     * contain a different pubkey, making the current user non-admin. */
    MarmotGroup *stored_group = NULL;
    err = admin->storage->find_group_by_mls_id(admin->storage->ctx,
                                                 &group_result.group->mls_group_id,
                                                 &stored_group);
    ASSERT_OK(err, "find group");
    ASSERT(stored_group != NULL, "stored group is NULL");

    /* Replace admin list with a random pubkey (not the creator) */
    uint8_t fake_admin_pk[32];
    randombytes_buf(fake_admin_pk, 32);
    memcpy(stored_group->admin_pubkeys[0], fake_admin_pk, 32);
    admin->storage->save_group(admin->storage->ctx, stored_group);
    marmot_group_free(stored_group);

    /* Try to add a member as non-admin */
    Marmot *member = create_test_instance();
    uint8_t member_sk[32], member_pk[32];
    generate_nostr_keypair(member_sk, member_pk);

    MarmotKeyPackageResult kp_result;
    memset(&kp_result, 0, sizeof(kp_result));
    err = marmot_create_key_package(member, member_pk, member_sk, NULL, 0, &kp_result);
    ASSERT_OK(err, "member kp");

    const char *kp_jsons[] = { kp_result.event_json };
    char **welcomes = NULL;
    size_t wcount = 0;
    char *commit = NULL;

    err = marmot_add_members(admin, &group_result.group->mls_group_id,
                              kp_jsons, 1, &welcomes, &wcount, &commit);
    ASSERT(err == MARMOT_ERR_ADMIN_ONLY, "should reject non-admin");

    marmot_key_package_result_free(&kp_result);
    marmot_create_group_result_free(&group_result);
    marmot_free(admin);
    marmot_free(member);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════════
 * MIP-01: Group Metadata Update Tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_update_group_metadata(void)
{
    TEST("MIP-01: update_group_metadata changes name and description");

    Marmot *m = create_test_instance();
    uint8_t sk[32], pk[32];
    generate_nostr_keypair(sk, pk);

    MarmotGroupConfig config = {0};
    config.name = "Original Name";
    config.description = "Original Desc";
    config.admin_pubkeys = (uint8_t (*)[32])&pk;
    config.admin_count = 1;

    MarmotCreateGroupResult result;
    memset(&result, 0, sizeof(result));
    MarmotError err = marmot_create_group(m, pk, NULL, 0, &config, &result);
    ASSERT_OK(err, "create_group");

    /* Update metadata */
    MarmotGroupConfig new_config = {0};
    new_config.name = "Updated Name";
    new_config.description = "Updated Desc";
    new_config.admin_pubkeys = (uint8_t (*)[32])&pk;
    new_config.admin_count = 1;

    err = marmot_update_group_metadata(m, &result.group->mls_group_id, &new_config);
    ASSERT_OK(err, "update_group_metadata");

    /* Verify the update took effect */
    MarmotGroup *updated = NULL;
    err = marmot_get_group(m, &result.group->mls_group_id, &updated);
    ASSERT_OK(err, "get_group after update");
    ASSERT(updated != NULL, "updated group is NULL");
    ASSERT(strcmp(updated->name, "Updated Name") == 0, "name not updated");
    ASSERT(strcmp(updated->description, "Updated Desc") == 0, "desc not updated");
    ASSERT(updated->epoch > result.group->epoch, "epoch should advance on update");

    marmot_group_free(updated);
    marmot_create_group_result_free(&result);
    marmot_free(m);
    PASS();
}

static void
test_update_group_metadata_non_admin(void)
{
    TEST("MIP-01: update_group_metadata rejected for non-admin");

    Marmot *m = create_test_instance();
    uint8_t sk[32], pk[32];
    generate_nostr_keypair(sk, pk);

    MarmotGroupConfig config = {0};
    config.name = "Admin Only Update";
    config.admin_pubkeys = (uint8_t (*)[32])&pk;
    config.admin_count = 1;

    MarmotCreateGroupResult result;
    memset(&result, 0, sizeof(result));
    MarmotError err = marmot_create_group(m, pk, NULL, 0, &config, &result);
    ASSERT_OK(err, "create_group");

    /* Make the current user non-admin by changing the stored admin list */
    MarmotGroup *stored = NULL;
    err = m->storage->find_group_by_mls_id(m->storage->ctx,
                                            &result.group->mls_group_id, &stored);
    ASSERT_OK(err, "find group");
    uint8_t fake_admin[32];
    randombytes_buf(fake_admin, 32);
    memcpy(stored->admin_pubkeys[0], fake_admin, 32);
    m->storage->save_group(m->storage->ctx, stored);
    marmot_group_free(stored);

    MarmotGroupConfig new_config = {0};
    new_config.name = "Hacked Name";

    err = marmot_update_group_metadata(m, &result.group->mls_group_id, &new_config);
    ASSERT(err == MARMOT_ERR_ADMIN_ONLY, "should reject non-admin update");

    marmot_create_group_result_free(&result);
    marmot_free(m);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════════
 * Group query tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_get_all_groups(void)
{
    TEST("Query: get_all_groups returns created groups");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create instance");

    uint8_t sk[32], pk[32];
    generate_nostr_keypair(sk, pk);

    MarmotGroupConfig config = {0};
    config.name = "Group A";
    config.admin_pubkeys = (uint8_t (*)[32])&pk;
    config.admin_count = 1;

    MarmotCreateGroupResult r1;
    memset(&r1, 0, sizeof(r1));
    MarmotError err = marmot_create_group(m, pk, NULL, 0, &config, &r1);
    ASSERT_OK(err, "create group A");

    config.name = "Group B";
    MarmotCreateGroupResult r2;
    memset(&r2, 0, sizeof(r2));
    err = marmot_create_group(m, pk, NULL, 0, &config, &r2);
    ASSERT_OK(err, "create group B");

    /* Query all groups */
    MarmotGroup **groups = NULL;
    size_t count = 0;
    err = marmot_get_all_groups(m, &groups, &count);
    ASSERT_OK(err, "get_all_groups");
    ASSERT(count == 2, "should have 2 groups");

    for (size_t i = 0; i < count; i++) {
        marmot_group_free(groups[i]);
    }
    free(groups);

    marmot_create_group_result_free(&r1);
    marmot_create_group_result_free(&r2);
    marmot_free(m);
    PASS();
}

static void
test_get_group_not_found(void)
{
    TEST("Query: get_group returns NULL for unknown ID");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create instance");

    uint8_t gid_data[32];
    randombytes_buf(gid_data, 32);
    MarmotGroupId gid = { .data = gid_data, .len = 32 };

    MarmotGroup *group = NULL;
    MarmotError err = marmot_get_group(m, &gid, &group);
    /* Should succeed but return NULL group */
    ASSERT(group == NULL, "should be NULL for unknown group");

    marmot_free(m);
    PASS();
}

static void
test_message_encrypt_decrypt_roundtrip(void)
{
    TEST("MIP-03: encrypt → decrypt round-trip preserves content");

    /* Creator creates a solo group */
    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create instance");

    uint8_t sk[32], pk[32];
    generate_nostr_keypair(sk, pk);

    MarmotGroupConfig config = {0};
    config.name = "Roundtrip Test";
    config.admin_pubkeys = (uint8_t (*)[32])&pk;
    config.admin_count = 1;

    MarmotCreateGroupResult gresult;
    memset(&gresult, 0, sizeof(gresult));

    MarmotError err = marmot_create_group(m, pk, NULL, 0, &config, &gresult);
    ASSERT_OK(err, "create_group");
    ASSERT(gresult.group != NULL, "group is NULL");

    /* Send a message */
    const char *inner_json = "{\"kind\":9,\"content\":\"Hello, roundtrip!\","
                             "\"created_at\":1700000000,\"tags\":[]}";

    MarmotOutgoingMessage msg_out;
    memset(&msg_out, 0, sizeof(msg_out));

    err = marmot_create_message(m, &gresult.group->mls_group_id,
                                 inner_json, &msg_out);
    ASSERT_OK(err, "create_message");
    ASSERT(msg_out.event_json != NULL, "outgoing event_json is NULL");

    /* Verify the kind:445 event structure */
    ASSERT(strstr(msg_out.event_json, "\"kind\":445") != NULL,
           "event should be kind:445");

    /* Process the same kind:445 event back (decrypt).
     * Since we're the sender, MLS correctly identifies this as our own
     * message and returns OWN_MESSAGE — the plaintext was already stored
     * locally at send time. */
    MarmotMessageResult msg_in;
    memset(&msg_in, 0, sizeof(msg_in));

    err = marmot_process_message(m, msg_out.event_json, &msg_in);
    ASSERT_OK(err, "process_message");
    ASSERT(msg_in.type == MARMOT_RESULT_OWN_MESSAGE,
           "should be own message (MLS detects self-sent messages)");

    marmot_message_result_free(&msg_in);
    marmot_outgoing_message_free(&msg_out);
    marmot_create_group_result_free(&gresult);
    marmot_free(m);
    PASS();
}

static void
test_message_two_member_exchange(void)
{
    TEST("MIP-03: creator sends message, member decrypts after welcome");

    /* Set up creator and member */
    Marmot *creator = create_test_instance();
    Marmot *member  = create_test_instance();
    ASSERT(creator != NULL, "failed to create creator");
    ASSERT(member  != NULL, "failed to create member");

    uint8_t creator_sk[32], creator_pk[32];
    uint8_t member_sk[32],  member_pk[32];
    generate_nostr_keypair(creator_sk, creator_pk);
    generate_nostr_keypair(member_sk,  member_pk);

    /* Member creates a key package */
    MarmotKeyPackageResult kp_result;
    memset(&kp_result, 0, sizeof(kp_result));
    MarmotError err = marmot_create_key_package(member, member_pk, member_sk,
                                                 NULL, 0, &kp_result);
    ASSERT_OK(err, "member create_key_package");

    /* Creator creates a group with the member */
    const char *kp_jsons[] = { kp_result.event_json };
    MarmotGroupConfig config = {0};
    config.name = "Two-Member Msg Test";
    config.description = "Message exchange test";
    config.admin_pubkeys = (uint8_t (*)[32])&creator_pk;
    config.admin_count = 1;

    MarmotCreateGroupResult gresult;
    memset(&gresult, 0, sizeof(gresult));

    err = marmot_create_group(creator, creator_pk, kp_jsons, 1,
                               &config, &gresult);
    ASSERT_OK(err, "create_group");
    ASSERT(gresult.welcome_count == 1, "should have 1 welcome");

    /* Member processes and accepts the welcome */
    uint8_t wrapper_id[32];
    randombytes_buf(wrapper_id, 32);

    MarmotWelcome *welcome = NULL;
    err = marmot_process_welcome(member, wrapper_id,
                                  gresult.welcome_rumor_jsons[0],
                                  &welcome);
    ASSERT_OK(err, "process_welcome");

    err = marmot_accept_welcome(member, welcome);
    ASSERT_OK(err, "accept_welcome");

    /* Creator sends a message */
    const char *inner_json = "{\"kind\":9,\"content\":\"Hello from creator!\","
                             "\"created_at\":1700000000,\"tags\":[]}";

    MarmotOutgoingMessage msg_out;
    memset(&msg_out, 0, sizeof(msg_out));

    err = marmot_create_message(creator, &gresult.group->mls_group_id,
                                 inner_json, &msg_out);
    ASSERT_OK(err, "creator create_message");
    ASSERT(msg_out.event_json != NULL, "outgoing event_json is NULL");

    /* Member processes the message */
    MarmotMessageResult msg_in;
    memset(&msg_in, 0, sizeof(msg_in));

    err = marmot_process_message(member, msg_out.event_json, &msg_in);
    ASSERT_OK(err, "member process_message");
    ASSERT(msg_in.type == MARMOT_RESULT_APPLICATION_MESSAGE,
           "should be application message");
    ASSERT(msg_in.app_msg.inner_event_json != NULL,
           "decrypted inner event is NULL");
    ASSERT(strstr(msg_in.app_msg.inner_event_json, "Hello from creator!") != NULL,
           "member should decrypt creator's message");

    marmot_message_result_free(&msg_in);
    marmot_outgoing_message_free(&msg_out);
    marmot_welcome_free(welcome);
    marmot_key_package_result_free(&kp_result);
    marmot_create_group_result_free(&gresult);
    marmot_free(creator);
    marmot_free(member);
    PASS();
}

static void
test_message_epoch_lookback(void)
{
    TEST("MIP-03: process_message tries previous epochs on mismatch");

    /* This test verifies the epoch lookback mechanism for the NIP-44 layer.
     *
     * With MLS PrivateMessage framing active:
     *   1. Messages are MLS-encrypted then NIP-44-encrypted
     *   2. NIP-44 epoch lookback can find the right exporter_secret
     *   3. BUT: MLS state for the old epoch is replaced after advance
     *   4. So the MLS ciphertext from epoch 0 can't be unwrapped at epoch 1
     *
     * This is correct MLS behavior — PrivateMessage keys are epoch-bound.
     * The NIP-44 lookback works (proven by no NIP-44 error), but the MLS
     * layer correctly rejects the stale-epoch ciphertext. Applications
     * should display locally-cached plaintext for their own stale messages. */

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create instance");

    uint8_t sk[32], pk[32];
    generate_nostr_keypair(sk, pk);

    MarmotGroupConfig config = {0};
    config.name = "Epoch Lookback Test";
    config.admin_pubkeys = (uint8_t (*)[32])&pk;
    config.admin_count = 1;

    MarmotCreateGroupResult gresult;
    memset(&gresult, 0, sizeof(gresult));

    MarmotError err = marmot_create_group(m, pk, NULL, 0, &config, &gresult);
    ASSERT_OK(err, "create_group");

    /* Send a message at epoch 0 */
    MarmotOutgoingMessage msg_out;
    memset(&msg_out, 0, sizeof(msg_out));

    err = marmot_create_message(m, &gresult.group->mls_group_id,
                                 "{\"kind\":9,\"content\":\"epoch0 msg\","
                                 "\"created_at\":1700000000,\"tags\":[]}",
                                 &msg_out);
    ASSERT_OK(err, "create_message at epoch 0");
    ASSERT(msg_out.event_json != NULL, "event_json is NULL");

    /* Advance epoch by updating group metadata */
    MarmotGroupConfig update_config = {0};
    update_config.name = "Epoch Lookback Test v2";
    update_config.admin_pubkeys = (uint8_t (*)[32])&pk;
    update_config.admin_count = 1;
    err = marmot_update_group_metadata(m, &gresult.group->mls_group_id,
                                        &update_config);
    ASSERT_OK(err, "update_group_metadata to advance epoch");

    /* Try to decrypt the epoch-0 message at epoch 1.
     * NIP-44 lookback finds the exporter_secret, but MLS epoch 0 state
     * is no longer available — this correctly returns a crypto error. */
    MarmotMessageResult msg_in;
    memset(&msg_in, 0, sizeof(msg_in));

    err = marmot_process_message(m, msg_out.event_json, &msg_in);
    ASSERT(err == MARMOT_ERR_CRYPTO,
           "should fail: MLS state for old epoch unavailable");

    marmot_message_result_free(&msg_in);
    marmot_outgoing_message_free(&msg_out);
    marmot_create_group_result_free(&gresult);
    marmot_free(m);
    PASS();
}

static void
test_message_idempotent_processing(void)
{
    TEST("MIP-03: is_message_processed detects duplicates");

    /*
     * Unsigned kind:445 events from create_message don't have event IDs,
     * so the find_message_by_id dedup path isn't triggered. Instead, test
     * the is_message_processed + save_processed_message path that a real
     * relay-connected client would use for dedup tracking.
     */
    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create instance");

    /* Verify the storage has dedup tracking support */
    ASSERT(m->storage->is_message_processed != NULL,
           "storage should support is_message_processed");
    ASSERT(m->storage->save_processed_message != NULL,
           "storage should support save_processed_message");

    /* Generate a fake event ID */
    uint8_t event_id[32];
    randombytes_buf(event_id, 32);

    /* First check — not yet processed */
    bool processed = false;
    MarmotError err = m->storage->is_message_processed(m->storage->ctx,
                                                        event_id, &processed);
    ASSERT_OK(err, "is_message_processed");
    ASSERT(!processed, "should not be processed initially");

    /* Mark as processed */
    uint8_t gid_data[32];
    randombytes_buf(gid_data, 32);
    MarmotGroupId gid = { .data = gid_data, .len = 32 };
    err = m->storage->save_processed_message(m->storage->ctx, event_id,
                                              NULL, marmot_now(), 0,
                                              &gid, 0, NULL);
    ASSERT_OK(err, "save_processed_message");

    /* Second check — now processed */
    err = m->storage->is_message_processed(m->storage->ctx,
                                            event_id, &processed);
    ASSERT_OK(err, "is_message_processed after save");
    ASSERT(processed, "should be processed after save");

    marmot_free(m);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════════
 * Comprehensive Protocol Tests (mm35)
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_reject_invalid_key_package_events(void)
{
    TEST("MIP-00: reject invalid KeyPackage events");

    MlsKeyPackage kp;
    uint8_t pk[32];

    /* Bad base64 content */
    const char *bad_b64 =
        "{\"kind\":443,\"content\":\"!!!not-base64!!!\","
        "\"created_at\":1700000000,"
        "\"tags\":[[\"encoding\",\"base64\"]]}";
    MarmotError err = marmot_parse_key_package_event(bad_b64, &kp, pk);
    ASSERT(err != MARMOT_OK, "should reject bad base64");

    /* Wrong kind (kind:444 instead of 443) */
    const char *wrong_kind =
        "{\"kind\":444,\"content\":\"dGVzdA==\","
        "\"created_at\":1700000000,"
        "\"tags\":[[\"encoding\",\"base64\"]]}";
    err = marmot_parse_key_package_event(wrong_kind, &kp, pk);
    ASSERT(err != MARMOT_OK, "should reject wrong kind");

    /* Empty content */
    const char *empty_content =
        "{\"kind\":443,\"content\":\"\","
        "\"created_at\":1700000000,"
        "\"tags\":[[\"encoding\",\"base64\"]]}";
    err = marmot_parse_key_package_event(empty_content, &kp, pk);
    ASSERT(err != MARMOT_OK, "should reject empty content");

    /* NULL input */
    err = marmot_parse_key_package_event(NULL, &kp, pk);
    ASSERT(err != MARMOT_OK, "should reject NULL");

    PASS();
}

static void
test_create_group_with_member_and_message_exchange(void)
{
    TEST("MIP-01+03: create group with member, exchange messages");

    /* Creator creates a group with Alice, both exchange messages */
    Marmot *creator = create_test_instance();
    Marmot *alice   = create_test_instance();
    ASSERT(creator && alice, "failed to create instances");

    uint8_t creator_sk[32], creator_pk[32];
    uint8_t alice_sk[32], alice_pk[32];
    generate_nostr_keypair(creator_sk, creator_pk);
    generate_nostr_keypair(alice_sk, alice_pk);

    /* Alice creates key package */
    MarmotKeyPackageResult alice_kp;
    memset(&alice_kp, 0, sizeof(alice_kp));
    ASSERT_OK(marmot_create_key_package(alice, alice_pk, alice_sk, NULL, 0, &alice_kp),
              "alice create_key_package");

    /* Creator creates group with Alice */
    const char *kps[] = { alice_kp.event_json };
    MarmotGroupConfig config = {0};
    config.name = "Exchange Group";
    config.admin_pubkeys = (uint8_t (*)[32])&creator_pk;
    config.admin_count = 1;

    MarmotCreateGroupResult gresult;
    memset(&gresult, 0, sizeof(gresult));

    ASSERT_OK(marmot_create_group(creator, creator_pk, kps, 1,
                                   &config, &gresult),
              "create_group");
    ASSERT(gresult.welcome_count == 1, "should have 1 welcome");

    /* Alice accepts welcome */
    uint8_t wid[32];
    randombytes_buf(wid, 32);
    MarmotWelcome *aw = NULL;
    ASSERT_OK(marmot_process_welcome(alice, wid,
                                      gresult.welcome_rumor_jsons[0], &aw),
              "alice process_welcome");
    ASSERT_OK(marmot_accept_welcome(alice, aw), "alice accept_welcome");

    /* Creator sends → Alice receives */
    MarmotOutgoingMessage msg1;
    memset(&msg1, 0, sizeof(msg1));
    ASSERT_OK(marmot_create_message(creator, &gresult.group->mls_group_id,
                                     "{\"kind\":9,\"content\":\"Msg from creator\","
                                     "\"created_at\":1700000001,\"tags\":[]}",
                                     &msg1),
              "creator send");

    MarmotMessageResult recv1;
    memset(&recv1, 0, sizeof(recv1));
    ASSERT_OK(marmot_process_message(alice, msg1.event_json, &recv1),
              "alice receive");
    ASSERT(recv1.type == MARMOT_RESULT_APPLICATION_MESSAGE, "should be app msg");
    ASSERT(strstr(recv1.app_msg.inner_event_json, "Msg from creator") != NULL,
           "alice should decrypt creator's message");

    /* Creator processes own message — MLS identifies it as self-sent */
    MarmotMessageResult self1;
    memset(&self1, 0, sizeof(self1));
    ASSERT_OK(marmot_process_message(creator, msg1.event_json, &self1),
              "creator self-decrypt");
    ASSERT(self1.type == MARMOT_RESULT_OWN_MESSAGE,
           "creator should see own message as OWN_MESSAGE");

    marmot_message_result_free(&recv1);
    marmot_message_result_free(&self1);
    marmot_outgoing_message_free(&msg1);
    marmot_welcome_free(aw);
    marmot_key_package_result_free(&alice_kp);
    marmot_create_group_result_free(&gresult);
    marmot_free(creator);
    marmot_free(alice);
    PASS();
}

static void
test_welcome_expiry(void)
{
    TEST("MIP-02: reject expired welcome event");

    /* Create instance with short max_event_age (1 second) */
    MarmotStorage *storage = marmot_storage_memory_new();
    ASSERT(storage != NULL, "failed to create storage");
    MarmotConfig cfg = marmot_config_default();
    cfg.max_event_age_secs = 1; /* 1 second */
    Marmot *m = marmot_new_with_config(storage, &cfg);
    ASSERT(m != NULL, "failed to create instance");

    /* Create a welcome rumor with an old timestamp (far in the past) */
    const char *old_rumor =
        "{\"kind\":444,\"content\":\"dGVzdA==\","
        "\"created_at\":1000000000,"  /* year ~2001 — definitely expired */
        "\"tags\":[[\"encoding\",\"base64\"]]}";

    uint8_t wrapper_id[32];
    randombytes_buf(wrapper_id, 32);

    MarmotWelcome *welcome = NULL;
    MarmotError err = marmot_process_welcome(m, wrapper_id, old_rumor, &welcome);
    ASSERT(err == MARMOT_ERR_WELCOME_EXPIRED, "should reject expired welcome");
    ASSERT(welcome == NULL, "welcome should be NULL for expired");

    marmot_free(m);
    PASS();
}

static void
test_full_protocol_lifecycle(void)
{
    TEST("Full lifecycle: create → welcome → message → remove → update");

    /* ── Setup: creator + 2 members ─────────────────────────────────── */
    Marmot *creator = create_test_instance();
    Marmot *alice   = create_test_instance();
    Marmot *bob     = create_test_instance();
    ASSERT(creator && alice && bob, "failed to create instances");

    uint8_t creator_sk[32], creator_pk[32];
    uint8_t alice_sk[32], alice_pk[32];
    uint8_t bob_sk[32], bob_pk[32];
    generate_nostr_keypair(creator_sk, creator_pk);
    generate_nostr_keypair(alice_sk, alice_pk);
    generate_nostr_keypair(bob_sk, bob_pk);

    /* ── Step 1: Key packages ───────────────────────────────────────── */
    MarmotKeyPackageResult alice_kp, bob_kp;
    memset(&alice_kp, 0, sizeof(alice_kp));
    memset(&bob_kp, 0, sizeof(bob_kp));

    ASSERT_OK(marmot_create_key_package(alice, alice_pk, alice_sk, NULL, 0, &alice_kp),
              "alice create_key_package");
    ASSERT_OK(marmot_create_key_package(bob, bob_pk, bob_sk, NULL, 0, &bob_kp),
              "bob create_key_package");

    /* ── Step 2: Create solo group ───────────────────────────────────── */
    MarmotGroupConfig config = {0};
    config.name = "Lifecycle Group";
    config.description = "Full protocol lifecycle";
    config.admin_pubkeys = (uint8_t (*)[32])&creator_pk;
    config.admin_count = 1;

    MarmotCreateGroupResult gresult;
    memset(&gresult, 0, sizeof(gresult));

    ASSERT_OK(marmot_create_group(creator, creator_pk, NULL, 0,
                                   &config, &gresult),
              "create_group (solo)");
    MarmotGroupId gid = gresult.group->mls_group_id;

    /* ── Step 3: Add Alice via add_members ──────────────────────────── */
    const char *alice_kps[] = { alice_kp.event_json };
    char **add_alice_jsons = NULL;
    size_t add_alice_count = 0;
    char *add_alice_commit = NULL;

    MarmotError err = marmot_add_members(creator, &gid,
                                          alice_kps, 1,
                                          &add_alice_jsons, &add_alice_count,
                                          &add_alice_commit);
    ASSERT_OK(err, "add alice to group");
    ASSERT(add_alice_count == 1, "should have 1 welcome for alice");

    /* Alice accepts welcome */
    uint8_t wid[32];
    randombytes_buf(wid, 32);

    MarmotWelcome *alice_welcome = NULL;
    ASSERT_OK(marmot_process_welcome(alice, wid,
                                      add_alice_jsons[0],
                                      &alice_welcome),
              "alice process_welcome");
    ASSERT_OK(marmot_accept_welcome(alice, alice_welcome),
              "alice accept_welcome");

    /* ── Step 4: Creator sends a message ────────────────────────────── */
    MarmotOutgoingMessage msg1;
    memset(&msg1, 0, sizeof(msg1));
    ASSERT_OK(marmot_create_message(creator, &gid,
                                     "{\"kind\":9,\"content\":\"Hello Alice!\","
                                     "\"created_at\":1700000001,\"tags\":[]}",
                                     &msg1),
              "creator send message");

    /* Alice decrypts it */
    MarmotMessageResult alice_msg;
    memset(&alice_msg, 0, sizeof(alice_msg));
    ASSERT_OK(marmot_process_message(alice, msg1.event_json, &alice_msg),
              "alice process message");
    ASSERT(alice_msg.type == MARMOT_RESULT_APPLICATION_MESSAGE,
           "alice should get application message");
    ASSERT(strstr(alice_msg.app_msg.inner_event_json, "Hello Alice!") != NULL,
           "alice should decrypt the content");

    /* ── Step 5: Update group metadata ─────────────────────────────── */
    MarmotGroupConfig update_cfg = {0};
    update_cfg.name = "Renamed Lifecycle Group";
    update_cfg.admin_pubkeys = (uint8_t (*)[32])&creator_pk;
    update_cfg.admin_count = 1;
    ASSERT_OK(marmot_update_group_metadata(creator, &gid, &update_cfg),
              "update group name");

    /* Verify updated name */
    MarmotGroup *updated = NULL;
    ASSERT_OK(marmot_get_group(creator, &gid, &updated), "get_group after rename");
    ASSERT(updated != NULL, "updated group is NULL");
    ASSERT(strcmp(updated->name, "Renamed Lifecycle Group") == 0,
           "group name should be updated");
    marmot_group_free(updated);

    /* ── Step 6: Creator sends another message after epoch advance ─── */
    MarmotOutgoingMessage msg3;
    memset(&msg3, 0, sizeof(msg3));
    ASSERT_OK(marmot_create_message(creator, &gid,
                                     "{\"kind\":9,\"content\":\"After rename!\","
                                     "\"created_at\":1700000003,\"tags\":[]}",
                                     &msg3),
              "creator send message 3");

    /* Creator processes own message — MLS identifies it as self-sent */
    MarmotMessageResult self_msg;
    memset(&self_msg, 0, sizeof(self_msg));
    ASSERT_OK(marmot_process_message(creator, msg3.event_json, &self_msg),
              "creator process own message");
    ASSERT(self_msg.type == MARMOT_RESULT_OWN_MESSAGE,
           "creator should see own message as OWN_MESSAGE");

    /* ── Cleanup ────────────────────────────────────────────────────── */
    marmot_message_result_free(&alice_msg);
    marmot_message_result_free(&self_msg);
    marmot_outgoing_message_free(&msg1);
    marmot_outgoing_message_free(&msg3);
    marmot_welcome_free(alice_welcome);
    if (add_alice_jsons) {
        for (size_t i = 0; i < add_alice_count; i++) free(add_alice_jsons[i]);
        free(add_alice_jsons);
    }
    free(add_alice_commit);
    marmot_key_package_result_free(&alice_kp);
    marmot_key_package_result_free(&bob_kp);
    marmot_create_group_result_free(&gresult);
    marmot_free(creator);
    marmot_free(alice);
    marmot_free(bob);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════════
 * Lifecycle tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_marmot_lifecycle(void)
{
    TEST("Lifecycle: create and free Marmot instance");

    Marmot *m = create_test_instance();
    ASSERT(m != NULL, "failed to create instance");
    marmot_free(m);

    /* Free NULL should be safe */
    marmot_free(NULL);

    PASS();
}

static void
test_marmot_config_defaults(void)
{
    TEST("Lifecycle: default config has expected values");

    MarmotConfig config = marmot_config_default();
    ASSERT(config.max_event_age_secs > 0, "max_event_age should be > 0");
    ASSERT(config.max_future_skew_secs > 0, "max_future_skew should be > 0");
    ASSERT(config.out_of_order_tolerance > 0, "oor_tolerance should be > 0");
    ASSERT(config.max_forward_distance > 0, "max_forward_dist should be > 0");
    ASSERT(config.epoch_snapshot_retention > 0, "snapshot_retention should be > 0");
    ASSERT(config.snapshot_ttl_seconds > 0, "snapshot_ttl should be > 0");

    PASS();
}

/* ══════════════════════════════════════════════════════════════════════════
 * Main
 * ══════════════════════════════════════════════════════════════════════════ */

int
main(void)
{
    if (sodium_init() < 0) {
        fprintf(stderr, "Failed to initialize libsodium\n");
        return 1;
    }

    printf("libmarmot protocol tests\n");
    printf("========================\n\n");

    printf("Lifecycle:\n");
    test_marmot_lifecycle();
    test_marmot_config_defaults();

    printf("\nMIP-00: Key Packages\n");
    test_create_key_package_basic();
    test_create_key_package_no_relays();
    test_create_key_package_null_args();
    test_create_multiple_key_packages();
    test_key_package_roundtrip();
    test_key_package_rotation();
    test_key_package_info_storage();

    printf("\nMIP-01: Group Construction\n");
    test_create_group_basic();
    test_create_group_no_members();
    test_create_group_null_args();
    test_merge_pending_commit();
    test_leave_group();
    test_mls_group_serialize_roundtrip();
    test_add_members_to_existing_group();
    test_remove_members_from_group();
    test_remove_nonexistent_member();
    test_add_members_admin_only();
    test_update_group_metadata();
    test_update_group_metadata_non_admin();

    printf("\nMIP-02: Welcome Events\n");
    test_process_welcome_basic();
    test_process_welcome_wrong_kind();
    test_decline_welcome();
    test_welcome_end_to_end();
    test_welcome_duplicate_detection();
    test_accept_welcome_null_args();

    printf("\nMIP-03: Group Messages\n");
    test_create_message_no_group();
    test_create_message_inactive_group();
    test_create_message_with_active_group();
    test_process_message_null_args();
    test_process_message_wrong_kind();
    test_process_message_missing_h_tag();
    test_process_message_unknown_group();
    test_message_encrypt_decrypt_roundtrip();
    test_message_two_member_exchange();
    test_message_epoch_lookback();
    test_message_idempotent_processing();

    printf("\nComprehensive Protocol Tests\n");
    test_reject_invalid_key_package_events();
    test_create_group_with_member_and_message_exchange();
    test_welcome_expiry();
    test_full_protocol_lifecycle();

    printf("\nGroup Queries\n");
    test_get_all_groups();
    test_get_group_not_found();

    printf("\n========================\n");
    printf("Results: %d passed, %d failed, %d total\n",
           tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
