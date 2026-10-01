/*
 * AppDataUpdate draft-10 wire vectors and fail-closed parser coverage.
 * These are syntax tests, not adopted Marmot group-state interoperability.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mls/mls_app_data_update.h"
#include "mls/mls_group.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; \
} } while (0)

static void
check_body(const MlsAppDataUpdate *source, const uint8_t *expected, size_t len)
{
    MlsTlsBuf buf;
    CHECK(mls_tls_buf_init(&buf, 16) == 0);
    CHECK(mls_app_data_update_serialize(source, &buf) == 0);
    CHECK(buf.len == len && memcmp(buf.data, expected, len) == 0);
    MlsTlsReader reader;
    MlsAppDataUpdate decoded;
    mls_tls_reader_init(&reader, expected, len);
    CHECK(mls_app_data_update_deserialize(&reader, &decoded) == 0);
    CHECK(mls_tls_reader_done(&reader));
    CHECK(decoded.component_id == source->component_id);
    CHECK(decoded.operation == source->operation);
    CHECK(decoded.update_len == source->update_len);
    if (source->update_len)
        CHECK(decoded.update && memcmp(decoded.update, source->update, source->update_len) == 0);
    mls_app_data_update_clear(&decoded);
    mls_tls_buf_free(&buf);
}

static void
test_vectors(void)
{
    /* draft-10: ComponentID(u16), operation(u8), and update<V> only for update. */
    uint8_t state[] = {0xaa, 0xbb};
    MlsAppDataUpdate update = {0x8003, MLS_APP_DATA_UPDATE_OP_UPDATE, state, sizeof(state)};
    MlsAppDataUpdate remove = {0x8003, MLS_APP_DATA_UPDATE_OP_REMOVE, NULL, 0};
    static const uint8_t update_wire[] = {0x80, 0x03, 0x01, 0x02, 0xaa, 0xbb};
    static const uint8_t remove_wire[] = {0x80, 0x03, 0x02};
    check_body(&update, update_wire, sizeof(update_wire));
    check_body(&remove, remove_wire, sizeof(remove_wire));

    /* ProposalOrRef inline + proposal type 0x0008 + body, inside Commit. */
    MlsProposal proposal = {0};
    proposal.type = MLS_PROPOSAL_APP_DATA_UPDATE;
    proposal.app_data_update = update;
    MlsCommit commit = {0};
    commit.proposals = &proposal;
    commit.proposal_count = 1;
    MlsTlsBuf buf;
    CHECK(mls_tls_buf_init(&buf, 16) == 0);
    CHECK(mls_commit_serialize(&commit, &buf) == 0);
    static const uint8_t commit_wire[] = {
        0x09, 0x01, 0x00, 0x08, 0x80, 0x03, 0x01, 0x02, 0xaa, 0xbb, 0x00
    };
    CHECK(buf.len == sizeof(commit_wire) &&
          memcmp(buf.data, commit_wire, sizeof(commit_wire)) == 0);
    MlsTlsReader reader;
    MlsCommit decoded;
    mls_tls_reader_init(&reader, commit_wire, sizeof(commit_wire));
    CHECK(mls_commit_deserialize(&reader, &decoded) == 0);
    CHECK(mls_tls_reader_done(&reader));
    CHECK(decoded.proposal_count == 1 &&
          decoded.proposals[0].type == MLS_PROPOSAL_APP_DATA_UPDATE &&
          decoded.proposals[0].unsupported &&
          decoded.proposals[0].app_data_update.component_id == 0x8003);
    mls_commit_clear(&decoded);
    mls_tls_buf_free(&buf);
}

static void
test_group_context_gate(void)
{
    static const uint8_t legacy_ext[] = {0xf2, 0xee, 0x00};
    static const uint8_t app_dict_ext[] = {0x00, 0x06, 0x00};
    static const uint8_t mixed_ext[] = {0xf2, 0xee, 0x00, 0x00, 0x06, 0x00};
    static const uint8_t truncated_ext[] = {0xf2, 0xee, 0x02, 0x01};
    static const uint8_t truncated_app_dict[] = {0x00, 0x06};
    CHECK(mls_group_extensions_supported(NULL, 0) == 0);
    CHECK(mls_group_extensions_supported(legacy_ext, sizeof(legacy_ext)) == 0);
    CHECK(mls_group_extensions_supported(app_dict_ext, sizeof(app_dict_ext)) != 0);
    CHECK(mls_group_extensions_supported(mixed_ext, sizeof(mixed_ext)) != 0);
    CHECK(mls_group_extensions_supported(truncated_ext, sizeof(truncated_ext)) == 0);
    CHECK(mls_group_extensions_supported(truncated_app_dict,
                                         sizeof(truncated_app_dict)) != 0);

    /* Since 0.12.0 creation admits an adopted GroupContext only when it is
     * complete (nostrc-qp24.5.1): an empty dictionary without
     * required_capabilities is malformed, and nothing is created. */
    MlsGroup group = {0};
    uint8_t id[32] = {0}, sk[MLS_SIG_SK_LEN] = {0};
    CHECK(mls_group_create(&group, id, sizeof(id), id, sizeof(id), sk,
                           app_dict_ext, sizeof(app_dict_ext)) == MARMOT_ERR_EXTENSION_FORMAT);
    CHECK(mls_group_create(&group, id, sizeof(id), id, sizeof(id), sk,
                           mixed_ext, sizeof(mixed_ext)) == MARMOT_ERR_VALIDATION);
}

static void
test_malformed(void)
{
    static const uint8_t invalid_op[] = {0x80, 0x03, 0x03};
    static const uint8_t truncated[] = {0x80, 0x03, 0x01, 0x02, 0xaa};
    static const uint8_t noncanonical_len[] = {0x80, 0x03, 0x01, 0x40, 0x02, 0xaa, 0xbb};
    const struct { const uint8_t *data; size_t len; } bad[] = {
        {invalid_op, sizeof(invalid_op)},
        {truncated, sizeof(truncated)},
        {noncanonical_len, sizeof(noncanonical_len)},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        MlsTlsReader reader;
        MlsAppDataUpdate decoded;
        mls_tls_reader_init(&reader, bad[i].data, bad[i].len);
        CHECK(mls_app_data_update_deserialize(&reader, &decoded) != 0);
        mls_app_data_update_clear(&decoded);
    }

    MlsTlsBuf buf;
    CHECK(mls_tls_buf_init(&buf, 8) == 0);
    uint8_t one = 1;
    MlsAppDataUpdate bad_remove = {0x8003, MLS_APP_DATA_UPDATE_OP_REMOVE, &one, 1};
    CHECK(mls_app_data_update_serialize(&bad_remove, &buf) != 0 && buf.len == 0);
    MlsAppDataUpdate bad_update = {0x8003, MLS_APP_DATA_UPDATE_OP_UPDATE, NULL, 1};
    CHECK(mls_app_data_update_serialize(&bad_update, &buf) != 0 && buf.len == 0);
    mls_tls_buf_free(&buf);
}

static int
make_admins(const uint8_t *keys, size_t count, MlsTlsBuf *out)
{
    if (mls_tls_buf_init(out, 1 + count * 32) != 0) return -1;
    if (mls_tls_write_vli(out, count * 32) != 0 ||
        mls_tls_buf_append(out, keys, count * 32) != 0) {
        mls_tls_buf_free(out);
        return -1;
    }
    return 0;
}

static int
make_dictionary(const uint8_t *admins, size_t admins_len,
                MlsTlsBuf *out)
{
    static const uint8_t required[] = {2, 0x80, 0x03};
    static const uint8_t unknown[] = {0xca, 0xfe};
    MlsTlsBuf entries;
    if (mls_tls_buf_init(&entries, 64) != 0) return -1;
    int ok = mls_tls_write_u16(&entries, MLS_COMPONENT_APP_COMPONENTS) == 0 &&
        mls_tls_write_opaque32(&entries, required, sizeof(required)) == 0 &&
        mls_tls_write_u16(&entries, MARMOT_COMPONENT_ADMIN_POLICY_V1) == 0 &&
        mls_tls_write_opaque32(&entries, admins, admins_len) == 0 &&
        mls_tls_write_u16(&entries, 0x9000) == 0 &&
        mls_tls_write_opaque32(&entries, unknown, sizeof(unknown)) == 0;
    if (!ok || mls_tls_buf_init(out, entries.len + 2) != 0) {
        mls_tls_buf_free(&entries);
        return -1;
    }
    ok = mls_tls_write_vli(out, entries.len) == 0 &&
         mls_tls_buf_append(out, entries.data, entries.len) == 0;
    mls_tls_buf_free(&entries);
    if (!ok) { mls_tls_buf_free(out); return -1; }
    return 0;
}

static void
test_admin_policy_state(void)
{
    uint8_t keys[3][32];
    memset(keys[0], 0x11, 32);
    memset(keys[1], 0x22, 32);
    memset(keys[2], 0x33, 32);
    MlsRatchetTree tree;
    CHECK(mls_tree_new(&tree, 2) == 0);
    for (uint32_t i = 0; i < 2; i++) {
        MlsNode *node = &tree.nodes[mls_tree_leaf_to_node(i)];
        node->type = MLS_NODE_LEAF;
        node->leaf.credential_type = MLS_CREDENTIAL_BASIC;
        node->leaf.credential_identity = malloc(32);
        CHECK(node->leaf.credential_identity != NULL);
        memcpy(node->leaf.credential_identity, keys[i], 32);
        node->leaf.credential_identity_len = 32;
    }
    MlsTlsBuf old, next, dictionary, expected;
    CHECK(make_admins(keys[0], 1, &old) == 0);
    CHECK(make_admins(keys[0], 2, &next) == 0);
    CHECK(make_dictionary(old.data, old.len, &dictionary) == 0);
    CHECK(make_dictionary(next.data, next.len, &expected) == 0);
    MlsAppDataUpdate update = {MARMOT_COMPONENT_ADMIN_POLICY_V1,
                               MLS_APP_DATA_UPDATE_OP_UPDATE,
                               next.data, next.len};
    uint8_t *out = NULL;
    size_t out_len = 0;
    CHECK(mls_app_data_update_admin_policy(dictionary.data, dictionary.len,
          &tree, &tree, 7, 7, 0, 0, &update, &out, &out_len) == 0);
    CHECK(out && out_len == expected.len &&
          memcmp(out, expected.data, expected.len) == 0);
    CHECK(dictionary.len != expected.len);
    free(out);

    /* Source-epoch and committer authority both come from the parent state. */
    CHECK(mls_app_data_update_admin_policy(dictionary.data, dictionary.len,
          &tree, &tree, 7, 6, 0, 0, &update, &out, &out_len) != 0 && !out);
    CHECK(mls_app_data_update_admin_policy(dictionary.data, dictionary.len,
          &tree, &tree, 7, 7, 1, 0, &update, &out, &out_len) != 0 && !out);
    CHECK(mls_app_data_update_admin_policy(dictionary.data, dictionary.len,
          &tree, &tree, 7, 7, 0, 1, &update, &out, &out_len) != 0 && !out);
    CHECK(mls_app_data_update_admin_policy(dictionary.data, dictionary.len,
          &tree, &tree, 7, 7, 2, 0, &update, &out, &out_len) != 0 && !out);

    MlsTlsBuf absent, duplicate, reversed;
    CHECK(make_admins(keys[2], 1, &absent) == 0);
    update.update = absent.data; update.update_len = absent.len;
    CHECK(mls_app_data_update_admin_policy(dictionary.data, dictionary.len,
          &tree, &tree, 7, 7, 0, 0, &update, &out, &out_len) != 0 && !out);
    uint8_t pair[64];
    memcpy(pair, keys[0], 32); memcpy(pair + 32, keys[0], 32);
    CHECK(make_admins(pair, 2, &duplicate) == 0);
    update.update = duplicate.data; update.update_len = duplicate.len;
    CHECK(mls_app_data_update_admin_policy(dictionary.data, dictionary.len,
          &tree, &tree, 7, 7, 0, 0, &update, &out, &out_len) != 0 && !out);
    memcpy(pair, keys[1], 32); memcpy(pair + 32, keys[0], 32);
    CHECK(make_admins(pair, 2, &reversed) == 0);
    update.update = reversed.data; update.update_len = reversed.len;
    CHECK(mls_app_data_update_admin_policy(dictionary.data, dictionary.len,
          &tree, &tree, 7, 7, 0, 0, &update, &out, &out_len) != 0 && !out);
    update.operation = MLS_APP_DATA_UPDATE_OP_REMOVE;
    update.update = NULL; update.update_len = 0;
    CHECK(mls_app_data_update_admin_policy(dictionary.data, dictionary.len,
          &tree, &tree, 7, 7, 0, 0, &update, &out, &out_len) != 0 && !out);
    update.operation = MLS_APP_DATA_UPDATE_OP_UPDATE;
    update.update = next.data; update.update_len = next.len;
    CHECK(mls_app_data_update_admin_policy(dictionary.data, dictionary.len - 1,
          &tree, &tree, 7, 7, 0, 0, &update, &out, &out_len) != 0 && !out);

    mls_tls_buf_free(&reversed);
    mls_tls_buf_free(&duplicate);
    mls_tls_buf_free(&absent);
    mls_tls_buf_free(&expected);
    mls_tls_buf_free(&dictionary);
    mls_tls_buf_free(&next);
    mls_tls_buf_free(&old);
    mls_tree_free(&tree);
}

int
main(void)
{
    test_vectors();
    test_admin_policy_state();
    test_group_context_gate();
    test_malformed();
    if (failures) return 1;
    puts("AppDataUpdate wire tests passed (engine application remains disabled)");
    return 0;
}
