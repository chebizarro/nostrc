#include "nip_communikeys.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *ck_strdup(const char *s) {
    return s ? strdup(s) : NULL;
}

static char *ck_strndup(const char *s, size_t n) {
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static const char *empty_subtype(const char *subtype) {
    return subtype ? subtype : "";
}

bool nostr_communikeys_is_lower_hex_32(const char *value) {
    if (!value || strlen(value) != 64) return false;
    for (size_t i = 0; i < 64; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) return false;
    }
    return true;
}

const char *nostr_communikeys_status_string(nostr_communikeys_status_t status) {
    switch (status) {
        case NOSTR_COMMUNIKEYS_OK: return "ok";
        case NOSTR_COMMUNIKEYS_ERR_NULL: return "null argument";
        case NOSTR_COMMUNIKEYS_ERR_OOM: return "out of memory";
        case NOSTR_COMMUNIKEYS_ERR_WRONG_KIND: return "wrong event kind";
        case NOSTR_COMMUNIKEYS_ERR_BAD_PUBKEY: return "invalid lowercase hex pubkey";
        case NOSTR_COMMUNIKEYS_ERR_BAD_CONTENT: return "content must be empty";
        case NOSTR_COMMUNIKEYS_ERR_BAD_TAG: return "malformed tag";
        case NOSTR_COMMUNIKEYS_ERR_CARDINALITY: return "invalid tag cardinality";
        case NOSTR_COMMUNIKEYS_ERR_MISSING_RELAY: return "definition has no relay";
        case NOSTR_COMMUNIKEYS_ERR_SECTION_ORDER: return "invalid section tag order";
        case NOSTR_COMMUNIKEYS_ERR_SECTION_NAME: return "invalid or duplicate section name";
        case NOSTR_COMMUNIKEYS_ERR_SECTION_KIND: return "section has no valid kind";
        case NOSTR_COMMUNIKEYS_ERR_SECTION_ACL: return "section ACL must be one kind-30000 coordinate";
        case NOSTR_COMMUNIKEYS_ERR_DUPLICATE_ASSIGNMENT: return "duplicate exact kind/subtype assignment";
        case NOSTR_COMMUNIKEYS_ERR_BAD_COORDINATE: return "invalid event coordinate";
        case NOSTR_COMMUNIKEYS_ERR_BAD_REFERENCE: return "invalid publication reference";
        case NOSTR_COMMUNIKEYS_ERR_BAD_TARGETS: return "invalid community targets";
        case NOSTR_COMMUNIKEYS_ERR_AUTHOR_MISMATCH: return "publication author mismatch";
        case NOSTR_COMMUNIKEYS_ERR_REFERENCE_MISMATCH: return "publication reference mismatch";
        case NOSTR_COMMUNIKEYS_ERR_BAD_URI: return "invalid ncommunity identifier";
    }
    return "unknown";
}

static bool parse_decimal_kind(const char *value, int *out) {
    if (!value || !*value || !out) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (!isdigit(*p)) return false;
    errno = 0;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (errno || !end || *end || parsed < 0 || parsed > 65535) return false;
    *out = (int)parsed;
    return true;
}

bool nostr_communikeys_coordinate_parse(const char *value,
                                        const char *relay,
                                        nostr_communikeys_coordinate_t *out) {
    if (!value || !out) return false;
    memset(out, 0, sizeof(*out));
    const char *first = strchr(value, ':');
    const char *second = first ? strchr(first + 1, ':') : NULL;
    if (!first || !second || first == value || second == first + 1 || !second[1]) return false;

    char *kind_text = ck_strndup(value, (size_t)(first - value));
    char *pubkey = ck_strndup(first + 1, (size_t)(second - first - 1));
    char *identifier = ck_strdup(second + 1);
    char *relay_copy = relay && *relay ? ck_strdup(relay) : NULL;
    int kind = 0;
    if (!kind_text || !pubkey || !identifier ||
        (relay && *relay && !relay_copy) ||
        !parse_decimal_kind(kind_text, &kind) ||
        !nostr_communikeys_is_lower_hex_32(pubkey)) {
        free(kind_text);
        free(pubkey);
        free(identifier);
        free(relay_copy);
        return false;
    }
    free(kind_text);
    out->kind = kind;
    out->pubkey = pubkey;
    out->identifier = identifier;
    out->relay = relay_copy;
    return true;
}

char *nostr_communikeys_coordinate_format(const nostr_communikeys_coordinate_t *coordinate) {
    if (!coordinate || coordinate->kind < 0 ||
        !nostr_communikeys_is_lower_hex_32(coordinate->pubkey) ||
        !coordinate->identifier || !coordinate->identifier[0]) return NULL;
    int n = snprintf(NULL, 0, "%d:%s:%s", coordinate->kind,
                     coordinate->pubkey, coordinate->identifier);
    if (n < 0) return NULL;
    char *out = malloc((size_t)n + 1);
    if (!out) return NULL;
    snprintf(out, (size_t)n + 1, "%d:%s:%s", coordinate->kind,
             coordinate->pubkey, coordinate->identifier);
    return out;
}

void nostr_communikeys_coordinate_clear(nostr_communikeys_coordinate_t *coordinate) {
    if (!coordinate) return;
    free(coordinate->pubkey);
    free(coordinate->identifier);
    free(coordinate->relay);
    memset(coordinate, 0, sizeof(*coordinate));
}

static bool append_string(char ***values, size_t *len, const char *value) {
    char *copy = ck_strdup(value);
    if (!copy) return false;
    char **next = realloc(*values, (*len + 1) * sizeof(*next));
    if (!next) {
        free(copy);
        return false;
    }
    *values = next;
    (*values)[(*len)++] = copy;
    return true;
}

static void clear_strings(char **values, size_t len) {
    for (size_t i = 0; i < len; ++i) free(values[i]);
    free(values);
}

static bool strings_contains(char **values, size_t len, const char *value) {
    for (size_t i = 0; i < len; ++i)
        if (values[i] && strcmp(values[i], value) == 0) return true;
    return false;
}

static void section_clear(nostr_communikeys_section_t *section) {
    if (!section) return;
    free(section->name);
    for (size_t i = 0; i < section->assignments_len; ++i)
        free(section->assignments[i].subtype);
    free(section->assignments);
    nostr_communikeys_coordinate_clear(&section->profile_list);
    clear_strings(section->badges, section->badges_len);
    memset(section, 0, sizeof(*section));
}

void nostr_communikeys_definition_clear(nostr_communikeys_definition_t *definition) {
    if (!definition) return;
    free(definition->pubkey);
    clear_strings(definition->relays, definition->relays_len);
    clear_strings(definition->blossom_servers, definition->blossom_servers_len);
    clear_strings(definition->grasp_servers, definition->grasp_servers_len);
    for (size_t i = 0; i < definition->mints_len; ++i) {
        free(definition->mints[i].url);
        free(definition->mints[i].protocol);
    }
    free(definition->mints);
    free(definition->tos);
    free(definition->tos_relay);
    free(definition->location);
    free(definition->geohash);
    free(definition->description);
    for (size_t i = 0; i < definition->sections_len; ++i)
        section_clear(&definition->sections[i]);
    free(definition->sections);
    memset(definition, 0, sizeof(*definition));
}

static void definition_error(nostr_communikeys_definition_t *definition,
                             nostr_communikeys_status_t status) {
    if (definition->validation_status == NOSTR_COMMUNIKEYS_OK)
        definition->validation_status = status;
    definition->valid = false;
}

static nostr_communikeys_section_t *
append_section(nostr_communikeys_definition_t *definition, const char *name) {
    nostr_communikeys_section_t *next =
        realloc(definition->sections,
                (definition->sections_len + 1) * sizeof(*next));
    if (!next) return NULL;
    definition->sections = next;
    nostr_communikeys_section_t *section =
        &definition->sections[definition->sections_len++];
    memset(section, 0, sizeof(*section));
    section->name = ck_strdup(name);
    if (!section->name) {
        definition->sections_len--;
        return NULL;
    }
    return section;
}

static bool append_assignment(nostr_communikeys_section_t *section,
                              int kind, const char *subtype) {
    nostr_communikeys_assignment_t *next =
        realloc(section->assignments,
                (section->assignments_len + 1) * sizeof(*next));
    if (!next) return false;
    section->assignments = next;
    nostr_communikeys_assignment_t *assignment =
        &section->assignments[section->assignments_len++];
    assignment->kind = kind;
    assignment->subtype = subtype && *subtype ? ck_strdup(subtype) : NULL;
    if (subtype && *subtype && !assignment->subtype) {
        section->assignments_len--;
        return false;
    }
    return true;
}

static bool assignment_exists(const nostr_communikeys_definition_t *definition,
                              int kind, const char *subtype) {
    const char *wanted = empty_subtype(subtype);
    for (size_t i = 0; i < definition->sections_len; ++i)
        for (size_t j = 0; j < definition->sections[i].assignments_len; ++j) {
            const nostr_communikeys_assignment_t *a =
                &definition->sections[i].assignments[j];
            if (a->kind == kind &&
                strcmp(empty_subtype(a->subtype), wanted) == 0) return true;
        }
    return false;
}

static bool set_single(char **slot, const char *value) {
    if (*slot) return false;
    *slot = ck_strdup(value);
    return *slot != NULL;
}

static bool append_mint(nostr_communikeys_definition_t *definition,
                        const char *url, const char *protocol) {
    nostr_communikeys_mint_t *next =
        realloc(definition->mints,
                (definition->mints_len + 1) * sizeof(*next));
    if (!next) return false;
    definition->mints = next;
    nostr_communikeys_mint_t *mint =
        &definition->mints[definition->mints_len++];
    memset(mint, 0, sizeof(*mint));
    mint->url = ck_strdup(url);
    mint->protocol = protocol && *protocol ? ck_strdup(protocol) : NULL;
    if (!mint->url || (protocol && *protocol && !mint->protocol)) {
        free(mint->url);
        free(mint->protocol);
        definition->mints_len--;
        return false;
    }
    return true;
}

bool nostr_communikeys_definition_parse(const NostrEvent *event,
                                        nostr_communikeys_definition_t *out) {
    if (!event || !out || nostr_event_get_kind(event) != CAS_COMMUNITY_DEFINITION)
        return false;
    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->validation_status = NOSTR_COMMUNIKEYS_OK;

    const char *pubkey = nostr_event_get_pubkey(event);
    if (pubkey) {
        out->pubkey = ck_strdup(pubkey);
        if (!out->pubkey) {
            out->validation_status = NOSTR_COMMUNIKEYS_ERR_OOM;
            return false;
        }
    }
    if (!nostr_communikeys_is_lower_hex_32(pubkey))
        definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_PUBKEY);

    int section_state = 0; /* 0 before, 1 contiguous sequence, 2 after */
    nostr_communikeys_section_t *current = NULL;
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(event);
    for (size_t i = 0; tags && i < nostr_tags_size(tags); ++i) {
        const NostrTag *tag = nostr_tags_get(tags, i);
        const char *key = nostr_tag_get_key(tag);
        size_t size = nostr_tag_size(tag);
        const char *value = size > 1 ? nostr_tag_get(tag, 1) : NULL;
        if (!key) {
            definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_TAG);
            continue;
        }

        if (strcmp(key, "content") == 0) {
            if (section_state == 2)
                definition_error(out, NOSTR_COMMUNIKEYS_ERR_SECTION_ORDER);
            section_state = 1;
            if (size != 2 || !value || !*value) {
                definition_error(out, NOSTR_COMMUNIKEYS_ERR_SECTION_NAME);
                current = NULL;
                continue;
            }
            for (size_t j = 0; j < out->sections_len; ++j)
                if (strcmp(out->sections[j].name, value) == 0)
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_SECTION_NAME);
            current = append_section(out, value);
            if (!current) goto oom;
            continue;
        }

        bool scoped = strcmp(key, "k") == 0 || strcmp(key, "a") == 0 ||
                      strcmp(key, "badge") == 0;
        if (scoped) {
            if (section_state != 1 || !current) {
                definition_error(out, NOSTR_COMMUNIKEYS_ERR_SECTION_ORDER);
                continue;
            }
            if (strcmp(key, "k") == 0) {
                int kind = 0;
                if ((size != 2 && size != 3) || !parse_decimal_kind(value, &kind)) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_SECTION_KIND);
                    continue;
                }
                const char *subtype = size == 3 ? nostr_tag_get(tag, 2) : NULL;
                if (subtype && !*subtype) subtype = NULL;
                if (assignment_exists(out, kind, subtype)) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_DUPLICATE_ASSIGNMENT);
                    continue;
                }
                if (!append_assignment(current, kind, subtype)) goto oom;
            } else if (strcmp(key, "a") == 0) {
                if ((size != 2 && size != 3) || current->profile_list.pubkey) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_SECTION_ACL);
                    continue;
                }
                const char *relay = size == 3 ? nostr_tag_get(tag, 2) : NULL;
                if (!nostr_communikeys_coordinate_parse(
                        value, relay, &current->profile_list) ||
                    current->profile_list.kind != NOSTR_COMMUNIKEYS_KIND_PROFILE_LIST) {
                    nostr_communikeys_coordinate_clear(&current->profile_list);
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_SECTION_ACL);
                }
            } else {
                nostr_communikeys_coordinate_t badge = {0};
                if (size != 2 ||
                    !nostr_communikeys_coordinate_parse(value, NULL, &badge) ||
                    badge.kind != NOSTR_COMMUNIKEYS_KIND_BADGE_DEFINITION) {
                    nostr_communikeys_coordinate_clear(&badge);
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_COORDINATE);
                    continue;
                }
                nostr_communikeys_coordinate_clear(&badge);
                if (!append_string(&current->badges, &current->badges_len, value))
                    goto oom;
            }
            continue;
        }

        if (section_state == 1) {
            section_state = 2;
            current = NULL;
        }

        if (strcmp(key, "r") == 0) {
            if (size != 2 || !value || !*value) {
                definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_TAG);
            } else if (!append_string(&out->relays, &out->relays_len, value)) goto oom;
        } else if (strcmp(key, "blossom") == 0) {
            if (size != 2 || !value || !*value)
                definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_TAG);
            else if (!append_string(&out->blossom_servers,
                                    &out->blossom_servers_len, value)) goto oom;
        } else if (strcmp(key, "grasp") == 0) {
            if (size != 2 || !value || !*value)
                definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_TAG);
            else if (!append_string(&out->grasp_servers,
                                    &out->grasp_servers_len, value)) goto oom;
        } else if (strcmp(key, "mint") == 0) {
            if ((size != 2 && size != 3) || !value || !*value)
                definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_TAG);
            else if (!append_mint(out, value,
                                  size == 3 ? nostr_tag_get(tag, 2) : NULL)) goto oom;
        } else if (strcmp(key, "tos") == 0) {
            if ((size != 2 && size != 3) || !value || !*value || out->tos) {
                definition_error(out, NOSTR_COMMUNIKEYS_ERR_CARDINALITY);
            } else {
                out->tos = ck_strdup(value);
                const char *relay = size == 3 ? nostr_tag_get(tag, 2) : NULL;
                out->tos_relay = relay && *relay ? ck_strdup(relay) : NULL;
                if (!out->tos || (relay && *relay && !out->tos_relay)) goto oom;
            }
        } else if (strcmp(key, "location") == 0 ||
                   strcmp(key, "g") == 0 ||
                   strcmp(key, "description") == 0) {
            char **slot = strcmp(key, "location") == 0 ? &out->location :
                          strcmp(key, "g") == 0 ? &out->geohash :
                          &out->description;
            if (size != 2 || !value || !*value || *slot) {
                definition_error(out, NOSTR_COMMUNIKEYS_ERR_CARDINALITY);
            } else if (!set_single(slot, value)) goto oom;
        }
    }

    if (out->relays_len == 0)
        definition_error(out, NOSTR_COMMUNIKEYS_ERR_MISSING_RELAY);
    if (out->sections_len == 0)
        definition_error(out, NOSTR_COMMUNIKEYS_ERR_SECTION_NAME);
    for (size_t i = 0; i < out->sections_len; ++i) {
        if (out->sections[i].assignments_len == 0)
            definition_error(out, NOSTR_COMMUNIKEYS_ERR_SECTION_KIND);
        if (!out->sections[i].profile_list.pubkey)
            definition_error(out, NOSTR_COMMUNIKEYS_ERR_SECTION_ACL);
    }
    return true;

oom:
    nostr_communikeys_definition_clear(out);
    out->validation_status = NOSTR_COMMUNIKEYS_ERR_OOM;
    return false;
}

nostr_communikeys_status_t
nostr_communikeys_definition_validate_event(const NostrEvent *event) {
    if (!event) return NOSTR_COMMUNIKEYS_ERR_NULL;
    if (nostr_event_get_kind(event) != CAS_COMMUNITY_DEFINITION)
        return NOSTR_COMMUNIKEYS_ERR_WRONG_KIND;
    nostr_communikeys_definition_t definition;
    if (!nostr_communikeys_definition_parse(event, &definition))
        return NOSTR_COMMUNIKEYS_ERR_OOM;
    nostr_communikeys_status_t status = definition.validation_status;
    nostr_communikeys_definition_clear(&definition);
    return status;
}

static void event_add_tag(NostrEvent *event, NostrTag *tag) {
    if (!event || !tag) return;
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(event);
    if (!tags) {
        tags = nostr_tags_new(0);
        nostr_event_set_tags(event, tags);
    }
    nostr_tags_append(tags, tag);
}

NostrEvent *
nostr_communikeys_definition_to_event(const nostr_communikeys_definition_t *definition,
                                      int64_t created_at) {
    if (!definition || !nostr_communikeys_is_lower_hex_32(definition->pubkey))
        return NULL;
    NostrEvent *event = nostr_event_new();
    if (!event) return NULL;
    nostr_event_set_kind(event, CAS_COMMUNITY_DEFINITION);
    nostr_event_set_pubkey(event, definition->pubkey);
    nostr_event_set_created_at(event, created_at);
    nostr_event_set_content(event, "");

#define ADD_LIST(KEY, VALUES, LEN) \
    do { for (size_t _i = 0; _i < (LEN); ++_i) \
        event_add_tag(event, nostr_tag_new((KEY), (VALUES)[_i], NULL)); } while (0)
    ADD_LIST("r", definition->relays, definition->relays_len);
    ADD_LIST("blossom", definition->blossom_servers, definition->blossom_servers_len);
    ADD_LIST("grasp", definition->grasp_servers, definition->grasp_servers_len);
    for (size_t i = 0; i < definition->mints_len; ++i) {
        const nostr_communikeys_mint_t *mint = &definition->mints[i];
        event_add_tag(event, mint->protocol
            ? nostr_tag_new("mint", mint->url, mint->protocol, NULL)
            : nostr_tag_new("mint", mint->url, NULL));
    }
    for (size_t i = 0; i < definition->sections_len; ++i) {
        const nostr_communikeys_section_t *section = &definition->sections[i];
        event_add_tag(event, nostr_tag_new("content", section->name, NULL));
        for (size_t j = 0; j < section->assignments_len; ++j) {
            const nostr_communikeys_assignment_t *a = &section->assignments[j];
            char kind[16];
            snprintf(kind, sizeof(kind), "%d", a->kind);
            event_add_tag(event, a->subtype
                ? nostr_tag_new("k", kind, a->subtype, NULL)
                : nostr_tag_new("k", kind, NULL));
        }
        char *coordinate =
            nostr_communikeys_coordinate_format(&section->profile_list);
        if (!coordinate) {
            nostr_event_free(event);
            return NULL;
        }
        event_add_tag(event, section->profile_list.relay
            ? nostr_tag_new("a", coordinate, section->profile_list.relay, NULL)
            : nostr_tag_new("a", coordinate, NULL));
        free(coordinate);
        ADD_LIST("badge", section->badges, section->badges_len);
    }
    if (definition->tos)
        event_add_tag(event, definition->tos_relay
            ? nostr_tag_new("tos", definition->tos, definition->tos_relay, NULL)
            : nostr_tag_new("tos", definition->tos, NULL));
    if (definition->location)
        event_add_tag(event, nostr_tag_new("location", definition->location, NULL));
    if (definition->geohash)
        event_add_tag(event, nostr_tag_new("g", definition->geohash, NULL));
    if (definition->description)
        event_add_tag(event, nostr_tag_new("description", definition->description, NULL));
#undef ADD_LIST

    if (nostr_communikeys_definition_validate_event(event) != NOSTR_COMMUNIKEYS_OK) {
        nostr_event_free(event);
        return NULL;
    }
    return event;
}

const nostr_communikeys_section_t *
nostr_communikeys_definition_find_section(const nostr_communikeys_definition_t *definition,
                                          int kind,
                                          const char *subtype) {
    if (!definition || !definition->valid) return NULL;
    const nostr_communikeys_section_t *found = NULL;
    const char *wanted = empty_subtype(subtype);
    for (size_t i = 0; i < definition->sections_len; ++i)
        for (size_t j = 0; j < definition->sections[i].assignments_len; ++j) {
            const nostr_communikeys_assignment_t *a =
                &definition->sections[i].assignments[j];
            if (a->kind == kind &&
                strcmp(empty_subtype(a->subtype), wanted) == 0) {
                if (found && found != &definition->sections[i]) return NULL;
                found = &definition->sections[i];
            }
        }
    return found;
}

void nostr_communikeys_targeted_publication_clear(
    nostr_communikeys_targeted_publication_t *publication) {
    if (!publication) return;
    free(publication->identifier);
    free(publication->reference);
    free(publication->reference_relay);
    free(publication->reference_author);
    free(publication->original_author);
    for (size_t i = 0; i < publication->targets_len; ++i) {
        free(publication->targets[i].pubkey);
        free(publication->targets[i].relay);
    }
    free(publication->targets);
    memset(publication, 0, sizeof(*publication));
}

static bool append_target(nostr_communikeys_targeted_publication_t *publication,
                          const char *pubkey, const char *relay) {
    nostr_communikeys_target_t *next =
        realloc(publication->targets,
                (publication->targets_len + 1) * sizeof(*next));
    if (!next) return false;
    publication->targets = next;
    nostr_communikeys_target_t *target =
        &publication->targets[publication->targets_len++];
    memset(target, 0, sizeof(*target));
    target->pubkey = ck_strdup(pubkey);
    target->relay = relay && *relay ? ck_strdup(relay) : NULL;
    if (!target->pubkey || (relay && *relay && !target->relay)) {
        free(target->pubkey);
        free(target->relay);
        publication->targets_len--;
        return false;
    }
    return true;
}

nostr_communikeys_status_t
nostr_communikeys_targeted_publication_parse(
    const NostrEvent *event,
    nostr_communikeys_targeted_publication_t *out) {
    if (!event || !out) return NOSTR_COMMUNIKEYS_ERR_NULL;
    if (nostr_event_get_kind(event) != CAS_TARGETED_PUBLICATION)
        return NOSTR_COMMUNIKEYS_ERR_WRONG_KIND;
    memset(out, 0, sizeof(*out));
    const char *content = nostr_event_get_content(event);
    if (content && *content) return NOSTR_COMMUNIKEYS_ERR_BAD_CONTENT;
    const char *author = nostr_event_get_pubkey(event);
    if (!nostr_communikeys_is_lower_hex_32(author))
        return NOSTR_COMMUNIKEYS_ERR_BAD_PUBKEY;
    out->original_author = ck_strdup(author);
    if (!out->original_author) goto oom;

    size_t d_count = 0, k_count = 0, ref_count = 0;
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(event);
    for (size_t i = 0; tags && i < nostr_tags_size(tags); ++i) {
        const NostrTag *tag = nostr_tags_get(tags, i);
        const char *key = nostr_tag_get_key(tag);
        size_t size = nostr_tag_size(tag);
        const char *value = size > 1 ? nostr_tag_get(tag, 1) : NULL;
        if (!key) continue;
        if (strcmp(key, "d") == 0) {
            d_count++;
            if (size != 2 || !value || !*value)
                goto bad_cardinality;
            free(out->identifier);
            out->identifier = ck_strdup(value);
            if (!out->identifier) goto oom;
        } else if (strcmp(key, "k") == 0) {
            k_count++;
            if (size != 2 || !parse_decimal_kind(value, &out->original_kind))
                goto bad_reference;
        } else if (strcmp(key, "e") == 0 || strcmp(key, "a") == 0) {
            ref_count++;
            if ((size < 2 || size > 4) || !value || !*value)
                goto bad_reference;
            out->reference_type = strcmp(key, "e") == 0
                ? NOSTR_COMMUNIKEYS_REFERENCE_EVENT
                : NOSTR_COMMUNIKEYS_REFERENCE_ADDRESS;
            free(out->reference);
            free(out->reference_relay);
            free(out->reference_author);
            out->reference = ck_strdup(value);
            const char *relay = size >= 3 ? nostr_tag_get(tag, 2) : NULL;
            out->reference_relay = relay && *relay ? ck_strdup(relay) : NULL;
            const char *hint = size >= 4 ? nostr_tag_get(tag, 3) : NULL;
            out->reference_author = hint && *hint ? ck_strdup(hint) : NULL;
            if (!out->reference || (relay && *relay && !out->reference_relay) ||
                (hint && *hint && !out->reference_author)) goto oom;
            if (out->reference_type == NOSTR_COMMUNIKEYS_REFERENCE_EVENT) {
                if (!nostr_communikeys_is_lower_hex_32(value) ||
                    (out->reference_author &&
                     !nostr_communikeys_is_lower_hex_32(out->reference_author)))
                    goto bad_reference;
            } else {
                nostr_communikeys_coordinate_t coordinate = {0};
                bool coordinate_ok =
                    nostr_communikeys_coordinate_parse(value, relay, &coordinate);
                if (size == 4 || !coordinate_ok ||
                    coordinate.kind < 30000 || coordinate.kind >= 40000) {
                    nostr_communikeys_coordinate_clear(&coordinate);
                    goto bad_reference;
                }
                free(out->reference_author);
                out->reference_author = ck_strdup(coordinate.pubkey);
                nostr_communikeys_coordinate_clear(&coordinate);
                if (!out->reference_author) goto oom;
            }
        } else if (strcmp(key, "p") == 0) {
            if (size != 2 || !nostr_communikeys_is_lower_hex_32(value))
                goto bad_targets;
            for (size_t j = 0; j < out->targets_len; ++j)
                if (strcmp(out->targets[j].pubkey, value) == 0)
                    goto bad_targets;
            const char *relay = NULL;
            if (i + 1 < nostr_tags_size(tags)) {
                const NostrTag *next = nostr_tags_get(tags, i + 1);
                if (next && nostr_tag_get_key(next) &&
                    strcmp(nostr_tag_get_key(next), "r") == 0 &&
                    nostr_tag_size(next) == 2)
                    relay = nostr_tag_get(next, 1);
            }
            if (!append_target(out, value, relay)) goto oom;
        }
    }
    if (d_count != 1 || k_count != 1 || ref_count != 1)
        goto bad_cardinality;
    if (out->targets_len < 1 ||
        out->targets_len > NOSTR_COMMUNIKEYS_MAX_TARGETS)
        goto bad_targets;
    if (out->reference_type == NOSTR_COMMUNIKEYS_REFERENCE_ADDRESS) {
        nostr_communikeys_coordinate_t coordinate = {0};
        if (!nostr_communikeys_coordinate_parse(
                out->reference, out->reference_relay, &coordinate) ||
            coordinate.kind != out->original_kind) {
            nostr_communikeys_coordinate_clear(&coordinate);
            goto bad_reference;
        }
        nostr_communikeys_coordinate_clear(&coordinate);
    }
    if (out->reference_author &&
        strcmp(out->reference_author, out->original_author) != 0)
        goto author_mismatch;
    return NOSTR_COMMUNIKEYS_OK;

bad_cardinality:
    nostr_communikeys_targeted_publication_clear(out);
    return NOSTR_COMMUNIKEYS_ERR_CARDINALITY;
bad_reference:
    nostr_communikeys_targeted_publication_clear(out);
    return NOSTR_COMMUNIKEYS_ERR_BAD_REFERENCE;
bad_targets:
    nostr_communikeys_targeted_publication_clear(out);
    return NOSTR_COMMUNIKEYS_ERR_BAD_TARGETS;
author_mismatch:
    nostr_communikeys_targeted_publication_clear(out);
    return NOSTR_COMMUNIKEYS_ERR_AUTHOR_MISMATCH;
oom:
    nostr_communikeys_targeted_publication_clear(out);
    return NOSTR_COMMUNIKEYS_ERR_OOM;
}

static char *event_address(const NostrEvent *event) {
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(event);
    const char *d = NULL;
    size_t count = 0;
    for (size_t i = 0; tags && i < nostr_tags_size(tags); ++i) {
        const NostrTag *tag = nostr_tags_get(tags, i);
        if (nostr_tag_get_key(tag) && strcmp(nostr_tag_get_key(tag), "d") == 0) {
            count++;
            d = nostr_tag_get_value(tag);
        }
    }
    if (count != 1 || !d || !*d ||
        !nostr_communikeys_is_lower_hex_32(nostr_event_get_pubkey(event))) return NULL;
    nostr_communikeys_coordinate_t coordinate = {
        .kind = nostr_event_get_kind(event),
        .pubkey = (char *)nostr_event_get_pubkey(event),
        .identifier = (char *)d
    };
    return nostr_communikeys_coordinate_format(&coordinate);
}

nostr_communikeys_status_t
nostr_communikeys_targeted_publication_validate(
    const NostrEvent *event,
    const NostrEvent *original) {
    nostr_communikeys_targeted_publication_t publication;
    nostr_communikeys_status_t status =
        nostr_communikeys_targeted_publication_parse(event, &publication);
    if (status != NOSTR_COMMUNIKEYS_OK) return status;
    if (!original) {
        nostr_communikeys_targeted_publication_clear(&publication);
        return NOSTR_COMMUNIKEYS_OK;
    }

    if (!nostr_event_get_pubkey(original) ||
        strcmp(publication.original_author,
               nostr_event_get_pubkey(original)) != 0 ||
        publication.original_kind != nostr_event_get_kind(original)) {
        status = NOSTR_COMMUNIKEYS_ERR_AUTHOR_MISMATCH;
    } else if (publication.reference_type ==
               NOSTR_COMMUNIKEYS_REFERENCE_EVENT) {
        char *computed = nostr_event_get_id((NostrEvent *)original);
        if (!computed || strcmp(publication.reference, computed) != 0)
            status = NOSTR_COMMUNIKEYS_ERR_REFERENCE_MISMATCH;
        free(computed);
    } else {
        char *address = event_address(original);
        if (!address || strcmp(publication.reference, address) != 0)
            status = NOSTR_COMMUNIKEYS_ERR_REFERENCE_MISMATCH;
        free(address);
    }
    nostr_communikeys_targeted_publication_clear(&publication);
    return status;
}

NostrEvent *
nostr_communikeys_targeted_publication_to_event(
    const nostr_communikeys_targeted_publication_t *publication,
    int64_t created_at) {
    if (!publication || !publication->identifier || !*publication->identifier ||
        !publication->reference || !*publication->reference ||
        !nostr_communikeys_is_lower_hex_32(publication->original_author) ||
        publication->targets_len < 1 ||
        publication->targets_len > NOSTR_COMMUNIKEYS_MAX_TARGETS)
        return NULL;
    NostrEvent *event = nostr_event_new();
    if (!event) return NULL;
    nostr_event_set_kind(event, CAS_TARGETED_PUBLICATION);
    nostr_event_set_pubkey(event, publication->original_author);
    nostr_event_set_created_at(event, created_at);
    nostr_event_set_content(event, "");
    event_add_tag(event, nostr_tag_new("d", publication->identifier, NULL));
    const char *ref_key =
        publication->reference_type == NOSTR_COMMUNIKEYS_REFERENCE_EVENT ? "e" : "a";
    if (publication->reference_type == NOSTR_COMMUNIKEYS_REFERENCE_EVENT &&
        publication->reference_author)
        event_add_tag(event, nostr_tag_new(ref_key, publication->reference,
                                          publication->reference_relay
                                              ? publication->reference_relay : "",
                                          publication->reference_author, NULL));
    else if (publication->reference_relay)
        event_add_tag(event, nostr_tag_new(ref_key, publication->reference,
                                          publication->reference_relay, NULL));
    else
        event_add_tag(event, nostr_tag_new(ref_key, publication->reference, NULL));
    char kind[16];
    snprintf(kind, sizeof(kind), "%d", publication->original_kind);
    event_add_tag(event, nostr_tag_new("k", kind, NULL));
    for (size_t i = 0; i < publication->targets_len; ++i) {
        event_add_tag(event, nostr_tag_new("p", publication->targets[i].pubkey, NULL));
        if (publication->targets[i].relay)
            event_add_tag(event, nostr_tag_new("r", publication->targets[i].relay, NULL));
    }
    if (nostr_communikeys_targeted_publication_validate(event, NULL) !=
        NOSTR_COMMUNIKEYS_OK) {
        nostr_event_free(event);
        return NULL;
    }
    return event;
}

nostr_communikeys_status_t
nostr_communikeys_exclusive_validate(const NostrEvent *event,
                                     char community_pubkey[65]) {
    if (!event) return NOSTR_COMMUNIKEYS_ERR_NULL;
    int kind = nostr_event_get_kind(event);
    if (kind != 9 && kind != 11) return NOSTR_COMMUNIKEYS_ERR_WRONG_KIND;
    const char *community = NULL;
    size_t count = 0;
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(event);
    for (size_t i = 0; tags && i < nostr_tags_size(tags); ++i) {
        const NostrTag *tag = nostr_tags_get(tags, i);
        if (nostr_tag_get_key(tag) && strcmp(nostr_tag_get_key(tag), "h") == 0) {
            count++;
            if (nostr_tag_size(tag) != 2) return NOSTR_COMMUNIKEYS_ERR_BAD_TAG;
            community = nostr_tag_get_value(tag);
        }
    }
    if (count != 1) return NOSTR_COMMUNIKEYS_ERR_CARDINALITY;
    if (!nostr_communikeys_is_lower_hex_32(community))
        return NOSTR_COMMUNIKEYS_ERR_BAD_PUBKEY;
    if (community_pubkey) memcpy(community_pubkey, community, 65);
    return NOSTR_COMMUNIKEYS_OK;
}

bool nostr_communikeys_exclusive_add_h(NostrEvent *event,
                                       const char *community_pubkey) {
    if (!event || !nostr_communikeys_is_lower_hex_32(community_pubkey))
        return false;
    int kind = nostr_event_get_kind(event);
    if (kind != 9 && kind != 11) return false;
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(event);
    for (size_t i = 0; tags && i < nostr_tags_size(tags); ++i) {
        const NostrTag *tag = nostr_tags_get(tags, i);
        if (nostr_tag_get_key(tag) && strcmp(nostr_tag_get_key(tag), "h") == 0)
            return false;
    }
    event_add_tag(event, nostr_tag_new("h", community_pubkey, NULL));
    return nostr_communikeys_exclusive_validate(event, NULL) ==
           NOSTR_COMMUNIKEYS_OK;
}

void nostr_communikeys_identifier_clear(nostr_communikeys_identifier_t *identifier) {
    if (!identifier) return;
    clear_strings(identifier->relays, identifier->relays_len);
    memset(identifier, 0, sizeof(*identifier));
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static char *percent_decode(const char *value, size_t len) {
    char *out = malloc(len + 1);
    if (!out) return NULL;
    size_t w = 0;
    for (size_t i = 0; i < len; ++i) {
        if (value[i] == '%') {
            if (i + 2 >= len) {
                free(out);
                return NULL;
            }
            int hi = hex_value(value[i + 1]), lo = hex_value(value[i + 2]);
            if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) {
                free(out);
                return NULL;
            }
            out[w++] = (char)((hi << 4) | lo);
            i += 2;
        } else {
            out[w++] = value[i];
        }
    }
    out[w] = '\0';
    return out;
}

static bool valid_relay_url(const char *url) {
    const char *rest = NULL;
    if (url && strncmp(url, "ws://", 5) == 0) rest = url + 5;
    else if (url && strncmp(url, "wss://", 6) == 0) rest = url + 6;
    if (!rest || !*rest || *rest == '/') return false;
    for (const unsigned char *p = (const unsigned char *)rest; *p; ++p)
        if (*p <= 0x20 || *p == 0x7f) return false;
    return true;
}

nostr_communikeys_status_t
nostr_communikeys_identifier_parse(const char *value,
                                   nostr_communikeys_identifier_t *out) {
    if (!value || !out) return NOSTR_COMMUNIKEYS_ERR_NULL;
    memset(out, 0, sizeof(*out));
    if (nostr_communikeys_is_lower_hex_32(value)) {
        memcpy(out->pubkey, value, 65);
        return NOSTR_COMMUNIKEYS_OK;
    }
    static const char prefix[] = "ncommunity://";
    if (strncmp(value, prefix, sizeof(prefix) - 1) != 0)
        return NOSTR_COMMUNIKEYS_ERR_BAD_URI;
    const char *authority = value + sizeof(prefix) - 1;
    const char *query = strchr(authority, '?');
    size_t authority_len = query ? (size_t)(query - authority) : strlen(authority);
    if (authority_len != 64 ||
        memchr(authority, '/', authority_len) ||
        memchr(authority, '#', authority_len))
        return NOSTR_COMMUNIKEYS_ERR_BAD_URI;
    char pubkey[65];
    memcpy(pubkey, authority, 64);
    pubkey[64] = '\0';
    if (!nostr_communikeys_is_lower_hex_32(pubkey))
        return NOSTR_COMMUNIKEYS_ERR_BAD_URI;
    memcpy(out->pubkey, pubkey, 65);
    if (!query) return NOSTR_COMMUNIKEYS_OK;
    if (strchr(query, '#')) {
        nostr_communikeys_identifier_clear(out);
        return NOSTR_COMMUNIKEYS_ERR_BAD_URI;
    }

    const char *p = query + 1;
    while (*p) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        size_t key_len = eq ? (size_t)(eq - p) : (size_t)(end - p);
        char *key = percent_decode(p, key_len);
        if (!key) goto bad;
        if (strcmp(key, "relay") == 0) {
            if (!eq) {
                free(key);
                goto bad;
            }
            char *relay = percent_decode(eq + 1, (size_t)(end - eq - 1));
            if (!relay || !valid_relay_url(relay) ||
                !append_string(&out->relays, &out->relays_len, relay)) {
                free(relay);
                free(key);
                goto bad;
            }
            free(relay);
        }
        free(key);
        p = *end ? end + 1 : end;
    }
    return NOSTR_COMMUNIKEYS_OK;
bad:
    nostr_communikeys_identifier_clear(out);
    return NOSTR_COMMUNIKEYS_ERR_BAD_URI;
}

static bool uri_unreserved(unsigned char c) {
    return isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

static size_t encoded_len(const char *value) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        n += uri_unreserved(*p) ? 1 : 3;
    return n;
}

static char *percent_encode_into(char *out, const char *value) {
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (uri_unreserved(*p)) *out++ = (char)*p;
        else {
            *out++ = '%';
            *out++ = hex[*p >> 4];
            *out++ = hex[*p & 15];
        }
    }
    return out;
}

char *nostr_communikeys_identifier_format(
    const nostr_communikeys_identifier_t *identifier) {
    if (!identifier ||
        !nostr_communikeys_is_lower_hex_32(identifier->pubkey)) return NULL;
    size_t len = strlen("ncommunity://") + 64;
    for (size_t i = 0; i < identifier->relays_len; ++i) {
        if (!valid_relay_url(identifier->relays[i])) return NULL;
        len += strlen(i == 0 ? "?relay=" : "&relay=") +
               encoded_len(identifier->relays[i]);
    }
    char *out = malloc(len + 1);
    if (!out) return NULL;
    char *w = out;
    memcpy(w, "ncommunity://", strlen("ncommunity://"));
    w += strlen("ncommunity://");
    memcpy(w, identifier->pubkey, 64);
    w += 64;
    for (size_t i = 0; i < identifier->relays_len; ++i) {
        const char *prefix = i == 0 ? "?relay=" : "&relay=";
        size_t n = strlen(prefix);
        memcpy(w, prefix, n);
        w += n;
        w = percent_encode_into(w, identifier->relays[i]);
    }
    *w = '\0';
    return out;
}

void nostr_communikeys_profile_list_clear(nostr_communikeys_profile_list_t *list) {
    if (!list) return;
    free(list->publisher);
    free(list->identifier);
    clear_strings(list->members, list->members_len);
    memset(list, 0, sizeof(*list));
}

nostr_communikeys_status_t
nostr_communikeys_profile_list_parse(const NostrEvent *event,
                                     const char *expected_publisher,
                                     const char *expected_identifier,
                                     nostr_communikeys_profile_list_t *out) {
    if (!event || !out) return NOSTR_COMMUNIKEYS_ERR_NULL;
    memset(out, 0, sizeof(*out));
    if (nostr_event_get_kind(event) != NOSTR_COMMUNIKEYS_KIND_PROFILE_LIST)
        return NOSTR_COMMUNIKEYS_ERR_WRONG_KIND;
    const char *publisher = nostr_event_get_pubkey(event);
    if (!nostr_communikeys_is_lower_hex_32(publisher))
        return NOSTR_COMMUNIKEYS_ERR_BAD_PUBKEY;
    if (expected_publisher && strcmp(expected_publisher, publisher) != 0)
        return NOSTR_COMMUNIKEYS_ERR_AUTHOR_MISMATCH;
    out->publisher = ck_strdup(publisher);
    if (!out->publisher) goto oom;

    size_t d_count = 0;
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(event);
    for (size_t i = 0; tags && i < nostr_tags_size(tags); ++i) {
        const NostrTag *tag = nostr_tags_get(tags, i);
        const char *key = nostr_tag_get_key(tag);
        const char *value = nostr_tag_get_value(tag);
        if (!key) continue;
        if (strcmp(key, "d") == 0) {
            d_count++;
            if (nostr_tag_size(tag) != 2 || !value || !*value) goto cardinality;
            free(out->identifier);
            out->identifier = ck_strdup(value);
            if (!out->identifier) goto oom;
        } else if (strcmp(key, "p") == 0) {
            if (nostr_tag_size(tag) < 2 ||
                !nostr_communikeys_is_lower_hex_32(value))
                goto bad_pubkey;
            if (!strings_contains(out->members, out->members_len, value) &&
                !append_string(&out->members, &out->members_len, value)) goto oom;
        }
    }
    if (d_count != 1) goto cardinality;
    if (expected_identifier &&
        strcmp(expected_identifier, out->identifier) != 0) {
        nostr_communikeys_profile_list_clear(out);
        return NOSTR_COMMUNIKEYS_ERR_REFERENCE_MISMATCH;
    }
    return NOSTR_COMMUNIKEYS_OK;
cardinality:
    nostr_communikeys_profile_list_clear(out);
    return NOSTR_COMMUNIKEYS_ERR_CARDINALITY;
bad_pubkey:
    nostr_communikeys_profile_list_clear(out);
    return NOSTR_COMMUNIKEYS_ERR_BAD_PUBKEY;
oom:
    nostr_communikeys_profile_list_clear(out);
    return NOSTR_COMMUNIKEYS_ERR_OOM;
}

bool nostr_communikeys_profile_list_contains(
    const nostr_communikeys_profile_list_t *list,
    const char *pubkey) {
    return list && pubkey &&
           strings_contains(list->members, list->members_len, pubkey);
}

bool nostr_communikeys_author_can_publish(
    const nostr_communikeys_definition_t *definition,
    int kind,
    const char *subtype,
    const NostrEvent *profile_list_event,
    const char *author_pubkey) {
    if (!definition || !definition->valid || !profile_list_event ||
        !nostr_communikeys_is_lower_hex_32(author_pubkey)) return false;
    const nostr_communikeys_section_t *section =
        nostr_communikeys_definition_find_section(definition, kind, subtype);
    if (!section || !section->profile_list.pubkey) return false;
    nostr_communikeys_profile_list_t list;
    nostr_communikeys_status_t status = nostr_communikeys_profile_list_parse(
        profile_list_event, section->profile_list.pubkey,
        section->profile_list.identifier, &list);
    if (status != NOSTR_COMMUNIKEYS_OK) return false;
    bool allowed = nostr_communikeys_profile_list_contains(&list, author_pubkey);
    nostr_communikeys_profile_list_clear(&list);
    return allowed;
}
