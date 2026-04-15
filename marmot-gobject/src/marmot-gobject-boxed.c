/*
 * marmot-gobject - GBoxed type implementations
 *
 * SPDX-License-Identifier: MIT
 */

#include "marmot-gobject-1.0/marmot-gobject-boxed.h"
#include <string.h>

/* ══════════════════════════════════════════════════════════════════════════
 * MarmotGobjectConfig
 * ══════════════════════════════════════════════════════════════════════════ */

MarmotGobjectConfig *
marmot_gobject_config_new_default(void)
{
    MarmotGobjectConfig *config = g_new0(MarmotGobjectConfig, 1);
    config->max_event_age_secs     = 3888000;   /* 45 days */
    config->max_future_skew_secs   = 300;       /* 5 minutes */
    config->out_of_order_tolerance = 100;
    config->max_forward_distance   = 1000;
    config->epoch_snapshot_retention = 5;
    config->snapshot_ttl_seconds   = 604800;    /* 1 week */
    return config;
}

MarmotGobjectConfig *
marmot_gobject_config_copy(const MarmotGobjectConfig *config)
{
    if (!config)
        return NULL;
    MarmotGobjectConfig *copy = g_new(MarmotGobjectConfig, 1);
    memcpy(copy, config, sizeof(MarmotGobjectConfig));
    return copy;
}

void
marmot_gobject_config_free(MarmotGobjectConfig *config)
{
    g_free(config);
}

G_DEFINE_BOXED_TYPE(MarmotGobjectConfig, marmot_gobject_config,
                    marmot_gobject_config_copy, marmot_gobject_config_free)

/* ══════════════════════════════════════════════════════════════════════════
 * MarmotGobjectPagination
 * ══════════════════════════════════════════════════════════════════════════ */

MarmotGobjectPagination *
marmot_gobject_pagination_new_default(void)
{
    MarmotGobjectPagination *p = g_new0(MarmotGobjectPagination, 1);
    p->limit  = 1000;
    p->offset = 0;
    return p;
}

MarmotGobjectPagination *
marmot_gobject_pagination_new(gsize limit, gsize offset)
{
    MarmotGobjectPagination *p = g_new0(MarmotGobjectPagination, 1);
    p->limit  = limit;
    p->offset = offset;
    return p;
}

MarmotGobjectPagination *
marmot_gobject_pagination_copy(const MarmotGobjectPagination *pagination)
{
    if (!pagination)
        return NULL;
    MarmotGobjectPagination *copy = g_new(MarmotGobjectPagination, 1);
    memcpy(copy, pagination, sizeof(MarmotGobjectPagination));
    return copy;
}

void
marmot_gobject_pagination_free(MarmotGobjectPagination *pagination)
{
    g_free(pagination);
}

G_DEFINE_BOXED_TYPE(MarmotGobjectPagination, marmot_gobject_pagination,
                    marmot_gobject_pagination_copy, marmot_gobject_pagination_free)
