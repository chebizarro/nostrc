#include "nip_communikeys.h"

#include "nostr/nip19/nip19.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JS_MAX_SAFE_INTEGER INT64_C(9007199254740991)
#define MAX_URL_BYTES 2048

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

static bool ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\f' || c == '\v';
}

/* Lengths are UTF-8 bytes after trimming leading/trailing ASCII whitespace. */
static char *trimmed_dup(const char *s) {
    if (!s) return NULL;
    const char *start = s;
    while (*start && ascii_space(*start)) start++;
    const char *end = s + strlen(s);
    while (end > start && ascii_space(end[-1])) end--;
    return ck_strndup(start, (size_t)(end - start));
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
        case NOSTR_COMMUNIKEYS_ERR_SECTION_ACL: return "invalid section profile-list reference";
        case NOSTR_COMMUNIKEYS_ERR_DUPLICATE_ASSIGNMENT: return "duplicate exact kind/subtype assignment";
        case NOSTR_COMMUNIKEYS_ERR_BAD_COORDINATE: return "invalid event coordinate";
        case NOSTR_COMMUNIKEYS_ERR_BAD_REFERENCE: return "invalid publication source reference";
        case NOSTR_COMMUNIKEYS_ERR_BAD_TARGETS: return "invalid community target count";
        case NOSTR_COMMUNIKEYS_ERR_AUTHOR_MISMATCH: return "publication author mismatch";
        case NOSTR_COMMUNIKEYS_ERR_REFERENCE_MISMATCH: return "publication reference mismatch";
        case NOSTR_COMMUNIKEYS_ERR_BAD_BRANCH: return "invalid community branch address";
        case NOSTR_COMMUNIKEYS_ERR_BAD_SECTION_IDENTIFIER: return "invalid section-scoped identifier";
        case NOSTR_COMMUNIKEYS_ERR_TARGET_PAIR: return "invalid h+a community target pair";
        case NOSTR_COMMUNIKEYS_ERR_MISSING_NAME: return "definition has no valid name tag";
        case NOSTR_COMMUNIKEYS_ERR_BAD_METADATA: return "invalid definition metadata tag";
    }
    return "unknown";
}

/* Canonical unsigned decimal: no sign, no leading zero except exactly "0". */
static bool parse_canonical_u16(const char *value, int *out) {
    if (!value || !*value || !out) return false;
    if (value[0] == '0' && value[1]) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (!isdigit(*p)) return false;
    errno = 0;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (errno || !end || *end || parsed < 0 || parsed > 65535) return false;
    *out = (int)parsed;
    return true;
}

static bool parse_canonical_positive_i64(const char *value, int64_t *out) {
    if (!value || !*value || !out) return false;
    if (value[0] == '0') return false; /* positive: no zero, no leading zero */
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (!isdigit(*p)) return false;
    errno = 0;
    char *end = NULL;
    long long parsed = strtoll(value, &end, 10);
    if (errno || !end || *end || parsed <= 0 || parsed > JS_MAX_SAFE_INTEGER)
        return false;
    *out = (int64_t)parsed;
    return true;
}

/* Pragmatic URL validation for the definition metadata table: exact lowercase
 * scheme prefix, non-empty host, no credentials, no fragment, no whitespace or
 * control bytes, at most MAX_URL_BYTES bytes. */
static bool valid_url_with_scheme(const char *url, const char *prefix) {
    if (!url) return false;
    size_t len = strlen(url);
    size_t prefix_len = strlen(prefix);
    if (len > MAX_URL_BYTES || len <= prefix_len) return false;
    if (strncmp(url, prefix, prefix_len) != 0) return false;
    const char *rest = url + prefix_len;
    if (*rest == '/' ) return false; /* empty host */
    const char *path = strchr(rest, '/');
    size_t authority_len = path ? (size_t)(path - rest) : strlen(rest);
    if (authority_len == 0) return false;
    if (memchr(rest, '@', authority_len)) return false; /* credentials */
    for (const unsigned char *p = (const unsigned char *)url; *p; ++p)
        if (*p <= 0x20 || *p == 0x7f || *p == '#') return false;
    return true;
}

static bool valid_wss_url(const char *url) {
    return valid_url_with_scheme(url, "wss://");
}

static bool valid_https_url(const char *url) {
    return valid_url_with_scheme(url, "https://");
}

static bool valid_web_url(const char *url) {
    return valid_url_with_scheme(url, "https://") ||
           valid_url_with_scheme(url, "http://");
}

bool nostr_communikeys_branch_parse(const char *address,
                                    nostr_communikeys_branch_t *out) {
    if (!address || !out) return false;
    memset(out, 0, sizeof(*out));
    if (strncmp(address, "32222:", 6) != 0) return false;
    const char *owner = address + 6;
    const char *sep = strchr(owner, ':');
    if (!sep || (size_t)(sep - owner) != 64) return false;
    const char *id = sep + 1;
    if (strlen(id) != 64) return false;
    char owner_buf[65];
    memcpy(owner_buf, owner, 64);
    owner_buf[64] = '\0';
    if (!nostr_communikeys_is_lower_hex_32(owner_buf) ||
        !nostr_communikeys_is_lower_hex_32(id)) return false;
    memcpy(out->owner, owner_buf, 65);
    memcpy(out->community_id, id, 65);
    return true;
}

char *nostr_communikeys_branch_format(const nostr_communikeys_branch_t *branch) {
    if (!branch ||
        !nostr_communikeys_is_lower_hex_32(branch->owner) ||
        !nostr_communikeys_is_lower_hex_32(branch->community_id)) return NULL;
    size_t len = 6 + 64 + 1 + 64;
    char *out = malloc(len + 1);
    if (!out) return NULL;
    snprintf(out, len + 1, "32222:%s:%s", branch->owner, branch->community_id);
    return out;
}

bool nostr_communikeys_branch_equal(const nostr_communikeys_branch_t *a,
                                    const nostr_communikeys_branch_t *b) {
    return a && b &&
           strcmp(a->owner, b->owner) == 0 &&
           strcmp(a->community_id, b->community_id) == 0;
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
        !parse_canonical_u16(kind_text, &kind) ||
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

static bool valid_purpose_token(const char *token, size_t len) {
    if (len == 0) return false;
    if (token[0] == '-' || token[len - 1] == '-') return false;
    for (size_t i = 0; i < len; ++i) {
        char c = token[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
        if (c == '-' && i + 1 < len && token[i + 1] == '-') return false;
    }
    return true;
}

/* Shard grammar: canonical decimal integer beginning at 2. */
static bool parse_shard(const char *text, int *out) {
    if (!text || !*text || !out) return false;
    if (text[0] == '0') return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if (!isdigit(*p)) return false;
    errno = 0;
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (errno || !end || *end || parsed < 2 || parsed > 1000000000L) return false;
    *out = (int)parsed;
    return true;
}

bool nostr_communikeys_section_identifier_parse(const char *d,
                                                const char *community_id,
                                                char **out_purpose,
                                                int *out_shard) {
    if (out_purpose) *out_purpose = NULL;
    if (out_shard) *out_shard = 1;
    if (!d || !nostr_communikeys_is_lower_hex_32(community_id)) return false;
    size_t total = strlen(d);
    if (total > NOSTR_COMMUNIKEYS_MAX_SECTION_D) return false;
    /* The complete community ID MUST NOT be truncated or hashed. */
    if (strncmp(d, community_id, 64) != 0 || d[64] != '-') return false;
    const char *rest = d + 65;
    if (!*rest) return false;
    /* The period is a reserved shard separator; at most one may appear. */
    const char *dot = strchr(rest, '.');
    if (dot && strchr(dot + 1, '.')) return false;
    size_t purpose_len = dot ? (size_t)(dot - rest) : strlen(rest);
    if (!valid_purpose_token(rest, purpose_len)) return false;
    int shard = 1;
    if (dot && !parse_shard(dot + 1, &shard)) return false;
    if (out_purpose) {
        *out_purpose = ck_strndup(rest, purpose_len);
        if (!*out_purpose) return false;
    }
    if (out_shard) *out_shard = shard;
    return true;
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
    for (size_t i = 0; i < section->profile_lists_len; ++i) {
        nostr_communikeys_coordinate_clear(&section->profile_lists[i].coordinate);
        free(section->profile_lists[i].purpose);
    }
    free(section->profile_lists);
    for (size_t i = 0; i < section->badges_len; ++i) {
        free(section->badges[i].coordinate);
        free(section->badges[i].relay);
    }
    free(section->badges);
    free(section->retention);
    memset(section, 0, sizeof(*section));
}

void nostr_communikeys_definition_clear(nostr_communikeys_definition_t *definition) {
    if (!definition) return;
    free(definition->name);
    free(definition->description);
    free(definition->picture);
    free(definition->banner);
    free(definition->website);
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
    for (size_t i = 0; i < definition->services_len; ++i) {
        free(definition->services[i].name);
        free(definition->services[i].request_relay);
        free(definition->services[i].handler_address);
        free(definition->services[i].handler_relay);
    }
    free(definition->services);
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

static bool append_section(nostr_communikeys_definition_t *definition,
                           const char *name) {
    nostr_communikeys_section_t *next =
        realloc(definition->sections,
                (definition->sections_len + 1) * sizeof(*next));
    if (!next) return false;
    definition->sections = next;
    nostr_communikeys_section_t *section =
        &definition->sections[definition->sections_len];
    memset(section, 0, sizeof(*section));
    section->name = ck_strdup(name);
    if (!section->name) return false;
    definition->sections_len++;
    return true;
}

static bool append_assignment(nostr_communikeys_section_t *section,
                              int kind, const char *subtype) {
    nostr_communikeys_assignment_t *next =
        realloc(section->assignments,
                (section->assignments_len + 1) * sizeof(*next));
    if (!next) return false;
    section->assignments = next;
    nostr_communikeys_assignment_t *assignment =
        &section->assignments[section->assignments_len];
    assignment->kind = kind;
    assignment->subtype = subtype && *subtype ? ck_strdup(subtype) : NULL;
    if (subtype && *subtype && !assignment->subtype) return false;
    section->assignments_len++;
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

static bool ascii_casefold_equal(const char *a, const char *b) {
    if (!a || !b) return a == b;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        a++;
        b++;
    }
    return !*a && !*b;
}

static bool append_mint(nostr_communikeys_definition_t *definition,
                        const char *url, const char *protocol) {
    nostr_communikeys_mint_t *next =
        realloc(definition->mints,
                (definition->mints_len + 1) * sizeof(*next));
    if (!next) return false;
    definition->mints = next;
    nostr_communikeys_mint_t *mint =
        &definition->mints[definition->mints_len];
    memset(mint, 0, sizeof(*mint));
    mint->url = ck_strdup(url);
    mint->protocol = protocol && *protocol ? ck_strdup(protocol) : NULL;
    if (!mint->url || (protocol && *protocol && !mint->protocol)) {
        free(mint->url);
        free(mint->protocol);
        return false;
    }
    definition->mints_len++;
    return true;
}

static bool mint_exists(const nostr_communikeys_definition_t *definition,
                        const char *url, const char *protocol) {
    for (size_t i = 0; i < definition->mints_len; ++i) {
        if (strcmp(definition->mints[i].url, url) != 0) continue;
        const char *have = definition->mints[i].protocol
            ? definition->mints[i].protocol : "";
        const char *want = protocol ? protocol : "";
        if (strcmp(have, want) == 0) return true;
    }
    return false;
}

static bool valid_service_name(const char *name) {
    if (!name || !*name) return false;
    size_t len = strlen(name);
    if (len > 32) return false;
    char c0 = name[0];
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= '0' && c0 <= '9'))) return false;
    for (size_t i = 1; i < len; ++i) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    }
    return true;
}

static bool service_exists(const nostr_communikeys_definition_t *definition,
                           const nostr_communikeys_service_t *service) {
    for (size_t i = 0; i < definition->services_len; ++i) {
        const nostr_communikeys_service_t *have = &definition->services[i];
        if (strcmp(have->name, service->name) == 0 &&
            strcmp(have->pubkey, service->pubkey) == 0 &&
            strcmp(have->request_relay, service->request_relay) == 0 &&
            strcmp(have->handler_address, service->handler_address) == 0 &&
            strcmp(have->handler_relay, service->handler_relay) == 0)
            return true;
    }
    return false;
}

static bool set_single(char **slot, const char *value) {
    if (*slot) return false;
    *slot = ck_strdup(value);
    return *slot != NULL;
}

static bool is_recognized_top_level(const char *key) {
    static const char *keys[] = {
        "d", "name", "description", "picture", "banner", "website",
        "r", "blossom", "grasp", "mint", "location", "g", "tos", "service"
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        if (strcmp(key, keys[i]) == 0) return true;
    return false;
}

static bool is_recognized_section_local(const char *key) {
    return strcmp(key, "k") == 0 || strcmp(key, "a") == 0 ||
           strcmp(key, "badge") == 0 || strcmp(key, "retention") == 0;
}

bool nostr_communikeys_definition_parse(const NostrEvent *event,
                                        nostr_communikeys_definition_t *out) {
    if (!event || !out || nostr_event_get_kind(event) != CAS_COMMUNITY_DEFINITION)
        return false;
    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->validation_status = NOSTR_COMMUNIKEYS_OK;

    const char *pubkey = nostr_event_get_pubkey(event);
    if (nostr_communikeys_is_lower_hex_32(pubkey))
        memcpy(out->branch.owner, pubkey, 65);
    else
        definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_PUBKEY);

    const char *content = nostr_event_get_content(event);
    if (content && *content)
        definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_CONTENT);

    size_t d_count = 0, name_count = 0;
    size_t r_count = 0, blossom_count = 0, grasp_count = 0,
           mint_count = 0, service_count = 0;
    bool in_section = false;
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(event);
    for (size_t i = 0; tags && i < nostr_tags_size(tags); ++i) {
        const NostrTag *tag = nostr_tags_get(tags, i);
        const char *key = tag ? nostr_tag_get_key(tag) : NULL;
        size_t size = tag ? nostr_tag_size(tag) : 0;
        const char *value = size > 1 ? nostr_tag_get(tag, 1) : NULL;
        if (!key || !*key) {
            definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_TAG);
            continue;
        }

        /* Definition Validity rule 4: no community-identifying h tag. */
        if (strcmp(key, "h") == 0) {
            definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_TAG);
            continue;
        }

        if (strcmp(key, "content") == 0) {
            in_section = true;
            if (size != 2 || !value) {
                definition_error(out, NOSTR_COMMUNIKEYS_ERR_SECTION_NAME);
                continue;
            }
            char *name = trimmed_dup(value);
            if (!name) goto oom;
            size_t name_len = strlen(name);
            if (name_len < 1 || name_len > 100) {
                free(name);
                definition_error(out, NOSTR_COMMUNIKEYS_ERR_SECTION_NAME);
                continue;
            }
            for (size_t j = 0; j < out->sections_len; ++j)
                if (ascii_casefold_equal(out->sections[j].name, name))
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_SECTION_NAME);
            bool appended = append_section(out, name);
            free(name);
            if (!appended) goto oom;
            continue;
        }

        if (is_recognized_section_local(key)) {
            if (!in_section || out->sections_len == 0) {
                /* Recognized section-local tag before the first content
                 * invalidates the definition. */
                definition_error(out, NOSTR_COMMUNIKEYS_ERR_SECTION_ORDER);
                continue;
            }
            nostr_communikeys_section_t *current =
                &out->sections[out->sections_len - 1];
            if (strcmp(key, "k") == 0) {
                int kind = 0;
                if ((size != 2 && size != 3) || !parse_canonical_u16(value, &kind)) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_SECTION_KIND);
                    continue;
                }
                const char *subtype = size == 3 ? nostr_tag_get(tag, 2) : NULL;
                if (subtype && (strlen(subtype) < 1 || strlen(subtype) > 64)) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_SECTION_KIND);
                    continue;
                }
                if (assignment_exists(out, kind, subtype)) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_DUPLICATE_ASSIGNMENT);
                    continue;
                }
                if (!append_assignment(current, kind, subtype)) goto oom;
            } else if (strcmp(key, "a") == 0) {
                const char *relay = size == 3 ? nostr_tag_get(tag, 2) : NULL;
                nostr_communikeys_coordinate_t coordinate = {0};
                if ((size != 2 && size != 3) || !value ||
                    !nostr_communikeys_coordinate_parse(value, relay, &coordinate) ||
                    coordinate.kind != NOSTR_COMMUNIKEYS_KIND_PROFILE_LIST ||
                    (relay && *relay && !valid_wss_url(relay))) {
                    nostr_communikeys_coordinate_clear(&coordinate);
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_SECTION_ACL);
                    continue;
                }
                char *purpose = NULL;
                int shard = 1;
                if (!nostr_communikeys_section_identifier_parse(
                        coordinate.identifier, out->branch.community_id,
                        &purpose, &shard)) {
                    nostr_communikeys_coordinate_clear(&coordinate);
                    free(purpose);
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_SECTION_IDENTIFIER);
                    continue;
                }
                nostr_communikeys_profile_list_ref_t *next =
                    realloc(current->profile_lists,
                            (current->profile_lists_len + 1) * sizeof(*next));
                if (!next) {
                    nostr_communikeys_coordinate_clear(&coordinate);
                    free(purpose);
                    goto oom;
                }
                current->profile_lists = next;
                nostr_communikeys_profile_list_ref_t *ref =
                    &current->profile_lists[current->profile_lists_len++];
                ref->coordinate = coordinate;
                ref->purpose = purpose;
                ref->shard = shard;
            } else if (strcmp(key, "badge") == 0) {
                const char *relay = size == 3 ? nostr_tag_get(tag, 2) : NULL;
                nostr_communikeys_coordinate_t badge = {0};
                if ((size != 2 && size != 3) || !value ||
                    !nostr_communikeys_coordinate_parse(value, NULL, &badge) ||
                    badge.kind != NOSTR_COMMUNIKEYS_KIND_BADGE_DEFINITION ||
                    (relay && *relay && !valid_wss_url(relay))) {
                    nostr_communikeys_coordinate_clear(&badge);
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_COORDINATE);
                    continue;
                }
                nostr_communikeys_coordinate_clear(&badge);
                nostr_communikeys_badge_ref_t *next =
                    realloc(current->badges,
                            (current->badges_len + 1) * sizeof(*next));
                if (!next) goto oom;
                current->badges = next;
                nostr_communikeys_badge_ref_t *ref =
                    &current->badges[current->badges_len];
                memset(ref, 0, sizeof(*ref));
                ref->coordinate = ck_strdup(value);
                ref->relay = relay && *relay ? ck_strdup(relay) : NULL;
                if (!ref->coordinate || (relay && *relay && !ref->relay)) {
                    free(ref->coordinate);
                    free(ref->relay);
                    goto oom;
                }
                current->badges_len++;
            } else { /* retention */
                int kind = 0;
                int64_t retention_value = 0;
                const char *type = size == 4 ? nostr_tag_get(tag, 3) : NULL;
                if (size != 4 || !parse_canonical_u16(value, &kind) ||
                    !parse_canonical_positive_i64(nostr_tag_get(tag, 2),
                                                  &retention_value) ||
                    !type ||
                    (strcmp(type, "time") != 0 && strcmp(type, "count") != 0)) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_TAG);
                    continue;
                }
                nostr_communikeys_retention_t *next =
                    realloc(current->retention,
                            (current->retention_len + 1) * sizeof(*next));
                if (!next) goto oom;
                current->retention = next;
                nostr_communikeys_retention_t *retention =
                    &current->retention[current->retention_len++];
                retention->kind = kind;
                retention->value = retention_value;
                retention->type = strcmp(type, "time") == 0
                    ? NOSTR_COMMUNIKEYS_RETENTION_TIME
                    : NOSTR_COMMUNIKEYS_RETENTION_COUNT;
            }
            continue;
        }

        if (is_recognized_top_level(key)) {
            if (in_section) {
                /* A recognized top-level tag inside a section invalidates. */
                definition_error(out, NOSTR_COMMUNIKEYS_ERR_SECTION_ORDER);
                continue;
            }
            if (strcmp(key, "d") == 0) {
                d_count++;
                if (size != 2 || !nostr_communikeys_is_lower_hex_32(value)) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_CARDINALITY);
                    continue;
                }
                if (d_count > 1) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_CARDINALITY);
                    continue;
                }
                memcpy(out->branch.community_id, value, 65);
            } else if (strcmp(key, "name") == 0) {
                name_count++;
                if (size != 2 || !value || name_count > 1) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_MISSING_NAME);
                    continue;
                }
                char *name = trimmed_dup(value);
                if (!name) goto oom;
                size_t len = strlen(name);
                if (len < 1 || len > 100) {
                    free(name);
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_MISSING_NAME);
                    continue;
                }
                free(out->name);
                out->name = name;
            } else if (strcmp(key, "description") == 0) {
                if (size != 2 || !value || out->description) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_METADATA);
                    continue;
                }
                char *text = trimmed_dup(value);
                if (!text) goto oom;
                size_t len = strlen(text);
                if (len < 1 || len > 4096) {
                    free(text);
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_METADATA);
                    continue;
                }
                out->description = text;
            } else if (strcmp(key, "picture") == 0 ||
                       strcmp(key, "banner") == 0 ||
                       strcmp(key, "website") == 0) {
                char **slot = strcmp(key, "picture") == 0 ? &out->picture :
                              strcmp(key, "banner") == 0 ? &out->banner :
                              &out->website;
                bool https_only = strcmp(key, "website") != 0;
                bool valid_scheme = https_only
                    ? valid_https_url(value) : valid_web_url(value);
                if (size != 2 || !value || *slot || !valid_scheme) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_METADATA);
                    continue;
                }
                if (!set_single(slot, value)) goto oom;
            } else if (strcmp(key, "r") == 0) {
                r_count++;
                if (size != 2 || !valid_wss_url(value)) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_METADATA);
                } else if (r_count > NOSTR_COMMUNIKEYS_MAX_RELAYS) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_CARDINALITY);
                } else if (!strings_contains(out->relays, out->relays_len, value)) {
                    if (!append_string(&out->relays, &out->relays_len, value))
                        goto oom;
                }
            } else if (strcmp(key, "blossom") == 0) {
                blossom_count++;
                if (size != 2 || !valid_https_url(value)) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_METADATA);
                } else if (blossom_count > NOSTR_COMMUNIKEYS_MAX_RELAYS) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_CARDINALITY);
                } else if (!strings_contains(out->blossom_servers,
                                             out->blossom_servers_len, value)) {
                    if (!append_string(&out->blossom_servers,
                                       &out->blossom_servers_len, value)) goto oom;
                }
            } else if (strcmp(key, "grasp") == 0) {
                grasp_count++;
                if (size != 2 || !valid_wss_url(value)) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_METADATA);
                } else if (grasp_count > NOSTR_COMMUNIKEYS_MAX_RELAYS) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_CARDINALITY);
                } else if (!strings_contains(out->grasp_servers,
                                             out->grasp_servers_len, value)) {
                    if (!append_string(&out->grasp_servers,
                                       &out->grasp_servers_len, value)) goto oom;
                }
            } else if (strcmp(key, "mint") == 0) {
                mint_count++;
                const char *protocol = size == 3 ? nostr_tag_get(tag, 2) : NULL;
                if ((size != 2 && size != 3) || !valid_https_url(value) ||
                    (protocol && strlen(protocol) > 32)) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_METADATA);
                } else if (mint_count > NOSTR_COMMUNIKEYS_MAX_RELAYS) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_CARDINALITY);
                } else if (!mint_exists(out, value, protocol)) {
                    if (!append_mint(out, value, protocol)) goto oom;
                }
            } else if (strcmp(key, "tos") == 0) {
                const char *relay = size == 3 ? nostr_tag_get(tag, 2) : NULL;
                if ((size != 2 && size != 3) || !value || !*value || out->tos ||
                    (relay && *relay && !valid_wss_url(relay))) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_CARDINALITY);
                } else {
                    out->tos = ck_strdup(value);
                    out->tos_relay = relay && *relay ? ck_strdup(relay) : NULL;
                    if (!out->tos || (relay && *relay && !out->tos_relay)) goto oom;
                }
            } else if (strcmp(key, "location") == 0) {
                if (size != 2 || !value || out->location) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_CARDINALITY);
                    continue;
                }
                char *text = trimmed_dup(value);
                if (!text) goto oom;
                size_t len = strlen(text);
                if (len < 1 || len > 256) {
                    free(text);
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_METADATA);
                    continue;
                }
                out->location = text;
            } else if (strcmp(key, "g") == 0) {
                bool geohash_ok = size == 2 && value && *value &&
                                  strlen(value) <= 12;
                for (const char *p = value; geohash_ok && *p; ++p)
                    if (!strchr("0123456789bcdefghjkmnpqrstuvwxyz", *p))
                        geohash_ok = false;
                if (!geohash_ok || out->geohash) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_METADATA);
                } else if (!set_single(&out->geohash, value)) goto oom;
            } else { /* service */
                service_count++;
                const char *name = value;
                const char *service_pubkey = size > 2 ? nostr_tag_get(tag, 2) : NULL;
                const char *request_relay = size > 3 ? nostr_tag_get(tag, 3) : NULL;
                const char *handler_address = size > 4 ? nostr_tag_get(tag, 4) : NULL;
                const char *handler_relay = size > 5 ? nostr_tag_get(tag, 5) : NULL;
                nostr_communikeys_coordinate_t handler = {0};
                bool handler_ok = handler_address &&
                    nostr_communikeys_coordinate_parse(handler_address, NULL, &handler) &&
                    strlen(handler.identifier) <= NOSTR_COMMUNIKEYS_MAX_SECTION_D;
                nostr_communikeys_coordinate_clear(&handler);
                if (size != 6 || !valid_service_name(name) ||
                    !nostr_communikeys_is_lower_hex_32(service_pubkey) ||
                    !valid_wss_url(request_relay) || !handler_ok ||
                    !valid_wss_url(handler_relay)) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_BAD_METADATA);
                    continue;
                }
                if (service_count > NOSTR_COMMUNIKEYS_MAX_SERVICES) {
                    definition_error(out, NOSTR_COMMUNIKEYS_ERR_CARDINALITY);
                    continue;
                }
                nostr_communikeys_service_t candidate;
                memset(&candidate, 0, sizeof(candidate));
                candidate.name = (char *)name;
                memcpy(candidate.pubkey, service_pubkey, 65);
                candidate.request_relay = (char *)request_relay;
                candidate.handler_address = (char *)handler_address;
                candidate.handler_relay = (char *)handler_relay;
                if (service_exists(out, &candidate)) continue;
                nostr_communikeys_service_t *next =
                    realloc(out->services,
                            (out->services_len + 1) * sizeof(*next));
                if (!next) goto oom;
                out->services = next;
                nostr_communikeys_service_t *service =
                    &out->services[out->services_len];
                memset(service, 0, sizeof(*service));
                service->name = ck_strdup(name);
                memcpy(service->pubkey, service_pubkey, 65);
                service->request_relay = ck_strdup(request_relay);
                service->handler_address = ck_strdup(handler_address);
                service->handler_relay = ck_strdup(handler_relay);
                if (!service->name || !service->request_relay ||
                    !service->handler_address || !service->handler_relay) {
                    free(service->name);
                    free(service->request_relay);
                    free(service->handler_address);
                    free(service->handler_relay);
                    goto oom;
                }
                out->services_len++;
            }
            continue;
        }

        /* Unknown tags are preserved extensions: top-level before the first
         * section, section-local afterwards. They are ignored for parsing. */
    }

    if (d_count != 1)
        definition_error(out, NOSTR_COMMUNIKEYS_ERR_CARDINALITY);
    if (!out->name)
        definition_error(out, NOSTR_COMMUNIKEYS_ERR_MISSING_NAME);
    if (out->relays_len == 0)
        definition_error(out, NOSTR_COMMUNIKEYS_ERR_MISSING_RELAY);
    if (out->sections_len == 0)
        definition_error(out, NOSTR_COMMUNIKEYS_ERR_SECTION_NAME);
    for (size_t i = 0; i < out->sections_len; ++i) {
        if (out->sections[i].assignments_len == 0)
            definition_error(out, NOSTR_COMMUNIKEYS_ERR_SECTION_KIND);
        /* Zero profile lists is VALID: a grant-free section contributes no
         * section-specific member grants. */
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
    if (!definition ||
        !nostr_communikeys_is_lower_hex_32(definition->branch.owner) ||
        !nostr_communikeys_is_lower_hex_32(definition->branch.community_id) ||
        !definition->name)
        return NULL;
    NostrEvent *event = nostr_event_new();
    if (!event) return NULL;
    nostr_event_set_kind(event, CAS_COMMUNITY_DEFINITION);
    nostr_event_set_pubkey(event, definition->branch.owner);
    nostr_event_set_created_at(event, created_at);
    nostr_event_set_content(event, "");

    /* Registry tag ordering: d, name, description, picture, banner, website,
     * r, blossom, grasp, mint, location, g, tos, service, then per-section
     * content, k, a, badge, retention. */
    event_add_tag(event, nostr_tag_new("d", definition->branch.community_id, NULL));
    event_add_tag(event, nostr_tag_new("name", definition->name, NULL));
    if (definition->description)
        event_add_tag(event, nostr_tag_new("description", definition->description, NULL));
    if (definition->picture)
        event_add_tag(event, nostr_tag_new("picture", definition->picture, NULL));
    if (definition->banner)
        event_add_tag(event, nostr_tag_new("banner", definition->banner, NULL));
    if (definition->website)
        event_add_tag(event, nostr_tag_new("website", definition->website, NULL));
#define ADD_LIST(KEY, VALUES, LEN) \
    do { for (size_t _i = 0; _i < (LEN); ++_i) \
        event_add_tag(event, nostr_tag_new((KEY), (VALUES)[_i], NULL)); } while (0)
    ADD_LIST("r", definition->relays, definition->relays_len);
    ADD_LIST("blossom", definition->blossom_servers, definition->blossom_servers_len);
    ADD_LIST("grasp", definition->grasp_servers, definition->grasp_servers_len);
#undef ADD_LIST
    for (size_t i = 0; i < definition->mints_len; ++i) {
        const nostr_communikeys_mint_t *mint = &definition->mints[i];
        event_add_tag(event, mint->protocol
            ? nostr_tag_new("mint", mint->url, mint->protocol, NULL)
            : nostr_tag_new("mint", mint->url, NULL));
    }
    if (definition->location)
        event_add_tag(event, nostr_tag_new("location", definition->location, NULL));
    if (definition->geohash)
        event_add_tag(event, nostr_tag_new("g", definition->geohash, NULL));
    if (definition->tos)
        event_add_tag(event, definition->tos_relay
            ? nostr_tag_new("tos", definition->tos, definition->tos_relay, NULL)
            : nostr_tag_new("tos", definition->tos, NULL));
    for (size_t i = 0; i < definition->services_len; ++i) {
        const nostr_communikeys_service_t *service = &definition->services[i];
        event_add_tag(event, nostr_tag_new("service", service->name,
                                           service->pubkey,
                                           service->request_relay,
                                           service->handler_address,
                                           service->handler_relay, NULL));
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
        for (size_t j = 0; j < section->profile_lists_len; ++j) {
            const nostr_communikeys_profile_list_ref_t *ref =
                &section->profile_lists[j];
            char *coordinate =
                nostr_communikeys_coordinate_format(&ref->coordinate);
            if (!coordinate) {
                nostr_event_free(event);
                return NULL;
            }
            event_add_tag(event, ref->coordinate.relay
                ? nostr_tag_new("a", coordinate, ref->coordinate.relay, NULL)
                : nostr_tag_new("a", coordinate, NULL));
            free(coordinate);
        }
        for (size_t j = 0; j < section->badges_len; ++j) {
            const nostr_communikeys_badge_ref_t *badge = &section->badges[j];
            event_add_tag(event, badge->relay
                ? nostr_tag_new("badge", badge->coordinate, badge->relay, NULL)
                : nostr_tag_new("badge", badge->coordinate, NULL));
        }
        for (size_t j = 0; j < section->retention_len; ++j) {
            const nostr_communikeys_retention_t *retention = &section->retention[j];
            char kind[16], amount[32];
            snprintf(kind, sizeof(kind), "%d", retention->kind);
            snprintf(amount, sizeof(amount), "%" PRId64, retention->value);
            event_add_tag(event, nostr_tag_new("retention", kind, amount,
                retention->type == NOSTR_COMMUNIKEYS_RETENTION_TIME
                    ? "time" : "count", NULL));
        }
    }

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
    const char *wanted = empty_subtype(subtype);
    for (size_t i = 0; i < definition->sections_len; ++i)
        for (size_t j = 0; j < definition->sections[i].assignments_len; ++j) {
            const nostr_communikeys_assignment_t *a =
                &definition->sections[i].assignments[j];
            /* Each exact (kind, subtype) occurs in at most one section, so
             * the first match is the only match. */
            if (a->kind == kind &&
                strcmp(empty_subtype(a->subtype), wanted) == 0)
                return &definition->sections[i];
        }
    return NULL;
}

void nostr_communikeys_targeted_publication_clear(
    nostr_communikeys_targeted_publication_t *publication) {
    if (!publication) return;
    free(publication->identifier);
    free(publication->reference);
    free(publication->reference_relay);
    free(publication->reference_author);
    free(publication->curator);
    for (size_t i = 0; i < publication->targets_len; ++i)
        free(publication->targets[i].relay);
    free(publication->targets);
    memset(publication, 0, sizeof(*publication));
}

static bool publication_has_target(
    const nostr_communikeys_targeted_publication_t *publication,
    const nostr_communikeys_branch_t *branch) {
    for (size_t i = 0; i < publication->targets_len; ++i)
        if (nostr_communikeys_branch_equal(&publication->targets[i].branch, branch))
            return true;
    return false;
}

static bool append_pair_target(
    nostr_communikeys_targeted_publication_t *publication,
    const nostr_communikeys_branch_t *branch,
    const char *relay) {
    nostr_communikeys_community_target_t *next =
        realloc(publication->targets,
                (publication->targets_len + 1) * sizeof(*next));
    if (!next) return false;
    publication->targets = next;
    nostr_communikeys_community_target_t *target =
        &publication->targets[publication->targets_len];
    memset(target, 0, sizeof(*target));
    memcpy(target->community_id, branch->community_id, 65);
    target->branch = *branch;
    target->relay = relay && *relay ? ck_strdup(relay) : NULL;
    if (relay && *relay && !target->relay) return false;
    publication->targets_len++;
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
    const char *curator = nostr_event_get_pubkey(event);
    if (!nostr_communikeys_is_lower_hex_32(curator))
        return NOSTR_COMMUNIKEYS_ERR_BAD_PUBKEY;
    out->curator = ck_strdup(curator);
    if (!out->curator) goto oom;
    out->original_kind = -1;

    size_t d_count = 0, k_count = 0;
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(event);
    size_t tag_count = tags ? nostr_tags_size(tags) : 0;

    /* Single left-to-right pass with an explicit pair state machine: an `h`
     * MUST be immediately followed by its community `a`; every unpaired `a`
     * or `e` is the (at most one) source; `p` targeting is invalid. */
    for (size_t i = 0; i < tag_count; ++i) {
        const NostrTag *tag = nostr_tags_get(tags, i);
        const char *key = tag ? nostr_tag_get_key(tag) : NULL;
        size_t size = tag ? nostr_tag_size(tag) : 0;
        const char *value = size > 1 ? nostr_tag_get(tag, 1) : NULL;
        if (!key) continue;
        if (strcmp(key, "d") == 0) {
            d_count++;
            if (size != 2 || !value || !*value || d_count > 1)
                goto bad_cardinality;
            out->identifier = ck_strdup(value);
            if (!out->identifier) goto oom;
        } else if (strcmp(key, "k") == 0) {
            k_count++;
            if (size != 2 || k_count > 1 ||
                !parse_canonical_u16(value, &out->original_kind))
                goto bad_cardinality;
        } else if (strcmp(key, "h") == 0) {
            if (size != 2 || !nostr_communikeys_is_lower_hex_32(value))
                goto bad_pair;
            if (i + 1 >= tag_count) goto bad_pair;
            const NostrTag *next = nostr_tags_get(tags, i + 1);
            const char *next_key = next ? nostr_tag_get_key(next) : NULL;
            size_t next_size = next ? nostr_tag_size(next) : 0;
            if (!next_key || strcmp(next_key, "a") != 0 ||
                (next_size != 2 && next_size != 3))
                goto bad_pair;
            nostr_communikeys_branch_t branch;
            if (!nostr_communikeys_branch_parse(nostr_tag_get(next, 1), &branch) ||
                strcmp(branch.community_id, value) != 0)
                goto bad_pair;
            if (publication_has_target(out, &branch)) goto bad_pair;
            const char *relay = next_size == 3 ? nostr_tag_get(next, 2) : NULL;
            if (!append_pair_target(out, &branch, relay)) goto oom;
            i++; /* consume the paired `a` */
        } else if (strcmp(key, "a") == 0) {
            /* Unpaired `a`: the address source. */
            if (out->has_source) goto bad_reference;
            if ((size != 2 && size != 3) || !value || !*value)
                goto bad_reference;
            nostr_communikeys_coordinate_t coordinate = {0};
            if (!nostr_communikeys_coordinate_parse(value, NULL, &coordinate))
                goto bad_reference;
            out->has_source = true;
            out->reference_type = NOSTR_COMMUNIKEYS_REFERENCE_ADDRESS;
            out->reference = ck_strdup(value);
            const char *relay = size == 3 ? nostr_tag_get(tag, 2) : NULL;
            out->reference_relay = relay && *relay ? ck_strdup(relay) : NULL;
            out->reference_author = ck_strdup(coordinate.pubkey);
            nostr_communikeys_coordinate_clear(&coordinate);
            if (!out->reference || !out->reference_author ||
                (relay && *relay && !out->reference_relay)) goto oom;
        } else if (strcmp(key, "e") == 0) {
            if (out->has_source) goto bad_reference;
            if (size < 2 || size > 4 ||
                !nostr_communikeys_is_lower_hex_32(value))
                goto bad_reference;
            const char *relay = size >= 3 ? nostr_tag_get(tag, 2) : NULL;
            const char *hint = size >= 4 ? nostr_tag_get(tag, 3) : NULL;
            /* An empty relay placeholder preserves the author position; the
             * author hint is recorded but NEVER enforced against the curator. */
            if (hint && *hint && !nostr_communikeys_is_lower_hex_32(hint))
                goto bad_reference;
            out->has_source = true;
            out->reference_type = NOSTR_COMMUNIKEYS_REFERENCE_EVENT;
            out->reference = ck_strdup(value);
            out->reference_relay = relay && *relay ? ck_strdup(relay) : NULL;
            out->reference_author = hint && *hint ? ck_strdup(hint) : NULL;
            if (!out->reference || (relay && *relay && !out->reference_relay) ||
                (hint && *hint && !out->reference_author)) goto oom;
        } else if (strcmp(key, "p") == 0) {
            /* p=<communityId> targeting is invalid in V2; reject every `p`
             * so a V1-shaped wrapper cannot slip through. */
            goto bad_targets;
        }
    }
    if (d_count != 1 || k_count != 1) goto bad_cardinality;
    if (out->targets_len < 1 ||
        out->targets_len > NOSTR_COMMUNIKEYS_MAX_TARGETS)
        goto bad_targets;
    if (out->has_source &&
        out->reference_type == NOSTR_COMMUNIKEYS_REFERENCE_ADDRESS) {
        nostr_communikeys_coordinate_t coordinate = {0};
        bool kind_matches =
            nostr_communikeys_coordinate_parse(out->reference, NULL, &coordinate) &&
            coordinate.kind == out->original_kind;
        nostr_communikeys_coordinate_clear(&coordinate);
        if (!kind_matches) goto bad_reference;
    }
    return NOSTR_COMMUNIKEYS_OK;

bad_cardinality:
    nostr_communikeys_targeted_publication_clear(out);
    return NOSTR_COMMUNIKEYS_ERR_CARDINALITY;
bad_reference:
    nostr_communikeys_targeted_publication_clear(out);
    return NOSTR_COMMUNIKEYS_ERR_BAD_REFERENCE;
bad_pair:
    nostr_communikeys_targeted_publication_clear(out);
    return NOSTR_COMMUNIKEYS_ERR_TARGET_PAIR;
bad_targets:
    nostr_communikeys_targeted_publication_clear(out);
    return NOSTR_COMMUNIKEYS_ERR_BAD_TARGETS;
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
        .identifier = (char *)d,
        .relay = NULL
    };
    return nostr_communikeys_coordinate_format(&coordinate);
}

static size_t event_h_values(const NostrEvent *event, const char **out_first) {
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(event);
    size_t count = 0;
    if (out_first) *out_first = NULL;
    for (size_t i = 0; tags && i < nostr_tags_size(tags); ++i) {
        const NostrTag *tag = nostr_tags_get(tags, i);
        if (nostr_tag_get_key(tag) && strcmp(nostr_tag_get_key(tag), "h") == 0) {
            count++;
            if (out_first && !*out_first && nostr_tag_size(tag) >= 2)
                *out_first = nostr_tag_get(tag, 1);
        }
    }
    return count;
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

    if (publication.original_kind != nostr_event_get_kind(original)) {
        status = NOSTR_COMMUNIKEYS_ERR_REFERENCE_MISMATCH;
    } else if (!publication.has_source) {
        /* Sourceless form: the original uses the wrapper `d` as its targeting
         * `h` and has the same author as the wrapper. */
        const char *h = NULL;
        size_t h_count = event_h_values(original, &h);
        const char *original_author = nostr_event_get_pubkey(original);
        if (h_count != 1 || !h ||
            strcmp(h, publication.identifier) != 0)
            status = NOSTR_COMMUNIKEYS_ERR_REFERENCE_MISMATCH;
        else if (!original_author ||
                 strcmp(original_author, publication.curator) != 0)
            status = NOSTR_COMMUNIKEYS_ERR_AUTHOR_MISMATCH;
    } else if (publication.reference_type == NOSTR_COMMUNIKEYS_REFERENCE_EVENT) {
        /* Curator wrappers are valid: authors are deliberately NOT compared. */
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
        !nostr_communikeys_is_lower_hex_32(publication->curator) ||
        publication->original_kind < 0 || publication->original_kind > 65535 ||
        publication->targets_len < 1 ||
        publication->targets_len > NOSTR_COMMUNIKEYS_MAX_TARGETS)
        return NULL;
    if (publication->has_source &&
        (!publication->reference || !*publication->reference))
        return NULL;
    NostrEvent *event = nostr_event_new();
    if (!event) return NULL;
    nostr_event_set_kind(event, CAS_TARGETED_PUBLICATION);
    nostr_event_set_pubkey(event, publication->curator);
    nostr_event_set_created_at(event, created_at);
    nostr_event_set_content(event, "");
    event_add_tag(event, nostr_tag_new("d", publication->identifier, NULL));
    if (publication->has_source) {
        const char *ref_key = publication->reference_type ==
            NOSTR_COMMUNIKEYS_REFERENCE_EVENT ? "e" : "a";
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
    }
    char kind[16];
    snprintf(kind, sizeof(kind), "%d", publication->original_kind);
    event_add_tag(event, nostr_tag_new("k", kind, NULL));
    for (size_t i = 0; i < publication->targets_len; ++i) {
        const nostr_communikeys_community_target_t *target =
            &publication->targets[i];
        char *address = nostr_communikeys_branch_format(&target->branch);
        if (!address) {
            nostr_event_free(event);
            return NULL;
        }
        event_add_tag(event,
                      nostr_tag_new("h", target->branch.community_id, NULL));
        event_add_tag(event, target->relay
            ? nostr_tag_new("a", address, target->relay, NULL)
            : nostr_tag_new("a", address, NULL));
        free(address);
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
                                     char community_id[65]) {
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
        return NOSTR_COMMUNIKEYS_ERR_BAD_TAG;
    /* The extracted value is an OPAQUE community ID: it MUST NOT be used as
     * an author, person `p` target, or profile lookup, and it does not by
     * itself select a branch. */
    if (community_id) memcpy(community_id, community, 65);
    return NOSTR_COMMUNIKEYS_OK;
}

bool nostr_communikeys_exclusive_add_h(NostrEvent *event,
                                       const char *community_id) {
    if (!event || !nostr_communikeys_is_lower_hex_32(community_id))
        return false;
    int kind = nostr_event_get_kind(event);
    if (kind != 9 && kind != 11) return false;
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(event);
    for (size_t i = 0; tags && i < nostr_tags_size(tags); ++i) {
        const NostrTag *tag = nostr_tags_get(tags, i);
        if (nostr_tag_get_key(tag) && strcmp(nostr_tag_get_key(tag), "h") == 0)
            return false;
    }
    event_add_tag(event, nostr_tag_new("h", community_id, NULL));
    return nostr_communikeys_exclusive_validate(event, NULL) ==
           NOSTR_COMMUNIKEYS_OK;
}

void nostr_communikeys_pointer_clear(nostr_communikeys_pointer_t *pointer) {
    if (!pointer) return;
    clear_strings(pointer->relays, pointer->relays_len);
    memset(pointer, 0, sizeof(*pointer));
}

nostr_communikeys_status_t
nostr_communikeys_pointer_parse(const char *naddr,
                                nostr_communikeys_pointer_t *out) {
    if (!naddr || !out) return NOSTR_COMMUNIKEYS_ERR_NULL;
    memset(out, 0, sizeof(*out));
    if (strncmp(naddr, "nostr:", 6) == 0) naddr += 6;
    NostrEntityPointer *entity = NULL;
    if (nostr_nip19_decode_naddr(naddr, &entity) != 0 || !entity)
        return NOSTR_COMMUNIKEYS_ERR_BAD_BRANCH;
    nostr_communikeys_status_t status = NOSTR_COMMUNIKEYS_OK;
    if (entity->kind != CAS_COMMUNITY_DEFINITION ||
        !nostr_communikeys_is_lower_hex_32(entity->public_key) ||
        !nostr_communikeys_is_lower_hex_32(entity->identifier)) {
        status = NOSTR_COMMUNIKEYS_ERR_BAD_BRANCH;
        goto done;
    }
    memcpy(out->branch.owner, entity->public_key, 65);
    memcpy(out->branch.community_id, entity->identifier, 65);
    /* Retain at most the first three valid unique normalized hints. */
    for (size_t i = 0; i < entity->relays_count &&
                       out->relays_len < NOSTR_COMMUNIKEYS_MAX_RELAY_HINTS; ++i) {
        const char *relay = entity->relays[i];
        if (!valid_wss_url(relay)) continue;
        if (strings_contains(out->relays, out->relays_len, relay)) continue;
        if (!append_string(&out->relays, &out->relays_len, relay)) {
            status = NOSTR_COMMUNIKEYS_ERR_OOM;
            goto done;
        }
    }
done:
    nostr_entity_pointer_free(entity);
    if (status != NOSTR_COMMUNIKEYS_OK) nostr_communikeys_pointer_clear(out);
    return status;
}

char *nostr_communikeys_pointer_format(const nostr_communikeys_pointer_t *pointer) {
    if (!pointer ||
        !nostr_communikeys_is_lower_hex_32(pointer->branch.owner) ||
        !nostr_communikeys_is_lower_hex_32(pointer->branch.community_id))
        return NULL;
    /* Canonical emission: at most three normalized wss:// hints in declared
     * order, duplicates removed by first occurrence. */
    const char *hints[NOSTR_COMMUNIKEYS_MAX_RELAY_HINTS] = {0};
    size_t hint_count = 0;
    for (size_t i = 0; i < pointer->relays_len &&
                       hint_count < NOSTR_COMMUNIKEYS_MAX_RELAY_HINTS; ++i) {
        const char *relay = pointer->relays[i];
        if (!valid_wss_url(relay)) return NULL;
        bool duplicate = false;
        for (size_t j = 0; j < hint_count; ++j)
            if (strcmp(hints[j], relay) == 0) duplicate = true;
        if (!duplicate) hints[hint_count++] = relay;
    }
    NostrEntityPointer *entity = nostr_entity_pointer_new();
    if (!entity) return NULL;
    entity->public_key = ck_strdup(pointer->branch.owner);
    entity->identifier = ck_strdup(pointer->branch.community_id);
    entity->kind = CAS_COMMUNITY_DEFINITION;
    entity->relays_count = 0;
    if (hint_count) {
        entity->relays = calloc(hint_count, sizeof(char *));
        if (!entity->relays) {
            nostr_entity_pointer_free(entity);
            return NULL;
        }
        for (size_t i = 0; i < hint_count; ++i) {
            entity->relays[i] = ck_strdup(hints[i]);
            if (!entity->relays[i]) {
                entity->relays_count = i;
                nostr_entity_pointer_free(entity);
                return NULL;
            }
        }
        entity->relays_count = hint_count;
    }
    char *bech = NULL;
    if (!entity->public_key || !entity->identifier ||
        nostr_nip19_encode_naddr(entity, &bech) != 0) {
        nostr_entity_pointer_free(entity);
        free(bech);
        return NULL;
    }
    nostr_entity_pointer_free(entity);
    return bech;
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
            /* Only valid real signing pubkeys contribute grants; malformed
             * `p` tags are ignored rather than invalidating the list. */
            if (nostr_tag_size(tag) < 2 ||
                !nostr_communikeys_is_lower_hex_32(value))
                continue;
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

/* Deterministic authority replacement: greatest created_at, ties broken by
 * lexicographically lowest event ID. */
static const NostrEvent *select_current_list_event(
    const nostr_communikeys_profile_list_ref_t *ref,
    const NostrEvent *const *events,
    size_t events_len) {
    const NostrEvent *best = NULL;
    char *best_id = NULL;
    for (size_t i = 0; i < events_len; ++i) {
        const NostrEvent *candidate = events[i];
        if (!candidate) continue;
        if (nostr_event_get_kind(candidate) != NOSTR_COMMUNIKEYS_KIND_PROFILE_LIST)
            continue;
        const char *author = nostr_event_get_pubkey(candidate);
        if (!author || strcmp(author, ref->coordinate.pubkey) != 0) continue;
        /* Exact d match: exactly one matching non-empty d tag. */
        const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(candidate);
        const char *d = NULL;
        size_t d_count = 0;
        for (size_t j = 0; tags && j < nostr_tags_size(tags); ++j) {
            const NostrTag *tag = nostr_tags_get(tags, j);
            if (nostr_tag_get_key(tag) &&
                strcmp(nostr_tag_get_key(tag), "d") == 0) {
                d_count++;
                d = nostr_tag_get_value(tag);
            }
        }
        if (d_count != 1 || !d ||
            strcmp(d, ref->coordinate.identifier) != 0) continue;
        if (!best) {
            best = candidate;
            best_id = nostr_event_get_id((NostrEvent *)candidate);
            continue;
        }
        int64_t best_at = nostr_event_get_created_at(best);
        int64_t candidate_at = nostr_event_get_created_at(candidate);
        if (candidate_at < best_at) continue;
        char *candidate_id = nostr_event_get_id((NostrEvent *)candidate);
        if (candidate_at > best_at ||
            (candidate_id && best_id && strcmp(candidate_id, best_id) < 0)) {
            best = candidate;
            free(best_id);
            best_id = candidate_id;
        } else {
            free(candidate_id);
        }
    }
    free(best_id);
    return best;
}

bool nostr_communikeys_author_can_publish(
    const nostr_communikeys_definition_t *definition,
    int kind,
    const char *subtype,
    const NostrEvent *const *profile_list_events,
    size_t profile_list_events_len,
    const char *author_pubkey) {
    if (!definition || !definition->valid ||
        !nostr_communikeys_is_lower_hex_32(author_pubkey)) return false;
    const nostr_communikeys_section_t *section =
        nostr_communikeys_definition_find_section(definition, kind, subtype);
    if (!section) return false;

    /* The owner has root authority for its branch. */
    if (strcmp(author_pubkey, definition->branch.owner) == 0) return true;

    /* Every non-owner pubkey referenced as a profile-list author anywhere in
     * the definition holds the structural community-wide member/write role. */
    for (size_t i = 0; i < definition->sections_len; ++i)
        for (size_t j = 0; j < definition->sections[i].profile_lists_len; ++j)
            if (strcmp(definition->sections[i].profile_lists[j].coordinate.pubkey,
                       author_pubkey) == 0)
                return true;

    /* Union of current valid `p` grants across every referenced shard.
     * Missing evidence contributes nothing; zero references is valid. */
    for (size_t i = 0; i < section->profile_lists_len; ++i) {
        const nostr_communikeys_profile_list_ref_t *ref =
            &section->profile_lists[i];
        const NostrEvent *current = select_current_list_event(
            ref, profile_list_events, profile_list_events_len);
        if (!current) continue;
        nostr_communikeys_profile_list_t list;
        nostr_communikeys_status_t status = nostr_communikeys_profile_list_parse(
            current, ref->coordinate.pubkey, ref->coordinate.identifier, &list);
        if (status != NOSTR_COMMUNIKEYS_OK) continue;
        bool allowed = nostr_communikeys_profile_list_contains(&list, author_pubkey);
        nostr_communikeys_profile_list_clear(&list);
        if (allowed) return true;
    }
    return false;
}
