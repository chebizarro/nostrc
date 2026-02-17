/*
 * libmarmot - MIP-01: Group Construction
 *
 * Creates and manages MLS groups with the Marmot Group Data Extension.
 *
 * Group creation flow:
 *   1. Parse each invited member's kind:443 KeyPackage event
 *   2. Create single-member MLS group with GroupData extension
 *   3. For each member: mls_group_add_member → Commit + Welcome
 *   4. Build kind:445 evolution event (the commit)
 *   5. Build kind:444 welcome rumors (unsigned, for gift-wrapping)
 *   6. Store group in storage backend
 *
 * SPDX-License-Identifier: MIT
 */

#include "marmot-internal.h"
#include "mls/mls_group.h"
#include "mls/mls_key_package.h"
#include "mls/mls_welcome.h"
#include "mls/mls-internal.h"
#include <nostr-event.h>
#include <nostr-tag.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Forward declaration from credentials.c */
extern MarmotError marmot_parse_key_package_event(const char *event_json,
                                                    MlsKeyPackage *kp_out,
                                                    uint8_t nostr_pubkey_out[32]);

/* Internal base64 encode (same as in credentials.c) */
static char *
base64_encode(const uint8_t *data, size_t len)
{
    size_t b64_maxlen = sodium_base64_ENCODED_LEN(len, sodium_base64_VARIANT_ORIGINAL);
    char *out = malloc(b64_maxlen);
    if (!out) return NULL;
    sodium_bin2base64(out, b64_maxlen, data, len, sodium_base64_VARIANT_ORIGINAL);
    return out;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: Build GroupData extension from MarmotGroupConfig
 * ──────────────────────────────────────────────────────────────────────── */

static int
build_group_data_extension(const MarmotGroupConfig *config,
                            const uint8_t nostr_group_id[32],
                            uint8_t **ext_data, size_t *ext_len)
{
    MarmotGroupDataExtension *gde = marmot_group_data_extension_new();
    if (!gde) return -1;

    gde->version = MARMOT_EXTENSION_VERSION;
    memcpy(gde->nostr_group_id, nostr_group_id, 32);

    if (config->name) {
        gde->name = strdup(config->name);
        if (!gde->name) goto fail;
    }
    if (config->description) {
        gde->description = strdup(config->description);
        if (!gde->description) goto fail;
    }

    /* Admin pubkeys */
    if (config->admin_count > 0 && config->admin_pubkeys) {
        gde->admin_count = config->admin_count;
        gde->admins = malloc(config->admin_count * 32);
        if (!gde->admins) goto fail;
        memcpy(gde->admins, config->admin_pubkeys, config->admin_count * 32);
    }

    /* Relays */
    if (config->relay_count > 0 && config->relay_urls) {
        gde->relay_count = config->relay_count;
        gde->relays = calloc(config->relay_count, sizeof(char *));
        if (!gde->relays) goto fail;
        for (size_t i = 0; i < config->relay_count; i++) {
            gde->relays[i] = strdup(config->relay_urls[i]);
            if (!gde->relays[i]) goto fail;
        }
    }

    /* Now serialize the extension inside a proper MLS Extensions structure.
     * Extensions is: Extension extension_type(uint16) + extension_data(opaque<V>)
     * We wrap the GroupData in an extension with type 0xF2EE. */
    uint8_t *gde_bytes = NULL;
    size_t gde_len = 0;
    int rc = marmot_group_data_extension_serialize(gde, &gde_bytes, &gde_len);
    marmot_group_data_extension_free(gde);
    if (rc != 0) return rc;

    /* Wrap as MLS Extension: type(2) + data<2>(length-prefixed) */
    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, gde_len + 8) != 0) {
        free(gde_bytes);
        return -1;
    }
    /* Extension type: 0xF2EE */
    if (mls_tls_write_u16(&buf, MARMOT_EXTENSION_TYPE) != 0 ||
        mls_tls_write_opaque16(&buf, gde_bytes, gde_len) != 0) {
        free(gde_bytes);
        mls_tls_buf_free(&buf);
        return -1;
    }
    free(gde_bytes);

    *ext_data = buf.data;
    *ext_len = buf.len;
    buf.data = NULL;
    return 0;

fail:
    marmot_group_data_extension_free(gde);
    return MARMOT_ERR_MEMORY;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: Build kind:445 evolution event (commit)
 * ──────────────────────────────────────────────────────────────────────── */

static char *
build_evolution_event(const uint8_t *commit_data, size_t commit_len,
                       const uint8_t nostr_group_id[32])
{
    /* The evolution event is a kind:445 with:
     * - content: base64 of the serialized commit (MLSMessage)
     * - pubkey: ephemeral (generated per-event)
     * - h tag: hex of the nostr_group_id
     * - encoding tag: "base64"
     *
     * The event is unsigned — caller signs with ephemeral key.
     */
    char *b64_content = base64_encode(commit_data, commit_len);
    if (!b64_content) return NULL;

    /* The evolution event uses an ephemeral pubkey.
     * The actual signing will be done by the caller. */

    NostrEvent *event = nostr_event_new();
    if (!event) { free(b64_content); return NULL; }

    nostr_event_set_kind(event, MARMOT_KIND_GROUP_MESSAGE);
    nostr_event_set_content(event, b64_content);
    nostr_event_set_created_at(event, (int64_t)time(NULL));
    free(b64_content);

    NostrTags *tags = nostr_tags_new(0);
    if (!tags) { nostr_event_free(event); return NULL; }

    /* h tag: nostr_group_id hex */
    char *gid_hex = marmot_hex_encode(nostr_group_id, 32);
    NostrTag *tag = nostr_tag_new("h", gid_hex, NULL);
    free(gid_hex);
    if (!tag) { nostr_tags_free(tags); nostr_event_free(event); return NULL; }
    nostr_tags_append(tags, tag);

    /* encoding tag */
    tag = nostr_tag_new("encoding", "base64", NULL);
    if (!tag) { nostr_tags_free(tags); nostr_event_free(event); return NULL; }
    nostr_tags_append(tags, tag);

    nostr_event_set_tags(event, tags);

    char *json = nostr_event_serialize_compact(event);
    nostr_event_free(event);
    return json;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: Build kind:444 welcome rumor (unsigned)
 * ──────────────────────────────────────────────────────────────────────── */

static char *
build_welcome_rumor(const uint8_t *welcome_data, size_t welcome_len,
                     const char *kp_event_id,
                     const char **relay_urls, size_t relay_count)
{
    /* Welcome rumor is a kind:444 unsigned event with:
     * - content: base64 of the serialized MLS Welcome
     * - e tag: referencing the KeyPackage event used
     * - relays tag: where to find group messages
     * - encoding tag: "base64"
     *
     * This event is unsigned per MIP-02 (prevents accidental public publishing).
     */
    char *b64_content = base64_encode(welcome_data, welcome_len);
    if (!b64_content) return NULL;

    NostrEvent *event = nostr_event_new();
    if (!event) { free(b64_content); return NULL; }

    nostr_event_set_kind(event, MARMOT_KIND_WELCOME);
    nostr_event_set_content(event, b64_content);
    nostr_event_set_created_at(event, (int64_t)time(NULL));
    free(b64_content);

    NostrTags *tags = nostr_tags_new(0);
    if (!tags) { nostr_event_free(event); return NULL; }

    /* e tag: KeyPackage event ID */
    if (kp_event_id) {
        NostrTag *tag = nostr_tag_new("e", kp_event_id, NULL);
        if (!tag) { nostr_tags_free(tags); nostr_event_free(event); return NULL; }
        nostr_tags_append(tags, tag);
    }

    /* encoding tag */
    NostrTag *tag = nostr_tag_new("encoding", "base64", NULL);
    if (!tag) { nostr_tags_free(tags); nostr_event_free(event); return NULL; }
    nostr_tags_append(tags, tag);

    /* relays tag */
    if (relay_count > 0 && relay_urls) {
        NostrTag *relay_tag = nostr_tag_new("relays", relay_urls[0], NULL);
        if (!relay_tag) { nostr_tags_free(tags); nostr_event_free(event); return NULL; }
        for (size_t i = 1; i < relay_count; i++) {
            nostr_tag_append(relay_tag, relay_urls[i]);
        }
        nostr_tags_append(tags, relay_tag);
    }

    nostr_event_set_tags(event, tags);

    char *json = nostr_event_serialize_compact(event);
    nostr_event_free(event);
    return json;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: Populate MarmotGroup from MlsGroup + GroupData
 * ──────────────────────────────────────────────────────────────────────── */

static MarmotGroup *
mls_group_to_marmot_group(const MlsGroup *mls,
                           const MarmotGroupDataExtension *gde)
{
    MarmotGroup *group = marmot_group_new();
    if (!group) return NULL;

    /* MLS group ID */
    group->mls_group_id = marmot_group_id_new(mls->group_id, mls->group_id_len);
    group->epoch = mls->epoch;
    group->state = MARMOT_GROUP_STATE_ACTIVE;

    /* From GroupData extension */
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

    return group;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_create_group
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_create_group(Marmot *m,
                     const uint8_t creator_pubkey[32],
                     const char **key_package_event_jsons, size_t kp_count,
                     const MarmotGroupConfig *config,
                     MarmotCreateGroupResult *result)
{
    if (!m || !creator_pubkey || !config || !result)
        return MARMOT_ERR_INVALID_ARG;
    if (kp_count > 0 && !key_package_event_jsons)
        return MARMOT_ERR_INVALID_ARG;

    memset(result, 0, sizeof(*result));

    /* Ensure identity */
    if (marmot_ensure_identity(m) != 0)
        return MARMOT_ERR_CRYPTO;

    /* Generate random 32-byte MLS group ID */
    uint8_t mls_group_id[32];
    randombytes_buf(mls_group_id, 32);

    /* Generate random 32-byte Nostr group ID */
    uint8_t nostr_group_id[32];
    randombytes_buf(nostr_group_id, 32);

    /* Build GroupContext extensions with GroupData */
    uint8_t *ext_data = NULL;
    size_t ext_len = 0;
    MarmotError err = build_group_data_extension(config, nostr_group_id,
                                                  &ext_data, &ext_len);
    if (err != MARMOT_OK) return err;

    /* Create the single-member MLS group */
    MlsGroup mls_group;
    memset(&mls_group, 0, sizeof(mls_group));

    int rc = mls_group_create(&mls_group,
                               mls_group_id, 32,
                               creator_pubkey, 32,
                               m->ed25519_sk,
                               ext_data, ext_len);
    free(ext_data);
    if (rc != 0) return MARMOT_ERR_MLS;

    /* Parse each KeyPackage event and add members */
    result->welcome_count = kp_count;
    if (kp_count > 0) {
        result->welcome_rumor_jsons = calloc(kp_count, sizeof(char *));
        if (!result->welcome_rumor_jsons) {
            mls_group_free(&mls_group);
            return MARMOT_ERR_MEMORY;
        }
    }

    MlsAddResult last_add_result;
    memset(&last_add_result, 0, sizeof(last_add_result));

    for (size_t i = 0; i < kp_count; i++) {
        /* Parse KeyPackage event */
        MlsKeyPackage kp;
        uint8_t member_pubkey[32];
        rc = marmot_parse_key_package_event(key_package_event_jsons[i],
                                             &kp, member_pubkey);
        if (rc != 0) {
            /* Clean up on failure */
            mls_add_result_clear(&last_add_result);
            for (size_t j = 0; j < i; j++)
                free(result->welcome_rumor_jsons[j]);
            free(result->welcome_rumor_jsons);
            result->welcome_rumor_jsons = NULL;
            mls_group_free(&mls_group);
            return MARMOT_ERR_VALIDATION;
        }

        /* Add member to MLS group */
        MlsAddResult add_result;
        memset(&add_result, 0, sizeof(add_result));
        rc = mls_group_add_member(&mls_group, &kp, &add_result);
        mls_key_package_clear(&kp);
        if (rc != 0) {
            mls_add_result_clear(&last_add_result);
            for (size_t j = 0; j < i; j++)
                free(result->welcome_rumor_jsons[j]);
            free(result->welcome_rumor_jsons);
            result->welcome_rumor_jsons = NULL;
            mls_group_free(&mls_group);
            return MARMOT_ERR_MLS;
        }

        /* Build welcome rumor for this member */
        /* Extract the KeyPackage event ID for the e-tag */
        /* For now, use NULL since we don't parse the event ID from JSON */
        result->welcome_rumor_jsons[i] = build_welcome_rumor(
            add_result.welcome_data, add_result.welcome_len,
            NULL, /* kp_event_id — would need to extract from event JSON */
            config->relay_urls, config->relay_count);

        /* Keep the last commit (for groups with >1 new member, we only
         * publish the final commit) */
        mls_add_result_clear(&last_add_result);
        last_add_result = add_result;
        /* Don't clear add_result — ownership transferred to last_add_result */
    }

    /* Build evolution event from the last commit */
    if (kp_count > 0 && last_add_result.commit_data) {
        result->evolution_event_json = build_evolution_event(
            last_add_result.commit_data, last_add_result.commit_len,
            nostr_group_id);
    }
    mls_add_result_clear(&last_add_result);

    /* Build the GroupData extension struct for populating the MarmotGroup */
    /* Note: Shallow copies are safe here because mls_group_to_marmot_group()
     * duplicates all string pointers with strdup() */
    MarmotGroupDataExtension gde_local;
    memset(&gde_local, 0, sizeof(gde_local));
    gde_local.version = MARMOT_EXTENSION_VERSION;
    memcpy(gde_local.nostr_group_id, nostr_group_id, 32);
    gde_local.name = config->name;
    gde_local.description = config->description;
    gde_local.admins = config->admin_pubkeys;
    gde_local.admin_count = config->admin_count;
    gde_local.relays = config->relay_urls;
    gde_local.relay_count = config->relay_count;

    /* Convert to MarmotGroup */
    result->group = mls_group_to_marmot_group(&mls_group, &gde_local);

    /* Store the MLS group state in storage */
    if (result->group && m->storage && m->storage->save_group) {
        m->storage->save_group(m->storage->ctx, result->group);
    }

    /* Store the MLS group state binary for message processing */
    if (m->storage && m->storage->mls_store) {
        /* For now, store a marker. Full serialization would go here. */
        uint8_t marker = 1;
        m->storage->mls_store(m->storage->ctx, "mls_group",
                               mls_group_id, 32,
                               &marker, 1);
    }

    /* Store exporter secret for NIP-44 message encryption */
    if (m->storage && m->storage->save_exporter_secret) {
        MarmotGroupId gid = marmot_group_id_new(mls_group_id, 32);
        m->storage->save_exporter_secret(m->storage->ctx, &gid,
                                          mls_group.epoch,
                                          mls_group.epoch_secrets.exporter_secret);
        marmot_group_id_free(&gid);
    }

    mls_group_free(&mls_group);
    return MARMOT_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_merge_pending_commit
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_merge_pending_commit(Marmot *m, const MarmotGroupId *mls_group_id)
{
    if (!m || !mls_group_id)
        return MARMOT_ERR_INVALID_ARG;
    if (!m->storage || !m->storage->find_group_by_mls_id || !m->storage->save_group)
        return MARMOT_ERR_STORAGE;

    /*
     * In the current architecture, mls_group_add_member() already advances
     * the group state in-place. So "merging a pending commit" is the
     * operation of confirming that the commit was accepted by relays.
     *
     * In the MDK, this is where you'd call merge_pending_commit() on the
     * OpenMLS group. For our pure-C implementation, the group state is
     * already advanced after create_group/add_members, so this is
     * primarily a storage-layer operation: update the group's
     * last_message_processed_at timestamp.
     */
    MarmotGroup *group = NULL;
    MarmotError err = m->storage->find_group_by_mls_id(m->storage->ctx,
                                                         mls_group_id, &group);
    if (err != MARMOT_OK || !group) return MARMOT_ERR_GROUP_NOT_FOUND;

    group->last_message_processed_at = marmot_now();
    err = m->storage->save_group(m->storage->ctx, group);
    marmot_group_free(group);
    return err;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_add_members
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_add_members(Marmot *m,
                    const MarmotGroupId *mls_group_id,
                    const char **key_package_event_jsons, size_t kp_count,
                    char ***out_welcome_jsons, size_t *out_welcome_count,
                    char **out_commit_json)
{
    if (!m || !mls_group_id || !out_welcome_jsons || !out_welcome_count || !out_commit_json)
        return MARMOT_ERR_INVALID_ARG;
    if (kp_count == 0 || !key_package_event_jsons)
        return MARMOT_ERR_INVALID_ARG;

    *out_welcome_jsons = NULL;
    *out_welcome_count = 0;
    *out_commit_json = NULL;

    /* TODO: Restore MLS group state from storage, add members,
     * produce commits and welcomes. This requires full MLS group
     * serialization/deserialization. */
    return MARMOT_ERR_NOT_IMPLEMENTED;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_remove_members
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_remove_members(Marmot *m,
                       const MarmotGroupId *mls_group_id,
                       const uint8_t (*member_pubkeys)[32], size_t count,
                       char **out_commit_json)
{
    if (!m || !mls_group_id || !member_pubkeys || count == 0 || !out_commit_json)
        return MARMOT_ERR_INVALID_ARG;

    *out_commit_json = NULL;

    /* TODO: Restore MLS group state, find leaf indices for pubkeys,
     * create remove commits. */
    return MARMOT_ERR_NOT_IMPLEMENTED;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_leave_group
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_leave_group(Marmot *m, const MarmotGroupId *mls_group_id)
{
    if (!m || !mls_group_id)
        return MARMOT_ERR_INVALID_ARG;
    if (!m->storage || !m->storage->find_group_by_mls_id || !m->storage->save_group)
        return MARMOT_ERR_STORAGE;

    MarmotGroup *group = NULL;
    MarmotError err = m->storage->find_group_by_mls_id(m->storage->ctx,
                                                         mls_group_id, &group);
    if (err != MARMOT_OK || !group) return MARMOT_ERR_GROUP_NOT_FOUND;

    group->state = MARMOT_GROUP_STATE_INACTIVE;
    err = m->storage->save_group(m->storage->ctx, group);
    marmot_group_free(group);
    return err;
}
