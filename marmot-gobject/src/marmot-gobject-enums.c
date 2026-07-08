/*
 * marmot-gobject - GEnum type registrations
 *
 * SPDX-License-Identifier: MIT
 */

#include "marmot-gobject-1.0/marmot-gobject-enums.h"

/* ── Explicit libmarmot → GObject enum conversions ──────────────── */

MarmotGobjectGroupState
marmot_gobject_group_state_from_marmot(MarmotGroupState state)
{
    switch (state) {
    case MARMOT_GROUP_STATE_ACTIVE:   return MARMOT_GOBJECT_GROUP_STATE_ACTIVE;
    case MARMOT_GROUP_STATE_INACTIVE: return MARMOT_GOBJECT_GROUP_STATE_INACTIVE;
    case MARMOT_GROUP_STATE_PENDING:  return MARMOT_GOBJECT_GROUP_STATE_PENDING;
    default:                          return MARMOT_GOBJECT_GROUP_STATE_INACTIVE;
    }
}

MarmotGobjectMessageState
marmot_gobject_message_state_from_marmot(MarmotMessageState state)
{
    switch (state) {
    case MARMOT_MSG_STATE_CREATED:           return MARMOT_GOBJECT_MESSAGE_STATE_CREATED;
    case MARMOT_MSG_STATE_PROCESSED:         return MARMOT_GOBJECT_MESSAGE_STATE_PROCESSED;
    case MARMOT_MSG_STATE_DELETED:           return MARMOT_GOBJECT_MESSAGE_STATE_DELETED;
    case MARMOT_MSG_STATE_EPOCH_INVALIDATED: return MARMOT_GOBJECT_MESSAGE_STATE_EPOCH_INVALIDATED;
    default:                                 return MARMOT_GOBJECT_MESSAGE_STATE_DELETED;
    }
}

MarmotGobjectWelcomeState
marmot_gobject_welcome_state_from_marmot(MarmotWelcomeState state)
{
    switch (state) {
    case MARMOT_WELCOME_STATE_PENDING:  return MARMOT_GOBJECT_WELCOME_STATE_PENDING;
    case MARMOT_WELCOME_STATE_ACCEPTED: return MARMOT_GOBJECT_WELCOME_STATE_ACCEPTED;
    case MARMOT_WELCOME_STATE_DECLINED: return MARMOT_GOBJECT_WELCOME_STATE_DECLINED;
    default:                            return MARMOT_GOBJECT_WELCOME_STATE_DECLINED;
    }
}

MarmotGobjectMessageResultType
marmot_gobject_message_result_type_from_marmot(MarmotMessageResultType type)
{
    switch (type) {
    case MARMOT_RESULT_APPLICATION_MESSAGE: return MARMOT_GOBJECT_MESSAGE_RESULT_APPLICATION;
    case MARMOT_RESULT_COMMIT:              return MARMOT_GOBJECT_MESSAGE_RESULT_COMMIT;
    case MARMOT_RESULT_PROPOSAL:            return MARMOT_GOBJECT_MESSAGE_RESULT_PROPOSAL;
    case MARMOT_RESULT_UNPROCESSABLE:       return MARMOT_GOBJECT_MESSAGE_RESULT_UNPROCESSABLE;
    case MARMOT_RESULT_OWN_MESSAGE:         return MARMOT_GOBJECT_MESSAGE_RESULT_OWN_MESSAGE;
    default:                                return MARMOT_GOBJECT_MESSAGE_RESULT_UNPROCESSABLE;
    }
}

/* ── MarmotGobjectGroupState ─────────────────────────────────────── */

static const GEnumValue group_state_values[] = {
    { MARMOT_GOBJECT_GROUP_STATE_ACTIVE,   "MARMOT_GOBJECT_GROUP_STATE_ACTIVE",   "active"   },
    { MARMOT_GOBJECT_GROUP_STATE_INACTIVE, "MARMOT_GOBJECT_GROUP_STATE_INACTIVE", "inactive" },
    { MARMOT_GOBJECT_GROUP_STATE_PENDING,  "MARMOT_GOBJECT_GROUP_STATE_PENDING",  "pending"  },
    { 0, NULL, NULL }
};

GType
marmot_gobject_group_state_get_type(void)
{
    static gsize g_type_id = 0;
    if (g_once_init_enter(&g_type_id)) {
        GType id = g_enum_register_static("MarmotGobjectGroupState", group_state_values);
        g_once_init_leave(&g_type_id, id);
    }
    return (GType)g_type_id;
}

/* ── MarmotGobjectMessageState ───────────────────────────────────── */

static const GEnumValue message_state_values[] = {
    { MARMOT_GOBJECT_MESSAGE_STATE_CREATED,           "MARMOT_GOBJECT_MESSAGE_STATE_CREATED",           "created"           },
    { MARMOT_GOBJECT_MESSAGE_STATE_PROCESSED,         "MARMOT_GOBJECT_MESSAGE_STATE_PROCESSED",         "processed"         },
    { MARMOT_GOBJECT_MESSAGE_STATE_DELETED,            "MARMOT_GOBJECT_MESSAGE_STATE_DELETED",            "deleted"            },
    { MARMOT_GOBJECT_MESSAGE_STATE_EPOCH_INVALIDATED, "MARMOT_GOBJECT_MESSAGE_STATE_EPOCH_INVALIDATED", "epoch-invalidated" },
    { 0, NULL, NULL }
};

GType
marmot_gobject_message_state_get_type(void)
{
    static gsize g_type_id = 0;
    if (g_once_init_enter(&g_type_id)) {
        GType id = g_enum_register_static("MarmotGobjectMessageState", message_state_values);
        g_once_init_leave(&g_type_id, id);
    }
    return (GType)g_type_id;
}

/* ── MarmotGobjectWelcomeState ───────────────────────────────────── */

static const GEnumValue welcome_state_values[] = {
    { MARMOT_GOBJECT_WELCOME_STATE_PENDING,  "MARMOT_GOBJECT_WELCOME_STATE_PENDING",  "pending"  },
    { MARMOT_GOBJECT_WELCOME_STATE_ACCEPTED, "MARMOT_GOBJECT_WELCOME_STATE_ACCEPTED", "accepted" },
    { MARMOT_GOBJECT_WELCOME_STATE_DECLINED, "MARMOT_GOBJECT_WELCOME_STATE_DECLINED", "declined" },
    { 0, NULL, NULL }
};

GType
marmot_gobject_welcome_state_get_type(void)
{
    static gsize g_type_id = 0;
    if (g_once_init_enter(&g_type_id)) {
        GType id = g_enum_register_static("MarmotGobjectWelcomeState", welcome_state_values);
        g_once_init_leave(&g_type_id, id);
    }
    return (GType)g_type_id;
}

/* ── MarmotGobjectMessageResultType ──────────────────────────────── */

static const GEnumValue message_result_type_values[] = {
    { MARMOT_GOBJECT_MESSAGE_RESULT_APPLICATION,   "MARMOT_GOBJECT_MESSAGE_RESULT_APPLICATION",   "application"   },
    { MARMOT_GOBJECT_MESSAGE_RESULT_COMMIT,        "MARMOT_GOBJECT_MESSAGE_RESULT_COMMIT",        "commit"        },
    { MARMOT_GOBJECT_MESSAGE_RESULT_PROPOSAL,      "MARMOT_GOBJECT_MESSAGE_RESULT_PROPOSAL",      "proposal"      },
    { MARMOT_GOBJECT_MESSAGE_RESULT_UNPROCESSABLE, "MARMOT_GOBJECT_MESSAGE_RESULT_UNPROCESSABLE", "unprocessable" },
    { MARMOT_GOBJECT_MESSAGE_RESULT_OWN_MESSAGE,   "MARMOT_GOBJECT_MESSAGE_RESULT_OWN_MESSAGE",   "own-message"   },
    { 0, NULL, NULL }
};

GType
marmot_gobject_message_result_type_get_type(void)
{
    static gsize g_type_id = 0;
    if (g_once_init_enter(&g_type_id)) {
        GType id = g_enum_register_static("MarmotGobjectMessageResultType", message_result_type_values);
        g_once_init_leave(&g_type_id, id);
    }
    return (GType)g_type_id;
}
