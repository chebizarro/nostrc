/**
 * SPDX-License-Identifier: MIT
 *
 * GNostrNip49: GObject wrapper for NIP-49 encrypted key operations
 *
 * Wraps the core NIP-49 encrypt/decrypt API (ncryptsec bech32) with
 * GObject properties, GError reporting, and integration with GNostrKeys.
 */

#include "nostr_nip49.h"
#include "nostr_keys.h"
#include <glib.h>
#include <string.h>

/* NIP-49 GLib thin wrapper (provides GError-based API + NFKC setup) */
#include "nostr/nip49/nip49_g.h"
/* Core NIP-49 for security enum */
#include "nostr/nip49/nip49.h"

/* Property IDs */
enum {
    PROP_0,
    PROP_NCRYPTSEC,
    PROP_SECURITY_LEVEL,
    PROP_LOG_N,
    N_PROPERTIES
};

static GParamSpec *obj_properties[N_PROPERTIES] = { NULL, };

struct _GNostrNip49 {
    GObject parent_instance;
    gchar *ncryptsec;                     /* bech32 ncryptsec string */
    GNostrNip49SecurityLevel security;    /* security level */
    guint8 log_n;                         /* scrypt exponent */
};

G_DEFINE_TYPE(GNostrNip49, gnostr_nip49, G_TYPE_OBJECT)

static void
gnostr_nip49_get_property(GObject    *object,
                           guint       property_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
    GNostrNip49 *self = GNOSTR_NIP49(object);

    switch (property_id) {
    case PROP_NCRYPTSEC:
        g_value_set_string(value, self->ncryptsec);
        break;
    case PROP_SECURITY_LEVEL:
        g_value_set_uint(value, self->security);
        break;
    case PROP_LOG_N:
        g_value_set_uint(value, self->log_n);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
gnostr_nip49_finalize(GObject *object)
{
    GNostrNip49 *self = GNOSTR_NIP49(object);

    g_free(self->ncryptsec);
    self->ncryptsec = NULL;

    G_OBJECT_CLASS(gnostr_nip49_parent_class)->finalize(object);
}

static void
gnostr_nip49_class_init(GNostrNip49Class *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->get_property = gnostr_nip49_get_property;
    object_class->finalize = gnostr_nip49_finalize;

    obj_properties[PROP_NCRYPTSEC] =
        g_param_spec_string("ncryptsec",
                            "Ncryptsec",
                            "The encrypted key as ncryptsec bech32 string",
                            NULL,
                            G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_SECURITY_LEVEL] =
        g_param_spec_uint("security-level",
                          "Security Level",
                          "NIP-49 security byte (0=insecure, 1=secure, 2=unknown)",
                          0, 2, GNOSTR_NIP49_SECURITY_UNKNOWN,
                          G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_LOG_N] =
        g_param_spec_uint("log-n",
                          "Log N",
                          "Scrypt exponent (e.g. 16=fast, 21=secure)",
                          0, 32, 16,
                          G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPERTIES, obj_properties);
}

static void
gnostr_nip49_init(GNostrNip49 *self)
{
    self->ncryptsec = NULL;
    self->security = GNOSTR_NIP49_SECURITY_UNKNOWN;
    self->log_n = 16;
}

/* Internal: hex string to raw bytes */
static gboolean
hex_to_bytes_nip49(const gchar *hex, guint8 *out, gsize out_len)
{
    gsize hex_len = strlen(hex);
    if (hex_len != out_len * 2)
        return FALSE;

    for (gsize i = 0; i < out_len; i++) {
        gchar byte_str[3] = { hex[i*2], hex[i*2+1], '\0' };
        gchar *endptr;
        gulong val = strtoul(byte_str, &endptr, 16);
        if (*endptr != '\0')
            return FALSE;
        out[i] = (guint8)val;
    }
    return TRUE;
}

/* Internal: raw bytes to hex string */
static gchar *
bytes_to_hex_nip49(const guint8 *bytes, gsize len)
{
    gchar *hex = g_malloc(len * 2 + 1);
    for (gsize i = 0; i < len; i++) {
        g_snprintf(hex + i * 2, 3, "%02x", bytes[i]);
    }
    hex[len * 2] = '\0';
    return hex;
}

/* Public API */

GNostrNip49 *
gnostr_nip49_new(void)
{
    return g_object_new(GNOSTR_TYPE_NIP49, NULL);
}

gchar *
gnostr_nip49_encrypt(GNostrNip49 *self,
                      const gchar *privkey_hex,
                      const gchar *password,
                      GNostrNip49SecurityLevel security,
                      guint8 log_n,
                      GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_NIP49(self), NULL);
    g_return_val_if_fail(privkey_hex != NULL, NULL);
    g_return_val_if_fail(password != NULL, NULL);

    /* Validate hex length */
    if (strlen(privkey_hex) != 64) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_INVALID_KEY,
                    "Private key must be 64 hex characters, got %zu",
                    strlen(privkey_hex));
        return NULL;
    }

    /* Convert hex to raw 32 bytes */
    guint8 privkey32[32];
    if (!hex_to_bytes_nip49(privkey_hex, privkey32, 32)) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_INVALID_KEY,
                    "Invalid hex encoding in private key");
        return NULL;
    }

    gchar *ncryptsec = NULL;
    gboolean ok = nostr_nip49_encrypt_g(privkey32,
                                         (guint8)security,
                                         password,
                                         log_n,
                                         &ncryptsec,
                                         error);

    /* Wipe raw key material */
    memset(privkey32, 0, sizeof(privkey32));

    if (!ok)
        return NULL;

    /* Update state */
    g_free(self->ncryptsec);
    self->ncryptsec = g_strdup(ncryptsec);
    self->security = security;
    self->log_n = log_n;

    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_NCRYPTSEC]);
    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_SECURITY_LEVEL]);
    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_LOG_N]);

    gchar *result = g_strdup(ncryptsec);
    g_free(ncryptsec);
    return result;
}

gchar *
gnostr_nip49_decrypt(GNostrNip49 *self,
                      const gchar *ncryptsec,
                      const gchar *password,
                      GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_NIP49(self), NULL);
    g_return_val_if_fail(ncryptsec != NULL, NULL);
    g_return_val_if_fail(password != NULL, NULL);

    guint8 privkey32[32];
    guint8 security_byte = 0;
    guint8 log_n = 0;

    gboolean ok = nostr_nip49_decrypt_g(ncryptsec,
                                         password,
                                         privkey32,
                                         &security_byte,
                                         &log_n,
                                         error);
    if (!ok)
        return NULL;

    /* Convert to hex */
    gchar *hex = bytes_to_hex_nip49(privkey32, 32);

    /* Wipe raw key material */
    memset(privkey32, 0, sizeof(privkey32));

    /* Update state */
    g_free(self->ncryptsec);
    self->ncryptsec = g_strdup(ncryptsec);
    self->security = (GNostrNip49SecurityLevel)security_byte;
    self->log_n = log_n;

    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_NCRYPTSEC]);
    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_SECURITY_LEVEL]);
    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_LOG_N]);

    return hex;
}

GObject *
gnostr_nip49_decrypt_to_keys(GNostrNip49 *self,
                              const gchar *ncryptsec,
                              const gchar *password,
                              GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_NIP49(self), NULL);

    gchar *privkey_hex = gnostr_nip49_decrypt(self, ncryptsec, password, error);
    if (privkey_hex == NULL)
        return NULL;

    GNostrKeys *keys = gnostr_keys_new_from_hex(privkey_hex, error);

    /* Securely wipe the hex key */
    memset(privkey_hex, 0, strlen(privkey_hex));
    g_free(privkey_hex);

    if (keys == NULL)
        return NULL;

    return G_OBJECT(keys);
}

const gchar *
gnostr_nip49_get_ncryptsec(GNostrNip49 *self)
{
    g_return_val_if_fail(GNOSTR_IS_NIP49(self), NULL);
    return self->ncryptsec;
}

GNostrNip49SecurityLevel
gnostr_nip49_get_security_level(GNostrNip49 *self)
{
    g_return_val_if_fail(GNOSTR_IS_NIP49(self), GNOSTR_NIP49_SECURITY_UNKNOWN);
    return self->security;
}

guint8
gnostr_nip49_get_log_n(GNostrNip49 *self)
{
    g_return_val_if_fail(GNOSTR_IS_NIP49(self), 0);
    return self->log_n;
}
