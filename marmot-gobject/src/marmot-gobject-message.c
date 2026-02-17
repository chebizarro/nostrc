/*
 * marmot-gobject - MarmotGobjectMessage implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include "marmot-gobject-1.0/marmot-gobject-message.h"

enum {
    PROP_MSG_0,
    PROP_MSG_EVENT_ID,
    PROP_MSG_PUBKEY,
    PROP_MSG_CONTENT,
    PROP_MSG_KIND,
    PROP_MSG_CREATED_AT,
    PROP_MSG_PROCESSED_AT,
    PROP_MSG_MLS_GROUP_ID,
    PROP_MSG_EPOCH,
    PROP_MSG_STATE,
    PROP_MSG_EVENT_JSON,
    N_MSG_PROPERTIES
};

static GParamSpec *msg_properties[N_MSG_PROPERTIES] = { NULL };

struct _MarmotGobjectMessage {
    GObject parent_instance;
    gchar *event_id_hex;
    gchar *pubkey_hex;
    gchar *content;
    guint kind;
    gint64 created_at;
    gint64 processed_at;
    gchar *mls_group_id_hex;
    guint64 epoch;
    MarmotGobjectMessageState state;
    gchar *event_json;
};

G_DEFINE_TYPE(MarmotGobjectMessage, marmot_gobject_message, G_TYPE_OBJECT)

static void
marmot_gobject_message_get_property(GObject *object, guint prop_id,
                                     GValue *value, GParamSpec *pspec)
{
    MarmotGobjectMessage *self = MARMOT_GOBJECT_MESSAGE(object);
    switch (prop_id) {
    case PROP_MSG_EVENT_ID:      g_value_set_string(value, self->event_id_hex); break;
    case PROP_MSG_PUBKEY:        g_value_set_string(value, self->pubkey_hex); break;
    case PROP_MSG_CONTENT:       g_value_set_string(value, self->content); break;
    case PROP_MSG_KIND:          g_value_set_uint(value, self->kind); break;
    case PROP_MSG_CREATED_AT:    g_value_set_int64(value, self->created_at); break;
    case PROP_MSG_PROCESSED_AT:  g_value_set_int64(value, self->processed_at); break;
    case PROP_MSG_MLS_GROUP_ID:  g_value_set_string(value, self->mls_group_id_hex); break;
    case PROP_MSG_EPOCH:         g_value_set_uint64(value, self->epoch); break;
    case PROP_MSG_STATE:         g_value_set_enum(value, self->state); break;
    case PROP_MSG_EVENT_JSON:    g_value_set_string(value, self->event_json); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec); break;
    }
}

static void
marmot_gobject_message_finalize(GObject *object)
{
    MarmotGobjectMessage *self = MARMOT_GOBJECT_MESSAGE(object);
    g_free(self->event_id_hex);
    g_free(self->pubkey_hex);
    g_free(self->content);
    g_free(self->mls_group_id_hex);
    g_free(self->event_json);
    G_OBJECT_CLASS(marmot_gobject_message_parent_class)->finalize(object);
}

static void
marmot_gobject_message_class_init(MarmotGobjectMessageClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->get_property = marmot_gobject_message_get_property;
    oc->finalize = marmot_gobject_message_finalize;

    msg_properties[PROP_MSG_EVENT_ID] =
        g_param_spec_string("event-id", NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    msg_properties[PROP_MSG_PUBKEY] =
        g_param_spec_string("pubkey", NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    msg_properties[PROP_MSG_CONTENT] =
        g_param_spec_string("content", NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    msg_properties[PROP_MSG_KIND] =
        g_param_spec_uint("kind", NULL, NULL, 0, G_MAXUINT, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    msg_properties[PROP_MSG_CREATED_AT] =
        g_param_spec_int64("created-at", NULL, NULL, G_MININT64, G_MAXINT64, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    msg_properties[PROP_MSG_PROCESSED_AT] =
        g_param_spec_int64("processed-at", NULL, NULL, G_MININT64, G_MAXINT64, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    msg_properties[PROP_MSG_MLS_GROUP_ID] =
        g_param_spec_string("mls-group-id", NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    msg_properties[PROP_MSG_EPOCH] =
        g_param_spec_uint64("epoch", NULL, NULL, 0, G_MAXUINT64, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    msg_properties[PROP_MSG_STATE] =
        g_param_spec_enum("state", NULL, NULL, MARMOT_GOBJECT_TYPE_MESSAGE_STATE,
                          MARMOT_GOBJECT_MESSAGE_STATE_CREATED, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    msg_properties[PROP_MSG_EVENT_JSON] =
        g_param_spec_string("event-json", NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(oc, N_MSG_PROPERTIES, msg_properties);
}

static void
marmot_gobject_message_init(MarmotGobjectMessage *self)
{
    (void)self;
}

MarmotGobjectMessage *
marmot_gobject_message_new_from_data(const gchar *event_id_hex,
                                      const gchar *pubkey_hex,
                                      const gchar *content,
                                      guint kind,
                                      gint64 created_at,
                                      const gchar *mls_group_id_hex)
{
    MarmotGobjectMessage *self = g_object_new(MARMOT_GOBJECT_TYPE_MESSAGE, NULL);
    self->event_id_hex     = g_strdup(event_id_hex);
    self->pubkey_hex       = g_strdup(pubkey_hex);
    self->content          = g_strdup(content);
    self->kind             = kind;
    self->created_at       = created_at;
    self->mls_group_id_hex = g_strdup(mls_group_id_hex);
    return self;
}

/* ── Accessors ─────────────────────────────────────────────────── */

const gchar *marmot_gobject_message_get_event_id(MarmotGobjectMessage *self) { return self->event_id_hex; }
const gchar *marmot_gobject_message_get_pubkey(MarmotGobjectMessage *self)   { return self->pubkey_hex; }
const gchar *marmot_gobject_message_get_content(MarmotGobjectMessage *self)  { return self->content; }
guint        marmot_gobject_message_get_kind(MarmotGobjectMessage *self)     { return self->kind; }
gint64       marmot_gobject_message_get_created_at(MarmotGobjectMessage *self)   { return self->created_at; }
gint64       marmot_gobject_message_get_processed_at(MarmotGobjectMessage *self) { return self->processed_at; }
const gchar *marmot_gobject_message_get_mls_group_id(MarmotGobjectMessage *self) { return self->mls_group_id_hex; }
guint64      marmot_gobject_message_get_epoch(MarmotGobjectMessage *self)    { return self->epoch; }
MarmotGobjectMessageState marmot_gobject_message_get_state(MarmotGobjectMessage *self) { return self->state; }
const gchar *marmot_gobject_message_get_event_json(MarmotGobjectMessage *self) { return self->event_json; }
