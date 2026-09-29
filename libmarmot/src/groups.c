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
#include "commits.h"
#include "kp_profile.h"
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
#include <time.h>

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
 * Internal: Load / save MLS group state from storage
 * ──────────────────────────────────────────────────────────────────────── */

static int
load_mls_group(Marmot *m, const MarmotGroupId *gid, MlsGroup *out)
{
    if (!m->storage || !m->storage->mls_load) return -1;

    uint8_t *state_data = NULL;
    size_t state_len = 0;
    MarmotError err = m->storage->mls_load(m->storage->ctx, "mls_group",
                                            gid->data, gid->len,
                                            &state_data, &state_len);
    if (err != MARMOT_OK || !state_data) return -1;

    int rc = mls_group_deserialize(state_data, state_len, out);
    free(state_data);
    return rc;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: Admin policy check
 * ──────────────────────────────────────────────────────────────────────── */

static bool
is_admin(const MarmotGroup *group, const uint8_t pubkey[32])
{
    if (!group || !pubkey) return false;
    /* If no admins defined, anyone can modify (backwards compatibility) */
    if (group->admin_count == 0 || !group->admin_pubkeys) return true;

    for (size_t i = 0; i < group->admin_count; i++) {
        if (memcmp(group->admin_pubkeys[i], pubkey, 32) == 0)
            return true;
    }
    return false;
}

/**
 * Get our Nostr pubkey (credential identity) from the MLS group's own leaf.
 * Returns 0 on success with the pubkey written to out_pk.
 */
static int
get_own_credential_identity(const MlsGroup *mls, uint8_t out_pk[32])
{
    uint32_t node_idx = mls_tree_leaf_to_node(mls->own_leaf_index);
    if (node_idx >= mls->tree.n_nodes) return -1;

    const MlsNode *node = &mls->tree.nodes[node_idx];
    if (node->type != MLS_NODE_LEAF) return -1;
    if (node->leaf.credential_identity_len != 32) return -1;

    memcpy(out_pk, node->leaf.credential_identity, 32);
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: Find a member's leaf index by their credential identity (pubkey)
 * ──────────────────────────────────────────────────────────────────────── */

static int
find_leaf_by_pubkey(const MlsGroup *mls, const uint8_t pubkey[32],
                    uint32_t *out_leaf_index)
{
    for (uint32_t i = 0; i < mls->tree.n_leaves; i++) {
        uint32_t node_idx = mls_tree_leaf_to_node(i);
        const MlsNode *node = &mls->tree.nodes[node_idx];
        if (node->type != MLS_NODE_LEAF) continue;
        if (node->leaf.credential_identity_len == 32 &&
            memcmp(node->leaf.credential_identity, pubkey, 32) == 0) {
            *out_leaf_index = i;
            return 0;
        }
    }
    return -1; /* not found */
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
 * Internal: Build kind:444 welcome rumor (unsigned)
 * ──────────────────────────────────────────────────────────────────────── */

static void
free_stack_event_fields(NostrEvent *ev)
{
    free(ev->id);
    free(ev->pubkey);
    free(ev->content);
    free(ev->sig);
    nostr_tags_free(ev->tags);
    memset(ev, 0, sizeof(*ev));
}

static char *
extract_event_id_hex(const char *event_json)
{
    NostrEvent ev;
    memset(&ev, 0, sizeof(ev));
    if (!event_json || !nostr_event_deserialize_compact(&ev, event_json, NULL))
        return NULL;
    char *id = ev.id ? strdup(ev.id) : NULL;
    free_stack_event_fields(&ev);
    return id;
}

static char *
build_welcome_rumor(const uint8_t *welcome_data, size_t welcome_len,
                     const char *kp_event_id,
                     const uint8_t nostr_group_id[32],
                     const char *group_name,
                     const char *group_description,
                     const uint8_t (*admin_pubkeys)[32], size_t admin_count,
                     size_t member_count,
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

    /* preview h tag: Nostr group id */
    if (nostr_group_id) {
        char *ngid_hex = marmot_hex_encode(nostr_group_id, 32);
        if (ngid_hex) {
            NostrTag *h_tag = nostr_tag_new("h", ngid_hex, NULL);
            free(ngid_hex);
            if (!h_tag) { nostr_tags_free(tags); nostr_event_free(event); return NULL; }
            nostr_tags_append(tags, h_tag);
        }
    }

    if (group_name) {
        NostrTag *name_tag = nostr_tag_new("name", group_name, NULL);
        if (!name_tag) { nostr_tags_free(tags); nostr_event_free(event); return NULL; }
        nostr_tags_append(tags, name_tag);
    }
    if (group_description) {
        NostrTag *desc_tag = nostr_tag_new("description", group_description, NULL);
        if (!desc_tag) { nostr_tags_free(tags); nostr_event_free(event); return NULL; }
        nostr_tags_append(tags, desc_tag);
    }
    for (size_t i = 0; i < admin_count && admin_pubkeys; i++) {
        char *admin_hex = marmot_hex_encode(admin_pubkeys[i], 32);
        if (admin_hex) {
            NostrTag *admin_tag = nostr_tag_new("admin", admin_hex, NULL);
            free(admin_hex);
            if (!admin_tag) { nostr_tags_free(tags); nostr_event_free(event); return NULL; }
            nostr_tags_append(tags, admin_tag);
        }
    }
    char member_count_buf[32];
    snprintf(member_count_buf, sizeof(member_count_buf), "%zu", member_count);
    NostrTag *members_tag = nostr_tag_new("member_count", member_count_buf, NULL);
    if (!members_tag) { nostr_tags_free(tags); nostr_event_free(event); return NULL; }
    nostr_tags_append(tags, members_tag);

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

    if (!m->storage || !m->storage->save_group || !m->storage->delete_group ||
        !m->storage->mls_store || !m->storage->mls_delete ||
        !m->storage->save_exporter_secret || !m->storage->delete_exporter_secret)
        return MARMOT_ERR_STORAGE;

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
    /* The published Commit is sealed with its source epoch's exporter
     * secret (MIP-03), like any kind:445 event of that epoch. */
    uint8_t last_source_exporter[32];
    memset(last_source_exporter, 0, sizeof(last_source_exporter));

    for (size_t i = 0; i < kp_count; i++) {
        /* Parse KeyPackage event */
        MlsKeyPackage kp;
        uint8_t member_pubkey[32];
        rc = marmot_parse_key_package_event(key_package_event_jsons[i],
                                             &kp, member_pubkey);
        if (rc != 0) {
            /* Clean up on failure */
            sodium_memzero(last_source_exporter, sizeof(last_source_exporter));
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
        uint8_t source_exporter[32];
        memcpy(source_exporter, mls_group.epoch_secrets.exporter_secret, 32);
        rc = mls_group_add_member(&mls_group, &kp, &add_result);
        mls_key_package_clear(&kp);
        if (rc != 0) {
            sodium_memzero(source_exporter, sizeof(source_exporter));
            sodium_memzero(last_source_exporter, sizeof(last_source_exporter));
            mls_add_result_clear(&last_add_result);
            for (size_t j = 0; j < i; j++)
                free(result->welcome_rumor_jsons[j]);
            free(result->welcome_rumor_jsons);
            result->welcome_rumor_jsons = NULL;
            mls_group_free(&mls_group);
            return MARMOT_ERR_MLS;
        }

        /* Build welcome rumor for this member, including the KeyPackage event
         * id and cleartext preview tags used by pending-welcome UIs. */
        char *kp_event_id = extract_event_id_hex(key_package_event_jsons[i]);
        result->welcome_rumor_jsons[i] = build_welcome_rumor(
            add_result.welcome_data, add_result.welcome_len,
            kp_event_id,
            nostr_group_id,
            config->name,
            config->description,
            config->admin_pubkeys, config->admin_count,
            mls_group.tree.n_leaves,
            (const char **)config->relay_urls, config->relay_count);
        free(kp_event_id);

        /* Keep the last commit (for groups with >1 new member, we only
         * publish the final commit) */
        mls_add_result_clear(&last_add_result);
        last_add_result = add_result;
        memcpy(last_source_exporter, source_exporter, 32);
        sodium_memzero(source_exporter, sizeof(source_exporter));
        /* Don't clear add_result — ownership transferred to last_add_result */
    }

    /* Build evolution event from the last commit */
    if (kp_count > 0 && last_add_result.commit_data) {
        result->evolution_event_json = marmot_commit_build_event(
            last_add_result.commit_data, last_add_result.commit_len,
            last_source_exporter, nostr_group_id);
    }
    sodium_memzero(last_source_exporter, sizeof(last_source_exporter));
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
    if (!result->group) {
        mls_group_free(&mls_group);
        marmot_create_group_result_free(result);
        return MARMOT_ERR_MEMORY;
    }

    /* Store the full MLS group state for future operations. This write is
     * mandatory: later add/remove/message operations cannot proceed without
     * the private MLS state. */
    uint8_t *state_data = NULL;
    size_t state_len = 0;
    if (mls_group_serialize(&mls_group, &state_data, &state_len) != 0) {
        mls_group_free(&mls_group);
        marmot_create_group_result_free(result);
        return MARMOT_ERR_SERIALIZATION;
    }
    err = m->storage->mls_store(m->storage->ctx, "mls_group",
                                mls_group_id, 32,
                                state_data, state_len);
    free(state_data);
    if (err != MARMOT_OK) {
        mls_group_free(&mls_group);
        marmot_create_group_result_free(result);
        return err;
    }

    /* Store exporter secret for NIP-44 message encryption. Mandatory: a group
     * without its exporter secret cannot send or receive messages. */
    MarmotGroupId gid = marmot_group_id_new(mls_group_id, 32);
    err = m->storage->save_exporter_secret(m->storage->ctx, &gid,
                                           mls_group.epoch,
                                           mls_group.epoch_secrets.exporter_secret);
    if (err != MARMOT_OK) {
        m->storage->mls_delete(m->storage->ctx, "mls_group", mls_group_id, 32);
        marmot_group_id_free(&gid);
        mls_group_free(&mls_group);
        marmot_create_group_result_free(result);
        return err;
    }

    /* Store group metadata last so callers never observe a group whose private
     * MLS state was not persisted. */
    err = m->storage->save_group(m->storage->ctx, result->group);
    if (err != MARMOT_OK) {
        m->storage->delete_exporter_secret(m->storage->ctx, &gid, mls_group.epoch);
        m->storage->mls_delete(m->storage->ctx, "mls_group", mls_group_id, 32);
        marmot_group_id_free(&gid);
        mls_group_free(&mls_group);
        marmot_create_group_result_free(result);
        return err;
    }

    if (config->relay_count > 0 && config->relay_urls) {
        if (!m->storage->replace_group_relays) {
            mls_group_free(&mls_group);
            marmot_create_group_result_free(result);
            return MARMOT_ERR_STORAGE;
        }
        err = m->storage->replace_group_relays(m->storage->ctx,
                                               &result->group->mls_group_id,
                                               (const char **)config->relay_urls,
                                               config->relay_count);
        if (err != MARMOT_OK) {
            m->storage->delete_group(m->storage->ctx, &result->group->mls_group_id);
            m->storage->delete_exporter_secret(m->storage->ctx, &gid, mls_group.epoch);
            m->storage->mls_delete(m->storage->ctx, "mls_group", mls_group_id, 32);
            marmot_group_id_free(&gid);
            mls_group_free(&mls_group);
            marmot_create_group_result_free(result);
            return err;
        }
    }

    marmot_group_id_free(&gid);
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
 * Internal: publish and persist a local Commit (nostrc-9ata)
 * ──────────────────────────────────────────────────────────────────────── */

static int
clone_mls_group(const MlsGroup *src, MlsGroup *dst)
{
    uint8_t *blob = NULL;
    size_t len = 0;
    memset(dst, 0, sizeof(*dst));
    int rc = (mls_group_serialize(src, &blob, &len) == 0 &&
              mls_group_deserialize(blob, len, dst) == 0) ? 0 : -1;
    if (blob) {
        sodium_memzero(blob, len);
        free(blob);
    }
    return rc;
}

static bool
storage_can_commit(const MarmotStorage *s)
{
    return s && s->find_group_by_mls_id && s->save_group && s->mls_store &&
           s->mls_load && s->mls_delete && s->save_exporter_secret &&
           s->get_exporter_secret && s->delete_exporter_secret;
}

/*
 * Tail shared by the Commit producers.  `pre` is the state the published
 * Commit was made from and `post` the state it produced.  The Commit must
 * pass the same Marmot policy every receiver applies (so a member never
 * publishes what the group rejects); the kind:445 event is sealed with the
 * source epoch's exporter secret; then post, its exporter secret, the
 * retained parent and `group` are persisted (all or nothing).  Nothing is
 * stored on failure.
 */
static MarmotError
finish_local_commit(Marmot *m, MarmotGroup *group,
                    const MlsGroup *pre, const MlsGroup *post,
                    const uint8_t *commit, size_t commit_len,
                    char **out_commit_json)
{
    if (!commit || commit_len == 0) return MARMOT_ERR_MLS;
    MarmotCommitKey key;
    MarmotGroupDataExtension *gde = NULL;
    MarmotError err = marmot_commit_authorize(pre, post, pre->own_leaf_index,
                                              &key, &gde);
    if (err != MARMOT_OK) return err;
    if (mls_crypto_hash(key.digest, commit, commit_len) != 0) {
        marmot_group_data_extension_free(gde);
        return MARMOT_ERR_CRYPTO;
    }
    char *json = marmot_commit_build_event(commit, commit_len,
                                           pre->epoch_secrets.exporter_secret,
                                           group->nostr_group_id);
    if (!json) {
        marmot_group_data_extension_free(gde);
        return MARMOT_ERR_EVENT_BUILD;
    }
    err = marmot_commit_persist(m, pre, post, &key, gde, group);
    marmot_group_data_extension_free(gde);
    if (err != MARMOT_OK) {
        free(json);
        return err;
    }
    *out_commit_json = json;
    return MARMOT_OK;
}

/* Load an active group we may commit to as an admin.  On success the caller
 * owns *group_out and *mls_out. */
static MarmotError
load_group_for_commit(Marmot *m, const MarmotGroupId *mls_group_id,
                      MarmotGroup **group_out, MlsGroup *mls_out)
{
    *group_out = NULL;
    memset(mls_out, 0, sizeof(*mls_out));
    if (marmot_ensure_identity(m) != 0)
        return MARMOT_ERR_CRYPTO;
    if (!storage_can_commit(m->storage))
        return MARMOT_ERR_STORAGE;

    MarmotGroup *group = NULL;
    MarmotError err = m->storage->find_group_by_mls_id(m->storage->ctx,
                                                        mls_group_id, &group);
    if (err != MARMOT_OK || !group)
        return MARMOT_ERR_GROUP_NOT_FOUND;
    if (group->state != MARMOT_GROUP_STATE_ACTIVE) {
        marmot_group_free(group);
        return MARMOT_ERR_USE_AFTER_EVICTION;
    }
    /* The group's MLS state is required: without it nothing can be committed. */
    if (load_mls_group(m, mls_group_id, mls_out) != 0) {
        marmot_group_free(group);
        return MARMOT_ERR_MLS;
    }
    /* Admin check using our Nostr pubkey from our leaf in the MLS tree */
    uint8_t our_nostr_pk[32];
    if (get_own_credential_identity(mls_out, our_nostr_pk) != 0 ||
        !is_admin(group, our_nostr_pk)) {
        mls_group_free(mls_out);
        marmot_group_free(group);
        return MARMOT_ERR_ADMIN_ONLY;
    }
    *group_out = group;
    return MARMOT_OK;
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

    MarmotGroup *group = NULL;
    MlsGroup mls;
    MarmotError err = load_group_for_commit(m, mls_group_id, &group, &mls);
    if (err != MARMOT_OK) return err;

    char **welcomes = calloc(kp_count, sizeof(char *));
    if (!welcomes) {
        mls_group_free(&mls);
        marmot_group_free(group);
        return MARMOT_ERR_MEMORY;
    }

    /* Each KeyPackage is one Commit; only the last one is published
     * (nostrc-9ata follow-up), so `pre` is the state before that Commit. */
    MlsGroup pre;
    memset(&pre, 0, sizeof(pre));
    MlsAddResult last_add_result;
    memset(&last_add_result, 0, sizeof(last_add_result));
    char *commit_json = NULL;

    for (size_t i = 0; i < kp_count; i++) {
        MlsKeyPackage kp;
        uint8_t member_pubkey[32];
        if (marmot_parse_key_package_event(key_package_event_jsons[i],
                                           &kp, member_pubkey) != 0) {
            err = MARMOT_ERR_VALIDATION;
            goto fail;
        }
        mls_group_free(&pre);
        if (clone_mls_group(&mls, &pre) != 0) {
            mls_key_package_clear(&kp);
            err = MARMOT_ERR_MLS;
            goto fail;
        }

        MlsAddResult add_result;
        memset(&add_result, 0, sizeof(add_result));
        int rc = mls_group_add_member(&mls, &kp, &add_result);
        mls_key_package_clear(&kp);
        if (rc != 0) {
            err = MARMOT_ERR_MLS;
            goto fail;
        }

        /* Build welcome rumor */
        char *kp_event_id = extract_event_id_hex(key_package_event_jsons[i]);
        welcomes[i] = build_welcome_rumor(
            add_result.welcome_data, add_result.welcome_len,
            kp_event_id,
            group->nostr_group_id,
            group->name,
            group->description,
            (const uint8_t (*)[32])group->admin_pubkeys, group->admin_count,
            mls.tree.n_leaves,
            NULL, 0);
        free(kp_event_id);

        mls_add_result_clear(&last_add_result);
        last_add_result = add_result;
    }

    err = finish_local_commit(m, group, &pre, &mls, last_add_result.commit_data,
                              last_add_result.commit_len, &commit_json);
    if (err != MARMOT_OK) goto fail;

    mls_add_result_clear(&last_add_result);
    mls_group_free(&pre);
    mls_group_free(&mls);
    marmot_group_free(group);
    *out_welcome_jsons = welcomes;
    *out_welcome_count = kp_count;
    *out_commit_json = commit_json;
    return MARMOT_OK;

fail:
    mls_add_result_clear(&last_add_result);
    for (size_t j = 0; j < kp_count; j++) free(welcomes[j]);
    free(welcomes);
    mls_group_free(&pre);
    mls_group_free(&mls);
    marmot_group_free(group);
    return err;
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

    MarmotGroup *group = NULL;
    MlsGroup mls;
    MarmotError err = load_group_for_commit(m, mls_group_id, &group, &mls);
    if (err != MARMOT_OK) return err;

    MlsGroup pre;
    memset(&pre, 0, sizeof(pre));
    MlsCommitResult last_result;
    memset(&last_result, 0, sizeof(last_result));

    for (size_t i = 0; i < count; i++) {
        uint32_t leaf_idx;
        if (find_leaf_by_pubkey(&mls, member_pubkeys[i], &leaf_idx) != 0) {
            err = MARMOT_ERR_MEMBER_NOT_FOUND;
            goto out;
        }
        mls_group_free(&pre);
        if (clone_mls_group(&mls, &pre) != 0) {
            err = MARMOT_ERR_MLS;
            goto out;
        }
        MlsCommitResult result;
        memset(&result, 0, sizeof(result));
        if (mls_group_remove_member(&mls, leaf_idx, &result) != 0) {
            err = MARMOT_ERR_MLS;
            goto out;
        }
        mls_commit_result_clear(&last_result);
        last_result = result;
    }

    err = finish_local_commit(m, group, &pre, &mls, last_result.commit_data,
                              last_result.commit_len, out_commit_json);
out:
    mls_commit_result_clear(&last_result);
    mls_group_free(&pre);
    mls_group_free(&mls);
    marmot_group_free(group);
    return err;
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

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: GroupContext extensions for a metadata update
 * ──────────────────────────────────────────────────────────────────────── */

/* Replace (or append) the marmot_group_data entry of a serialized Extension
 * list with `gde_bytes`, keeping every other extension and the order. */
static int
replace_group_data_extension(const uint8_t *exts, size_t exts_len,
                             const uint8_t *gde_bytes, size_t gde_len,
                             uint8_t **out, size_t *out_len)
{
    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, exts_len + gde_len + 8) != 0) return -1;
    MlsTlsReader r;
    mls_tls_reader_init(&r, exts, exts_len);
    bool replaced = false;
    while (!mls_tls_reader_done(&r)) {
        size_t entry_start = r.pos;
        uint16_t type;
        size_t len;
        if (mls_tls_read_u16(&r, &type) != 0 || mls_tls_read_vli(&r, &len) != 0 ||
            len > mls_tls_reader_remaining(&r))
            goto fail;
        r.pos += len;
        if (type != MARMOT_EXTENSION_TYPE) {
            if (mls_tls_buf_append(&buf, exts + entry_start, r.pos - entry_start) != 0)
                goto fail;
        } else if (!replaced) {
            if (mls_tls_write_u16(&buf, MARMOT_EXTENSION_TYPE) != 0 ||
                mls_tls_write_opaque16(&buf, gde_bytes, gde_len) != 0)
                goto fail;
            replaced = true;
        } else {
            goto fail; /* two marmot_group_data extensions */
        }
    }
    if (!replaced &&
        (mls_tls_write_u16(&buf, MARMOT_EXTENSION_TYPE) != 0 ||
         mls_tls_write_opaque16(&buf, gde_bytes, gde_len) != 0))
        goto fail;
    *out = buf.data;
    *out_len = buf.len;
    return 0;
fail:
    mls_tls_buf_free(&buf);
    return -1;
}

/* Free and replace a string field with a copy of `value`. */
static int
replace_string(char **field, const char *value)
{
    char *copy = strdup(value);
    if (!copy) return -1;
    free(*field);
    *field = copy;
    return 0;
}

/**
 * The GroupData after a metadata update: the group's current
 * marmot_group_data with the non-NULL fields of `config` applied (MIP-01;
 * image fields and anything not in the config are kept), and the full
 * GroupContext extension list carrying it.  A group without GroupData gets
 * one for its nostr_group_id.
 */
static MarmotError
updated_group_data(const MlsGroup *mls, const MarmotGroupConfig *config,
                   const uint8_t nostr_group_id[32],
                   MarmotGroupDataExtension **gde_out,
                   uint8_t **exts_out, size_t *exts_len_out)
{
    const uint8_t *cur = NULL;
    size_t cur_len = 0, count = 0;
    if (marmot_extensions_find(mls->extensions_data, mls->extensions_len,
                               MARMOT_EXTENSION_TYPE, &cur, &cur_len, &count) != 0 ||
        count > 1)
        return MARMOT_ERR_MLS;

    MarmotGroupDataExtension *gde = NULL;
    if (count == 1) {
        gde = marmot_group_data_extension_deserialize(cur, cur_len);
        if (!gde) return MARMOT_ERR_MLS;
    } else {
        gde = marmot_group_data_extension_new();
        if (!gde) return MARMOT_ERR_MEMORY;
        gde->version = MARMOT_EXTENSION_VERSION;
        memcpy(gde->nostr_group_id, nostr_group_id, 32);
    }

    MarmotError err = MARMOT_ERR_MEMORY;
    if ((config->name && replace_string(&gde->name, config->name) != 0) ||
        (config->description &&
         replace_string(&gde->description, config->description) != 0))
        goto fail;
    if (config->admin_count > 0 && config->admin_pubkeys) {
        uint8_t (*admins)[32] = malloc(config->admin_count * 32);
        if (!admins) goto fail;
        memcpy(admins, config->admin_pubkeys, config->admin_count * 32);
        free(gde->admins);
        gde->admins = admins;
        gde->admin_count = config->admin_count;
    }
    if (config->relay_count > 0 && config->relay_urls) {
        char **relays = calloc(config->relay_count, sizeof(char *));
        if (!relays) goto fail;
        for (size_t i = 0; i < config->relay_count; i++) {
            relays[i] = strdup(config->relay_urls[i]);
            if (!relays[i]) {
                for (size_t j = 0; j < i; j++) free(relays[j]);
                free(relays);
                goto fail;
            }
        }
        for (size_t i = 0; i < gde->relay_count; i++) free(gde->relays[i]);
        free(gde->relays);
        gde->relays = relays;
        gde->relay_count = config->relay_count;
    }

    uint8_t *gde_bytes = NULL;
    size_t gde_len = 0;
    if (marmot_group_data_extension_serialize(gde, &gde_bytes, &gde_len) != 0) {
        err = MARMOT_ERR_SERIALIZATION;
        goto fail;
    }
    int rc = replace_group_data_extension(mls->extensions_data, mls->extensions_len,
                                          gde_bytes, gde_len, exts_out, exts_len_out);
    free(gde_bytes);
    if (rc != 0) {
        err = MARMOT_ERR_MLS;
        goto fail;
    }
    *gde_out = gde;
    return MARMOT_OK;
fail:
    marmot_group_data_extension_free(gde);
    return err;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_update_group_metadata
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_update_group_metadata(Marmot *m,
                              const MarmotGroupId *mls_group_id,
                              const MarmotGroupConfig *config,
                              char **out_commit_json)
{
    if (!m || !mls_group_id || !config || !out_commit_json)
        return MARMOT_ERR_INVALID_ARG;
    *out_commit_json = NULL;

    MarmotGroup *group = NULL;
    MlsGroup mls;
    MarmotError err = load_group_for_commit(m, mls_group_id, &group, &mls);
    if (err != MARMOT_OK) return err;

    /* Commit the new GroupData as a GroupContextExtensions proposal (RFC 9420
     * §12.1.7) so every member moves to the same GroupContext; the Commit is
     * returned for publication (nostrc-9ata).  The local group record changes
     * only once that Commit exists and is persisted. */
    MarmotGroupDataExtension *gde = NULL;
    uint8_t *new_ext = NULL;
    size_t new_ext_len = 0;
    MlsGroup pre;
    memset(&pre, 0, sizeof(pre));
    MlsCommitResult commit_result;
    memset(&commit_result, 0, sizeof(commit_result));

    err = updated_group_data(&mls, config, group->nostr_group_id,
                             &gde, &new_ext, &new_ext_len);
    marmot_group_data_extension_free(gde);  /* the committed copy is re-read */
    if (err != MARMOT_OK) goto out;
    if (clone_mls_group(&mls, &pre) != 0) {
        err = MARMOT_ERR_MLS;
        goto out;
    }
    int rc = mls_group_commit_extensions(&mls, new_ext, new_ext_len, &commit_result);
    if (rc != 0) {
        err = rc == MARMOT_ERR_UNSUPPORTED ? MARMOT_ERR_UNSUPPORTED : MARMOT_ERR_MLS;
        goto out;
    }
    err = finish_local_commit(m, group, &pre, &mls, commit_result.commit_data,
                              commit_result.commit_len, out_commit_json);
out:
    free(new_ext);
    mls_commit_result_clear(&commit_result);
    mls_group_free(&pre);
    mls_group_free(&mls);
    marmot_group_free(group);
    return err;
}
