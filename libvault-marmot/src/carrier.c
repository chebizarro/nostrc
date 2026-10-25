/* SPDX-License-Identifier: MIT */
#include <vault-marmot/vault-marmot.h>
#include <nostr-event.h>
#include <nostr-tag.h>
#include <openssl/crypto.h>
#include <stdlib.h>
#include <string.h>

struct vault_marmot_carrier {
    vault_grant_carrier iface;
    Marmot *marmot;
    MarmotGroupId group;
    uint8_t vault_id[32];
    vault_marmot_transport io;
};

static void wipe(char *s)
{
    if (s)
        OPENSSL_cleanse(s, strlen(s));
}

static int active_group(vault_marmot_carrier *c, MarmotGroup **out)
{
    *out = NULL;
    if (marmot_get_group(c->marmot, &c->group, out) != MARMOT_OK || !*out)
        return VAULT_ERR_AUTH;
    if ((*out)->state != MARMOT_GROUP_STATE_ACTIVE) {
        marmot_group_free(*out);
        *out = NULL;
        return VAULT_ERR_AUTH;
    }
    return 0;
}

static int proven_member(vault_marmot_carrier *c, const uint8_t pubkey[32])
{
    MarmotMemberIdentity *members = NULL;
    size_t count = 0;
    if (marmot_get_group_member_identities(c->marmot, &c->group, &members, &count) != MARMOT_OK)
        return VAULT_ERR_AUTH;
    int rc = VAULT_ERR_AUTH;
    for (size_t i = 0; i < count; ++i) {
        if (memcmp(members[i].account_pubkey, pubkey, 32))
            continue;
        /* MessageResult identifies the account, not the sending leaf. Never
         * let another device's valid proof authenticate an unproven leaf. */
        if (members[i].status != MARMOT_MEMBER_IDENTITY_PROVEN) {
            free(members);
            return VAULT_ERR_AUTH;
        }
        rc = 0;
    }
    free(members);
    return rc;
}

static int check_grant(vault_marmot_carrier *c, const NostrEvent *rumor)
{
    vault_grant *g = NULL;
    int rc = vault_grant_rumor_open(rumor, &g);
    if (!rc && memcmp(g->vault_id, c->vault_id, 32))
        rc = VAULT_ERR_AUTH;
    vault_grant_free(g);
    return rc;
}

static int send_grant(void *ctx, const uint8_t recipient[32], const NostrEvent *rumor)
{
    vault_marmot_carrier *c = ctx;
    if (!c->io.publish || !recipient || !rumor)
        return VAULT_ERR_ARG;
    int rc = check_grant(c, rumor);
    if (rc)
        return rc;
    uint8_t author[32];
    if (porthome_from_hex64(rumor->pubkey, author) || proven_member(c, author) ||
        proven_member(c, recipient))
        return VAULT_ERR_AUTH;
    MarmotGroup *group = NULL;
    rc = active_group(c, &group);
    marmot_group_free(group);
    if (rc)
        return rc;
    char *json = nostr_event_serialize_compact(rumor);
    if (!json)
        return VAULT_ERR_OOM;
    MarmotOutgoingMessage msg = {0};
    /* libmarmot binds the inner author to our own MLS leaf, signs a fresh
     * ephemeral kind-445 envelope and commits the advanced sender ratchet. */
    MarmotError err = marmot_create_message(c->marmot, &c->group, json, &msg);
    wipe(json);
    free(json);
    rc = VAULT_ERR_AUTH;
    NostrEvent *event = NULL;
    if (err == MARMOT_OK) {
        event = nostr_event_new();
        if (!event)
            rc = VAULT_ERR_OOM;
        else if (msg.event_json &&
                 nostr_event_deserialize_signed(event, msg.event_json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
                 event->kind == MARMOT_KIND_GROUP_MESSAGE)
            rc = c->io.publish(c->io.ctx, event);
    }
    nostr_event_free(event);
    if (msg.message) {
        wipe(msg.message->content);
        wipe(msg.message->event_json);
    }
    marmot_outgoing_message_free(&msg);
    return rc;
}

static int matches_group(const NostrEvent *event, const MarmotGroup *group)
{
    char expected[65];
    porthome_hex64(group->nostr_group_id, expected);
    size_t matches = 0;
    for (size_t i = 0; i < nostr_tags_size(event->tags); ++i) {
        const NostrTag *tag = nostr_tags_get(event->tags, i);
        const char *key = nostr_tag_get_key(tag);
        if (key && !strcmp(key, "h")) {
            const char *value = nostr_tag_get_value(tag);
            if (!value || strcmp(value, expected))
                return 0;
            ++matches;
        }
    }
    return matches == 1;
}

static int receive_grant(void *ctx, NostrEvent **out, uint8_t sender[32])
{
    vault_marmot_carrier *c = ctx;
    if (out)
        *out = NULL;
    if (sender)
        memset(sender, 0, 32);
    if (!c->io.receive || !out || !sender)
        return VAULT_ERR_ARG;
    NostrEvent *event = NULL, *rumor = NULL;
    MarmotGroup *group = NULL;
    MarmotMessageResult result = {0};
    char *json = NULL;
    uint8_t authenticated[32] = {0};
    int rc = c->io.receive(c->io.ctx, &event);
    if (rc)
        goto done;
    rc = VAULT_ERR_AUTH;
    if (!event || event->kind != MARMOT_KIND_GROUP_MESSAGE ||
        active_group(c, &group) || !matches_group(event, group))
        goto done;
    json = nostr_event_serialize_compact(event);
    if (!json) {
        rc = VAULT_ERR_OOM;
        goto done;
    }
    if (marmot_process_message(c->marmot, json, &result) != MARMOT_OK)
        goto done;
    if (result.type != MARMOT_RESULT_APPLICATION_MESSAGE) {
        rc = VAULT_ERR_AGAIN;
        goto done;
    }
    /* Current identities cannot authenticate a leaf from a retained epoch. */
    if (result.app_msg.epoch != group->epoch)
        goto done;
    if (!result.app_msg.inner_event_json || !result.app_msg.sender_pubkey_hex ||
        porthome_from_hex64(result.app_msg.sender_pubkey_hex, authenticated) ||
        proven_member(c, authenticated))
        goto done;
    rumor = nostr_event_new();
    if (!rumor) {
        rc = VAULT_ERR_OOM;
        goto done;
    }
    if (!nostr_event_deserialize_compact(rumor, result.app_msg.inner_event_json, NULL) ||
        check_grant(c, rumor))
        goto done;
    uint8_t author[32];
    if (porthome_from_hex64(rumor->pubkey, author) || memcmp(author, authenticated, 32))
        goto done;
    memcpy(sender, authenticated, 32);
    *out = rumor;
    rumor = NULL;
    rc = 0;
done:
    vault_grant_rumor_free(rumor);
    if (result.type == MARMOT_RESULT_APPLICATION_MESSAGE)
        wipe(result.app_msg.inner_event_json);
    marmot_message_result_free(&result);
    marmot_group_free(group);
    free(json);
    nostr_event_free(event);
    return rc;
}

int vault_marmot_carrier_new(Marmot *m, const MarmotGroupId *group, const uint8_t vault_id[32],
                              const vault_marmot_transport *io, vault_marmot_carrier **out)
{
    if (out)
        *out = NULL;
    if (!out || !m || !group || !group->data || !group->len || !vault_id || !io ||
        (!io->publish && !io->receive))
        return VAULT_ERR_ARG;
    vault_marmot_carrier *c = calloc(1, sizeof *c);
    if (!c)
        return VAULT_ERR_OOM;
    c->marmot = m;
    c->group = marmot_group_id_new(group->data, group->len);
    if (!c->group.data) {
        free(c);
        return VAULT_ERR_OOM;
    }
    memcpy(c->vault_id, vault_id, 32);
    c->io = *io;
    c->iface = (vault_grant_carrier){c, send_grant, receive_grant};
    *out = c;
    return 0;
}

const vault_grant_carrier *vault_marmot_carrier_interface(vault_marmot_carrier *c)
{
    return c ? &c->iface : NULL;
}

void vault_marmot_carrier_free(vault_marmot_carrier *c)
{
    if (!c)
        return;
    marmot_group_id_free(&c->group);
    OPENSSL_cleanse(c, sizeof *c);
    free(c);
}

int vault_marmot_prefer(vault_marmot_key_package_status status, const vault_grant_carrier *marmot,
                         const vault_grant_carrier *nip59, const vault_grant_carrier **out)
{
    if (!out)
        return VAULT_ERR_ARG;
    *out = NULL;
    if (status == VAULT_MARMOT_KP_UNKNOWN)
        return VAULT_ERR_AGAIN;
    if (status != VAULT_MARMOT_KP_PRESENT && status != VAULT_MARMOT_KP_ABSENT)
        return VAULT_ERR_ARG;
    const vault_grant_carrier *selected = status == VAULT_MARMOT_KP_PRESENT ? marmot : nip59;
    if (!selected || !selected->send)
        return VAULT_ERR_ARG;
    *out = selected;
    return 0;
}
