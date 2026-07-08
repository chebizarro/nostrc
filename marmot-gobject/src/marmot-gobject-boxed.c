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

/* ══════════════════════════════════════════════════════════════════════════
 * MarmotGobjectEncryptedMedia
 * ══════════════════════════════════════════════════════════════════════════ */

MarmotGobjectEncryptedMedia *
marmot_gobject_encrypted_media_new(GBytes *encrypted_data,
                                    const gchar *mime_type,
                                    const gchar *filename,
                                    const gchar *url,
                                    gsize original_size,
                                    const guint8 file_hash[32],
                                    const guint8 nonce[12],
                                    guint64 epoch)
{
    MarmotGobjectEncryptedMedia *media = g_new0(MarmotGobjectEncryptedMedia, 1);
    media->encrypted_data = encrypted_data ? g_bytes_ref(encrypted_data) : NULL;
    media->mime_type = g_strdup(mime_type);
    media->filename = g_strdup(filename);
    media->url = g_strdup(url);
    media->original_size = original_size;
    if (file_hash)
        memcpy(media->file_hash, file_hash, 32);
    if (nonce)
        memcpy(media->nonce, nonce, 12);
    media->epoch = epoch;
    return media;
}

MarmotGobjectEncryptedMedia *
marmot_gobject_encrypted_media_copy(const MarmotGobjectEncryptedMedia *media)
{
    if (!media)
        return NULL;
    return marmot_gobject_encrypted_media_new(
        media->encrypted_data, media->mime_type, media->filename, media->url,
        media->original_size, media->file_hash, media->nonce, media->epoch);
}

void
marmot_gobject_encrypted_media_free(MarmotGobjectEncryptedMedia *media)
{
    if (!media)
        return;
    g_clear_pointer(&media->encrypted_data, g_bytes_unref);
    g_clear_pointer(&media->mime_type, g_free);
    g_clear_pointer(&media->filename, g_free);
    g_clear_pointer(&media->url, g_free);
    g_free(media);
}

GBytes *
marmot_gobject_encrypted_media_get_data(MarmotGobjectEncryptedMedia *media)
{
    return media ? media->encrypted_data : NULL;
}

const gchar *marmot_gobject_encrypted_media_get_mime_type(MarmotGobjectEncryptedMedia *media) { return media ? media->mime_type : NULL; }
const gchar *marmot_gobject_encrypted_media_get_filename(MarmotGobjectEncryptedMedia *media) { return media ? media->filename : NULL; }
gsize marmot_gobject_encrypted_media_get_original_size(MarmotGobjectEncryptedMedia *media) { return media ? media->original_size : 0; }
const guint8 *marmot_gobject_encrypted_media_get_file_hash(MarmotGobjectEncryptedMedia *media) { return media ? media->file_hash : NULL; }
const guint8 *marmot_gobject_encrypted_media_get_nonce(MarmotGobjectEncryptedMedia *media) { return media ? media->nonce : NULL; }
guint64 marmot_gobject_encrypted_media_get_epoch(MarmotGobjectEncryptedMedia *media) { return media ? media->epoch : 0; }

G_DEFINE_BOXED_TYPE(MarmotGobjectEncryptedMedia, marmot_gobject_encrypted_media,
                    marmot_gobject_encrypted_media_copy, marmot_gobject_encrypted_media_free)
