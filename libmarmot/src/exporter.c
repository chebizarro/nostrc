/* SPDX-License-Identifier: MIT */
#include "marmot-internal.h"
#include "mls/mls_group.h"
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

MarmotError
marmot_export_secret(Marmot *m, const MarmotGroupId *gid,
                       const uint8_t *label, size_t label_len,
                       const uint8_t *context, size_t context_len,
                       uint8_t *out, size_t out_len)
{
    if (out)
        sodium_memzero(out, out_len);
    if (!m || !gid || !gid->data || !gid->len || !out || !out_len ||
        out_len > 255u * MLS_HASH_LEN || (!label && label_len) ||
        label_len > SIZE_MAX - 64 || (!context && context_len))
        return MARMOT_ERR_INVALID_ARG;
    if (!m->storage || !m->storage->mls_load)
        return MARMOT_ERR_STORAGE;
    MarmotGroup *record = NULL;
    MarmotError err = marmot_get_group(m, gid, &record);
    if (err != MARMOT_OK)
        return err;
    if (!record)
        return MARMOT_ERR_GROUP_NOT_FOUND;
    if (record->state != MARMOT_GROUP_STATE_ACTIVE) {
        marmot_group_free(record);
        return MARMOT_ERR_USE_AFTER_EVICTION;
    }
    uint8_t *state = NULL;
    size_t state_len = 0;
    MlsGroup group = {0};
    err = m->storage->mls_load(m->storage->ctx, "mls_group", gid->data, gid->len,
                              &state, &state_len);
    if (err != MARMOT_OK || !state) {
        if (err == MARMOT_OK)
            err = MARMOT_ERR_GROUP_NOT_FOUND;
        goto done;
    }
    if (mls_group_deserialize(state, state_len, &group) != 0) {
        err = MARMOT_ERR_DESERIALIZATION;
        goto done;
    }
    if (group.epoch != record->epoch || group.group_id_len != gid->len ||
        memcmp(group.group_id, gid->data, gid->len)) {
        err = MARMOT_ERR_WRONG_EPOCH;
        goto done;
    }
    if (sodium_is_zero(group.epoch_secrets.exporter_secret, MLS_HASH_LEN)) {
        err = MARMOT_ERR_GROUP_EXPORTER_SECRET;
        goto done;
    }
    err = mls_exporter_raw(group.epoch_secrets.exporter_secret, label, label_len,
                           context, context_len, out, out_len) == 0
              ? MARMOT_OK : MARMOT_ERR_MLS_EXPORT_SECRET;
done:
    if (state)
        sodium_memzero(state, state_len);
    free(state);
    mls_group_free(&group);
    marmot_group_free(record);
    if (err != MARMOT_OK)
        sodium_memzero(out, out_len);
    return err;
}
