/*
 * bc-db-backend.c - BcDbBackend common utilities
 *
 * SPDX-License-Identifier: MIT
 */

#include "bc-db-backend.h"
#include <stdlib.h>
#include <string.h>

BcDbBlobMeta *
bc_db_blob_meta_copy(const BcDbBlobMeta *meta)
{
  if (meta == NULL)
    return NULL;

  BcDbBlobMeta *copy = g_new0(BcDbBlobMeta, 1);
  copy->sha256        = g_strdup(meta->sha256);
  copy->size          = meta->size;
  copy->mime_type     = g_strdup(meta->mime_type);
  copy->created_at    = meta->created_at;
  copy->last_accessed = meta->last_accessed;
  copy->access_count  = meta->access_count;
  return copy;
}

void
bc_db_blob_meta_free(BcDbBlobMeta *meta)
{
  if (meta == NULL)
    return;
  g_free(meta->sha256);
  g_free(meta->mime_type);
  g_free(meta);
}

void
bc_db_backend_free(BcDbBackend *backend)
{
  if (backend == NULL)
    return;
  if (backend->destroy)
    backend->destroy(backend->ctx);
  g_free(backend);
}
