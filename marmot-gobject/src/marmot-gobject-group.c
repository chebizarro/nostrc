/*
 * marmot-gobject - MarmotGobjectGroup implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include "marmot-gobject-1.0/marmot-gobject-group.h"
#include <string.h>

enum {
    PROP_GROUP_0,
    PROP_GROUP_MLS_GROUP_ID,
    PROP_GROUP_NOSTR_GROUP_ID,
    PROP_GROUP_NAME,
    PROP_GROUP_DESCRIPTION,
    PROP_GROUP_STATE,
    PROP_GROUP_EPOCH,
    PROP_GROUP_ADMIN_COUNT,
    PROP_GROUP_LAST_MESSAGE_AT,
    N_GROUP_PROPERTIES
};

static GParamSpec *group_properties[N_GROUP_PROPERTIES] = { NULL };

struct _MarmotGobjectGroup {
    GObject parent_instance;
    gchar *mls_group_id_hex;
    gchar *nostr_group_id_hex;
    gchar *name;
    gchar *description;
    MarmotGobjectGroupState state;
    guint64 epoch;
    gchar **admin_pubkey_hexes;   /* NULL-terminated */
    guint admin_count;
    gint64 last_message_at;
};

G_DEFINE_TYPE(MarmotGobjectGroup, marmot_gobject_group, G_TYPE_OBJECT)

static void
marmot_gobject_group_get_property(GObject *object, guint prop_id,
                                   GValue *value, GParamSpec *pspec)
{
    MarmotGobjectGroup *self = MARMOT_GOBJECT_GROUP(object);

    switch (prop_id) {
    case PROP_GROUP_MLS_GROUP_ID:
        g_value_set_string(value, self->mls_group_id_hex);
        break;
    case PROP_GROUP_NOSTR_GROUP_ID:
        g_value_set_string(value, self->nostr_group_id_hex);
        break;
    case PROP_GROUP_NAME:
        g_value_set_string(value, self->name);
        break;
    case PROP_GROUP_DESCRIPTION:
        g_value_set_string(value, self->description);
        break;
    case PROP_GROUP_STATE:
        g_value_set_enum(value, self->state);
        break;
    case PROP_GROUP_EPOCH:
        g_value_set_uint64(value, self->epoch);
        break;
    case PROP_GROUP_ADMIN_COUNT:
        g_value_set_uint(value, self->admin_count);
        break;
    case PROP_GROUP_LAST_MESSAGE_AT:
        g_value_set_int64(value, self->last_message_at);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
marmot_gobject_group_finalize(GObject *object)
{
    MarmotGobjectGroup *self = MARMOT_GOBJECT_GROUP(object);

    g_free(self->mls_group_id_hex);
    g_free(self->nostr_group_id_hex);
    g_free(self->name);
    g_free(self->description);
    g_strfreev(self->admin_pubkey_hexes);

    G_OBJECT_CLASS(marmot_gobject_group_parent_class)->finalize(object);
}

static void
marmot_gobject_group_class_init(MarmotGobjectGroupClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->get_property = marmot_gobject_group_get_property;
    object_class->finalize = marmot_gobject_group_finalize;

    group_properties[PROP_GROUP_MLS_GROUP_ID] =
        g_param_spec_string("mls-group-id", NULL, NULL, NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    group_properties[PROP_GROUP_NOSTR_GROUP_ID] =
        g_param_spec_string("nostr-group-id", NULL, NULL, NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    group_properties[PROP_GROUP_NAME] =
        g_param_spec_string("name", NULL, NULL, NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    group_properties[PROP_GROUP_DESCRIPTION] =
        g_param_spec_string("description", NULL, NULL, NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    group_properties[PROP_GROUP_STATE] =
        g_param_spec_enum("state", NULL, NULL,
                          MARMOT_GOBJECT_TYPE_GROUP_STATE,
                          MARMOT_GOBJECT_GROUP_STATE_ACTIVE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    group_properties[PROP_GROUP_EPOCH] =
        g_param_spec_uint64("epoch", NULL, NULL,
                            0, G_MAXUINT64, 0,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    group_properties[PROP_GROUP_ADMIN_COUNT] =
        g_param_spec_uint("admin-count", NULL, NULL,
                          0, G_MAXUINT, 0,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    group_properties[PROP_GROUP_LAST_MESSAGE_AT] =
        g_param_spec_int64("last-message-at", NULL, NULL,
                           G_MININT64, G_MAXINT64, 0,
                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_GROUP_PROPERTIES, group_properties);
}

static void
marmot_gobject_group_init(MarmotGobjectGroup *self)
{
    self->state = MARMOT_GOBJECT_GROUP_STATE_ACTIVE;
}

MarmotGobjectGroup *
marmot_gobject_group_new_from_data(const gchar *mls_group_id_hex,
                                    const gchar *nostr_group_id_hex,
                                    const gchar *name,
                                    const gchar *description,
                                    MarmotGobjectGroupState state,
                                    guint64 epoch)
{
    MarmotGobjectGroup *self = g_object_new(MARMOT_GOBJECT_TYPE_GROUP, NULL);
    self->mls_group_id_hex   = g_strdup(mls_group_id_hex);
    self->nostr_group_id_hex = g_strdup(nostr_group_id_hex);
    self->name               = g_strdup(name);
    self->description        = g_strdup(description);
    self->state              = state;
    self->epoch              = epoch;
    return self;
}

/* ── Accessors ─────────────────────────────────────────────────── */

const gchar *marmot_gobject_group_get_mls_group_id(MarmotGobjectGroup *self)  { return self->mls_group_id_hex; }
const gchar *marmot_gobject_group_get_nostr_group_id(MarmotGobjectGroup *self) { return self->nostr_group_id_hex; }
const gchar *marmot_gobject_group_get_name(MarmotGobjectGroup *self)           { return self->name; }
const gchar *marmot_gobject_group_get_description(MarmotGobjectGroup *self)    { return self->description; }
MarmotGobjectGroupState marmot_gobject_group_get_state(MarmotGobjectGroup *self) { return self->state; }
guint64 marmot_gobject_group_get_epoch(MarmotGobjectGroup *self)                { return self->epoch; }
guint marmot_gobject_group_get_admin_count(MarmotGobjectGroup *self)            { return self->admin_count; }
gint64 marmot_gobject_group_get_last_message_at(MarmotGobjectGroup *self)       { return self->last_message_at; }

gchar *
marmot_gobject_group_get_admin_pubkey_hex(MarmotGobjectGroup *self, guint index)
{
    if (!self->admin_pubkey_hexes || index >= self->admin_count)
        return NULL;
    return g_strdup(self->admin_pubkey_hexes[index]);
}
