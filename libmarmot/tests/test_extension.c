/*
 * libmarmot - Extension (0xF2EE) serialization tests
 *
 * Tests round-trip TLS serialization of the NostrGroupDataExtension.
 * This is the most testable component early — exercises both TLS codec
 * and the Marmot extension format.
 */

#include <marmot/marmot.h>
#include "mls/mls-internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TEST(name) do { printf("  %-50s", #name); name(); printf("PASS\n"); } while(0)

/* ── Minimal extension (no image) ──────────────────────────────────────── */

static void test_extension_minimal_roundtrip(void)
{
    MarmotGroupDataExtension *ext = marmot_group_data_extension_new();
    assert(ext != NULL);
    assert(ext->version == MARMOT_EXTENSION_VERSION);

    /* Set fields */
    memset(ext->nostr_group_id, 0xAB, 32);
    ext->name = strdup("Test Group");
    ext->description = strdup("A test group for Marmot");

    /* One admin */
    ext->admin_count = 1;
    ext->admins = malloc(32);
    memset(ext->admins[0], 0x01, 32);

    /* Two relays */
    ext->relay_count = 2;
    ext->relays = calloc(2, sizeof(char *));
    ext->relays[0] = strdup("wss://relay.damus.io");
    ext->relays[1] = strdup("wss://nos.lol");

    /* No image */
    assert(ext->image_hash == NULL);

    /* Serialize */
    uint8_t *data = NULL;
    size_t len = 0;
    int rc = marmot_group_data_extension_serialize(ext, &data, &len);
    assert(rc == MARMOT_OK);
    assert(data != NULL);
    assert(len > 0);

    /* Deserialize */
    MarmotGroupDataExtension *ext2 = marmot_group_data_extension_deserialize(data, len);
    assert(ext2 != NULL);

    /* Verify all fields match */
    assert(ext2->version == ext->version);
    assert(memcmp(ext2->nostr_group_id, ext->nostr_group_id, 32) == 0);
    assert(strcmp(ext2->name, ext->name) == 0);
    assert(strcmp(ext2->description, ext->description) == 0);
    assert(ext2->admin_count == 1);
    assert(memcmp(ext2->admins[0], ext->admins[0], 32) == 0);
    assert(ext2->relay_count == 2);
    assert(strcmp(ext2->relays[0], "wss://relay.damus.io") == 0);
    assert(strcmp(ext2->relays[1], "wss://nos.lol") == 0);
    assert(ext2->image_hash == NULL);

    free(data);
    marmot_group_data_extension_free(ext);
    marmot_group_data_extension_free(ext2);
}

/* ── Extension with image (v2) ─────────────────────────────────────────── */

static void test_extension_with_image_v2(void)
{
    MarmotGroupDataExtension *ext = marmot_group_data_extension_new();
    ext->version = 2;
    memset(ext->nostr_group_id, 0xCC, 32);
    ext->name = strdup("Image Group");
    ext->description = strdup("");

    ext->admin_count = 2;
    ext->admins = malloc(64);
    memset(ext->admins[0], 0xAA, 32);
    memset(ext->admins[1], 0xBB, 32);

    ext->relay_count = 1;
    ext->relays = calloc(1, sizeof(char *));
    ext->relays[0] = strdup("wss://relay.nostr.band");

    /* Image metadata */
    ext->image_hash = malloc(32);
    memset(ext->image_hash, 0x11, 32);
    ext->image_key = malloc(32);
    memset(ext->image_key, 0x22, 32);
    ext->image_nonce = malloc(12);
    memset(ext->image_nonce, 0x33, 12);
    ext->image_upload_key = malloc(32);
    memset(ext->image_upload_key, 0x44, 32);

    /* Serialize + deserialize */
    uint8_t *data = NULL;
    size_t len = 0;
    assert(marmot_group_data_extension_serialize(ext, &data, &len) == MARMOT_OK);

    MarmotGroupDataExtension *ext2 = marmot_group_data_extension_deserialize(data, len);
    assert(ext2 != NULL);

    assert(ext2->version == 2);
    assert(strcmp(ext2->name, "Image Group") == 0);
    assert(ext2->admin_count == 2);
    assert(memcmp(ext2->admins[1], ext->admins[1], 32) == 0);

    assert(ext2->image_hash != NULL);
    assert(memcmp(ext2->image_hash, ext->image_hash, 32) == 0);
    assert(ext2->image_key != NULL);
    assert(memcmp(ext2->image_key, ext->image_key, 32) == 0);
    assert(ext2->image_nonce != NULL);
    assert(memcmp(ext2->image_nonce, ext->image_nonce, 12) == 0);
    assert(ext2->image_upload_key != NULL);
    assert(memcmp(ext2->image_upload_key, ext->image_upload_key, 32) == 0);

    free(data);
    marmot_group_data_extension_free(ext);
    marmot_group_data_extension_free(ext2);
}

/* ── Empty fields ──────────────────────────────────────────────────────── */

static void test_extension_empty_strings(void)
{
    MarmotGroupDataExtension *ext = marmot_group_data_extension_new();
    memset(ext->nostr_group_id, 0x00, 32);
    /* Leave name and description NULL */
    ext->admin_count = 0;
    ext->relay_count = 0;

    uint8_t *data = NULL;
    size_t len = 0;
    assert(marmot_group_data_extension_serialize(ext, &data, &len) == MARMOT_OK);

    MarmotGroupDataExtension *ext2 = marmot_group_data_extension_deserialize(data, len);
    assert(ext2 != NULL);
    assert(ext2->name == NULL);
    assert(ext2->description == NULL);
    assert(ext2->admin_count == 0);
    assert(ext2->relay_count == 0);
    assert(ext2->image_hash == NULL);

    free(data);
    marmot_group_data_extension_free(ext);
    marmot_group_data_extension_free(ext2);
}

/* ── Many admins and relays ────────────────────────────────────────────── */

static void test_extension_many_admins(void)
{
    MarmotGroupDataExtension *ext = marmot_group_data_extension_new();
    memset(ext->nostr_group_id, 0xFF, 32);
    ext->name = strdup("Big Group");
    ext->description = strdup("Many admins");

    ext->admin_count = 50;
    ext->admins = malloc(50 * 32);
    for (size_t i = 0; i < 50; i++)
        memset(ext->admins[i], (uint8_t)i, 32);

    ext->relay_count = 10;
    ext->relays = calloc(10, sizeof(char *));
    for (size_t i = 0; i < 10; i++) {
        char url[64];
        snprintf(url, sizeof(url), "wss://relay%zu.example.com", i);
        ext->relays[i] = strdup(url);
    }

    uint8_t *data = NULL;
    size_t len = 0;
    assert(marmot_group_data_extension_serialize(ext, &data, &len) == MARMOT_OK);

    MarmotGroupDataExtension *ext2 = marmot_group_data_extension_deserialize(data, len);
    assert(ext2 != NULL);
    assert(ext2->admin_count == 50);
    assert(ext2->relay_count == 10);

    /* Spot-check */
    uint8_t expected_admin[32];
    memset(expected_admin, 42, 32);
    assert(memcmp(ext2->admins[42], expected_admin, 32) == 0);
    assert(strcmp(ext2->relays[7], "wss://relay7.example.com") == 0);

    free(data);
    marmot_group_data_extension_free(ext);
    marmot_group_data_extension_free(ext2);
}

/* ── Invalid input ─────────────────────────────────────────────────────── */

static void test_extension_deserialize_garbage(void)
{
    uint8_t garbage[] = {0x00, 0x03, 0xFF}; /* invalid version 3 */
    MarmotGroupDataExtension *ext = marmot_group_data_extension_deserialize(garbage, sizeof(garbage));
    assert(ext == NULL);
}

static void test_extension_deserialize_truncated(void)
{
    /* Valid version, but truncated after version field */
    uint8_t truncated[] = {0x00, 0x02};
    MarmotGroupDataExtension *ext = marmot_group_data_extension_deserialize(truncated, sizeof(truncated));
    assert(ext == NULL);
}

static void test_extension_deserialize_null(void)
{
    assert(marmot_group_data_extension_deserialize(NULL, 0) == NULL);
    assert(marmot_group_data_extension_deserialize(NULL, 100) == NULL);
}

int main(void)
{
    printf("libmarmot: Extension serialization tests\n");
    TEST(test_extension_minimal_roundtrip);
    TEST(test_extension_with_image_v2);
    TEST(test_extension_empty_strings);
    TEST(test_extension_many_admins);
    TEST(test_extension_deserialize_garbage);
    TEST(test_extension_deserialize_truncated);
    TEST(test_extension_deserialize_null);
    printf("All extension tests passed.\n");
    return 0;
}
