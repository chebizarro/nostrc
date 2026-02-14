/**
 * SPDX-License-Identifier: MIT
 *
 * GNostrBip39: GObject wrapper for BIP-39 mnemonic operations
 *
 * Wraps the core BIP-39 API (generate, validate, seed derivation)
 * with GObject properties, GError reporting, and NIP-06 key derivation
 * integration via GNostrKeys.
 */

#include "nostr_bip39.h"
#include "nostr_keys.h"
#include <glib.h>
#include <string.h>

/* Core BIP-39 API */
#include "nostr/crypto/bip39.h"

/* Property IDs */
enum {
    PROP_0,
    PROP_MNEMONIC,
    PROP_WORD_COUNT,
    PROP_IS_VALID,
    N_PROPERTIES
};

static GParamSpec *obj_properties[N_PROPERTIES] = { NULL, };

struct _GNostrBip39 {
    GObject parent_instance;
    gchar *mnemonic;       /* The mnemonic phrase */
    gint word_count;       /* Number of words */
    gboolean is_valid;     /* Whether validation passed */
};

G_DEFINE_TYPE(GNostrBip39, gnostr_bip39, G_TYPE_OBJECT)

static void
gnostr_bip39_get_property(GObject    *object,
                           guint       property_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
    GNostrBip39 *self = GNOSTR_BIP39(object);

    switch (property_id) {
    case PROP_MNEMONIC:
        g_value_set_string(value, self->mnemonic);
        break;
    case PROP_WORD_COUNT:
        g_value_set_int(value, self->word_count);
        break;
    case PROP_IS_VALID:
        g_value_set_boolean(value, self->is_valid);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
gnostr_bip39_finalize(GObject *object)
{
    GNostrBip39 *self = GNOSTR_BIP39(object);

    /* Securely wipe mnemonic before freeing - it's sensitive material */
    if (self->mnemonic != NULL) {
        memset(self->mnemonic, 0, strlen(self->mnemonic));
        g_free(self->mnemonic);
        self->mnemonic = NULL;
    }

    G_OBJECT_CLASS(gnostr_bip39_parent_class)->finalize(object);
}

static void
gnostr_bip39_class_init(GNostrBip39Class *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->get_property = gnostr_bip39_get_property;
    object_class->finalize = gnostr_bip39_finalize;

    obj_properties[PROP_MNEMONIC] =
        g_param_spec_string("mnemonic",
                            "Mnemonic",
                            "The BIP-39 mnemonic phrase",
                            NULL,
                            G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_WORD_COUNT] =
        g_param_spec_int("word-count",
                         "Word Count",
                         "Number of words in the mnemonic (12/15/18/21/24)",
                         0, 24, 0,
                         G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_IS_VALID] =
        g_param_spec_boolean("is-valid",
                             "Is Valid",
                             "Whether the mnemonic passes BIP-39 validation",
                             FALSE,
                             G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPERTIES, obj_properties);
}

static void
gnostr_bip39_init(GNostrBip39 *self)
{
    self->mnemonic = NULL;
    self->word_count = 0;
    self->is_valid = FALSE;
}

/* Internal: count words in a space-separated string */
static gint
count_words(const gchar *str)
{
    if (str == NULL || str[0] == '\0')
        return 0;

    gint count = 0;
    gboolean in_word = FALSE;

    for (const gchar *p = str; *p != '\0'; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n') {
            in_word = FALSE;
        } else if (!in_word) {
            in_word = TRUE;
            count++;
        }
    }

    return count;
}

/* Internal: update properties after mnemonic change */
static void
update_mnemonic_state(GNostrBip39 *self, const gchar *mnemonic)
{
    /* Wipe old mnemonic */
    if (self->mnemonic != NULL) {
        memset(self->mnemonic, 0, strlen(self->mnemonic));
        g_free(self->mnemonic);
    }

    self->mnemonic = g_strdup(mnemonic);
    self->word_count = count_words(mnemonic);
    self->is_valid = (mnemonic != NULL) ? nostr_bip39_validate(mnemonic) : FALSE;

    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_MNEMONIC]);
    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_WORD_COUNT]);
    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_IS_VALID]);
}

/* Public API */

GNostrBip39 *
gnostr_bip39_new(void)
{
    return g_object_new(GNOSTR_TYPE_BIP39, NULL);
}

const gchar *
gnostr_bip39_generate(GNostrBip39 *self,
                       gint word_count,
                       GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_BIP39(self), NULL);

    /* Validate word count */
    if (word_count != 12 && word_count != 15 &&
        word_count != 18 && word_count != 21 && word_count != 24) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_INVALID_KEY,
                    "Invalid word count %d: must be 12, 15, 18, 21, or 24",
                    word_count);
        return NULL;
    }

    char *generated = nostr_bip39_generate(word_count);
    if (generated == NULL) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_INVALID_KEY,
                    "Failed to generate BIP-39 mnemonic");
        return NULL;
    }

    update_mnemonic_state(self, generated);
    free(generated);

    return self->mnemonic;
}

gboolean
gnostr_bip39_set_mnemonic(GNostrBip39 *self,
                            const gchar *mnemonic,
                            GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_BIP39(self), FALSE);
    g_return_val_if_fail(mnemonic != NULL, FALSE);

    if (!nostr_bip39_validate(mnemonic)) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_INVALID_KEY,
                    "Invalid BIP-39 mnemonic: check word count, wordlist, and checksum");
        return FALSE;
    }

    update_mnemonic_state(self, mnemonic);
    return TRUE;
}

gboolean
gnostr_bip39_validate(const gchar *mnemonic)
{
    if (mnemonic == NULL)
        return FALSE;

    return nostr_bip39_validate(mnemonic);
}

gboolean
gnostr_bip39_to_seed(GNostrBip39 *self,
                      const gchar *passphrase,
                      guint8 **out_seed,
                      GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_BIP39(self), FALSE);
    g_return_val_if_fail(out_seed != NULL, FALSE);

    if (self->mnemonic == NULL) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_INVALID_STATE,
                    "No mnemonic loaded; call generate or set_mnemonic first");
        return FALSE;
    }

    *out_seed = g_malloc(64);

    const char *pass = (passphrase != NULL) ? passphrase : "";

    if (!nostr_bip39_seed(self->mnemonic, pass, *out_seed)) {
        g_free(*out_seed);
        *out_seed = NULL;
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_INVALID_KEY,
                    "PBKDF2 seed derivation failed");
        return FALSE;
    }

    return TRUE;
}

GObject *
gnostr_bip39_to_keys(GNostrBip39 *self,
                      const gchar *passphrase,
                      GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_BIP39(self), NULL);

    if (self->mnemonic == NULL) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_INVALID_STATE,
                    "No mnemonic loaded; call generate or set_mnemonic first");
        return NULL;
    }

    /* Delegate to GNostrKeys which already handles mnemonic -> NIP-06 derivation */
    GNostrKeys *keys = gnostr_keys_new_from_mnemonic(self->mnemonic, passphrase, error);
    if (keys == NULL)
        return NULL;

    return G_OBJECT(keys);
}

const gchar *
gnostr_bip39_get_mnemonic(GNostrBip39 *self)
{
    g_return_val_if_fail(GNOSTR_IS_BIP39(self), NULL);
    return self->mnemonic;
}

gint
gnostr_bip39_get_word_count(GNostrBip39 *self)
{
    g_return_val_if_fail(GNOSTR_IS_BIP39(self), 0);
    return self->word_count;
}

gboolean
gnostr_bip39_get_is_valid(GNostrBip39 *self)
{
    g_return_val_if_fail(GNOSTR_IS_BIP39(self), FALSE);
    return self->is_valid;
}
