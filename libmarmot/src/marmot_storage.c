/*
 * libmarmot - Storage helper
 *
 * SPDX-License-Identifier: MIT
 */

#include <marmot/marmot-storage.h>
#include <stdlib.h>

void
marmot_storage_free(MarmotStorage *storage)
{
    if (!storage) return;
    if (storage->destroy)
        storage->destroy(storage->ctx);
    free(storage);
}
