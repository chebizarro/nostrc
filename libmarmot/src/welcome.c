/*
 * libmarmot - MIP-02: Welcome Events
 *
 * Processes kind:444 welcome events (NIP-59 gift-wrapped).
 *
 * Welcome processing flow:
 *   1. Receive kind:1059 gift wrap event
 *   2. NIP-59 unwrap → kind:444 rumor (unsigned)
 *   3. Decode content (base64 → MLS Welcome bytes)
 *   4. Parse MLS Welcome to extract group preview info
 *   5. Store as pending welcome
 *   6. On accept: process MLS Welcome → initialize group state
 *
 * SPDX-License-Identifier: MIT
 */

#include "marmot-internal.h"
#include "mls/mls_welcome.h"
#include "mls/mls_key_package.h"
#include "mls/mls-internal.h"
#include <nostr-event.h>
#include <nostr-tag.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Internal base64 helpers (shared with credentials.c)
 * ──────────────────────────────────────────────────────────────────────── */

static uint8_t *
base64_decode(const char *b64, size_t *out_len)
{
    if (!b64 || !out_len) return NULL;
    size_t b64_len = strlen(b64);
    size_t max_bin = (b64_len / 4) * 3 + 3;
    uint8_t *out = malloc(max_bin);
    if (!out) return NULL;

    size_t bin_len = 0;
    if (sodium_base642bin(out, max_bin, b64, b64_len,
                          NULL, &bin_len, NULL,
                          sodium_base64_VARIANT_ORIGINAL) != 0) {
        free(out);
        return NULL;
    }
    *out_len = bin_len;
    return out;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: extract group preview from MLS Welcome's GroupInfo extension
 * ──────────────────────────────────────────────────────────────────────── */

static void
extract_group_preview(MarmotWelcome *welcome,
                       const uint8_t *welcome_data, size_t welcome_len)
{
    /*
     * To preview group info without fully processing the Welcome,
     * we would need to parse the encrypted GroupInfo — which requires
     * knowing the welcome_secret. This isn't possible without the
     * KeyPackage private key.
     *
     * Instead, we store the raw Welcome data and extract preview info
     * when the user accepts the welcome (at which point we decrypt
     * the GroupInfo and read the extension).
     *
     * For now, the preview fields are left empty and populated on accept.
     */
    (void)welcome_data;
    (void)welcome_len;

    /* These will be populated on accept */
    memset(welcome->nostr_group_id, 0, 32);
    welcome->group_name = NULL;
    welcome->group_description = NULL;
    welcome->member_count = 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_process_welcome
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_process_welcome(Marmot *m,
                        const uint8_t wrapper_event_id[32],
                        const char *rumor_event_json,
                        MarmotWelcome **out_welcome)
{
    if (!m || !wrapper_event_id || !rumor_event_json || !out_welcome)
        return MARMOT_ERR_INVALID_ARG;

    *out_welcome = NULL;

    /* Parse the rumor event (kind:444, unsigned) */
    NostrEvent rumor;
    memset(&rumor, 0, sizeof(rumor));
    if (!nostr_event_deserialize_compact(&rumor, rumor_event_json, NULL))
        return MARMOT_ERR_DESERIALIZATION;

    /* Verify kind */
    if (rumor.kind != MARMOT_KIND_WELCOME) {
        /* Free stack-allocated event fields */
        free(rumor.id); free(rumor.pubkey); free(rumor.content);
        free(rumor.sig); nostr_tags_free(rumor.tags);
        return MARMOT_ERR_INVALID_ARG;
    }

    /* Get content */
    if (!rumor.content || strlen(rumor.content) == 0) {
        free(rumor.id); free(rumor.pubkey); free(rumor.content);
        free(rumor.sig); nostr_tags_free(rumor.tags);
        return MARMOT_ERR_DESERIALIZATION;
    }

    /* Check encoding */
    bool is_base64 = false;
    if (rumor.tags) {
        for (size_t i = 0; i < nostr_tags_size(rumor.tags); i++) {
            NostrTag *tag = nostr_tags_get(rumor.tags, i);
            if (nostr_tag_size(tag) >= 2 &&
                strcmp(nostr_tag_get_key(tag), "encoding") == 0 &&
                strcmp(nostr_tag_get_value(tag), "base64") == 0) {
                is_base64 = true;
                break;
            }
        }
    }

    /* Decode content to MLS Welcome bytes */
    uint8_t *welcome_data = NULL;
    size_t welcome_len = 0;

    if (is_base64) {
        welcome_data = base64_decode(rumor.content, &welcome_len);
    } else {
        /* Hex decode (deprecated) */
        size_t hex_len = strlen(rumor.content);
        if (hex_len % 2 == 0) {
            welcome_len = hex_len / 2;
            welcome_data = malloc(welcome_len);
            if (welcome_data && marmot_hex_decode(rumor.content, welcome_data, welcome_len) != 0) {
                free(welcome_data);
                welcome_data = NULL;
            }
        }
    }

    /* Extract relay URLs from tags */
    char **relay_urls = NULL;
    size_t relay_count = 0;
    if (rumor.tags) {
        for (size_t i = 0; i < nostr_tags_size(rumor.tags); i++) {
            NostrTag *tag = nostr_tags_get(rumor.tags, i);
            if (nostr_tag_size(tag) >= 2 &&
                strcmp(nostr_tag_get_key(tag), "relays") == 0) {
                relay_count = nostr_tag_size(tag) - 1; /* skip the key */
                if (relay_count > 0) {
                    relay_urls = calloc(relay_count, sizeof(char *));
                    for (size_t j = 0; j < relay_count && relay_urls; j++) {
                        relay_urls[j] = strdup(nostr_tag_get(tag, j + 1));
                    }
                }
                break;
            }
        }
    }

    /* Free the rumor event */
    free(rumor.id); free(rumor.pubkey); free(rumor.content);
    free(rumor.sig); nostr_tags_free(rumor.tags);

    if (!welcome_data) return MARMOT_ERR_DESERIALIZATION;

    /* Create the MarmotWelcome record */
    MarmotWelcome *welcome = marmot_welcome_new();
    if (!welcome) {
        free(welcome_data);
        if (relay_urls) {
            for (size_t i = 0; i < relay_count; i++) free(relay_urls[i]);
            free(relay_urls);
        }
        return MARMOT_ERR_MEMORY;
    }

    memcpy(welcome->wrapper_event_id, wrapper_event_id, 32);
    welcome->event_json = strdup(rumor_event_json);
    welcome->state = MARMOT_WELCOME_STATE_PENDING;

    /* Set relays */
    welcome->group_relays = relay_urls;
    welcome->group_relay_count = relay_count;

    /* Extract preview info (limited without decryption) */
    extract_group_preview(welcome, welcome_data, welcome_len);

    /* Store the raw welcome data for later processing */
    if (m->storage && m->storage->mls_store) {
        m->storage->mls_store(m->storage->ctx, "welcome_data",
                               wrapper_event_id, 32,
                               welcome_data, welcome_len);
    }
    free(welcome_data);

    /* Store the welcome */
    if (m->storage && m->storage->save_welcome) {
        m->storage->save_welcome(m->storage->ctx, welcome);
    }

    *out_welcome = welcome;
    return MARMOT_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_accept_welcome
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_accept_welcome(Marmot *m, const MarmotWelcome *welcome)
{
    if (!m || !welcome)
        return MARMOT_ERR_INVALID_ARG;
    if (!m->storage || !m->storage->mls_load)
        return MARMOT_ERR_STORAGE;

    /* Retrieve the raw MLS Welcome data from storage */
    uint8_t *welcome_data = NULL;
    size_t welcome_len = 0;
    if (m->storage->mls_load(m->storage->ctx, "welcome_data",
                              welcome->wrapper_event_id, 32,
                              &welcome_data, &welcome_len) != 0) {
        return MARMOT_ERR_STORAGE;
    }

    /* We need to find which KeyPackage was used for this Welcome.
     * The MLS Welcome contains KeyPackageRef entries — we need to
     * match against our stored KeyPackage private keys. */

    /* Deserialize the MLS Welcome to find our KeyPackageRef */
    MlsWelcome mls_welcome;
    memset(&mls_welcome, 0, sizeof(mls_welcome));
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, welcome_data, welcome_len);

    if (mls_welcome_deserialize(&reader, &mls_welcome) != 0) {
        free(welcome_data);
        return MARMOT_ERR_MLS;
    }
    free(welcome_data);

    /* Find our entry among the EncryptedGroupSecrets */
    MlsKeyPackage matched_kp;
    MlsKeyPackagePrivate matched_priv;
    bool found = false;

    for (size_t i = 0; i < mls_welcome.secret_count; i++) {
        uint8_t *priv_data = NULL;
        size_t priv_len = 0;

        /* Look up private key material by (label="kp_priv", key=KeyPackageRef) */
        if (m->storage->mls_load &&
            m->storage->mls_load(m->storage->ctx, "kp_priv",
                                  mls_welcome.secrets[i].key_package_ref,
                                  MLS_HASH_LEN,
                                  &priv_data, &priv_len) == 0 &&
            priv_data != NULL &&
            priv_len == (MLS_KEM_SK_LEN + MLS_KEM_SK_LEN + MLS_SIG_SK_LEN)) {
            /* Found our KeyPackage private material */
            memcpy(matched_priv.init_key_private, priv_data, MLS_KEM_SK_LEN);
            memcpy(matched_priv.encryption_key_private,
                   priv_data + MLS_KEM_SK_LEN, MLS_KEM_SK_LEN);
            memcpy(matched_priv.signature_key_private,
                   priv_data + MLS_KEM_SK_LEN + MLS_KEM_SK_LEN, MLS_SIG_SK_LEN);
            free(priv_data);

            /* We don't have the full KeyPackage here, but mls_welcome_process_parsed
             * needs it. We'll create a minimal one. */
            memset(&matched_kp, 0, sizeof(matched_kp));
            matched_kp.version = 1; /* mls10 */
            matched_kp.cipher_suite = MARMOT_CIPHERSUITE;
            /* Derive public keys from private */
            crypto_scalarmult_base(matched_kp.init_key, matched_priv.init_key_private);
            found = true;
            break;
        }
        if (priv_data) free(priv_data);
    }

    if (!found) {
        mls_welcome_clear(&mls_welcome);
        return MARMOT_ERR_KEY_NOT_FOUND;
    }

    /* Process the MLS Welcome to join the group */
    MlsGroup mls_group;
    memset(&mls_group, 0, sizeof(mls_group));

    int rc = mls_welcome_process_parsed(&mls_welcome, &matched_kp, &matched_priv,
                                         NULL, 0, /* no out-of-band ratchet tree */
                                         &mls_group);
    mls_welcome_clear(&mls_welcome);
    mls_key_package_clear(&matched_kp);
    sodium_memzero(&matched_priv, sizeof(matched_priv));

    if (rc != 0) return MARMOT_ERR_MLS;

    /* Extract GroupData extension from the group's extensions */
    MarmotGroupDataExtension *gde = NULL;
    if (mls_group.extensions_data && mls_group.extensions_len > 0) {
        /* Extensions are TLS-serialized. Find the 0xF2EE extension. */
        MlsTlsReader ext_reader;
        mls_tls_reader_init(&ext_reader, mls_group.extensions_data,
                            mls_group.extensions_len);

        while (!mls_tls_reader_done(&ext_reader)) {
            uint16_t ext_type;
            if (mls_tls_read_u16(&ext_reader, &ext_type) != 0) break;

            uint8_t *ext_data = NULL;
            size_t ext_data_len = 0;
            if (mls_tls_read_opaque16(&ext_reader, &ext_data, &ext_data_len) != 0)
                break;

            if (ext_type == MARMOT_EXTENSION_TYPE) {
                gde = marmot_group_data_extension_deserialize(ext_data, ext_data_len);
            }
            free(ext_data);

            if (gde) break;
        }
    }

    /* Create the MarmotGroup */
    MarmotGroup *group = marmot_group_new();
    if (!group) {
        mls_group_free(&mls_group);
        marmot_group_data_extension_free(gde);
        return MARMOT_ERR_MEMORY;
    }

    group->mls_group_id = marmot_group_id_new(mls_group.group_id, mls_group.group_id_len);
    group->epoch = mls_group.epoch;
    group->state = MARMOT_GROUP_STATE_ACTIVE;

    if (gde) {
        memcpy(group->nostr_group_id, gde->nostr_group_id, 32);
        if (gde->name) group->name = strdup(gde->name);
        if (gde->description) group->description = strdup(gde->description);

        if (gde->admin_count > 0 && gde->admins) {
            group->admin_count = gde->admin_count;
            group->admin_pubkeys = malloc(gde->admin_count * 32);
            if (group->admin_pubkeys)
                memcpy(group->admin_pubkeys, gde->admins, gde->admin_count * 32);
        }

        if (gde->image_hash) {
            group->image_hash = malloc(32);
            if (group->image_hash) memcpy(group->image_hash, gde->image_hash, 32);
        }
        if (gde->image_key) {
            group->image_key = malloc(32);
            if (group->image_key) memcpy(group->image_key, gde->image_key, 32);
        }
        if (gde->image_nonce) {
            group->image_nonce = malloc(12);
            if (group->image_nonce) memcpy(group->image_nonce, gde->image_nonce, 12);
        }
    }
    marmot_group_data_extension_free(gde);

    /* Store exporter secret */
    if (m->storage && m->storage->save_exporter_secret) {
        m->storage->save_exporter_secret(m->storage->ctx,
                                          &group->mls_group_id,
                                          mls_group.epoch,
                                          mls_group.epoch_secrets.exporter_secret);
    }

    /* Store the group */
    if (m->storage && m->storage->save_group) {
        m->storage->save_group(m->storage->ctx, group);
    }

    mls_group_free(&mls_group);
    marmot_group_free(group);

    return MARMOT_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_decline_welcome
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_decline_welcome(Marmot *m, const MarmotWelcome *welcome)
{
    if (!m || !welcome)
        return MARMOT_ERR_INVALID_ARG;

    /* Clean up stored welcome data */
    if (m->storage && m->storage->mls_delete) {
        m->storage->mls_delete(m->storage->ctx, "welcome_data",
                                welcome->wrapper_event_id, 32);
    }

    /* Note: We do NOT delete the KeyPackage from relays here per MIP-02:
     * "If Welcome processing fails, do NOT delete the KeyPackage from relays" */

    return MARMOT_OK;
}
