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
#include "adopted.h"
#include "kp_profile.h"
#include "kp_lifecycle.h"
#include "commits.h"
#include "members.h"
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

static bool
is_lower_hex64(const char *s)
{
    if (!s || strlen(s) != 64) return false;
    for (size_t i = 0; i < 64; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}

/* The MLS bytes of a kind:444 rumor's content: base64 when it is tagged
 * `encoding` = `base64` (MDK 0.8 profile); with no `encoding` tag, base64 of
 * an MLSMessage(mls_welcome) -- the adopted Nostr binding (transports/
 * nostr.md "Welcome delivery", MDK 0.11), *adopted set -- or else hex (a
 * pre-encoding rumor).  The two cannot be confused: base64 of an MLSMessage
 * starts "AAEAAw", which is not hex (nostrc-qp24.5.1). */
static uint8_t *
decode_welcome_content(const char *content, NostrTags *tags, size_t *out_len, bool *adopted)
{
    *adopted = false;
    bool has_encoding = false, is_base64 = false;
    for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (nostr_tag_size(tag) >= 1 && strcmp(nostr_tag_get_key(tag), "encoding") == 0) {
            has_encoding = true;
            is_base64 = nostr_tag_size(tag) >= 2 &&
                        strcmp(nostr_tag_get_value(tag), "base64") == 0;
            break;
        }
    }
    if (is_base64) return base64_decode(content, out_len);
    if (!has_encoding) {
        size_t len = 0;
        uint8_t *data = base64_decode(content, &len);
        if (data && len >= 4 && data[0] == 0x00 && data[1] == 0x01 && data[2] == 0x00 &&
            data[3] == MLS_WIRE_FORMAT_WELCOME) {
            *adopted = true;
            *out_len = len;
            return data;
        }
        free(data);
    }
    /* Hex decode (deprecated) */
    size_t hex_len = strlen(content);
    if (hex_len % 2 != 0) return NULL;
    uint8_t *data = malloc(hex_len / 2 ? hex_len / 2 : 1);
    if (data && marmot_hex_decode(content, data, hex_len / 2) != 0) {
        free(data);
        return NULL;
    }
    *out_len = hex_len / 2;
    return data;
}

/* transports/nostr.md "Event identity and tag cardinality": an adopted
 * kind:444 rumor has exactly one `e` tag with exactly one lowercase-hex
 * event id, and exactly one `relays` tag with one or more distinct relay
 * URLs of the relay URL profile. */
static bool
adopted_rumor_tags_valid(NostrTags *tags)
{
    size_t n_e = 0, n_relays = 0;
    for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        const char *key = nostr_tag_size(tag) >= 1 ? nostr_tag_get_key(tag) : NULL;
        if (!key) continue;
        if (strcmp(key, "e") == 0) {
            if (++n_e > 1 || nostr_tag_size(tag) != 2 ||
                !is_lower_hex64(nostr_tag_get_value(tag)))
                return false;
        } else if (strcmp(key, "relays") == 0) {
            if (++n_relays > 1 || nostr_tag_size(tag) < 2) return false;
            for (size_t j = 1; j < nostr_tag_size(tag); j++) {
                const char *url = nostr_tag_get(tag, j);
                if (!url || !mls_relay_url_valid((const uint8_t *)url, strlen(url)))
                    return false;
                for (size_t k = 1; k < j; k++)
                    if (strcmp(nostr_tag_get(tag, k), url) == 0) return false;
            }
        }
    }
    return n_e == 1 && n_relays == 1;
}

static MarmotError welcome_open(Marmot *m, const MarmotWelcome *welcome, const uint8_t *data,
                                size_t len, MlsGroup *out, uint32_t *out_signer_leaf,
                                MarmotKpUse *out_use, const char **reason);

/* The invitation preview of an opened adopted Welcome, from the group's
 * signed components (not from rumor tags). */
static MarmotError
adopted_preview(MarmotWelcome *welcome, const MlsGroup *g)
{
    MarmotGroup *group = NULL;
    char **relays = NULL;
    size_t n_relays = 0;
    MarmotError err = marmot_adopted_group_from_mls(g, &group, &relays, &n_relays);
    if (err != MARMOT_OK) return err;
    marmot_adopted_relays_free(relays, n_relays);
    memcpy(welcome->nostr_group_id, group->nostr_group_id, 32);
    welcome->group_name = group->name;
    welcome->group_description = group->description;
    welcome->group_admin_pubkeys = group->admin_pubkeys;
    welcome->group_admin_count = group->admin_count;
    group->name = group->description = NULL;
    group->admin_pubkeys = NULL;
    group->admin_count = 0;
    size_t members = 0;
    for (uint32_t i = 0; i < g->tree.n_leaves; i++)
        if (g->tree.nodes[mls_tree_leaf_to_node(i)].type == MLS_NODE_LEAF) members++;
    welcome->member_count = members;
    marmot_group_free(group);
    return MARMOT_OK;
}

/* Record the Welcome as failed.  Before anything else was written that
 * record is the outcome (`final`): it is kept although the operation
 * returns an error.  After a failed write it is not: with a storage
 * transaction it is rolled back with the partial writes, so the Welcome
 * stays pending and can be accepted again; without one it stays, as the
 * partial writes do. */
static void
record_welcome_failure(Marmot *m, const uint8_t wrapper_event_id[32],
                       const char *reason, bool final)
{
    if (m && m->storage && m->storage->save_processed_welcome) {
        MarmotError err = m->storage->save_processed_welcome(m->storage->ctx,
                                                             wrapper_event_id,
                                                             NULL,
                                                             marmot_now(),
                                                             MARMOT_WELCOME_STATE_FAILED,
                                                             reason);
        if (err == MARMOT_OK && final) marmot_txn_keep(m);
    }
}

/* A Welcome that can never be accepted (a final failure of the accept
 * path): recorded as failed, and its own record leaves the pending list as a
 * declined one does. Before 0.11.0 it stayed pending, listed as an
 * invitation that fails again on every attempt (nostrc-7gx7: an MDK 0.8
 * group with a second unproven member). Returns `refusal`; if the failed
 * state cannot be saved, nothing is recorded, the Welcome stays pending and
 * the save's error is returned (nostrc-w285). */
static MarmotError
refuse_welcome(Marmot *m, const MarmotWelcome *welcome, const char *reason,
               MarmotError refusal)
{
    MarmotWelcome failed = *welcome;
    failed.state = MARMOT_WELCOME_STATE_FAILED;
    MarmotError err = m->storage->save_welcome(m->storage->ctx, &failed);
    if (err != MARMOT_OK)
        return err;
    record_welcome_failure(m, welcome->wrapper_event_id, reason, true);
    return refusal;
}

#define ROUTING_ID_HELD_REASON "nostr group routing id already held by another group"

/* nostrc-scki: whether `nostr_group_id` is already the address of a held
 * group other than `mls_group_id` -- its current one, or one it had before
 * a routing rotation (an alias, nostrc-qp24.5.1.3).  nostr_group_id is a
 * public h tag and libmarmot routes an event by it (first match, then the
 * aliases): a second group at the same address would take the first one's
 * traffic, which then never decrypts for either.  A malicious inviter who
 * saw another group's h tag could break that group for us.  The group
 * itself (a re-invite after a removal) is no collision.  Any group record
 * counts, an ended one too: its address stays routed to it. */
static MarmotError
routing_id_held_elsewhere(Marmot *m, const uint8_t nostr_group_id[32],
                          const MarmotGroupId *mls_group_id, bool *out_held)
{
    *out_held = false;
    if (!m->storage->find_group_by_nostr_id) return MARMOT_ERR_STORAGE;
    MarmotGroup *owner = NULL;
    MarmotError err = m->storage->find_group_by_nostr_id(m->storage->ctx, nostr_group_id,
                                                         &owner);
    if (err == MARMOT_OK && !owner) err = MARMOT_ERR_GROUP_NOT_FOUND;
    if (err == MARMOT_ERR_GROUP_NOT_FOUND || err == MARMOT_ERR_STORAGE_NOT_FOUND)
        err = marmot_commit_find_group_by_alias(m, nostr_group_id, &owner);
    if (err == MARMOT_OK && owner)
        *out_held = !marmot_group_id_equal(&owner->mls_group_id, mls_group_id);
    marmot_group_free(owner);
    return err == MARMOT_ERR_GROUP_NOT_FOUND || err == MARMOT_ERR_STORAGE_NOT_FOUND
               ? MARMOT_OK : err;
}

/*
 * W17b addendum N1: TRUE when this Welcome is a copy of one we already
 * joined through: we hold MLS state for the group at the Welcome's epoch or
 * later, and our stored own leaf (by its signature key, which an UpdatePath
 * keeps) is a member of the tree the Welcome describes.  Such a copy
 * (another gift wrap, or one resent from the Welcome outbox) must not
 * replace the member's newer state.  A re-invite after a removal usually
 * brings a new KeyPackage (a new leaf signature key); when it reuses the old
 * one (e.g. a last-resort KeyPackage) it is still for a later epoch than the
 * removed member's stored state, so re-joining works either way.
 */
static bool
already_member_of(Marmot *m, const MlsGroup *joined)
{
    if (!m->storage->mls_load) return false;
    uint8_t *blob = NULL;
    size_t len = 0;
    if (m->storage->mls_load(m->storage->ctx, "mls_group", joined->group_id,
                             joined->group_id_len, &blob, &len) != MARMOT_OK || !blob)
        return false;
    MlsGroup stored;
    int rc = mls_group_deserialize(blob, len, &stored);
    sodium_memzero(blob, len);
    free(blob);
    if (rc != 0) return false;
    bool member = false;
    uint32_t own = mls_tree_leaf_to_node(stored.own_leaf_index);
    if (stored.epoch >= joined->epoch &&
        stored.own_leaf_index < stored.tree.n_leaves &&
        stored.tree.nodes[own].type == MLS_NODE_LEAF) {
        const uint8_t *sig = stored.tree.nodes[own].leaf.signature_key;
        for (uint32_t i = 0; i < joined->tree.n_leaves && !member; i++) {
            const MlsNode *n = &joined->tree.nodes[mls_tree_leaf_to_node(i)];
            member = n->type == MLS_NODE_LEAF &&
                     memcmp(n->leaf.signature_key, sig, MLS_SIG_PK_LEN) == 0;
        }
    }
    mls_group_free(&stored);
    return member;
}

/*
 * nostrc-7vyi (Marmot protocol-core/joining.md steps 5-6): every member leaf
 * of the joined tree is bound to the account its credential names.
 * - A valid account-identity proof binds any leaf; one that does not verify
 *   (or a malformed dictionary) rejects the Welcome.
 * - Our own leaf is our KeyPackage's.
 * - The leaf that signed the GroupInfo -- the committer who built this
 *   Welcome -- is bound by the Welcome's sender (the rumor pubkey, which the
 *   caller checked against the NIP-59 seal): it may lack a proof.
 * - Any other leaf without a proof is accepted in legacy mode only.
 */
/* The Welcome's sender: the rumor's pubkey, as process_welcome recorded it
 * or, for a backend that keeps only the rumor (nostrdb), from the rumor. */
static bool
welcome_sender(const MarmotWelcome *w, uint8_t out[32])
{
    static const uint8_t zero[32];
    if (sodium_memcmp(w->welcomer, zero, 32) != 0) {
        memcpy(out, w->welcomer, 32);
        return true;
    }
    if (!w->event_json) return false;
    NostrEvent rumor;
    memset(&rumor, 0, sizeof(rumor));
    bool ok = nostr_event_deserialize_compact(&rumor, w->event_json, NULL) &&
              rumor.pubkey && is_hex_len(rumor.pubkey, 64) &&
              marmot_hex_decode(rumor.pubkey, out, 32) == 0;
    free(rumor.id); free(rumor.pubkey); free(rumor.content);
    free(rumor.sig); nostr_tags_free(rumor.tags);
    return ok;
}

/* The Welcome rumor's created_at: the inviter dates it as the Commit that
 * adds us (nostrc-2lrz, review W24 L3); 0 when unknown. */
static int64_t
welcome_created_at(const MarmotWelcome *w)
{
    if (!w->event_json) return 0;
    NostrEvent rumor;
    memset(&rumor, 0, sizeof(rumor));
    int64_t t = nostr_event_deserialize_compact(&rumor, w->event_json, NULL)
                    ? rumor.created_at : 0;
    free(rumor.id); free(rumor.pubkey); free(rumor.content);
    free(rumor.sig); nostr_tags_free(rumor.tags);
    return t;
}

static MarmotError
welcome_tree_bound(const Marmot *m, const MlsGroup *g, uint32_t signer_leaf,
                   const MarmotWelcome *welcome)
{
    uint8_t welcomer[32];
    uint8_t id[32];
    /* Only the GroupInfo signer, and only when its account sent the
     * Welcome, may lack a proof. */
    uint32_t exempt = UINT32_MAX;
    if (welcome_sender(welcome, welcomer) &&
        marmot_mls_sender_identity(g, signer_leaf, id) == 0 &&
        memcmp(id, welcomer, 32) == 0)
        exempt = signer_leaf;
    return marmot_tree_members_bound(g, exempt, m->config.allow_unproven_members);
}

/*
 * The record of a legacy-profile group joined from @mls.  MIP-01: every
 * Marmot group carries exactly one marmot_group_data (0xF2EE), and the
 * joiner verifies it (MIP-02, step 3). A group with none, several, or one
 * that does not decode is refused (MARMOT_ERR_EXTENSION_FORMAT): it used to
 * be joined without its nostr_group_id, name, relays and admins (nostrc-
 * 7gx7: every MDK 0.8 Welcome, before 0.11.0 read MIP-01's encoding).
 */
static MarmotError
legacy_group_from_mls(const MlsGroup *mls, MarmotGroup **out, char ***relays_out,
                      size_t *relay_count_out)
{
    *out = NULL;
    *relays_out = NULL;
    *relay_count_out = 0;
    MarmotGroupDataExtension *gde = NULL;
    const uint8_t *gde_data = NULL;
    size_t gde_data_len = 0, gde_count = 0;
    if (marmot_extensions_find(mls->extensions_data, mls->extensions_len,
                               MARMOT_EXTENSION_TYPE, &gde_data, &gde_data_len,
                               &gde_count) == 0 &&
        gde_count == 1)
        gde = marmot_group_data_extension_deserialize(gde_data, gde_data_len);
    if (!gde) return MARMOT_ERR_EXTENSION_FORMAT;

    /* Create the MarmotGroup */
    MarmotGroup *group = marmot_group_new();
    if (!group) {
        marmot_group_data_extension_free(gde);
        return MARMOT_ERR_MEMORY;
    }

    group->mls_group_id = marmot_group_id_new(mls->group_id, mls->group_id_len);
    group->epoch = mls->epoch;
    group->state = MARMOT_GROUP_STATE_ACTIVE;

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

    /* Extract relay URLs from gde before freeing */
    if (gde->relay_count > 0 && gde->relays) {
        char **relays = calloc(gde->relay_count, sizeof(char *));
        if (relays) {
            for (size_t i = 0; i < gde->relay_count; i++)
                relays[i] = gde->relays[i] ? strdup(gde->relays[i]) : NULL;
            *relays_out = relays;
            *relay_count_out = gde->relay_count;
        }
    }
    marmot_group_data_extension_free(gde);
    *out = group;
    return MARMOT_OK;
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

static MarmotError
process_welcome_impl(Marmot *m,
                        const uint8_t wrapper_event_id[32],
                        const char *rumor_event_json,
                        const uint8_t *sender_pubkey,
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
        record_welcome_failure(m, wrapper_event_id, "deserialization failed", true);
        return MARMOT_ERR_DESERIALIZATION;
    }

    /* The caller's seal-authenticated sender (review W20 N1): a rumor that
     * names another author is forged. */
    if (sender_pubkey && rumor.pubkey) {
        uint8_t named[32];
        if (!is_hex_len(rumor.pubkey, 64) || marmot_hex_decode(rumor.pubkey, named, 32) != 0 ||
            memcmp(named, sender_pubkey, 32) != 0) {
            free(rumor.id); free(rumor.pubkey); free(rumor.content);
            free(rumor.sig); nostr_tags_free(rumor.tags);
            record_welcome_failure(m, wrapper_event_id, "rumor author is not the seal's", true);
            return MARMOT_ERR_AUTHOR_MISMATCH;
        }
    }

    /* Verify kind */
    if (rumor.kind != MARMOT_KIND_WELCOME) {
        /* Free stack-allocated event fields */
        free(rumor.id); free(rumor.pubkey); free(rumor.content);
        free(rumor.sig); nostr_tags_free(rumor.tags);
        record_welcome_failure(m, wrapper_event_id, "unexpected welcome kind", true);
        return MARMOT_ERR_INVALID_ARG;
    }

    /* Check welcome expiry: reject events older than max_event_age */
    if (rumor.created_at > 0 && m->config.max_event_age_secs > 0) {
        int64_t now = marmot_now();
        int64_t age = now - rumor.created_at;
        if (age > (int64_t)m->config.max_event_age_secs) {
            free(rumor.id); free(rumor.pubkey); free(rumor.content);
            free(rumor.sig); nostr_tags_free(rumor.tags);
            record_welcome_failure(m, wrapper_event_id, "welcome expired", true);
            return MARMOT_ERR_WELCOME_EXPIRED;
        }
    }

    /* Get content */
    if (!rumor.content || strlen(rumor.content) == 0) {
        free(rumor.id); free(rumor.pubkey); free(rumor.content);
        free(rumor.sig); nostr_tags_free(rumor.tags);
        record_welcome_failure(m, wrapper_event_id, "empty welcome content", true);
        return MARMOT_ERR_DESERIALIZATION;
    }

    /* Decode content to MLS Welcome bytes */
    size_t welcome_len = 0;
    bool adopted_rumor = false;
    uint8_t *welcome_data = decode_welcome_content(rumor.content, rumor.tags, &welcome_len,
                                                   &adopted_rumor);
    /* transports/nostr.md: the rumor "MUST NOT have a `sig` field" (W24
     * review N2).  The parser sets sig only when the key is present. */
    if (welcome_data && adopted_rumor && rumor.sig) {
        free(welcome_data);
        free(rumor.id); free(rumor.pubkey); free(rumor.content);
        free(rumor.sig); nostr_tags_free(rumor.tags);
        record_welcome_failure(m, wrapper_event_id, "adopted welcome rumor carries a sig",
                               true);
        return MARMOT_ERR_VALIDATION;
    }
    if (welcome_data && adopted_rumor && !adopted_rumor_tags_valid(rumor.tags)) {
        free(welcome_data);
        free(rumor.id); free(rumor.pubkey); free(rumor.content);
        free(rumor.sig); nostr_tags_free(rumor.tags);
        record_welcome_failure(m, wrapper_event_id, "malformed adopted welcome rumor tags",
                               true);
        return MARMOT_ERR_VALIDATION;
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
        record_welcome_failure(m, wrapper_event_id, "welcome content decode failed", true);
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
    if (sender_pubkey)
        memcpy(welcome->welcomer, sender_pubkey, 32);
    else if (rumor.pubkey && is_hex_len(rumor.pubkey, 64))
        marmot_hex_decode(rumor.pubkey, welcome->welcomer, 32);
    welcome->event_json = strdup(rumor_event_json);
    welcome->state = MARMOT_WELCOME_STATE_PENDING;

    /* Set relays */
    welcome->group_relays = relay_urls;
    welcome->group_relay_count = relay_count;

    /* Extract cleartext preview info from rumor tags.  The adopted binding
     * has no cleartext preview: such tags would be unauthenticated claims
     * (nostrc-qp24.5.1), so an adopted rumor's group fields stay empty
     * until the signed components are read on accept. */
    if (adopted_rumor) {
        memset(welcome->nostr_group_id, 0, 32);
        welcome->group_name = NULL;
        welcome->group_description = NULL;
        welcome->member_count = 0;
    } else {
        extract_group_preview_from_tags(welcome, rumor.tags);
    }

    /* An adopted Welcome is opened now (nostrc-qp24.5.1): one libmarmot
     * cannot join -- not for our KeyPackages, unsupported (every White Noise
     * group, for now), invalid -- is refused here with its reason, instead
     * of becoming an invitation that can only fail; one it can join shows
     * its signed group profile. */
    if (adopted_rumor) {
        MlsGroup opened;
        const char *why = NULL;
        MarmotError oerr = welcome_open(m, welcome, welcome_data, welcome_len, &opened, NULL,
                                        NULL, &why);
        /* A storage error that may be transient (no reason) does not fail
         * the Welcome for good: it is stored as a pending invitation like
         * any other, and accepting it runs the whole check again (W24
         * review L3). */
        if (oerr != MARMOT_OK && !why) oerr = MARMOT_OK;
        else if (oerr == MARMOT_OK) {
            if (opened.profile == MARMOT_GROUP_PROFILE_ADOPTED)
                oerr = adopted_preview(welcome, &opened);
            /* nostrc-scki: the address is signed here, so a Welcome that
             * can only be refused on accept is refused now, never listed.
             * A storage error leaves the check to accept. */
            bool held = false;
            MarmotGroupId opened_id = { opened.group_id, opened.group_id_len };
            if (oerr == MARMOT_OK &&
                routing_id_held_elsewhere(m, welcome->nostr_group_id, &opened_id, &held) ==
                    MARMOT_OK &&
                held) {
                oerr = MARMOT_ERR_PROTOCOL_GROUP_MISMATCH;
                why = ROUTING_ID_HELD_REASON;
            }
            mls_group_free(&opened);
        }
        if (oerr != MARMOT_OK) {
            free(rumor.id); free(rumor.pubkey); free(rumor.content);
            free(rumor.sig); nostr_tags_free(rumor.tags);
            free(welcome_data);
            marmot_welcome_free(welcome);
            /* Recorded only when definitive (a reason): an allocation
             * failure in the preview is not. */
            if (why) record_welcome_failure(m, wrapper_event_id, why, true);
            return oerr;
        }
    }

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

/*
 * Open the MLS Welcome @data (len @len) of @welcome for one of our
 * KeyPackages and admit the group it describes: decrypt and verify it
 * (mls_welcome.c: for an adopted group also its GroupContext, members and
 * leaf signatures), then bind every member to its account -- for an adopted
 * group (nostrc-qp24.5.1) with no exemption, the inviter an admin, and the
 * Welcome sent by that inviter.  On success *out is the joined state (caller
 * frees) and *out_signer_leaf (nullable) the GroupInfo signer's leaf, whose
 * device the Welcome's sender vouched for (W24 review owkh), and *out_use
 * (nullable) the KeyPackage of ours it was opened with (nostrc-0bdg);
 * otherwise *reason names the failure, or is NULL for a storage
 * error that may be transient (the Welcome stays pending).  Writes nothing.
 */
static MarmotError
welcome_open(Marmot *m, const MarmotWelcome *welcome, const uint8_t *data, size_t len,
             MlsGroup *out, uint32_t *out_signer_leaf, MarmotKpUse *out_use,
             const char **reason)
{
    memset(out, 0, sizeof(*out));
    if (out_signer_leaf) *out_signer_leaf = UINT32_MAX;
    if (out_use) memset(out_use, 0, sizeof(*out_use));
    MarmotKpUse use;
    memset(&use, 0, sizeof(use));
    *reason = NULL;
    /* We need to find which KeyPackage was used for this Welcome.
     * The MLS Welcome contains KeyPackageRef entries — we need to
     * match against our stored KeyPackage private keys. */

    /* Deserialize the MLS Welcome to find our KeyPackageRef */
    MlsWelcome mls_welcome;
    memset(&mls_welcome, 0, sizeof(mls_welcome));
    MlsTlsReader reader;
    mls_tls_reader_init(&reader, data, len);

    if (mls_welcome_deserialize(&reader, &mls_welcome) != 0) {
        *reason = "MLS Welcome deserialize failed";
        return MARMOT_ERR_MLS;
    }

    /* Find our entry among the EncryptedGroupSecrets.  Only a definitive
     * not-found means "not our KeyPackage": any other storage error leaves
     * the Welcome pending, as for its raw data (nostrc-w285, review W24 M3).
     * Such an error returns with *reason NULL: nothing is recorded (W24
     * slice E review L3). */
    MlsKeyPackage matched_kp;
    MlsKeyPackagePrivate matched_priv;
    memset(&matched_kp, 0, sizeof(matched_kp));
    bool found = false;

    for (size_t i = 0; i < mls_welcome.secret_count && !found; i++) {
        const uint8_t *ref = mls_welcome.secrets[i].key_package_ref;
        uint8_t *priv_data = NULL;
        size_t priv_len = 0;
        MarmotError kp_err = m->storage->mls_load(m->storage->ctx, "kp_priv", ref,
                                                  MLS_HASH_LEN, &priv_data, &priv_len);
        if (kp_err == MARMOT_ERR_STORAGE_NOT_FOUND) continue;
        if (kp_err != MARMOT_OK) {
            mls_welcome_clear(&mls_welcome);
            return kp_err;
        }
        if (!priv_data || priv_len != MLS_KEM_SK_LEN + MLS_KEM_SK_LEN + MLS_SIG_SK_LEN) {
            if (priv_data) sodium_memzero(priv_data, priv_len);
            free(priv_data);
            continue;                       /* not a key of ours we can use */
        }
        memcpy(matched_priv.init_key_private, priv_data, MLS_KEM_SK_LEN);
        memcpy(matched_priv.encryption_key_private, priv_data + MLS_KEM_SK_LEN, MLS_KEM_SK_LEN);
        memcpy(matched_priv.signature_key_private, priv_data + 2 * MLS_KEM_SK_LEN,
               MLS_SIG_SK_LEN);
        sodium_memzero(priv_data, priv_len);
        free(priv_data);

        /* The full KeyPackage, so mls_welcome_process_parsed can compute the
         * KeyPackageRef and populate the tree. */
        uint8_t *kp_data = NULL;
        size_t kp_len = 0;
        kp_err = m->storage->mls_load(m->storage->ctx, "kp_full", ref, MLS_HASH_LEN,
                                      &kp_data, &kp_len);
        if (kp_err != MARMOT_OK && kp_err != MARMOT_ERR_STORAGE_NOT_FOUND) {
            sodium_memzero(&matched_priv, sizeof(matched_priv));
            mls_welcome_clear(&mls_welcome);
            return kp_err;
        }
        bool parsed = false;
        if (kp_err == MARMOT_OK && kp_data) {
            MlsTlsReader kp_reader;
            mls_tls_reader_init(&kp_reader, kp_data, kp_len);
            parsed = mls_key_package_deserialize(&kp_reader, &matched_kp) == 0;
        }
        free(kp_data);
        if (!parsed) {
            /* Missing or unreadable: a minimal KeyPackage, whose ref check
             * fails below, a final refusal. */
            mls_key_package_clear(&matched_kp);
            memset(&matched_kp, 0, sizeof(matched_kp));
            matched_kp.version = 1;
            matched_kp.cipher_suite = MARMOT_CIPHERSUITE;
            crypto_scalarmult_base(matched_kp.init_key, matched_priv.init_key_private);
        }
        found = true;
        /* Which KeyPackage this is, for its lifecycle after a join. */
        memcpy(use.ref, ref, sizeof(use.ref));
        use.have = parsed && matched_kp.leaf_node.credential_identity &&
                   matched_kp.leaf_node.credential_identity_len == 32;
        if (use.have) {
            memcpy(use.owner, matched_kp.leaf_node.credential_identity, 32);
            use.last_resort = marmot_kp_is_last_resort(&matched_kp);
        }
    }

    if (!found) {
        mls_welcome_clear(&mls_welcome);
        *reason = "matching KeyPackage private key not found";
        return MARMOT_ERR_KEY_NOT_FOUND;
    }

    /* Process the MLS Welcome to join the group */
    MlsGroup mls_group;
    memset(&mls_group, 0, sizeof(mls_group));

    uint32_t signer_leaf = UINT32_MAX;
    int rc = mls_welcome_process_parsed_signer(&mls_welcome, &matched_kp, &matched_priv,
                                               &mls_group, &signer_leaf);
    mls_welcome_clear(&mls_welcome);
    mls_key_package_clear(&matched_kp);
    sodium_memzero(&matched_priv, sizeof(matched_priv));

    if (rc != 0) {
        /* An adopted group libmarmot cannot honour, or whose GroupContext or
         * members are malformed, is refused with that reason (nostrc-
         * qp24.5.1); any other failure is the MLS layer's. */
        switch (rc) {
        case MARMOT_ERR_UNSUPPORTED:
            *reason = "group requires something libmarmot does not support";
            return MARMOT_ERR_UNSUPPORTED;
        case MARMOT_ERR_EXTENSION_FORMAT:
        case MARMOT_ERR_VALIDATION:
            *reason = "invalid adopted group state";
            return (MarmotError)rc;
        default:
            *reason = "MLS Welcome processing failed";
            return MARMOT_ERR_MLS;
        }
    }

    /* Every member is who its credential says (nostrc-7vyi): otherwise
     * nothing of the group is stored.  An adopted group (nostrc-qp24.5.1)
     * has no exemption and no legacy mode: every leaf carries a verified
     * account proof, and the inviter -- the GroupInfo signer -- is an admin
     * of the joined state (protocol-core/joining.md, admin-policy-v1.md). */
    MarmotError bind_err;
    const char *bind_reason = "member leaf without a valid account-identity proof";
    if (mls_group.profile == MARMOT_GROUP_PROFILE_ADOPTED) {
        bind_err = marmot_adopted_members_proven(&mls_group);
        if (bind_err == MARMOT_OK && !marmot_adopted_leaf_is_admin(&mls_group, signer_leaf)) {
            bind_err = MARMOT_ERR_ADMIN_ONLY;
            bind_reason = "welcome not sent by an admin of the group";
        }
        /* The Nostr sender (seal author, or the rumor's) is that inviter's
         * account: a Welcome re-wrapped by anyone else is refused. */
        uint8_t sender[32], inviter[32];
        if (bind_err == MARMOT_OK &&
            (!welcome_sender(welcome, sender) ||
             marmot_mls_sender_identity(&mls_group, signer_leaf, inviter) != 0 ||
             sodium_memcmp(sender, inviter, 32) != 0)) {
            bind_err = MARMOT_ERR_AUTHOR_MISMATCH;
            bind_reason = "welcome not sent by its inviter";
        }
    } else {
        bind_err = welcome_tree_bound(m, &mls_group, signer_leaf, welcome);
    }
    if (bind_err != MARMOT_OK) {
        mls_group_free(&mls_group);
        *reason = bind_reason;
        return bind_err;
    }
    *out = mls_group;
    if (out_signer_leaf) *out_signer_leaf = signer_leaf;
    if (out_use) *out_use = use;
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
    MarmotError load_err = m->storage->mls_load(m->storage->ctx, "welcome_data",
                                                 welcome->wrapper_event_id, 32,
                                                 &welcome_data, &welcome_len);
    /* Only missing data is final: another storage error may be transient (a
     * busy database), and the invitation stays pending (nostrc-w285). */
    if (load_err == MARMOT_ERR_STORAGE_NOT_FOUND)
        return refuse_welcome(m, welcome, "stored welcome data not found", load_err);
    if (load_err != MARMOT_OK)
        return load_err;

    MlsGroup mls_group;
    const char *open_reason = NULL;
    uint32_t signer_leaf = UINT32_MAX;   /* the GroupInfo signer (W24 review owkh) */
    MarmotKpUse used;                    /* our KeyPackage it opened with (nostrc-0bdg) */
    MarmotError open_err = welcome_open(m, welcome, welcome_data, welcome_len, &mls_group,
                                        &signer_leaf, &used, &open_reason);
    free(welcome_data);
    /* A failure without a reason is a storage error that may be transient:
     * the invitation stays pending (nostrc-w285). */
    if (open_err != MARMOT_OK)
        return open_reason ? refuse_welcome(m, welcome, open_reason, open_err) : open_err;

    /* A duplicate Welcome for a group we already joined: keep our state
     * (it may be epochs ahead) and retire this copy as accepted. */
    if (already_member_of(m, &mls_group)) {
        mls_group_free(&mls_group);
        MarmotWelcome retired = *welcome;
        retired.state = MARMOT_WELCOME_STATE_ACCEPTED;
        /* Retiring the copy is the outcome: keep it (both writes or neither). */
        if (m->storage->save_welcome(m->storage->ctx, &retired) == MARMOT_OK &&
            m->storage->save_processed_welcome(m->storage->ctx,
                                               welcome->wrapper_event_id, welcome->id,
                                               marmot_now(),
                                               MARMOT_WELCOME_STATE_ACCEPTED,
                                               "already a member") == MARMOT_OK)
            marmot_txn_keep(m);
        return MARMOT_ERR_WELCOME_ALREADY_ACCEPTED;
    }

    /* The group record: a legacy group's from its marmot_group_data, an
     * adopted group's from its signed components (nostrc-qp24.5.1). */
    MarmotGroup *group = NULL;
    char **gde_relays = NULL;
    size_t gde_relay_count = 0;
    const bool adopted = mls_group.profile == MARMOT_GROUP_PROFILE_ADOPTED;
    MarmotError group_err =
        adopted ? marmot_adopted_group_from_mls(&mls_group, &group, &gde_relays, &gde_relay_count)
                : legacy_group_from_mls(&mls_group, &group, &gde_relays, &gde_relay_count);
    if (group_err != MARMOT_OK) {
        mls_group_free(&mls_group);
        if (group_err != MARMOT_ERR_MEMORY)
            return refuse_welcome(m, welcome,
                                  adopted ? "malformed adopted group components"
                                          : "missing or malformed marmot_group_data (0xF2EE)",
                                  group_err);
        return group_err;
    }

    /* nostrc-scki: before anything of this Welcome is stored, its address
     * must be no other held group's (routing_id_held_elsewhere()). */
    bool collision = false;
    MarmotError address_err = routing_id_held_elsewhere(m, group->nostr_group_id,
                                                        &group->mls_group_id, &collision);
    if (address_err != MARMOT_OK || collision) {
        mls_group_free(&mls_group);
        marmot_group_free(group);
        marmot_adopted_relays_free(gde_relays, gde_relay_count);
        return address_err != MARMOT_OK
                   ? address_err
                   : refuse_welcome(m, welcome, ROUTING_ID_HELD_REASON,
                                    MARMOT_ERR_PROTOCOL_GROUP_MISMATCH);
    }

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
    sodium_memzero(state_data, state_len);
    free(state_data);
    if (err != MARMOT_OK)
        goto fail;

    /* Store the group metadata. */
    err = m->storage->save_group(m->storage->ctx, group);
    if (err != MARMOT_OK)
        goto fail;
    /* Members again: an earlier removal (nostrc-xrya) no longer holds. */
    err = marmot_commit_clear_removal(m, &group->mls_group_id);
    if (err != MARMOT_OK)
        goto fail;
    /* Our first events follow the Commit that added us. */
    err = marmot_observe_group_event_time(m, group->nostr_group_id, welcome_created_at(welcome));
    if (err != MARMOT_OK)
        goto fail;
    /* Which device the Welcome's sender vouched for (W24 review owkh). */
    {
        uint8_t sender[32];
        err = marmot_welcome_signer_record(m, &mls_group, signer_leaf,
                                           welcome_sender(welcome, sender) ? sender : NULL);
        if (err != MARMOT_OK)
            goto fail;
    }

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

    /* The join succeeded: a consumed non-last-resort KeyPackage's private
     * material goes now, in this transaction (foundation/key-packages.md;
     * kp_lifecycle.c). A failed Welcome never gets here: nothing of the
     * KeyPackage changes. */
    err = marmot_kp_lifecycle_consumed(m, &used);
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
    record_welcome_failure(m, welcome->wrapper_event_id, marmot_error_string(err), false);
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

static MarmotError
accept_welcome_impl(Marmot *m, const MarmotWelcome *welcome)
{
    return accept_welcome_internal(m, welcome, NULL);
}

static MarmotError
accept_welcome_by_wrapper_id_impl(Marmot *m,
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

static MarmotError
decline_welcome_impl(Marmot *m, const MarmotWelcome *welcome)
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

/* ──────────────────────────────────────────────────────────────────────────
 * Public API: one storage transaction per operation (nostrc-qp24.7)
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_process_welcome(Marmot *m,
                        const uint8_t wrapper_event_id[32],
                        const char *rumor_event_json,
                        MarmotWelcome **out_welcome)
{
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    err = process_welcome_impl(m, wrapper_event_id, rumor_event_json, NULL, out_welcome);
    MarmotError end = marmot_txn_end(m, err);
    if (err == MARMOT_OK && end != MARMOT_OK) {
        marmot_welcome_free(*out_welcome);
        *out_welcome = NULL;
    }
    return end;
}

MarmotError
marmot_process_welcome_from(Marmot *m,
                            const uint8_t wrapper_event_id[32],
                            const char *rumor_event_json,
                            const uint8_t sender_pubkey[32],
                            MarmotWelcome **out_welcome)
{
    if (!sender_pubkey) return MARMOT_ERR_INVALID_ARG;
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    err = process_welcome_impl(m, wrapper_event_id, rumor_event_json, sender_pubkey,
                               out_welcome);
    MarmotError end = marmot_txn_end(m, err);
    if (err == MARMOT_OK && end != MARMOT_OK) {
        marmot_welcome_free(*out_welcome);
        *out_welcome = NULL;
    }
    return end;
}

MarmotError
marmot_accept_welcome(Marmot *m, const MarmotWelcome *welcome)
{
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    return marmot_txn_end(m, accept_welcome_impl(m, welcome));
}

MarmotError
marmot_accept_welcome_by_wrapper_id(Marmot *m,
                                     const uint8_t wrapper_event_id[32],
                                     MarmotGroup **out_group)
{
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    err = accept_welcome_by_wrapper_id_impl(m, wrapper_event_id, out_group);
    MarmotError end = marmot_txn_end(m, err);
    if (err == MARMOT_OK && end != MARMOT_OK && out_group) {
        marmot_group_free(*out_group);
        *out_group = NULL;
    }
    return end;
}

MarmotError
marmot_decline_welcome(Marmot *m, const MarmotWelcome *welcome)
{
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    return marmot_txn_end(m, decline_welcome_impl(m, welcome));
}
