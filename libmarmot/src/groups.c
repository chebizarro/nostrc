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
#include "adopted.h"
#include "commits.h"
#include "proposals.h"
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
    sodium_memzero(state_data, state_len);   /* epoch secrets */
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

#define MLS_EXTENSION_REQUIRED_CAPABILITIES 0x0003

/* MIP-01 "Required MLS Extensions": every group's GroupContext carries
 * required_capabilities (RFC 9420 section 7.2) next to marmot_group_data,
 * requiring 0xF2EE of every member:
 *
 *   struct { ExtensionType extension_types<V>;   = [0xF2EE]
 *            ProposalType proposal_types<V>;     = []
 *            CredentialType credential_types<V>; = [] } RequiredCapabilities;
 *
 * Proposal types: MIP-01 also wants self_remove (0x000a), which libmarmot
 * implements since 0.12.0 (nostrc-2um6).  It is required exactly as MDK
 * 0.8's "LCD" rule (groups.rs) computes, byte for byte: [0x000a] when every
 * invitee advertises it, [] when one lacks it -- and [] for a group created
 * with no invitee ("empty stays empty", review L1): required_capabilities
 * is never rewritten, and requiring SelfRemove would shut out every later
 * invitee whose leaf lacks it (libmarmot 0.11.0 and older).  Members leave
 * by SelfRemove only when the group requires it, and otherwise by a Remove
 * of themselves that an admin commits (MDK 0.8 and libmarmot alike).  libmarmot 0.10.0 and older omitted the
 * extension, and OpenMLS (valn1001) then refuses every
 * GroupContextExtensions proposal, which lists 0xF2EE without it: MDK could
 * not follow a rename (nostrc-7gx7). */
static int
write_required_capabilities(MlsTlsBuf *buf, bool self_remove)
{
    static const uint8_t with_self_remove[] = {
        0x02, (uint8_t)(MARMOT_EXTENSION_TYPE >> 8), (uint8_t)(MARMOT_EXTENSION_TYPE & 0xff),
        0x02, (uint8_t)(MLS_PROPOSAL_SELF_REMOVE >> 8),
        (uint8_t)(MLS_PROPOSAL_SELF_REMOVE & 0xff),   /* proposal_types */
        0x00,                                         /* credential_types */
    };
    static const uint8_t without[] = {
        0x02, (uint8_t)(MARMOT_EXTENSION_TYPE >> 8), (uint8_t)(MARMOT_EXTENSION_TYPE & 0xff),
        0x00,   /* proposal_types */
        0x00,   /* credential_types */
    };
    const uint8_t *data = self_remove ? with_self_remove : without;
    size_t len = self_remove ? sizeof with_self_remove : sizeof without;
    return mls_tls_write_u16(buf, MLS_EXTENSION_REQUIRED_CAPABILITIES) != 0 ||
                   mls_tls_write_opaque16(buf, data, len) != 0
               ? -1
               : 0;
}

/* Whether a KeyPackage's leaf advertises SelfRemove (MDK's LCD input). */
static bool
key_package_self_remove(const MlsKeyPackage *kp)
{
    for (size_t i = 0; i < kp->leaf_node.proposal_count; i++)
        if (kp->leaf_node.proposals[i] == MLS_PROPOSAL_SELF_REMOVE) return true;
    return false;
}

static int
build_group_data_extension(const MarmotGroupConfig *config,
                            const uint8_t nostr_group_id[32], bool require_self_remove,
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
    /* Extension type: 0xF2EE, then required_capabilities (MIP-01). */
    if (mls_tls_write_u16(&buf, MARMOT_EXTENSION_TYPE) != 0 ||
        mls_tls_write_opaque16(&buf, gde_bytes, gde_len) != 0 ||
        write_required_capabilities(&buf, require_self_remove) != 0) {
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
                     const uint8_t sender_pubkey[32],
                     const char *kp_event_id,
                     const uint8_t nostr_group_id[32],
                     const char *group_name,
                     const char *group_description,
                     const uint8_t (*admin_pubkeys)[32], size_t admin_count,
                     size_t member_count,
                     const char **relay_urls, size_t relay_count,
                     int64_t created_at)
{
    /* Welcome rumor is a kind:444 unsigned event with:
     * - pubkey: the sender's account (NIP-59: the seal's author). The joiner
     *   binds the Welcome's GroupInfo signer leaf to it (nostrc-7vyi).
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

    char *sender_hex = marmot_hex_encode(sender_pubkey, 32);
    if (!sender_hex) { nostr_event_free(event); free(b64_content); return NULL; }
    nostr_event_set_pubkey(event, sender_hex);
    free(sender_hex);
    nostr_event_set_kind(event, MARMOT_KIND_WELCOME);
    nostr_event_set_content(event, b64_content);
    /* Dated as the Commit that adds the joiner, whose first events follow it
     * (nostrc-2lrz, review W24 L3). */
    nostr_event_set_created_at(event, created_at);
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
 * Internal: several invitees, one Commit (nostrc-wc6v)
 * ──────────────────────────────────────────────────────────────────────── */

static void
free_key_packages(MlsKeyPackage *kps, size_t count)
{
    if (!kps) return;
    for (size_t i = 0; i < count; i++) mls_key_package_clear(&kps[i]);
    free(kps);
}

/* `pubkeys` (optional, `count` entries) receives each KeyPackage's account.
 * Every leaf must carry a valid account proof, which every member will check
 * (nostrc-7vyi): MARMOT_ERR_KEY_PACKAGE_IDENTITY otherwise, unless legacy
 * mode accepts one without any. */
static MarmotError
parse_key_packages(Marmot *m, const char **jsons, size_t count, MlsKeyPackage **out,
                   uint8_t (*pubkeys)[32])
{
    *out = NULL;
    MlsKeyPackage *kps = calloc(count, sizeof(*kps));
    if (!kps) return MARMOT_ERR_MEMORY;
    for (size_t i = 0; i < count; i++) {
        uint8_t member_pubkey[32];
        MarmotError err = jsons[i]
            ? marmot_parse_key_package_event(jsons[i], &kps[i],
                                             pubkeys ? pubkeys[i] : member_pubkey)
            : MARMOT_ERR_VALIDATION;
        if (err == MARMOT_OK) {
            MarmotLeafProofStatus st = marmot_leaf_proof_status(&kps[i].leaf_node,
                                                                kps[i].cipher_suite);
            if (st == MARMOT_LEAF_PROOF_INVALID ||
                (st == MARMOT_LEAF_PROOF_ABSENT && !m->config.allow_unproven_members))
                err = MARMOT_ERR_KEY_PACKAGE_IDENTITY;
            if (err != MARMOT_OK) mls_key_package_clear(&kps[i]);
        }
        if (err != MARMOT_OK) {
            free_key_packages(kps, i);
            return err == MARMOT_ERR_KEY_PACKAGE_IDENTITY ? err : MARMOT_ERR_VALIDATION;
        }
    }
    *out = kps;
    return MARMOT_OK;
}

/* One Commit adding every KeyPackage, and replacing the GroupContext
 * extensions with `extensions` when not NULL; the group is unchanged on
 * failure. */
static int
add_key_packages(MlsGroup *mls, const MlsKeyPackage *kps, size_t count,
                 const uint8_t *extensions, size_t extensions_len, MlsAddResult *result)
{
    const MlsKeyPackage **ptrs = calloc(count, sizeof(*ptrs));
    if (!ptrs) return MARMOT_ERR_MEMORY;
    for (size_t i = 0; i < count; i++) ptrs[i] = &kps[i];
    int rc = mls_group_add_members_with_extensions(mls, ptrs, count, extensions,
                                                   extensions_len, result);
    free(ptrs);
    return rc;
}

/* The single Welcome, wrapped once per invitee (each rumor names that
 * invitee's KeyPackage event; the joiner finds its own EncryptedGroupSecrets
 * entry by KeyPackageRef). */
static MarmotError
build_welcome_rumors(const MlsAddResult *add, const uint8_t sender_pubkey[32],
                     const char **kp_event_jsons,
                     size_t count, const uint8_t nostr_group_id[32],
                     const char *name, const char *description,
                     const uint8_t (*admins)[32], size_t admin_count,
                     size_t member_count, const char **relay_urls,
                     size_t relay_count, int64_t created_at, char **out)
{
    for (size_t i = 0; i < count; i++) {
        char *kp_event_id = extract_event_id_hex(kp_event_jsons[i]);
        out[i] = build_welcome_rumor(add->welcome_data, add->welcome_len, sender_pubkey,
                                     kp_event_id,
                                     nostr_group_id, name, description, admins,
                                     admin_count, member_count, relay_urls, relay_count,
                                     created_at);
        free(kp_event_id);
        if (!out[i]) return MARMOT_ERR_EVENT_BUILD;
    }
    return MARMOT_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_create_group
 * ──────────────────────────────────────────────────────────────────────── */

static MarmotError
create_group_impl(Marmot *m,
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

    /* The invitees' KeyPackages first: what they support decides the
     * group's required proposals (write_required_capabilities()). */
    MlsKeyPackage *kps = NULL;
    MarmotError err = MARMOT_OK;
    bool require_self_remove = kp_count > 0;   /* none invited: [] (MDK) */
    if (kp_count > 0) {
        err = parse_key_packages(m, key_package_event_jsons, kp_count, &kps, NULL);
        if (err != MARMOT_OK) return err;
        for (size_t i = 0; i < kp_count; i++)
            require_self_remove = require_self_remove && key_package_self_remove(&kps[i]);
    }

    /* Build GroupContext extensions with GroupData */
    uint8_t *ext_data = NULL;
    size_t ext_len = 0;
    err = build_group_data_extension(config, nostr_group_id, require_self_remove,
                                     &ext_data, &ext_len);
    if (err != MARMOT_OK) {
        free_key_packages(kps, kp_count);
        return err;
    }

    /* The creator's leaf carries this instance's account proof
     * (nostrc-7vyi; marmot_set_account_proof()).  Without one no joiner
     * that requires proofs would accept the leaf in another admin's
     * Welcome: only MarmotConfig.allow_unproven_self creates it unproven. */
    uint8_t proof[MARMOT_ACCOUNT_PROOF_LEN];
    uint8_t *leaf_ext = NULL;
    size_t leaf_ext_len = 0;
    bool proven = marmot_account_proof_lookup(m, creator_pubkey, proof);
    if (!proven && !m->config.allow_unproven_self) {
        free(ext_data);
        free_key_packages(kps, kp_count);
        return MARMOT_ERR_KEY_PACKAGE_IDENTITY;
    }
    if (proven) {
        err = marmot_leaf_proof_extensions(proof, &leaf_ext, &leaf_ext_len);
        sodium_memzero(proof, sizeof(proof));
        if (err != MARMOT_OK) {
            free(ext_data);
            free_key_packages(kps, kp_count);
            return err;
        }
    }

    /* Create the single-member MLS group */
    MlsGroup mls_group;
    memset(&mls_group, 0, sizeof(mls_group));

    int rc = mls_group_create_with_leaf_extensions(&mls_group,
                                                   mls_group_id, 32,
                                                   creator_pubkey, 32,
                                                   m->ed25519_sk,
                                                   ext_data, ext_len,
                                                   leaf_ext, leaf_ext_len);
    free(ext_data);
    free(leaf_ext);
    if (rc != 0) {
        free_key_packages(kps, kp_count);
        return MARMOT_ERR_MLS;
    }

    /* All invitees join through one Commit with one Add each and one
     * Welcome (RFC 9420 §12.4; nostrc-wc6v): one Commit per invitee left
     * every earlier invitee at a stale epoch. */
    result->welcome_count = kp_count;
    if (kp_count > 0) {
        result->welcome_rumor_jsons = calloc(kp_count, sizeof(char *));
        /* The published Commit is sealed with its source epoch's exporter
         * secret (MIP-03), like any kind:445 event of that epoch. */
        uint8_t source_exporter[32];
        memcpy(source_exporter, mls_group.epoch_secrets.exporter_secret, 32);
        MlsAddResult add_result;
        memset(&add_result, 0, sizeof(add_result));
        rc = result->welcome_rumor_jsons
                 ? add_key_packages(&mls_group, kps, kp_count, NULL, 0, &add_result)
                 : MARMOT_ERR_MEMORY;
        free_key_packages(kps, kp_count);
        /* Every joiner must accept the tree the Welcome carries. */
        if (rc == 0)
            rc = marmot_tree_members_bound(&mls_group, UINT32_MAX,
                                           m->config.allow_unproven_members);
        if (rc == MARMOT_ERR_KEY_PACKAGE_IDENTITY) {
            sodium_memzero(source_exporter, sizeof(source_exporter));
            mls_add_result_clear(&add_result);
            mls_group_free(&mls_group);
            marmot_create_group_result_free(result);
            return MARMOT_ERR_KEY_PACKAGE_IDENTITY;
        }
        if (rc == 0) {
            int64_t created_at = 0;
            err = marmot_next_group_event_time(m, nostr_group_id, true, &created_at);
            if (err == MARMOT_OK)
                err = build_welcome_rumors(&add_result, creator_pubkey,
                                           key_package_event_jsons, kp_count,
                                           nostr_group_id, config->name, config->description,
                                           (const uint8_t (*)[32])config->admin_pubkeys,
                                           config->admin_count, mls_group.tree.n_leaves,
                                           (const char **)config->relay_urls,
                                           config->relay_count, created_at,
                                           result->welcome_rumor_jsons);
            if (err == MARMOT_OK)
                result->evolution_event_json = marmot_commit_build_event(
                    add_result.commit_data, add_result.commit_len,
                    source_exporter, nostr_group_id, created_at);
            if (err == MARMOT_OK && !result->evolution_event_json)
                err = MARMOT_ERR_EVENT_BUILD;
        } else {
            err = rc == MARMOT_ERR_MEMORY ? MARMOT_ERR_MEMORY : MARMOT_ERR_MLS;
        }
        sodium_memzero(source_exporter, sizeof(source_exporter));
        mls_add_result_clear(&add_result);
        if (err != MARMOT_OK) {
            mls_group_free(&mls_group);
            marmot_create_group_result_free(result);
            return err;
        }
    }

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
    sodium_memzero(state_data, state_len);
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
 * Adopted-profile creation (nostrc-qp24.5.1, nostrc-qp24.5.1.1)
 * ────────────────────────────────────────────────────────────────────────── */

/* The kind:444 rumor of the adopted Nostr binding (transports/nostr.md
 * "Welcome delivery"): content is base64 of MLSMessage(mls_welcome); exactly
 * one `e` tag (the consumed KeyPackage event) and one `relays` tag (the
 * group's routing relays); no `encoding` tag and no cleartext preview.
 * Unsigned (it is gift-wrapped by the caller). */
static char *
build_adopted_welcome_rumor(const uint8_t *welcome, size_t welcome_len,
                            const uint8_t sender[32], const char *kp_event_id,
                            char *const *relays, size_t relay_count, int64_t created_at)
{
    if (!kp_event_id || strlen(kp_event_id) != 64 || relay_count == 0) return NULL;
    char *b64 = base64_encode(welcome, welcome_len);
    char *sender_hex = marmot_hex_encode(sender, 32);
    NostrEvent *event = b64 && sender_hex ? nostr_event_new() : NULL;
    NostrTags *tags = event ? nostr_tags_new(0) : NULL;
    NostrTag *e = tags ? nostr_tag_new("e", kp_event_id, NULL) : NULL;
    NostrTag *r = e ? nostr_tag_new("relays", relays[0], NULL) : NULL;
    char *json = NULL;
    if (r) {
        for (size_t i = 1; i < relay_count; i++) nostr_tag_append(r, relays[i]);
        nostr_tags_append(tags, e);
        nostr_tags_append(tags, r);
        e = r = NULL;
        nostr_event_set_pubkey(event, sender_hex);
        nostr_event_set_kind(event, MARMOT_KIND_WELCOME);
        nostr_event_set_content(event, b64);
        /* Dated as the Commit that adds the joiner (nostrc-2lrz). */
        nostr_event_set_created_at(event, created_at);
        nostr_event_set_tags(event, tags);
        tags = NULL;
        json = nostr_event_serialize_compact(event);
    }
    if (e) nostr_tag_free(e);
    if (r) nostr_tag_free(r);
    if (tags) nostr_tags_free(tags);
    if (event) nostr_event_free(event);
    free(sender_hex);
    free(b64);
    return json;
}

static int
cmp_key32(const void *a, const void *b)
{
    return memcmp(a, b, 32);
}

/* Store a new group: MLS state, exporter secret, record, routing relays.
 * On error nothing of it remains (for backends without transactions). */
static MarmotError
persist_new_adopted_group(Marmot *m, const MlsGroup *mls, const MarmotGroup *group,
                          char *const *relays, size_t relay_count)
{
    MarmotStorage *s = m->storage;
    if (!s->replace_group_relays) return MARMOT_ERR_STORAGE;
    uint8_t *state = NULL;
    size_t state_len = 0;
    if (mls_group_serialize(mls, &state, &state_len) != 0) return MARMOT_ERR_SERIALIZATION;
    MarmotError err = s->mls_store(s->ctx, "mls_group", mls->group_id, mls->group_id_len,
                                   state, state_len);
    sodium_memzero(state, state_len);
    free(state);
    if (err != MARMOT_OK) return err;
    err = s->save_exporter_secret(s->ctx, &group->mls_group_id, mls->epoch,
                                  mls->epoch_secrets.exporter_secret);
    if (err == MARMOT_OK) {
        err = s->save_group(s->ctx, group);
        if (err == MARMOT_OK) {
            err = s->replace_group_relays(s->ctx, &group->mls_group_id,
                                          (const char **)relays, relay_count);
            if (err != MARMOT_OK) s->delete_group(s->ctx, &group->mls_group_id);
        }
        if (err != MARMOT_OK)
            s->delete_exporter_secret(s->ctx, &group->mls_group_id, mls->epoch);
    }
    if (err != MARMOT_OK) s->mls_delete(s->ctx, "mls_group", mls->group_id, mls->group_id_len);
    return err;
}

static MarmotError
create_group_adopted_impl(Marmot *m, const uint8_t creator_pubkey[32],
                          const uint8_t *creator_sk, MarmotAccountSignFunc account_sign,
                          void *sign_data, const char **kp_jsons, size_t kp_count,
                          const MarmotGroupConfig *config, MarmotCreateGroupResult *result)
{
    if (!m->storage || !m->storage->save_group || !m->storage->delete_group ||
        !m->storage->mls_store || !m->storage->mls_delete ||
        !m->storage->save_exporter_secret || !m->storage->delete_exporter_secret ||
        !m->storage->replace_group_relays)
        return MARMOT_ERR_STORAGE;
    if (marmot_ensure_identity(m) != 0) return MARMOT_ERR_CRYPTO;

    MarmotError err = MARMOT_OK;
    MlsKeyPackage *kps = NULL;
    uint8_t (*members)[32] = NULL;
    uint8_t (*admins)[32] = NULL;
    const char **relays = NULL;
    size_t relay_count = 0, admin_count = 0;
    uint8_t *gc = NULL, *leaf_ext = NULL;
    size_t gc_len = 0, leaf_ext_len = 0;
    char **group_relays = NULL;
    size_t group_relay_count = 0;
    uint8_t source_exporter[32];
    uint8_t proof[MARMOT_ACCOUNT_PROOF_LEN];
    MlsGroup mls;
    MlsAddResult add;
    MlsAdoptedGroupContext parsed;
    uint8_t mls_group_id[32], nostr_group_id[32];
    bool have_mls = false;
    int rc;
    memset(&mls, 0, sizeof(mls));
    memset(&add, 0, sizeof(add));
    memset(source_exporter, 0, sizeof(source_exporter));

    /* 1. Invitees: signed ADOPTED kind:30443 events, each fully validated
     *    (id, signature, tags, framing, lifetime, account proof). */
    if (kp_count > 0) {
        kps = calloc(kp_count, sizeof(*kps));
        members = calloc(kp_count, sizeof(*members));
        if (!kps || !members) {
            err = MARMOT_ERR_MEMORY;
            goto done;
        }
        for (size_t i = 0; i < kp_count; i++) {
            err = kp_jsons[i] ? marmot_parse_key_package_event_for_profile(
                                    kp_jsons[i], MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, 0,
                                    &kps[i], members[i])
                              : MARMOT_ERR_INVALID_ARG;
            if (err != MARMOT_OK) goto done;
        }
    }

    /* 2. Admins: the creator plus the requested ones, each of them a
     *    founding member (admin-policy-v1.md; MDK mdk#737), sorted, unique. */
    admins = calloc(1 + config->admin_count, sizeof(*admins));
    if (!admins) {
        err = MARMOT_ERR_MEMORY;
        goto done;
    }
    memcpy(admins[0], creator_pubkey, 32);
    admin_count = 1;
    for (size_t i = 0; i < config->admin_count; i++) {
        bool member = memcmp(config->admin_pubkeys[i], creator_pubkey, 32) == 0;
        for (size_t j = 0; !member && j < kp_count; j++)
            member = memcmp(config->admin_pubkeys[i], members[j], 32) == 0;
        if (!member) {
            err = MARMOT_ERR_INVALID_ARG;
            goto done;
        }
        memcpy(admins[admin_count++], config->admin_pubkeys[i], 32);
    }
    qsort(admins, admin_count, 32, cmp_key32);
    size_t unique = 0;
    for (size_t i = 0; i < admin_count; i++)
        if (unique == 0 || memcmp(admins[unique - 1], admins[i], 32) != 0)
            memcpy(admins[unique++], admins[i], 32);
    admin_count = unique;

    /* 3. Signed routing relays (1..16, the relay URL profile). */
    err = marmot_adopted_canonical_relays((const char *const *)config->relay_urls,
                                          config->relay_count, &relays, &relay_count);
    if (err != MARMOT_OK) goto done;

    /* 4. The GroupContext; every invitee's leaf must satisfy it before any
     *    MLS state is built (MDK: MissingRequiredCapabilities). */
    randombytes_buf(mls_group_id, sizeof(mls_group_id));
    randombytes_buf(nostr_group_id, sizeof(nostr_group_id));
    err = marmot_adopted_group_context_build(config->name, config->description,
                                             (const uint8_t (*)[32])admins, admin_count,
                                             nostr_group_id, relays, relay_count, &gc, &gc_len);
    if (err != MARMOT_OK) goto done;
    if (mls_adopted_group_context_parse(gc, gc_len, &parsed) != 0) {
        err = MARMOT_ERR_INTERNAL;
        goto done;
    }
    for (size_t i = 0; i < kp_count; i++)
        if (mls_adopted_leaf_check(&kps[i].leaf_node, &parsed) != 0) {
            err = MARMOT_ERR_KEY_PACKAGE;
            goto done;
        }

    /* 5. The creator's leaf proof (qp24.5.1.1): signed now over this
     *    instance's MLS signature key, by @creator_sk or @account_sign; else
     *    the proof this instance was enrolled with for @creator_pubkey. */
    if (creator_sk || account_sign) {
        int64_t now = marmot_now();
        err = marmot_account_proof_create(creator_pubkey, creator_sk, account_sign, sign_data,
                                          MARMOT_CIPHERSUITE, MARMOT_SIGNATURE_SCHEME_ED25519,
                                          m->ed25519_pk, MLS_SIG_PK_LEN,
                                          (uint64_t)(now > 0 ? now : 1), proof);
    } else {
        err = marmot_account_proof_lookup(m, creator_pubkey, proof)
                  ? MARMOT_OK : MARMOT_ERR_KEY_PACKAGE_IDENTITY;
    }
    if (err == MARMOT_OK) err = marmot_leaf_adopted_extensions(proof, &leaf_ext, &leaf_ext_len);
    sodium_memzero(proof, sizeof(proof));
    if (err != MARMOT_OK) goto done;

    /* 6. The group (validated as ADOPTED by the MLS layer), then one Commit
     *    adding every invitee and one Welcome. */
    rc = mls_group_create_with_leaf_extensions(&mls, mls_group_id, 32, creator_pubkey, 32,
                                                   m->ed25519_sk, gc, gc_len, leaf_ext,
                                                   leaf_ext_len);
    if (rc != 0) {
        err = rc == MARMOT_ERR_MEMORY ? MARMOT_ERR_MEMORY : (MarmotError)rc;
        goto done;
    }
    have_mls = true;
    if (mls.profile != MARMOT_GROUP_PROFILE_ADOPTED) {
        err = MARMOT_ERR_INTERNAL;
        goto done;
    }
    memcpy(source_exporter, mls.epoch_secrets.exporter_secret, 32);
    result->welcome_count = kp_count;
    if (kp_count > 0) {
        result->welcome_rumor_jsons = calloc(kp_count, sizeof(char *));
        if (!result->welcome_rumor_jsons) {
            err = MARMOT_ERR_MEMORY;
            goto done;
        }
        rc = add_key_packages(&mls, kps, kp_count, NULL, 0, &add);
        if (rc != 0) {
            err = rc == MARMOT_ERR_MEMORY ? MARMOT_ERR_MEMORY : MARMOT_ERR_MLS;
            goto done;
        }
    }
    /* Every joiner checks the epoch it enters and verifies every leaf: so do
     * we, before anything leaves. */
    rc = mls_group_profile_check_entered(&mls);
    if (rc != 0) {
        err = (MarmotError)rc;
        goto done;
    }
    err = marmot_adopted_members_proven(&mls);
    if (err != MARMOT_OK) goto done;

    /* 7. The group record from its (signed) components. */
    err = marmot_adopted_group_from_mls(&mls, &result->group, &group_relays,
                                        &group_relay_count);
    if (err != MARMOT_OK) goto done;
    /* The founding Commit and its Welcomes are dated through the group's
     * created_at floor, as for a legacy group (nostrc-2lrz). */
    int64_t created_at = 0;
    if (kp_count > 0) {
        err = marmot_next_group_event_time(m, result->group->nostr_group_id, true, &created_at);
        if (err != MARMOT_OK) goto done;
    }
    for (size_t i = 0; i < kp_count; i++) {
        char *kp_event_id = extract_event_id_hex(kp_jsons[i]);
        result->welcome_rumor_jsons[i] = build_adopted_welcome_rumor(
            add.welcome_data, add.welcome_len, creator_pubkey, kp_event_id, group_relays,
            group_relay_count, created_at);
        free(kp_event_id);
        if (!result->welcome_rumor_jsons[i]) {
            err = MARMOT_ERR_EVENT_BUILD;
            goto done;
        }
    }
    if (kp_count > 0) {
        result->evolution_event_json =
            marmot_commit_build_event(add.commit_data, add.commit_len, source_exporter,
                                      result->group->nostr_group_id, created_at);
        if (!result->evolution_event_json) {
            err = MARMOT_ERR_EVENT_BUILD;
            goto done;
        }
    }

    /* 8. Store it (the founding Commit is applied at once, as for legacy
     *    groups: nobody but the joiners can see it). */
    err = persist_new_adopted_group(m, &mls, result->group, group_relays, group_relay_count);

done:
    sodium_memzero(source_exporter, sizeof(source_exporter));
    mls_add_result_clear(&add);
    if (have_mls) mls_group_free(&mls);
    if (kps) free_key_packages(kps, kp_count);
    free(members);
    free(admins);
    free(relays);
    free(gc);
    free(leaf_ext);
    marmot_adopted_relays_free(group_relays, group_relay_count);
    if (err != MARMOT_OK) marmot_create_group_result_free(result);
    return err;
}

static MarmotError
create_group_for_profile_impl(Marmot *m, MarmotGroupProfile profile,
                              const uint8_t creator_pubkey[32], const uint8_t *creator_sk,
                              MarmotAccountSignFunc account_sign, void *sign_data,
                              const char **kp_jsons, size_t kp_count,
                              const MarmotGroupConfig *config,
                              MarmotCreateGroupResult *result)
{
    if (!m || !creator_pubkey || !config || !result || (kp_count > 0 && !kp_jsons) ||
        (config->admin_count > 0 && !config->admin_pubkeys) ||
        (config->relay_count > 0 && !config->relay_urls))
        return MARMOT_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    switch (profile) {
    case MARMOT_GROUP_PROFILE_LEGACY:
        /* A legacy creator leaf carries the enrolled proof (marmot_create_group()). */
        if (creator_sk || account_sign) return MARMOT_ERR_INVALID_ARG;
        return create_group_impl(m, creator_pubkey, kp_jsons, kp_count, config, result);
    case MARMOT_GROUP_PROFILE_ADOPTED:
        return create_group_adopted_impl(m, creator_pubkey, creator_sk, account_sign, sign_data,
                                         kp_jsons, kp_count, config, result);
    }
    return MARMOT_ERR_INVALID_ARG;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Internal: stage a local Commit for publication (nostrc-9ata)
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
 * Tail shared by the Commit producers.  `pre` is the current state and
 * `post` the state the Commit produces.  The Commit must pass the same
 * Marmot policy every receiver applies; the kind:445 event is sealed with
 * `pre`'s exporter secret and signed by a fresh ephemeral key; and `post` is
 * stored as the group's pending Commit.  Nothing is applied: the caller
 * publishes the event and calls marmot_merge_pending_commit() once a relay
 * accepted it, or marmot_clear_pending_commit() when none did (MIP-03:
 * never apply a Commit before a relay confirms it).
 */
static MarmotError
finish_local_commit(Marmot *m, MarmotGroup *group,
                    const MlsGroup *pre, const MlsGroup *post,
                    const uint8_t *commit, size_t commit_len, int64_t created_at,
                    const MarmotUnsentWelcome *welcomes, size_t welcome_count,
                    char **out_commit_json)
{
    if (!commit || commit_len == 0) return MARMOT_ERR_MLS;
    char *json = marmot_commit_build_event(commit, commit_len,
                                           pre->epoch_secrets.exporter_secret,
                                           group->nostr_group_id, created_at);
    if (!json) return MARMOT_ERR_EVENT_BUILD;
    /* The pending record keeps the signed event (republish after a restart)
     * and the Welcomes with their recipients (sent after the merge). */
    MarmotError err = marmot_commit_stage_pending(m, pre, post, commit, commit_len,
                                                  json, welcomes, welcome_count);
    if (err != MARMOT_OK) {
        free(json);
        return err;
    }
    *out_commit_json = json;
    return MARMOT_OK;
}

/* The policy every receiver applies to our Commit `pre` -> `post`, before
 * anything is dated or stored: a Commit the group refuses must not draw a
 * created_at from the group's floor first (nostrc-u9kv; the backends
 * without transactions keep a drawn time).  Pure. */
static MarmotError
local_commit_precheck(Marmot *m, const MlsGroup *pre, const MlsGroup *post,
                      const MlsCommitSummary *departures)
{
    MarmotCommitKey key;
    MarmotGroupDataExtension *gde = NULL;
    MarmotError err = marmot_commit_authorize_ex(pre, post, pre->own_leaf_index,
                                                 m->config.allow_unproven_members, departures,
                                                 &key, &gde);
    marmot_group_data_extension_free(gde);
    return err;
}

/* finish_local_commit() for a Commit dated now (nostrc-2lrz). */
static MarmotError
finish_local_commit_now(Marmot *m, MarmotGroup *group,
                        const MlsGroup *pre, const MlsGroup *post,
                        const uint8_t *commit, size_t commit_len, char **out_commit_json)
{
    int64_t created_at = 0;
    MarmotError err = local_commit_precheck(m, pre, post, NULL);
    if (err == MARMOT_OK)
        err = marmot_next_group_event_time(m, group->nostr_group_id, true, &created_at);
    if (err != MARMOT_OK) return err;
    return finish_local_commit(m, group, pre, post, commit, commit_len, created_at, NULL, 0,
                               out_commit_json);
}

/* Load an active group we may commit to as an admin.  On success the caller
 * owns *group_out and *mls_out. */
static MarmotError
load_group_for_commit_ex(Marmot *m, const MarmotGroupId *mls_group_id,
                         MarmotGroup **group_out, MlsGroup *mls_out, bool require_admin);

static MarmotError
load_group_for_commit(Marmot *m, const MarmotGroupId *mls_group_id,
                      MarmotGroup **group_out, MlsGroup *mls_out)
{
    return load_group_for_commit_ex(m, mls_group_id, group_out, mls_out, true);
}

/* `require_admin` false: an ordinary Commit (a self-update) any member may
 * make. */
static MarmotError
load_group_for_commit_ex(Marmot *m, const MarmotGroupId *mls_group_id,
                         MarmotGroup **group_out, MlsGroup *mls_out, bool require_admin)
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
    /* Leaving: no Commit of ours any more (nostrc-2um6; member-departure.md). */
    err = marmot_leaving_gate(m, mls_group_id);
    if (err != MARMOT_OK) {
        marmot_group_free(group);
        return err;
    }
    err = marmot_group_reconcile(m, group);
    /* One Commit at a time: the previous one must be merged or cleared. */
    bool pending = false;
    if (err == MARMOT_OK) err = marmot_commit_has_pending(m, group, &pending);
    if (err == MARMOT_OK && pending) err = MARMOT_ERR_OWN_COMMIT_PENDING;
    if (err != MARMOT_OK) {
        marmot_group_free(group);
        return err;
    }
    /* The group's MLS state is required: without it nothing can be committed. */
    if (load_mls_group(m, mls_group_id, mls_out) != 0) {
        marmot_group_free(group);
        return MARMOT_ERR_MLS;
    }
    /* Admin check using our Nostr pubkey from our leaf in the MLS tree */
    uint8_t our_nostr_pk[32];
    if (get_own_credential_identity(mls_out, our_nostr_pk) != 0 ||
        (require_admin && !is_admin(group, our_nostr_pk))) {
        mls_group_free(mls_out);
        marmot_group_free(group);
        return MARMOT_ERR_ADMIN_ONLY;
    }
    *group_out = group;
    return MARMOT_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_merge_pending_commit / marmot_clear_pending_commit
 * ──────────────────────────────────────────────────────────────────────── */

static MarmotError
merge_pending_commit_impl(Marmot *m, const MarmotGroupId *mls_group_id)
{
    if (!m || !mls_group_id)
        return MARMOT_ERR_INVALID_ARG;
    if (!storage_can_commit(m->storage))
        return MARMOT_ERR_STORAGE;

    MarmotGroup *group = NULL;
    MarmotError err = m->storage->find_group_by_mls_id(m->storage->ctx,
                                                         mls_group_id, &group);
    if (err != MARMOT_OK || !group) return MARMOT_ERR_GROUP_NOT_FOUND;

    err = marmot_group_reconcile(m, group);
    if (err == MARMOT_OK) err = marmot_commit_merge_pending(m, group);
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND) {
        /* Nothing pending (e.g. after marmot_create_group(), whose Commit
         * only joiners see): just note the confirmation. */
        group->last_message_processed_at = marmot_now();
        err = m->storage->save_group(m->storage->ctx, group);
    }
    marmot_group_free(group);
    return err;
}

static MarmotError
clear_pending_commit_impl(Marmot *m, const MarmotGroupId *mls_group_id)
{
    if (!m || !mls_group_id)
        return MARMOT_ERR_INVALID_ARG;
    if (!storage_can_commit(m->storage))
        return MARMOT_ERR_STORAGE;
    MarmotGroup *group = NULL;
    MarmotError err = m->storage->find_group_by_mls_id(m->storage->ctx,
                                                         mls_group_id, &group);
    if (err != MARMOT_OK || !group) return MARMOT_ERR_GROUP_NOT_FOUND;
    err = marmot_group_reconcile(m, group);
    if (err == MARMOT_OK) err = marmot_commit_clear_pending(m, group);
    marmot_group_free(group);
    return err;
}

/* The group record, reconciled, for the pending/outbox queries. */
static MarmotError
find_reconciled_group(Marmot *m, const MarmotGroupId *mls_group_id, MarmotGroup **out)
{
    *out = NULL;
    if (!storage_can_commit(m->storage)) return MARMOT_ERR_STORAGE;
    MarmotGroup *group = NULL;
    MarmotError err = m->storage->find_group_by_mls_id(m->storage->ctx,
                                                         mls_group_id, &group);
    if (err != MARMOT_OK || !group) return MARMOT_ERR_GROUP_NOT_FOUND;
    err = marmot_group_reconcile(m, group);
    if (err != MARMOT_OK) {
        marmot_group_free(group);
        return err;
    }
    *out = group;
    return MARMOT_OK;
}

static MarmotError
get_pending_commit_impl(Marmot *m, const MarmotGroupId *mls_group_id,
                          char **out_event_json, bool *out_superseded)
{
    if (!m || !mls_group_id || !out_event_json) return MARMOT_ERR_INVALID_ARG;
    *out_event_json = NULL;
    if (out_superseded) *out_superseded = false;
    MarmotGroup *group = NULL;
    MarmotError err = find_reconciled_group(m, mls_group_id, &group);
    if (err != MARMOT_OK) return err;
    bool live = false;
    err = marmot_commit_get_pending(m, group, out_event_json, &live);
    if (err == MARMOT_OK && *out_event_json && out_superseded) *out_superseded = !live;
    marmot_group_free(group);
    return err;
}

static MarmotError
get_unsent_welcomes_impl(Marmot *m, const MarmotGroupId *mls_group_id,
                           MarmotUnsentWelcome **out_welcomes, size_t *out_count)
{
    if (!m || !mls_group_id || !out_welcomes || !out_count) return MARMOT_ERR_INVALID_ARG;
    *out_welcomes = NULL;
    *out_count = 0;
    MarmotGroup *group = NULL;
    MarmotError err = find_reconciled_group(m, mls_group_id, &group);
    if (err != MARMOT_OK) return err;
    /* Finishes a pending Commit that was applied before a crash. */
    char *ev = NULL;
    bool live = false;
    err = marmot_commit_get_pending(m, group, &ev, &live);
    free(ev);
    if (err == MARMOT_OK)
        err = marmot_commit_get_unsent_welcomes(m, mls_group_id, out_welcomes, out_count);
    marmot_group_free(group);
    return err;
}

static MarmotError
mark_welcomes_sent_impl(Marmot *m, const MarmotGroupId *mls_group_id,
                          const uint8_t (*ids)[32], size_t count)
{
    if (!m || !mls_group_id || (count > 0 && !ids)) return MARMOT_ERR_INVALID_ARG;
    if (!storage_can_commit(m->storage)) return MARMOT_ERR_STORAGE;
    return marmot_commit_mark_welcomes_sent(m, mls_group_id, ids, count);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_add_members
 * ──────────────────────────────────────────────────────────────────────── */

static MarmotError
updated_group_data(const MlsGroup *mls, const MarmotGroupConfig *config,
                   const uint8_t nostr_group_id[32],
                   MarmotGroupDataExtension **gde_out,
                   uint8_t **exts_out, size_t *exts_len_out);

/* A group libmarmot 0.10.0 made may still hold its GroupData in that
 * version's layout, which no joiner reads: a Welcome's GroupData must be
 * MIP-01 (nostrc-c7ho).  For such a group, *exts_out is the extension list
 * with the same GroupData re-encoded as MIP-01, for the Add to commit along
 * (review W24 M1); NULL when the GroupData is MIP-01 or absent. */
static MarmotError
legacy_group_data_reencoded(const MlsGroup *mls, const uint8_t nostr_group_id[32],
                            uint8_t **exts_out, size_t *exts_len_out)
{
    *exts_out = NULL;
    *exts_len_out = 0;
    const uint8_t *data = NULL;
    size_t len = 0, count = 0;
    if (marmot_extensions_find(mls->extensions_data, mls->extensions_len,
                               MARMOT_EXTENSION_TYPE, &data, &len, &count) != 0 ||
        count > 1)
        return MARMOT_ERR_MLS;
    if (count == 0) return MARMOT_OK;
    MarmotGroupDataExtension *gde = marmot_group_data_extension_deserialize(data, len);
    if (gde) {
        marmot_group_data_extension_free(gde);
        return MARMOT_OK;
    }
    static const MarmotGroupConfig unchanged = { 0 };
    MarmotError err = updated_group_data(mls, &unchanged, nostr_group_id, &gde,
                                         exts_out, exts_len_out);
    marmot_group_data_extension_free(gde);
    return err;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Adopted-profile Commits of our own (nostrc-qp24.5.1.3)
 *
 * The same transitions MDK v0.11.0 makes (cgka-engine message_processor
 * send.rs, update_group_data.rs): Adds of proof-bearing adopted KeyPackages;
 * Removes, with the admin-policy update an admin's removal needs in the same
 * Commit; component updates as inline AppDataUpdates (full replacement
 * state).  Each is checked as receivers check it (local_commit_precheck(),
 * then the MLS layer's resulting-epoch check) before it is dated or stored.
 * ──────────────────────────────────────────────────────────────────────── */

/* The adopted GroupContext of `mls`, parsed (borrowing its bytes). */
static MarmotError
adopted_context(const MlsGroup *mls, MlsAdoptedGroupContext *gc)
{
    if (mls->profile != MARMOT_GROUP_PROFILE_ADOPTED) return MARMOT_ERR_UNSUPPORTED;
    int rc = mls_adopted_group_context_parse(mls->extensions_data, mls->extensions_len, gc);
    return rc == 0 ? MARMOT_OK : (MarmotError)rc;
}

/* An AppDataUpdate setting `id` to `bytes` (borrowed). */
static void
adu_update(MlsAppDataUpdate *op, uint16_t id, uint8_t *bytes, size_t len)
{
    memset(op, 0, sizeof(*op));
    op->component_id = id;
    op->operation = MLS_APP_DATA_UPDATE_OP_UPDATE;
    op->update = bytes;
    op->update_len = len;
}

/* marmot.group.admin-policy.v1 state for `admins` (sorted, unique, at least
 * one): admins<V>. */
static MarmotError
admin_policy_encode(const uint8_t (*admins)[32], size_t count, MlsTlsBuf *out)
{
    if (count == 0) return MARMOT_ERR_INVALID_ARG;
    for (size_t i = 1; i < count; i++)
        if (memcmp(admins[i - 1], admins[i], 32) >= 0) return MARMOT_ERR_INVALID_ARG;
    if (mls_tls_buf_init(out, count * 32 + 8) != 0) return MARMOT_ERR_MEMORY;
    if (mls_tls_write_opaque32(out, (const uint8_t *)admins, count * 32) != 0) {
        mls_tls_buf_free(out);
        return MARMOT_ERR_MEMORY;
    }
    return MARMOT_OK;
}

/* Whether account `pk` keeps a member leaf in `mls` once `removed` (leaf
 * indices) are gone. */
static bool
account_stays(const MlsGroup *mls, const uint8_t pk[32], const uint32_t *removed, size_t n)
{
    for (uint32_t i = 0; i < mls->tree.n_leaves; i++) {
        const MlsNode *node = &mls->tree.nodes[mls_tree_leaf_to_node(i)];
        if (node->type != MLS_NODE_LEAF || node->leaf.credential_identity_len != 32 ||
            memcmp(node->leaf.credential_identity, pk, 32) != 0)
            continue;
        bool gone = false;
        for (size_t k = 0; k < n && !gone; k++) gone = removed[k] == i;
        if (!gone) return true;
    }
    return false;
}

/* Our Commit `post` (built from `pre`): checked as receivers check it, dated
 * through the group's floor, sealed and stored as pending with `welcomes`. */
static MarmotError
finish_adopted_commit(Marmot *m, MarmotGroup *group, const MlsGroup *pre, const MlsGroup *post,
                      const MlsAddResult *res, const char **kp_jsons, size_t kp_count,
                      const uint8_t (*recipients)[32], char ***out_welcomes,
                      char **out_commit_json)
{
    MarmotError err = local_commit_precheck(m, pre, post, NULL);
    if (err != MARMOT_OK) return err;
    char **rumors = NULL;
    MarmotUnsentWelcome *outbox = NULL;
    char **relays = NULL;
    size_t n_relays = 0;
    MarmotGroup *rec = NULL;
    uint8_t sender[32];
    int64_t created_at = 0;
    if (kp_count > 0) {
        rumors = calloc(kp_count, sizeof(*rumors));
        outbox = calloc(kp_count, sizeof(*outbox));
        if (!rumors || !outbox) {
            err = MARMOT_ERR_MEMORY;
            goto out;
        }
        /* The Welcome names the group's relays as the epoch it opens signs
         * them (transports/nostr.md "Welcome delivery"). */
        err = marmot_adopted_group_from_mls(post, &rec, &relays, &n_relays);
        if (err == MARMOT_OK && get_own_credential_identity(pre, sender) != 0)
            err = MARMOT_ERR_OWN_LEAF_NOT_FOUND;
        if (err != MARMOT_OK) goto out;
    }
    err = marmot_next_group_event_time(m, group->nostr_group_id, true, &created_at);
    if (err != MARMOT_OK) goto out;
    for (size_t i = 0; i < kp_count; i++) {
        char *kp_event_id = extract_event_id_hex(kp_jsons[i]);
        rumors[i] = build_adopted_welcome_rumor(res->welcome_data, res->welcome_len, sender,
                                                kp_event_id, relays, n_relays, created_at);
        free(kp_event_id);
        if (!rumors[i]) {
            err = MARMOT_ERR_EVENT_BUILD;
            goto out;
        }
        memcpy(outbox[i].recipient, recipients[i], 32);
        outbox[i].rumor_json = rumors[i];   /* borrowed */
    }
    err = finish_local_commit(m, group, pre, post, res->commit_data, res->commit_len, created_at,
                              outbox, kp_count, out_commit_json);
    if (err == MARMOT_OK && out_welcomes) {
        *out_welcomes = rumors;
        rumors = NULL;
    }
out:
    if (rumors) {
        for (size_t i = 0; i < kp_count; i++) free(rumors[i]);
        free(rumors);
    }
    free(outbox);
    marmot_adopted_relays_free(relays, n_relays);
    marmot_group_free(rec);
    return err;
}

static MarmotError
add_members_adopted(Marmot *m, MarmotGroup *group, const MlsGroup *mls,
                    const char **kp_jsons, size_t kp_count, char ***out_welcome_jsons,
                    size_t *out_welcome_count, char **out_commit_json)
{
    MlsAdoptedGroupContext gc;
    MarmotError err = adopted_context(mls, &gc);
    if (err != MARMOT_OK) return err;
    MlsKeyPackage *kps = calloc(kp_count, sizeof(*kps));
    const MlsKeyPackage **ptrs = calloc(kp_count, sizeof(*ptrs));
    uint8_t (*recipients)[32] = calloc(kp_count, 32);
    MlsGroup post;
    memset(&post, 0, sizeof(post));
    MlsAddResult add;
    memset(&add, 0, sizeof(add));
    size_t parsed = 0;
    if (!kps || !ptrs || !recipients) {
        err = MARMOT_ERR_MEMORY;
        goto out;
    }
    /* Invitees: signed adopted kind:30443 events (id, signature, tags,
     * framing, lifetime, a verified account proof), each leaf satisfying the
     * group's requirements (MDK: MissingRequiredCapabilities). */
    for (size_t i = 0; i < kp_count; i++) {
        err = kp_jsons[i] ? marmot_parse_key_package_event_for_profile(
                                kp_jsons[i], MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, 0, &kps[i],
                                recipients[i])
                          : MARMOT_ERR_INVALID_ARG;
        if (err != MARMOT_OK) goto out;
        parsed++;
        if (mls_adopted_leaf_check(&kps[i].leaf_node, &gc) != 0) {
            err = MARMOT_ERR_KEY_PACKAGE;
            goto out;
        }
        ptrs[i] = &kps[i];
    }
    if (clone_mls_group(mls, &post) != 0) {
        err = MARMOT_ERR_MLS;
        goto out;
    }
    int rc = mls_group_commit_adopted(&post, NULL, 0, ptrs, kp_count, NULL, 0, &add);
    if (rc != 0) {
        err = rc == MARMOT_ERR_MEMORY ? MARMOT_ERR_MEMORY
              : rc == MARMOT_ERR_INVALID_ARG ? MARMOT_ERR_INVALID_ARG
              : rc == MARMOT_ERR_KEY_PACKAGE_CAPABILITIES ? MARMOT_ERR_KEY_PACKAGE_CAPABILITIES
              : MARMOT_ERR_MLS;
        goto out;
    }
    /* Every leaf of the epoch the joiners enter verifies (they check it). */
    err = marmot_adopted_members_proven(&post);
    if (err == MARMOT_OK)
        err = finish_adopted_commit(m, group, mls, &post, &add, kp_jsons, kp_count,
                                    (const uint8_t (*)[32])recipients, out_welcome_jsons,
                                    out_commit_json);
    if (err == MARMOT_OK) *out_welcome_count = kp_count;
out:
    if (kps) free_key_packages(kps, parsed);
    free(ptrs);
    free(recipients);
    mls_add_result_clear(&add);
    mls_group_free(&post);
    return err;
}

static MarmotError
remove_members_adopted(Marmot *m, MarmotGroup *group, const MlsGroup *mls,
                       const uint32_t *leaves, size_t count, char **out_commit_json)
{
    MlsAdoptedGroupContext gc;
    MarmotError err = adopted_context(mls, &gc);
    if (err != MARMOT_OK) return err;
    const uint8_t *keys = NULL;
    size_t n_admins = 0;
    if (!gc.admins || mls_admin_policy_v1_decode(gc.admins, gc.admins_len, &keys, &n_admins) != 0)
        return MARMOT_ERR_EXTENSION_FORMAT;
    /* admin-policy-v1.md "Active admins": a Commit that removes an admin's
     * last leaf drops its key in the same Commit (MDK do_send_remove_members
     * stages that update with the Removes). */
    uint8_t (*kept)[32] = calloc(n_admins, 32);
    if (!kept) return MARMOT_ERR_MEMORY;
    size_t n_kept = 0;
    for (size_t i = 0; i < n_admins; i++)
        if (account_stays(mls, keys + 32 * i, leaves, count)) memcpy(kept[n_kept++], keys + 32 * i, 32);
    MlsTlsBuf policy = {0};
    MlsAppDataUpdate op;
    size_t n_ops = 0;
    if (n_kept != n_admins) {
        err = admin_policy_encode((const uint8_t (*)[32])kept, n_kept, &policy);
        if (err != MARMOT_OK) {
            free(kept);
            return err;
        }
        adu_update(&op, MARMOT_COMPONENT_ADMIN_POLICY_V1, policy.data, policy.len);
        n_ops = 1;
    }
    MlsGroup post;
    memset(&post, 0, sizeof(post));
    MlsAddResult res;
    memset(&res, 0, sizeof(res));
    if (clone_mls_group(mls, &post) != 0) {
        err = MARMOT_ERR_MLS;
    } else {
        int rc = mls_group_commit_adopted(&post, leaves, count, NULL, 0, n_ops ? &op : NULL,
                                          n_ops, &res);
        err = rc == 0 ? MARMOT_OK
              : rc == MARMOT_ERR_INVALID_ARG ? MARMOT_ERR_INVALID_ARG
              : rc == MARMOT_ERR_MEMORY ? MARMOT_ERR_MEMORY : MARMOT_ERR_MLS;
    }
    if (err == MARMOT_OK)
        err = finish_adopted_commit(m, group, mls, &post, &res, NULL, 0, NULL, NULL,
                                    out_commit_json);
    mls_add_result_clear(&res);
    mls_group_free(&post);
    mls_tls_buf_free(&policy);
    free(kept);
    return err;
}

static int
cmp_admin_key(const void *a, const void *b)
{
    return memcmp(a, b, 32);
}

/* marmot_update_group_metadata() for an adopted group: each field of
 * `config` that changes its component becomes one inline AppDataUpdate --
 * name/description 0x8001, admins 0x8003 (each a member; sorted), relays
 * 0x8004 (same nostr_group_id; sorted, deduplicated).  Nothing to change is
 * MARMOT_ERR_INVALID_ARG. */
static MarmotError
update_metadata_adopted(Marmot *m, MarmotGroup *group, const MlsGroup *mls,
                        const MarmotGroupConfig *config, char **out_commit_json)
{
    MlsAdoptedGroupContext gc;
    MarmotError err = adopted_context(mls, &gc);
    if (err != MARMOT_OK) return err;
    MlsAppDataUpdate ops[3];
    size_t n_ops = 0;
    MlsTlsBuf profile = {0}, admins = {0}, routing = {0}, relay_entries = {0};
    uint8_t (*sorted)[32] = NULL;
    const char **relays = NULL;
    size_t relay_count = 0;

    if (config->name || config->description) {
        const uint8_t *nm = (const uint8_t *)"", *ds = (const uint8_t *)"";
        size_t nl = 0, dl = 0;
        if (gc.profile &&
            mls_group_profile_v1_decode(gc.profile, gc.profile_len, &nm, &nl, &ds, &dl) != 0) {
            err = MARMOT_ERR_EXTENSION_FORMAT;
            goto out;
        }
        if (config->name) {
            nm = (const uint8_t *)config->name;
            nl = strlen(config->name);
        }
        if (config->description) {
            ds = (const uint8_t *)config->description;
            dl = strlen(config->description);
        }
        if (nl > MARMOT_GROUP_PROFILE_NAME_MAX || dl > MARMOT_GROUP_PROFILE_DESCRIPTION_MAX ||
            !mls_utf8_valid(nm, nl) || !mls_utf8_valid(ds, dl)) {
            err = MARMOT_ERR_INVALID_ARG;
            goto out;
        }
        if (mls_tls_buf_init(&profile, nl + dl + 16) != 0 ||
            mls_tls_write_opaque32(&profile, nm, nl) != 0 ||
            mls_tls_write_opaque32(&profile, ds, dl) != 0) {
            err = MARMOT_ERR_MEMORY;
            goto out;
        }
        if (!gc.profile || gc.profile_len != profile.len ||
            memcmp(gc.profile, profile.data, profile.len) != 0)
            adu_update(&ops[n_ops++], MARMOT_COMPONENT_GROUP_PROFILE_V1, profile.data,
                       profile.len);
    }
    if (config->admin_count > 0 && config->admin_pubkeys) {
        sorted = calloc(config->admin_count, 32);
        if (!sorted) {
            err = MARMOT_ERR_MEMORY;
            goto out;
        }
        memcpy(sorted, config->admin_pubkeys, config->admin_count * 32);
        qsort(sorted, config->admin_count, 32, cmp_admin_key);
        size_t unique = 0;
        for (size_t i = 0; i < config->admin_count; i++) {
            uint32_t leaf = 0;
            if (find_leaf_by_pubkey(mls, sorted[i], &leaf) != 0) {
                err = MARMOT_ERR_MEMBER_NOT_FOUND;   /* every admin is a member */
                goto out;
            }
            if (unique == 0 || memcmp(sorted[unique - 1], sorted[i], 32) != 0)
                memcpy(sorted[unique++], sorted[i], 32);
        }
        err = admin_policy_encode((const uint8_t (*)[32])sorted, unique, &admins);
        if (err != MARMOT_OK) goto out;
        if (!gc.admins || gc.admins_len != admins.len ||
            memcmp(gc.admins, admins.data, admins.len) != 0)
            adu_update(&ops[n_ops++], MARMOT_COMPONENT_ADMIN_POLICY_V1, admins.data, admins.len);
    }
    if (config->relay_count > 0 && config->relay_urls) {
        const uint8_t *ngid = NULL;
        MlsRelaySpan spans[MARMOT_NOSTR_ROUTING_MAX_RELAYS];
        size_t n_spans = 0;
        if (!gc.routing ||
            mls_nostr_routing_v1_decode(gc.routing, gc.routing_len, &ngid, spans, &n_spans) != 0) {
            err = MARMOT_ERR_EXTENSION_FORMAT;
            goto out;
        }
        err = marmot_adopted_canonical_relays((const char *const *)config->relay_urls,
                                              config->relay_count, &relays, &relay_count);
        if (err != MARMOT_OK) goto out;
        if (mls_tls_buf_init(&relay_entries, 256) != 0 || mls_tls_buf_init(&routing, 256) != 0) {
            err = MARMOT_ERR_MEMORY;
            goto out;
        }
        for (size_t i = 0; i < relay_count; i++)
            if (mls_tls_write_opaque32(&relay_entries, (const uint8_t *)relays[i],
                                       strlen(relays[i])) != 0) {
                err = MARMOT_ERR_MEMORY;
                goto out;
            }
        if (mls_tls_buf_append(&routing, ngid, 32) != 0 ||
            mls_tls_write_opaque32(&routing, relay_entries.data, relay_entries.len) != 0) {
            err = MARMOT_ERR_MEMORY;
            goto out;
        }
        if (gc.routing_len != routing.len || memcmp(gc.routing, routing.data, routing.len) != 0)
            adu_update(&ops[n_ops++], MARMOT_COMPONENT_NOSTR_ROUTING_V1, routing.data,
                       routing.len);
    }
    if (n_ops == 0) {
        err = MARMOT_ERR_INVALID_ARG;   /* nothing changes: no Commit */
        goto out;
    }
    MlsGroup post;
    memset(&post, 0, sizeof(post));
    MlsAddResult res;
    memset(&res, 0, sizeof(res));
    if (clone_mls_group(mls, &post) != 0) {
        err = MARMOT_ERR_MLS;
    } else {
        int rc = mls_group_commit_adopted(&post, NULL, 0, NULL, 0, ops, n_ops, &res);
        err = rc == 0 ? MARMOT_OK
              : rc == MARMOT_ERR_INVALID_ARG ? MARMOT_ERR_INVALID_ARG
              : rc == MARMOT_ERR_MEMORY ? MARMOT_ERR_MEMORY
              : rc == MARMOT_ERR_VALIDATION ? MARMOT_ERR_VALIDATION : MARMOT_ERR_MLS;
    }
    if (err == MARMOT_OK)
        err = finish_adopted_commit(m, group, mls, &post, &res, NULL, 0, NULL, NULL,
                                    out_commit_json);
    mls_add_result_clear(&res);
    mls_group_free(&post);
out:
    mls_tls_buf_free(&profile);
    mls_tls_buf_free(&admins);
    mls_tls_buf_free(&routing);
    mls_tls_buf_free(&relay_entries);
    free(sorted);
    free(relays);
    return err;
}

static int exts_with_self_remove_required(const uint8_t *exts, size_t exts_len, uint8_t **out,
                                          size_t *out_len);

/* nostrc-8ndz: whether the Add of `kps` brings legacy group `mls` its
 * second member and should also require SelfRemove: only our leaf so far,
 * SelfRemove not required yet, and our leaf and every invitee advertise it
 * (MDK 0.8's creation-time LCD rule; "empty stays empty" until the first
 * Add, and an invitee without it keeps the group permissive). */
static bool
first_add_requires_self_remove(const Marmot *m, const MlsGroup *mls, const MlsKeyPackage *kps,
                               size_t kp_count)
{
    if (m->config.keep_first_add_permissive || mls->profile == MARMOT_GROUP_PROFILE_ADOPTED ||
        mls_group_requires_proposal(mls, MLS_PROPOSAL_SELF_REMOVE) ||
        !mls_group_members_support_proposal(mls, MLS_PROPOSAL_SELF_REMOVE))
        return false;
    size_t occupied = 0;
    for (uint32_t i = 0; i < mls->tree.n_leaves; i++)
        occupied += mls->tree.nodes[mls_tree_leaf_to_node(i)].type == MLS_NODE_LEAF;
    if (occupied != 1) return false;
    for (size_t i = 0; i < kp_count; i++)
        if (!key_package_self_remove(&kps[i])) return false;
    return kp_count > 0;
}

/* 0x8002 and 0x8007 are independent, full-replacement states. Do not
 * remove a dictionary entry to clear one: the codecs' empty state is the
 * interoperable value and preserves a group's component capability. */
static MarmotError
update_adopted_image_component(Marmot *m, const MarmotGroupId *gid, uint16_t id,
                               const uint8_t *state, size_t state_len,
                               char **out_commit_json)
{
    if (!state) return MARMOT_ERR_INVALID_ARG;
    int valid = mls_adopted_component_state_valid(id, state, state_len);
    if (valid != 0) return (MarmotError)valid;

    MarmotGroup *group = NULL;
    MlsGroup mls;
    MarmotError err = load_group_for_commit(m, gid, &group, &mls);
    if (err != MARMOT_OK) return err;
    if (mls.profile != MARMOT_GROUP_PROFILE_ADOPTED) {
        err = MARMOT_ERR_UNSUPPORTED;
        goto out;
    }
    MlsAdoptedGroupContext gc;
    err = adopted_context(&mls, &gc);
    if (err != MARMOT_OK) goto out;
    const uint8_t *old = id == MARMOT_COMPONENT_GROUP_BLOSSOM_IMAGE_V1
        ? gc.image : gc.avatar;
    size_t old_len = id == MARMOT_COMPONENT_GROUP_BLOSSOM_IMAGE_V1
        ? gc.image_len : gc.avatar_len;
    if (old && old_len == state_len && memcmp(old, state, state_len) == 0) {
        err = MARMOT_ERR_INVALID_ARG; /* no change, no Commit */
        goto out;
    }

    MlsGroup post;
    memset(&post, 0, sizeof(post));
    MlsAddResult res;
    memset(&res, 0, sizeof(res));
    MlsAppDataUpdate op;
    adu_update(&op, id, (uint8_t *)state, state_len);
    if (clone_mls_group(&mls, &post) != 0) {
        err = MARMOT_ERR_MLS;
    } else {
        int rc = mls_group_commit_adopted(&post, NULL, 0, NULL, 0, &op, 1, &res);
        err = rc == 0 ? MARMOT_OK
              : rc == MARMOT_ERR_INVALID_ARG ? MARMOT_ERR_INVALID_ARG
              : rc == MARMOT_ERR_MEMORY ? MARMOT_ERR_MEMORY
              : rc == MARMOT_ERR_VALIDATION ? MARMOT_ERR_VALIDATION : MARMOT_ERR_MLS;
    }
    if (err == MARMOT_OK)
        err = finish_adopted_commit(m, group, &mls, &post, &res, NULL, 0, NULL, NULL,
                                    out_commit_json);
    mls_add_result_clear(&res);
    mls_group_free(&post);
out:
    mls_group_free(&mls);
    marmot_group_free(group);
    return err;
}

/* One libmarmot transaction, as marmot_update_group_metadata(): the pending
 * Commit, the dated group floor and the staged state commit together or not
 * at all. */
static MarmotError
update_image_component_txn(Marmot *m, const MarmotGroupId *gid, uint16_t id,
                           const uint8_t *state, size_t state_len, char **out_commit_json)
{
    if (!m || !gid || !out_commit_json) return MARMOT_ERR_INVALID_ARG;
    *out_commit_json = NULL;
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    err = update_adopted_image_component(m, gid, id, state, state_len, out_commit_json);
    MarmotError end = marmot_txn_end(m, err);
    if (err == MARMOT_OK && end != MARMOT_OK) {
        free(*out_commit_json);
        *out_commit_json = NULL;
    }
    return end;
}

MarmotError
marmot_update_group_blossom_image(Marmot *m, const MarmotGroupId *gid,
                                   const MarmotGroupBlossomImage *image,
                                   char **out_commit_json)
{
    MarmotGroupBlossomImage empty = {0};
    uint8_t *state = NULL;
    size_t state_len = 0;
    MarmotError err = marmot_group_blossom_image_encode(image ? image : &empty,
                                                        &state, &state_len);
    if (err != MARMOT_OK) return err;
    err = update_image_component_txn(m, gid, MARMOT_COMPONENT_GROUP_BLOSSOM_IMAGE_V1,
                                     state, state_len, out_commit_json);
    sodium_memzero(state, state_len);   /* the image key and upload key */
    free(state);
    return err;
}

MarmotError
marmot_update_group_avatar_url(Marmot *m, const MarmotGroupId *gid,
                               const MarmotGroupAvatarUrl *avatar,
                               char **out_commit_json)
{
    MarmotGroupAvatarUrl empty = {0};
    uint8_t *state = NULL;
    size_t state_len = 0;
    MarmotError err = marmot_group_avatar_url_encode(avatar ? avatar : &empty,
                                                     &state, &state_len);
    if (err != MARMOT_OK) return err;
    err = update_image_component_txn(m, gid, MARMOT_COMPONENT_GROUP_AVATAR_URL_V1,
                                     state, state_len, out_commit_json);
    free(state);
    return err;
}

static MarmotError
add_members_impl(Marmot *m,
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
    if (mls.profile == MARMOT_GROUP_PROFILE_ADOPTED) {   /* nostrc-qp24.5.1.3 */
        err = add_members_adopted(m, group, &mls, key_package_event_jsons, kp_count,
                                  out_welcome_jsons, out_welcome_count, out_commit_json);
        mls_group_free(&mls);
        marmot_group_free(group);
        return err;
    }

    MlsKeyPackage *kps = NULL;
    MlsGroup post;
    memset(&post, 0, sizeof(post));
    MlsAddResult add;
    memset(&add, 0, sizeof(add));
    uint8_t *reencoded = NULL;
    size_t reencoded_len = 0;
    char *commit_json = NULL;
    char **welcomes = calloc(kp_count, sizeof(char *));
    uint8_t (*recipients)[32] = calloc(kp_count, 32);
    MarmotUnsentWelcome *outbox = calloc(kp_count, sizeof(*outbox));
    if (!welcomes || !recipients || !outbox) {
        err = MARMOT_ERR_MEMORY;
        goto fail;
    }
    err = parse_key_packages(m, key_package_event_jsons, kp_count, &kps, recipients);
    if (err != MARMOT_OK) goto fail;
    uint8_t sender[32];
    if (get_own_credential_identity(&mls, sender) != 0) {
        err = MARMOT_ERR_OWN_LEAF_NOT_FOUND;
        goto fail;
    }

    /* Every KeyPackage in one Commit (nostrc-wc6v), with the GroupData
     * re-encoded as MIP-01 if it is still in the 0.10.0 layout. */
    err = legacy_group_data_reencoded(&mls, group->nostr_group_id, &reencoded,
                                      &reencoded_len);
    if (err != MARMOT_OK) goto fail;
    /* The Add bringing a group created alone its second member requires
     * SelfRemove too, in the same Commit (nostrc-8ndz). */
    if (first_add_requires_self_remove(m, &mls, kps, kp_count)) {
        uint8_t *with_sr = NULL;
        size_t with_sr_len = 0;
        if (exts_with_self_remove_required(reencoded ? reencoded : mls.extensions_data,
                                           reencoded ? reencoded_len : mls.extensions_len,
                                           &with_sr, &with_sr_len) != 0) {
            err = MARMOT_ERR_EXTENSION_FORMAT;
            goto fail;
        }
        free(reencoded);
        reencoded = with_sr;
        reencoded_len = with_sr_len;
    }
    if (clone_mls_group(&mls, &post) != 0) {
        err = MARMOT_ERR_MLS;
        goto fail;
    }
    int rc = add_key_packages(&post, kps, kp_count, reencoded, reencoded_len, &add);
    if (rc != 0) {
        /* nostrc-zbmb: an invitee whose app cannot join what the group
         * requires is named as such. */
        err = rc == MARMOT_ERR_MEMORY ? MARMOT_ERR_MEMORY
              : rc == MARMOT_ERR_KEY_PACKAGE_CAPABILITIES ? MARMOT_ERR_KEY_PACKAGE_CAPABILITIES
              : MARMOT_ERR_MLS;
        goto fail;
    }
    /* Never publish an Add whose Welcome the joiners must reject (review
     * W20 B1): they accept an unproven leaf only as ours, the sender's.
     * Otherwise the joiner's leaf would stay in the tree as a ghost. */
    err = marmot_tree_members_bound(&post, UINT32_MAX, m->config.allow_unproven_members);
    if (err != MARMOT_OK) goto fail;
    /* MIP-02: the rumor's relays tag names the group's relays, from its
     * marmot_group_data. Before 0.11.0 an Add's Welcome had none, and MDK 0.8
     * refuses such a Welcome (nostrc-7gx7). A group without the extension
     * (legacy) still gets none. */
    MarmotGroupDataExtension *gde = NULL;
    {
        const uint8_t *data = NULL;
        size_t data_len = 0, n_gde = 0;
        if (marmot_extensions_find(post.extensions_data, post.extensions_len,
                                   MARMOT_EXTENSION_TYPE, &data, &data_len, &n_gde) == 0 &&
            n_gde == 1)   /* MIP-01 now, re-encoded above if it was not */
            gde = marmot_group_data_extension_deserialize(data, data_len);
    }
    int64_t created_at = 0;
    err = local_commit_precheck(m, &mls, &post, NULL);   /* nostrc-u9kv */
    if (err == MARMOT_OK)
        err = marmot_next_group_event_time(m, group->nostr_group_id, true, &created_at);
    if (err == MARMOT_OK)
        err = build_welcome_rumors(&add, sender, key_package_event_jsons, kp_count,
                                   group->nostr_group_id, group->name, group->description,
                                   (const uint8_t (*)[32])group->admin_pubkeys,
                                   group->admin_count, post.tree.n_leaves,
                                   gde ? (const char **)gde->relays : NULL,
                                   gde ? gde->relay_count : 0, created_at, welcomes);
    marmot_group_data_extension_free(gde);
    if (err != MARMOT_OK) goto fail;
    for (size_t i = 0; i < kp_count; i++) {
        memcpy(outbox[i].recipient, recipients[i], 32);
        outbox[i].rumor_json = welcomes[i];   /* borrowed */
    }
    err = finish_local_commit(m, group, &mls, &post, add.commit_data, add.commit_len,
                              created_at, outbox, kp_count, &commit_json);
    if (err != MARMOT_OK) goto fail;

    free(recipients);
    free(outbox);
    free(reencoded);
    free_key_packages(kps, kp_count);
    mls_add_result_clear(&add);
    mls_group_free(&post);
    mls_group_free(&mls);
    marmot_group_free(group);
    *out_welcome_jsons = welcomes;
    *out_welcome_count = kp_count;
    *out_commit_json = commit_json;
    return MARMOT_OK;

fail:
    free(recipients);
    free(outbox);
    free(reencoded);
    free_key_packages(kps, kp_count);
    mls_add_result_clear(&add);
    if (welcomes) {
        for (size_t j = 0; j < kp_count; j++) free(welcomes[j]);
        free(welcomes);
    }
    mls_group_free(&post);
    mls_group_free(&mls);
    marmot_group_free(group);
    return err;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_remove_members
 * ──────────────────────────────────────────────────────────────────────── */

static MarmotError
remove_members_impl(Marmot *m,
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

    MlsGroup post;
    memset(&post, 0, sizeof(post));
    MlsCommitResult result;
    memset(&result, 0, sizeof(result));
    uint32_t *leaves = calloc(count, sizeof(uint32_t));
    if (!leaves) {
        err = MARMOT_ERR_MEMORY;
        goto out;
    }
    for (size_t i = 0; i < count; i++) {
        if (find_leaf_by_pubkey(&mls, member_pubkeys[i], &leaves[i]) != 0) {
            err = MARMOT_ERR_MEMBER_NOT_FOUND;
            goto out;
        }
    }
    if (mls.profile == MARMOT_GROUP_PROFILE_ADOPTED) {   /* nostrc-qp24.5.1.3 */
        err = remove_members_adopted(m, group, &mls, leaves, count, out_commit_json);
        goto out;
    }
    /* Every member in one Commit (nostrc-wc6v). */
    if (clone_mls_group(&mls, &post) != 0) {
        err = MARMOT_ERR_MLS;
        goto out;
    }
    int rc = mls_group_remove_members(&post, leaves, count, &result);
    if (rc != 0) {
        err = rc == MARMOT_ERR_INVALID_ARG ? MARMOT_ERR_INVALID_ARG : MARMOT_ERR_MLS;
        goto out;
    }
    err = finish_local_commit_now(m, group, &mls, &post, result.commit_data,
                                  result.commit_len, out_commit_json);
out:
    free(leaves);
    mls_commit_result_clear(&result);
    mls_group_free(&post);
    mls_group_free(&mls);
    marmot_group_free(group);
    return err;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_commit_pending_proposals (nostrc-2um6)
 * ──────────────────────────────────────────────────────────────────────── */

static MarmotError
commit_pending_proposals_impl(Marmot *m, const MarmotGroupId *mls_group_id,
                              char **out_commit_json)
{
    if (!m || !mls_group_id || !out_commit_json) return MARMOT_ERR_INVALID_ARG;
    *out_commit_json = NULL;
    MarmotGroup *group = NULL;
    MlsGroup mls;
    /* Not admin-only: a SelfRemove-only Commit is any member's
     * (proposals.c); a member's own Remove needs an admin, which
     * marmot_proposals_select() checks. */
    MarmotError err = load_group_for_commit_ex(m, mls_group_id, &group, &mls, false);
    if (err != MARMOT_OK) return err;
    /* A group of neither profile commits nothing (W24 slice E); adopted
     * groups commit SelfRemoves as any member may (nostrc-qp24.5.1.3). */
    if (mls.profile != MARMOT_GROUP_PROFILE_LEGACY &&
        mls.profile != MARMOT_GROUP_PROFILE_ADOPTED) {
        mls_group_free(&mls);
        marmot_group_free(group);
        return MARMOT_ERR_UNSUPPORTED;
    }
    MlsGroup post;
    memset(&post, 0, sizeof(post));
    MlsCommitResult result;
    memset(&result, 0, sizeof(result));
    MarmotProposalSet set;
    size_t *pick = NULL, n = 0;
    const uint8_t **acs = NULL;
    size_t *lens = NULL;
    err = marmot_proposals_load(m, mls_group_id->data, mls_group_id->len, &set);
    if (err == MARMOT_OK) err = marmot_proposals_select(&mls, &set, &pick, &n);
    if (err != MARMOT_OK || n == 0) goto out;   /* nothing to commit */
    acs = calloc(n, sizeof(*acs));
    lens = calloc(n, sizeof(*lens));
    if (!acs || !lens) {
        err = MARMOT_ERR_MEMORY;
        goto out;
    }
    for (size_t i = 0; i < n; i++) {
        acs[i] = set.items[pick[i]].ac;
        lens[i] = set.items[pick[i]].ac_len;
    }
    if (clone_mls_group(&mls, &post) != 0) {
        err = MARMOT_ERR_MLS;
        goto out;
    }
    int rc = mls_group_commit_by_ref(&post, acs, lens, n, &result);
    if (rc != 0) {
        err = rc == MARMOT_ERR_INVALID_ARG ? MARMOT_ERR_INVALID_ARG : MARMOT_ERR_MLS;
        goto out;
    }
    /* What the Commit does with departures, for the authorization of the
     * Commit (now and at merge), as receivers will see it. */
    MlsCommitSummary departures;
    memset(&departures, 0, sizeof(departures));
    departures.proposal_count = n;
    for (size_t i = 0; i < n; i++) {
        const MarmotStoredProposal *p = &set.items[pick[i]];
        if (p->type == MLS_PROPOSAL_SELF_REMOVE)
            departures.self_removed[departures.self_remove_count++] = p->sender_leaf;
        else
            departures.left[departures.left_count++] = p->target_leaf;
    }
    /* The group's policy first (nostrc-u9kv), then dated after every event
     * of ours in the group, or refused with MARMOT_ERR_EVENT_RATE (retry in
     * a second; nostrc-2lrz). */
    int64_t created_at = 0;
    err = local_commit_precheck(m, &mls, &post, &departures);
    if (err == MARMOT_OK)
        err = marmot_next_group_event_time(m, group->nostr_group_id, true, &created_at);
    if (err != MARMOT_OK) goto out;
    char *json = marmot_commit_build_event(result.commit_data, result.commit_len,
                                           mls.epoch_secrets.exporter_secret,
                                           group->nostr_group_id, created_at);
    if (!json) {
        err = MARMOT_ERR_EVENT_BUILD;
        goto out;
    }
    err = marmot_commit_stage_pending_ex(m, &mls, &post, result.commit_data, result.commit_len,
                                         json, NULL, 0, &departures);
    if (err != MARMOT_OK) free(json);
    else *out_commit_json = json;
out:
    free(acs);
    free(lens);
    free(pick);
    marmot_proposals_clear(&set);
    mls_commit_result_clear(&result);
    mls_group_free(&post);
    mls_group_free(&mls);
    marmot_group_free(group);
    return err;
}

MarmotError
marmot_commit_pending_proposals(Marmot *m, const MarmotGroupId *mls_group_id,
                                char **out_commit_json)
{
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    err = commit_pending_proposals_impl(m, mls_group_id, out_commit_json);
    MarmotError end = marmot_txn_end(m, err);
    if (err == MARMOT_OK && end != MARMOT_OK) {
        free(*out_commit_json);
        *out_commit_json = NULL;
    }
    return end;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_leave_group
 * ──────────────────────────────────────────────────────────────────────── */

static MarmotError
leave_group_impl(Marmot *m, const MarmotGroupId *mls_group_id)
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
 * list with `gde_bytes`, keeping every other extension and the order. A list
 * without required_capabilities (a group of libmarmot <= 0.10.0) gains
 * MIP-01's: the GroupContextExtensions proposal carrying this list must
 * list 0xF2EE there (see write_required_capabilities()). */
static int
replace_group_data_extension(const uint8_t *exts, size_t exts_len,
                             const uint8_t *gde_bytes, size_t gde_len,
                             uint8_t **out, size_t *out_len)
{
    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, exts_len + gde_len + 8) != 0) return -1;
    MlsTlsReader r;
    mls_tls_reader_init(&r, exts, exts_len);
    bool replaced = false, has_required = false;
    while (!mls_tls_reader_done(&r)) {
        size_t entry_start = r.pos;
        uint16_t type;
        size_t len;
        if (mls_tls_read_u16(&r, &type) != 0 || mls_tls_read_vli(&r, &len) != 0 ||
            len > mls_tls_reader_remaining(&r))
            goto fail;
        r.pos += len;
        if (type == MLS_EXTENSION_REQUIRED_CAPABILITIES) has_required = true;
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
    if (!has_required && write_required_capabilities(&buf, false) != 0) goto fail;
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
        gde = marmot_group_data_extension_deserialize_stored(cur, cur_len);
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

static MarmotError
update_group_metadata_impl(Marmot *m,
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
    if (mls.profile == MARMOT_GROUP_PROFILE_ADOPTED) {   /* nostrc-qp24.5.1.3 */
        err = update_metadata_adopted(m, group, &mls, config, out_commit_json);
        mls_group_free(&mls);
        marmot_group_free(group);
        return err;
    }

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
    err = finish_local_commit_now(m, group, &pre, &mls, commit_result.commit_data,
                                  commit_result.commit_len, out_commit_json);
out:
    free(new_ext);
    mls_commit_result_clear(&commit_result);
    mls_group_free(&pre);
    mls_group_free(&mls);
    marmot_group_free(group);
    return err;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: requiring SelfRemove in an existing group (nostrc-8ndz)
 *
 * A legacy group created with no invitee, or with one whose KeyPackage
 * lacked SelfRemove, does not require it (write_required_capabilities():
 * MDK 0.8's LCD rule), so its members leave by a Remove of themselves that
 * only an admin commits.  Once every member's leaf advertises SelfRemove an
 * admin may add it to required_capabilities by a GroupContextExtensions
 * Commit (RFC 9420 §12.1.7; changing the GroupContext is privileged in
 * MIP-01, commits.c), as MDK 0.11's upgrade_group_capabilities() does.
 * Then any member commits a leave.  The cost is MDK's at creation: a later
 * invitee's KeyPackage must advertise SelfRemove too.
 * ──────────────────────────────────────────────────────────────────────── */

/* `exts` (a legacy GroupContext's extension list) with SelfRemove added to
 * required_capabilities' proposal_types in ascending order, every other
 * byte kept; a list without required_capabilities gains MIP-01's, with
 * SelfRemove. */
static int
exts_with_self_remove_required(const uint8_t *exts, size_t exts_len,
                               uint8_t **out, size_t *out_len)
{
    MlsTlsBuf buf, rc = {0};
    if (mls_tls_buf_init(&buf, exts_len + 16) != 0) return -1;
    MlsTlsReader r;
    mls_tls_reader_init(&r, exts, exts_len);
    bool found = false;
    while (!mls_tls_reader_done(&r)) {
        size_t entry_start = r.pos;
        uint16_t type;
        size_t len;
        if (mls_tls_read_u16(&r, &type) != 0 || mls_tls_read_vli(&r, &len) != 0 ||
            len > mls_tls_reader_remaining(&r))
            goto fail;
        const uint8_t *data = exts + r.pos;
        r.pos += len;
        if (type != MLS_EXTENSION_REQUIRED_CAPABILITIES) {
            if (mls_tls_buf_append(&buf, exts + entry_start, r.pos - entry_start) != 0)
                goto fail;
            continue;
        }
        if (found) goto fail;   /* two required_capabilities */
        found = true;
        /* { extension_types<V>; proposal_types<V>; credential_types<V> } */
        MlsTlsReader in;
        mls_tls_reader_init(&in, data, len);
        size_t ext_bytes, prop_bytes;
        if (mls_tls_read_vli(&in, &ext_bytes) != 0 ||
            ext_bytes > mls_tls_reader_remaining(&in))
            goto fail;
        const uint8_t *ext_types = data + in.pos;
        in.pos += ext_bytes;
        if (mls_tls_read_vli(&in, &prop_bytes) != 0 || prop_bytes % 2 != 0 ||
            prop_bytes > mls_tls_reader_remaining(&in))
            goto fail;
        const uint8_t *props = data + in.pos;
        in.pos += prop_bytes;
        bool has = false;
        size_t at = prop_bytes;   /* where 0x000a goes */
        for (size_t k = 0; k < prop_bytes; k += 2) {
            uint16_t t = (uint16_t)((props[k] << 8) | props[k + 1]);
            if (t == MLS_PROPOSAL_SELF_REMOVE) has = true;
            if (t > MLS_PROPOSAL_SELF_REMOVE && at == prop_bytes) at = k;
        }
        static const uint8_t sr[2] = { (uint8_t)(MLS_PROPOSAL_SELF_REMOVE >> 8),
                                       (uint8_t)(MLS_PROPOSAL_SELF_REMOVE & 0xff) };
        if (mls_tls_buf_init(&rc, len + 4) != 0 ||
            mls_tls_write_vli(&rc, ext_bytes) != 0 ||
            mls_tls_buf_append(&rc, ext_types, ext_bytes) != 0 ||
            mls_tls_write_vli(&rc, prop_bytes + (has ? 0 : 2)) != 0 ||
            mls_tls_buf_append(&rc, props, has ? prop_bytes : at) != 0 ||
            (!has && (mls_tls_buf_append(&rc, sr, 2) != 0 ||
                      mls_tls_buf_append(&rc, props + at, prop_bytes - at) != 0)) ||
            mls_tls_buf_append(&rc, data + in.pos, len - in.pos) != 0 ||   /* credentials */
            mls_tls_write_u16(&buf, MLS_EXTENSION_REQUIRED_CAPABILITIES) != 0 ||
            mls_tls_write_opaque16(&buf, rc.data, rc.len) != 0)
            goto fail;
        mls_tls_buf_free(&rc);
    }
    if (!found && write_required_capabilities(&buf, true) != 0) goto fail;
    *out = buf.data;
    *out_len = buf.len;
    return 0;
fail:
    mls_tls_buf_free(&rc);
    mls_tls_buf_free(&buf);
    return -1;
}

MarmotError
marmot_get_self_remove_requirement(Marmot *m, const MarmotGroupId *mls_group_id,
                                   bool *out_required, bool *out_upgradable)
{
    if (!m || !mls_group_id || !mls_group_id->data || !out_required)
        return MARMOT_ERR_INVALID_ARG;
    *out_required = false;
    if (out_upgradable) *out_upgradable = false;
    MlsGroup mls;
    if (load_mls_group(m, mls_group_id, &mls) != 0) return MARMOT_ERR_GROUP_NOT_FOUND;
    *out_required = mls_group_requires_proposal(&mls, MLS_PROPOSAL_SELF_REMOVE);
    if (out_upgradable)
        *out_upgradable = mls.profile != MARMOT_GROUP_PROFILE_ADOPTED && !*out_required &&
                          mls_group_members_support_proposal(&mls, MLS_PROPOSAL_SELF_REMOVE);
    mls_group_free(&mls);
    return MARMOT_OK;
}

static MarmotError
require_self_remove_impl(Marmot *m, const MarmotGroupId *mls_group_id, char **out_commit_json)
{
    if (!m || !mls_group_id || !out_commit_json) return MARMOT_ERR_INVALID_ARG;
    *out_commit_json = NULL;
    MarmotGroup *group = NULL;
    MlsGroup mls;
    MarmotError err = load_group_for_commit(m, mls_group_id, &group, &mls);
    if (err != MARMOT_OK) return err;
    MlsGroup pre;
    memset(&pre, 0, sizeof(pre));
    MlsCommitResult res;
    memset(&res, 0, sizeof(res));
    uint8_t *exts = NULL;
    size_t exts_len = 0;
    if (mls.profile == MARMOT_GROUP_PROFILE_ADOPTED) {
        err = MARMOT_ERR_UNSUPPORTED;   /* see marmot.h */
        goto out;
    }
    if (mls_group_requires_proposal(&mls, MLS_PROPOSAL_SELF_REMOVE)) {
        err = MARMOT_ERR_VALIDATION;    /* nothing to commit */
        goto out;
    }
    /* A member whose leaf lacks it could no longer stay (RFC 9420 §11.1);
     * the MLS layer checks every leaf again. */
    if (!mls_group_members_support_proposal(&mls, MLS_PROPOSAL_SELF_REMOVE)) {
        err = MARMOT_ERR_UNSUPPORTED;
        goto out;
    }
    if (exts_with_self_remove_required(mls.extensions_data, mls.extensions_len, &exts,
                                       &exts_len) != 0) {
        err = MARMOT_ERR_EXTENSION_FORMAT;
        goto out;
    }
    if (clone_mls_group(&mls, &pre) != 0) {
        err = MARMOT_ERR_MLS;
        goto out;
    }
    int rc = mls_group_commit_extensions(&mls, exts, exts_len, &res);
    if (rc != 0) {
        err = rc == MARMOT_ERR_UNSUPPORTED ? MARMOT_ERR_UNSUPPORTED : MARMOT_ERR_MLS;
        goto out;
    }
    err = finish_local_commit_now(m, group, &pre, &mls, res.commit_data, res.commit_len,
                                  out_commit_json);
out:
    free(exts);
    mls_commit_result_clear(&res);
    mls_group_free(&pre);
    mls_group_free(&mls);
    marmot_group_free(group);
    return err;
}

MarmotError
marmot_require_self_remove(Marmot *m, const MarmotGroupId *mls_group_id,
                           char **out_commit_json)
{
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    err = require_self_remove_impl(m, mls_group_id, out_commit_json);
    MarmotError end = marmot_txn_end(m, err);
    if (err == MARMOT_OK && end != MARMOT_OK) {
        free(*out_commit_json);
        *out_commit_json = NULL;
    }
    return end;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: self-update, optionally adding the account proof to our leaf
 * (nostrc-rgb5, nostrc-yd0q)
 * ──────────────────────────────────────────────────────────────────────── */

/* Our leaf in `mls`: its account and signature key. */
static int
own_leaf_binding(const MlsGroup *mls, uint8_t account[32], uint8_t sig_key[MLS_SIG_PK_LEN])
{
    if (get_own_credential_identity(mls, account) != 0) return -1;
    memcpy(sig_key, mls->tree.nodes[mls_tree_leaf_to_node(mls->own_leaf_index)].leaf.signature_key,
           MLS_SIG_PK_LEN);
    return 0;
}

static MarmotError
group_account_proof_template_impl(Marmot *m, const MarmotGroupId *mls_group_id,
                                  char **out_unsigned_event_json)
{
    if (!m || !mls_group_id || !out_unsigned_event_json) return MARMOT_ERR_INVALID_ARG;
    *out_unsigned_event_json = NULL;
    MlsGroup mls;
    if (load_mls_group(m, mls_group_id, &mls) != 0) return MARMOT_ERR_GROUP_NOT_FOUND;
    uint8_t account[32], sig_key[MLS_SIG_PK_LEN];
    MarmotError err = MARMOT_ERR_OWN_LEAF_NOT_FOUND;
    if (own_leaf_binding(&mls, account, sig_key) == 0) {
        int64_t now = marmot_now();
        *out_unsigned_event_json = marmot_account_proof_template_json(
            account, (uint64_t)(now > 0 ? now : 1), sig_key, MLS_SIG_PK_LEN);
        err = *out_unsigned_event_json ? MARMOT_OK : MARMOT_ERR_MEMORY;
    }
    mls_group_free(&mls);
    return err;
}

MarmotError
marmot_group_account_proof_template(Marmot *m, const MarmotGroupId *mls_group_id,
                                    char **out_unsigned_event_json)
{
    return group_account_proof_template_impl(m, mls_group_id, out_unsigned_event_json);
}

/* Our adopted leaf's LeafNode extensions with its account proof (0x8009)
 * replaced by `proof`, every other entry (the components it advertises)
 * kept: a renewed proof, never a changed capability (nostrc-qp24.5.1.3). */
static MarmotError
adopted_leaf_ext_with_proof(const MlsGroup *mls, const uint8_t proof[MARMOT_ACCOUNT_PROOF_LEN],
                            uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    const MlsLeafNode *leaf = &mls->tree.nodes[mls_tree_leaf_to_node(mls->own_leaf_index)].leaf;
    const uint8_t *dict = NULL;
    size_t dict_len = 0, count = 0;
    MarmotComponentData *entries = NULL;
    size_t n = 0;
    if (marmot_extensions_find(leaf->extensions_data, leaf->extensions_len,
                               MARMOT_EXT_APP_DATA_DICTIONARY, &dict, &dict_len, &count) != 0 ||
        count != 1 || marmot_app_data_dict_parse(dict, dict_len, &entries, &n) != 0)
        return MARMOT_ERR_EXTENSION_FORMAT;
    MarmotError err = MARMOT_ERR_EXTENSION_FORMAT;
    for (size_t i = 0; i < n; i++)
        if (entries[i].component_id == MARMOT_COMPONENT_ACCOUNT_PROOF_V2) {
            entries[i].data = proof;
            entries[i].len = MARMOT_ACCOUNT_PROOF_LEN;
            err = MARMOT_OK;
        }
    MlsTlsBuf d = {0}, exts = {0};
    if (err == MARMOT_OK &&
        (mls_tls_buf_init(&d, dict_len + 16) != 0 || mls_tls_buf_init(&exts, dict_len + 32) != 0))
        err = MARMOT_ERR_MEMORY;
    if (err == MARMOT_OK &&
        (marmot_app_data_dict_encode(entries, n, &d) != 0 ||
         mls_tls_write_u16(&exts, MARMOT_EXT_APP_DATA_DICTIONARY) != 0 ||
         mls_tls_write_opaque32(&exts, d.data, d.len) != 0))
        err = MARMOT_ERR_SERIALIZATION;
    if (err == MARMOT_OK) {
        *out = exts.data;
        *out_len = exts.len;
        exts.data = NULL;
    }
    mls_tls_buf_free(&d);
    mls_tls_buf_free(&exts);
    free(entries);
    return err;
}

static MarmotError
self_update_impl(Marmot *m, const MarmotGroupId *mls_group_id, const char *signed_proof_json,
                 char **out_commit_json)
{
    if (!m || !mls_group_id || !out_commit_json) return MARMOT_ERR_INVALID_ARG;
    *out_commit_json = NULL;
    MarmotGroup *group = NULL;
    MlsGroup mls;
    MarmotError err = load_group_for_commit_ex(m, mls_group_id, &group, &mls, false);
    if (err != MARMOT_OK) return err;

    MlsGroup pre;
    memset(&pre, 0, sizeof(pre));
    MlsCommitResult res;
    memset(&res, 0, sizeof(res));
    uint8_t *leaf_ext = NULL;
    size_t leaf_ext_len = 0;
    if (signed_proof_json) {
        /* The proof must bind exactly our leaf: our account, our leaf key. */
        uint8_t account[32], sig_key[MLS_SIG_PK_LEN], proof[MARMOT_ACCOUNT_PROOF_LEN];
        err = own_leaf_binding(&mls, account, sig_key) == 0
                  ? marmot_account_proof_from_signed(account, sig_key, MLS_SIG_PK_LEN,
                                                     signed_proof_json, proof)
                  : MARMOT_ERR_OWN_LEAF_NOT_FOUND;
        if (err == MARMOT_OK)
            err = mls.profile == MARMOT_GROUP_PROFILE_ADOPTED
                      ? adopted_leaf_ext_with_proof(&mls, proof, &leaf_ext, &leaf_ext_len)
                      : marmot_leaf_proof_extensions(proof, &leaf_ext, &leaf_ext_len);
        sodium_memzero(proof, sizeof(proof));
        if (err != MARMOT_OK) goto out;
    }
    if (clone_mls_group(&mls, &pre) != 0) {
        err = MARMOT_ERR_MLS;
        goto out;
    }
    int rc = signed_proof_json
                 ? mls_group_self_update_with_leaf_extensions(&mls, leaf_ext, leaf_ext_len, &res)
                 : mls_group_self_update(&mls, &res);
    if (rc != 0) {
        err = rc == MARMOT_ERR_MEMORY ? MARMOT_ERR_MEMORY : MARMOT_ERR_MLS;
        goto out;
    }
    err = finish_local_commit_now(m, group, &pre, &mls, res.commit_data, res.commit_len,
                                  out_commit_json);
out:
    free(leaf_ext);
    mls_commit_result_clear(&res);
    mls_group_free(&pre);
    mls_group_free(&mls);
    marmot_group_free(group);
    return err;
}

MarmotError
marmot_self_update(Marmot *m, const MarmotGroupId *mls_group_id, const char *signed_proof_json,
                   char **out_commit_json)
{
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    err = self_update_impl(m, mls_group_id, signed_proof_json, out_commit_json);
    MarmotError end = marmot_txn_end(m, err);
    if (err == MARMOT_OK && end != MARMOT_OK) {
        free(*out_commit_json);
        *out_commit_json = NULL;
    }
    return end;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: one storage transaction per operation (nostrc-qp24.7)
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_create_group(Marmot *m,
                     const uint8_t creator_pubkey[32],
                     const char **key_package_event_jsons, size_t kp_count,
                     const MarmotGroupConfig *config,
                     MarmotCreateGroupResult *result)
{
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    err = create_group_impl(m, creator_pubkey, key_package_event_jsons, kp_count,
                            config, result);
    MarmotError end = marmot_txn_end(m, err);
    if (err == MARMOT_OK && end != MARMOT_OK)
        marmot_create_group_result_free(result);   /* rolled back */
    return end;
}

MarmotError
marmot_create_group_for_profile(Marmot *m, MarmotGroupProfile profile,
                                const uint8_t creator_pubkey[32], const uint8_t creator_sk[32],
                                MarmotAccountSignFunc account_sign, void *sign_data,
                                const char **key_package_event_jsons, size_t kp_count,
                                const MarmotGroupConfig *config,
                                MarmotCreateGroupResult *result)
{
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    err = create_group_for_profile_impl(m, profile, creator_pubkey, creator_sk, account_sign,
                                        sign_data, key_package_event_jsons, kp_count, config,
                                        result);
    MarmotError end = marmot_txn_end(m, err);
    if (err == MARMOT_OK && end != MARMOT_OK)
        marmot_create_group_result_free(result);   /* rolled back */
    return end;
}

MarmotError
marmot_get_group_profile(Marmot *m, const MarmotGroupId *mls_group_id,
                         MarmotGroupProfile *out_profile)
{
    return marmot_adopted_stored_profile(m, mls_group_id, out_profile);
}

MarmotError
marmot_merge_pending_commit(Marmot *m, const MarmotGroupId *mls_group_id)
{
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    return marmot_txn_end(m, merge_pending_commit_impl(m, mls_group_id));
}

MarmotError
marmot_clear_pending_commit(Marmot *m, const MarmotGroupId *mls_group_id)
{
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    return marmot_txn_end(m, clear_pending_commit_impl(m, mls_group_id));
}

/* It may finish a Commit that was merged before a crash: a write. */
MarmotError
marmot_get_pending_commit(Marmot *m, const MarmotGroupId *mls_group_id,
                          char **out_event_json, bool *out_superseded)
{
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    err = get_pending_commit_impl(m, mls_group_id, out_event_json, out_superseded);
    MarmotError end = marmot_txn_end(m, err);
    if (err == MARMOT_OK && end != MARMOT_OK) {
        free(*out_event_json);
        *out_event_json = NULL;
        if (out_superseded) *out_superseded = false;
    }
    return end;
}

MarmotError
marmot_get_unsent_welcomes(Marmot *m, const MarmotGroupId *mls_group_id,
                           MarmotUnsentWelcome **out_welcomes, size_t *out_count)
{
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    err = get_unsent_welcomes_impl(m, mls_group_id, out_welcomes, out_count);
    MarmotError end = marmot_txn_end(m, err);
    if (err == MARMOT_OK && end != MARMOT_OK) {
        marmot_unsent_welcomes_free(*out_welcomes, *out_count);
        *out_welcomes = NULL;
        *out_count = 0;
    }
    return end;
}

MarmotError
marmot_mark_welcomes_sent(Marmot *m, const MarmotGroupId *mls_group_id,
                          const uint8_t (*ids)[32], size_t count)
{
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    return marmot_txn_end(m, mark_welcomes_sent_impl(m, mls_group_id, ids, count));
}

MarmotError
marmot_add_members(Marmot *m,
                    const MarmotGroupId *mls_group_id,
                    const char **key_package_event_jsons, size_t kp_count,
                    char ***out_welcome_jsons, size_t *out_welcome_count,
                    char **out_commit_json)
{
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    err = add_members_impl(m, mls_group_id, key_package_event_jsons, kp_count,
                           out_welcome_jsons, out_welcome_count, out_commit_json);
    MarmotError end = marmot_txn_end(m, err);
    if (err == MARMOT_OK && end != MARMOT_OK) {
        for (size_t i = 0; i < *out_welcome_count; i++) free((*out_welcome_jsons)[i]);
        free(*out_welcome_jsons);
        free(*out_commit_json);
        *out_welcome_jsons = NULL;
        *out_welcome_count = 0;
        *out_commit_json = NULL;
    }
    return end;
}

MarmotError
marmot_remove_members(Marmot *m,
                       const MarmotGroupId *mls_group_id,
                       const uint8_t (*member_pubkeys)[32], size_t count,
                       char **out_commit_json)
{
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    err = remove_members_impl(m, mls_group_id, member_pubkeys, count, out_commit_json);
    MarmotError end = marmot_txn_end(m, err);
    if (err == MARMOT_OK && end != MARMOT_OK) {
        free(*out_commit_json);
        *out_commit_json = NULL;
    }
    return end;
}

MarmotError
marmot_leave_group(Marmot *m, const MarmotGroupId *mls_group_id)
{
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    return marmot_txn_end(m, leave_group_impl(m, mls_group_id));
}

MarmotError
marmot_update_group_metadata(Marmot *m,
                              const MarmotGroupId *mls_group_id,
                              const MarmotGroupConfig *config,
                              char **out_commit_json)
{
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    err = update_group_metadata_impl(m, mls_group_id, config, out_commit_json);
    MarmotError end = marmot_txn_end(m, err);
    if (err == MARMOT_OK && end != MARMOT_OK) {
        free(*out_commit_json);
        *out_commit_json = NULL;
    }
    return end;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: marmot_get_group_members
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_get_group_members(Marmot *m, const MarmotGroupId *mls_group_id,
                         uint8_t (**out_members)[32], size_t *out_count)
{
    if (!m || !mls_group_id || !mls_group_id->data || !out_members || !out_count)
        return MARMOT_ERR_INVALID_ARG;
    *out_members = NULL;
    *out_count = 0;

    MlsGroup mls;
    memset(&mls, 0, sizeof(mls));
    if (load_mls_group(m, mls_group_id, &mls) != 0)
        return MARMOT_ERR_GROUP_NOT_FOUND;

    uint8_t (*members)[32] = mls.tree.n_leaves ? calloc(mls.tree.n_leaves, 32) : NULL;
    if (mls.tree.n_leaves && !members) {
        mls_group_free(&mls);
        return MARMOT_ERR_MEMORY;
    }
    size_t n = 0;
    for (uint32_t i = 0; i < mls.tree.n_leaves; i++) {
        uint32_t node_idx = mls_tree_leaf_to_node(i);
        if (node_idx >= mls.tree.n_nodes) continue;
        const MlsNode *node = &mls.tree.nodes[node_idx];
        /* A blank leaf is a removed member's slot. */
        if (node->type != MLS_NODE_LEAF || node->leaf.credential_identity_len != 32 ||
            !node->leaf.credential_identity)
            continue;
        bool seen = false;
        for (size_t j = 0; j < n && !seen; j++)
            seen = memcmp(members[j], node->leaf.credential_identity, 32) == 0;
        if (!seen)
            memcpy(members[n++], node->leaf.credential_identity, 32);
    }
    mls_group_free(&mls);
    if (n == 0) {
        free(members);
        members = NULL;
    }
    *out_members = members;
    *out_count = n;
    return MARMOT_OK;
}
