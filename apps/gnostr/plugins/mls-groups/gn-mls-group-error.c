/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-mls-group-error.c - When a rejected group event means "out of sync"
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-mls-group-error.h"
#include <marmot/marmot-error.h>

/* marmot-gobject's error domain (its quark function is private; the domain
 * string is what it registers). */
#define MARMOT_GOBJECT_CLIENT_ERROR_DOMAIN "marmot-gobject-client-error"

gboolean
gn_mls_group_error_is_divergence(const GError *error)
{
  if (error == NULL ||
      error->domain != g_quark_from_static_string(MARMOT_GOBJECT_CLIENT_ERROR_DOMAIN))
    return FALSE;
  switch (error->code)
    {
    case MARMOT_ERR_MLS:
    case MARMOT_ERR_MLS_LIBRARY:
    case MARMOT_ERR_MLS_CREATE_MESSAGE:
    case MARMOT_ERR_MLS_EXPORT_SECRET:
    case MARMOT_ERR_MLS_MERGE_COMMIT:
    case MARMOT_ERR_MLS_SELF_UPDATE:
    case MARMOT_ERR_MLS_ADD_MEMBERS:
    case MARMOT_ERR_MLS_PROCESS_MESSAGE:
    case MARMOT_ERR_MLS_FRAMING:
      return TRUE;
    default:
      return FALSE;
    }
}

gboolean
gn_mls_group_error_gate_admit(GHashTable *reported, const gchar *group_id)
{
  if (reported == NULL || group_id == NULL || *group_id == '\0')
    return FALSE;
  if (g_hash_table_contains(reported, group_id))
    return FALSE;
  g_hash_table_add(reported, g_strdup(group_id));
  return TRUE;
}

void
gn_mls_group_error_gate_reset(GHashTable *reported, const gchar *group_id)
{
  if (reported != NULL && group_id != NULL)
    g_hash_table_remove(reported, group_id);
}
