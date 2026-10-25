/* SPDX-License-Identifier: MIT */
#include <marmot/marmot.h>
#include <nostr-keys.h>
#include <json.h>
#include "mls/mls_group.h"
#include <sodium.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void hex(const char *s, uint8_t *out, size_t len)
{
    assert(sodium_hex2bin(out, len, s, strlen(s), NULL, NULL, NULL) == 0);
}

static void store(MarmotStorage *s, const MarmotGroupId *gid, const MlsGroup *g)
{
    uint8_t *state = NULL;
    size_t len = 0;
    assert(mls_group_serialize(g, &state, &len) == 0);
    assert(s->mls_store(s->ctx, "mls_group", gid->data, gid->len, state, len) == MARMOT_OK);
    sodium_memzero(state, len);
    free(state);
}

int main(void)
{
    nostr_json_init();
    MarmotStorage *s = marmot_storage_memory_new();
    Marmot *m = marmot_new(s);
    assert(m);
    uint8_t sk[32] = {0}, pk[32];
    sk[31] = 1;
    char sk_hex[65];
    sodium_bin2hex(sk_hex, sizeof sk_hex, sk, sizeof sk);
    char *pk_hex = nostr_key_get_public(sk_hex);
    assert(pk_hex);
    hex(pk_hex, pk, sizeof pk);
    free(pk_hex);
    MarmotKeyPackageResult kp = {0};
    assert(marmot_create_key_package(m, pk, sk, NULL, 0, &kp) == MARMOT_OK);
    marmot_key_package_result_free(&kp);
    MarmotGroupConfig config = {.name = "Exporter vector", .admin_pubkeys = &pk, .admin_count = 1};
    MarmotCreateGroupResult result = {0};
    assert(marmot_create_group(m, pk, NULL, 0, &config, &result) == MARMOT_OK);
    const MarmotGroupId *gid = &result.group->mls_group_id;
    uint8_t *state = NULL;
    size_t len = 0;
    assert(s->mls_load(s->ctx, "mls_group", gid->data, gid->len, &state, &len) == MARMOT_OK);
    MlsGroup group = {0};
    assert(mls_group_deserialize(state, len, &group) == 0);
    sodium_memzero(state, len);
    free(state);

    /* Existing RFC 9420 interop key-schedule vector, ciphersuite 1, epoch 0:
     * tests/vectors/mdk/key-schedule.json [0].epochs[0].exporter.
     * Inject its raw exporter secret into a serialized current-epoch fixture;
     * compare the PUBLIC API's bytes, not the private KDF against itself. */
    hex("5a097e149f2a375d0b9e1d1f4dc3a9c6c1788df888e5441f41a8791f4dc56cea",
        group.epoch_secrets.exporter_secret, 32);
    store(s, gid, &group);
    /* In this vector the hex representation IS the ASCII exporter label. */
    uint8_t label[] = "9ba13d54ecdec7cbefcb47b4268d7b1990fabc6d6e67681e167959389d84e4e4";
    uint8_t context[32], expected[32], output[32];
    hex("884f1af892ab002f5be4c5d5081ade9e0e6418c6ea7a9a92e90534f19dcef785", context, 32);
    hex("dbce4e25e59ab4dfa6f6200f113ed08393cf6e7286d024811141c6a4dd11c0cb", expected, 32);
    assert(marmot_export_secret(m, gid, label, sizeof label - 1, context, 32, output, 32) == MARMOT_OK);
    assert(!memcmp(output, expected, 32));
    /* Label and context are both domain-separating inputs. */
    label[0] ^= 1;
    assert(marmot_export_secret(m, gid, label, sizeof label - 1, context, 32, output, 32) == MARMOT_OK);
    assert(memcmp(output, expected, 32));
    label[0] ^= 1;
    context[0] ^= 1;
    assert(marmot_export_secret(m, gid, label, sizeof label - 1, context, 32, output, 32) == MARMOT_OK);
    assert(memcmp(output, expected, 32));
    context[0] ^= 1;
    assert(marmot_export_secret(m, gid, NULL, 0, NULL, 0, output, 32) == MARMOT_OK);

    memset(output, 0xaa, 32);
    assert(marmot_export_secret(m, gid, NULL, 1, context, 32, output, 32) == MARMOT_ERR_INVALID_ARG);
    assert(sodium_is_zero(output, 32));
    memset(output, 0xaa, 32);
    assert(marmot_export_secret(m, gid, label, sizeof label - 1, NULL, 1, output, 32) == MARMOT_ERR_INVALID_ARG);
    assert(sodium_is_zero(output, 32));
    memset(output, 0xaa, 32);
    assert(marmot_export_secret(NULL, gid, label, sizeof label - 1, context, 32, output, 32) == MARMOT_ERR_INVALID_ARG);
    assert(sodium_is_zero(output, 32));
    MarmotGroupId missing = {(uint8_t *)"absent", 6};
    memset(output, 0xaa, 32);
    assert(marmot_export_secret(m, &missing, label, sizeof label - 1, context, 32, output, 32) == MARMOT_ERR_GROUP_NOT_FOUND);
    assert(sodium_is_zero(output, 32));
    uint8_t oversized[8161];
    memset(oversized, 0xaa, sizeof oversized);
    assert(marmot_export_secret(m, gid, label, sizeof label - 1, context, 32, oversized, sizeof oversized) == MARMOT_ERR_INVALID_ARG);
    assert(sodium_is_zero(oversized, sizeof oversized));

    /* A stale group record must not select some other epoch's state. */
    ++group.epoch;
    store(s, gid, &group);
    memset(output, 0xaa, 32);
    assert(marmot_export_secret(m, gid, label, sizeof label - 1, context, 32, output, 32) == MARMOT_ERR_WRONG_EPOCH);
    assert(sodium_is_zero(output, 32));
    --group.epoch;
    sodium_memzero(group.epoch_secrets.exporter_secret, 32);
    store(s, gid, &group);
    memset(output, 0xaa, 32);
    assert(marmot_export_secret(m, gid, label, sizeof label - 1, context, 32, output, 32) == MARMOT_ERR_GROUP_EXPORTER_SECRET);
    assert(sodium_is_zero(output, 32));
    MarmotGroup *record = NULL;
    assert(marmot_get_group(m, gid, &record) == MARMOT_OK && record);
    record->state = MARMOT_GROUP_STATE_INACTIVE;
    assert(s->save_group(s->ctx, record) == MARMOT_OK);
    marmot_group_free(record);
    memset(output, 0xaa, 32);
    assert(marmot_export_secret(m, gid, label, sizeof label - 1, context, 32, output, 32) == MARMOT_ERR_USE_AFTER_EVICTION);
    assert(sodium_is_zero(output, 32));
    mls_group_free(&group);
    marmot_create_group_result_free(&result);
    marmot_free(m);
    sodium_memzero(sk, sizeof sk);
    sodium_memzero(sk_hex, sizeof sk_hex);
    sodium_memzero(output, sizeof output);
    puts("marmot_test_exporter: OK (existing key-schedule exporter vector + refusal wiping)");
    return 0;
}
