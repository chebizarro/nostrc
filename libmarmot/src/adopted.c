/*
 * libmarmot - adopted-profile groups, Marmot layer (nostrc-qp24.5.1).
 * See adopted.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "adopted.h"
#include "kp_profile.h"
#include "mls/mls_app_components.h"
#include "mls/mls_app_data_update.h"
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

/* W24 review N4: the MLS layer (mls/mls_app_components.h,
 * mls/mls_app_data_update.h) and the Marmot layer (kp_profile.h) each name
 * these component ids.  They are kept as two spellings so neither layer
 * includes the other's header; these assertions keep them equal. */
_Static_assert(MLS_COMPONENT_APP_COMPONENTS == MARMOT_COMPONENT_APP_COMPONENTS,
               "app_components id spelled two ways");
_Static_assert(MLS_COMPONENT_SAFE_AAD == MARMOT_COMPONENT_SAFE_AAD,
               "safe_aad id spelled two ways");
_Static_assert(MLS_COMPONENT_ACCOUNT_PROOF_V2 == MARMOT_COMPONENT_ACCOUNT_PROOF_V2,
               "account proof v2 id spelled two ways");

MarmotError
marmot_adopted_members_proven(const MlsGroup *g)
{
    if (!g || g->profile != MARMOT_GROUP_PROFILE_ADOPTED) return MARMOT_ERR_INVALID_ARG;
    /* W24 review N5: the proof binds the ciphersuite, and MlsGroup has no
     * suite of its own because libmarmot is single-suite: the state format
     * carries none, the Welcome join refuses any other suite
     * (mls_welcome_process_parsed), and create and the GroupInfo parser use only
     * MARMOT_CIPHERSUITE.  If MlsGroup ever gains a suite field, verify
     * under it here. */
    for (uint32_t i = 0; i < g->tree.n_leaves; i++) {
        const MlsNode *n = &g->tree.nodes[mls_tree_leaf_to_node(i)];
        if (n->type != MLS_NODE_LEAF) continue;
        if (marmot_leaf_proof_status(&n->leaf, MARMOT_CIPHERSUITE) != MARMOT_LEAF_PROOF_VALID)
            return MARMOT_ERR_KEY_PACKAGE_IDENTITY;
    }
    return MARMOT_OK;
}

bool
marmot_adopted_leaf_is_admin(const MlsGroup *g, uint32_t leaf)
{
    if (!g || g->profile != MARMOT_GROUP_PROFILE_ADOPTED || leaf >= g->tree.n_leaves)
        return false;
    const MlsNode *n = &g->tree.nodes[mls_tree_leaf_to_node(leaf)];
    if (n->type != MLS_NODE_LEAF || n->leaf.credential_identity_len != 32 ||
        !n->leaf.credential_identity)
        return false;
    MlsAdoptedGroupContext gc;
    const uint8_t *keys = NULL;
    size_t count = 0;
    if (mls_adopted_group_context_parse(g->extensions_data, g->extensions_len, &gc) != 0 ||
        !gc.admins || mls_admin_policy_v1_decode(gc.admins, gc.admins_len, &keys, &count) != 0)
        return false;
    for (size_t k = 0; k < count; k++)
        if (memcmp(keys + 32 * k, n->leaf.credential_identity, 32) == 0) return true;
    return false;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Creation
 * ──────────────────────────────────────────────────────────────────────── */

static int
cmp_relay(const void *a, const void *b)
{
    /* NUL-terminated, NUL-free: strcmp is the bytewise order, a proper
     * prefix first (the order 0x8004 requires). */
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

MarmotError
marmot_adopted_canonical_relays(const char *const *relays, size_t count,
                                const char ***out, size_t *out_count)
{
    if (!out || !out_count || (count > 0 && !relays)) return MARMOT_ERR_INVALID_ARG;
    *out = NULL;
    *out_count = 0;
    if (count == 0) return MARMOT_ERR_INVALID_ARG;
    const char **sorted = calloc(count, sizeof(*sorted));
    if (!sorted) return MARMOT_ERR_MEMORY;
    for (size_t i = 0; i < count; i++) {
        size_t len = relays[i] ? strlen(relays[i]) : 0;
        if (!relays[i] || !mls_relay_url_valid((const uint8_t *)relays[i], len)) {
            free(sorted);
            return MARMOT_ERR_INVALID_ARG;
        }
        sorted[i] = relays[i];
    }
    qsort(sorted, count, sizeof(*sorted), cmp_relay);
    size_t n = 0;
    for (size_t i = 0; i < count; i++)
        if (n == 0 || strcmp(sorted[n - 1], sorted[i]) != 0) sorted[n++] = sorted[i];
    if (n > MARMOT_NOSTR_ROUTING_MAX_RELAYS) {
        free(sorted);
        return MARMOT_ERR_INVALID_ARG;
    }
    *out = sorted;
    *out_count = n;
    return MARMOT_OK;
}

static int
write_entry(MlsTlsBuf *entries, uint16_t id, const MlsTlsBuf *data)
{
    return mls_tls_write_u16(entries, id) != 0 ||
                   mls_tls_write_opaque32(entries, data->data, data->len) != 0
               ? -1 : 0;
}

MarmotError
marmot_adopted_group_context_build(const char *name, const char *description,
                                   const uint8_t (*admins)[32], size_t admin_count,
                                   const uint8_t nostr_group_id[32],
                                   const char *const *relays, size_t relay_count,
                                   uint8_t **out, size_t *out_len)
{
    if (!out || !out_len || !nostr_group_id || !admins || admin_count == 0 || !relays ||
        relay_count == 0 || relay_count > MARMOT_NOSTR_ROUTING_MAX_RELAYS)
        return MARMOT_ERR_INVALID_ARG;
    *out = NULL;
    *out_len = 0;
    const char *nm = name ? name : "";
    const char *ds = description ? description : "";
    size_t nm_len = strlen(nm), ds_len = strlen(ds);
    if (nm_len > MARMOT_GROUP_PROFILE_NAME_MAX || ds_len > MARMOT_GROUP_PROFILE_DESCRIPTION_MAX ||
        !mls_utf8_valid((const uint8_t *)nm, nm_len) ||
        !mls_utf8_valid((const uint8_t *)ds, ds_len))
        return MARMOT_ERR_INVALID_ARG;
    for (size_t i = 1; i < admin_count; i++)
        if (memcmp(admins[i - 1], admins[i], 32) >= 0) return MARMOT_ERR_INVALID_ARG;
    for (size_t i = 0; i < relay_count; i++) {
        if (!relays[i] ||
            !mls_relay_url_valid((const uint8_t *)relays[i], strlen(relays[i])) ||
            (i > 0 && strcmp(relays[i - 1], relays[i]) >= 0))
            return MARMOT_ERR_INVALID_ARG;
    }

    MarmotError err = MARMOT_ERR_MEMORY;
    MlsTlsBuf caps = {0}, comps = {0}, profile = {0}, admin = {0}, routing = {0},
              relay_entries = {0}, life = {0}, entries = {0}, dict = {0}, exts = {0};
    if (mls_tls_buf_init(&caps, 16) != 0 || mls_tls_buf_init(&comps, 16) != 0 ||
        mls_tls_buf_init(&profile, nm_len + ds_len + 8) != 0 ||
        mls_tls_buf_init(&admin, admin_count * 32 + 8) != 0 ||
        mls_tls_buf_init(&routing, 64) != 0 || mls_tls_buf_init(&relay_entries, 64) != 0 ||
        mls_tls_buf_init(&life, 1) != 0 || mls_tls_buf_init(&entries, 256) != 0 ||
        mls_tls_buf_init(&dict, 256) != 0 || mls_tls_buf_init(&exts, 256) != 0)
        goto done;

    /* RequiredCapabilities: extensions [0x0006], proposals [0x0008], none. */
    static const uint16_t req_ext[] = {MLS_EXTENSION_APP_DATA_DICTIONARY};
    static const uint16_t req_prop[] = {MLS_PROPOSAL_TYPE_APP_DATA_UPDATE};
    if (marmot_components_list_encode(req_ext, 1, &caps) != 0 ||
        marmot_components_list_encode(req_prop, 1, &caps) != 0 ||
        mls_tls_write_vli(&caps, 0) != 0)
        goto done;

    if (marmot_components_list_encode(MLS_ADOPTED_SUPPORTED_COMPONENTS,
                                      MLS_ADOPTED_SUPPORTED_COMPONENT_COUNT, &comps) != 0 ||
        mls_tls_write_opaque32(&profile, (const uint8_t *)nm, nm_len) != 0 ||
        mls_tls_write_opaque32(&profile, (const uint8_t *)ds, ds_len) != 0 ||
        mls_tls_write_opaque32(&admin, (const uint8_t *)admins, admin_count * 32) != 0 ||
        mls_tls_write_u8(&life, 0) != 0) /* active */
        goto done;
    for (size_t i = 0; i < relay_count; i++)
        if (mls_tls_write_opaque32(&relay_entries, (const uint8_t *)relays[i],
                                   strlen(relays[i])) != 0)
            goto done;
    if (mls_tls_buf_append(&routing, nostr_group_id, 32) != 0 ||
        mls_tls_write_opaque32(&routing, relay_entries.data, relay_entries.len) != 0)
        goto done;

    /* Entries strictly ascending by component id. */
    if (write_entry(&entries, MLS_COMPONENT_APP_COMPONENTS, &comps) != 0 ||
        write_entry(&entries, MARMOT_COMPONENT_GROUP_PROFILE_V1, &profile) != 0 ||
        write_entry(&entries, MARMOT_COMPONENT_ADMIN_POLICY_V1, &admin) != 0 ||
        write_entry(&entries, MARMOT_COMPONENT_NOSTR_ROUTING_V1, &routing) != 0 ||
        write_entry(&entries, MARMOT_COMPONENT_GROUP_LIFECYCLE_V1, &life) != 0 ||
        mls_tls_write_opaque32(&dict, entries.data, entries.len) != 0)
        goto done;

    if (mls_tls_write_u16(&exts, MLS_EXT_REQUIRED_CAPABILITIES) != 0 ||
        mls_tls_write_opaque32(&exts, caps.data, caps.len) != 0 ||
        mls_tls_write_u16(&exts, MLS_EXTENSION_APP_DATA_DICTIONARY) != 0 ||
        mls_tls_write_opaque32(&exts, dict.data, dict.len) != 0)
        goto done;

    /* What we build is exactly what we would admit. */
    MlsAdoptedGroupContext check;
    if (mls_adopted_group_context_parse(exts.data, exts.len, &check) != 0) {
        err = MARMOT_ERR_INTERNAL;
        goto done;
    }
    *out = exts.data;
    *out_len = exts.len;
    exts.data = NULL;
    err = MARMOT_OK;
done:
    mls_tls_buf_free(&caps);
    mls_tls_buf_free(&comps);
    mls_tls_buf_free(&profile);
    mls_tls_buf_free(&admin);
    mls_tls_buf_free(&routing);
    mls_tls_buf_free(&relay_entries);
    mls_tls_buf_free(&life);
    mls_tls_buf_free(&entries);
    mls_tls_buf_free(&dict);
    mls_tls_buf_free(&exts);
    return err;
}

/* ──────────────────────────────────────────────────────────────────────────
 * MarmotGroup from the components
 * ──────────────────────────────────────────────────────────────────────── */

static char *
dup_bytes(const uint8_t *s, size_t len)
{
    char *out = malloc(len + 1);
    if (!out) return NULL;
    if (len) memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

void
marmot_adopted_relays_free(char **relays, size_t count)
{
    if (!relays) return;
    for (size_t i = 0; i < count; i++) free(relays[i]);
    free(relays);
}

MarmotError
marmot_adopted_group_from_mls(const MlsGroup *g, MarmotGroup **group_out,
                              char ***relays_out, size_t *relay_count_out)
{
    if (!g || !group_out || !relays_out || !relay_count_out ||
        g->profile != MARMOT_GROUP_PROFILE_ADOPTED)
        return MARMOT_ERR_INVALID_ARG;
    *group_out = NULL;
    *relays_out = NULL;
    *relay_count_out = 0;

    MlsAdoptedGroupContext gc;
    int rc = mls_adopted_group_context_parse(g->extensions_data, g->extensions_len, &gc);
    if (rc != 0) return rc;
    const uint8_t *ngid = NULL, *keys = NULL;
    MlsRelaySpan spans[MARMOT_NOSTR_ROUTING_MAX_RELAYS];
    size_t n_relays = 0, n_admins = 0;
    if (!gc.routing || !gc.admins ||
        mls_nostr_routing_v1_decode(gc.routing, gc.routing_len, &ngid, spans, &n_relays) != 0 ||
        mls_admin_policy_v1_decode(gc.admins, gc.admins_len, &keys, &n_admins) != 0)
        return MARMOT_ERR_EXTENSION_FORMAT;

    MarmotGroup *group = marmot_group_new();
    char **relays = calloc(n_relays, sizeof(*relays));
    if (!group || !relays) goto oom;
    group->mls_group_id = marmot_group_id_new(g->group_id, g->group_id_len);
    if (!group->mls_group_id.data) goto oom;
    group->epoch = g->epoch;
    group->state = MARMOT_GROUP_STATE_ACTIVE;
    memcpy(group->nostr_group_id, ngid, 32);

    const uint8_t *name = (const uint8_t *)"", *desc = (const uint8_t *)"";
    size_t name_len = 0, desc_len = 0;
    if (gc.profile &&
        mls_group_profile_v1_decode(gc.profile, gc.profile_len, &name, &name_len, &desc,
                                    &desc_len) != 0) {
        marmot_group_free(group);
        free(relays);
        return MARMOT_ERR_EXTENSION_FORMAT;
    }
    group->name = dup_bytes(name, name_len);
    group->description = dup_bytes(desc, desc_len);
    group->admin_pubkeys = malloc(n_admins * 32);
    if (!group->name || !group->description || !group->admin_pubkeys) goto oom;
    memcpy(group->admin_pubkeys, keys, n_admins * 32);
    group->admin_count = n_admins;

    for (size_t i = 0; i < n_relays; i++) {
        relays[i] = dup_bytes(spans[i].url, spans[i].len);
        if (!relays[i]) {
            marmot_adopted_relays_free(relays, i);
            relays = NULL;
            goto oom;
        }
    }
    *group_out = group;
    *relays_out = relays;
    *relay_count_out = n_relays;
    return MARMOT_OK;
oom:
    marmot_group_free(group);
    free(relays);
    return MARMOT_ERR_MEMORY;
}

MarmotError
marmot_adopted_stored_profile(Marmot *m, const MarmotGroupId *gid, MarmotGroupProfile *out)
{
    if (!m || !gid || !gid->data || !out) return MARMOT_ERR_INVALID_ARG;
    if (!m->storage || !m->storage->mls_load) return MARMOT_ERR_STORAGE;
    uint8_t *blob = NULL;
    size_t len = 0;
    MarmotError err = m->storage->mls_load(m->storage->ctx, "mls_group", gid->data, gid->len,
                                           &blob, &len);
    if (err != MARMOT_OK || !blob) {
        free(blob);
        return err == MARMOT_OK || err == MARMOT_ERR_STORAGE_NOT_FOUND
                   ? MARMOT_ERR_GROUP_NOT_FOUND : err;
    }
    MlsGroup g;
    int rc = mls_group_deserialize(blob, len, &g);
    sodium_memzero(blob, len);
    free(blob);
    if (rc != 0) return MARMOT_ERR_DESERIALIZATION;
    *out = g.profile;
    mls_group_free(&g);
    return MARMOT_OK;
}
