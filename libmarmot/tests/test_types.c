/*
 * libmarmot - Type lifecycle and utility tests
 *
 * Tests constructors, destructors, config defaults, and error strings.
 */

#include <marmot/marmot.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TEST(name) do { printf("  %-50s", #name); name(); printf("PASS\n"); } while(0)

/* ── Config defaults match MDK ─────────────────────────────────────────── */

static void test_config_defaults(void)
{
    MarmotConfig cfg = marmot_config_default();
    assert(cfg.max_event_age_secs == 3888000);
    assert(cfg.max_future_skew_secs == 300);
    assert(cfg.out_of_order_tolerance == 100);
    assert(cfg.max_forward_distance == 1000);
    assert(cfg.epoch_snapshot_retention == 5);
    assert(cfg.snapshot_ttl_seconds == 604800);
}

/* ── GroupId ────────────────────────────────────────────────────────────── */

static void test_group_id_new_and_free(void)
{
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    MarmotGroupId gid = marmot_group_id_new(data, sizeof(data));
    assert(gid.len == 4);
    assert(gid.data != NULL);
    assert(memcmp(gid.data, data, 4) == 0);

    /* Modifying original shouldn't affect copy */
    data[0] = 0x00;
    assert(gid.data[0] == 0xDE);

    marmot_group_id_free(&gid);
    assert(gid.data == NULL);
    assert(gid.len == 0);
}

static void test_group_id_null(void)
{
    MarmotGroupId gid = marmot_group_id_new(NULL, 0);
    assert(gid.data == NULL);
    assert(gid.len == 0);
    marmot_group_id_free(&gid); /* should not crash */
}

static void test_group_id_equal(void)
{
    uint8_t a[] = {1, 2, 3};
    uint8_t b[] = {1, 2, 3};
    uint8_t c[] = {1, 2, 4};
    uint8_t d[] = {1, 2};

    MarmotGroupId ga = marmot_group_id_new(a, 3);
    MarmotGroupId gb = marmot_group_id_new(b, 3);
    MarmotGroupId gc = marmot_group_id_new(c, 3);
    MarmotGroupId gd = marmot_group_id_new(d, 2);

    assert(marmot_group_id_equal(&ga, &gb) == true);
    assert(marmot_group_id_equal(&ga, &gc) == false);
    assert(marmot_group_id_equal(&ga, &gd) == false);
    assert(marmot_group_id_equal(NULL, &ga) == false);

    marmot_group_id_free(&ga);
    marmot_group_id_free(&gb);
    marmot_group_id_free(&gc);
    marmot_group_id_free(&gd);
}

static void test_group_id_to_hex(void)
{
    uint8_t data[] = {0xAB, 0xCD, 0xEF, 0x01};
    MarmotGroupId gid = marmot_group_id_new(data, 4);
    char *hex = marmot_group_id_to_hex(&gid);
    assert(hex != NULL);
    assert(strcmp(hex, "abcdef01") == 0);
    free(hex);
    marmot_group_id_free(&gid);

    /* Null case */
    assert(marmot_group_id_to_hex(NULL) == NULL);
}

/* ── Group lifecycle ───────────────────────────────────────────────────── */

static void test_group_new_free(void)
{
    MarmotGroup *g = marmot_group_new();
    assert(g != NULL);
    assert(g->name == NULL);
    assert(g->description == NULL);
    assert(g->admin_count == 0);
    assert(g->epoch == 0);
    assert(g->state == MARMOT_GROUP_STATE_ACTIVE); /* 0 = ACTIVE */
    marmot_group_free(g);
    marmot_group_free(NULL); /* should not crash */
}

/* ── Group state strings ───────────────────────────────────────────────── */

static void test_group_state_strings(void)
{
    assert(strcmp(marmot_group_state_to_string(MARMOT_GROUP_STATE_ACTIVE), "active") == 0);
    assert(strcmp(marmot_group_state_to_string(MARMOT_GROUP_STATE_INACTIVE), "inactive") == 0);
    assert(strcmp(marmot_group_state_to_string(MARMOT_GROUP_STATE_PENDING), "pending") == 0);

    assert(marmot_group_state_from_string("active") == MARMOT_GROUP_STATE_ACTIVE);
    assert(marmot_group_state_from_string("inactive") == MARMOT_GROUP_STATE_INACTIVE);
    assert(marmot_group_state_from_string("pending") == MARMOT_GROUP_STATE_PENDING);
    assert(marmot_group_state_from_string("garbage") == MARMOT_GROUP_STATE_INACTIVE);
    assert(marmot_group_state_from_string(NULL) == MARMOT_GROUP_STATE_INACTIVE);
}

/* ── Message lifecycle ─────────────────────────────────────────────────── */

static void test_message_new_free(void)
{
    MarmotMessage *m = marmot_message_new();
    assert(m != NULL);
    assert(m->content == NULL);
    assert(m->kind == 0);
    marmot_message_free(m);
    marmot_message_free(NULL);
}

/* ── Welcome lifecycle ─────────────────────────────────────────────────── */

static void test_welcome_new_free(void)
{
    MarmotWelcome *w = marmot_welcome_new();
    assert(w != NULL);
    assert(w->group_name == NULL);
    assert(w->state == MARMOT_WELCOME_STATE_PENDING); /* 0 = PENDING */
    marmot_welcome_free(w);
    marmot_welcome_free(NULL);
}

/* ── Pagination defaults ───────────────────────────────────────────────── */

static void test_pagination_defaults(void)
{
    MarmotPagination pg = marmot_pagination_default();
    assert(pg.limit == 1000);
    assert(pg.offset == 0);
    assert(pg.sort_order == MARMOT_SORT_CREATED_AT_FIRST);
}

/* ── Error strings ─────────────────────────────────────────────────────── */

static void test_error_strings(void)
{
    assert(strcmp(marmot_error_string(MARMOT_OK), "success") == 0);
    assert(strcmp(marmot_error_string(MARMOT_ERR_INVALID_ARG), "invalid argument") == 0);
    assert(strcmp(marmot_error_string(MARMOT_ERR_MLS_FRAMING), "MLS: framing error") == 0);
    assert(strcmp(marmot_error_string(MARMOT_ERR_NOT_IMPLEMENTED), "not implemented") == 0);
    assert(strcmp(marmot_error_string(999), "unknown error") == 0);
}

/* ── Marmot instance lifecycle ─────────────────────────────────────────── */

static void test_marmot_lifecycle(void)
{
    MarmotStorage *s = marmot_storage_memory_new();
    assert(s != NULL);

    Marmot *m = marmot_new(s);
    assert(m != NULL);

    /* Query should work (empty result) */
    MarmotGroup **groups = NULL;
    size_t count = 0;
    assert(marmot_get_all_groups(m, &groups, &count) == MARMOT_OK);
    assert(count == 0);
    assert(groups == NULL);

    marmot_free(m);
    marmot_free(NULL); /* should not crash */
}

static void test_marmot_with_config(void)
{
    MarmotStorage *s = marmot_storage_memory_new();
    MarmotConfig cfg = marmot_config_default();
    cfg.max_event_age_secs = 86400; /* 1 day */

    Marmot *m = marmot_new_with_config(s, &cfg);
    assert(m != NULL);

    /* Stubs should return NOT_IMPLEMENTED */
    MarmotKeyPackageResult kp;
    memset(&kp, 0, sizeof(kp));
    uint8_t pk[32] = {0};
    uint8_t sk[32] = {0};
    assert(marmot_create_key_package(m, pk, sk, NULL, 0, &kp) == MARMOT_ERR_NOT_IMPLEMENTED);

    marmot_free(m);
}

/* ── Result type cleanup ───────────────────────────────────────────────── */

static void test_result_cleanup(void)
{
    /* MarmotMessageResult (app_msg) */
    MarmotMessageResult mr = {
        .type = MARMOT_RESULT_APPLICATION_MESSAGE,
        .app_msg = {
            .inner_event_json = strdup("{\"content\":\"test\"}"),
            .sender_pubkey_hex = strdup("abcd1234"),
        },
    };
    marmot_message_result_free(&mr);
    assert(mr.app_msg.inner_event_json == NULL);

    /* MarmotKeyPackageResult */
    MarmotKeyPackageResult kpr = {
        .event_json = strdup("{\"kind\":443}"),
    };
    marmot_key_package_result_free(&kpr);
    assert(kpr.event_json == NULL);

    /* MarmotCreateGroupResult */
    MarmotCreateGroupResult cgr = {
        .group = marmot_group_new(),
        .welcome_count = 1,
        .welcome_rumor_jsons = calloc(1, sizeof(char *)),
        .evolution_event_json = strdup("{\"kind\":445}"),
    };
    cgr.welcome_rumor_jsons[0] = strdup("{\"rumor\":true}");
    marmot_create_group_result_free(&cgr);
    assert(cgr.group == NULL);
    assert(cgr.evolution_event_json == NULL);
}

/* ── Event kind constants ──────────────────────────────────────────────── */

static void test_kind_constants(void)
{
    assert(MARMOT_KIND_KEY_PACKAGE == 443);
    assert(MARMOT_KIND_WELCOME == 444);
    assert(MARMOT_KIND_GROUP_MESSAGE == 445);
    assert(MARMOT_EXTENSION_TYPE == 0xF2EE);
    assert(MARMOT_CIPHERSUITE == 0x0001);
}

int main(void)
{
    printf("libmarmot: Type and lifecycle tests\n");
    TEST(test_config_defaults);
    TEST(test_group_id_new_and_free);
    TEST(test_group_id_null);
    TEST(test_group_id_equal);
    TEST(test_group_id_to_hex);
    TEST(test_group_new_free);
    TEST(test_group_state_strings);
    TEST(test_message_new_free);
    TEST(test_welcome_new_free);
    TEST(test_pagination_defaults);
    TEST(test_error_strings);
    TEST(test_marmot_lifecycle);
    TEST(test_marmot_with_config);
    TEST(test_result_cleanup);
    TEST(test_kind_constants);
    printf("All type and lifecycle tests passed.\n");
    return 0;
}
