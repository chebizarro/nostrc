#ifndef NOSTR_NIP29_H
#define NOSTR_NIP29_H

#include "nostr-event.h"
#include "nostr-timestamp.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compatibility aliases for the older lowercase API spelling. */
typedef NostrEvent nostr_event_t;
typedef NostrTimestamp nostr_timestamp_t;

typedef enum {
    NOSTR_PERMISSION_UNKNOWN = -1,
    NOSTR_PERMISSION_PUT_USER = 0,
    NOSTR_PERMISSION_REMOVE_USER,
    NOSTR_PERMISSION_EDIT_METADATA,
    NOSTR_PERMISSION_DELETE_EVENT,
    NOSTR_PERMISSION_CREATE_GROUP,
    NOSTR_PERMISSION_DELETE_GROUP,
    NOSTR_PERMISSION_CREATE_INVITE,
    NOSTR_PERMISSION_UPDATE_PIN_LIST,

    /* Deprecated name retained as an alias for callers that still compile
     * against the old API; serialization always emits "put-user". */
    NOSTR_PERMISSION_ADD_USER = NOSTR_PERMISSION_PUT_USER
} nostr_permission_t;

typedef struct {
    char *relay;
    char *id;
} nostr_group_address_t;

typedef struct {
    char *pubkey;
    char **roles;
    size_t roles_len;
} nostr_group_admin_t;

typedef struct {
    char *pubkey;
    char *label;
} nostr_group_member_t;

typedef struct {
    char *name;
    char *description;
} nostr_group_role_t;

typedef nostr_group_role_t nostr_role_t;

/* One entry of a kind:39005 pinned-events list (mirrors kind:9010). */
typedef enum {
    NOSTR_GROUP_PIN_EVENT = 0, /* ["e", <64-char lowercase hex id>] */
    NOSTR_GROUP_PIN_ADDRESS,   /* ["a", "<kind>:<pubkey hex>:<d>"] */
} nostr_group_pin_type_t;

typedef struct {
    nostr_group_pin_type_t type;
    char *value;
} nostr_group_pin_t;

typedef struct {
    nostr_group_address_t address;

    char *name;
    char *picture;
    char *banner;
    char *about;

    bool is_private;
    bool is_restricted;
    bool is_hidden;
    bool is_closed;

    /* Subgroups (kind:39000 `parent` / `child` tags). parent == NULL means
     * a root group; children keeps the relay's display order. */
    char *parent;
    char **children;
    size_t children_len;

    nostr_group_admin_t *admins;
    size_t admins_len;
    bool admins_loaded;

    nostr_group_member_t *members;
    size_t members_len;
    bool members_loaded;
    bool members_may_be_partial;

    nostr_group_role_t *roles;
    size_t roles_len;
    bool roles_loaded;

    /* kind:39005, in display order. */
    nostr_group_pin_t *pins;
    size_t pins_len;
    bool pins_loaded;

    nostr_timestamp_t last_metadata_update;
    nostr_timestamp_t last_admins_update;
    nostr_timestamp_t last_members_update;
    nostr_timestamp_t last_roles_update;
    nostr_timestamp_t last_pins_update;
} nostr_group_t;

bool nostr_group_address_parse(const char *raw, nostr_group_address_t *out);
bool nostr_group_address_is_valid(const nostr_group_address_t *address);
char *nostr_group_address_to_string(const nostr_group_address_t *address);
void nostr_group_address_clear(nostr_group_address_t *address);

/*
 * Parse a user-supplied group reference: a NIP-29 `naddr1...` (optionally
 * `nostr:`-prefixed) pointing at the group's kind:39000 with a relay hint,
 * or the legacy "relay'id" form. Either may carry the NIP-29 invite suffix
 * "?invite=<code>" (percent-decoded); other query parameters are ignored.
 * On success *out_invite_code (if non-NULL) receives a malloc'd code, or
 * NULL when the reference carries none.
 */
bool nostr_group_reference_parse(const char *raw, nostr_group_address_t *out,
                                 char **out_invite_code);

nostr_group_t *nostr_new_group(const char *gadstr);
void nostr_free_group(nostr_group_t *group);

nostr_event_t *nostr_group_to_metadata_event(const nostr_group_t *group);
nostr_event_t *nostr_group_to_admins_event(const nostr_group_t *group);
nostr_event_t *nostr_group_to_members_event(const nostr_group_t *group);
nostr_event_t *nostr_group_to_roles_event(const nostr_group_t *group);
nostr_event_t *nostr_group_to_pins_event(const nostr_group_t *group);

bool nostr_group_merge_in_metadata_event(nostr_group_t *group, const nostr_event_t *event);
bool nostr_group_merge_in_admins_event(nostr_group_t *group, const nostr_event_t *event);
bool nostr_group_merge_in_members_event(nostr_group_t *group, const nostr_event_t *event);
bool nostr_group_merge_in_roles_event(nostr_group_t *group, const nostr_event_t *event);
bool nostr_group_merge_in_pins_event(nostr_group_t *group, const nostr_event_t *event);

bool nostr_group_is_root(const nostr_group_t *group);
bool nostr_group_pin_is_valid(nostr_group_pin_type_t type, const char *value);

nostr_group_admin_t *nostr_group_get_admin(const nostr_group_t *group, const char *pubkey);
nostr_group_member_t *nostr_group_get_member(const nostr_group_t *group, const char *pubkey);
nostr_group_role_t *nostr_group_get_role(const nostr_group_t *group, const char *name);

nostr_group_admin_t *nostr_group_add_admin(nostr_group_t *group, const char *pubkey);
bool nostr_group_admin_add_role(nostr_group_admin_t *admin, const char *role);
nostr_group_member_t *nostr_group_add_member(nostr_group_t *group, const char *pubkey, const char *label);
nostr_group_role_t *nostr_group_add_role(nostr_group_t *group, const char *name, const char *description);
nostr_group_pin_t *nostr_group_add_pin(nostr_group_t *group, nostr_group_pin_type_t type, const char *value);

const char *nostr_permission_to_string(nostr_permission_t perm);
nostr_permission_t nostr_permission_from_string(const char *str);

#ifdef __cplusplus
}
#endif

#endif // NOSTR_NIP29_H
