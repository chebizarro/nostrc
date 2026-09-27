/*
 * MLS extensions draft-10 AppDataUpdate proposal wire codec.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mls_app_data_update.h"
#include <stdlib.h>
#include <string.h>

int
mls_app_data_update_serialize(const MlsAppDataUpdate *p, MlsTlsBuf *buf)
{
    if (!p || !buf ||
        (p->operation != MLS_APP_DATA_UPDATE_OP_UPDATE &&
         p->operation != MLS_APP_DATA_UPDATE_OP_REMOVE) ||
        (p->operation == MLS_APP_DATA_UPDATE_OP_REMOVE &&
         (p->update || p->update_len != 0)) ||
        (p->operation == MLS_APP_DATA_UPDATE_OP_UPDATE &&
         p->update_len != 0 && !p->update))
        return -1;

    if (mls_tls_write_u16(buf, p->component_id) != 0 ||
        mls_tls_write_u8(buf, p->operation) != 0)
        return -1;
    if (p->operation == MLS_APP_DATA_UPDATE_OP_UPDATE)
        return mls_tls_write_opaque32(buf, p->update, p->update_len);
    return 0;
}

int
mls_app_data_update_deserialize(MlsTlsReader *reader, MlsAppDataUpdate *p)
{
    if (!reader || !p) return -1;
    memset(p, 0, sizeof(*p));
    if (mls_tls_read_u16(reader, &p->component_id) != 0 ||
        mls_tls_read_u8(reader, &p->operation) != 0)
        return -1;
    if (p->operation == MLS_APP_DATA_UPDATE_OP_REMOVE)
        return 0;
    if (p->operation != MLS_APP_DATA_UPDATE_OP_UPDATE ||
        mls_tls_read_opaque32(reader, &p->update, &p->update_len) != 0) {
        mls_app_data_update_clear(p);
        return -1;
    }
    return 0;
}

void
mls_app_data_update_clear(MlsAppDataUpdate *p)
{
    if (!p) return;
    free(p->update);
    memset(p, 0, sizeof(*p));
}

int
mls_group_extensions_supported(const uint8_t *data, size_t len)
{
    if (len && !data) return -1;
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, data, len);
    while (!mls_tls_reader_done(&reader)) {
        uint16_t type;
        size_t data_len;
        /* Legacy groups historically keep opaque extension bytes even when
         * their list is not canonically encoded. Do not reinterpret or reject
         * that state here; a recognizable adopted extension still fails. */
        if (mls_tls_read_u16(&reader, &type) != 0)
            return 0;
        if (type == MLS_EXTENSION_APP_DATA_DICTIONARY)
            return -1;
        if (mls_tls_read_vli(&reader, &data_len) != 0 ||
            mls_tls_reader_remaining(&reader) < data_len)
            return 0;
        reader.pos += data_len;
    }
    return 0;
}
