/*
 * marmot-gobject - MarmotGobjectKeyPackage implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include "marmot-gobject-1.0/marmot-gobject-key-package.h"

enum {
    PROP_KP_0,
    PROP_KP_REF,
    PROP_KP_OWNER_PUBKEY,
    PROP_KP_RELAY_URLS,
    PROP_KP_CREATED_AT,
    PROP_KP_ACTIVE,
    N_KP_PROPERTIES
};

static GParamSpec *kp_properties[N_KP_PROPERTIES] = { NULL };

struct _MarmotGobjectKeyPackage {
    GObject parent_instance;
    gchar *ref_hex;
    gchar *owner_pubkey_hex;
    gchar **relay_urls;  /* NULL-terminated */
    gint64 created_at;
    gboolean active;
};

G_DEFINE_TYPE(MarmotGobjectKeyPackage, marmot_gobject_key_package, G_TYPE_OBJECT)

static void
marmot_gobject_key_package_get_property(GObject *object, guint prop_id,
                                         GValue *value, GParamSpec *pspec)
{
    MarmotGobjectKeyPackage *self = MARMOT_GOBJECT_KEY_PACKAGE(object);
    switch (prop_id) {
    case PROP_KP_REF:          g_value_set_string(value, self->ref_hex); break;
    case PROP_KP_OWNER_PUBKEY: g_value_set_string(value, self->owner_pubkey_hex); break;
    case PROP_KP_RELAY_URLS:   g_value_set_boxed(value, self->relay_urls); break;
    case PROP_KP_CREATED_AT:   g_value_set_int64(value, self->created_at); break;
    case PROP_KP_ACTIVE:       g_value_set_boolean(value, self->active); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec); break;
    }
}

static void
marmot_gobject_key_package_finalize(GObject *object)
{
    MarmotGobjectKeyPackage *self = MARMOT_GOBJECT_KEY_PACKAGE(object);
    g_clear_pointer(&self->ref_hex, g_free);
    g_clear_pointer(&self->owner_pubkey_hex, g_free);
    g_clear_pointer(&self->relay_urls, g_strfreev);
    G_OBJECT_CLASS(marmot_gobject_key_package_parent_class)->finalize(object);
}

static void
marmot_gobject_key_package_class_init(MarmotGobjectKeyPackageClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->get_property = marmot_gobject_key_package_get_property;
    oc->finalize = marmot_gobject_key_package_finalize;

    kp_properties[PROP_KP_REF] =
        g_param_spec_string("ref", NULL, NULL, NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    kp_properties[PROP_KP_OWNER_PUBKEY] =
        g_param_spec_string("owner-pubkey", NULL, NULL, NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    kp_properties[PROP_KP_RELAY_URLS] =
        g_param_spec_boxed("relay-urls", NULL, NULL, G_TYPE_STRV,
                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    kp_properties[PROP_KP_CREATED_AT] =
        g_param_spec_int64("created-at", NULL, NULL,
                           G_MININT64, G_MAXINT64, 0,
                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    kp_properties[PROP_KP_ACTIVE] =
        g_param_spec_boolean("active", NULL, NULL, FALSE,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(oc, N_KP_PROPERTIES, kp_properties);
}

static void
marmot_gobject_key_package_init(MarmotGobjectKeyPackage *self)
{
    (void)self;
}

MarmotGobjectKeyPackage *
marmot_gobject_key_package_new_from_data(const gchar *ref_hex,
                                          const gchar *owner_pubkey_hex,
                                          const gchar * const *relay_urls,
                                          gint64 created_at,
                                          gboolean active)
{
    MarmotGobjectKeyPackage *self = g_object_new(MARMOT_GOBJECT_TYPE_KEY_PACKAGE, NULL);
    self->ref_hex          = g_strdup(ref_hex);
    self->owner_pubkey_hex = g_strdup(owner_pubkey_hex);
    self->relay_urls       = g_strdupv((gchar **)relay_urls);
    self->created_at       = created_at;
    self->active           = active;
    return self;
}

/* ── Accessors ─────────────────────────────────────────────────── */

const gchar *marmot_gobject_key_package_get_ref(MarmotGobjectKeyPackage *self)          { return self->ref_hex; }
const gchar *marmot_gobject_key_package_get_owner_pubkey(MarmotGobjectKeyPackage *self) { return self->owner_pubkey_hex; }

const gchar * const *
marmot_gobject_key_package_get_relay_urls(MarmotGobjectKeyPackage *self)
{
    return (const gchar * const *)self->relay_urls;
}

gint64   marmot_gobject_key_package_get_created_at(MarmotGobjectKeyPackage *self) { return self->created_at; }
gboolean marmot_gobject_key_package_is_active(MarmotGobjectKeyPackage *self)      { return self->active; }
