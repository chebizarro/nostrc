/*
 * AppDataUpdate draft-10 wire vectors and fail-closed parser coverage.
 * These are syntax tests, not adopted Marmot group-state interoperability.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mls/mls_app_data_update.h"
#include "mls/mls_group.h"
#include <stdio.h>
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

    MlsGroup group = {0};
    uint8_t id[32] = {0}, sk[MLS_SIG_SK_LEN] = {0};
    CHECK(mls_group_create(&group, id, sizeof(id), id, sizeof(id), sk,
                           app_dict_ext, sizeof(app_dict_ext)) == MARMOT_ERR_UNSUPPORTED);
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

int
main(void)
{
    test_vectors();
    test_group_context_gate();
    test_malformed();
    if (failures) return 1;
    puts("AppDataUpdate wire tests passed (engine application remains disabled)");
    return 0;
}
