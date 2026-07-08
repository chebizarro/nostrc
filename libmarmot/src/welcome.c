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
#include "mls/mls_group.h"
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

static bool
is_hex_len(const char *s, size_t len)
{
    if (!s || strlen(s) != len) return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

static void
extract_group_preview_from_tags(MarmotWelcome *welcome, NostrTags *tags)
{
    memset(welcome->nostr_group_id, 0, 32);
    welcome->group_name = NULL;
    welcome->group_description = NULL;
    welcome->member_count = 0;

    if (!tags) return;

    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (!tag || nostr_tag_size(tag) < 2) continue;
        const char *key = nostr_tag_get_key(tag);
        const char *val = nostr_tag_get_value(tag);
        if (!key || !val) continue;

        if (strcmp(key, "h") == 0 && is_hex_len(val, 64)) {
            marmot_hex_decode(val, welcome->nostr_group_id, 32);
        } else if (strcmp(key, "name") == 0 && !welcome->group_name) {
            welcome->group_name = strdup(val);
        } else if (strcmp(key, "description") == 0 && !welcome->group_description) {
            welcome->group_description = strdup(val);
        } else if (strcmp(key, "member_count") == 0) {
            welcome->member_count = (size_t)strtoull(val, NULL, 10);
        } else if (strcmp(key, "admin") == 0 && is_hex_len(val, 64)) {
            uint8_t (*admins)[32] = realloc(welcome->group_admin_pubkeys,
                                            (welcome->group_admin_count + 1) * 32);
            if (admins) {
                welcome->group_admin_pubkeys = admins;
                marmot_hex_decode(val, welcome->group_admin_pubkeys[welcome->group_admin_count], 32);
                welcome->group_admin_count++;
            }
        }
    }
}

static void
record_welcome_failure(Marmot *m, const uint8_t wrapper_event_id[32],
                       const char *reason)
{
    if (m && m->storage && m->storage->save_processed_welcome) {
        m->storage->save_processed_welcome(m->storage->ctx,
                                           wrapper_event_id,
                                           NULL,
                                           marmot_now(),
                                           MARMOT_WELCOME_STATE_FAILED,
                                           reason);
    }
}

static MarmotError
error_for_processed_welcome_state(int state)
{
    switch ((MarmotWelcomeState)state) {
    case MARMOT_WELCOME_STATE_ACCEPTED:
        return MARMOT_ERR_WELCOME_ALREADY_ACCEPTED;
    case MARMOT_WELCOME_STATE_DECLINED:
        return MARMOT_ERR_WELCOME_ALREADY_DECLINED;
    case MARMOT_WELCOME_STATE_FAILED:
        return MARMOT_ERR_WELCOME_PREVIOUSLY_FAILED;
    case MARMOT_WELCOME_STATE_PENDING:
    default:
        return MARMOT_ERR_WELCOME;
    }
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

    if (!m->storage || !m->storage->mls_store || !m->storage->save_welcome ||
        !m->storage->save_processed_welcome)
        return MARMOT_ERR_STORAGE;

    /* Check for duplicate: was this wrapper event already processed? */
    if (m->storage && m->storage->find_processed_welcome) {
        bool already_processed = false;
        int state = 0;
        char *reason = NULL;
        if (m->storage->find_processed_welcome(m->storage->ctx,
                                                 wrapper_event_id,
                                                 &already_processed,
                                                 &state, &reason) == MARMOT_OK &&
            already_processed) {
            MarmotError processed_err = error_for_processed_welcome_state(state);
            free(reason);
            return processed_err;
        }
        free(reason);
    }

    /* Parse the rumor event (kind:444, unsigned) */
    NostrEvent rumor;
    memset(&rumor, 0, sizeof(rumor));
    if (!nostr_event_deserialize_compact(&rumor, rumor_event_json, NULL)) {
        record_welcome_failure(m, wrapper_event_id, "deserialization failed");
        return MARMOT_ERR_DESERIALIZATION;
    }

    /* Verify kind */
    if (rumor.kind != MARMOT_KIND_WELCOME) {
        /* Free stack-allocated event fields */
        free(rumor.id); free(rumor.pubkey); free(rumor.content);
        free(rumor.sig); nostr_tags_free(rumor.tags);
        record_welcome_failure(m, wrapper_event_id, "unexpected welcome kind");
        return MARMOT_ERR_INVALID_ARG;
    }

    /* Check welcome expiry: reject events older than max_event_age */
    if (rumor.created_at > 0 && m->config.max_event_age_secs > 0) {
        int64_t now = marmot_now();
        int64_t age = now - rumor.created_at;
        if (age > (int64_t)m->config.max_event_age_secs) {
            free(rumor.id); free(rumor.pubkey); free(rumor.content);
            free(rumor.sig); nostr_tags_free(rumor.tags);
            record_welcome_failure(m, wrapper_event_id, "welcome expired");
            return MARMOT_ERR_WELCOME_EXPIRED;
        }
    }

    /* Get content */
    if (!rumor.content || strlen(rumor.content) == 0) {
        free(rumor.id); free(rumor.pubkey); free(rumor.content);
        free(rumor.sig); nostr_tags_free(rumor.tags);
        record_welcome_failure(m, wrapper_event_id, "empty welcome content");
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

    if (!welcome_data) {
        free(rumor.id); free(rumor.pubkey); free(rumor.content);
        free(rumor.sig); nostr_tags_free(rumor.tags);
        record_welcome_failure(m, wrapper_event_id, "welcome content decode failed");
        return MARMOT_ERR_DESERIALIZATION;
    }

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
    if (rumor.id && is_hex_len(rumor.id, 64))
        marmot_hex_decode(rumor.id, welcome->id, 32);
    else
        memcpy(welcome->id, wrapper_event_id, 32);
    if (rumor.pubkey && is_hex_len(rumor.pubkey, 64))
        marmot_hex_decode(rumor.pubkey, welcome->welcomer, 32);
    welcome->event_json = strdup(rumor_event_json);
    welcome->state = MARMOT_WELCOME_STATE_PENDING;

    /* Set relays */
    welcome->group_relays = relay_urls;
    welcome->group_relay_count = relay_count;

    /* Extract cleartext preview info from rumor tags. */
    extract_group_preview_from_tags(welcome, rumor.tags);

    /* Free the rumor event after preview extraction */
    free(rumor.id); free(rumor.pubkey); free(rumor.content);
    free(rumor.sig); nostr_tags_free(rumor.tags);

    /* Store the raw welcome data for later processing. Mandatory: accepting
     * the welcome requires this MLS Welcome blob. */
    MarmotError err = m->storage->mls_store(m->storage->ctx, "welcome_data",
                                             wrapper_event_id, 32,
                                             welcome_data, welcome_len);
    free(welcome_data);
    if (err != MARMOT_OK) {
        marmot_welcome_free(welcome);
        return err;
    }

    /* Store the welcome metadata. Mandatory for pending-welcome APIs. */
    err = m->storage->save_welcome(m->storage->ctx, welcome);
    if (err != MARMOT_OK) {
        if (m->storage->mls_delete)
            m->storage->mls_delete(m->storage->ctx, "welcome_data", wrapper_event_id, 32);
        marmot_welcome_free(welcome);
        return err;
    }

    *out_welcome = welcome;
    return MARMOT_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_accept_welcome
 * ──────────────────────────────────────────────────────────────────────── */

static MarmotError
accept_welcome_internal(Marmot *m, const MarmotWelcome *welcome, MarmotGroup **out_group)
{
    if (out_group)
        *out_group = NULL;
    if (!m || !welcome)
        return MARMOT_ERR_INVALID_ARG;
    if (!m->storage || !m->storage->mls_load || !m->storage->mls_store ||
        !m->storage->save_exporter_secret || !m->storage->save_group ||
        !m->storage->save_welcome || !m->storage->save_processed_welcome)
        return MARMOT_ERR_STORAGE;

    /* Retrieve the raw MLS Welcome data from storage */
    uint8_t *welcome_data = NULL;
    size_t welcome_len = 0;
    if (m->storage->mls_load(m->storage->ctx, "welcome_data",
                              welcome->wrapper_event_id, 32,
                              &welcome_data, &welcome_len) != 0) {
        record_welcome_failure(m, welcome->wrapper_event_id, "stored welcome data not found");
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
        record_welcome_failure(m, welcome->wrapper_event_id, "MLS Welcome deserialize failed");
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

            /* Load the full serialized KeyPackage so mls_welcome_process_parsed
             * can compute the correct KeyPackageRef and populate the tree. */
            memset(&matched_kp, 0, sizeof(matched_kp));

            uint8_t *kp_data = NULL;
            size_t kp_len = 0;
            if (m->storage->mls_load(m->storage->ctx, "kp_full",
                                      mls_welcome.secrets[i].key_package_ref,
                                      MLS_HASH_LEN,
                                      &kp_data, &kp_len) == 0 && kp_data) {
                MlsTlsReader kp_reader;
                mls_tls_reader_init(&kp_reader, kp_data, kp_len);
                if (mls_key_package_deserialize(&kp_reader, &matched_kp) != 0) {
                    free(kp_data);
                    /* Fallback: create minimal KP (may fail ref check) */
                    matched_kp.version = 1;
                    matched_kp.cipher_suite = MARMOT_CIPHERSUITE;
                    crypto_scalarmult_base(matched_kp.init_key,
                                           matched_priv.init_key_private);
                } else {
                    free(kp_data);
                }
            } else {
                free(kp_data);
                /* Fallback: create minimal KP (may fail ref check) */
                matched_kp.version = 1;
                matched_kp.cipher_suite = MARMOT_CIPHERSUITE;
                crypto_scalarmult_base(matched_kp.init_key,
                                       matched_priv.init_key_private);
            }
            found = true;
            break;
        }
        if (priv_data) free(priv_data);
    }

    if (!found) {
        mls_welcome_clear(&mls_welcome);
        record_welcome_failure(m, welcome->wrapper_event_id, "matching KeyPackage private key not found");
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

    if (rc != 0) {
        record_welcome_failure(m, welcome->wrapper_event_id, "MLS Welcome processing failed");
        return MARMOT_ERR_MLS;
    }

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

    /* Extract relay URLs from gde before freeing */
    char **gde_relays = NULL;
    size_t gde_relay_count = 0;
    if (gde && gde->relay_count > 0 && gde->relays) {
        gde_relay_count = gde->relay_count;
        gde_relays = calloc(gde_relay_count, sizeof(char *));
        if (gde_relays) {
            for (size_t i = 0; i < gde_relay_count; i++)
                gde_relays[i] = gde->relays[i] ? strdup(gde->relays[i]) : NULL;
        }
    }
    marmot_group_data_extension_free(gde);

    /* Store exporter secret. Mandatory for message encryption after accept. */
    MarmotError err = m->storage->save_exporter_secret(m->storage->ctx,
                                                       &group->mls_group_id,
                                                       mls_group.epoch,
                                                       mls_group.epoch_secrets.exporter_secret);
    if (err != MARMOT_OK)
        goto fail;

    /* Persist the full MLS group state for future operations
     * (add/remove members, send/receive messages). Mandatory. */
    uint8_t *state_data = NULL;
    size_t state_len = 0;
    if (mls_group_serialize(&mls_group, &state_data, &state_len) != 0) {
        err = MARMOT_ERR_SERIALIZATION;
        goto fail;
    }
    err = m->storage->mls_store(m->storage->ctx, "mls_group",
                                mls_group.group_id, mls_group.group_id_len,
                                state_data, state_len);
    free(state_data);
    if (err != MARMOT_OK)
        goto fail;

    /* Store the group metadata. */
    err = m->storage->save_group(m->storage->ctx, group);
    if (err != MARMOT_OK)
        goto fail;

    /* Store group relays when the welcome carries them. */
    if (gde_relays && gde_relay_count > 0) {
        if (!m->storage->replace_group_relays) {
            err = MARMOT_ERR_STORAGE;
            goto fail;
        }
        err = m->storage->replace_group_relays(m->storage->ctx,
                                               &group->mls_group_id,
                                               (const char **)gde_relays,
                                               gde_relay_count);
        if (err != MARMOT_OK)
            goto fail;
    }

    /* Return accepted group metadata to callers that need to emit/bind it. */
    if (out_group && m->storage->find_group_by_mls_id) {
        err = m->storage->find_group_by_mls_id(m->storage->ctx,
                                               &group->mls_group_id,
                                               out_group);
        if (err != MARMOT_OK)
            goto fail;
    }

    /* Update the welcome row and record it as processed. */
    MarmotWelcome updated_welcome = *welcome;
    updated_welcome.state = MARMOT_WELCOME_STATE_ACCEPTED;
    updated_welcome.mls_group_id = group->mls_group_id;
    memcpy(updated_welcome.nostr_group_id, group->nostr_group_id, 32);
    updated_welcome.group_name = group->name;
    updated_welcome.group_description = group->description;
    updated_welcome.group_image_hash = group->image_hash;
    updated_welcome.group_admin_pubkeys = group->admin_pubkeys;
    updated_welcome.group_admin_count = group->admin_count;
    updated_welcome.member_count = mls_group.tree.n_leaves;
    err = m->storage->save_welcome(m->storage->ctx, &updated_welcome);
    if (err != MARMOT_OK)
        goto fail;

    err = m->storage->save_processed_welcome(m->storage->ctx,
                                             welcome->wrapper_event_id,
                                             welcome->id,
                                             marmot_now(),
                                             MARMOT_WELCOME_STATE_ACCEPTED,
                                             NULL);
    if (err != MARMOT_OK)
        goto fail;

    /* Clean up stored raw welcome data. This delete is best-effort: the
     * accepted group is already durably stored, and stale raw welcome data is
     * only a cache/cleanup concern. */
    if (m->storage->mls_delete) {
        m->storage->mls_delete(m->storage->ctx, "welcome_data",
                                welcome->wrapper_event_id, 32);
    }

    mls_group_free(&mls_group);
    marmot_group_free(group);

    /* Free relay copies */
    if (gde_relays) {
        for (size_t i = 0; i < gde_relay_count; i++) free(gde_relays[i]);
        free(gde_relays);
    }

    return MARMOT_OK;

fail:
    record_welcome_failure(m, welcome->wrapper_event_id, marmot_error_string(err));
    if (out_group) {
        marmot_group_free(*out_group);
        *out_group = NULL;
    }
    mls_group_free(&mls_group);
    marmot_group_free(group);
    if (gde_relays) {
        for (size_t i = 0; i < gde_relay_count; i++) free(gde_relays[i]);
        free(gde_relays);
    }
    return err;
}

MarmotError
marmot_accept_welcome(Marmot *m, const MarmotWelcome *welcome)
{
    return accept_welcome_internal(m, welcome, NULL);
}

MarmotError
marmot_accept_welcome_by_wrapper_id(Marmot *m,
                                     const uint8_t wrapper_event_id[32],
                                     MarmotGroup **out_group)
{
    if (out_group)
        *out_group = NULL;
    if (!m || !wrapper_event_id)
        return MARMOT_ERR_INVALID_ARG;
    if (!m->storage || !m->storage->pending_welcomes)
        return MARMOT_ERR_STORAGE;

    MarmotWelcome **welcomes = NULL;
    size_t count = 0;
    MarmotError err = m->storage->pending_welcomes(m->storage->ctx, NULL,
                                                    &welcomes, &count);
    if (err != MARMOT_OK)
        return err;

    MarmotWelcome *matched = NULL;
    for (size_t i = 0; i < count; i++) {
        if (!matched && memcmp(welcomes[i]->wrapper_event_id, wrapper_event_id, 32) == 0) {
            matched = welcomes[i];
        } else {
            marmot_welcome_free(welcomes[i]);
        }
    }
    free(welcomes);

    if (!matched)
        return MARMOT_ERR_STORAGE_NOT_FOUND;

    err = accept_welcome_internal(m, matched, out_group);
    marmot_welcome_free(matched);
    return err;
}

MarmotError
marmot_get_group_relay_urls(Marmot *m,
                             const MarmotGroupId *mls_group_id,
                             MarmotGroupRelay **out_relays,
                             size_t *out_count)
{
    if (!m || !mls_group_id || !out_relays || !out_count)
        return MARMOT_ERR_INVALID_ARG;
    *out_relays = NULL;
    *out_count = 0;
    if (!m->storage || !m->storage->group_relays)
        return MARMOT_ERR_STORAGE;
    return m->storage->group_relays(m->storage->ctx, mls_group_id,
                                    out_relays, out_count);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_decline_welcome
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_decline_welcome(Marmot *m, const MarmotWelcome *welcome)
{
    if (!m || !welcome)
        return MARMOT_ERR_INVALID_ARG;
    if (!m->storage || !m->storage->save_welcome ||
        !m->storage->save_processed_welcome)
        return MARMOT_ERR_STORAGE;

    MarmotWelcome declined = *welcome;
    declined.state = MARMOT_WELCOME_STATE_DECLINED;
    MarmotError err = m->storage->save_welcome(m->storage->ctx, &declined);
    if (err != MARMOT_OK)
        return err;

    err = m->storage->save_processed_welcome(m->storage->ctx,
                                             welcome->wrapper_event_id,
                                             welcome->id,
                                             marmot_now(),
                                             MARMOT_WELCOME_STATE_DECLINED,
                                             NULL);
    if (err != MARMOT_OK)
        return err;

    /* Clean up stored welcome data */
    if (m->storage->mls_delete) {
        m->storage->mls_delete(m->storage->ctx, "welcome_data",
                                welcome->wrapper_event_id, 32);
    }

    /* Note: We do NOT delete the KeyPackage from relays here per MIP-02:
     * "If Welcome processing fails, do NOT delete the KeyPackage from relays" */

    return MARMOT_OK;
}
