/*
 * marmot-gobject - GObject wrapper for libmarmot
 *
 * GEnum/GFlags type registrations for GObject introspection.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_GOBJECT_ENUMS_H
#define MARMOT_GOBJECT_ENUMS_H

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * MarmotGobjectGroupState:
 * @MARMOT_GOBJECT_GROUP_STATE_ACTIVE: Group is active
 * @MARMOT_GOBJECT_GROUP_STATE_INACTIVE: Group is inactive (left/removed)
 * @MARMOT_GOBJECT_GROUP_STATE_PENDING: Group creation pending commit merge
 *
 * State of a Marmot group.
 */
typedef enum {
    MARMOT_GOBJECT_GROUP_STATE_ACTIVE   = 0,
    MARMOT_GOBJECT_GROUP_STATE_INACTIVE = 1,
    MARMOT_GOBJECT_GROUP_STATE_PENDING  = 2,
} MarmotGobjectGroupState;

GType marmot_gobject_group_state_get_type(void) G_GNUC_CONST;
#define MARMOT_GOBJECT_TYPE_GROUP_STATE (marmot_gobject_group_state_get_type())

/**
 * MarmotGobjectMessageState:
 * @MARMOT_GOBJECT_MESSAGE_STATE_CREATED: Message created/pending
 * @MARMOT_GOBJECT_MESSAGE_STATE_PROCESSED: Message processed
 * @MARMOT_GOBJECT_MESSAGE_STATE_DELETED: Message deleted
 * @MARMOT_GOBJECT_MESSAGE_STATE_EPOCH_INVALIDATED: Message invalidated by epoch change
 *
 * State of a Marmot message.
 */
typedef enum {
    MARMOT_GOBJECT_MESSAGE_STATE_CREATED            = 0,
    MARMOT_GOBJECT_MESSAGE_STATE_PROCESSED          = 1,
    MARMOT_GOBJECT_MESSAGE_STATE_DELETED             = 2,
    MARMOT_GOBJECT_MESSAGE_STATE_EPOCH_INVALIDATED  = 3,
} MarmotGobjectMessageState;

GType marmot_gobject_message_state_get_type(void) G_GNUC_CONST;
#define MARMOT_GOBJECT_TYPE_MESSAGE_STATE (marmot_gobject_message_state_get_type())

/**
 * MarmotGobjectWelcomeState:
 * @MARMOT_GOBJECT_WELCOME_STATE_PENDING: Welcome pending user action
 * @MARMOT_GOBJECT_WELCOME_STATE_ACCEPTED: Welcome accepted
 * @MARMOT_GOBJECT_WELCOME_STATE_DECLINED: Welcome declined
 *
 * State of a Marmot welcome.
 */
typedef enum {
    MARMOT_GOBJECT_WELCOME_STATE_PENDING  = 0,
    MARMOT_GOBJECT_WELCOME_STATE_ACCEPTED = 1,
    MARMOT_GOBJECT_WELCOME_STATE_DECLINED = 2,
} MarmotGobjectWelcomeState;

GType marmot_gobject_welcome_state_get_type(void) G_GNUC_CONST;
#define MARMOT_GOBJECT_TYPE_WELCOME_STATE (marmot_gobject_welcome_state_get_type())

/**
 * MarmotGobjectMessageResultType:
 * @MARMOT_GOBJECT_MESSAGE_RESULT_APPLICATION: Decrypted application message
 * @MARMOT_GOBJECT_MESSAGE_RESULT_COMMIT: Group state change (commit)
 * @MARMOT_GOBJECT_MESSAGE_RESULT_PROPOSAL: Group change proposal
 * @MARMOT_GOBJECT_MESSAGE_RESULT_UNPROCESSABLE: Message could not be processed
 * @MARMOT_GOBJECT_MESSAGE_RESULT_OWN_MESSAGE: Our own message (skip)
 */
typedef enum {
    MARMOT_GOBJECT_MESSAGE_RESULT_APPLICATION   = 0,
    MARMOT_GOBJECT_MESSAGE_RESULT_COMMIT        = 1,
    MARMOT_GOBJECT_MESSAGE_RESULT_PROPOSAL      = 2,
    MARMOT_GOBJECT_MESSAGE_RESULT_UNPROCESSABLE = 3,
    MARMOT_GOBJECT_MESSAGE_RESULT_OWN_MESSAGE   = 4,
} MarmotGobjectMessageResultType;

GType marmot_gobject_message_result_type_get_type(void) G_GNUC_CONST;
#define MARMOT_GOBJECT_TYPE_MESSAGE_RESULT_TYPE (marmot_gobject_message_result_type_get_type())

G_END_DECLS

#endif /* MARMOT_GOBJECT_ENUMS_H */
