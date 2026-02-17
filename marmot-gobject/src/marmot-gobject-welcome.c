/*
 * marmot-gobject - MarmotGobjectWelcome implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include "marmot-gobject-welcome.h"

enum {
    PROP_WEL_0,
    PROP_WEL_EVENT_ID,
    PROP_WEL_GROUP_NAME,
    PROP_WEL_GROUP_DESCRIPTION,
    PROP_WEL_WELCOMER,
    PROP_WEL_MEMBER_COUNT,
    PROP_WEL_STATE,
    PROP_WEL_MLS_GROUP_ID,
    PROP_WEL_NOSTR_GROUP_ID,
    N_WEL_PROPERTIES
};

static GParamSpec *wel_properties[N_WEL_PROPERTIES] = { NULL };

struct _MarmotGobjectWelcome {
    GObject parent_instance;
    gchar *event_id_hex;
    gchar *group_name;
    gchar *group_description;
    gchar *welcomer_hex;
    guint member_count;
    MarmotGobjectWelcomeState state;
    gchar *mls_group_id_hex;
    gchar *nostr_group_id_hex;
    gchar **relay_urls;  /* NULL-terminated */
};

G_DEFINE_TYPE(MarmotGobjectWelcome, marmot_gobject_welcome, G_TYPE_OBJECT)

static void
marmot_gobject_welcome_get_property(GObject *object, guint prop_id,
                                     GValue *value, GParamSpec *pspec)
{
    MarmotGobjectWelcome *self = MARMOT_GOBJECT_WELCOME(object);
    switch (prop_id) {
    case PROP_WEL_EVENT_ID:          g_value_set_string(value, self->event_id_hex); break;
    case PROP_WEL_GROUP_NAME:        g_value_set_string(value, self->group_name); break;
    case PROP_WEL_GROUP_DESCRIPTION: g_value_set_string(value, self->group_description); break;
    case PROP_WEL_WELCOMER:          g_value_set_string(value, self->welcomer_hex); break;
    case PROP_WEL_MEMBER_COUNT:      g_value_set_uint(value, self->member_count); break;
    case PROP_WEL_STATE:             g_value_set_enum(value, self->state); break;
    case PROP_WEL_MLS_GROUP_ID:      g_value_set_string(value, self->mls_group_id_hex); break;
    case PROP_WEL_NOSTR_GROUP_ID:    g_value_set_string(value, self->nostr_group_id_hex); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec); break;
    }
}

static void
marmot_gobject_welcome_finalize(GObject *object)
{
    MarmotGobjectWelcome *self = MARMOT_GOBJECT_WELCOME(object);
    g_free(self->event_id_hex);
    g_free(self->group_name);
    g_free(self->group_description);
    g_free(self->welcomer_hex);
    g_free(self->mls_group_id_hex);
    g_free(self->nostr_group_id_hex);
    g_strfreev(self->relay_urls);
    G_OBJECT_CLASS(marmot_gobject_welcome_parent_class)->finalize(object);
}

static void
marmot_gobject_welcome_class_init(MarmotGobjectWelcomeClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->get_property = marmot_gobject_welcome_get_property;
    oc->finalize = marmot_gobject_welcome_finalize;

    wel_properties[PROP_WEL_EVENT_ID] =
        g_param_spec_string("event-id", NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    wel_properties[PROP_WEL_GROUP_NAME] =
        g_param_spec_string("group-name", NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    wel_properties[PROP_WEL_GROUP_DESCRIPTION] =
        g_param_spec_string("group-description", NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    wel_properties[PROP_WEL_WELCOMER] =
        g_param_spec_string("welcomer", NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    wel_properties[PROP_WEL_MEMBER_COUNT] =
        g_param_spec_uint("member-count", NULL, NULL, 0, G_MAXUINT, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    wel_properties[PROP_WEL_STATE] =
        g_param_spec_enum("state", NULL, NULL, MARMOT_GOBJECT_TYPE_WELCOME_STATE,
                          MARMOT_GOBJECT_WELCOME_STATE_PENDING, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    wel_properties[PROP_WEL_MLS_GROUP_ID] =
        g_param_spec_string("mls-group-id", NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    wel_properties[PROP_WEL_NOSTR_GROUP_ID] =
        g_param_spec_string("nostr-group-id", NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(oc, N_WEL_PROPERTIES, wel_properties);
}

static void
marmot_gobject_welcome_init(MarmotGobjectWelcome *self) { (void)self; }

MarmotGobjectWelcome *
marmot_gobject_welcome_new_from_data(const gchar *event_id_hex,
                                      const gchar *group_name,
                                      const gchar *group_description,
                                      const gchar *welcomer_hex,
                                      guint member_count,
                                      MarmotGobjectWelcomeState state,
                                      const gchar *mls_group_id_hex,
                                      const gchar *nostr_group_id_hex)
{
    MarmotGobjectWelcome *self = g_object_new(MARMOT_GOBJECT_TYPE_WELCOME, NULL);
    self->event_id_hex      = g_strdup(event_id_hex);
    self->group_name        = g_strdup(group_name);
    self->group_description = g_strdup(group_description);
    self->welcomer_hex      = g_strdup(welcomer_hex);
    self->member_count      = member_count;
    self->state             = state;
    self->mls_group_id_hex  = g_strdup(mls_group_id_hex);
    self->nostr_group_id_hex = g_strdup(nostr_group_id_hex);
    return self;
}

/* ── Accessors ─────────────────────────────────────────────────── */

const gchar *marmot_gobject_welcome_get_event_id(MarmotGobjectWelcome *self)          { return self->event_id_hex; }
const gchar *marmot_gobject_welcome_get_group_name(MarmotGobjectWelcome *self)        { return self->group_name; }
const gchar *marmot_gobject_welcome_get_group_description(MarmotGobjectWelcome *self) { return self->group_description; }
const gchar *marmot_gobject_welcome_get_welcomer(MarmotGobjectWelcome *self)          { return self->welcomer_hex; }
guint        marmot_gobject_welcome_get_member_count(MarmotGobjectWelcome *self)      { return self->member_count; }
MarmotGobjectWelcomeState marmot_gobject_welcome_get_state(MarmotGobjectWelcome *self) { return self->state; }
const gchar *marmot_gobject_welcome_get_mls_group_id(MarmotGobjectWelcome *self)     { return self->mls_group_id_hex; }
const gchar *marmot_gobject_welcome_get_nostr_group_id(MarmotGobjectWelcome *self)   { return self->nostr_group_id_hex; }

const gchar * const *
marmot_gobject_welcome_get_relay_urls(MarmotGobjectWelcome *self)
{
    return (const gchar * const *)self->relay_urls;
}
