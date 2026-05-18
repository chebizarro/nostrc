#include "nip29.h"

#include "nostr-kinds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *nip29_strdup(const char *s) {
    return s ? strdup(s) : NULL;
}

static char *nip29_strndup(const char *s, size_t n) {
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static bool group_id_is_valid(const char *id) {
    if (!id || *id == '\0') return false;
    for (const unsigned char *p = (const unsigned char *)id; *p; ++p) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_') {
            continue;
        }
        return false;
    }
    return true;
}

bool nostr_group_address_parse(const char *raw, nostr_group_address_t *out) {
    if (!raw || !out) return false;
    memset(out, 0, sizeof(*out));

    const char *sep = strchr(raw, '\'');
    if (!sep || sep == raw || sep[1] == '\0' || strchr(sep + 1, '\'')) {
        return false;
    }

    char *relay = nip29_strndup(raw, (size_t)(sep - raw));
    char *id = nip29_strdup(sep + 1);
    if (!relay || !id || !group_id_is_valid(id)) {
        free(relay);
        free(id);
        return false;
    }

    out->relay = relay;
    out->id = id;
    return true;
}

bool nostr_group_address_is_valid(const nostr_group_address_t *address) {
    return address && address->relay && address->relay[0] && group_id_is_valid(address->id);
}

char *nostr_group_address_to_string(const nostr_group_address_t *address) {
    if (!nostr_group_address_is_valid(address)) return NULL;
    size_t len = strlen(address->relay) + 1 + strlen(address->id) + 1;
    char *out = malloc(len);
    if (!out) return NULL;
    snprintf(out, len, "%s'%s", address->relay, address->id);
    return out;
}

void nostr_group_address_clear(nostr_group_address_t *address) {
    if (!address) return;
    free(address->relay);
    free(address->id);
    address->relay = NULL;
    address->id = NULL;
}

static void clear_admin(nostr_group_admin_t *admin) {
    if (!admin) return;
    free(admin->pubkey);
    for (size_t i = 0; i < admin->roles_len; ++i) {
        free(admin->roles[i]);
    }
    free(admin->roles);
    memset(admin, 0, sizeof(*admin));
}

static void free_admin_array(nostr_group_admin_t *admins, size_t admins_len) {
    for (size_t i = 0; i < admins_len; ++i) {
        clear_admin(&admins[i]);
    }
    free(admins);
}

static void clear_admins(nostr_group_t *group) {
    if (!group) return;
    free_admin_array(group->admins, group->admins_len);
    group->admins = NULL;
    group->admins_len = 0;
}

static void free_member_array(nostr_group_member_t *members, size_t members_len) {
    for (size_t i = 0; i < members_len; ++i) {
        free(members[i].pubkey);
        free(members[i].label);
    }
    free(members);
}

static void clear_members(nostr_group_t *group) {
    if (!group) return;
    free_member_array(group->members, group->members_len);
    group->members = NULL;
    group->members_len = 0;
}

static void free_role_array(nostr_group_role_t *roles, size_t roles_len) {
    for (size_t i = 0; i < roles_len; ++i) {
        free(roles[i].name);
        free(roles[i].description);
    }
    free(roles);
}

static void clear_roles(nostr_group_t *group) {
    if (!group) return;
    free_role_array(group->roles, group->roles_len);
    group->roles = NULL;
    group->roles_len = 0;
}

nostr_group_t *nostr_new_group(const char *gadstr) {
    nostr_group_address_t gad;
    if (!nostr_group_address_parse(gadstr, &gad)) {
        return NULL;
    }

    nostr_group_t *group = calloc(1, sizeof(nostr_group_t));
    if (!group) {
        nostr_group_address_clear(&gad);
        return NULL;
    }

    group->address = gad;
    group->name = nip29_strdup(gad.id);
    if (!group->name) {
        nostr_free_group(group);
        return NULL;
    }
    return group;
}

void nostr_free_group(nostr_group_t *group) {
    if (!group) return;
    nostr_group_address_clear(&group->address);
    free(group->name);
    free(group->picture);
    free(group->about);
    clear_admins(group);
    clear_members(group);
    clear_roles(group);
    free(group);
}

static void event_add_tag(NostrEvent *evt, NostrTag *tag) {
    if (!evt || !tag) return;
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(evt);
    if (!tags) {
        tags = nostr_tags_new(0);
        nostr_event_set_tags(evt, tags);
    }
    nostr_tags_append(tags, tag);
}

static void event_add_flag(NostrEvent *evt, const char *key) {
    event_add_tag(evt, nostr_tag_new(key, NULL));
}

nostr_event_t *nostr_group_to_metadata_event(const nostr_group_t *group) {
    if (!group || !nostr_group_address_is_valid(&group->address)) return NULL;

    NostrEvent *evt = nostr_event_new();
    if (!evt) return NULL;
    nostr_event_set_kind(evt, NOSTR_KIND_SIMPLE_GROUP_METADATA);
    nostr_event_set_created_at(evt, group->last_metadata_update);
    nostr_event_set_content(evt, "");

    event_add_tag(evt, nostr_tag_new("d", group->address.id, NULL));
    if (group->name) event_add_tag(evt, nostr_tag_new("name", group->name, NULL));
    if (group->about) event_add_tag(evt, nostr_tag_new("about", group->about, NULL));
    if (group->picture) event_add_tag(evt, nostr_tag_new("picture", group->picture, NULL));
    if (group->is_private) event_add_flag(evt, "private");
    if (group->is_restricted) event_add_flag(evt, "restricted");
    if (group->is_hidden) event_add_flag(evt, "hidden");
    if (group->is_closed) event_add_flag(evt, "closed");
    return evt;
}

nostr_event_t *nostr_group_to_admins_event(const nostr_group_t *group) {
    if (!group || !nostr_group_address_is_valid(&group->address)) return NULL;

    NostrEvent *evt = nostr_event_new();
    if (!evt) return NULL;
    nostr_event_set_kind(evt, NOSTR_KIND_SIMPLE_GROUP_ADMINS);
    nostr_event_set_created_at(evt, group->last_admins_update);
    nostr_event_set_content(evt, "");

    event_add_tag(evt, nostr_tag_new("d", group->address.id, NULL));
    for (size_t i = 0; i < group->admins_len; ++i) {
        const nostr_group_admin_t *admin = &group->admins[i];
        if (!admin->pubkey) continue;
        NostrTag *tag = nostr_tag_new("p", admin->pubkey, NULL);
        if (!tag) continue;
        for (size_t j = 0; j < admin->roles_len; ++j) {
            if (admin->roles[j] && admin->roles[j][0]) {
                nostr_tag_append(tag, admin->roles[j]);
            }
        }
        event_add_tag(evt, tag);
    }
    return evt;
}

nostr_event_t *nostr_group_to_members_event(const nostr_group_t *group) {
    if (!group || !nostr_group_address_is_valid(&group->address)) return NULL;

    NostrEvent *evt = nostr_event_new();
    if (!evt) return NULL;
    nostr_event_set_kind(evt, NOSTR_KIND_SIMPLE_GROUP_MEMBERS);
    nostr_event_set_created_at(evt, group->last_members_update);
    nostr_event_set_content(evt, "");

    event_add_tag(evt, nostr_tag_new("d", group->address.id, NULL));
    for (size_t i = 0; i < group->members_len; ++i) {
        const nostr_group_member_t *member = &group->members[i];
        if (!member->pubkey) continue;
        if (member->label) {
            event_add_tag(evt, nostr_tag_new("p", member->pubkey, member->label, NULL));
        } else {
            event_add_tag(evt, nostr_tag_new("p", member->pubkey, NULL));
        }
    }
    return evt;
}

nostr_event_t *nostr_group_to_roles_event(const nostr_group_t *group) {
    if (!group || !nostr_group_address_is_valid(&group->address)) return NULL;

    NostrEvent *evt = nostr_event_new();
    if (!evt) return NULL;
    nostr_event_set_kind(evt, NOSTR_KIND_SIMPLE_GROUP_ROLES);
    nostr_event_set_created_at(evt, group->last_roles_update);
    nostr_event_set_content(evt, "");

    event_add_tag(evt, nostr_tag_new("d", group->address.id, NULL));
    for (size_t i = 0; i < group->roles_len; ++i) {
        const nostr_group_role_t *role = &group->roles[i];
        if (!role->name) continue;
        if (role->description) {
            event_add_tag(evt, nostr_tag_new("role", role->name, role->description, NULL));
        } else {
            event_add_tag(evt, nostr_tag_new("role", role->name, NULL));
        }
    }
    return evt;
}

static bool copy_optional_string(const char *src, char **dst) {
    if (!dst) return false;
    *dst = NULL;
    if (!src) return true;
    *dst = nip29_strdup(src);
    return *dst != NULL;
}

static const char *tag_value(const NostrEvent *event, const char *key) {
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(event);
    if (!tags) return NULL;
    for (size_t i = 0; i < nostr_tags_size(tags); ++i) {
        const NostrTag *tag = nostr_tags_get(tags, i);
        const char *tag_key = nostr_tag_get_key(tag);
        if (tag_key && strcmp(tag_key, key) == 0) {
            return nostr_tag_get_value(tag);
        }
    }
    return NULL;
}

static bool has_tag(const NostrEvent *event, const char *key) {
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(event);
    if (!tags) return false;
    for (size_t i = 0; i < nostr_tags_size(tags); ++i) {
        const NostrTag *tag = nostr_tags_get(tags, i);
        const char *tag_key = nostr_tag_get_key(tag);
        if (tag_key && strcmp(tag_key, key) == 0) {
            return true;
        }
    }
    return false;
}

static bool event_matches_group(const nostr_group_t *group, const NostrEvent *event) {
    if (!group || !event || !nostr_group_address_is_valid(&group->address)) return false;
    const char *d = tag_value(event, "d");
    return d && strcmp(d, group->address.id) == 0;
}

bool nostr_group_merge_in_metadata_event(nostr_group_t *group, const nostr_event_t *event) {
    if (!group || !event || nostr_event_get_kind(event) != NOSTR_KIND_SIMPLE_GROUP_METADATA) return false;
    if (!event_matches_group(group, event)) return false;
    if (nostr_event_get_created_at(event) < group->last_metadata_update) return false;

    char *name = NULL;
    char *about = NULL;
    char *picture = NULL;
    if (!copy_optional_string(tag_value(event, "name"), &name) ||
        !copy_optional_string(tag_value(event, "about"), &about) ||
        !copy_optional_string(tag_value(event, "picture"), &picture)) {
        free(name);
        free(about);
        free(picture);
        return false;
    }

    free(group->name);
    free(group->about);
    free(group->picture);
    group->name = name;
    group->about = about;
    group->picture = picture;
    group->is_private = has_tag(event, "private");
    group->is_restricted = has_tag(event, "restricted");
    group->is_hidden = has_tag(event, "hidden");
    group->is_closed = has_tag(event, "closed");
    group->last_metadata_update = nostr_event_get_created_at(event);

    return true;
}

static nostr_group_admin_t *find_admin(nostr_group_admin_t *admins, size_t len, const char *pubkey) {
    if (!pubkey) return NULL;
    for (size_t i = 0; i < len; ++i) {
        if (admins[i].pubkey && strcmp(admins[i].pubkey, pubkey) == 0) {
            return &admins[i];
        }
    }
    return NULL;
}

static nostr_group_admin_t *append_admin(nostr_group_admin_t **admins, size_t *len, const char *pubkey) {
    nostr_group_admin_t *existing = find_admin(*admins, *len, pubkey);
    if (existing) return existing;

    nostr_group_admin_t *new_admins = realloc(*admins, (*len + 1) * sizeof(*new_admins));
    if (!new_admins) return NULL;
    *admins = new_admins;

    nostr_group_admin_t *admin = &(*admins)[(*len)++];
    memset(admin, 0, sizeof(*admin));
    admin->pubkey = nip29_strdup(pubkey);
    if (!admin->pubkey) {
        --(*len);
        return NULL;
    }
    return admin;
}

bool nostr_group_admin_add_role(nostr_group_admin_t *admin, const char *role) {
    if (!admin || !role || role[0] == '\0') return false;
    for (size_t i = 0; i < admin->roles_len; ++i) {
        if (admin->roles[i] && strcmp(admin->roles[i], role) == 0) {
            return true;
        }
    }

    char **new_roles = realloc(admin->roles, (admin->roles_len + 1) * sizeof(*new_roles));
    if (!new_roles) return false;
    admin->roles = new_roles;
    admin->roles[admin->roles_len] = nip29_strdup(role);
    if (!admin->roles[admin->roles_len]) return false;
    admin->roles_len++;
    return true;
}

bool nostr_group_merge_in_admins_event(nostr_group_t *group, const nostr_event_t *event) {
    if (!group || !event || nostr_event_get_kind(event) != NOSTR_KIND_SIMPLE_GROUP_ADMINS) return false;
    if (!event_matches_group(group, event)) return false;
    if (nostr_event_get_created_at(event) < group->last_admins_update) return false;

    nostr_group_admin_t *admins = NULL;
    size_t admins_len = 0;
    bool ok = true;
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(event);
    for (size_t i = 0; tags && ok && i < nostr_tags_size(tags); ++i) {
        const NostrTag *tag = nostr_tags_get(tags, i);
        const char *key = nostr_tag_get_key(tag);
        if (!key || strcmp(key, "p") != 0 || nostr_tag_size(tag) < 2) continue;

        const char *pubkey = nostr_tag_get(tag, 1);
        if (!pubkey || pubkey[0] == '\0') continue;

        nostr_group_admin_t *admin = append_admin(&admins, &admins_len, pubkey);
        if (!admin) {
            ok = false;
            break;
        }
        for (size_t j = 2; j < nostr_tag_size(tag); ++j) {
            const char *role = nostr_tag_get(tag, j);
            if (!role || role[0] == '\0') continue;
            if (!nostr_group_admin_add_role(admin, role)) {
                ok = false;
                break;
            }
        }
    }
    if (!ok) {
        free_admin_array(admins, admins_len);
        return false;
    }

    clear_admins(group);
    group->admins = admins;
    group->admins_len = admins_len;
    group->admins_loaded = true;
    group->last_admins_update = nostr_event_get_created_at(event);
    return true;
}

static nostr_group_member_t *append_member(nostr_group_member_t **members, size_t *len, const char *pubkey, const char *label) {
    if (!pubkey || pubkey[0] == '\0') return NULL;
    for (size_t i = 0; i < *len; ++i) {
        if ((*members)[i].pubkey && strcmp((*members)[i].pubkey, pubkey) == 0) {
            char *new_label = NULL;
            if (!copy_optional_string(label, &new_label)) return NULL;
            free((*members)[i].label);
            (*members)[i].label = new_label;
            return &(*members)[i];
        }
    }

    nostr_group_member_t *new_members = realloc(*members, (*len + 1) * sizeof(*new_members));
    if (!new_members) return NULL;
    *members = new_members;

    nostr_group_member_t *member = &(*members)[(*len)++];
    memset(member, 0, sizeof(*member));
    member->pubkey = nip29_strdup(pubkey);
    if (!copy_optional_string(label, &member->label) || !member->pubkey) {
        free(member->pubkey);
        free(member->label);
        memset(member, 0, sizeof(*member));
        --(*len);
        return NULL;
    }
    return member;
}

bool nostr_group_merge_in_members_event(nostr_group_t *group, const nostr_event_t *event) {
    if (!group || !event || nostr_event_get_kind(event) != NOSTR_KIND_SIMPLE_GROUP_MEMBERS) return false;
    if (!event_matches_group(group, event)) return false;
    if (nostr_event_get_created_at(event) < group->last_members_update) return false;

    nostr_group_member_t *members = NULL;
    size_t members_len = 0;
    bool ok = true;
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(event);
    for (size_t i = 0; tags && ok && i < nostr_tags_size(tags); ++i) {
        const NostrTag *tag = nostr_tags_get(tags, i);
        const char *key = nostr_tag_get_key(tag);
        if (!key || strcmp(key, "p") != 0 || nostr_tag_size(tag) < 2) continue;
        const char *pubkey = nostr_tag_get(tag, 1);
        if (!pubkey || pubkey[0] == '\0') continue;
        if (!append_member(&members, &members_len, pubkey, nostr_tag_size(tag) > 2 ? nostr_tag_get(tag, 2) : NULL)) {
            ok = false;
        }
    }
    if (!ok) {
        free_member_array(members, members_len);
        return false;
    }

    clear_members(group);
    group->members = members;
    group->members_len = members_len;
    group->members_loaded = true;
    group->members_may_be_partial = true;
    group->last_members_update = nostr_event_get_created_at(event);
    return true;
}

static nostr_group_role_t *append_role(nostr_group_role_t **roles, size_t *len, const char *name, const char *description) {
    if (!name || name[0] == '\0') return NULL;
    for (size_t i = 0; i < *len; ++i) {
        if ((*roles)[i].name && strcmp((*roles)[i].name, name) == 0) {
            char *new_description = NULL;
            if (!copy_optional_string(description, &new_description)) return NULL;
            free((*roles)[i].description);
            (*roles)[i].description = new_description;
            return &(*roles)[i];
        }
    }

    nostr_group_role_t *new_roles = realloc(*roles, (*len + 1) * sizeof(*new_roles));
    if (!new_roles) return NULL;
    *roles = new_roles;

    nostr_group_role_t *role = &(*roles)[(*len)++];
    memset(role, 0, sizeof(*role));
    role->name = nip29_strdup(name);
    if (!copy_optional_string(description, &role->description) || !role->name) {
        free(role->name);
        free(role->description);
        memset(role, 0, sizeof(*role));
        --(*len);
        return NULL;
    }
    return role;
}

bool nostr_group_merge_in_roles_event(nostr_group_t *group, const nostr_event_t *event) {
    if (!group || !event || nostr_event_get_kind(event) != NOSTR_KIND_SIMPLE_GROUP_ROLES) return false;
    if (!event_matches_group(group, event)) return false;
    if (nostr_event_get_created_at(event) < group->last_roles_update) return false;

    nostr_group_role_t *roles = NULL;
    size_t roles_len = 0;
    bool ok = true;
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(event);
    for (size_t i = 0; tags && ok && i < nostr_tags_size(tags); ++i) {
        const NostrTag *tag = nostr_tags_get(tags, i);
        const char *key = nostr_tag_get_key(tag);
        if (!key || strcmp(key, "role") != 0 || nostr_tag_size(tag) < 2) continue;
        const char *name = nostr_tag_get(tag, 1);
        if (!name || name[0] == '\0') continue;
        if (!append_role(&roles, &roles_len, name, nostr_tag_size(tag) > 2 ? nostr_tag_get(tag, 2) : NULL)) {
            ok = false;
        }
    }
    if (!ok) {
        free_role_array(roles, roles_len);
        return false;
    }

    clear_roles(group);
    group->roles = roles;
    group->roles_len = roles_len;
    group->roles_loaded = true;
    group->last_roles_update = nostr_event_get_created_at(event);
    return true;
}

nostr_group_admin_t *nostr_group_get_admin(const nostr_group_t *group, const char *pubkey) {
    if (!group || !pubkey) return NULL;
    return find_admin(group->admins, group->admins_len, pubkey);
}

nostr_group_member_t *nostr_group_get_member(const nostr_group_t *group, const char *pubkey) {
    if (!group || !pubkey) return NULL;
    for (size_t i = 0; i < group->members_len; ++i) {
        if (group->members[i].pubkey && strcmp(group->members[i].pubkey, pubkey) == 0) {
            return &group->members[i];
        }
    }
    return NULL;
}

nostr_group_role_t *nostr_group_get_role(const nostr_group_t *group, const char *name) {
    if (!group || !name) return NULL;
    for (size_t i = 0; i < group->roles_len; ++i) {
        if (group->roles[i].name && strcmp(group->roles[i].name, name) == 0) {
            return &group->roles[i];
        }
    }
    return NULL;
}

nostr_group_admin_t *nostr_group_add_admin(nostr_group_t *group, const char *pubkey) {
    if (!group) return NULL;
    nostr_group_admin_t *admin = append_admin(&group->admins, &group->admins_len, pubkey);
    if (admin) group->admins_loaded = true;
    return admin;
}

nostr_group_member_t *nostr_group_add_member(nostr_group_t *group, const char *pubkey, const char *label) {
    if (!group) return NULL;
    nostr_group_member_t *member = append_member(&group->members, &group->members_len, pubkey, label);
    if (member) {
        group->members_loaded = true;
        group->members_may_be_partial = false;
    }
    return member;
}

nostr_group_role_t *nostr_group_add_role(nostr_group_t *group, const char *name, const char *description) {
    if (!group) return NULL;
    nostr_group_role_t *role = append_role(&group->roles, &group->roles_len, name, description);
    if (role) group->roles_loaded = true;
    return role;
}

const char *nostr_permission_to_string(nostr_permission_t perm) {
    switch (perm) {
        case NOSTR_PERMISSION_PUT_USER:
            return "put-user";
        case NOSTR_PERMISSION_REMOVE_USER:
            return "remove-user";
        case NOSTR_PERMISSION_EDIT_METADATA:
            return "edit-metadata";
        case NOSTR_PERMISSION_DELETE_EVENT:
            return "delete-event";
        case NOSTR_PERMISSION_CREATE_GROUP:
            return "create-group";
        case NOSTR_PERMISSION_DELETE_GROUP:
            return "delete-group";
        case NOSTR_PERMISSION_CREATE_INVITE:
            return "create-invite";
        default:
            return NULL;
    }
}

nostr_permission_t nostr_permission_from_string(const char *str) {
    if (!str) return NOSTR_PERMISSION_UNKNOWN;
    if (strcmp(str, "put-user") == 0 || strcmp(str, "add-user") == 0) {
        return NOSTR_PERMISSION_PUT_USER;
    } else if (strcmp(str, "remove-user") == 0) {
        return NOSTR_PERMISSION_REMOVE_USER;
    } else if (strcmp(str, "edit-metadata") == 0) {
        return NOSTR_PERMISSION_EDIT_METADATA;
    } else if (strcmp(str, "delete-event") == 0) {
        return NOSTR_PERMISSION_DELETE_EVENT;
    } else if (strcmp(str, "create-group") == 0) {
        return NOSTR_PERMISSION_CREATE_GROUP;
    } else if (strcmp(str, "delete-group") == 0) {
        return NOSTR_PERMISSION_DELETE_GROUP;
    } else if (strcmp(str, "create-invite") == 0) {
        return NOSTR_PERMISSION_CREATE_INVITE;
    }
    return NOSTR_PERMISSION_UNKNOWN;
}
