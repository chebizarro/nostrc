#ifndef NOSTR_NIP29_H
#define NOSTR_NIP29_H

#include "event.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NOSTR_PERMISSION_ADD_USER,
    NOSTR_PERMISSION_EDIT_METADATA,
    NOSTR_PERMISSION_DELETE_EVENT,
    NOSTR_PERMISSION_REMOVE_USER,
    NOSTR_PERMISSION_ADD_PERMISSION,
    NOSTR_PERMISSION_REMOVE_PERMISSION,
    NOSTR_PERMISSION_EDIT_GROUP_STATUS
} nostr_permission_t;

typedef struct {
    char *name;
    nostr_permission_t *permissions;
    size_t permissions_len;
} nostr_role_t;

typedef struct {
    char *relay;
    char *id;
} nostr_group_address_t;

typedef struct {
    nostr_group_address_t address;
    char *name;
    char *picture;
    char *about;
    nostr_role_t *members;
    size_t members_len;
    bool is_private;
    bool is_closed;
    nostr_timestamp_t last_metadata_update;
    nostr_timestamp_t last_admins_update;
    nostr_timestamp_t last_members_update;
} nostr_group_t;

nostr_group_t *nostr_new_group(const char *gadstr);
void nostr_free_group(nostr_group_t *group);
nostr_event_t *nostr_group_to_metadata_event(const nostr_group_t *group);
nostr_event_t *nostr_group_to_admins_event(const nostr_group_t *group);
nostr_event_t *nostr_group_to_members_event(const nostr_group_t *group);
bool nostr_group_merge_in_metadata_event(nostr_group_t *group, const nostr_event_t *event);
bool nostr_group_merge_in_admins_event(nostr_group_t *group, const nostr_event_t *event);
bool nostr_group_merge_in_members_event(nostr_group_t *group, const nostr_event_t *event);

#endif // NOSTR_NIP29_H
