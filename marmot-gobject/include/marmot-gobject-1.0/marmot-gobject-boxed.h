/*
 * marmot-gobject - GObject wrapper for libmarmot
 *
 * GBoxed type registrations for value types that need to be passed
 * through GObject properties, signals, and GObject Introspection.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_GOBJECT_BOXED_H
#define MARMOT_GOBJECT_BOXED_H

#include <glib-object.h>

G_BEGIN_DECLS

/* ══════════════════════════════════════════════════════════════════════════
 * MarmotGobjectConfig — GBoxed wrapper for MarmotConfig
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * MarmotGobjectConfig:
 * @max_event_age_secs: Maximum age for accepted events (seconds). Default: 3888000 (45 days)
 * @max_future_skew_secs: Maximum future timestamp skew (seconds). Default: 300 (5 min)
 * @out_of_order_tolerance: Past message secrets to retain. Default: 100
 * @max_forward_distance: Max skipped messages before failure. Default: 1000
 * @epoch_snapshot_retention: Epoch snapshots to retain. Default: 5
 * @snapshot_ttl_seconds: Snapshot time-to-live (seconds). Default: 604800 (1 week)
 *
 * Configuration for Marmot behavior. All fields have secure defaults.
 *
 * Since: 1.0
 */
typedef struct {
    guint64 max_event_age_secs;
    guint64 max_future_skew_secs;
    guint32 out_of_order_tolerance;
    guint32 max_forward_distance;
    guint32 epoch_snapshot_retention;
    guint64 snapshot_ttl_seconds;
} MarmotGobjectConfig;

GType marmot_gobject_config_get_type(void) G_GNUC_CONST;
#define MARMOT_GOBJECT_TYPE_CONFIG (marmot_gobject_config_get_type())

/**
 * marmot_gobject_config_new_default:
 *
 * Creates a new MarmotGobjectConfig with secure defaults matching MDK.
 *
 * Returns: (transfer full): a new heap-allocated #MarmotGobjectConfig
 */
MarmotGobjectConfig *marmot_gobject_config_new_default(void);

/**
 * marmot_gobject_config_copy:
 * @config: a #MarmotGobjectConfig
 *
 * Creates a deep copy of @config.
 *
 * Returns: (transfer full): a new #MarmotGobjectConfig
 */
MarmotGobjectConfig *marmot_gobject_config_copy(const MarmotGobjectConfig *config);

/**
 * marmot_gobject_config_free:
 * @config: a #MarmotGobjectConfig
 *
 * Frees a #MarmotGobjectConfig allocated by marmot_gobject_config_new_default()
 * or marmot_gobject_config_copy().
 */
void marmot_gobject_config_free(MarmotGobjectConfig *config);

/* ══════════════════════════════════════════════════════════════════════════
 * MarmotGobjectPagination — GBoxed wrapper for MarmotPagination
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * MarmotGobjectPagination:
 * @limit: Maximum number of results. Default: 1000
 * @offset: Pagination offset. Default: 0
 *
 * Pagination parameters for list queries.
 *
 * Since: 1.0
 */
typedef struct {
    gsize limit;
    gsize offset;
} MarmotGobjectPagination;

GType marmot_gobject_pagination_get_type(void) G_GNUC_CONST;
#define MARMOT_GOBJECT_TYPE_PAGINATION (marmot_gobject_pagination_get_type())

/**
 * marmot_gobject_pagination_new_default:
 *
 * Creates a new MarmotGobjectPagination with defaults (limit=1000, offset=0).
 *
 * Returns: (transfer full): a new heap-allocated #MarmotGobjectPagination
 */
MarmotGobjectPagination *marmot_gobject_pagination_new_default(void);

/**
 * marmot_gobject_pagination_new:
 * @limit: maximum results
 * @offset: pagination offset
 *
 * Creates a new MarmotGobjectPagination with the given parameters.
 *
 * Returns: (transfer full): a new heap-allocated #MarmotGobjectPagination
 */
MarmotGobjectPagination *marmot_gobject_pagination_new(gsize limit, gsize offset);

/**
 * marmot_gobject_pagination_copy:
 * @pagination: a #MarmotGobjectPagination
 *
 * Creates a deep copy of @pagination.
 *
 * Returns: (transfer full): a new #MarmotGobjectPagination
 */
MarmotGobjectPagination *marmot_gobject_pagination_copy(const MarmotGobjectPagination *pagination);

/**
 * marmot_gobject_pagination_free:
 * @pagination: a #MarmotGobjectPagination
 *
 * Frees a #MarmotGobjectPagination.
 */
void marmot_gobject_pagination_free(MarmotGobjectPagination *pagination);

G_END_DECLS

#endif /* MARMOT_GOBJECT_BOXED_H */
