/*
 * libmarmot - Core type constructors and destructors
 *
 * SPDX-License-Identifier: MIT
 */

#include <marmot/marmot-types.h>
#include <stdlib.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────────────────────
 * MarmotGroupId
 * ──────────────────────────────────────────────────────────────────────── */

MarmotGroupId
marmot_group_id_new(const uint8_t *data, size_t len)
{
    MarmotGroupId gid = { .data = NULL, .len = 0 };
    if (!data || len == 0)
        return gid;
    gid.data = malloc(len);
    if (!gid.data)
        return gid;
    memcpy(gid.data, data, len);
    gid.len = len;
    return gid;
}

void
marmot_group_id_free(MarmotGroupId *gid)
{
    if (!gid) return;
    free(gid->data);
    gid->data = NULL;
    gid->len = 0;
}

bool
marmot_group_id_equal(const MarmotGroupId *a, const MarmotGroupId *b)
{
    if (!a || !b) return false;
    if (a->len != b->len) return false;
    if (a->len == 0) return true;
    return memcmp(a->data, b->data, a->len) == 0;
}

char *
marmot_group_id_to_hex(const MarmotGroupId *gid)
{
    static const char hex_chars[] = "0123456789abcdef";
    if (!gid || !gid->data || gid->len == 0)
        return NULL;
    char *out = malloc(gid->len * 2 + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < gid->len; i++) {
        out[i * 2]     = hex_chars[(gid->data[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex_chars[gid->data[i] & 0x0f];
    }
    out[gid->len * 2] = '\0';
    return out;
}

/* ──────────────────────────────────────────────────────────────────────────
 * MarmotConfig
 * ──────────────────────────────────────────────────────────────────────── */

MarmotConfig
marmot_config_default(void)
{
    return (MarmotConfig){
        .max_event_age_secs      = 3888000,   /* 45 days */
        .max_future_skew_secs    = 300,        /* 5 minutes */
        .out_of_order_tolerance  = 100,
        .max_forward_distance    = 1000,
        .epoch_snapshot_retention = 5,
        .snapshot_ttl_seconds    = 604800,     /* 1 week */
    };
}

/* ──────────────────────────────────────────────────────────────────────────
 * Group state strings
 * ──────────────────────────────────────────────────────────────────────── */

const char *
marmot_group_state_to_string(MarmotGroupState state)
{
    switch (state) {
    case MARMOT_GROUP_STATE_ACTIVE:   return "active";
    case MARMOT_GROUP_STATE_INACTIVE: return "inactive";
    case MARMOT_GROUP_STATE_PENDING:  return "pending";
    default: return "unknown";
    }
}

MarmotGroupState
marmot_group_state_from_string(const char *s)
{
    if (!s) return MARMOT_GROUP_STATE_INACTIVE;
    if (strcmp(s, "active") == 0)   return MARMOT_GROUP_STATE_ACTIVE;
    if (strcmp(s, "pending") == 0)  return MARMOT_GROUP_STATE_PENDING;
    return MARMOT_GROUP_STATE_INACTIVE;
}

/* ──────────────────────────────────────────────────────────────────────────
 * MarmotGroup
 * ──────────────────────────────────────────────────────────────────────── */

MarmotGroup *
marmot_group_new(void)
{
    MarmotGroup *g = calloc(1, sizeof(MarmotGroup));
    return g;
}

void
marmot_group_free(MarmotGroup *group)
{
    if (!group) return;
    marmot_group_id_free(&group->mls_group_id);
    free(group->name);
    free(group->description);
    free(group->image_hash);
    free(group->image_key);
    free(group->image_nonce);
    free(group->admin_pubkeys);
    free(group->last_message_id);
    free(group);
}

/* ──────────────────────────────────────────────────────────────────────────
 * MarmotMessage
 * ──────────────────────────────────────────────────────────────────────── */

MarmotMessage *
marmot_message_new(void)
{
    MarmotMessage *m = calloc(1, sizeof(MarmotMessage));
    return m;
}

void
marmot_message_free(MarmotMessage *msg)
{
    if (!msg) return;
    marmot_group_id_free(&msg->mls_group_id);
    free(msg->content);
    free(msg->tags_json);
    free(msg->event_json);
    free(msg);
}

/* ──────────────────────────────────────────────────────────────────────────
 * MarmotWelcome
 * ──────────────────────────────────────────────────────────────────────── */

MarmotWelcome *
marmot_welcome_new(void)
{
    MarmotWelcome *w = calloc(1, sizeof(MarmotWelcome));
    return w;
}

void
marmot_welcome_free(MarmotWelcome *welcome)
{
    if (!welcome) return;
    free(welcome->event_json);
    marmot_group_id_free(&welcome->mls_group_id);
    free(welcome->group_name);
    free(welcome->group_description);
    free(welcome->group_image_hash);
    free(welcome->group_admin_pubkeys);
    if (welcome->group_relays) {
        for (size_t i = 0; i < welcome->group_relay_count; i++)
            free(welcome->group_relays[i]);
        free(welcome->group_relays);
    }
    free(welcome);
}

/* ──────────────────────────────────────────────────────────────────────────
 * MarmotPagination
 * ──────────────────────────────────────────────────────────────────────── */

MarmotPagination
marmot_pagination_default(void)
{
    return (MarmotPagination){
        .limit      = 1000,
        .offset     = 0,
        .sort_order = MARMOT_SORT_CREATED_AT_FIRST,
    };
}

/* ──────────────────────────────────────────────────────────────────────────
 * Result types cleanup
 * ──────────────────────────────────────────────────────────────────────── */

void
marmot_message_result_free(MarmotMessageResult *result)
{
    if (!result) return;
    switch (result->type) {
    case MARMOT_RESULT_APPLICATION_MESSAGE:
        free(result->app_msg.inner_event_json);
        free(result->app_msg.sender_pubkey_hex);
        break;
    case MARMOT_RESULT_COMMIT:
        marmot_group_free(result->commit.updated_group);
        break;
    default:
        break;
    }
    memset(result, 0, sizeof(*result));
}

void
marmot_create_group_result_free(MarmotCreateGroupResult *result)
{
    if (!result) return;
    marmot_group_free(result->group);
    if (result->welcome_rumor_jsons) {
        for (size_t i = 0; i < result->welcome_count; i++)
            free(result->welcome_rumor_jsons[i]);
        free(result->welcome_rumor_jsons);
    }
    free(result->evolution_event_json);
    memset(result, 0, sizeof(*result));
}

void
marmot_key_package_result_free(MarmotKeyPackageResult *result)
{
    if (!result) return;
    free(result->event_json);
    memset(result, 0, sizeof(*result));
}

void
marmot_outgoing_message_free(MarmotOutgoingMessage *result)
{
    if (!result) return;
    free(result->event_json);
    marmot_message_free(result->message);
    memset(result, 0, sizeof(*result));
}
