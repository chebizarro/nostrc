/*
 * MLS extensions draft-10 AppDataUpdate proposal wire codec.
 * The Marmot group engine does not apply these proposals yet.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MLS_APP_DATA_UPDATE_H
#define MLS_APP_DATA_UPDATE_H

#include "mls-internal.h"
#include <stddef.h>
#include <stdint.h>

#define MLS_EXTENSION_APP_DATA_DICTIONARY 0x0006
#define MLS_APP_DATA_UPDATE_OP_UPDATE 1
#define MLS_APP_DATA_UPDATE_OP_REMOVE 2

typedef struct {
    uint16_t component_id;
    uint8_t  operation;
    uint8_t *update;
    size_t   update_len;
} MlsAppDataUpdate;

/* These encode/decode only the proposal body (after the u16 proposal type).
 * The component-specific payload and authorization are not validated here. */
int  mls_app_data_update_serialize(const MlsAppDataUpdate *p, MlsTlsBuf *buf);
int  mls_app_data_update_deserialize(MlsTlsReader *reader, MlsAppDataUpdate *p);
void mls_app_data_update_clear(MlsAppDataUpdate *p);

/* Until GroupContext components can be applied and validated, reject a
 * recognizable app_data_dictionary (0x0006). Legacy opaque extension bytes,
 * including malformed lists, retain their existing read behavior. */
int mls_group_extensions_supported(const uint8_t *data, size_t len);

#endif /* MLS_APP_DATA_UPDATE_H */
