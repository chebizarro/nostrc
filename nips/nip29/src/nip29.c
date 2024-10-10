#include "nostr/nip29.h"
#include "event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

nostr_group_t *nostr_new_group(const char *gadstr) {
    nostr_group_address_t gad;
    if (sscanf(gadstr, "%m[^']'%ms", &gad.relay, &gad.id) != 2) {
        return NULL;
    }
    nostr_group_t *group = malloc(sizeof(nostr_group_t));
    if (!group) {
        free(gad.relay);
        free(gad.id);
        return NULL;
    }
    group->address = gad;
    group->name = strdup(gad.id);
    group->picture = NULL;
    group->about = NULL;
    group->members = NULL;
    group->members_len = 0;
    group->is_private = false;
    group->is_closed = false;
    group->last_metadata_update = 0;
    group->last_admins_update = 0;
    group->last_members_update = 0;
    return group;
}

void nostr_free_group(nostr_group_t *group) {
    if (!group) return;
    free(group->address.relay);
    free(group->address.id);
    free(group->name);
    free(group->picture);
    free(group->about);
    for (size_t i = 0; i < group->members_len; ++i) {
        free(group->members[i].name);
        free(group->members[i].permissions);
    }
    free(group->members);
    free(group);
}

nostr_event_t *nostr_group_to_metadata_event(const nostr_group_t *group) {
    nostr_event_t *evt = nostr_event_new();
    evt->kind = KIND_SIMPLE_GROUP_METADATA;
    evt->created_at = group->last_metadata_update;
    nostr_event_add_tag(evt, "d", group->address.id);
    if (group->name) nostr_event_add_tag(evt, "name", group->name);
    if (group->about) nostr_event_add_tag(evt, "about", group->about);
    if (group->picture) nostr_event_add_tag(evt, "picture", group->picture);
    if (group->is_private) nostr_event_add_tag(evt, "private", "");
    if (!group->is_private) nostr_event_add_tag(evt, "public", "");
    if (group->is_closed) nostr_event_add_tag(evt, "closed", "");
    if (!group->is_closed) nostr_event_add_tag(evt, "open", "");
    return evt;
}

nostr_event_t *nostr_group_to_admins_event(const nostr_group_t *group) {
    nostr_event_t *evt = nostr_event_new();
    evt->kind = NOSTR_KIND_SIMPLE_GROUP_ADMINS;
    evt->created_at = group->last_admins_update;
    nostr_event_add_tag(evt, "d", group->address.id);
    for (size_t i = 0; i < group->members_len; ++i) {
        nostr_role_t *role = &group->members[i];
        if (role->permissions_len > 0) {
            char *tag = malloc(3 + role->permissions_len * 10);
            sprintf(tag, "p,%s,%s", group->members[i].name, group->members[i].name);
            for (size_t j = 0; j < role->permissions_len; ++j) {
                strcat(tag, ",");
                strcat(tag, nostr_permission_to_string(role->permissions[j]));
            }
            nostr_event_add_tag(evt, tag, "");
            free(tag);
        }
    }
    return evt;
}

nostr_event_t *nostr_group_to_members_event(const nostr_group_t *group) {
    nostr_event_t *evt = nostr_event_new();
    evt->kind = NOSTR_KIND_SIMPLE_GROUP_MEMBERS;
    evt->created_at = group->last_members_update;
    nostr_event_add_tag(evt, "d", group->address.id);
    for (size_t i = 0; i < group->members_len; ++i) {
        nostr_event_add_tag(evt, "p", group->members[i].name);
    }
    return evt;
}

bool nostr_group_merge_in_metadata_event(nostr_group_t *group, const nostr_event_t *event) {
    if (event->kind != NOSTR_KIND_SIMPLE_GROUP_METADATA) {
        return false;
    }
    if (event->created_at < group->last_metadata_update) {
        return false;
    }

    group->last_metadata_update = event->created_at;
    group->name = nostr_event_get_tag_value(event, "name");
    group->about = nostr_event_get_tag_value(event, "about");
    group->picture = nostr_event_get_tag_value(event, "picture");
    group->is_private = nostr_event_get_tag(event, "private") != NULL;
    group->is_closed = nostr_event_get_tag(event, "closed") != NULL;

    return true;
}

bool nostr_group_merge_in_admins_event(nostr_group_t *group, const nostr_event_t *event) {
    if (event->kind != NOSTR_KIND_SIMPLE_GROUP_ADMINS) {
        return false;
    }
    if (event->created_at < group->last_admins_update) {
        return false;
    }

    group->last_admins_update = event->created_at;
    for (size_t i = 0; i < event->tags_len; ++i) {
        if (strcmp(event->tags[i].key, "p") == 0) {
            char *name = event->tags[i].value;
            nostr_role_t *role = nostr_group_get_member(group, name);
            if (!role) {
                role = nostr_group_add_member(group, name);
            }
            nostr_group_add_permission(role, nostr_permission_from_string(event->tags[i].value));
        }
    }
    return true;
}

bool nostr_group_merge_in_members_event(nostr_group_t *group, const nostr_event_t *event) {
    if (event->kind != NOSTR_KIND_SIMPLE_GROUP_MEMBERS) {
        return false;
    }
    if (event->created_at < group->last_members_update) {
        return false;
    }

    group->last_members_update = event->created_at;
    for (size_t i = 0; i < event->tags_len; ++i) {
        if (strcmp(event->tags[i].key, "p") == 0) {
            char *name = event->tags[i].value;
            nostr_group_add_member(group, name);
        }
    }
    return true;
}

nostr_role_t *nostr_group_get_member(const nostr_group_t *group, const char *name) {
    for (size_t i = 0; i < group->members_len; ++i) {
        if (strcmp(group->members[i].name, name) == 0) {
            return &group->members[i];
        }
    }
    return NULL;
}

nostr_role_t *nostr_group_add_member(nostr_group_t *group, const char *name) {
    group->members = realloc(group->members, (group->members_len + 1) * sizeof(nostr_role_t));
    nostr_role_t *role = &group->members[group->members_len++];
    role->name = strdup(name);
    role->permissions = NULL;
    role->permissions_len = 0;
    return role;
}

void nostr_group_add_permission(nostr_role_t *role, nostr_permission_t perm) {
    for (size_t i = 0; i < role->permissions_len; ++i) {
        if (role->permissions[i] == perm) {
            return;
        }
    }
    role->permissions = realloc(role->permissions, (role->permissions_len + 1) * sizeof(nostr_permission_t));
    role->permissions[role->permissions_len++] = perm;
}

const char *nostr_permission_to_string(nostr_permission_t perm) {
    switch (perm) {
        case NOSTR_PERMISSION_ADD_USER:
            return "add-user";
        case NOSTR_PERMISSION_EDIT_METADATA:
            return "edit-metadata";
        case NOSTR_PERMISSION_DELETE_EVENT:
            return "delete-event";
        case NOSTR_PERMISSION_REMOVE_USER:
            return "remove-user";
        case NOSTR_PERMISSION_ADD_PERMISSION:
            return "add-permission";
        case NOSTR_PERMISSION_REMOVE_PERMISSION:
            return "remove-permission";
        case NOSTR_PERMISSION_EDIT_GROUP_STATUS:
            return "edit-group-status";
        default:
            return NULL;
    }
}

nostr_permission_t nostr_permission_from_string(const char *str) {
    if (strcmp(str, "add-user") == 0) {
        return NOSTR_PERMISSION_ADD_USER;
    } else if (strcmp(str, "edit-metadata") == 0) {
        return NOSTR_PERMISSION_EDIT_METADATA;
    } else if (strcmp(str, "delete-event") == 0) {
        return NOSTR_PERMISSION_DELETE_EVENT;
    } else if (strcmp(str, "remove-user") == 0) {
        return NOSTR_PERMISSION_REMOVE_USER;
    } else if (strcmp(str, "add-permission") == 0) {
        return NOSTR_PERMISSION_ADD_PERMISSION;
    } else if (strcmp(str, "remove-permission") == 0) {
        return NOSTR_PERMISSION_REMOVE_PERMISSION;
    } else if (strcmp(str, "edit-group-status") == 0) {
        return NOSTR_PERMISSION_EDIT_GROUP_STATUS;
    } else {
        return -1;
    }
}
